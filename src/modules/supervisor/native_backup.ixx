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

#include <windows.h>
#include <userenv.h>

#include <wil/win32_helpers.h>

#include <cstddef>
#include <cstdio>

export module devicefs.supervisor.native_backup;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import <wil/stl.h>;
import <wil/token_helpers.h>;
import devicefs.common;
import devicefs.supervisor.configuration;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose these C++23 functions.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
template <class T> auto start_lifetime_as_array(void *, size_t) noexcept -> T *;
} // namespace std
#endif

export constexpr auto kCancelledExitCode = 130;

export [[nodiscard]] auto RunDeviceToFifo(
    HANDLE cancellation_event,
    std::wstring_view device,
    std::wstring_view fifo_path) -> int;

namespace internal {

using namespace std::chrono_literals;

[[nodiscard]] auto SnapshotImageName(
    const devicefs::vshadow::Snapshot &) -> std::wstring;

[[nodiscard]] auto SerializeSnapshotManifest(
    const devicefs::vshadow::SnapshotSet &) -> std::u8string;

[[nodiscard]] auto WaitForProcess(
    const HANDLE process,
    const std::chrono::milliseconds timeout) -> bool {
    const auto result = WaitForSingleObject(
        process, wil::safe_cast<DWORD>(timeout.count()));
    if (result == WAIT_FAILED) {
        WinError("could not wait for a backup process");
    }
    return result == WAIT_OBJECT_0;
}

[[nodiscard]] auto ProcessExitCode(const HANDLE process) {
    auto result = DWORD{};
    if (!GetExitCodeProcess(process, &result)) {
        WinError("could not obtain a backup process exit code");
    }
    return result;
}

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
    const Start &start,
    const wil::zstring_view operation) {
    const auto child_handles = std::array{
        DuplicateInheritableHandle(standard_input),
        DuplicateInheritableHandle(standard_output),
        DuplicateInheritableHandle(standard_error),
    };
    auto inherited_handles = std::array{
        child_handles[0].get(), child_handles[1].get(), child_handles[2].get(),
    };
    constexpr auto kAttributeCount = DWORD{1};
    auto attribute_bytes = SIZE_T{};
    InitializeProcThreadAttributeList(
        nullptr, kAttributeCount, 0, &attribute_bytes);
    if ((attribute_bytes == 0) || (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
        WinError("could not size the process attribute list");
    }
    const auto attribute_storage = [&] {
        try {
            return std::make_unique_for_overwrite<std::byte[]>(attribute_bytes);
        } catch (const std::bad_alloc &) {
            WinError("could not allocate the process attribute list",
                ExplicitWin32Error{ERROR_NOT_ENOUGH_MEMORY});
        }
    }();
    auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        static_cast<void *>(attribute_storage.get()));
    if (!InitializeProcThreadAttributeList(
            attributes, kAttributeCount, 0, &attribute_bytes)) {
        WinError("could not initialize the process attribute list");
    }
    const auto delete_attributes = wil::scope_exit(
        [=] { DeleteProcThreadAttributeList(attributes); });
    if (!UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            sizeof(inherited_handles), nullptr, nullptr)) {
        WinError("could not restrict inherited process handles");
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
        WinError("{}", operation);
    }
    return process;
}

constexpr auto kLocalDomain = wil::zwstring_view(L".");
constexpr auto kWslCreationFlags = DWORD{
    CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT};

[[nodiscard]] auto LogOnWindowsAccount(
    const wil::zwstring_view username,
    const wil::zwstring_view password) {
    auto token = wil::unique_handle{};
    if (!LogonUserW(username.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON32_LOGON_INTERACTIVE, LOGON32_PROVIDER_DEFAULT,
            token.addressof())) {
        WinError("could not log on the configured WSL account");
    }
    return token;
}

class ProfilePrivilegeEnabler {
    static constexpr auto kPrivilegeNames = std::array{
        wil::zwstring_view(SE_BACKUP_NAME),
        wil::zwstring_view(SE_RESTORE_NAME),
    };
    static constexpr auto kStateSize =
        offsetof(TOKEN_PRIVILEGES, Privileges) +
        (kPrivilegeNames.size() * sizeof(LUID_AND_ATTRIBUTES));

