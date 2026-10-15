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

export module devicefs.supervisor;

import std;
import <devicefs/windows_imports.h>;
import <sal.h>;
import <devicefs/common.h>;
import devicefs.coherent_vector;
import devicefs.filesystem;
import devicefs.guid_formatter;
import devicefs.stream_writer;
import devicefs.supervisor.account_management;
import devicefs.supervisor.browse;
import devicefs.supervisor.launch_powershell;
import devicefs.supervisor.installation;
import devicefs.supervisor.logging_console;
import devicefs.supervisor.materialize_oci;
import devicefs.supervisor.oci_signature_sandbox;
import devicefs.supervisor.native_backup;
import devicefs.supervisor.process_diagnostics;
import devicefs.supervisor.process_launch;
import devicefs.supervisor.vshadow;
import devicefs.supervisor.view_output;
import devicefs.terminal.menu;
import devicefs.terminal.transcoding;
import devicefs.terminal.windows;
import devicefs.vss_block_descriptors.cli;

using devicefs::terminal::Transcode;

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using namespace wil::literals;

constexpr auto kCancellationEventName =
    "Local\\devicefs-backup-stop"_zv;
constexpr auto kOrchestrateOption = "--orchestrate"_zv;
constexpr auto kRunInConsoleOption = "--internal-run-in-console"sv;

