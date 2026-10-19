// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.windows_service;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.supervisor.installation;
import devicefs.supervisor.logging_console;
import devicefs.supervisor.process_diagnostics;
import devicefs.supervisor.process_launch;
import devicefs.terminal.transcoding;

using namespace wil::literals;

export constexpr auto kOrchestrateOption = "--orchestrate"_zv;

export [[nodiscard]] auto CreateCancellationEvent(
    _In_opt_z_ const char *const name) {
    auto event = wil::unique_event_nothrow{CreateEventExA(
        nullptr, name, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
    if (!event || (GetLastError() == ERROR_ALREADY_EXISTS)) {
        WinError("failed to create cancellation event{}",
            [name] {
                const auto last_error = wil::last_error_context{};
                return name ? std::format(": '{}'", name) : std::string{};
            }());
    }
    return event;
}

namespace {

using namespace std::chrono_literals;

constexpr auto kCancellationEventName =
    "Local\\devicefs-backup-stop"_zv;

[[nodiscard]] auto CreateChildJob() {
    auto job = wil::unique_handle(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        WinError("failed to create the backup process job");
    }
    auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        WinError("failed to configure the backup process job");
    }
    return job;
}

struct BackupProcess {
    static constexpr auto kJobPollInterval = 100ms;
    static constexpr auto kForcedTerminationTimeout = 30s;

    std::optional<LoggingConsole> console;
    wil::unique_process_information process;
    wil::unique_handle job;

    [[nodiscard]] auto Wait(
        const std::chrono::milliseconds timeout) const noexcept {
        return WaitForProcess(process.hProcess, timeout);
    }

    [[nodiscard]] auto ExitCode() const {
        return ProcessExitCode(process.hProcess);
    }

    [[nodiscard]] auto WaitForAll(
        const std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto accounting = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION{};
            if (!QueryInformationJobObject(job.get(),
                    JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr)) {
                WinError("failed to query the backup process job");
            }
            if (accounting.ActiveProcesses == 0) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(kJobPollInterval);
        }
    }

    auto TerminateAndWait(const DWORD exit_code) const {
        if (!TerminateJobObject(job.get(), exit_code)) {
            WinError("failed to terminate the backup process job");
        }
        if (!WaitForAll(kForcedTerminationTimeout)) {
            throw std::runtime_error(
                "backup processes survived job termination");
        }
    }
};

[[nodiscard]] auto StartOrchestrator(std::optional<Log> &log) {
    auto console = LoggingConsole([&log](const std::u8string_view output) noexcept {
        if (log) {
            std::ignore = log->WriteStream(output);
        }
        return ERROR_SUCCESS;
    });
    auto job = CreateChildJob();
    const auto supervisor = devicefs::terminal::Transcode<std::string>(
        CurrentExecutablePath().native());
    const auto arguments = std::array{
        wil::zstring_view{supervisor}, kOrchestrateOption};
    auto process = console.StartProcess(
        arguments, CREATE_SUSPENDED,
        std::tuple{PROC_THREAD_ATTRIBUTE_JOB_LIST,
            job.addressof(), sizeof(job.get())});
    return BackupProcess{
        .console = std::move(console),
        .process = std::move(process),
        .job = std::move(job),
    };
}

struct ServiceContext {
    static constexpr auto kInitialPendingCheckpoint = DWORD{1};
    static constexpr auto kForcedCleanupCheckpoint =
        kInitialPendingCheckpoint + 1;
    static constexpr auto kStartWaitHint = 30s;
    static constexpr auto kGracefulStopTimeout = 5min;
    static constexpr auto kStrayProcessTimeout = 5s;
    static constexpr auto kStopWaitHint =
        kGracefulStopTimeout + kStrayProcessTimeout +
        BackupProcess::kForcedTerminationTimeout;
    static constexpr auto kPreshutdownMargin = 25s;
    static constexpr auto kMinimumPreshutdownTimeout =
        kStopWaitHint + kPreshutdownMargin;
    static constexpr auto kAcceptedControls =
        DWORD{SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN};

    SERVICE_STATUS_HANDLE status_handle = nullptr;
    wil::unique_event_nothrow cancellation_event;
};

using ServiceWaitHint = std::chrono::duration<DWORD, std::milli>;

auto SetServiceState(
    const ServiceContext &context,
    const DWORD state,
    const DWORD win32_error = ERROR_SUCCESS,
    const DWORD service_error = 0,
    const DWORD checkpoint = 0,
    const ServiceWaitHint wait_hint = {}) noexcept {
    auto status = SERVICE_STATUS{
        .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
        .dwCurrentState = state,
        .dwControlsAccepted = state == SERVICE_RUNNING
            ? ServiceContext::kAcceptedControls
            : 0,
        .dwWin32ExitCode = win32_error,
        .dwServiceSpecificExitCode = service_error,
        .dwCheckPoint = checkpoint,
        .dwWaitHint = wait_hint.count(),
    };
    return SetServiceStatus(context.status_handle, &status) != FALSE;
}

auto WINAPI ServiceControlHandler(
    const DWORD control, DWORD, void *,
    _In_ void *const raw_context) noexcept -> DWORD {
    const auto &context = *static_cast<const ServiceContext *>(raw_context);
    if (control == SERVICE_CONTROL_INTERROGATE) {
        return ERROR_SUCCESS;
    }
    if ((control != SERVICE_CONTROL_STOP) &&
        (control != SERVICE_CONTROL_PRESHUTDOWN)) {
        return ERROR_CALL_NOT_IMPLEMENTED;
    }

    context.cancellation_event.SetEvent();
    return ERROR_SUCCESS;
}

struct ServiceOutcome {
    DWORD win32_error = ERROR_SUCCESS;
    DWORD service_error = 0;
};

[[nodiscard]] auto RunBackup(
    ServiceContext &context,
    std::optional<Log> &log) {
    auto backup = StartOrchestrator(log);
    if (log) {
        log->TryWrite("backup-supervisor: running version '{}'",
            CurrentProductVersion().value_or("unknown"));
        log->TryWrite("backup-supervisor: backup starting");
    }
    if (!SetServiceState(context, SERVICE_RUNNING)) {
        WinError("failed to report that the backup service is running");
    }
    auto report_stopping = wil::scope_exit([&context] {
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
            ServiceContext::kStopWaitHint);
    });
    if (ResumeThread(backup.process.hThread) == MAXDWORD) {
        WinError("failed to resume the backup orchestrator");
    }

    std::ignore = wil::wait_any_failfast(
        backup.process.hProcess, context.cancellation_event);
    SetServiceState(context, SERVICE_STOP_PENDING,
        ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
        ServiceContext::kStopWaitHint);
    report_stopping.release();
    const auto cancellation_signaled =
        context.cancellation_event.is_signaled();

    auto forced = false;
    const auto force_cleanup = [&log, &backup, &context, &forced](
        const std::string_view reason, const DWORD exit_code) {
        if (log) {
            log->TryWrite("{}", reason);
            LogJobProcesses(*log, backup.job.get());
        }
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, ServiceContext::kForcedCleanupCheckpoint,
            BackupProcess::kForcedTerminationTimeout);
        backup.TerminateAndWait(exit_code);
        forced = true;
    };
    if (cancellation_signaled && !backup.Wait(
            ServiceContext::kGracefulStopTimeout)) {
        force_cleanup(
            "backup-supervisor: graceful stop timed out; terminating the process job",
            ERROR_CANCELLED);
    }

    const auto child_exit_code = backup.ExitCode();
    if (!backup.WaitForAll(ServiceContext::kStrayProcessTimeout)) {
        force_cleanup(
            "backup-supervisor: terminating child processes left behind by the orchestrator",
            ERROR_PROCESS_ABORTED);
    }
    backup.console.reset();
    if (log) {
        if (forced) {
            log->TryWrite(
                "backup-supervisor: backup failed after forced process termination; "
                "orchestrator exit code {}",
                child_exit_code);
        } else if ((child_exit_code == 0) && cancellation_signaled) {
            log->TryWrite(
                "backup-supervisor: backup completed before cancellation took effect");
        } else if (child_exit_code == 0) {
            log->TryWrite("backup-supervisor: backup completed successfully");
        } else if (cancellation_signaled) {
            log->TryWrite(
                "backup-supervisor: backup cancelled; orchestrator exit code {}",
                child_exit_code);
        } else {
            log->TryWrite(
                "backup-supervisor: backup failed; orchestrator exit code {}",
                child_exit_code);
        }
    }
    if (forced) {
        return ServiceOutcome{.win32_error = ERROR_TIMEOUT};
    }
    if (cancellation_signaled || (child_exit_code == 0)) {
        return ServiceOutcome{};
    }
    return ServiceOutcome{
        .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
        .service_error = child_exit_code,
    };
}

