// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <windows.h>
#include <sddl.h>
// WIL's shared resource owners require `<memory>` before `<wil/resource.h>`.
#include <memory>
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/win32_helpers.h>

export module devicefs.publisher.windows_process;

import std;
export import devicefs.publisher.base_process;
import devicefs.terminal.scope_exit;

namespace devicefs::publisher::windows_detail {

// Batch transfers to reduce syscall overhead, but keep the per-reader stack
// buffer modest. 8 KiB is a compromise between those two costs.
constexpr auto kIoChunkSize = 8192uz;

[[nodiscard]] auto MakeEvent() -> wil::unique_event {
    auto event = wil::unique_event{CreateEventA(nullptr, TRUE, FALSE, nullptr)};
    if (!event) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not create a child-pipe event");
    }
    return event;
}

struct Endpoint {
    wil::unique_hfile pipe;
    wil::unique_event completion;
    wil::shared_event cancellation;
};

struct Pipe {
    Endpoint parent;
    wil::unique_hfile child;
};

[[nodiscard]] auto MakePipe(
    const wil::shared_event &cancellation,
    const bool input = false) -> Pipe {
    static auto sequence = std::atomic<unsigned long long>{};
    const auto name = std::format("\\\\.\\pipe\\devicefs-publisher-{}-{}",
        GetCurrentProcessId(), sequence.fetch_add(1));
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    // Unlike anonymous pipes, these names can be opened by other processes.
    // Restrict access to the owner and SYSTEM, not the default Everyone ACL.
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "D:P(A;;GA;;;OW)(A;;GA;;;SY)", SDDL_REVISION_1,
            &descriptor, nullptr)) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not secure a child pipe");
    }
    auto parent_security = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get()};
    // One instance serves the single child endpoint opened below. Leave buffer
    // sizing to Windows, as with the former anonymous pipes, rather than
    // coupling it to our read/write chunk size. The timeout is unused because
    // the client endpoint is opened directly, without `WaitNamedPipe`.
    auto parent = wil::unique_hfile{CreateNamedPipeA(name.c_str(),
        (input ? PIPE_ACCESS_OUTBOUND : PIPE_ACCESS_INBOUND) |
            FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 0, 0, 0, &parent_security)};
    if (!parent) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not create a cancellable child pipe");
    }
    auto security = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
    auto child = wil::unique_hfile{CreateFileA(name.c_str(),
        input ? GENERIC_READ : GENERIC_WRITE, 0, &security,
        OPEN_EXISTING, 0, nullptr)};
    if (!child) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not open the child's pipe endpoint");
    }
    auto completion = MakeEvent();
    auto connection = OVERLAPPED{.hEvent = completion.get()};
    // Our client is already connected, so no connection wait is needed.
    if ((!ConnectNamedPipe(parent.get(), &connection)) &&
        (GetLastError() != ERROR_PIPE_CONNECTED)) {
        throw std::system_error(GetLastError(), std::system_category(),
            "could not connect a child pipe");
    }
    return {.parent = {.pipe = std::move(parent),
        .completion = std::move(completion), .cancellation = cancellation},
        .child = std::move(child)};
}

