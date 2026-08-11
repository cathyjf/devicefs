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
#include <tlhelp32.h>
#include <userenv.h>
#include <winternl.h>

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
#include <format>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
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
constexpr auto kHelperOption =
    std::wstring_view(L"--run-wsl-as-pbs-vss");
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
constexpr auto kLogRelativePath =
    std::wstring_view(L"logs\\backup-supervisor.log");
constexpr auto kPasswordRelativePath =
    std::wstring_view(L"credentials\\pbs-vss.password");
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
[[gsl::suppress("type.1",
    justification: "ProcessCommandLineInformation is NT process information class 60.")]]
constexpr auto kProcessCommandLineInformation =
    static_cast<PROCESSINFOCLASS>(60);

[[noreturn]] auto WinError(
    const wil::zstring_view operation, const DWORD error = GetLastError()) {
    throw std::system_error(
        std::bit_cast<int>(error), std::system_category(), operation.c_str());
}

auto HardenProcess() {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        WinError("could not restrict DLL search directories");
    }

    auto dynamic_code = PROCESS_MITIGATION_DYNAMIC_CODE_POLICY{};
    dynamic_code.ProhibitDynamicCode = 1;
    if (!SetProcessMitigationPolicy(
            ProcessDynamicCodePolicy, &dynamic_code, sizeof(dynamic_code))) {
        WinError("could not prohibit dynamic code");
    }

    auto strict_handles = PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY{};
    strict_handles.RaiseExceptionOnInvalidHandleReference = 1;
    strict_handles.HandleExceptionsPermanentlyEnabled = 1;
    if (!SetProcessMitigationPolicy(
            ProcessStrictHandleCheckPolicy, &strict_handles, sizeof(strict_handles))) {
        WinError("could not enable strict handle checking");
    }

    auto extension_points = PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY{};
    extension_points.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy,
            &extension_points, sizeof(extension_points))) {
        WinError("could not disable legacy extension points");
    }

    auto image_load = PROCESS_MITIGATION_IMAGE_LOAD_POLICY{};
    image_load.NoRemoteImages = 1;
    image_load.NoLowMandatoryLabelImages = 1;
    if (!SetProcessMitigationPolicy(
            ProcessImageLoadPolicy, &image_load, sizeof(image_load))) {
        WinError("could not restrict image loading");
    }
}

[[nodiscard]] auto ExecutablePath() {
    auto result = std::wstring{};
    const auto error = wil::GetModuleFileNameW(nullptr, result);
    if (FAILED(error)) {
        WinError("could not obtain the backup supervisor path",
            HRESULT_CODE(error));
    }
    return result;
}

[[nodiscard]] auto OrchestrationDirectory() {
    return std::filesystem::path(ExecutablePath()).parent_path().parent_path();
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
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation,
            &limits, sizeof(limits))) {
        WinError("could not configure the backup process job");
    }
    return job;
}

