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

#include <windows.h>
#include <userenv.h>

// wil/stl.h uses these facilities without including their standard headers.
#include <algorithm>
#include <cstdint>

#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/token_helpers.h>
#include <wil/win32_helpers.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

import devicefs.common;
import devicefs.filesystem;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.logging_console;
import devicefs.supervisor.process_diagnostics;

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose these C++23 functions.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
template <class T> auto start_lifetime_as_array(void *, size_t) noexcept -> T *;
} // namespace std
#endif

namespace {

constexpr auto kServiceName = wil::zwstring_view(L"DeviceFsBackup");
constexpr auto kCancellationEventName =
    wil::zwstring_view(L"Local\\devicefs-backup-stop");
constexpr auto kCancellationEventEnvironment =
    wil::zwstring_view(L"DEVICEFS_BACKUP_STOP_EVENT");
constexpr auto kBackupLockEnvironment =
    wil::zwstring_view(L"DEVICEFS_BACKUP_LOCK_PATH");
constexpr auto kSupervisorPathEnvironment =
    wil::zwstring_view(L"DEVICEFS_BACKUP_SUPERVISOR_PATH");
constexpr auto kDeviceFsOption = std::wstring_view(L"--devicefs");
constexpr auto kHelperOption =
    std::wstring_view(L"--run-wsl-as-pbs-vss");
constexpr auto kForegroundOption = std::wstring_view(L"--foreground");
constexpr auto kNoWritersOption = std::wstring_view(L"--no-writers");
constexpr auto kRunServiceOption = std::wstring_view(L"--run-service");
constexpr auto kInstallOption = std::wstring_view(L"--install-service");
constexpr auto kUpdateOption = std::wstring_view(L"--update");
constexpr auto kServiceDisplayName =
    wil::zwstring_view(L"DeviceFs Backup");
constexpr auto kLocalSystemAccount =
    wil::zwstring_view(L".\\LocalSystem");
constexpr auto kPowerShellPath =
    wil::zwstring_view(L"C:\\Program Files\\PowerShell\\7\\pwsh.exe");
constexpr auto kWslPath =
    wil::zwstring_view(L"C:\\Program Files\\WSL\\wsl.exe");
constexpr auto kOrchestratorName = std::wstring_view(L"Orchestrate-Backup.ps1");
constexpr auto kLogDirectory = std::wstring_view(L"logs");
constexpr auto kPasswordRelativePath =
    std::wstring_view(L"credentials\\pbs-vss.password");
constexpr auto kBackupLockRelativePath =
    std::wstring_view(L"credentials\\pbs-vss-backup.lock");
constexpr auto kPbsUser = wil::zwstring_view(L"pbs-vss");
constexpr auto kLocalDomain = wil::zwstring_view(L".");
constexpr auto kGracefulStopMilliseconds = DWORD{300000};
constexpr auto kStrayProcessWaitMilliseconds = DWORD{5000};
constexpr auto kForcedProcessWaitMilliseconds = DWORD{30000};
constexpr auto kStopWaitHintMilliseconds = kGracefulStopMilliseconds +
    kStrayProcessWaitMilliseconds + kForcedProcessWaitMilliseconds;
constexpr auto kMinimumPreshutdownMilliseconds =
    kStopWaitHintMilliseconds + 25000;
constexpr auto kJobPollMilliseconds = DWORD{100};
constexpr auto kInternalFailure = DWORD{1};
constexpr auto kAcceptedControls =
    DWORD{SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN};
constexpr auto kWslCreationFlags =
    DWORD{CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT};

[[nodiscard]] auto ExecutablePath() {
    auto result = std::wstring{};
    const auto error = wil::GetModuleFileNameW(nullptr, result);
    if (FAILED(error)) {
        WinError("could not obtain the backup supervisor path",
            HRESULT_CODE(error));
    }
    return result;
}

[[nodiscard]] auto InstallationDirectory() {
    return std::filesystem::path(ExecutablePath()).parent_path().parent_path();
}

auto PublishPersistentPaths(const std::filesystem::path &directory) {
    const auto backup_lock = directory / kBackupLockRelativePath;
    if (!SetEnvironmentVariableW(
            kBackupLockEnvironment.c_str(), backup_lock.c_str())) {
        WinError("could not publish the backup lock path");
    }
    const auto supervisor = ExecutablePath();
    if (!SetEnvironmentVariableW(
            kSupervisorPathEnvironment.c_str(), supervisor.c_str())) {
        WinError("could not publish the backup supervisor path");
    }
}

[[nodiscard]] auto CreateCancellationEvent() {
    auto event = wil::unique_event_nothrow{};
    auto already_exists = false;
    if (!event.try_create(wil::EventOptions::ManualReset,
            kCancellationEventName.c_str(), nullptr, &already_exists)) {
        WinError("could not create the cancellation event");
    }
    if (already_exists) {
        throw std::runtime_error("the cancellation event already exists");
    }
    return event;
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
    LoggingConsole console;
    wil::unique_process_information process;
    wil::unique_handle job;

    [[nodiscard]] auto Wait(const DWORD timeout) const {
        const auto result = WaitForSingleObject(process.hProcess, timeout);
        if (result == WAIT_FAILED) {
            WinError("could not wait for the backup orchestrator");
        }
        return result == WAIT_OBJECT_0;
    }

    [[nodiscard]] auto ExitCode() const {
        auto result = DWORD{};
        if (!GetExitCodeProcess(process.hProcess, &result)) {
            WinError("could not obtain the backup orchestrator exit code");
        }
        return result;
    }

    [[nodiscard]] auto WaitForAll(const DWORD timeout) const {
        const auto deadline = GetTickCount64() + timeout;
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
            if (GetTickCount64() >= deadline) {
                return false;
            }
            Sleep(kJobPollMilliseconds);
        }
    }