  public:
    explicit ProfilePrivilegeEnabler(const HANDLE process) {
        if (!OpenProcessToken(process,
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                token_.addressof())) {
            WinError("could not open the backup-supervisor process token");
        }

        alignas(TOKEN_PRIVILEGES)
        auto state_storage = std::array<std::byte, kStateSize>{};
        const auto entries = std::span{
            std::start_lifetime_as_array<LUID_AND_ATTRIBUTES>(
                state_storage.data() +
                    offsetof(TOKEN_PRIVILEGES, Privileges),
                kPrivilegeNames.size()),
            kPrivilegeNames.size(),
        };
        for (auto index = 0uz; index < kPrivilegeNames.size(); ++index) {
            auto &entry = entries[index];
            if (!LookupPrivilegeValueW(
                    nullptr, kPrivilegeNames.at(index).c_str(), &entry.Luid)) {
                WinError("could not identify a user-profile privilege");
            }
            entry.Attributes = SE_PRIVILEGE_ENABLED;
        }
        auto *const state =
            std::start_lifetime_as<TOKEN_PRIVILEGES>(state_storage.data());
        state->PrivilegeCount =
            wil::safe_cast<DWORD>(kPrivilegeNames.size());

        static_cast<void>(std::start_lifetime_as_array<LUID_AND_ATTRIBUTES>(
            previous_state_storage_.data() +
                offsetof(TOKEN_PRIVILEGES, Privileges),
            kPrivilegeNames.size()));
        auto *const previous_state = std::start_lifetime_as<TOKEN_PRIVILEGES>(
            previous_state_storage_.data());
        auto previous_state_size = DWORD{};
        if (!AdjustTokenPrivileges(
                token_.get(), FALSE, state,
                DWORD{kStateSize}, previous_state,
                &previous_state_size)) {
            WinError("could not enable the user-profile privileges");
        }
        previous_state_ = previous_state;
        const auto error = GetLastError();
        if (error != ERROR_SUCCESS) {
            static_cast<void>(RestoreNoThrow());
            WinError("could not enable the user-profile privileges",
                ExplicitWin32Error{error});
        }
    }

    ProfilePrivilegeEnabler(const ProfilePrivilegeEnabler &) = delete;
    auto operator=(const ProfilePrivilegeEnabler &)
        -> ProfilePrivilegeEnabler & = delete;
    ProfilePrivilegeEnabler(ProfilePrivilegeEnabler &&) = delete;
    auto operator=(ProfilePrivilegeEnabler &&)
        -> ProfilePrivilegeEnabler & = delete;

    ~ProfilePrivilegeEnabler() {
        static_cast<void>(RestoreNoThrow());
    }

    auto Restore() {
        const auto error = RestoreNoThrow();
        if (error != ERROR_SUCCESS) {
            WinError("could not restore the user-profile privileges",
                ExplicitWin32Error{error});
        }
    }

  private:
    [[nodiscard]] auto RestoreNoThrow() noexcept -> DWORD {
        if (previous_state_ == nullptr) {
            return DWORD{ERROR_SUCCESS};
        }
        if (!AdjustTokenPrivileges(
                token_.get(), FALSE, previous_state_,
                0, nullptr, nullptr)) {
            return GetLastError();
        }
        const auto error = GetLastError();
        if (error == ERROR_SUCCESS) {
            previous_state_ = nullptr;
        }
        return error;
    }

    wil::unique_handle token_;
    alignas(TOKEN_PRIVILEGES)
    std::array<std::byte, kStateSize> previous_state_storage_{};
    TOKEN_PRIVILEGES *previous_state_ = nullptr;
};

[[nodiscard]] auto RunningAsLocalSystem() {
    auto result = false;
    const auto error = wil::test_token_membership_nothrow(
        &result, nullptr, SECURITY_NT_AUTHORITY, SECURITY_LOCAL_SYSTEM_RID);
    if (FAILED(error)) {
        WinError("could not identify the backup-supervisor account",
            ExplicitWin32Error{
                wil::safe_cast<DWORD>(HRESULT_CODE(error))});
    }
    return result;
}

