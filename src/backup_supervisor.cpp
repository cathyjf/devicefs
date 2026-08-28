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

#include <devicefs/strsafe_compat.h>

import std;
import <clocale>;
import <devicefs/windows_imports.h>;
import <sal.h>;
import devicefs.common;
import devicefs.filesystem;
import devicefs.stream_writer;
import devicefs.supervisor.installation;
import devicefs.supervisor.logging_console;
import devicefs.supervisor.native_backup;
import devicefs.supervisor.process_diagnostics;
import devicefs.supervisor.vshadow;

namespace {

using namespace std::chrono_literals;

constexpr auto kCancellationEventName =
    wil::zstring_view("Local\\devicefs-backup-stop");
constexpr auto kOrchestrateOption = std::string_view("--orchestrate");

[[nodiscard]] auto CreateCancellationEvent(
    _In_opt_z_ const char *const name) {
    auto event = wil::unique_event_nothrow{CreateEventExA(
        nullptr, name, CREATE_EVENT_MANUAL_RESET, EVENT_ALL_ACCESS)};
    if (!event) {
        WinError("could not create the cancellation event");
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        throw std::runtime_error("the cancellation event already exists");
    }
    return event;
}

[[nodiscard]] auto OpenCancellationEvent() {
    auto event = wil::unique_event_nothrow{OpenEventA(
        SYNCHRONIZE, FALSE, kCancellationEventName.c_str())};
    if (!event) {
        WinError("could not open the backup cancellation event");
    }
    return event;
}

[[nodiscard]] auto AcquireBackupLock(
    const std::filesystem::path &path) {
    auto lock = wil::unique_hfile(CreateFileW(
        path.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!lock) {
        const auto error = GetLastError();
        if (error == ERROR_SHARING_VIOLATION) {
            throw std::runtime_error("a backup is already running");
        }
        WinError("could not open the backup lock file",
            ExplicitWin32Error{error});
    }
    return lock;
}

[[nodiscard]] auto CreateChildJob() {
    auto job = wil::unique_handle(CreateJobObjectW(nullptr, nullptr));
    if (!job) {
        WinError("could not create the backup process job");
    }
    auto limits = JOBOBJECT_EXTENDED_LIMIT_INFORMATION{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_BREAKAWAY_OK;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        WinError("could not configure the backup process job");
    }
    return job;
}

struct BackupProcess {
    static constexpr auto kJobPollInterval = 100ms;
    static constexpr auto kForcedTerminationTimeout = 30s;

    LoggingConsole console;
    wil::unique_process_information process;
    wil::unique_handle job;

    [[nodiscard]] auto Wait(
        const std::chrono::milliseconds timeout) const {
        const auto result = WaitForSingleObject(
            process.hProcess, wil::safe_cast<DWORD>(timeout.count()));
        if (result == WAIT_FAILED) {
            WinError("could not wait for a backup process");
        }
        return result == WAIT_OBJECT_0;
    }

    [[nodiscard]] auto ExitCode() const {
        auto result = DWORD{};
        if (!GetExitCodeProcess(process.hProcess, &result)) {
            WinError("could not obtain a backup process exit code");
        }
        return result;
    }

    [[nodiscard]] auto WaitForAll(
        const std::chrono::milliseconds timeout) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            auto accounting = JOBOBJECT_BASIC_ACCOUNTING_INFORMATION{};
            if (!QueryInformationJobObject(job.get(),
                    JobObjectBasicAccountingInformation,
                    &accounting, sizeof(accounting), nullptr)) {
                WinError("could not query the backup process job");
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
            WinError("could not terminate the backup process job");
        }
        if (!WaitForAll(kForcedTerminationTimeout)) {
            throw std::runtime_error(
                "backup processes survived job termination");
        }
    }
};

[[nodiscard]] auto StartOrchestrator(Log &log) {
    auto console = LoggingConsole(log);
    auto job = CreateChildJob();
    const auto supervisor = CurrentExecutablePath().string();
    const auto arguments = std::array{
        std::string_view{supervisor}, kOrchestrateOption};
    auto command = wil::ArgvToCommandLine(arguments);
    auto process = console.StartProcess(
        job.get(), wil::zstring_view(supervisor), command);
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

    if (!SetEvent(context.cancellation_event.get())) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

struct ServiceOutcome {
    DWORD win32_error = ERROR_SUCCESS;
    DWORD service_error = 0;
};

[[nodiscard]] auto RunBackup(
    ServiceContext &context,
    Log &log) {
    auto backup = StartOrchestrator(log);
    log.Write("backup-supervisor: backup starting");
    if (!SetServiceState(context, SERVICE_RUNNING)) {
        WinError("could not report that the backup service is running");
    }
    auto report_stopping = wil::scope_exit([&] {
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
            ServiceContext::kStopWaitHint);
    });
    if (ResumeThread(backup.process.hThread) == MAXDWORD) {
        WinError("could not resume the backup orchestrator");
    }

    const auto waits = std::array{
        backup.process.hProcess, context.cancellation_event.get(),
    };
    const auto wait = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(waits.size()),
        waits.data(), FALSE, INFINITE);
    if (wait == WAIT_FAILED) {
        WinError("could not wait for the backup orchestrator");
    }
    SetServiceState(context, SERVICE_STOP_PENDING,
        ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
        ServiceContext::kStopWaitHint);
    report_stopping.release();
    const auto cancellation_signaled =
        context.cancellation_event.is_signaled();

