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

export module devicefs.supervisor.process_launch;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import <devicefs/malloca.h>;
import devicefs.supervisor.process_attribute_list;

export
[[gsl::suppress("26447",
    justification: "`wil::safe_cast_failfast` cannot throw.")]]
[[nodiscard]] auto WaitForProcess(
    const HANDLE process,
    const std::chrono::milliseconds timeout) noexcept -> bool {
    return wil::handle_wait(
        process, wil::safe_cast_failfast<DWORD>(timeout.count()));
}

export [[nodiscard]] auto ProcessExitCode(const HANDLE process) {
    auto result = DWORD{};
    if (!GetExitCodeProcess(process, &result)) {
        WinError("failed to obtain the child process exit code");
    }
    return result;
}

export [[nodiscard]] auto DuplicateInheritableHandle(const HANDLE source) {
    if ((source == nullptr) || (source == INVALID_HANDLE_VALUE)) {
        throw std::runtime_error("a child-process handle is unavailable");
    }
    auto duplicate = wil::unique_handle{};
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
            duplicate.addressof(), 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        WinError("failed to duplicate a child-process handle");
    }
    return duplicate;
}

export [[nodiscard]] auto StartProcessWithHandles(
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error,
    const auto &start,
    const std::span<const HANDLE> additional_handles = {},
    auto &&...additional_attributes) {
    const auto child_handles = std::array{
        DuplicateInheritableHandle(standard_input),
        DuplicateInheritableHandle(standard_output),
        DuplicateInheritableHandle(standard_error),
    };
    const auto inherited_handle_count =
        child_handles.size() + additional_handles.size();
    const auto inherited_handles = NEW_MALLOCA_BUFFER(
        decltype(child_handles[0].get()), inherited_handle_count);
    if (!inherited_handles) {
        throw std::bad_alloc{};
    }
    std::ranges::copy(additional_handles,
        std::ranges::transform(
            child_handles, inherited_handles.get(),
            [](const auto &handle) { return handle.get(); }).out);
    const auto attributes = ProcessAttributeList{
        std::tuple{PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.get(),
            inherited_handle_count * sizeof(inherited_handles[0])},
        std::forward<decltype(additional_attributes)>(additional_attributes)...};

    auto startup = STARTUPINFOEXA{
        .StartupInfo = {
            .cb = sizeof(STARTUPINFOEXA),
            .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
            .wShowWindow = SW_HIDE,
            .hStdInput = inherited_handles[0],
            .hStdOutput = inherited_handles[1],
            .hStdError = inherited_handles[2],
        },
        .lpAttributeList = attributes.get(),
    };
    return start(&startup.StartupInfo);
}

export template <typename Argv>
struct ProcessLaunchOptions {
    const Argv &argv;
    DWORD flags = 0;
    HANDLE standard_input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE standard_output = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE standard_error = GetStdHandle(STD_ERROR_HANDLE);
    // These handles must already be inheritable.
    std::span<const HANDLE> inherited_handles;
};

// Start the fully qualified executable in `options.argv[0]`. The child
// receives duplicates of the current standard handles unless replacements
// are supplied.
export template <typename Argv>
[[nodiscard]] auto StartProcessWithHandles(
    const ProcessLaunchOptions<Argv> &options,
    auto &&...additional_attributes) {
    auto command = wil::ArgvToCommandLine(options.argv);
    return StartProcessWithHandles(
        options.standard_input, options.standard_output, options.standard_error,
        [executable = std::string{options.argv.front()}, &command,
        flags = options.flags](STARTUPINFOA *const startup) {
            auto process = wil::unique_process_information{};
            if (!CreateProcessA(executable.c_str(),
                    command.data(), nullptr, nullptr, TRUE,
                    flags | EXTENDED_STARTUPINFO_PRESENT,
                    nullptr, nullptr, startup, &process)) {
                WinError("failed to start program '{}'", executable);
            }
            return process;
        }, options.inherited_handles,
        std::forward<decltype(additional_attributes)>(
            additional_attributes)...);
}

export using PipeOutputConsumption = std::expected<std::size_t, DWORD>;

export template <typename Consumer>
    requires std::is_invocable_v<
        Consumer &, std::span<const char8_t>>
[[nodiscard]] auto ReadPipeOutput(
    const HANDLE source,
    Consumer &&consumer) noexcept(std::is_nothrow_invocable_v<
        Consumer &, std::span<const char8_t>>) -> DWORD {
    constexpr auto kBufferSize = DWORD{4096};
    /* [[indeterminate]] */ std::array<char8_t, kBufferSize> buffer;
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

export struct ForwardPipeOutput {
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