[[nodiscard]] auto StartWslWithLogon(
    const wil::zwstring_view username,
    const wil::zwstring_view password,
    const std::filesystem::path &wsl_path,
    std::wstring &command,
    const std::filesystem::path &working_directory,
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error) {
    const auto child_handles = std::array{
        DuplicateInheritableHandle(standard_input),
        DuplicateInheritableHandle(standard_output),
        DuplicateInheritableHandle(standard_error),
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
            username.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON_WITH_PROFILE, wsl_path.c_str(), command.data(),
            kWslCreationFlags,
            nullptr, working_directory.c_str(), &startup, &process)) {
        WinError("could not start WSL as the configured account");
    }
    return process;
}

struct ProfileCloser {
    HANDLE token = nullptr;

    auto operator()(void *const profile) const noexcept {
        static_cast<void>(UnloadUserProfile(token, profile));
    }
};

using LoadedProfile =
    std::unique_ptr<std::remove_pointer_t<HANDLE>, ProfileCloser>;

[[nodiscard]] auto CopyPipeOutput(
    const HANDLE source, const HANDLE destination) noexcept {
    constexpr auto kBufferSize = DWORD{4096};
    auto buffer = std::array<std::byte, kBufferSize>{};
    while (true) {
        auto read = DWORD{};
        if (!ReadFile(source, buffer.data(), kBufferSize,
                &read, nullptr)) {
            const auto error = GetLastError();
            return error == ERROR_BROKEN_PIPE ? ERROR_SUCCESS : error;
        }
        if (read == 0) {
            return DWORD{ERROR_SUCCESS};
        }
        auto offset = DWORD{};
        while (offset < read) {
            auto written = DWORD{};
            if (!WriteFile(destination, buffer.data() + offset,
                    read - offset, &written, nullptr)) {
                return GetLastError();
            }
            if (written == 0) {
                return DWORD{ERROR_WRITE_FAULT};
            }
            offset += written;
        }
    }
}

class PipeCopy {
  public:
    PipeCopy(wil::unique_handle source, const HANDLE destination) {
        auto task = std::packaged_task<DWORD()>(
            [source = std::move(source), destination]() noexcept {
                return CopyPipeOutput(source.get(), destination);
            });
        result_ = task.get_future();
        worker_ = std::thread(std::move(task));
    }

    PipeCopy(const PipeCopy &) = delete;
    auto operator=(const PipeCopy &) -> PipeCopy & = delete;
    PipeCopy(PipeCopy &&) = default;
    auto operator=(PipeCopy &&) -> PipeCopy & = delete;

    [[gsl::suppress("26447",
        justification: "A still-running pipe copy must be detached when its WSL operation is abandoned.")]]
    ~PipeCopy() {
        if (worker_.joinable()) {
            worker_.detach();
        }
    }

    auto Finish() {
        worker_.join();
        return result_.get();
    }

  private:
    std::future<DWORD> result_;
    std::thread worker_;
};

struct WslProcess {
    wil::unique_handle token;
    LoadedProfile profile;
    wil::unique_process_information process;
    std::optional<PipeCopy> standard_output;
    std::optional<PipeCopy> standard_error;

    WslProcess() noexcept = default;
    WslProcess(const WslProcess &) = delete;
    auto operator=(const WslProcess &) -> WslProcess & = delete;
    WslProcess(WslProcess &&) = default;
    auto operator=(WslProcess &&) -> WslProcess & = delete;

    ~WslProcess() {
        if (profile && (process.hProcess != nullptr) &&
            (WaitForSingleObject(process.hProcess, 0) != WAIT_OBJECT_0)) {
            // UnloadUserProfile must not run until the child has exited. An
            // abandoned WSL process therefore retains the loaded profile and
            // its associated token until this process exits.
            static_cast<void>(profile.release());
            static_cast<void>(token.release());
        }
    }

    auto FinishOutput() {
        auto first_error = DWORD{ERROR_SUCCESS};
        if (standard_output) {
            first_error = standard_output->Finish();
        }
        if (standard_error) {
            const auto error = standard_error->Finish();
            if (first_error == ERROR_SUCCESS) {
                first_error = error;
            }
        }
        if (first_error != ERROR_SUCCESS) {
            WinError("could not copy WSL output",
                ExplicitWin32Error{first_error});
        }
    }
};