    auto forced = false;
    if (cancellation_signaled && !backup.Wait(
            ServiceContext::kGracefulStopTimeout)) {
        log.Write(
            "backup-supervisor: graceful stop timed out; terminating the process job");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, ServiceContext::kForcedCleanupCheckpoint,
            BackupProcess::kForcedTerminationTimeout);
        backup.TerminateAndWait(ERROR_CANCELLED);
        forced = true;
    }

    const auto child_exit_code = backup.ExitCode();
    if (!backup.WaitForAll(ServiceContext::kStrayProcessTimeout)) {
        log.Write(
            "backup-supervisor: terminating child processes left behind by the orchestrator");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, ServiceContext::kForcedCleanupCheckpoint,
            BackupProcess::kForcedTerminationTimeout);
        backup.TerminateAndWait(ERROR_PROCESS_ABORTED);
        forced = true;
    }
    backup.console.Finish();

    if (forced) {
        log.Write(
            "backup-supervisor: backup failed after forced process termination; "
            "orchestrator exit code {}",
            child_exit_code);
    } else if ((child_exit_code == 0) && cancellation_signaled) {
        log.Write(
            "backup-supervisor: backup completed before cancellation took effect");
    } else if (child_exit_code == 0) {
        log.Write("backup-supervisor: backup completed successfully");
    } else if (cancellation_signaled) {
        log.Write(
            "backup-supervisor: backup cancelled; orchestrator exit code {}",
            child_exit_code);
    } else {
        log.Write(
            "backup-supervisor: backup failed; orchestrator exit code {}",
            child_exit_code);
    }
    log.Flush();
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

auto TryWriteFailure(
    Log *const log, const std::exception &error) noexcept {
    try {
        if (log != nullptr) {
            log->Write("backup-supervisor: {}", error.what());
        }
    } catch (...) {}
}

[[nodiscard]] auto RunService(
    ServiceContext &context, const DWORD argc) noexcept {
    constexpr auto kInternalFailure = DWORD{1};
    auto result = ServiceOutcome{};
    auto log = std::optional<Log>{};
    try {
        if (argc != 1) {
            throw std::system_error(
                ERROR_INVALID_PARAMETER, std::system_category(),
                "service start arguments are not supported");
        }
        const auto persistent = ResolvePersistentPaths();
        log.emplace(persistent.logs);
        context.cancellation_event = CreateCancellationEvent(
            kCancellationEventName.c_str());
        result = RunBackup(context, *log);
    } catch (const std::system_error &error) {
        result.win32_error = std::bit_cast<DWORD>(error.code().value());
        TryWriteFailure(log ? &*log : nullptr, error);
    } catch (const std::runtime_error &error) {
        result = {
            .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
            .service_error = kInternalFailure,
        };
        TryWriteFailure(log ? &*log : nullptr, error);
    } catch (...) {
        result = {
            .win32_error = ERROR_UNHANDLED_EXCEPTION,
        };
    }

    if (log) {
        log->TryFlush();
    }
    return result;
}