    auto TerminateAndWait(const DWORD exit_code) const {
        if (!TerminateJobObject(job.get(), exit_code)) {
            WinError("could not terminate the backup process job");
        }
        if (!WaitForAll(kForcedProcessWaitMilliseconds)) {
            throw std::runtime_error(
                "backup processes survived job termination");
        }
    }
};

[[nodiscard]] auto DuplicateInheritableHandle(const HANDLE source) {
    if ((source == nullptr) || (source == INVALID_HANDLE_VALUE)) {
        throw std::runtime_error("a child-process standard handle is unavailable");
    }
    auto duplicate = wil::unique_handle{};
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
            duplicate.addressof(), 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        WinError("could not duplicate a child-process standard handle");
    }
    return duplicate;
}

template <typename Start>
[[nodiscard]] auto StartProcessWithHandles(
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error,
    const HANDLE job,
    const DWORD additional_startup_flags,
    const Start &start,
    const wil::zstring_view operation) {
    auto child_handles = std::array{
        DuplicateInheritableHandle(standard_input),
        DuplicateInheritableHandle(standard_output),
        DuplicateInheritableHandle(standard_error),
    };
    auto inherited_handles = std::array{
        child_handles[0].get(), child_handles[1].get(), child_handles[2].get(),
    };
    const auto attribute_count = job == nullptr ? 1u : 2u;
    auto attribute_bytes = SIZE_T{};
    InitializeProcThreadAttributeList(
        nullptr, attribute_count, 0, &attribute_bytes);
    if ((attribute_bytes == 0) || (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
        WinError("could not size the process attribute list");
    }
    // Process-heap allocations are 16-byte aligned on the required x64 target.
    auto attribute_storage = wil::unique_process_heap(
        HeapAlloc(GetProcessHeap(), 0, attribute_bytes));
    if (!attribute_storage) {
        WinError("could not allocate the process attribute list",
            ERROR_NOT_ENOUGH_MEMORY);
    }
    auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.get());
    if (!InitializeProcThreadAttributeList(
            attributes, attribute_count, 0, &attribute_bytes)) {
        WinError("could not initialize the process attribute list");
    }
    auto jobs = std::array{job};
    const auto delete_attributes = wil::scope_exit(
        [=] { DeleteProcThreadAttributeList(attributes); });
    if (!UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(), sizeof(inherited_handles), nullptr, nullptr)) {
        WinError("could not restrict inherited process handles");
    }

    if ((job != nullptr) &&
        !UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs.data(),
            sizeof(jobs), nullptr, nullptr)) {
        WinError("could not assign the child process to its job");
    }

    auto startup = STARTUPINFOEXW{
        .StartupInfo = {
            .cb = sizeof(STARTUPINFOEXW),
            .dwFlags = STARTF_USESTDHANDLES | additional_startup_flags,
            .wShowWindow = SW_HIDE,
            .hStdInput = inherited_handles[0],
            .hStdOutput = inherited_handles[1],
            .hStdError = inherited_handles[2],
        },
        .lpAttributeList = attributes,
    };
    auto process = wil::unique_process_information{};
    if (!start(&startup.StartupInfo, &process)) {
        WinError(operation);
    }
    return process;
}

