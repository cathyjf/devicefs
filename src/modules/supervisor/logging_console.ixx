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

// wil/stl.h uses these facilities without including their standard headers.
#include <algorithm>
#include <cstdint>

#include <wil/resource.h>
#include <wil/stl.h>

#include <array>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <future>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

export module devicefs.supervisor.logging_console;

namespace {

using unique_pseudoconsole = wil::unique_any<
    HPCON, decltype(&::ClosePseudoConsole), ::ClosePseudoConsole>;

[[noreturn]] auto WinError(
    const wil::zstring_view operation, const DWORD error = GetLastError()) {
    throw std::system_error(
        std::bit_cast<int>(error), std::system_category(), operation.c_str());
}

auto WriteOutput(const HANDLE log, std::u8string_view output,
    DWORD &first_error) noexcept {
    while (!output.empty() && (first_error == ERROR_SUCCESS)) {
        const auto chunk_size =
            std::min(output.size(), std::size_t{MAXDWORD});
        [[gsl::suppress("type.1",
            justification: "chunk_size is limited to MAXDWORD above.")]]
        const auto size = static_cast<DWORD>(chunk_size);
        auto written = DWORD{};
        if (!WriteFile(log, output.data(), size, &written, nullptr)) {
            first_error = GetLastError();
        } else if (written != size) {
            first_error = ERROR_WRITE_FAULT;
        }
        output.remove_prefix(chunk_size);
    }
}

// ConPTY emits UTF-8 interleaved with VT sequences. Keep parser state across
// pipe reads and strip the ESC-prefixed sequences throughout the stream while
// preserving non-ASCII UTF-8 bytes. Of the C0 controls, retain only tabs and
// line feeds; dropping carriage returns turns CRLF into LF and prevents
// progress output from rewriting replayed lines.
// https://learn.microsoft.com/en-us/windows/console/createpseudoconsole
// https://ecma-international.org/publications-and-standards/standards/ecma-48/
class VtFilter {
public:
    [[nodiscard]] auto Remove(std::span<char8_t> input) noexcept {
        auto output_size = 0uz;
        for (const auto character : input) {
            if (Preserve(character)) {
                input[output_size++] = character;
            }
        }
        return std::u8string_view{input.data(), output_size};
    }

private:
    enum class State {
        Text,
        Escape,
        EscapeIntermediate,
        ControlSequence,
        OscString,
        StString,
    };

    [[nodiscard]] auto Preserve(const char8_t character) noexcept -> bool {
        if ((state_ == State::OscString) ||
            (state_ == State::StString)) {
            if (IsSequenceCancellation(character) ||
                ((state_ == State::OscString) &&
                 (character == u8'\x07'))) {
                state_ = State::Text;
            } else if (character == u8'\x1b') {
                state_ = State::Escape;
            }
            return false;
        }
        if (character == u8'\x1b') {
            state_ = State::Escape;
            return false;
        }
        if (IsSequenceCancellation(character)) {
            state_ = State::Text;
            return false;
        }
        if (IsControl(character)) {
            return IsPlainTextControl(character);
        }

        switch (state_) {
        case State::Text:
            return true;

        case State::Escape:
            switch (character) {
            case u8'[':
                state_ = State::ControlSequence;
                break;
            case u8']':
                state_ = State::OscString;
                break;
            case u8'P':
            case u8'X':
            case u8'^':
            case u8'_':
                state_ = State::StString;
                break;
            default:
                if (character <= u8'/') {
                    state_ = State::EscapeIntermediate;
                } else if (character <= u8'~') {
                    state_ = State::Text;
                } else {
                    state_ = State::Text;
                    return true;
                }
            }
            return false;

        case State::EscapeIntermediate:
            if (character <= u8'/') {
                return false;
            }
            state_ = State::Text;
            return character > u8'~';

        case State::ControlSequence:
            if (character >= u8'@') {
                state_ = State::Text;
                return character > u8'~';
            }
            return false;

        default:
            std::unreachable();
        }
    }

    [[nodiscard]] static constexpr auto IsPlainTextControl(
        const char8_t character) noexcept -> bool {
        return (character == u8'\t') ||
               (character == u8'\n');
    }

    [[nodiscard]] static constexpr auto IsControl(
        const char8_t character) noexcept -> bool {
        return (character < u8' ') || (character == u8'\x7f');
    }