[[nodiscard]] auto StartWslAsConfiguredUser(
    const BackupConfiguration &configuration,
    const std::span<const std::wstring_view> arguments,
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error) {
    const auto wsl_path = ProgramFilesDirectory() / L"WSL" / L"wsl.exe";
    auto argument_views = std::vector<std::wstring_view>{wsl_path.native()};
    argument_views.append_range(arguments);
    auto command = wil::ArgvToCommandLine(argument_views);
    const auto wsl_directory = wsl_path.parent_path();
    if (!RunningAsLocalSystem()) {
        auto result = WslProcess{};
        result.process = StartWslWithLogon(
            configuration.windows_username,
            configuration.windows_password,
            wsl_path, command, wsl_directory,
            standard_input, standard_output, standard_error);
        return result;
    }

    auto result = WslProcess{};
    result.token = LogOnWindowsAccount(
        configuration.windows_username,
        configuration.windows_password);

    [[gsl::suppress("type.3",
        justification: "LoadUserProfileW retains a mutable historical parameter for an input-only username.")]]
    auto *const profile_user =
        const_cast<PWSTR>(configuration.windows_username.c_str());
    auto profile = PROFILEINFOW{
        .dwSize = sizeof(PROFILEINFOW),
        .dwFlags = PI_NOUI,
        .lpUserName = profile_user,
    };
    {
        // LoadUserProfileW requires these privileges on its LocalSystem
        // caller. They are not added to the user token used to start WSL.
        auto privileges = ProfilePrivilegeEnabler{GetCurrentProcess()};
        if (!LoadUserProfileW(result.token.get(), &profile)) {
            WinError("could not load the configured WSL account profile");
        }
        result.profile = LoadedProfile{
            profile.hProfile, ProfileCloser{result.token.get()}};
        privileges.Restore();
    }

    auto environment = wil::unique_environment_block{};
    if (!CreateEnvironmentBlock(
            environment.addressof(), result.token.get(), FALSE)) {
        WinError("could not create the configured WSL account environment");
    }

    // CreateProcessWithTokenW has no bInheritHandles parameter, so it cannot
    // satisfy PROC_THREAD_ATTRIBUTE_HANDLE_LIST's documented requirements.
    // WSL manages its own infrastructure, so exclude it from the backup job.
    result.process = StartProcessWithHandles(
        standard_input,
        standard_output,
        standard_error,
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const process) {
            return CreateProcessAsUserW(
                result.token.get(), wsl_path.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                kWslCreationFlags | CREATE_BREAKAWAY_FROM_JOB |
                    EXTENDED_STARTUPINFO_PRESENT,
                environment.get(), wsl_directory.c_str(), startup, process);
        },
        "could not start WSL as the configured account");
    return result;
}

struct StartedWslFish {
    wil::unique_handle standard_input;
    wil::unique_handle standard_output;
    wil::unique_handle standard_error;
    WslProcess process;
};

[[nodiscard]] auto StartWslFishProcess(
    const BackupConfiguration &configuration,
    const std::span<const std::wstring_view> arguments) -> StartedWslFish {
    auto input_read = wil::unique_handle{};
    auto input_write = wil::unique_handle{};
    if (!CreatePipe(input_read.addressof(), input_write.addressof(),
            nullptr, 0)) {
        WinError("could not create the WSL input channel");
    }
    auto output_read = wil::unique_handle{};
    auto output_write = wil::unique_handle{};
    if (!CreatePipe(output_read.addressof(), output_write.addressof(),
            nullptr, 0)) {
        WinError("could not create the WSL output channel");
    }
    auto error_read = wil::unique_handle{};
    auto error_write = wil::unique_handle{};
    if (!CreatePipe(error_read.addressof(), error_write.addressof(),
            nullptr, 0)) {
        WinError("could not create the WSL error channel");
    }

    auto wsl_arguments = std::vector<std::wstring_view>{
        L"--distribution", configuration.wsl_distribution,
    };
    if (configuration.wsl_linux_user) {
        wsl_arguments.push_back(L"--user");
        wsl_arguments.push_back(*configuration.wsl_linux_user);
    }
    wsl_arguments.append_range(std::array<std::wstring_view, 6>{
        L"--exec", L"/usr/bin/fish", L"--no-config", L"-c",
        L"read --null --global DEVICEFS_FISH_PROGRAM; "
        L"and eval $DEVICEFS_FISH_PROGRAM",
        L"--",
    });
    wsl_arguments.append_range(arguments);
    auto process = StartWslAsConfiguredUser(
        configuration,
        wsl_arguments,
        input_read.get(),
        output_write.get(),
        error_write.get());
    input_read.reset();
    output_write.reset();
    error_write.reset();
    return StartedWslFish{
        .standard_input = std::move(input_write),
        .standard_output = std::move(output_read),
        .standard_error = std::move(error_read),
        .process = std::move(process),
    };
}