[[nodiscard]] auto StartOrchestrator(
    const std::filesystem::path &directory,
    Log &log) {
    auto console = LoggingConsole(log);
    auto job = CreateChildJob();
    const auto script = directory / kOrchestratorName;
    const auto arguments = std::to_array<std::wstring_view>({
        kPowerShellPath,
        L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-File",
        script.native(),
    });
    auto command = wil::ArgvToCommandLine(arguments);
    auto process = console.StartProcess(
        job.get(), kPowerShellPath, command, directory);
    return BackupProcess{
        .console = std::move(console),
        .process = std::move(process),
        .job = std::move(job),
    };
}

struct ServiceContext {
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    wil::unique_event_nothrow cancellation_event;
};

auto SetServiceState(
    const ServiceContext &context,
    const DWORD state,
    const DWORD win32_error = ERROR_SUCCESS,
    const DWORD service_error = 0,
    const DWORD checkpoint = 0,
    const DWORD wait_hint = 0) noexcept {
    auto status = SERVICE_STATUS{
        .dwServiceType = SERVICE_WIN32_OWN_PROCESS,
        .dwCurrentState = state,
        .dwControlsAccepted = state == SERVICE_RUNNING
            ? kAcceptedControls
            : 0,
        .dwWin32ExitCode = win32_error,
        .dwServiceSpecificExitCode = service_error,
        .dwCheckPoint = checkpoint,
        .dwWaitHint = wait_hint,
    };
    return SetServiceStatus(context.status_handle, &status) != FALSE;
}

auto WINAPI ServiceControlHandler(
    const DWORD control, DWORD, void *, void *const raw_context) noexcept -> DWORD {
    auto &context = *static_cast<ServiceContext *>(raw_context);
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
    const std::filesystem::path &directory,
    Log &log) {
    if (!SetEnvironmentVariableW(
            kCancellationEventEnvironment.c_str(),
            kCancellationEventName.c_str())) {
        WinError("could not publish the cancellation event");
    }

    auto backup = StartOrchestrator(directory, log);
    log.Write("backup-supervisor: backup starting");
    if (!SetServiceState(context, SERVICE_RUNNING)) {
        WinError("could not report that the backup service is running");
    }
    auto report_stopping = wil::scope_exit([&] {
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, 1, kStopWaitHintMilliseconds);
    });
    if (ResumeThread(backup.process.hThread) == MAXDWORD) {
        WinError("could not resume the backup orchestrator");
    }

    const auto waits = std::array{
        backup.process.hProcess, context.cancellation_event.get(),
    };
    const auto wait = WaitForMultipleObjects(
        DWORD{waits.size()}, waits.data(), FALSE, INFINITE);
    if (wait == WAIT_FAILED) {
        WinError("could not wait for the backup orchestrator");
    }
    SetServiceState(context, SERVICE_STOP_PENDING,
        ERROR_SUCCESS, 0, 1, kStopWaitHintMilliseconds);
    report_stopping.release();

    const auto cancelled = context.cancellation_event.is_signaled();
    auto forced = false;
    if (cancelled && !backup.Wait(kGracefulStopMilliseconds)) {
        log.Write(
            "backup-supervisor: graceful stop timed out; terminating the process job");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, 2, kForcedProcessWaitMilliseconds);
        backup.TerminateAndWait(ERROR_CANCELLED);
        forced = true;
    }

    const auto child_exit_code = backup.ExitCode();
    if (!backup.WaitForAll(kStrayProcessWaitMilliseconds)) {
        log.Write(
            "backup-supervisor: terminating child processes left behind by the orchestrator");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, 2, kForcedProcessWaitMilliseconds);
        backup.TerminateAndWait(ERROR_PROCESS_ABORTED);
        forced = true;
    }
    backup.console.Finish();

    if (forced) {
        log.Write(
            "backup-supervisor: backup failed after forced process termination; "
            "orchestrator exit code {}",
            child_exit_code);
    } else if ((child_exit_code == 0) && cancelled) {
        log.Write(
            "backup-supervisor: backup completed before cancellation took effect");
    } else if (child_exit_code == 0) {
        log.Write("backup-supervisor: backup completed successfully");
    } else if (cancelled) {
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
    if (cancelled || (child_exit_code == 0)) {
        return ServiceOutcome{};
    }
    return ServiceOutcome{
        .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
        .service_error = child_exit_code,
    };
}