    [[nodiscard]] static constexpr auto IsSequenceCancellation(
        const char8_t character) noexcept -> bool {
        return (character == u8'\x18') || (character == u8'\x1a');
    }

    State state_ = State::Text;
};

[[nodiscard]] auto CopyConsoleOutput(
    wil::unique_handle output, const HANDLE log) noexcept {
    constexpr auto kBufferSize = DWORD{4096};
    auto buffer = std::array<char8_t, kBufferSize>{};
    auto first_error = DWORD{ERROR_SUCCESS};
    auto filter = VtFilter{};
    while (true) {
        auto read = DWORD{};
        if (!ReadFile(output.get(), buffer.data(), kBufferSize,
                &read, nullptr)) {
            const auto error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                return first_error == ERROR_SUCCESS ? error : first_error;
            }
        }
        if (read == 0) {
            return first_error;
        }
        WriteOutput(log,
            filter.Remove(std::span{buffer}.first(read)), first_error);
    }
}

} // namespace

export class LoggingConsole {
public:
    explicit LoggingConsole(const HANDLE log) {
        if (!CreatePipe(input_read_.addressof(),
                input_write_.addressof(), nullptr, 0)) {
            WinError("could not create the pseudoconsole input channel");
        }
        auto output_read = wil::unique_handle{};
        if (!CreatePipe(output_read.addressof(),
                output_write_.addressof(), nullptr, 0)) {
            WinError("could not create the pseudoconsole output channel");
        }
        const auto error = CreatePseudoConsole(
            COORD{1000, 30}, input_read_.get(), output_write_.get(),
            0, console_.put());
        if (FAILED(error)) {
            WinError("could not create the backup pseudoconsole",
                HRESULT_CODE(error));
        }
        output_task_ = std::async(
            std::launch::async, CopyConsoleOutput,
            std::move(output_read), log);
    }

    [[nodiscard]] auto StartProcess(
        const HANDLE job,
        const wil::zwstring_view application,
        std::wstring &command,
        const std::filesystem::path &working_directory) {
        auto attribute_bytes = SIZE_T{};
        InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_bytes);
        if ((attribute_bytes == 0) ||
            (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
            WinError("could not size the process attribute list");
        }
        auto attribute_storage = wil::unique_process_heap(
            HeapAlloc(GetProcessHeap(), 0, attribute_bytes));
        if (!attribute_storage) {
            WinError("could not allocate the process attribute list",
                ERROR_NOT_ENOUGH_MEMORY);
        }
        auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            attribute_storage.get());
        if (!InitializeProcThreadAttributeList(
                attributes, 2, 0, &attribute_bytes)) {
            WinError("could not initialize the process attribute list");
        }
        auto jobs = std::array{job};
        const auto delete_attributes = wil::scope_exit(
            [=] { DeleteProcThreadAttributeList(attributes); });
        if (!UpdateProcThreadAttribute(attributes, 0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                console_.get(), sizeof(HPCON), nullptr, nullptr)) {
            WinError("could not attach the child process to the pseudoconsole");
        }
        if (!UpdateProcThreadAttribute(attributes, 0,
                PROC_THREAD_ATTRIBUTE_JOB_LIST, jobs.data(),
                sizeof(jobs), nullptr, nullptr)) {
            WinError("could not assign the child process to its job");
        }

        auto startup = STARTUPINFOEXW{
            .StartupInfo = {.cb = sizeof(STARTUPINFOEXW)},
            .lpAttributeList = attributes,
        };
        auto process = wil::unique_process_information{};
        if (!CreateProcessW(application.c_str(), command.data(),
                nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                nullptr, working_directory.c_str(),
                &startup.StartupInfo, &process)) {
            WinError("could not start the backup orchestrator");
        }
        input_read_.reset();
        output_write_.reset();
        return process;
    }

    auto Finish() {
        input_read_.reset();
        input_write_.reset();
        output_write_.reset();
        console_.reset();
        const auto error = output_task_.get();
        if (error != ERROR_SUCCESS) {
            WinError("could not capture the backup pseudoconsole output", error);
        }
    }

private:
    // Destruction is in reverse order, so the reader remains active while
    // ClosePseudoConsole emits and closes its final output.
    std::future<DWORD> output_task_;
    unique_pseudoconsole console_;
    wil::unique_handle input_read_;
    wil::unique_handle input_write_;
    wil::unique_handle output_write_;
};