[[nodiscard]] auto StartWslFish(
    const BackupConfiguration &configuration,
    const std::span<const std::wstring_view> arguments,
    const std::span<const char> program,
    const std::span<const char8_t> standard_input) {
    auto started = StartWslFishProcess(configuration, arguments);
    started.process.standard_output.emplace(
        std::move(started.standard_output), GetStdHandle(STD_OUTPUT_HANDLE));
    started.process.standard_error.emplace(
        std::move(started.standard_error), GetStdHandle(STD_ERROR_HANDLE));

    const auto write_input = [&](const auto input) {
        if (input.empty()) {
            return;
        }
        const auto size = wil::safe_cast<DWORD>(input.size_bytes());
        auto written = DWORD{};
        if (!WriteFile(started.standard_input.get(), input.data(), size,
                &written, nullptr)) {
            WinError("could not write the WSL input");
        }
        if (written != size) {
            WinError("could not write the complete WSL input",
                ExplicitWin32Error{ERROR_WRITE_FAULT});
        }
    };
    constexpr auto kProgramTerminator = std::array{char{0}};
    write_input(program);
    write_input(std::span<const char>{kProgramTerminator});
    write_input(standard_input);
    started.standard_input.reset();
    return std::move(started.process);
}

[[nodiscard]] auto FinishWsl(WslProcess &process) -> int {
    process.FinishOutput();
    return std::bit_cast<int>(ProcessExitCode(process.process.hProcess));
}

enum class WslBackupSignal {
    Term,
    Kill,
};

struct WslBackupSignalText {
    std::wstring_view argument;
    std::string_view diagnostic;
};

[[nodiscard]] constexpr auto SignalText(
    const WslBackupSignal signal) noexcept {
    switch (signal) {
    case WslBackupSignal::Term:
        return WslBackupSignalText{L"TERM", "TERM"};
    case WslBackupSignal::Kill:
        return WslBackupSignalText{L"KILL", "KILL"};
    }
    std::unreachable();
}

auto SendWslBackupSignal(
    const std::wstring_view pid_file,
    const std::wstring_view stop_file,
    const WslBackupSignal signal) {
    constexpr auto kControlTimeout = 15s;
    constexpr auto program = std::string_view(
        "touch $argv[2]; "
        "if test -s $argv[1]; kill -s $argv[3] (cat $argv[1]); end");
    const auto text = SignalText(signal);
    const auto arguments = std::array{
        pid_file, stop_file, text.argument};
    auto request = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return StartWslFish(
            configuration,
            arguments,
            std::span<const char>{program},
            std::span<const char8_t>{});
    }();
    if (!WaitForProcess(
            request.process.hProcess, kControlTimeout)) {
        throw std::runtime_error(std::format(
            "the WSL {} request did not exit before its timeout",
            text.diagnostic));
    }
    const auto exit_code = FinishWsl(request);
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "the WSL {} request exited with code {}",
            text.diagnostic, exit_code));
    }
}

[[gsl::suppress("26447",
    justification: "std::exception::what is noexcept, and fwprintf does not throw C++ exceptions.")]]
auto TryWriteError(
    const char *const context,
    const std::exception &error) noexcept {
    std::fwprintf(stderr, L"backup-supervisor: %hs: %hs\n",
        context, error.what());
}