[[nodiscard]] auto CreateCancellationEvent(
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

[[nodiscard]] auto OpenCancellationEvent() {
    auto event = wil::unique_event_nothrow{OpenEventA(
        SYNCHRONIZE, FALSE, kCancellationEventName.c_str())};
    if (!event) {
        WinError("failed to open backup cancellation event '{}'",
            std::string_view{kCancellationEventName});
    }
    return event;
}

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
        const std::chrono::milliseconds timeout) const {
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
    const auto supervisor = Transcode<std::string>(CurrentExecutablePath().native());
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

struct ForegroundContext {
    wil::unique_event_nothrow cancellation_event = CreateCancellationEvent(nullptr);
    wil::unique_event_nothrow completed_event = CreateCancellationEvent(nullptr);
};

// Each invocation has its own events. A handler's snapshot keeps those events
// alive even if the invocation returns while the handler is still using them.
auto foreground_contexts = std::optional<
    CoherentVector<std::shared_ptr<const ForegroundContext>>>{};
auto foreground_contexts_once = std::once_flag{};

auto WINAPI ForegroundControlHandler(const DWORD) noexcept -> BOOL {
    const auto contexts = foreground_contexts->Read();
    if (!contexts || contexts->empty()) {
        return FALSE;
    }
    // Windows terminates the process when a closure handler returns. Waiting
    // for the operations here lets them finish their cleanup first.
    // https://learn.microsoft.com/en-us/windows/console/handlerroutine
    //
    // Signal every operation before waiting, because one operation's cleanup
    // may depend on another operation finishing.
    for (const auto &context : *contexts) {
        context->cancellation_event.SetEvent();
    }
    for (const auto &context : *contexts) {
        context->completed_event.wait();
    }
    return TRUE;
}

// Return a worker callback whose cancellation and completion events are
// registered only while the operation is running.
[[nodiscard]] auto MakeForegroundOperation(
    const std::shared_ptr<const ForegroundContext> &context, auto operation) {
    return [context, operation = std::move(operation)]() mutable {
        const auto completed = context->completed_event.SetEvent_scope_exit();
        std::call_once(foreground_contexts_once, [] {
            foreground_contexts.emplace();
            if (!SetConsoleCtrlHandler(ForegroundControlHandler, TRUE)) {
                WinError("failed to register the foreground console control handler");
            }
        });
        foreground_contexts->PushBack(context);
        const auto remove_context = wil::scope_exit([&context] {
            foreground_contexts->Erase(context);
        });
        return std::invoke(std::move(operation), context->cancellation_event.get());
    };
}

template <typename Operation>
[[nodiscard]] auto RunForegroundOperation(
    const std::string_view unavailable_message,
    Operation operation) {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    auto console_mode = DWORD{};
    if (!GetConsoleMode(input, &console_mode)) {
        WinError("{}", unavailable_message);
    }
    const auto context = std::make_shared<const ForegroundContext>();
    if (!SetConsoleMode(
            input, console_mode & ~DWORD{ENABLE_PROCESSED_INPUT})) {
        WinError("failed to enable foreground cancellation input");
    }
    auto restore_console_mode = wil::scope_exit([input, console_mode] {
        if (SetConsoleMode(input, console_mode)) {
            return;
        }
        devicefs::WriteToStream(devicefs::stderr, "backup-supervisor: {}.\n",
            TryConstructWinError(
                "failed to restore the foreground console input mode"));
    });

    auto result = std::async(
        std::launch::async, MakeForegroundOperation(context, std::move(operation)));
    auto cancel_on_monitor_failure =
        context->cancellation_event.SetEvent_scope_exit();
    // The first signaled handle wins, giving completion priority over input.
    // https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitformultipleobjects
    constexpr auto kInputBatchSize = 16uz;
    while (true) {
        if (wil::wait_any_failfast(
                context->completed_event, input).status() == WAIT_OBJECT_0) {
            break;
        }
        auto records = std::array<INPUT_RECORD, kInputBatchSize>{};
        auto read = DWORD{};
        if (!ReadConsoleInputW(input, records.data(),
                DWORD{records.size()}, &read)) {
            WinError("failed to read foreground console input");
        }
        for (const auto &record : std::span{records}.first(read)) {
            // With VT input enabled, Ctrl+C can arrive as the ETX character
            // (`0x03`) without a virtual key code or Ctrl modifier. This was
            // observed when running through CTest. Both representations
            // request cancellation, allowing callers to retain VT input.
            if ((record.EventType != KEY_EVENT) ||
                !record.Event.KeyEvent.bKeyDown ||
                ((record.Event.KeyEvent.uChar.UnicodeChar != L'\x03') &&
                    ((record.Event.KeyEvent.wVirtualKeyCode != L'C') ||
                        ((record.Event.KeyEvent.dwControlKeyState &
                            (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) == 0)))) {
                continue;
            }
            context->cancellation_event.SetEvent();
            devicefs::WriteToStream(devicefs::stdout,
                "Cancellation requested; waiting for cleanup.\n");
        }
    }
    cancel_on_monitor_failure.release();
    return result.get();
}

struct ForegroundOptions {
    bool no_writers = false;
    std::optional<std::u8string> namespace_override;
    std::vector<std::string> volume_override;
};

struct SelectiveViewOptions {
    std::string archive;
    std::optional<std::string> snapshot_override;
    std::optional<std::string> timestamp;
    std::string address = "127.0.0.1";
    std::optional<std::u8string> namespace_override;
};

[[nodiscard]] auto ParseVolumeList(std::string_view source) {
    auto result = std::vector<std::string>{};
    constexpr auto kWhitespace = " \t\r\n\f\v"sv;
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

[[nodiscard]] auto ParseBackupOverride(
    const std::span<const std::string_view> arguments,
    std::size_t &index, auto &options) {
    const auto argument = arguments[index];
    if ((argument != "--namespace") && (argument != "--volume") &&
        (argument != "--volumes")) {
        return false;
    }
    if (++index == arguments.size()) {
        throw std::invalid_argument(std::format("{} requires a value",
            argument == "--namespace" ? argument : "--volume/--volumes"sv));
    }
    if (argument == "--namespace") {
        options.namespace_override.emplace(
            arguments[index].begin(), arguments[index].end());
    } else {
        options.volume_override = ParseVolumeList(arguments[index]);
    }
    return true;
}

[[nodiscard]] auto ParseForegroundOptions(
    const std::span<const std::string_view> arguments) {
    auto result = ForegroundOptions{};
    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (ParseBackupOverride(arguments, index, result)) {
            continue;
        }
        if (argument == "--no-writers") {
            result.no_writers = true;
        } else {
            throw std::invalid_argument(std::format(
                "--foreground received an unknown argument: {}", argument));
        }
    }
    return result;
}

[[nodiscard]] auto ParseSelectiveViewOptions(
    const std::span<const std::string_view> arguments) {
    if (arguments.empty()) {
        throw std::invalid_argument("--view requires ARCHIVE");
    }

    auto result = SelectiveViewOptions{
        .archive = std::string{arguments[0]},
    };
    for (auto index = 1uz; index < arguments.size(); ++index) {
        const auto option = arguments[index];
        if ((option != "--snapshot") &&
            (option != "--timestamp") &&
            (option != "--address") &&
            (option != "--namespace")) {
            throw std::invalid_argument(std::format(
                "--view received an unknown argument: {}", option));
        }
        if (++index == arguments.size()) {
            throw std::invalid_argument(std::format(
                "{} requires a value", option));
        }
        if (option == "--snapshot") {
            result.snapshot_override.emplace(arguments[index]);
        } else if (option == "--timestamp") {
            result.timestamp.emplace(arguments[index]);
        } else if (option == "--address") {
            result.address = arguments[index];
        } else if (option == "--namespace") {
            result.namespace_override.emplace(
                arguments[index].begin(), arguments[index].end());
        }
    }
    return result;
}

[[nodiscard]] auto ParseVerificationPercentage(const std::string_view value) {
    auto percentage = double{};
    const auto [end, error] = std::from_chars(
        value.data(), value.data() + value.size(), percentage);
    if ((error != std::errc{}) || (end != value.data() + value.size()) ||
        !std::isfinite(percentage) || (percentage <= 0.0) || (percentage > 100.0)) {
        throw std::invalid_argument(std::format(
            "--verify-percentage requires a number greater than "
            "zero and no greater than 100; received '{}'", value));
    }
    return percentage;
}

[[nodiscard]] auto ParseIncrementalDiagnosticOptions(
    const std::span<const std::string_view> arguments) {
    auto result = IncrementalDiagnosticOptions{};
    auto verification_percentage_supplied = false;
    for (auto index = 0uz; index < arguments.size(); ++index) {
        const auto argument = arguments[index];
        if (ParseBackupOverride(arguments, index, result)) {
            continue;
        }
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
            result.filesystem_verification_percentage =
                ParseVerificationPercentage(arguments[index]);
            verification_percentage_supplied = true;
        } else if ((argument == "--baseline") || (argument == "--payload")) {
            if (++index == arguments.size()) {
                throw std::invalid_argument(
                    std::format("{} requires a value", argument));
            }
            auto &destination = argument == "--baseline"
                ? result.baseline_snapshot_identifier : result.payload_snapshot_identifier;
            destination = [option = argument, value = arguments[index]] {
                const auto identifier = ParseGuid(value);
                if (!identifier) {
                    throw std::invalid_argument(std::format(
                        "{} requires a snapshot GUID; received '{}'", option, value));
                }
                return *identifier;
            }();
        } else {
            throw std::invalid_argument(std::format(
                "incremental diagnostics received an unknown argument: {}",
                argument));
        }
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
    const std::span<const std::string_view> arguments,
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

[[nodiscard]] auto RunForeground(ForegroundOptions options) {
    const auto persistent = ResolvePersistentPaths();
    const auto backup_lock = AcquireBackupLock(persistent.backup_lock);
    return RunForegroundOperation(
        "--foreground requires an attached console",
        [options = std::move(options)](const HANDLE cancellation_event) {
            return RunNativeBackup(
                cancellation_event,
                options.no_writers,
                options.volume_override,
                options.namespace_override);
        });
}

[[nodiscard]] auto RunInConsoleMode(
    const std::span<const std::string_view> arguments) {
    if ((arguments.size() < 2) || (arguments.front() != "--")) {
        throw std::invalid_argument(std::format(
            "{} requires -- COMMAND [ARGUMENT ...]", kRunInConsoleOption));
    }
    auto console = LoggingConsole([](const std::u8string_view output) noexcept {
        devicefs::WriteToStream(devicefs::stdout, output);
        return ERROR_SUCCESS;
    });
    const auto process = console.StartProcess(
        std::to_array<std::span<const std::string_view>>({
            std::views::single(std::string_view{[command = arguments[1]] {
                auto path = std::wstring{};
                if (const auto status = wil::SearchPathW(nullptr,
                        Transcode<wchar_t>(command).data(), L".exe", path);
                    FAILED(status)) {
                    WinError("failed to find executable '{}'",
                        command, ExplicitHresult{status});
                }
                return Transcode<char>(path);
            }()}),
            arguments.subspan(2),
        }) | std::views::join | std::ranges::to<std::vector<std::string>>());
    wil::handle_wait(process.hProcess);
    return ProcessExitCode(process.hProcess);
}

[[nodiscard]] auto RunFishProgramMode(
    const std::span<const std::string_view> arguments) {
    if (arguments.empty() ||
        ((arguments.size() > 1) && (arguments[1] != "--"))) {
        throw std::invalid_argument(
            "--run-fish-program requires FILE [-- PROGRAM-ARGUMENTS...]");
    }
    return RunForegroundOperation("--run-fish-program requires an attached console",
        [arguments](const HANDLE cancellation_event) {
            return RunFishProgram(cancellation_event,
                std::filesystem::path{arguments.front()},
                arguments.subspan(1));
        });
}

[[nodiscard]] auto RunListBackups(
    std::optional<std::u8string> namespace_override) {
    return RunForegroundOperation(
        "--list-backups requires an attached console",
        [namespace_override = std::move(namespace_override)](
            const HANDLE cancellation_event) {
            return devicefs::WriteToStream(devicefs::stdout,
                RetrieveBackupCatalog(
                    cancellation_event, namespace_override)) ? 0 : 1;
        });
}

[[nodiscard]] auto RunQueryManifest(
    std::optional<std::u8string> namespace_override) {
    return RunForegroundOperation(
        "--query-manifest requires an attached console",
        [namespace_override = std::move(namespace_override)](
            const HANDLE cancellation_event) {
            const auto result = RetrieveBackupManifest(
                cancellation_event, namespace_override);
            if (result.exit_code != 0) {
                return result.exit_code;
            }
            devicefs::WriteToStream(devicefs::stdout, result.manifest);

            const auto snapshot_volumes =
                result.ParseManifest().QuerySnapshotVolumes();
            devicefs::WriteToStream(
                devicefs::stdout,
                "\n\nValidated snapshots still available:\n");
            if (snapshot_volumes.empty()) {
                devicefs::WriteToStream(devicefs::stdout, "  (none)\n");
            }
            for (const auto &[volume_identifier, snapshot] :
                snapshot_volumes) {
                devicefs::WriteToStream(
                    devicefs::stdout,
                    "  Volume ID: {}\n"
                    "    Snapshot ID: {}\n"
                    "    Device: {}\n",
                    FormatGuid(volume_identifier),
                    FormatGuid(snapshot.snapshot_identifier),
                    snapshot.device);
            }
            return 0;
        });
}

template <typename Operation, typename Header, typename Cancellation,
    devicefs::terminal::MenuTerminal Console>
struct CancellableOutputOperation {
    Operation operation;
    Header draw_header;
    Cancellation cancel;
    Console &terminal;
    const bool initially_visible = true;
    std::optional<decltype(terminal.EnterScreen())> screen;
    bool dismissal_requested = false;

    auto notify_completion(devicefs::terminal::OutputMenu &menu,
        const int exit_code, const auto &close) -> void {
        if ((exit_code == EXIT_SUCCESS) &&
            (!initially_visible || dismissal_requested)) {
            close();
            return;
        }
        if (!initially_visible) {
            screen.emplace(terminal.EnterScreen());
        }
        menu.SetVisible(true);
        if (dismissal_requested) {
            menu.AppendLine("Operation failed after cancellation was requested.");
        }
        menu.SetCommands(std::array{devicefs::terminal::OutputCommand{
            0, initially_visible ? "Close" : "Continue"}});
    }

    auto dispatch(devicefs::terminal::OutputMenu &,
        auto &task, std::optional<std::size_t>, const auto &close) -> void {
        if (*task || task.MaybeCompleteTask()) {
            close();
            return;
        }
        dismissal_requested = true;
        cancel();
    }
};

[[nodiscard]] auto RunBackupViewInScrollingScreen(
    devicefs::terminal::WindowsConsole &terminal,
    SelectiveViewOptions options, BackupViewUser view_user) {
    const auto context = std::make_shared<const ForegroundContext>();
    auto &account = (view_user.shell_user && *view_user.shell_user)
        ? **view_user.shell_user : view_user.invoking_user;
    return RunOperationWithOutput(CancellableOutputOperation{
        .operation = MakeForegroundOperation(context, [&options, &view_user, &account](
            const HANDLE cancellation_event) {
            if (!view_user.shell_user) {
                devicefs::WriteToStream(devicefs::stdout,
                    "Information: Backup viewing will continue with inspection "
                    "permission for the invoking user.\n"
                    "The optional account lookup for the desktop process could "
                    "not be completed. Details: {}\n",
                    view_user.shell_user.error());
            }
            return RunSelectiveView(
                cancellation_event,
                std::move(account.information),
                account.account_name,
                options.archive,
                options.snapshot_override,
                options.timestamp,
                options.address,
                options.namespace_override);
        }),
        .draw_header = [&options](auto &frame) {
            const auto namespace_name = options.namespace_override
                ? (options.namespace_override->empty() ? std::string{"(root)"} :
                    Transcode<std::string>(*options.namespace_override))
                : std::string{"(configured namespace)"};
            const auto backup = options.snapshot_override.value_or("(default backup group)");
            frame.Write("DeviceFs Backup Supervisor\nBrowse backup\n\n"
                "Namespace: {}\nBackup: {}\nImage: {}\n\n",
                namespace_name,
                options.timestamp ? std::format("{}/{}", backup, *options.timestamp) : backup,
                options.archive);
        },
        .cancel = [context] {
            context->cancellation_event.SetEvent();
        },
        .terminal = terminal,
    });
}

[[nodiscard]] auto RunSelectiveViewMode(SelectiveViewOptions options) {
    return RunForegroundOperation("--view requires an attached console",
        [options = std::move(options)](const HANDLE cancellation_event) {
            return RunSelectiveView(cancellation_event, {}, {},
                options.archive, options.snapshot_override, options.timestamp,
                options.address, options.namespace_override);
        });
}

[[nodiscard]] auto RunFastOperation(
    devicefs::terminal::WindowsConsole &terminal,
    const std::string_view title, auto operation) {
    const auto context = std::make_shared<const ForegroundContext>();
    return RunOperationWithOutput(CancellableOutputOperation{
        .operation = MakeForegroundOperation(context, std::move(operation)),
        .draw_header = [title](auto &frame) {
            frame.Write("DeviceFs Backup Supervisor\n{}\n\n", title);
        },
        .cancel = [context] {
            context->cancellation_event.SetEvent();
        },
        .terminal = terminal,
        .initially_visible = false,
    });
}

[[nodiscard]] auto RunBrowseMode(
    std::optional<std::u8string> namespace_override) {
    auto terminal = devicefs::terminal::WindowsConsole{};
    auto catalog = std::u8string{};
    const auto result = RunFastOperation(terminal, "Load backup catalog",
        [&catalog, &namespace_override](const HANDLE cancellation_event) {
            catalog = RetrieveBackupCatalog(cancellation_event, namespace_override);
            return EXIT_SUCCESS;
        });
    if (result != EXIT_SUCCESS) {
        return result;
    }
    const auto screen = terminal.EnterScreen();
    auto selection = SelectBackup(terminal, catalog,
        [&terminal](const std::u8string_view namespace_name,
            const std::string_view snapshot) -> std::optional<std::u8string> {
            auto manifest = std::u8string{};
            std::ignore = RunFastOperation(
                terminal, "Load backup manifest",
                [snapshot, namespace_name, &manifest](
                    const HANDLE cancellation_event) {
                    auto response = RetrieveBackupManifest(
                        cancellation_event, std::u8string{namespace_name},
                        snapshot);
                    if (response.exit_code == EXIT_SUCCESS) {
                        manifest = std::move(response.manifest);
                    }
                    return response.exit_code;
                });
            // The manifest enriches labels. An empty result retains the
            // image names supplied by the catalog.
            return manifest;
        });
    if (!selection) {
        return EXIT_SUCCESS;
    }
    return RunBackupViewInScrollingScreen(terminal, SelectiveViewOptions{
        .archive = selection->archive,
        .snapshot_override = selection->snapshot,
        .namespace_override = selection->namespace_name,
    }, std::move(selection->view_user));
}

[[nodiscard]] auto RunIncrementalDiagnosticMode(
    IncrementalDiagnosticOptions options) {
    const auto persistent = ResolvePersistentPaths();
    const auto backup_lock = AcquireBackupLock(persistent.backup_lock);
    return RunForegroundOperation(
        "incremental diagnostics require an attached console",
        [options = std::move(options)](
            const HANDLE cancellation_event) {
            return RunIncrementalDiagnostics(
                cancellation_event, options);
        });
}

[[nodiscard]] auto RunInventoryVhdx(std::string device) {
    return RunForegroundOperation(
        "--inventory-vhdx requires an attached console",
        [device = std::move(device)](
            const HANDLE cancellation_event) {
            return InventoryVhdx(cancellation_event, device);
        });
}

[[nodiscard]] auto RunVerifyFilesystems(
    const std::span<const std::string_view> arguments) {
    if ((arguments.size() != 3) &&
        ((arguments.size() != 5) || (arguments[3] != "--verify-percentage"))) {
        throw std::invalid_argument(
            "--verify-filesystems requires SYNTHETIC-ROOT REAL-ROOT "
            "and an optional --verify-percentage PERCENT");
    }
    const auto percentage = arguments.size() == 5
        ? ParseVerificationPercentage(arguments[4]) : 100.0;
    return RunForegroundOperation("--verify-filesystems requires an attached console",
        [synthetic = std::filesystem::path{arguments[1]},
            real = std::filesystem::path{arguments[2]}, percentage](
            const HANDLE cancellation_event) {
            return VerifyMountedFilesystems(
                cancellation_event, synthetic, real, percentage);
        });
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

auto RunServiceDispatcher() {
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

auto RunInstallerInScrollingView(auto &&terminal, auto &&screen) {
    return RunOperationWithOutput(CancellableOutputOperation{
        .operation = [] {
            InstallService(ServiceContext::kMinimumPreshutdownTimeout);
            return EXIT_SUCCESS;
        },
        .draw_header = [](auto &frame) {
            frame.Write("DeviceFs Backup Supervisor\n"
                "Install or update backup environment\n\n");
        },
        .cancel = [&screen, &terminal] {
            // A presentation failure is already propagating during unwinding.
            // Returning allows the future's destructor to wait for installation,
            // after which the original error reaches the caller.
            if (std::uncaught_exceptions() != 0) {
                return;
            }
            screen.reset();
            terminal.reset();
            ExitProcess(ERROR_CANCELLED);
        },
        .terminal = *terminal,
    });
}

auto InvokeSelfAsAdministrator() {
    const auto com_result = CoInitializeEx(
        nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const auto uninitialize = wil::unique_couninitialize_call{SUCCEEDED(com_result)};
    if (FAILED(com_result)) {
        WinError("failed to initialize COM before requesting administrator access",
            ExplicitHresult{com_result});
    }
    const auto executable = CurrentExecutablePath();
    const auto parameters = Transcode<std::wstring>(kRunInWindowsTerminalOption);
    auto launch = SHELLEXECUTEINFOW{
        .cbSize = sizeof(SHELLEXECUTEINFOW),
        .fMask = SEE_MASK_NOASYNC,
        .lpVerb = L"runas",
        .lpFile = executable.c_str(),
        .lpParameters = parameters.c_str(),
        .nShow = SW_HIDE,
    };
    if (!ShellExecuteExW(&launch)) {
        WinError("failed to relaunch backup supervisor '{}' as an administrator",
            std::wstring_view{executable.native()});
    }
}

[[nodiscard]] auto RunMainMenu() {
    const auto is_administrator = [] {
        auto is_administrator = bool{};
        const auto token_result = wil::test_token_membership_nothrow(
            &is_administrator, nullptr, SECURITY_NT_AUTHORITY,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS);
        if (FAILED(token_result)) {
            devicefs::WriteToStream(
                devicefs::stderr, "backup-supervisor: {}\n",
                    TryConstructWinError(
                        "failed to determine whether invoking user has "
                        "administrator access; continuing anyway",
                        ExplicitHresult{token_result}));
            return true;
        }
        return is_administrator;
    }();
    if (!is_administrator) {
        InvokeSelfAsAdministrator();
        return 0;
    }

    auto terminal = std::optional<devicefs::terminal::WindowsConsole>{std::in_place};
    auto screen = terminal->EnterScreen();
    if (!BackupServiceExists()) {
        return RunInstallerInScrollingView(terminal, screen);
    }
    const auto entries = std::to_array<std::pair<
        std::string_view, std::function<auto () -> void>>>({
        {"Install or update backup environment"sv,
            [&terminal, &screen] {
            RunInstallerInScrollingView(terminal, screen);
        }},
        {"Browse backups"sv, [&terminal] {
            std::ignore = RunBrowseMode(std::nullopt);
            terminal->InvalidateFrame();
        }},
        {"Close"sv, nullptr}
    });
    const auto keys = entries | std::views::keys |
        std::ranges::to<std::vector<std::string_view>>();
    auto selection = std::size_t{};
    while (const auto maybe_selection =
        devicefs::terminal::SelectMenuItem(*terminal, [](auto &frame) {
            frame.Write("DeviceFs Backup Supervisor\nMain menu\n\n");
        }, keys, {}, selection)) {
        selection = *maybe_selection;
        const auto operation = entries.at(selection).second;
        if (!operation) {
            break;
        }
        std::invoke(operation);
    }
    return 0;
}

auto PrintHelp() noexcept {
    devicefs::WriteToStream(
        devicefs::stdout,
        "Usage:\n"
        "  backup-supervisor.exe\n"
        "      Request administrator access, then install or open the main menu.\n"
        "  backup-supervisor.exe --help\n"
        "  backup-supervisor.exe --devicefs [devicefs arguments]\n"
        "  backup-supervisor.exe --backup-console\n"
        "  backup-supervisor.exe --foreground [--no-writers] "
        "[--namespace NAMESPACE] "
        "[--volumes VOLUME[,VOLUME...]]\n"
        "      --namespace and --volumes override configured values.\n"
        "  backup-supervisor.exe --query-manifest "
        "[--namespace NAMESPACE]\n"
        "  backup-supervisor.exe --list-backups [--namespace NAMESPACE]\n"
        "      Print the namespace's full backup catalog as JSON.\n"
        "  backup-supervisor.exe --browse [--namespace NAMESPACE]\n"
        "      Choose a backup and image archive to open with --view.\n"
        "  backup-supervisor.exe --view ARCHIVE "
        "[--snapshot SNAPSHOT] [--timestamp TIMESTAMP] "
        "[--address ADDRESS] "
        "[--namespace NAMESPACE]\n"
        "      ADDRESS defaults to 127.0.0.1.\n"
        "  backup-supervisor.exe --inventory-vhdx DEVICE\n"
        "  backup-supervisor.exe --verify-filesystems SYNTHETIC-ROOT REAL-ROOT\n"
        "      [--verify-percentage PERCENT]\n"
        "      Compare two already mounted volume roots.\n"
        "      Keep both volumes mounted until verification finishes.\n"
        "  backup-supervisor.exe [--incremental-verify] "
        "[--incremental-stats]\n"
        "      [--expose-synthetic-backup [MOUNT-POINT] |\n"
        "       --verify-synthetic-backup [MOUNT-POINT] "
        "[--verify-percentage PERCENT]]\n"
        "      Omit MOUNT-POINT to use a temporary SystemTemp directory.\n"
        "      [--namespace NAMESPACE] [--baseline SNAPSHOT-ID] [--payload SNAPSHOT-ID]\n"
        "      --payload uses an existing payload snapshot instead of creating one.\n"
        "      [--volumes VOLUME[,VOLUME...]]\n"
        "  backup-supervisor.exe --install\n"
        "  backup-supervisor.exe --run-service\n");
}

} // namespace

namespace supervisor_detail::hashing {

// Compute a 64-bit FNV-1a hash.
[[nodiscard]] constexpr auto Hash(
    const std::string_view text) noexcept -> std::uint64_t {
    return std::ranges::fold_left(text, std::uint64_t{0xcbf29ce484222325},
        [](const auto result, const char character) noexcept {
            return (result ^ std::bit_cast<unsigned char>(character)) *
                0x100000001b3;
        });
}

[[nodiscard]] consteval auto operator""_h(
    _In_ const char *const text, const std::size_t size) noexcept {
    return Hash(std::string_view{text, size});
}

} // namespace supervisor_detail::hashing

export auto BackupSupervisorMain(
    const std::span<const std::string_view> arguments) -> int {
    using namespace supervisor_detail::hashing;

    if (arguments.empty()) {
        return RunMainMenu();
    }
    const auto option = arguments.front();
    switch (Hash(option)) {
    case "--help"_h:
        PrintHelp();
        return 0;
    case Hash(kRunInConsoleOption):
        return RunInConsoleMode(arguments.subspan(1));
    case Hash(kRunInWindowsTerminalOption):
        return LaunchSupervisorConsole();
    case Hash(kVerifyOpenPgpDetachedSignatureOption):
        return RunOciSignatureWorker(arguments.subspan(1));
    case "--devicefs"_h:
        return devicefs::Main(arguments);
    case "--vss-descriptor-dump"_h:
        return VssDescriptorDumpMain(arguments.subspan(1));
    case "--backup-console"_h:
        return RunBackupConsole();
    case "--run-fish-program"_h:
        return RunFishProgramMode(arguments.subspan(1));
    case Hash(kRunServiceOption):
        if (arguments.size() != 1) {
            throw std::invalid_argument(
                "--run-service does not accept arguments");
        }
        return RunServiceDispatcher();
    case Hash(kOrchestrateOption): {
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
    case "--view"_h:
        return RunSelectiveViewMode(
            ParseSelectiveViewOptions(
                std::span{arguments}.subspan(1)));
    case "--list-backups"_h:
        return RunListBackups(
            ParseNamespaceOverride(
                std::span{arguments}.subspan(1), "--list-backups"));
    case "--browse"_h:
        devicefs::WriteToStream(devicefs::stdout,
            "Loading the backup catalog...\n");
        return RunBrowseMode(ParseNamespaceOverride(
            std::span{arguments}.subspan(1), "--browse"));
    case "--query-manifest"_h:
        return RunQueryManifest(
            ParseNamespaceOverride(
                std::span{arguments}.subspan(1), "--query-manifest"));
    case "--inventory-vhdx"_h:
        if (arguments.size() != 2) {
            throw std::invalid_argument(
                "--inventory-vhdx requires exactly one DEVICE");
        }
        return RunInventoryVhdx(std::string{arguments[1]});
    case "--verify-filesystems"_h:
        return RunVerifyFilesystems(arguments);
    case "--incremental-stats"_h:
    case "--incremental-verify"_h:
    case "--expose-synthetic-backup"_h:
    case "--verify-synthetic-backup"_h:
        return RunIncrementalDiagnosticMode(
            ParseIncrementalDiagnosticOptions(arguments));
    case "--install"_h:
        if (arguments.size() != 1) {
            throw std::invalid_argument(
                "--install does not accept arguments");
        }
        InstallService(ServiceContext::kMinimumPreshutdownTimeout);
        return EXIT_SUCCESS;
    case Hash(kPreparePowerShellProfileOption):
        EnsurePowerShellProfile();
        return 0;
    case Hash(kRegisterMsixOption):
        if (arguments.size() != 2) {
            throw std::invalid_argument(std::format(
                "{} requires exactly one PACKAGE_FULL_NAME", kRegisterMsixOption));
        }
        EnsureConsoleMsixRegistration(
            Transcode<std::wstring>(arguments[1]));
        return 0;
    case Hash(kMaterializeOciOption): {
        if (arguments.size() < 2) {
            throw std::invalid_argument(std::format(
                "{} requires DISTRIBUTION", kMaterializeOciOption));
        }
        if ((arguments.size() != 2) &&
            ((arguments.size() != 4) || (arguments[2] != "--oci"))) {
            throw std::invalid_argument(std::format(
                "expected {} DISTRIBUTION [--oci FILENAME]", kMaterializeOciOption));
        }
        const auto oci = (arguments.size() == 4) ?
            std::optional{std::filesystem::path{arguments[3]}} : std::nullopt;
        return MaterializeOci(arguments[1], oci) ? 0 : 1;
    }
    case "--foreground"_h:
        return RunForeground(
            ParseForegroundOptions(std::span{arguments}.subspan(1)));
    default:
        throw std::invalid_argument(std::format(
            "unknown backup-supervisor argument: {}", option));
    }
}