struct BackupProcess {
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
            .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
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

template <typename... Arguments>
auto WriteLog(const HANDLE log,
    const std::format_string<Arguments...> format,
    Arguments&&... arguments) {
    auto message = std::format(
        format, std::forward<Arguments>(arguments)...);
    message.append("\r\n");
    auto written = DWORD{};
    [[gsl::suppress("type.1",
        justification: "Service diagnostics are far smaller than the DWORD WriteFile limit.")]]
    const auto size = static_cast<DWORD>(message.size());
    if (!WriteFile(log, message.data(), size, &written, nullptr)) {
        WinError("could not write the backup supervisor log");
    }
    if (written != size) {
        throw std::runtime_error(
            "the backup supervisor log write was incomplete");
    }
}

template <typename... Arguments>
auto TryWriteLog(const HANDLE log,
    const std::format_string<Arguments...> format,
    Arguments&&... arguments) noexcept {
    try {
        WriteLog(log, format, std::forward<Arguments>(arguments)...);
    } catch (...) {}
}

[[nodiscard]] auto Utf8(const std::wstring_view value) {
    const auto encoded = std::filesystem::path(value).u8string();
    return std::string(encoded.begin(), encoded.end());
}

[[nodiscard]] auto ProcessCommandLine(const HANDLE process) {
    auto bytes = ULONG{};
    static_cast<void>(NtQueryInformationProcess(
        process, kProcessCommandLineInformation,
        nullptr, 0, &bytes));
    if (bytes < sizeof(UNICODE_STRING)) {
        return std::wstring{};
    }
    auto storage = std::vector<std::byte>(bytes);
    if (NtQueryInformationProcess(process, kProcessCommandLineInformation,
            storage.data(), bytes, &bytes) < 0) {
        return std::wstring{};
    }
    const auto *const command_line =
        std::start_lifetime_as<UNICODE_STRING>(storage.data());
    if (command_line->Buffer == nullptr) {
        return std::wstring{};
    }
    return std::wstring(command_line->Buffer,
        command_line->Length / sizeof(wchar_t));
}

auto LogJobProcesses(const HANDLE log, const HANDLE job) noexcept {
    try {
        auto snapshot = wil::unique_tool_help_snapshot(
            CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (!snapshot) {
            TryWriteLog(log,
                "backup-supervisor: could not enumerate processes before "
                "terminating the backup job: error {}", GetLastError());
            return;
        }

        auto entry = PROCESSENTRY32W{.dwSize = sizeof(PROCESSENTRY32W)};
        if (!Process32FirstW(snapshot.get(), &entry)) {
            TryWriteLog(log,
                "backup-supervisor: could not read the process snapshot: "
                "error {}", GetLastError());
            return;
        }
        do {
            auto process = wil::unique_process_handle(OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process) {
                continue;
            }
            auto in_job = FALSE;
            if (!IsProcessInJob(process.get(), job, &in_job) || !in_job) {
                continue;
            }

            const auto command_line = ProcessCommandLine(process.get());
            WriteLog(log,
                "backup-supervisor: terminating '{}' "
                "(PID {}, command line '{}')",
                Utf8(entry.szExeFile), entry.th32ProcessID,
                command_line.empty() ? "unavailable" : Utf8(command_line));
        } while (Process32NextW(snapshot.get(), &entry));
    } catch (const std::exception &error) {
        TryWriteLog(log,
            "backup-supervisor: could not identify processes before "
            "terminating the backup job: {}", error.what());
    }
}

[[nodiscard]] auto StartOrchestrator(
    const std::filesystem::path &directory,
    const HANDLE log) {
    auto job = CreateChildJob();
    const auto script = directory / kOrchestratorName;
    const auto arguments = std::to_array<std::wstring_view>({
        kPowerShellPath,
        L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-File",
        script.native(),
    });
    auto command = wil::ArgvToCommandLine(arguments);
    auto null_input = wil::unique_hfile(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!null_input) {
        WinError("could not open the null input device");
    }
    auto process = StartProcessWithHandles(
        null_input.get(), log, log, job.get(),
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessW(kPowerShellPath.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW |
                    EXTENDED_STARTUPINFO_PRESENT,
                nullptr, directory.c_str(), startup, result);
        },
        "could not start the backup orchestrator");
    return BackupProcess{
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
    const HANDLE log) {
    if (!SetEnvironmentVariableW(
            kCancellationEventEnvironment.c_str(),
            kCancellationEventName.c_str())) {
        WinError("could not publish the cancellation event");
    }

    auto backup = StartOrchestrator(directory, log);
    WriteLog(log, "backup-supervisor: backup starting");
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
        WriteLog(log,
            "backup-supervisor: graceful stop timed out; terminating the process job");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, 2, kForcedProcessWaitMilliseconds);
        backup.TerminateAndWait(ERROR_CANCELLED);
        forced = true;
    }