auto TrySendWslBackupSignal(
    const std::wstring_view pid_file,
    const std::wstring_view stop_file,
    const WslBackupSignal signal) noexcept {
    try {
        SendWslBackupSignal(pid_file, stop_file, signal);
    } catch (const std::exception &error) {
        TryWriteError("could not signal the WSL backup", error);
    }
}

[[nodiscard]] auto UniqueName() {
    auto id = GUID{};
    const auto error = CoCreateGuid(&id);
    if (FAILED(error)) {
        [[gsl::suppress("type.1",
            justification: "HRESULT_CODE returns the Win32-sized error code carried by the HRESULT.")]]
        const auto win32_error =
            static_cast<DWORD>(HRESULT_CODE(error));
        WinError("could not create a unique backup identifier",
            ExplicitWin32Error{win32_error});
    }
    return std::format(
        L"{:08x}{:04x}{:04x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        id.Data1, id.Data2, id.Data3,
        id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
        id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
}

[[nodiscard]] auto RunWslBackup(
    const HANDLE cancellation_event,
    const std::u8string_view snapshot_manifest,
    const std::optional<std::u8string> &namespace_override) {
    constexpr auto kPollInterval = 100ms;
    constexpr auto kTermTimeout = 45s;
    constexpr auto kKillTimeout = 30s;
    const auto control_path = std::format(
        L"/tmp/devicefs-{}", UniqueName());
    const auto pid_file = std::format(L"{}.pid", control_path);
    const auto stop_file = std::format(L"{}.stop", control_path);
    const auto computer_name =
        wil::GetEnvironmentVariableW<std::wstring>(L"COMPUTERNAME");
    auto backup = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        auto arguments = std::vector<std::wstring_view>{
            pid_file, stop_file, computer_name};
        if (configuration.pbs_parallelize_image_upload) {
            arguments.push_back(L"--parallel-images");
        }
        auto input = SecureUtf8String{};
        // start-pbs.fish consumes these NUL-delimited records in order. Add
        // future records here and matching Fish reads before the remaining key
        // document, which proxmox-backup-client reads through fd 0.
        const auto append_record = [&](const std::u8string_view value) {
            input.append(value);
            input.push_back(u8'\0');
        };
        append_record(configuration.wsl_client_path);
        append_record(configuration.pbs_server);
        const auto port = std::to_string(configuration.pbs_port);
        append_record(std::u8string{port.begin(), port.end()});
        append_record(configuration.pbs_datastore);
        append_record(configuration.pbs_auth_id);
        append_record(namespace_override
            ? *namespace_override : configuration.pbs_namespace);
        append_record(configuration.pbs_fingerprint);
        append_record(configuration.pbs_authentication_secret);
        append_record(snapshot_manifest);
        input.append(configuration.pbs_encryption_key);
        return StartWslFish(
            configuration,
            arguments,
            StartPbsProgram(),
            std::span<const char8_t>{input.data(), input.size()});
    }();

    while (true) {
        if (WaitForProcess(
                backup.process.hProcess, kPollInterval)) {
            return FinishWsl(backup);
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            break;
        }
    }

    TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Term);
    if (!WaitForProcess(
            backup.process.hProcess, kTermTimeout)) {
        TrySendWslBackupSignal(pid_file, stop_file, WslBackupSignal::Kill);
        if (!WaitForProcess(
                backup.process.hProcess, kKillTimeout)) {
            throw std::runtime_error(
                "the WSL backup did not exit after the KILL request");
        }
    }
    const auto exit_code = FinishWsl(backup);
    if (exit_code != 0) {
        std::wcout << L"The WSL backup exited with code " << exit_code
                   << L" during cancellation.\n";
    }
    return kCancelledExitCode;
}

struct DeviceFsProcess {
    static constexpr auto kPollInterval = 100ms;
    static constexpr auto kStartTimeout = 30s;
    static constexpr auto kShutdownTimeout = 60s;
    static constexpr auto kMountTarget = std::wstring_view(L"X:");
    static constexpr auto kMountDriveMask =
        DWORD{1} << (kMountTarget.front() - L'A');