auto ConfigurePreshutdownTimeout(const SC_HANDLE service) {
    auto configuration = SERVICE_PRESHUTDOWN_INFO{
        .dwPreshutdownTimeout = kMinimumPreshutdownMilliseconds,
    };
    if (!ChangeServiceConfig2W(service,
            SERVICE_CONFIG_PRESHUTDOWN_INFO, &configuration)) {
        WinError("could not configure the backup service preshutdown timeout");
    }
}

enum class InstallMode {
    CreateOnly,
    CreateOrUpdate,
};

constexpr auto kNoDependencies = std::array{L'\0', L'\0'};

auto InstallService(const InstallMode mode) {
    const auto executable = ExecutablePath();
    const auto binary_path = wil::ArgvToCommandLine(std::array{
        std::wstring_view(executable), kRunServiceOption,
    });
    auto manager = wil::unique_schandle(OpenSCManagerW(
        nullptr, nullptr,
        SC_MANAGER_CONNECT | SC_MANAGER_CREATE_SERVICE));
    if (!manager) {
        WinError("could not open the Service Control Manager");
    }
    auto service = wil::unique_schandle(CreateServiceW(
        manager.get(), kServiceName.c_str(), kServiceDisplayName.c_str(),
        SERVICE_CHANGE_CONFIG | DELETE,
        SERVICE_WIN32_OWN_PROCESS, SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL,
        binary_path.c_str(), nullptr, nullptr, nullptr,
        kLocalSystemAccount.c_str(), L""));
    if (service) {
        auto remove_incomplete_service = wil::scope_exit([&] {
            DeleteService(service.get());
        });
        ConfigurePreshutdownTimeout(service.get());
        remove_incomplete_service.release();
        std::fwprintf(stdout, L"backup-supervisor: installed %ls\n",
            kServiceName.c_str());
        return;
    }

    const auto error = GetLastError();
    if ((mode != InstallMode::CreateOrUpdate) ||
        (error != ERROR_SERVICE_EXISTS)) {
        WinError("could not install the backup service", error);
    }
    service.reset(OpenServiceW(manager.get(),
        kServiceName.c_str(), SERVICE_CHANGE_CONFIG));
    if (!service) {
        WinError("could not open the existing backup service");
    }
    ConfigurePreshutdownTimeout(service.get());
    if (!ChangeServiceConfigW(service.get(),
            SERVICE_WIN32_OWN_PROCESS,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            binary_path.c_str(), L"", nullptr, kNoDependencies.data(),
            kLocalSystemAccount.c_str(), L"",
            kServiceDisplayName.c_str())) {
        WinError("could not update the backup service");
    }
    std::fwprintf(stdout, L"backup-supervisor: updated %ls\n",
        kServiceName.c_str());
}

auto TryWriteFailure(
    Log *const log, const std::string_view diagnostic) noexcept {
    if (log != nullptr) {
        log->TryWrite("backup-supervisor: {}", diagnostic);
    }
}