[[nodiscard]] auto Transfer(const Endpoint &endpoint, const auto &operation,
    const bool reading) -> std::optional<DWORD> {
    if (endpoint.cancellation.is_signaled()) {
        return std::nullopt;
    }
    auto pending = OVERLAPPED{.hEvent = endpoint.completion.get()};
    auto count = DWORD{};
    auto error = operation(&count, &pending) ?
        ERROR_SUCCESS : GetLastError();
    if (error == ERROR_IO_PENDING) {
        // Cancellation must also wait for completion: the kernel still owns
        // the buffer and `OVERLAPPED` until then, even after `CancelIoEx`.
        // https://learn.microsoft.com/windows/win32/api/ioapiset/nf-ioapiset-cancelioex
        auto finish = devicefs::terminal::ScopeExit{[&endpoint, &pending] {
            std::ignore = CancelIoEx(endpoint.pipe.get(), &pending);
            auto ignored = DWORD{};
            std::ignore = GetOverlappedResult(
                endpoint.pipe.get(), &pending, &ignored, TRUE);
        }};
        const auto events = std::array{
            endpoint.cancellation.get(), endpoint.completion.get()};
        const auto result = WaitForMultipleObjects(
            wil::safe_cast_failfast<DWORD>(events.size()),
            events.data(), FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            return std::nullopt;
        }
        if (result == WAIT_FAILED) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not wait for child-pipe I/O or cancellation");
        }
        error = GetOverlappedResult(
            endpoint.pipe.get(), &pending, &count, FALSE) ?
                ERROR_SUCCESS : GetLastError();
        finish.release();
    }
    if (reading && (error == ERROR_BROKEN_PIPE)) {
        return std::nullopt;
    }
    if (error != ERROR_SUCCESS) {
        throw std::system_error(error, std::system_category(),
            reading ? "could not read a child-output pipe" :
                "could not write the child's standard input");
    }
    return count;
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
        std::string input = {},
        const std::function<void(std::string_view)> &write_output = {})
        : BaseProcess(arguments) {
        auto output = windows_detail::MakePipe(cancellation_);
        auto diagnostic = windows_detail::MakePipe(cancellation_);
        StartReaders(std::move(output.parent), std::move(diagnostic.parent),
            write_output);
        auto command = wil::ArgvToCommandLine(arguments);
        auto security = SECURITY_ATTRIBUTES{
            .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE};
        const auto input_source = [this, &input, &security]()
            -> wil::unique_hfile {
            if (input.empty()) {
                auto source = wil::unique_hfile{CreateFileA("NUL", GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                    &security, OPEN_EXISTING, 0, nullptr)};
                if (!source) {
                    throw std::system_error(GetLastError(),
                        std::system_category(),
                        "could not open NUL for the child's standard input");
                }
                return source;
            }
            auto pipe = windows_detail::MakePipe(cancellation_, true);
            input_writer_ = StartWorker(
                [](const windows_detail::Endpoint writer,
                    const std::string input) {
                    auto remaining = std::string_view{input};
                    while (!remaining.empty()) {
                        const auto requested = wil::safe_cast_failfast<DWORD>(
                            std::min(remaining.size(),
                                windows_detail::kIoChunkSize));
                        const auto count = windows_detail::Transfer(writer,
                            [&writer, remaining, requested](auto *const count,
                                auto *const pending) {
                                return WriteFile(writer.pipe.get(),
                                    remaining.data(), requested,
                                    count, pending);
                            }, false);
                        if (!count) {
                            return;
                        }
                        if (*count == 0) {
                            throw std::system_error(ERROR_WRITE_FAULT,
                                std::system_category(),
                                "child-stdin write made no progress");
                        }
                        remaining.remove_prefix(*count);
                    }
                }, std::move(pipe.parent), std::move(input));
            return std::move(pipe.child);
        }();
        auto startup = STARTUPINFOA{.cb = sizeof(STARTUPINFOA),
            .dwFlags = STARTF_USESTDHANDLES,
            .hStdInput = input_source.get(),
            .hStdOutput = output.child.get(),
            .hStdError = diagnostic.child.get()};
        if (!CreateProcessA(nullptr, command.data(), nullptr, nullptr, TRUE, 0,
                nullptr, nullptr, &startup, &process_)) {
            const auto error = GetLastError();
            throw std::system_error(error, std::system_category(),
                std::format("could not start '{}'", arguments.front()));
        }
    }

    WindowsProcess(const WindowsProcess &) = delete;
    WindowsProcess(WindowsProcess &&) = delete;

    [[gsl::suppress("26456", justification:
        "The base class's assignment operator is deleted.")]]
    auto operator=(const WindowsProcess &) -> WindowsProcess & = delete;

    [[gsl::suppress("26456", justification:
        "The base class's move assignment operator is deleted.")]]
    auto operator=(WindowsProcess &&) -> WindowsProcess & = delete;

    auto Terminate() const noexcept -> void {
        cancellation_.SetEvent();
        std::ignore = TerminateProcess(process_.hProcess, 1);
    }

    ~WindowsProcess() {
        Terminate();
        JoinIo();
        std::ignore = WaitForSingleObject(process_.hProcess, INFINITE);
    }

private:
    friend class BaseProcess;

    [[nodiscard]] static auto Read(const windows_detail::Endpoint &source)
        -> std::string {
        auto buffer = std::array<char, windows_detail::kIoChunkSize>{};
        for (;;) {
            const auto count = windows_detail::Transfer(source,
                [&source, &buffer](auto *const count, auto *const pending) {
                    return ReadFile(source.pipe.get(), buffer.data(),
                        wil::safe_cast_failfast<DWORD>(buffer.size()),
                        count, pending);
                }, true);
            if (!count) {
                return {};
            }
            // A zero-byte pipe write can complete a read without closing the
            // pipe. Only `ERROR_BROKEN_PIPE` establishes EOF here.
            if (*count != 0) {
                return {buffer.data(), *count};
            }
        }
    }

    [[nodiscard]] auto WaitForExit() const -> int {
        if (WaitForSingleObject(process_.hProcess, INFINITE) == WAIT_FAILED) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not wait for the child process");
        }
        auto code = DWORD{};
        if (!GetExitCodeProcess(process_.hProcess, &code)) {
            throw std::system_error(GetLastError(), std::system_category(),
                "could not obtain the child's exit status");
        }
        return code;
    }

    const wil::shared_event cancellation_{windows_detail::MakeEvent()};
    wil::unique_process_information process_;
};

}