template <typename Operation>
[[nodiscard]] auto RunForegroundOperation(
    const HANDLE input,
    const DWORD console_mode,
    Operation operation) {
    const auto cancellation_event = CreateCancellationEvent(nullptr);
    if (!SetConsoleMode(
            input, console_mode & ~DWORD{ENABLE_PROCESSED_INPUT})) {
        WinError("could not enable foreground cancellation input");
    }
    auto restore_console_mode = wil::scope_exit([&] {
        static_cast<void>(SetConsoleMode(input, console_mode));
    });

    auto result = std::async(
        std::launch::async, std::move(operation), cancellation_event.get());
    auto cancel_on_monitor_failure = wil::scope_exit([&] {
        static_cast<void>(SetEvent(cancellation_event.get()));
    });
    constexpr auto kPollInterval = std::chrono::milliseconds{100};
    constexpr auto kInputBatchSize = 16uz;
    while (result.wait_for(kPollInterval) !=
        std::future_status::ready) {
        auto available = DWORD{};
        if (!GetNumberOfConsoleInputEvents(input, &available)) {
            WinError("could not inspect foreground console input");
        }
        while (available != 0) {
            auto records = std::array<INPUT_RECORD, kInputBatchSize>{};
            auto read = DWORD{};
            const auto count = std::min<DWORD>(
                available, DWORD{records.size()});
            if (!ReadConsoleInputW(input, records.data(), count, &read)) {
                WinError("could not read foreground console input");
            }
            for (const auto &record : std::span{records}.first(read)) {
                if ((record.EventType != KEY_EVENT) ||
                    !record.Event.KeyEvent.bKeyDown ||
                    (record.Event.KeyEvent.wVirtualKeyCode != L'C') ||
                    ((record.Event.KeyEvent.dwControlKeyState &
                        (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) == 0)) {
                    continue;
                }
                if (cancellation_event.is_signaled()) {
                    continue;
                }
                if (!SetEvent(cancellation_event.get())) {
                    WinError("could not request cancellation");
                }
                devicefs::WriteToStream(
                    std::cout,
                    "Cancellation requested; waiting for cleanup.\n");
            }
            if (!GetNumberOfConsoleInputEvents(input, &available)) {
                WinError("could not inspect foreground console input");
            }
        }
    }
    cancel_on_monitor_failure.release();
    const auto operation_result = [&] {
        try {
            const auto completed_result = result.get();
            return cancellation_event.is_signaled()
                ? kCancelledExitCode
                : completed_result;
        } catch (...) {
            if (cancellation_event.is_signaled()) {
                return kCancelledExitCode;
            }
            throw;
        }
    }();
    if (!SetConsoleMode(input, console_mode)) {
        WinError("could not restore the foreground console input mode");
    }
    restore_console_mode.release();
    return operation_result;
}

struct ForegroundOptions {
    bool no_writers = false;
    std::optional<std::u8string> namespace_override;
    std::vector<std::string> volume_override;
};

[[nodiscard]] auto ParseVolumeList(std::string_view source) {
    auto result = std::vector<std::string>{};
    constexpr auto kWhitespace = std::string_view{" \t\r\n\f\v"};
    while (true) {
        const auto separator = source.find(',');
        const auto field = source.substr(0, separator);
        const auto first = field.find_first_not_of(kWhitespace);
        if (first == std::string_view::npos) {
            throw std::invalid_argument(
                "--volume/--volumes must not contain an empty volume");
        }
        const auto last = field.find_last_not_of(kWhitespace);
        result.emplace_back(field.substr(first, last - first + 1));
        if (separator == std::string_view::npos) {
            return result;
        }
        source.remove_prefix(separator + 1);
    }
}

[[nodiscard]] auto ParseForegroundOptions(
    const std::span<const std::string> arguments) {
    auto result = ForegroundOptions{};
    auto raw_namespace_override = std::optional<std::string_view>{};
    auto raw_volume_override = std::optional<std::string_view>{};
    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (argument == "--no-writers") {
            result.no_writers = true;
        } else if (argument == "--namespace") {
            if (++index == arguments.size()) {
                throw std::invalid_argument("--namespace requires a value");
            }
            raw_namespace_override = arguments[index];
        } else if ((argument == "--volume") ||
            (argument == "--volumes")) {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    "--volume/--volumes requires a value");
            }
            raw_volume_override = arguments[index];
        } else {
            throw std::invalid_argument(
                "--foreground received an unknown argument");
        }
    }
    if (raw_namespace_override) {
        result.namespace_override = std::u8string{
            raw_namespace_override->begin(), raw_namespace_override->end()};
    }
    if (raw_volume_override) {
        result.volume_override = ParseVolumeList(*raw_volume_override);
    }
    return result;
}

