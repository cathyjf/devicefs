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

module devicefs.supervisor.native_backup:pbs;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import <wil/stl.h>;
import <wil/token_helpers.h>;
import :internal;
import devicefs.common;
import devicefs.supervisor.configuration;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.installation;

#if defined(__INTELLISENSE__) && !defined(__cpp_lib_start_lifetime_as)
// IntelliSense uses EDG, which does not yet expose these C++23 functions.
// Tracked by <https://github.com/microsoft/STL/issues/6169>.
namespace std {
template <class T> auto start_lifetime_as(void *) noexcept -> T *;
template <class T> auto start_lifetime_as_array(void *, size_t) noexcept -> T *;
} // namespace std
#endif

namespace internal {

using namespace std::chrono_literals;

enum class PbsStandardOutput {
    Forward,
    Capture,
};

struct PbsFishRequest {
    std::span<const std::wstring_view> additional_arguments{};
    std::optional<std::u8string_view> snapshot_manifest;
    PbsStandardOutput standard_output = PbsStandardOutput::Forward;
    std::chrono::milliseconds term_grace = 45s;
    std::chrono::milliseconds kill_grace = 30s;
};

struct PbsFishResult {
    int exit_code;
    std::optional<std::u8string> standard_output;
};

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
    if (command.length() > 1024) {
        // CreateProcessWithLogonW supports a maximum command line length of
        // 1024 characters. A longer command line will cause
        // CreateProcessWithLogonW to fail in a manner that is difficult to
        // diagnose. This explicit error makes the failure more identifiable.
        // See <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw>.
        throw std::runtime_error{std::format(
            "command line is {} characters, which is too long for CreateProcessWithLogonW: {}",
            command.length(),
            ([](const auto &str) {
                return std::string(str.begin(), str.end());
            })(std::filesystem::path(command).u8string())
        )};
    }
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

using PipeOutputConsumption = std::expected<std::size_t, DWORD>;

template <typename Consumer>
    requires std::is_invocable_v<
        Consumer &, std::span<const char8_t>>
[[nodiscard]] auto ReadPipeOutput(
    const HANDLE source,
    Consumer &consumer) noexcept(std::is_nothrow_invocable_v<
        Consumer &, std::span<const char8_t>>) -> DWORD {
    constexpr auto kBufferSize = DWORD{4096};
    auto buffer = std::array<char8_t, kBufferSize>{};
    const auto read_output = [&]() noexcept
        -> std::expected<std::span<const char8_t>, DWORD> {
        auto read = DWORD{};
        if (!ReadFile(source, buffer.data(), kBufferSize,
                &read, nullptr)) {
            const auto error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                return std::unexpected{error};
            }
            return std::span<const char8_t>{};
        }
        return std::span{buffer}.first(read);
    };

    auto pending = read_output();
    while (pending && (!pending->empty())) {
        const auto consumed = consumer(*pending);
        if (!consumed) {
            return consumed.error();
        }
        if (*consumed == 0) {
            return DWORD{ERROR_WRITE_FAULT};
        }

        *pending = pending->subspan(*consumed);
        if (pending->empty()) {
            pending = read_output();
        }
    }
    return pending ? DWORD{ERROR_SUCCESS} : pending.error();
}

struct ForwardPipeOutput {
    HANDLE destination;

    [[nodiscard]] auto operator()(
        const std::span<const char8_t> data) const noexcept
        -> PipeOutputConsumption {
        const auto size =
            wil::safe_cast_failfast<DWORD>(data.size_bytes());
        auto consumed = DWORD{};
        if (!WriteFile(destination, data.data(), size, &consumed, nullptr)) {
            return std::unexpected{GetLastError()};
        }
        return consumed;
    }

    [[nodiscard]] auto Finish(const DWORD error) const noexcept {
        return error;
    }
};

struct CapturePipeOutput {
    [[nodiscard]] auto operator()(const std::span<const char8_t> data)
        -> PipeOutputConsumption {
        output.append(data.data(), data.size());
        return data.size();
    }

    [[nodiscard]] auto Finish(const DWORD error) {
        if (error != ERROR_SUCCESS) {
            WinError("could not copy WSL output",
                ExplicitWin32Error{error});
        }
        return std::move(output);
    }

    std::u8string output;
};

template <typename Operation>
class PipeReader {
    using Result = decltype(std::declval<Operation &>().Finish(DWORD{}));

  public:
    PipeReader(wil::unique_handle source, Operation operation) {
        auto task = std::packaged_task<Result()>(
            [source = std::move(source),
                operation = std::move(operation)]() mutable {
                const auto error = ReadPipeOutput(source.get(), operation);
                return operation.Finish(error);
            });
        result_ = task.get_future();
        worker_ = std::thread(std::move(task));
    }

    PipeReader(const PipeReader &) = delete;
    auto operator=(const PipeReader &) -> PipeReader & = delete;
    PipeReader(PipeReader &&) = default;
    auto operator=(PipeReader &&) -> PipeReader & = delete;

    [[gsl::suppress("26447",
        justification: "A still-running pipe reader must be detached when its WSL operation is abandoned.")]]
    ~PipeReader() {
        if (worker_.joinable()) {
            worker_.detach();
        }
    }

    auto Finish() {
        worker_.join();
        return result_.get();
    }

  private:
    std::future<Result> result_;
    std::thread worker_;
};

