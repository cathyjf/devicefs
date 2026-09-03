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

module devicefs.supervisor.native_backup:pbs;

import std;
import <devicefs/windows_imports.h>;
import :internal;
import :password_reset;
import :privileges;
import :s4u_logon;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.configuration;
import devicefs.supervisor.embedded_artifacts;
import devicefs.supervisor.installation;

namespace internal {

using namespace std::chrono_literals;

enum class PbsStandardOutput {
    Forward,
    Capture,
    Readiness,
};

enum class WslEnvironment {
    Backup,
    Restore,
};

struct PbsFishRequest {
    std::span<const std::string_view> additional_arguments{};
    std::optional<std::u8string_view> snapshot_manifest;
    std::string_view rpc_password{};
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

[[nodiscard]] auto RunningAsLocalSystem() {
    auto result = false;
    const auto error = wil::test_token_membership_nothrow(
        &result, nullptr, SECURITY_NT_AUTHORITY, SECURITY_LOCAL_SYSTEM_RID);
    if (FAILED(error)) {
        WinError("could not identify the backup-supervisor account",
            ExplicitWin32Error::FromHresult(error));
    }
    return result;
}

[[nodiscard]] auto StartWslWithLogon(
    const wil::zwstring_view username,
    const wil::zwstring_view password,
    const std::filesystem::path &wsl_path,
    std::string &command,
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
    auto wide_command = std::filesystem::path{command}.wstring();
    if (wide_command.length() > 1024) {
        // CreateProcessWithLogonW supports a maximum command line length of
        // 1024 characters. A longer command line will cause
        // CreateProcessWithLogonW to fail in a manner that is difficult to
        // diagnose. This explicit error makes the failure more identifiable.
        // See <https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-createprocesswithlogonw>.
        throw std::runtime_error{std::format(
            "command line is {} characters, which is too long for CreateProcessWithLogonW: {}",
            wide_command.length(), command)};
    }
    auto process = wil::unique_process_information{};
    // This API copies STARTF_USESTDHANDLES itself and, unlike the service
    // launch path below, is available only when the caller is not LocalSystem.
    // Manual backups create no supervisor job, so do not request breakaway.
    if (!CreateProcessWithLogonW(
            username.c_str(), kLocalDomain.c_str(), password.c_str(),
            LOGON_WITH_PROFILE, wsl_path.c_str(), wide_command.data(),
            kWslCreationFlags,
            nullptr, working_directory.c_str(), &startup, &process)) {
        WinError("could not start WSL '{}' as configured account '{}'",
            std::wstring_view{wsl_path.native()},
            std::wstring_view{username.c_str(), username.size()});
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

    [[gsl::suppress("26447",
        justification:
            "The span cannot exceed the 4096-byte read buffer, so the conversion "
            "cannot fail. safe_cast_failfast terminates rather than throws if that "
            "invariant is violated.")]]
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

class SambaReadinessReader {
  public:
    explicit SambaReadinessReader(wil::unique_handle source) {
        if (!completion_.try_create(
                wil::EventOptions::ManualReset, nullptr)) {
            WinError("could not create the Samba-readiness event");
        }
        auto completion_signal = wil::unique_handle{};
        if (!DuplicateHandle(
                GetCurrentProcess(), completion_.get(),
                GetCurrentProcess(), completion_signal.addressof(),
                0, FALSE, DUPLICATE_SAME_ACCESS)) {
            WinError("could not duplicate the Samba-readiness event");
        }
        auto task = std::packaged_task<DWORD()>(
            [source = std::move(source),
                completion_signal = std::move(completion_signal)] {
                auto ready = char{};
                auto read = DWORD{};
                const auto result = [&] {
                    if (!ReadFile(source.get(), &ready, 1, &read, nullptr)) {
                        return GetLastError();
                    }
                    return read == 1
                        ? DWORD{ERROR_SUCCESS} : DWORD{ERROR_BROKEN_PIPE};
                }();
                static_cast<void>(SetEvent(completion_signal.get()));
                return result;
            });
        result_ = task.get_future();
        worker_ = std::thread(std::move(task));
    }

    SambaReadinessReader(const SambaReadinessReader &) = delete;
    auto operator=(const SambaReadinessReader &)
        -> SambaReadinessReader & = delete;
    SambaReadinessReader(SambaReadinessReader &&) = default;
    auto operator=(SambaReadinessReader &&)
        -> SambaReadinessReader & = delete;

    [[gsl::suppress("26447",
        justification:
            "A still-running Samba-readiness read must be detached when its "
            "WSL operation is abandoned.")]]
    ~SambaReadinessReader() {
        if (worker_.joinable()) {
            worker_.detach();
        }
    }

    [[nodiscard]] auto Event() const noexcept {
        return completion_.get();
    }

    [[nodiscard]] auto Finish() {
        worker_.join();
        return result_.get();
    }

  private:
    wil::unique_event_nothrow completion_;
    std::future<DWORD> result_;
    std::thread worker_;
};

struct WslProcess {
    wil::unique_handle token;
    LoadedProfile profile;
    wil::unique_process_information process;
    std::optional<PipeCopy> standard_output;
    std::optional<PipeCapture> captured_standard_output;
    std::optional<SambaReadinessReader> samba_readiness;
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
        if (samba_readiness) {
            static_cast<void>(samba_readiness->Finish());
            samba_readiness.reset();
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
    const std::span<const std::string_view> arguments,
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error) {
    const auto wsl_path = ProgramFilesDirectory() / L"WSL" / L"wsl.exe";
    const auto wsl_path_text = wsl_path.string();
    auto argument_views = std::vector<std::string_view>{wsl_path_text};
    argument_views.append_range(arguments);
    auto command = wil::ArgvToCommandLine(argument_views);
    const auto wsl_directory = wsl_path.parent_path();
    const auto windows_username =
        std::filesystem::path{
            configuration.windows_username}.wstring();
    if (!RunningAsLocalSystem()) {
        auto result = WslProcess{};
        result.process = StartWslWithLogon(
            windows_username,
            ResetBackupAccountPassword(windows_username),
            wsl_path, command, wsl_directory,
            standard_input, standard_output, standard_error);
        return result;
    }

    auto result = WslProcess{};
    result.token = LogOnWindowsAccountWithS4u(windows_username);

    [[gsl::suppress("type.3",
        justification: "LoadUserProfileW retains a mutable historical parameter for an input-only username.")]]
    auto *const profile_user =
        const_cast<PWSTR>(windows_username.c_str());
    auto profile = PROFILEINFOW{
        .dwSize = sizeof(PROFILEINFOW),
        .dwFlags = PI_NOUI,
        .lpUserName = profile_user,
    };
    {
        // LoadUserProfileW requires these privileges on its LocalSystem
        // caller. They are not added to the user token used to start WSL.
        constexpr auto privilege_names = std::array{
            wil::zwstring_view(SE_BACKUP_NAME),
            wil::zwstring_view(SE_RESTORE_NAME),
        };
        auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(), privilege_names,
            std::string_view{"the user-profile privileges"}};
        if (!LoadUserProfileW(result.token.get(), &profile)) {
            WinError("could not load the profile for configured WSL account '{}'",
                std::wstring_view{windows_username});
        }
        result.profile = LoadedProfile{
            profile.hProfile, ProfileCloser{result.token.get()}};
        privileges.Restore();
    }

    auto environment = wil::unique_environment_block{};
    if (!CreateEnvironmentBlock(
            environment.addressof(), result.token.get(), FALSE)) {
        WinError("could not create the environment for configured WSL account '{}'",
            std::wstring_view{windows_username});
    }

    // CreateProcessWithTokenW has no bInheritHandles parameter, so it cannot
    // satisfy PROC_THREAD_ATTRIBUTE_HANDLE_LIST's documented requirements.
    // WSL manages its own infrastructure, so exclude it from the backup job.
    result.process = StartProcessWithHandles(
        standard_input,
        standard_output,
        standard_error,
        [&](STARTUPINFOA *const startup, PROCESS_INFORMATION *const process) {
            return CreateProcessAsUserA(
                result.token.get(), wsl_path_text.c_str(), command.data(),
                nullptr, nullptr, TRUE,
                kWslCreationFlags | CREATE_BREAKAWAY_FROM_JOB |
                    EXTENDED_STARTUPINFO_PRESENT,
                environment.get(), wsl_directory.string().c_str(), startup, process);
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

[[nodiscard]] auto SelectRestoreWslConfiguration(
    const BackupConfiguration &configuration)
    -> const WslRestoreConfiguration & {
    if (!configuration.wsl_restore) {
        throw std::runtime_error("wsl.restore is not configured");
    }
    return *configuration.wsl_restore;
}

[[nodiscard]] auto SelectWslConfiguration(
    const BackupConfiguration &configuration,
    const WslEnvironment environment) -> const WslConfiguration & {
    switch (environment) {
    case WslEnvironment::Backup:
        return configuration.wsl_backup;
    case WslEnvironment::Restore:
        return SelectRestoreWslConfiguration(configuration);
    }
    std::unreachable();
}

[[nodiscard]] auto StartWslFishProcess(
    const BackupConfiguration &configuration,
    const WslConfiguration &wsl_configuration,
    const std::span<const std::string_view> arguments) -> StartedWslFish {
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

    auto wsl_arguments = std::vector<std::string_view>{
        "--distribution", wsl_configuration.distribution,
    };
    if (wsl_configuration.linux_user) {
        wsl_arguments.push_back("--user");
        wsl_arguments.push_back(*wsl_configuration.linux_user);
    }
    wsl_arguments.append_range(std::to_array<std::string_view>({
        "--exec", "/usr/bin/fish", "--no-config", "-c",
        "read --null --global DEVICEFS_FISH_PROGRAM && eval $DEVICEFS_FISH_PROGRAM",
        "--",
    }));
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
    const WslConfiguration &wsl_configuration,
    const std::span<const std::string_view> arguments,
    const std::span<const char> program,
    const std::span<const char8_t> standard_input,
    const PbsStandardOutput standard_output = PbsStandardOutput::Forward) {
    auto started = StartWslFishProcess(
        configuration, wsl_configuration, arguments);
    if (standard_output == PbsStandardOutput::Forward) {
        started.process.standard_output.emplace(
            std::move(started.standard_output),
            ForwardPipeOutput{
                .destination = GetStdHandle(STD_OUTPUT_HANDLE)});
    } else if (standard_output == PbsStandardOutput::Capture) {
        started.process.captured_standard_output.emplace(
            std::move(started.standard_output), CapturePipeOutput{});
    } else {
        started.process.samba_readiness.emplace(
            std::move(started.standard_output));
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

[[nodiscard]] constexpr auto SignalText(
    const PbsFishSignal signal) noexcept -> std::string_view {
    switch (signal) {
    case PbsFishSignal::Term:
        return "TERM";
    case PbsFishSignal::Kill:
        return "KILL";
    }
    std::unreachable();
}

auto SendPbsFishSignal(
    const std::string_view pid_file,
    const std::string_view stop_file,
    const WslEnvironment environment,
    const PbsFishSignal signal) {
    constexpr auto kControlTimeout = 15s;
    constexpr auto program = std::string_view(
        "touch $argv[2]; "
        "if test -s $argv[1]; kill -s $argv[3] (cat $argv[1]); end");
    const auto text = SignalText(signal);
    const auto arguments = std::array{
        pid_file, stop_file, text};
    auto request = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        const auto &wsl_configuration =
            SelectWslConfiguration(configuration, environment);
        return StartWslFish(
            configuration,
            wsl_configuration,
            arguments,
            std::span<const char>{program},
            std::span<const char8_t>{});
    }();
    if (!WaitForProcess(
            request.process.hProcess, kControlTimeout)) {
        throw std::runtime_error(std::format(
            "the WSL {} request did not exit before its timeout",
            text));
    }
    const auto result = FinishWsl(request);
    if (result.exit_code != 0) {
        throw std::runtime_error(std::format(
            "the WSL {} request exited with code {}",
            text, result.exit_code));
    }
}

auto TrySendPbsFishSignal(
    const std::string_view pid_file,
    const std::string_view stop_file,
    const WslEnvironment environment,
    const PbsFishSignal signal) noexcept {
    try {
        SendPbsFishSignal(pid_file, stop_file, environment, signal);
    } catch (const std::exception &error) {
        TryWriteError("could not signal the PBS operation", error);
    }
}

struct PbsFishOperation {
    [[nodiscard]] auto Process() const noexcept {
        return process.process.hProcess;
    }

    WslProcess process;
    std::string pid_file;
    std::string stop_file;
    WslEnvironment environment;
    std::chrono::milliseconds term_grace;
    std::chrono::milliseconds kill_grace;
};

[[nodiscard]] auto WaitForSambaReadiness(
    PbsFishOperation &operation,
    const HANDLE cancellation_event) {
    auto &readiness = *operation.process.samba_readiness;
    const auto handles = std::array{
        readiness.Event(), operation.Process(), cancellation_event};
    const auto result = WaitForMultipleObjects(
        wil::safe_cast_failfast<DWORD>(handles.size()),
        handles.data(), FALSE, INFINITE);
    if (result == WAIT_FAILED) {
        WinError("could not wait for Samba readiness");
    }
    if (result == WAIT_OBJECT_0) {
        const auto error = readiness.Finish();
        operation.process.samba_readiness.reset();
        if (error != ERROR_SUCCESS) {
            if (WaitForProcess(operation.Process(), 0ms)) {
                throw std::runtime_error(std::format(
                    "the PBS view operation exited before Samba "
                    "reported readiness with code {}",
                    ProcessExitCode(operation.Process())));
            }
            WinError("could not read Samba readiness from WSL",
                ExplicitWin32Error{error});
        }
        return true;
    }
    if (result == (WAIT_OBJECT_0 + 1)) {
        throw std::runtime_error(std::format(
            "the PBS view operation exited before Samba "
            "reported readiness with code {}",
            ProcessExitCode(operation.Process())));
    }
    if (result == (WAIT_OBJECT_0 + 2)) {
        return false;
    }
    std::unreachable();
}

[[nodiscard]] auto StopPbsFish(PbsFishOperation &operation) {
    if (!WaitForProcess(operation.Process(), 0ms)) {
        TrySendPbsFishSignal(
            operation.pid_file, operation.stop_file,
            operation.environment, PbsFishSignal::Term);
        if (!WaitForProcess(
                operation.Process(), operation.term_grace)) {
            TrySendPbsFishSignal(
                operation.pid_file, operation.stop_file,
                operation.environment, PbsFishSignal::Kill);
            if (!WaitForProcess(
                    operation.Process(), operation.kill_grace)) {
                throw std::runtime_error(
                    "the PBS operation did not exit after the KILL request");
            }
        }
    }
    return FinishWsl(operation.process);
}

auto TryStopPbsFish(PbsFishOperation &operation) noexcept -> void {
    try {
        const auto result = StopPbsFish(operation);
        if (result.exit_code != 0) {
            devicefs::WriteToStream(
                devicefs::stdout,
                "The Linux view operation exited with code {} "
                "during cleanup.\n",
                result.exit_code);
        }
    } catch (const std::exception &error) {
        TryWriteError("Linux view cleanup failed", error);
    }
}

[[nodiscard]] auto StartPbsFish(
    const BackupConfiguration &configuration,
    const WslConfiguration &wsl_configuration,
    const WslEnvironment environment,
    const std::optional<std::u8string> &namespace_override,
    const PbsFishRequest &request,
    const bool parallel_images) {
    const auto control_path = std::format(
        "/tmp/devicefs-{}", UniqueName());
    auto pid_file = std::format("{}.pid", control_path);
    auto stop_file = std::format("{}.stop", control_path);
    const auto computer_name = std::filesystem::path{
        wil::GetEnvironmentVariableW<std::wstring>(L"COMPUTERNAME")}.string();
    auto arguments = std::vector<std::string_view>{
        pid_file, stop_file, computer_name};
    if (parallel_images) {
        arguments.push_back("--parallel-images");
    }
    arguments.append_range(request.additional_arguments);

    auto input = SecureUtf8String{};
    // start-pbs.fish consumes these ten NUL-delimited records in order,
    // followed by the key document that proxmox-backup-client reads from
    // its inherited standard input.
    const auto append_record = [&](const std::u8string_view value) {
        input.append(value);
        input.push_back(u8'\0');
    };
    append_record(wsl_configuration.client_path);
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
    input.append(
        request.rpc_password.begin(), request.rpc_password.end());
    input.push_back(u8'\0');
    input.append(configuration.pbs_encryption_key);
    auto process = StartWslFish(
        configuration,
        wsl_configuration,
        arguments,
        StartPbsProgram(),
        std::span<const char8_t>{input.data(), input.size()},
        request.standard_output);
    return PbsFishOperation{
        .process = std::move(process),
        .pid_file = std::move(pid_file),
        .stop_file = std::move(stop_file),
        .environment = environment,
        .term_grace = request.term_grace,
        .kill_grace = request.kill_grace,
    };
}

[[nodiscard]] auto StartViewFish(
    const std::optional<std::u8string> &namespace_override,
    const std::optional<std::string_view> snapshot_override,
    const std::optional<std::string_view> timestamp,
    const std::string_view archive,
    const std::string_view address,
    const std::string_view port,
    const std::string_view rpc_password) {
    const auto persistent = ResolvePersistentPaths();
    const auto configuration =
        ReadBackupConfiguration(persistent.configuration);
    const auto &restore_configuration =
        SelectRestoreWslConfiguration(configuration);
    const auto snapshot_override_argument =
        snapshot_override.value_or(std::string_view{});
    auto arguments = std::vector<std::string_view>{
        "--view", "--", snapshot_override_argument, archive, address, port,
        restore_configuration.rpc_helper_path,
        restore_configuration.samba_dcerpcd_path,
    };
    if (timestamp) {
        arguments.push_back(*timestamp);
    }
    return StartPbsFish(
        configuration, restore_configuration,
        WslEnvironment::Restore, namespace_override,
        PbsFishRequest{
            .additional_arguments = arguments,
            .rpc_password = rpc_password,
            .standard_output = PbsStandardOutput::Readiness,
        },
        false);
}

[[nodiscard]] auto RunPbsFish(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override,
    const PbsFishRequest &request) -> std::optional<PbsFishResult> {
    constexpr auto kPollInterval = 100ms;
    auto operation = [&] {
        const auto persistent = ResolvePersistentPaths();
        const auto configuration =
            ReadBackupConfiguration(persistent.configuration);
        return StartPbsFish(
            configuration, configuration.wsl_backup,
            WslEnvironment::Backup,
            namespace_override, request,
            configuration.pbs_parallelize_image_upload);
    }();

    while (true) {
        if (WaitForProcess(
                operation.Process(), kPollInterval)) {
            return FinishWsl(operation.process);
        }
        const auto cancelled = WaitForSingleObject(cancellation_event, 0);
        if (cancelled == WAIT_FAILED) {
            WinError("could not inspect the backup cancellation event");
        }
        if (cancelled == WAIT_OBJECT_0) {
            break;
        }
    }

    const auto result = StopPbsFish(operation);
    if (result.exit_code != 0) {
        devicefs::WriteToStream(
            devicefs::stdout,
            "The PBS operation exited with code {} during cancellation.\n",
            result.exit_code);
    }
    return std::nullopt;
}

} // namespace internal