[[nodiscard]] auto ParseIncrementalDiagnosticOptions(
    const std::span<const std::string> arguments) {
    auto result = IncrementalDiagnosticOptions{};
    auto raw_namespace_override = std::optional<std::string_view>{};
    auto raw_volume_override = std::optional<std::string_view>{};
    auto verification_percentage_supplied = false;
    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto &argument = arguments[index];
        if (argument == "--incremental-stats") {
            result.print_statistics = true;
        } else if (argument == "--incremental-verify") {
            result.verify = true;
        } else if (argument == "--expose-synthetic-backup") {
            result.expose_synthetic_backup = true;
            if (((index + 1) < arguments.size()) &&
                !arguments[index + 1].starts_with("--")) {
                result.backup_view_mount_root =
                    std::filesystem::path{arguments[++index]};
            }
        } else if (argument == "--verify-synthetic-backup") {
            result.verify_synthetic_backup = true;
            if (((index + 1) < arguments.size()) &&
                !arguments[index + 1].starts_with("--")) {
                result.backup_view_mount_root =
                    std::filesystem::path{arguments[++index]};
            }
        } else if (argument == "--verify-percentage") {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    "--verify-percentage requires a value");
            }
            const auto &text = arguments[index];
            auto consumed = std::size_t{};
            try {
                result.filesystem_verification_percentage =
                    std::stod(text, &consumed);
            } catch (const std::invalid_argument &) {
                throw std::invalid_argument(
                    "--verify-percentage requires a number greater than "
                    "zero and no greater than 100");
            } catch (const std::out_of_range &) {
                throw std::invalid_argument(
                    "--verify-percentage requires a number greater than "
                    "zero and no greater than 100");
            }
            if ((consumed != text.size()) ||
                !std::isfinite(
                    result.filesystem_verification_percentage) ||
                (result.filesystem_verification_percentage <= 0.0) ||
                (result.filesystem_verification_percentage > 100.0)) {
                throw std::invalid_argument(
                    "--verify-percentage requires a number greater than "
                    "zero and no greater than 100");
            }
            verification_percentage_supplied = true;
        } else if (argument == "--namespace") {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    "--namespace requires a value");
            }
            raw_namespace_override = arguments[index];
        } else if ((argument == "--volume") ||
            (argument == "--volumes")) {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    "--volume/--volumes requires a value");
            }
            raw_volume_override = arguments[index];
        } else if (argument == "--baseline") {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    "--baseline requires a value");
            }
            result.baseline_snapshot_identifier =
                winrt::guid{std::string_view{arguments[index]}};
        } else {
            throw std::invalid_argument(
                "incremental diagnostics received an unknown argument");
        }
    }
    if (raw_namespace_override) {
        result.namespace_override = std::u8string{
            raw_namespace_override->begin(), raw_namespace_override->end()};
    }
    if (raw_volume_override) {
        result.volume_override = ParseVolumeList(*raw_volume_override);
    }
    if (result.expose_synthetic_backup &&
        result.verify_synthetic_backup) {
        throw std::invalid_argument(
            "--expose-synthetic-backup and --verify-synthetic-backup "
            "cannot be used together");
    }
    if (verification_percentage_supplied &&
        !result.verify_synthetic_backup) {
        throw std::invalid_argument(
            "--verify-percentage requires --verify-synthetic-backup");
    }
    return result;
}

[[nodiscard]] auto ParseNamespaceOverride(
    const std::span<const std::string> arguments,
    const std::string_view mode)
    -> std::optional<std::u8string> {
    if (arguments.empty()) {
        return std::nullopt;
    }
    if ((arguments.size() != 2) ||
        (arguments[0] != "--namespace")) {
        throw std::invalid_argument(std::format(
            "{} accepts only --namespace NAMESPACE", mode));
    }
    return std::u8string{arguments[1].begin(), arguments[1].end()};
}

[[nodiscard]] auto GetForegroundConsoleInput(
    const wil::zstring_view unavailable_message) {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    auto console_mode = DWORD{};
    if (!GetConsoleMode(input, &console_mode)) {
        throw std::runtime_error(unavailable_message.c_str());
    }
    return std::pair{input, console_mode};
}