    wil::unique_process_information process;
    std::filesystem::path readiness_path;
    std::wstring stop_event_name;
};

[[nodiscard]] auto StartDeviceFs(
    const std::span<const devicefs::vshadow::Snapshot> snapshots,
    const std::wstring_view read_user) {
    const auto supervisor = CurrentExecutablePath();
    auto stop_event_name = std::format(
        L"Global\\devicefs-stop-{}", UniqueName());
    auto arguments = std::vector<std::wstring>{
        supervisor.native(),
        L"--devicefs",
        L"--synthetic-free-clusters", L"--cache",
        L"--mount", std::wstring{DeviceFsProcess::kMountTarget},
        L"--read-user", std::wstring{read_user},
        L"--stop-event", stop_event_name,
    };
    auto readiness_path = std::filesystem::path{};
    for (const auto &snapshot : snapshots) {
        auto filename = SnapshotImageName(snapshot);
        if (readiness_path.empty()) {
            readiness_path = std::filesystem::path(
                std::format(L"{}\\", DeviceFsProcess::kMountTarget)) /
                filename;
        }
        arguments.emplace_back(L"--map");
        arguments.emplace_back(std::move(filename));
        arguments.emplace_back(snapshot.device);
    }
    auto command = wil::ArgvToCommandLine(arguments);
    std::wcout << L"Setting up virtual filesystem: " << command << L'\n';
    auto process = StartProcessWithHandles(
        GetStdHandle(STD_INPUT_HANDLE),
        GetStdHandle(STD_OUTPUT_HANDLE),
        GetStdHandle(STD_ERROR_HANDLE),
        [&](STARTUPINFOW *const startup, PROCESS_INFORMATION *const result) {
            return CreateProcessW(
                supervisor.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr, startup, result);
        },
        "could not start devicefs");
    return DeviceFsProcess{
        .process = std::move(process),
        .readiness_path = std::move(readiness_path),
        .stop_event_name = std::move(stop_event_name),
    };
}

[[nodiscard]] auto WaitForDeviceFs(
    const DeviceFsProcess &devicefs,
    const HANDLE cancellation_event) {
    const auto deadline =
        std::chrono::steady_clock::now() + DeviceFsProcess::kStartTimeout;
    while (true) {
        if (WaitForProcess(devicefs.process.hProcess,
                DeviceFsProcess::kPollInterval)) {
            throw std::runtime_error(std::format(
                "devicefs exited during startup with code {}.",
                ProcessExitCode(devicefs.process.hProcess)));
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            return false;
        }
        auto exists_error = std::error_code{};
        if (std::filesystem::is_regular_file(
                devicefs.readiness_path, exists_error)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "devicefs did not mount X: before the startup timeout elapsed");
        }
    }
}

auto StopDeviceFs(
    const DeviceFsProcess &devicefs,
    const bool backup_succeeded) {
    const auto deadline =
        std::chrono::steady_clock::now() +
        DeviceFsProcess::kShutdownTimeout;
    auto stop_requested = false;
    while (!WaitForProcess(devicefs.process.hProcess, 0ms)) {
        if (!stop_requested) {
            auto stop_event = wil::unique_event_nothrow{};
            if (stop_event.try_open(
                    devicefs.stop_event_name.c_str(),
                    EVENT_MODIFY_STATE)) {
                if (!SetEvent(stop_event.get())) {
                    WinError("could not request devicefs shutdown");
                }
                stop_requested = true;
            } else {
                const auto error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND) {
                    WinError("could not open the devicefs shutdown event",
                        ExplicitWin32Error{error});
                }
            }
        }
        if (WaitForProcess(
                devicefs.process.hProcess,
                DeviceFsProcess::kPollInterval)) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error(
                "devicefs did not exit before the shutdown timeout elapsed");
        }
    }
    if (!backup_succeeded) {
        return;
    }
    const auto exit_code = ProcessExitCode(devicefs.process.hProcess);
    if (!stop_requested) {
        throw std::runtime_error(std::format(
            "devicefs exited before shutdown was requested with code {}.",
            exit_code));
    }
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "devicefs exited with code {}.", exit_code));
    }
}