    const auto child_exit_code = backup.ExitCode();
    if (!backup.WaitForAll(kStrayProcessWaitMilliseconds)) {
        WriteLog(log,
            "backup-supervisor: terminating child processes left behind by the orchestrator");
        LogJobProcesses(log, backup.job.get());
        SetServiceState(context, SERVICE_STOP_PENDING,
            ERROR_SUCCESS, 0, 2, kForcedProcessWaitMilliseconds);
        backup.TerminateAndWait(ERROR_PROCESS_ABORTED);
        forced = true;
    }

    if (forced) {
        WriteLog(log,
            "backup-supervisor: backup failed after forced process termination; "
            "orchestrator exit code {}",
            child_exit_code);
    } else if ((child_exit_code == 0) && cancelled) {
        WriteLog(log,
            "backup-supervisor: backup completed before cancellation took effect");
    } else if (child_exit_code == 0) {
        WriteLog(log, "backup-supervisor: backup completed successfully");
    } else if (cancelled) {
        WriteLog(log,
            "backup-supervisor: backup cancelled; orchestrator exit code {}",
            child_exit_code);
    } else {
        WriteLog(log,
            "backup-supervisor: backup failed; orchestrator exit code {}",
            child_exit_code);
    }
    if (!FlushFileBuffers(log)) {
        WinError("could not flush the backup supervisor log");
    }
    if (forced) {
        return ServiceOutcome{.win32_error = ERROR_TIMEOUT};
    }
    if (child_exit_code == 0) {
        return ServiceOutcome{};
    }
    if (cancelled) {
        return ServiceOutcome{.win32_error = ERROR_CANCELLED};
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
    const auto binary_path = wil::ArgvToCommandLine(
        std::array{std::wstring_view(executable)});
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
    const wil::unique_hfile &log, const std::string_view diagnostic) noexcept {
    if (log) {
        TryWriteLog(log.get(), "backup-supervisor: {}", diagnostic);
    }
}

[[nodiscard]] auto RunService(
    ServiceContext &context, const DWORD argc) noexcept {
    auto result = ServiceOutcome{};
    auto log = wil::unique_hfile{};
    try {
        if (argc != 1) {
            throw std::system_error(
                ERROR_INVALID_PARAMETER, std::system_category(),
                "service start arguments are not supported");
        }
        const auto directory = OrchestrationDirectory();
        const auto log_path = directory / kLogRelativePath;
        std::filesystem::create_directory(log_path.parent_path());
        log.reset(CreateFileW(log_path.c_str(),
            GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!log) {
            WinError("could not open the backup supervisor log");
        }
        context.cancellation_event = CreateCancellationEvent();
        result = RunBackup(context, directory, log.get());
    } catch (const std::system_error &error) {
        result.win32_error = std::bit_cast<DWORD>(error.code().value());
        TryWriteFailure(log, error.what());
    } catch (const std::runtime_error &error) {
        result = {
            .win32_error = ERROR_SERVICE_SPECIFIC_ERROR,
            .service_error = kInternalFailure,
        };
        TryWriteFailure(log, error.what());
    } catch (...) {
        result = {
            .win32_error = ERROR_UNHANDLED_EXCEPTION,
        };
    }

    if (log) {
        FlushFileBuffers(log.get());
    }
    return result;
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
    if (!CreateProcessWithLogonW(
            kPbsUser.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON_WITH_PROFILE, kWslPath.c_str(), command.data(),
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
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
    const auto directory = OrchestrationDirectory();
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
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        nullptr,
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessAsUserW(pbs_token.get(), kWslPath.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT |
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

} // namespace

auto wmain(const int argc, wchar_t **const argv) -> int {
    try {
        HardenProcess();
        const auto arguments =
            std::span<const wchar_t *const>{argv + 1, argv + argc};
        if (arguments.empty()) {
            return RunServiceDispatcher();
        }
        const auto option = std::wstring_view(arguments.front());
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