using PipeCopy = PipeReader<ForwardPipeOutput>;
using PipeCapture = PipeReader<CapturePipeOutput>;

struct WslProcess {
    wil::unique_handle token;
    LoadedProfile profile;
    wil::unique_process_information process;
    std::optional<PipeCopy> standard_output;
    std::optional<PipeCapture> captured_standard_output;
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

    auto FinishOutput() -> std::optional<std::u8string> {
        if (captured_standard_output) {
            const auto standard_error_result = standard_error
                ? standard_error->Finish() : DWORD{ERROR_SUCCESS};
            auto captured = captured_standard_output->Finish();
            if (standard_error_result != ERROR_SUCCESS) {
                WinError("could not copy WSL output",
                    ExplicitWin32Error{standard_error_result});
            }
            return std::move(captured);
        }

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
        return std::nullopt;
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
        L"read --null --global DEVICEFS_FISH_PROGRAM && eval $DEVICEFS_FISH_PROGRAM",
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
    const std::span<const char8_t> standard_input,
    const PbsStandardOutput standard_output = PbsStandardOutput::Forward) {
    auto started = StartWslFishProcess(configuration, arguments);
    if (standard_output == PbsStandardOutput::Forward) {
        started.process.standard_output.emplace(
            std::move(started.standard_output),
            ForwardPipeOutput{
                .destination = GetStdHandle(STD_OUTPUT_HANDLE)});
    } else {
        started.process.captured_standard_output.emplace(
            std::move(started.standard_output), CapturePipeOutput{});
    }
    started.process.standard_error.emplace(
        std::move(started.standard_error),
        ForwardPipeOutput{
            .destination = GetStdHandle(STD_ERROR_HANDLE)});

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

[[nodiscard]] auto FinishWsl(WslProcess &process) -> PbsFishResult {
    auto standard_output = process.FinishOutput();
    return PbsFishResult{
        .exit_code = std::bit_cast<int>(
            ProcessExitCode(process.process.hProcess)),
        .standard_output = std::move(standard_output),
    };
}

enum class PbsFishSignal {
    Term,
    Kill,
};

struct PbsFishSignalText {
    std::wstring_view argument;
    std::string_view diagnostic;
};

[[nodiscard]] constexpr auto SignalText(
    const PbsFishSignal signal) noexcept {
    switch (signal) {
    case PbsFishSignal::Term:
        return PbsFishSignalText{L"TERM", "TERM"};
    case PbsFishSignal::Kill:
        return PbsFishSignalText{L"KILL", "KILL"};
    }
    std::unreachable();
}

auto SendPbsFishSignal(
    const std::wstring_view pid_file,
    const std::wstring_view stop_file,
    const PbsFishSignal signal) {
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
    const auto result = FinishWsl(request);
    if (result.exit_code != 0) {
        throw std::runtime_error(std::format(
            "the WSL {} request exited with code {}",
            text.diagnostic, result.exit_code));
    }
}

auto TrySendPbsFishSignal(
    const std::wstring_view pid_file,
    const std::wstring_view stop_file,
    const PbsFishSignal signal) noexcept {
    try {
        SendPbsFishSignal(pid_file, stop_file, signal);
    } catch (const std::exception &error) {
        TryWriteError("could not signal the PBS operation", error);
    }
}

[[nodiscard]] auto RunPbsFish(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override,
    const PbsFishRequest &request) -> std::optional<PbsFishResult> {
    constexpr auto kPollInterval = 100ms;
    const auto control_path = std::format(
        L"/tmp/devicefs-{}", UniqueName());
    const auto pid_file = std::format(L"{}.pid", control_path);
    const auto stop_file = std::format(L"{}.stop", control_path);
    const auto computer_name =
        wil::GetEnvironmentVariableW<std::wstring>(L"COMPUTERNAME");
    auto operation = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        auto arguments = std::vector<std::wstring_view>{
            pid_file, stop_file, computer_name};
        if (configuration.pbs_parallelize_image_upload) {
            arguments.push_back(L"--parallel-images");
        }
        arguments.append_range(request.additional_arguments);

        auto input = SecureUtf8String{};
        // start-pbs.fish consumes these nine NUL-delimited records in order,
        // followed by the key document that proxmox-backup-client reads
        // through fd 0.
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
        append_record(request.snapshot_manifest.value_or(
            std::u8string_view{}));
        input.append(configuration.pbs_encryption_key);
        return StartWslFish(
            configuration,
            arguments,
            StartPbsProgram(),
            std::span<const char8_t>{input.data(), input.size()},
            request.standard_output);
    }();

    while (true) {
        if (WaitForProcess(
                operation.process.hProcess, kPollInterval)) {
            return FinishWsl(operation);
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            break;
        }
    }

    TrySendPbsFishSignal(pid_file, stop_file, PbsFishSignal::Term);
    if (!WaitForProcess(
            operation.process.hProcess, request.term_grace)) {
        TrySendPbsFishSignal(pid_file, stop_file, PbsFishSignal::Kill);
        if (!WaitForProcess(
                operation.process.hProcess, request.kill_grace)) {
            throw std::runtime_error(
                "the PBS operation did not exit after the KILL request");
        }
    }
    const auto result = FinishWsl(operation);
    if (result.exit_code != 0) {
        std::wcout << L"The PBS operation exited with code "
                   << result.exit_code << L" during cancellation.\n";
    }
    return std::nullopt;
}

} // namespace internal