auto TryStopDeviceFs(const DeviceFsProcess &devicefs) noexcept {
    try {
        StopDeviceFs(devicefs, false);
    } catch (const std::exception &error) {
        TryWriteError("devicefs cleanup failed", error);
    }
}

[[nodiscard]] auto RunSnapshotBackup(
    const HANDLE cancellation_event,
    const devicefs::vshadow::SnapshotSet &snapshot_set,
    const std::wstring_view read_user,
    const std::optional<std::u8string> &namespace_override) {
    const auto cancelled = WaitForSingleObject(cancellation_event, 0);
    if (cancelled == WAIT_FAILED) {
        WinError("could not inspect the backup cancellation event");
    }
    if (cancelled == WAIT_OBJECT_0) {
        return kCancelledExitCode;
    }

    const auto logical_drives = GetLogicalDrives();
    if (logical_drives == 0) {
        WinError("could not enumerate drive letters");
    }
    if ((logical_drives & DeviceFsProcess::kMountDriveMask) != 0) {
        throw std::runtime_error("mount target is already present: X:");
    }

    const auto snapshot_manifest = SerializeSnapshotManifest(snapshot_set);
    const auto devicefs = StartDeviceFs(snapshot_set.snapshots, read_user);
    auto cleanup = wil::scope_exit([&] {
        TryStopDeviceFs(devicefs);
    });

    auto result = kCancelledExitCode;
    if (WaitForDeviceFs(devicefs, cancellation_event)) {
        result = RunWslBackup(
            cancellation_event, snapshot_manifest, namespace_override);
    }

    cleanup.release();
    try {
        StopDeviceFs(devicefs, result == 0);
    } catch (const std::exception &error) {
        if (result == 0) {
            throw;
        }
        TryWriteError("devicefs cleanup failed", error);
    }
    return result;
}

} // namespace internal

export [[nodiscard]] auto RunNativeBackup(
    const HANDLE cancellation_event,
    const bool no_writers,
    const std::span<const std::wstring> volume_override,
    const std::optional<std::u8string> &namespace_override) -> int {
    const auto cancelled = WaitForSingleObject(cancellation_event, 0);
    if (cancelled == WAIT_FAILED) {
        WinError("could not inspect the backup cancellation event");
    }
    if (cancelled == WAIT_OBJECT_0) {
        return kCancelledExitCode;
    }

    auto [read_user, selected_volumes] = [] {
        const auto persistent = ResolvePersistentPaths();
        auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return std::pair{
            std::move(configuration.windows_username),
            std::move(configuration.volumes),
        };
    }();
    if (!volume_override.empty()) {
        selected_volumes.assign_range(volume_override);
    }
    auto volumes = std::vector<std::wstring_view>{};
    for (const auto &volume : selected_volumes) {
        volumes.push_back(volume);
    }
    constexpr auto kCallbackFailureExitCode = 2;
    try {
        return devicefs::vshadow::Run(
            cancellation_event,
            !no_writers,
            volumes,
            [&](const devicefs::vshadow::SnapshotSet &snapshot_set) {
                try {
                    return internal::RunSnapshotBackup(
                        cancellation_event,
                        snapshot_set,
                        read_user,
                        namespace_override);
                } catch (const wil::ResultException &error) {
                    std::fwprintf(stderr, L"backup-supervisor: %hs\n",
                        error.what());
                    return kCallbackFailureExitCode;
                } catch (const std::runtime_error &error) {
                    std::fwprintf(stderr, L"backup-supervisor: %hs\n",
                        error.what());
                    return kCallbackFailureExitCode;
                }
            });
    } catch (const std::system_error &error) {
        const auto cancellation_signaled =
            WaitForSingleObject(cancellation_event, 0);
        if (cancellation_signaled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if ((cancellation_signaled == WAIT_OBJECT_0) &&
            (error.code() == std::error_code(
                ERROR_CANCELLED, std::system_category()))) {
            return kCancelledExitCode;
        }
        if (error.code() == std::error_code(
                ERROR_CANCELLED, std::system_category())) {
            throw devicefs::vshadow::OperationError(error.what());
        }
        throw;
    }
}
