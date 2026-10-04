// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <windows.h>
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/win32_helpers.h>

export module devicefs.publisher.windows_process;

import std;
export import devicefs.publisher.base_process;

namespace devicefs::publisher::windows_detail {

struct Pipe {
    wil::unique_handle read;
    wil::unique_handle write;
};

[[nodiscard]] auto MakePipe(const bool inherit_read = false) -> Pipe {
    auto result = Pipe{};
    auto security = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    if ((!CreatePipe(result.read.addressof(), result.write.addressof(),
            &security, 0)) ||
        (!SetHandleInformation(
            inherit_read ? result.write.get() : result.read.get(),
            HANDLE_FLAG_INHERIT, 0))) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not create a child-output pipe");
    }
    return result;
}

}

export namespace devicefs::publisher {

// Use WIL's command-line encoder rather than a shell or handwritten quoting.
// The UTF-8 process manifest lets `CreateProcessA` consume these UTF-8
// arguments. A writer supplies stdin independently of output consumption,
// closing its pipe to provide EOF. Destruction terminates an unfinished direct
// child before joining that writer; it does not supervise descendants.
class WindowsProcess : public BaseProcess {
public:
    explicit WindowsProcess(const std::span<const std::string> arguments,
        std::string input = {})
        : BaseProcess(arguments) {
        auto output = windows_detail::MakePipe();
        auto diagnostic = windows_detail::MakePipe();
        auto command = wil::ArgvToCommandLine(arguments);
        auto security = SECURITY_ATTRIBUTES{
            .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
        const auto input_source = [&]() -> wil::unique_handle {
            if (input.empty()) {
                auto source = wil::unique_hfile{CreateFileA("NUL", GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &security, OPEN_EXISTING, 0, nullptr)};
                if (!source) {
                    throw std::system_error(GetLastError(),
                        std::system_category(),
                        "could not open NUL for the child's standard input");
                }
                return wil::unique_handle{source.release()};
            }
            auto pipe = windows_detail::MakePipe(true);
            input_writer_ = std::async(std::launch::async,
                [](const wil::unique_handle writer, const std::string input) {
                    auto remaining = std::string_view{input};
                    while (!remaining.empty()) {
                        const auto requested = wil::safe_cast_failfast<DWORD>(
                            std::min(remaining.size(), 8192uz));
                        auto count = DWORD{};
                        if (!WriteFile(writer.get(), remaining.data(),
                                requested, &count, nullptr)) {
                            throw std::system_error(GetLastError(),
                                std::system_category(),
                                "could not write the child's standard input");
                        }
                        if (count == 0) {
                            throw std::system_error(ERROR_WRITE_FAULT,
                                std::system_category(),
                                "child-stdin write made no progress");
                        }
                        remaining.remove_prefix(count);
                    }
                }, std::move(pipe.write), std::move(input));
            return std::move(pipe.read);
        }();
        auto startup = STARTUPINFOA{.cb = sizeof(STARTUPINFOA),
            .dwFlags = STARTF_USESTDHANDLES,
            .hStdInput = input_source.get(),
            .hStdOutput = output.write.get(),
            .hStdError = diagnostic.write.get()};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                nullptr, nullptr, &startup, &process_)) {
            const auto error = GetLastError();
            throw std::system_error(error, std::system_category(),
                std::format("could not start '{}'", arguments.front()));
        }
        output_ = std::move(output.read);
        diagnostic_ = std::move(diagnostic.read);
    }

    WindowsProcess(const WindowsProcess &) = delete;
    auto operator=(const WindowsProcess &) -> WindowsProcess & = delete;
    ~WindowsProcess() {
        if (!Result().exit_code) {
            std::ignore = TerminateProcess(process_.hProcess, 1);
            std::ignore = WaitForSingleObject(process_.hProcess, INFINITE);
        }
    }

private:
    friend class BaseProcess;

    [[nodiscard]] static auto ReadAvailable(wil::unique_handle &source)
        -> std::string {
        auto available = DWORD{};
        if (!PeekNamedPipe(source.get(), nullptr, 0, nullptr,
                &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                source.reset();
                return {};
            }
            throw std::system_error(error, std::system_category(),
                "could not inspect a child-output pipe");
        }
        if (available == 0) {
            return {};
        }
        auto buffer = std::array<char, 8192>{};
        auto count = DWORD{};
        if (!ReadFile(source.get(), buffer.data(),
                std::min(available, DWORD{buffer.size()}), &count, nullptr)) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not read a child-output pipe");
        }
        return {buffer.data(), count};
    }

    [[nodiscard]] auto ReadExitCode() -> std::optional<int> {
        const auto waited = WaitForSingleObject(process_.hProcess, 0);
        if (waited == WAIT_FAILED) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not wait for the child process");
        }
        if (waited != WAIT_OBJECT_0) {
            return std::nullopt;
        }
        auto code = DWORD{};
        if (!GetExitCodeProcess(process_.hProcess, &code)) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not obtain the child's exit status");
        }
        return code;
    }

    wil::unique_handle output_;
    wil::unique_handle diagnostic_;
    wil::unique_process_information process_;
};

}
