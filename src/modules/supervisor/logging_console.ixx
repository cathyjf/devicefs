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

export module devicefs.supervisor.logging_console;

import std;
import <devicefs/windows_imports.h>;
import devicefs.common;
import devicefs.terminal.text;

using namespace std::string_view_literals;

using unique_pseudoconsole = wil::unique_any<
    HPCON, decltype(&::ClosePseudoConsole), ::ClosePseudoConsole>;

export class Log {
public:
    explicit Log(const std::filesystem::path &directory) {
        // Missing time-zone data should remove line timestamps and select the
        // fixed fallback filename, not prevent the backup from running.
        try {
            zone_ = std::chrono::current_zone();
        } catch (...) {}

        std::filesystem::create_directory(directory);
        const auto path = directory / LogFilename();
        file_.reset(CreateFileW(path.c_str(),
            GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file_) {
            WinError("could not open backup supervisor log '{}'",
                std::wstring_view{path.native()});
        }

        auto size = LARGE_INTEGER{};
        if (!GetFileSizeEx(file_.get(), &size)) {
            WinError("could not obtain the size of backup supervisor log '{}'",
                std::wstring_view{path.native()});
        }
        if (size.QuadPart != 0) {
            const auto offset = LARGE_INTEGER{.QuadPart = -1};
            if (!SetFilePointerEx(
                    file_.get(), offset, nullptr, FILE_END)) {
                WinError("could not seek in backup supervisor log '{}'",
                    std::wstring_view{path.native()});
            }
            auto last = char{};
            auto read = DWORD{};
            if (!ReadFile(file_.get(), &last, DWORD{sizeof(last)},
                    &read, nullptr)) {
                WinError("could not inspect backup supervisor log '{}'",
                    std::wstring_view{path.native()});
            }
            if (read != DWORD{sizeof(last)}) {
                throw std::runtime_error(std::format(
                    "the read from backup supervisor log '{}' was incomplete",
                    path.string()));
            }
            if (last != '\n') {
                WriteRaw(last == '\r'
                    ? "\n"sv
                    : "\r\n"sv);
            }
        }
    }

    template <typename... Arguments>
    auto Write(const std::format_string<Arguments...> format,
        Arguments&&... arguments) -> void {
        WriteLine(std::format(
            format, std::forward<Arguments>(arguments)...));
    }

    template <typename... Arguments>
    auto TryWrite(const std::format_string<Arguments...> format,
        Arguments&&... arguments) noexcept -> void {
        try {
            Write(format, std::forward<Arguments>(arguments)...);
        } catch (...) {}
    }

    [[nodiscard]] auto WriteStream(
        const std::u8string_view output) noexcept -> DWORD {
        try {
            const auto lock = lock_.lock_exclusive();
            WriteText(output);
            return ERROR_SUCCESS;
        } catch (const std::bad_alloc &) {
            return ERROR_NOT_ENOUGH_MEMORY;
        } catch (const std::system_error &error) {
            if (error.code().category() == std::system_category()) {
                return std::bit_cast<DWORD>(error.code().value());
            }
            return ERROR_WRITE_FAULT;
        } catch (const std::exception &) {
            return ERROR_INVALID_DATA;
        } catch (...) {
            // The result reaches LoggingConsole::Finish. The reader must keep
            // draining after any failure so a full ConPTY pipe cannot deadlock.
            return ERROR_UNHANDLED_EXCEPTION;
        }
    }

    auto Flush() -> void {
        const auto lock = lock_.lock_exclusive();
        if (!FlushFileBuffers(file_.get())) {
            WinError("could not flush the backup supervisor log");
        }
    }

    auto TryFlush() noexcept -> void {
        try {
            Flush();
        } catch (...) {}
    }

private:
    [[nodiscard]] auto LogFilename() const -> std::wstring {
        if (zone_ != nullptr) {
            try {
                const auto now = std::chrono::floor<std::chrono::seconds>(
                    std::chrono::system_clock::now());
                return std::format(L"{:%F}-backup.log",
                    std::chrono::zoned_seconds{zone_, now});
            } catch (...) {}
        }
        return L"backup.log";
    }

    template <typename Character>
    auto WriteRaw(const std::basic_string_view<Character> output) -> void {
        static_assert(sizeof(Character) == 1);
        if (output.empty()) {
            return;
        }
        // Current diagnostics and ConPTY chunks are far below MAXDWORD, but WriteRaw
        // accepts an arbitrary string view, so reject a length WriteFile cannot represent.
        const auto size = wil::safe_cast<DWORD>(output.size());
        auto written = DWORD{};
        if (!WriteFile(file_.get(), output.data(),
                size, &written, nullptr)) {
            WinError("could not write the backup supervisor log");
        }
        if (written != size) {
            WinError("could not write the complete backup supervisor log",
                ExplicitWin32Error{ERROR_WRITE_FAULT});
        }
    }

    [[nodiscard]] auto Timestamp() const noexcept -> std::string {
        try {
            if (zone_ == nullptr) {
                return {};
            }
            const auto now = std::chrono::floor<std::chrono::seconds>(
                std::chrono::system_clock::now());
            return std::format("[{:%a, %d %b %Y %T %z}] ",
                std::chrono::zoned_seconds{zone_, now});
        } catch (...) {
            // Timestamp metadata is never worth failing the backup.
            return {};
        }
    }

    template <typename Character>
    auto WriteText(std::basic_string_view<Character> output) -> void {
        while (!output.empty()) {
            if (output.front() == Character{'\r'}) {
                output.remove_prefix(1);
                continue;
            }
            if (at_line_start_) {
                const auto timestamp = Timestamp();
                WriteRaw(std::string_view(timestamp));
                at_line_start_ = false;
            }
            constexpr auto line_ends =
                std::array{Character{'\r'}, Character{'\n'}};
            const auto line_end = output.find_first_of(
                std::basic_string_view{line_ends});
            if (line_end == output.npos) {
                WriteRaw(output);
                return;
            }
            WriteRaw(output.substr(0, line_end));
            if (output[line_end] == Character{'\n'}) {
                WriteRaw("\r\n"sv);
                at_line_start_ = true;
            }
            output.remove_prefix(line_end + 1);
        }
    }

    auto WriteLine(const std::string_view message) -> void {
        const auto lock = lock_.lock_exclusive();
        if (!at_line_start_) {
            WriteRaw("\r\n"sv);
            at_line_start_ = true;
        }
        WriteText(message);
        if (message.empty() || (message.back() != '\n')) {
            WriteText("\n"sv);
        }
    }

    wil::unique_hfile file_;
    const std::chrono::time_zone *zone_ = nullptr;
    wil::srwlock lock_;
    bool at_line_start_ = true;
};

[[nodiscard]] auto CopyConsoleOutput(
    wil::unique_handle output, Log &log) noexcept {
    constexpr auto kBufferSize = DWORD{4096};
    auto buffer = std::array<char8_t, kBufferSize>{};
    auto first_error = DWORD{ERROR_SUCCESS};
    auto filter = devicefs::terminal::VtFilter{};
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
        if (first_error == ERROR_SUCCESS) {
            first_error = log.WriteStream(
                filter.Remove(std::span{buffer}.first(read)));
        }
    }
}