[[nodiscard]] auto RunService(
    ServiceContext &context, const DWORD argc) noexcept {
    auto result = ServiceOutcome{};
    auto log = std::optional<Log>{};
    try {
        if (argc != 1) {
            throw std::system_error(
                ERROR_INVALID_PARAMETER, std::system_category(),
                "service start arguments are not supported");
        }
        const auto installation = InstallationDirectory();
        log.emplace(installation / kLogDirectory);
        const auto directory = ExtractEmbeddedArtifacts();
        const auto remove_artifacts = wil::scope_exit([&] {
            auto ignored = std::error_code{};
            std::filesystem::remove_all(directory, ignored);
        });
        PublishPersistentPaths(installation);
        context.cancellation_event = CreateCancellationEvent();
        result = RunBackup(context, directory, *log);
    } catch (const std::system_error &error) {
        result.win32_error = std::bit_cast<DWORD>(error.code().value());
        TryWriteFailure(log ? &*log : nullptr, error.what());
    } catch (const std::runtime_error &error) {
        result = {
            .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
            .service_error = kInternalFailure,
        };
        TryWriteFailure(log ? &*log : nullptr, error.what());
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

[[nodiscard]] auto RunForeground(const bool no_writers) {
    auto console_mode = DWORD{};
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &console_mode)) {
        throw std::runtime_error(
            "--foreground requires an attached console");
    }
    if (!SetEnvironmentVariableW(
            kCancellationEventEnvironment.c_str(), nullptr)) {
        WinError("could not clear the supervisor cancellation event");
    }

    const auto installation = InstallationDirectory();
    PublishPersistentPaths(installation);
    const auto directory = ExtractEmbeddedArtifacts();
    const auto remove_artifacts = wil::scope_exit([&] {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(directory, ignored);
    });

    const auto script = directory / kOrchestratorName;
    auto arguments = std::vector<std::wstring_view>{
        kPowerShellPath,
        L"-NoLogo", L"-NoProfile", L"-File", script.native(),
    };
    if (no_writers) {
        arguments.push_back(L"-NoWriters");
    }
    auto command = wil::ArgvToCommandLine(arguments);
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        nullptr,
        0,
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessW(kPowerShellPath.c_str(), command.data(),
                nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT,
                nullptr, directory.c_str(), startup, result);
        },
        "could not start the foreground backup");
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        WinError("could not wait for the foreground backup");
    }
    auto exit_code = DWORD{};
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        WinError("could not obtain the foreground backup exit code");
    }
    return std::bit_cast<int>(exit_code);
}

auto WINAPI ServiceMain(
    const DWORD argc, wchar_t **) noexcept -> void {
    static auto context = ServiceContext{};
    context.status_handle = RegisterServiceCtrlHandlerExW(
        kServiceName.c_str(), ServiceControlHandler, &context);
    if (context.status_handle == nullptr) {
        return;
    }
    SetServiceState(context, SERVICE_START_PENDING,
        ERROR_SUCCESS, 0, 1, 30000);
    const auto result = RunService(context, argc);
    SetServiceState(context, SERVICE_STOPPED,
        result.win32_error, result.service_error);
}

[[nodiscard]] auto ReadPbsVssPassword(
    const std::filesystem::path &password_path) {
    auto file = std::ifstream(password_path, std::ios::binary);
    if (!file.is_open()) {
        throw std::runtime_error("could not open the pbs-vss password file");
    }
    auto password = std::string(
        std::istreambuf_iterator<char>{file}, {});
    while (!password.empty() &&
        ((password.back() == '\r') || (password.back() == '\n'))) {
        password.pop_back();
    }
    return std::filesystem::path(
        std::u8string(password.begin(), password.end())).native();
}

[[nodiscard]] auto LogOnPbsVss(
    const wil::zwstring_view password) {
    auto token = wil::unique_handle{};
    if (!LogonUserW(kPbsUser.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT,
            token.addressof())) {
        WinError("could not log on pbs-vss");
    }
    return token;
}

auto EnableProfilePrivileges(const HANDLE token) {
    constexpr auto privilege_names = std::array{
        wil::zwstring_view(SE_BACKUP_NAME),
        wil::zwstring_view(SE_RESTORE_NAME),
    };
    constexpr auto storage_size =
        offsetof(TOKEN_PRIVILEGES, Privileges) +
        (privilege_names.size() * sizeof(LUID_AND_ATTRIBUTES));
    alignas(TOKEN_PRIVILEGES)
    auto storage = std::array<std::byte, storage_size>{};
    auto entries = std::span{
        std::start_lifetime_as_array<LUID_AND_ATTRIBUTES>(
            storage.data() + offsetof(TOKEN_PRIVILEGES, Privileges),
            privilege_names.size()),
        privilege_names.size(),
    };
    for (auto index = 0uz; index < privilege_names.size(); ++index) {
        auto &entry = entries[index];
        if (!LookupPrivilegeValueW(
                nullptr, privilege_names[index].c_str(), &entry.Luid)) {
            WinError("could not identify a user-profile privilege");
        }
        entry.Attributes = SE_PRIVILEGE_ENABLED;
    }
    auto *const privileges =
        std::start_lifetime_as<TOKEN_PRIVILEGES>(storage.data());
    privileges->PrivilegeCount = DWORD{privilege_names.size()};
    if (!AdjustTokenPrivileges(
            token, FALSE, privileges,
            0, nullptr, nullptr)) {
        WinError("could not enable the user-profile privileges");
    }
    const auto error = GetLastError();
    if (error != ERROR_SUCCESS) {
        WinError("could not enable the user-profile privileges", error);
    }
}