[[nodiscard]] auto RunForeground(ForegroundOptions options) {
    const auto [input, console_mode] = GetForegroundConsoleInput(
        "--foreground requires an attached console");
    const auto persistent = ResolvePersistentPaths();
    const auto backup_lock = AcquireBackupLock(persistent.backup_lock);
    return RunForegroundOperation(
        input, console_mode,
        [options = std::move(options)](const HANDLE cancellation_event) {
            return RunNativeBackup(
                cancellation_event,
                options.no_writers,
                options.volume_override,
                options.namespace_override);
        });
}

[[nodiscard]] auto RunQueryManifest(
    std::optional<std::u8string> namespace_override) {
    const auto [input, console_mode] = GetForegroundConsoleInput(
        "--query-manifest requires an attached console");
    return RunForegroundOperation(
        input, console_mode,
        [namespace_override = std::move(namespace_override)](
            const HANDLE cancellation_event) {
            const auto result = RetrievePreviousBackupManifest(
                cancellation_event, namespace_override);
            if (!result) {
                return kCancelledExitCode;
            }
            if (result->exit_code != 0) {
                return result->exit_code;
            }
            devicefs::WriteToStream(std::cout, result->manifest);

            const auto snapshot_volumes =
                result->ParseManifest().QuerySnapshotVolumes();
            devicefs::WriteToStream(
                std::cout,
                "\n\nValidated snapshots still available:\n");
            if (snapshot_volumes.empty()) {
                devicefs::WriteToStream(std::cout, "  (none)\n");
            }
            for (const auto &[volume_identifier, snapshot] :
                snapshot_volumes) {
                devicefs::WriteToStream(
                    std::cout,
                    "  Volume ID: {}\n"
                    "    Snapshot ID: {}\n"
                    "    Device: {}\n",
                    winrt::to_string(winrt::to_hstring(volume_identifier)),
                    winrt::to_string(
                        winrt::to_hstring(snapshot.snapshot_identifier)),
                    snapshot.device);
            }
            return 0;
        });
}

[[nodiscard]] auto RunIncrementalDiagnosticMode(
    IncrementalDiagnosticOptions options) {
    const auto [input, console_mode] = GetForegroundConsoleInput(
        "incremental diagnostics require an attached console");
    const auto persistent = ResolvePersistentPaths();
    const auto backup_lock = AcquireBackupLock(persistent.backup_lock);
    return RunForegroundOperation(
        input, console_mode,
        [options = std::move(options)](
            const HANDLE cancellation_event) {
            return RunIncrementalDiagnostics(
                cancellation_event, options);
        });
}

[[nodiscard]] auto RunInventoryVhdx(std::string device) {
    const auto [input, console_mode] = GetForegroundConsoleInput(
        "--inventory-vhdx requires an attached console");
    return RunForegroundOperation(
        input, console_mode,
        [device = std::move(device)](
            const HANDLE cancellation_event) {
            return InventoryVhdx(cancellation_event, device);
        });
}

auto WINAPI ServiceMain(
    const DWORD argc, char **) noexcept -> void {
    static auto context = ServiceContext{};
    context.status_handle = RegisterServiceCtrlHandlerExA(
        kServiceName.data(), ServiceControlHandler, &context);
    if (context.status_handle == nullptr) {
        return;
    }
    SetServiceState(context, SERVICE_START_PENDING,
        ERROR_SUCCESS, 0, ServiceContext::kInitialPendingCheckpoint,
        ServiceContext::kStartWaitHint);
    const auto result = RunService(context, argc);
    SetServiceState(context, SERVICE_STOPPED,
        result.win32_error, result.service_error);
}

auto RunServiceDispatcher() {
    [[gsl::suppress("type.3",
        justification: "The SCM retains a mutable historical parameter for an input-only service name.")]]
    auto *const service_name = const_cast<PSTR>(kServiceName.data());
    auto entries = std::array{
        SERVICE_TABLE_ENTRYA{
            .lpServiceName = service_name,
            .lpServiceProc = ServiceMain,
        },
        SERVICE_TABLE_ENTRYA{},
    };
    if (!StartServiceCtrlDispatcherA(entries.data())) {
        WinError("could not connect to the Service Control Manager");
    }
    return 0;
}