[[gsl::suppress("26447",
    justification:
        "MSVC incorrectly reports that calls in the catch handlers can throw. "
        "`std::exception::what()` and `Log::TryWrite` are declared `noexcept`. "
        "The literal `std::format_string` arguments use the `consteval` "
        "constructor of `std::basic_format_string`, so their construction "
        "cannot throw at runtime. Exceptions from the remaining operations "
        "are caught within `RunService`, so no exception can escape it.")]]
[[nodiscard]] auto RunService(ServiceContext &context) noexcept {
    constexpr auto kInternalFailure = DWORD{1};
    auto log = std::optional<Log>{};
    try {
        log.emplace(ResolvePersistentPaths().logs);
    } catch (...) {}

    auto result = ServiceOutcome{};
    try {
        context.cancellation_event = CreateCancellationEvent(
            kCancellationEventName.c_str());
        result = RunBackup(context, log);
    } catch (const std::system_error &error) {
        result.win32_error = std::bit_cast<DWORD>(error.code().value());
        if (log) {
            log->TryWrite("backup-supervisor: {}", error.what());
        }
    } catch (const std::runtime_error &error) {
        result = {
            .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
            .service_error = kInternalFailure,
        };
        if (log) {
            log->TryWrite("backup-supervisor: {}", error.what());
        }
    } catch (...) {
        result = {.win32_error = ERROR_UNHANDLED_EXCEPTION};
    }

    if (log) {
        log->TryFlush();
    }
    return result;
}