[[nodiscard]] auto RunningAsLocalSystem() {
    auto result = false;
    const auto error = wil::test_token_membership_nothrow(
        &result, nullptr, SECURITY_NT_AUTHORITY, SECURITY_LOCAL_SYSTEM_RID);
    if (FAILED(error)) {
        WinError("could not identify the backup-supervisor account",
            HRESULT_CODE(error));
    }
    return result;
}

[[nodiscard]] auto StartWslWithLogon(
    const wil::zwstring_view password,
    std::wstring &command,
    const std::filesystem::path &working_directory) {
    auto child_handles = std::array{
        DuplicateInheritableHandle(GetStdHandle(STD_INPUT_HANDLE)),
        DuplicateInheritableHandle(GetStdHandle(STD_OUTPUT_HANDLE)),
        DuplicateInheritableHandle(GetStdHandle(STD_ERROR_HANDLE)),
    };
    auto startup = STARTUPINFOW{
        .cb = sizeof(STARTUPINFOW),
        .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
        .wShowWindow = SW_HIDE,
        .hStdInput = child_handles[0].get(),
        .hStdOutput = child_handles[1].get(),
        .hStdError = child_handles[2].get(),
    };
    auto process = wil::unique_process_information{};
    // This API copies STARTF_USESTDHANDLES itself and, unlike the service
    // launch path below, is available only when the caller is not LocalSystem.
    // Manual backups create no supervisor job, so do not request breakaway.
    if (!CreateProcessWithLogonW(
            kPbsUser.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON_WITH_PROFILE, kWslPath.c_str(), command.data(),
            kWslCreationFlags,
            nullptr, working_directory.c_str(), &startup, &process)) {
        WinError("could not start WSL as pbs-vss");
    }
    return process;
}

[[nodiscard]] auto WaitForWsl(
    const wil::unique_process_information &process) {
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        WinError("could not wait for WSL");
    }
    auto exit_code = DWORD{};
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        WinError("could not obtain the WSL exit code");
    }
    return std::bit_cast<int>(exit_code);
}

[[nodiscard]] auto RunWslAsPbsVss(
    const std::span<const wchar_t *const> arguments) {
    if (arguments.empty()) {
        throw std::invalid_argument(
            "--run-wsl-as-pbs-vss requires at least one WSL argument");
    }
    const auto directory = InstallationDirectory();
    const auto password =
        ReadPbsVssPassword(directory / kPasswordRelativePath);
    auto argument_views = std::vector{kWslPath};
    argument_views.append_range(arguments);
    auto command = wil::ArgvToCommandLine(argument_views);
    const auto wsl_directory =
        std::filesystem::path(kWslPath.c_str()).parent_path();
    if (!RunningAsLocalSystem()) {
        return WaitForWsl(
            StartWslWithLogon(password, command, wsl_directory));
    }

    auto pbs_token = LogOnPbsVss(password);

    auto helper_token = wil::unique_handle{};
    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES,
            helper_token.addressof())) {
        WinError("could not open the backup-supervisor process token");
    }
    // LoadUserProfileW requires these privileges on its LocalSystem caller.
    // They are not added to the pbs-vss token used to start WSL.
    EnableProfilePrivileges(helper_token.get());

    [[gsl::suppress("type.3",
        justification: "LoadUserProfileW retains a mutable historical parameter for an input-only username.")]]
    auto *const profile_user = const_cast<PWSTR>(kPbsUser.c_str());
    auto profile = PROFILEINFOW{
        .dwSize = sizeof(PROFILEINFOW),
        .dwFlags = PI_NOUI,
        .lpUserName = profile_user,
    };
    if (!LoadUserProfileW(pbs_token.get(), &profile)) {
        WinError("could not load the pbs-vss profile");
    }
    const auto unload_profile = [pbs_token = pbs_token.get()](
        void *const profile_handle) noexcept {
        static_cast<void>(UnloadUserProfile(pbs_token, profile_handle));
    };
    auto loaded_profile = std::unique_ptr<void, decltype(unload_profile)>(
        profile.hProfile, unload_profile);

    auto environment = wil::unique_environment_block{};
    if (!CreateEnvironmentBlock(
            environment.addressof(), pbs_token.get(), FALSE)) {
        WinError("could not create the pbs-vss environment");
    }

    // CreateProcessWithTokenW has no bInheritHandles parameter, so it cannot
    // satisfy PROC_THREAD_ATTRIBUTE_HANDLE_LIST's documented requirements.
    // WSL manages its own infrastructure, so exclude it from the backup job.
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        nullptr,
        STARTF_USESHOWWINDOW,
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessAsUserW(pbs_token.get(), kWslPath.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                kWslCreationFlags | CREATE_BREAKAWAY_FROM_JOB |
                    EXTENDED_STARTUPINFO_PRESENT,
                environment.get(), wsl_directory.c_str(), startup, result);
        },
        "could not start WSL as pbs-vss");
    return WaitForWsl(process);
}