auto PrintHelp() noexcept {
    devicefs::WriteToStream(
        std::cout,
        "Usage:\n"
        "  backup-supervisor.exe --devicefs [devicefs arguments]\n"
        "  backup-supervisor.exe --foreground [--no-writers] "
        "[--namespace NAMESPACE] "
        "[--volumes VOLUME[,VOLUME...]]\n"
        "      --namespace and --volumes override configured values.\n"
        "  backup-supervisor.exe --query-manifest "
        "[--namespace NAMESPACE]\n"
        "  backup-supervisor.exe --inventory-vhdx DEVICE\n"
        "  backup-supervisor.exe [--incremental-verify] "
        "[--incremental-stats]\n"
        "      [--expose-synthetic-backup [MOUNT-POINT] |\n"
        "       --verify-synthetic-backup [MOUNT-POINT] "
        "[--verify-percentage PERCENT]]\n"
        "      Omit MOUNT-POINT to use a temporary SystemTemp directory.\n"
        "      [--namespace NAMESPACE] [--baseline SNAPSHOT-ID]\n"
        "      [--volumes VOLUME[,VOLUME...]]\n"
        "  backup-supervisor.exe --install-service [--update]\n"
        "  backup-supervisor.exe --run-service\n");
}

} // namespace

auto wmain(
    _Pre_satisfies_(argc > 0) const int argc,
    _In_reads_(argc) wchar_t **const argv) -> int {
    std::ignore = std::setlocale(LC_CTYPE, ".UTF8");
    try {
        HardenProcess();
        const auto arguments =
            std::span{argv + 1, argv + argc} |
            std::views::transform([](const auto argument) {
                return std::filesystem::path{argument}.string();
            }) |
            std::ranges::to<std::vector<std::string>>();
        if (!arguments.empty() &&
            (arguments.front() == "--devicefs")) {
            return devicefs::Main(arguments);
        }
        if (arguments.empty()) {
            PrintHelp();
            return 0;
        }
        const auto &option = arguments.front();
        if (option == kRunServiceOption) {
            if (arguments.size() != 1) {
                throw std::invalid_argument(
                    "--run-service does not accept arguments");
            }
            return RunServiceDispatcher();
        }
        if (option == kOrchestrateOption) {
            if (arguments.size() != 1) {
                throw std::invalid_argument(
                    "--orchestrate does not accept arguments");
            }
            const auto persistent = ResolvePersistentPaths();
            const auto backup_lock = AcquireBackupLock(
                persistent.backup_lock);
            const auto cancellation_event = OpenCancellationEvent();
            return RunNativeBackup(
                cancellation_event.get(), false, {}, std::nullopt);
        }
        if (option == "--query-manifest") {
            return RunQueryManifest(
                ParseNamespaceOverride(
                    std::span{arguments}.subspan(1), "--query-manifest"));
        }
        if (option == "--inventory-vhdx") {
            if (arguments.size() != 2) {
                throw std::invalid_argument(
                    "--inventory-vhdx requires exactly one DEVICE");
            }
            return RunInventoryVhdx(arguments[1]);
        }
        if ((option == "--incremental-stats") ||
            (option == "--incremental-verify") ||
            (option == "--expose-synthetic-backup") ||
            (option == "--verify-synthetic-backup")) {
            return RunIncrementalDiagnosticMode(
                ParseIncrementalDiagnosticOptions(arguments));
        }
        if (option == "--install-service") {
            if (arguments.size() == 1) {
                InstallService(InstallMode::CreateOnly,
                    ServiceContext::kMinimumPreshutdownTimeout);
                return 0;
            }
            if ((arguments.size() == 2) &&
                (arguments[1] == "--update")) {
                InstallService(InstallMode::CreateOrUpdate,
                    ServiceContext::kMinimumPreshutdownTimeout);
                return 0;
            }
            throw std::invalid_argument(
                "--install-service accepts only the optional --update argument");
        }
        if (option == "--foreground") {
            return RunForeground(
                ParseForegroundOptions(std::span{arguments}.subspan(1)));
        }
        throw std::invalid_argument("unknown backup-supervisor argument");
    } catch (const std::invalid_argument &error) {
        devicefs::WriteToStream(
            std::cerr, "backup-supervisor: {}\n", error.what());
        return 2;
    } catch (const devicefs::vshadow::OperationError &error) {
        devicefs::WriteToStream(
            std::cerr, "backup-supervisor: {}\n", error.what());
        return 2;
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(
            std::cerr, "backup-supervisor: {}\n", error.what());
        return 1;
    }
}