auto WINAPI ServiceMain(DWORD, char **) noexcept -> void {
    static auto context = ServiceContext{};
    context.status_handle = RegisterServiceCtrlHandlerExA(
        kServiceName.data(), ServiceControlHandler, &context);
    if (context.status_handle == nullptr) {
        return;
    }
    SetServiceState(context, SERVICE_START_PENDING,
        ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
        ServiceContext::kStartWaitHint);
    const auto result = RunService(context);
    SetServiceState(context, SERVICE_STOPPED,
        result.win32_error, result.service_error);
}

} // namespace

export constexpr auto kMinimumServicePreshutdownTimeout =
    ServiceContext::kMinimumPreshutdownTimeout;

export [[nodiscard]] auto OpenCancellationEvent() {
    auto event = wil::unique_event_nothrow{OpenEventA(
        SYNCHRONIZE, FALSE, kCancellationEventName.c_str())};
    if (!event) {
        WinError("failed to open backup cancellation event '{}'",
            std::string_view{kCancellationEventName});
    }
    return event;
}

export auto RunServiceDispatcher() {
    [[gsl::suppress("type.3",
        justification:
            "The SCM retains a mutable historical parameter for an input-only "
            "service name.")]]
    auto *const service_name = const_cast<PSTR>(kServiceName.data());
    auto entries = std::array{
        SERVICE_TABLE_ENTRYA{
            .lpServiceName = service_name,
            .lpServiceProc = ServiceMain,
        },
        SERVICE_TABLE_ENTRYA{},
    };
    if (!StartServiceCtrlDispatcherA(entries.data())) {
        WinError("failed to connect service '{}' to the Service Control Manager",
            kServiceName);
    }
    return 0;
}