auto RunServiceDispatcher() {
    [[gsl::suppress("type.3",
        justification: "The SCM retains a mutable historical parameter for an input-only service name.")]]
    auto *const service_name = const_cast<PWSTR>(kServiceName.c_str());
    auto entries = std::array{
        SERVICE_TABLE_ENTRYW{
            .lpServiceName = service_name,
            .lpServiceProc = ServiceMain,
        },
        SERVICE_TABLE_ENTRYW{},
    };
    if (!StartServiceCtrlDispatcherW(entries.data())) {
        WinError("could not connect to the Service Control Manager");
    }
    return 0;
}

auto PrintHelp() {
    std::fputws(
        L"Usage:\n"
        L"  backup-supervisor.exe --devicefs [devicefs arguments]\n"
        L"  backup-supervisor.exe --foreground [--no-writers]\n"
        L"  backup-supervisor.exe --install-service [--update]\n"
        L"  backup-supervisor.exe --run-service\n",
        stdout);
}

} // namespace

auto wmain(const int argc, wchar_t **const argv) -> int {
    try {
        HardenProcess();
        const auto arguments =
            std::span<const wchar_t *const>{argv + 1, argv + argc};
        if (!arguments.empty() &&
            (std::wstring_view(arguments.front()) == kDeviceFsOption)) {
            return devicefs::Main(arguments);
        }
        if (arguments.empty()) {
            PrintHelp();
            return 0;
        }
        const auto option = std::wstring_view(arguments.front());
        if (option == kRunServiceOption) {
            if (arguments.size() != 1) {
                throw std::invalid_argument(
                    "--run-service does not accept arguments");
            }
            return RunServiceDispatcher();
        }
        if (option == kInstallOption) {
            if (arguments.size() == 1) {
                InstallService(InstallMode::CreateOnly);
                return 0;
            }
            if ((arguments.size() == 2) &&
                (std::wstring_view(arguments[1]) == kUpdateOption)) {
                InstallService(InstallMode::CreateOrUpdate);
                return 0;
            }
            throw std::invalid_argument(
                "--install-service accepts only the optional --update argument");
        }
        if (option == kForegroundOption) {
            if (arguments.size() == 1) {
                return RunForeground(false);
            }
            if ((arguments.size() == 2) &&
                (std::wstring_view(arguments[1]) == kNoWritersOption)) {
                return RunForeground(true);
            }
            throw std::invalid_argument(
                "--foreground accepts only the optional --no-writers argument");
        }
        if (option == kHelperOption) {
            return RunWslAsPbsVss(arguments.subspan(1));
        }
        throw std::invalid_argument("unknown backup-supervisor argument");
    } catch (const std::invalid_argument &error) {
        std::fwprintf(stderr, L"backup-supervisor: %hs\n", error.what());
        return 2;
    } catch (const std::runtime_error &error) {
        std::fwprintf(stderr, L"backup-supervisor: %hs\n", error.what());
        return 1;
    }
}