export class LoggingConsole {
public:
    explicit LoggingConsole(Log &log) {
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
                ExplicitWin32Error::FromHresult(error));
        }
        output_task_ = std::async(
            std::launch::async, CopyConsoleOutput,
            std::move(output_read), std::ref(log));
    }

    [[nodiscard]] auto StartProcess(
        const HANDLE job,
        const wil::zstring_view application,
        std::string &command) {
        constexpr auto kAttributeCount = DWORD{2};
        auto attribute_bytes = SIZE_T{};
        InitializeProcThreadAttributeList(
            nullptr, kAttributeCount, 0, &attribute_bytes);
        if ((attribute_bytes == 0) ||
            (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
            WinError("could not size the process attribute list");
        }
        const auto attribute_storage = [&] {
            try {
                return std::make_unique_for_overwrite<std::byte[]>(
                    attribute_bytes);
            } catch (const std::bad_alloc &) {
                WinError("could not allocate the process attribute list",
                    ExplicitWin32Error{ERROR_NOT_ENOUGH_MEMORY});
            }
        }();
        void *const raw_attributes = attribute_storage.get();
        auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            raw_attributes);
        if (!InitializeProcThreadAttributeList(
                attributes, kAttributeCount, 0, &attribute_bytes)) {
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
        auto startup = STARTUPINFOEXA{
            .StartupInfo = {.cb = sizeof(STARTUPINFOEXA)},
            .lpAttributeList = attributes,
        };
        auto process = wil::unique_process_information{};
        if (!CreateProcessA(application.c_str(), command.data(),
                nullptr, nullptr, FALSE,
                CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                nullptr, nullptr,
                &startup.StartupInfo, &process)) {
            WinError("could not start backup orchestrator '{}'",
                std::string_view{application});
        }
        input_read_.reset();
        output_write_.reset();
        return process;
    }

    auto Finish() {
        input_write_.reset();
        console_.reset();
        const auto error = output_task_.get();
        if (error != ERROR_SUCCESS) {
            WinError("could not capture the backup pseudoconsole output",
                ExplicitWin32Error{error});
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
