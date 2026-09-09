// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "devicefs/strsafe_compat.h"

export module devicefs.terminal.windows;

import std;
import <windows.h>;
import <wil/resource.h>;
import <wil/safecast.h>;
import <wil/stl.h>;
import devicefs.terminal;

using namespace std::string_view_literals;
using namespace std::chrono_literals;
using namespace wil::literals;

namespace devicefs::terminal::detail {

[[nodiscard]] auto SetConsoleModeScoped(
    const HANDLE handle, const DWORD enable, const DWORD disable) {
    auto previous = DWORD{};
    if (!GetConsoleMode(handle, &previous)) {
        throw std::system_error(std::bit_cast<int>(GetLastError()),
            std::system_category(), "could not read the terminal's console mode");
    }
    auto restore = wil::scope_exit([handle, previous] {
        std::ignore = SetConsoleMode(handle, previous);
    });
    if (!SetConsoleMode(handle, (previous | enable) & ~disable)) {
        throw std::system_error(std::bit_cast<int>(GetLastError()),
            std::system_category(), "could not enable terminal input or output");
    }
    return restore;
}

[[nodiscard]] auto OpenConsole(const wil::zstring_view name) {
    auto handle = wil::unique_hfile{CreateFileA(name.c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr)};
    if (!handle) {
        const auto error = GetLastError();
        throw std::system_error(std::bit_cast<int>(error),
            std::system_category(), std::format("could not open '{}'",
                std::string_view{name}));
    }
    return handle;
}

}

export namespace devicefs::terminal {

// `WindowsConsole` supplies terminal output, cursor and size queries, and input
// events for the Windows console attached to this process. Cursor positions and
// dimensions come from VT replies, so layout uses the displaying terminal's
// reports. Keyboard and resize events are available through `ReadInput`.
//
// The object opens the console handles and enables the modes needed for these
// operations. Destruction restores the previous modes before closing the handles.
// A query returns an empty optional when a usable report is unavailable within
// its timeout. Failures to open, configure, read, or write the console throw
// exceptions.
class WindowsConsole {
public:
    WindowsConsole() = default;

    auto Write(const std::string_view text) -> void {
        if (text.empty()) {
            return;
        }
        // `Write` accepts UTF-8 regardless of the console's output code page.
        // `WriteConsoleW` accepts UTF-16 directly, so this conversion lets the
        // caller display Unicode text while preserving the console code page
        // shared with other programs. Microsoft's WriteConsole documentation
        // describes the separate encodings accepted by the A and W functions:
        // https://learn.microsoft.com/en-us/windows/console/writeconsole
        const auto wide = std::filesystem::path{text}.wstring();
        const auto length = wil::safe_cast<DWORD>(wide.size());
        auto written = DWORD{};
        if (!WriteConsoleW(output_.get(), wide.data(), length,
                &written, nullptr)) {
            throw std::system_error(std::bit_cast<int>(GetLastError()),
                std::system_category(), "could not write terminal text");
        }
        if (written != length) {
            throw std::runtime_error(std::format(
                "the terminal accepted {} of {} UTF-16 code units",
                written, length));
        }
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        const auto reply = Query("\x1b[6n"sv, 'R', 2);
        if (!reply) {
            return std::nullopt;
        }
        return CursorPosition{.row = (*reply)[0], .column = (*reply)[1]};
    }

    [[nodiscard]] auto QuerySize() -> std::optional<TerminalSize> {
        const auto reply = Query("\x1b[18t"sv, 't', 3);
        if (!reply || ((*reply)[0] != 8)) {
            return std::nullopt;
        }
        return TerminalSize{.rows = (*reply)[1], .columns = (*reply)[2]};
    }

    // `ReadInput` supplies the events used to operate the interface. Queries may
    // have encountered keyboard or resize events while waiting for a report, so
    // those saved events are delivered before reading more from the console.
    [[nodiscard]] auto ReadInput() -> INPUT_RECORD {
        if (!pending_.empty()) {
            const auto record = pending_.front();
            pending_.pop_front();
            return record;
        }
        return ReadConsoleRecord();
    }

private:
    [[nodiscard]] auto ReadConsoleRecord() const -> INPUT_RECORD {
        auto record = INPUT_RECORD{};
        auto read = DWORD{};
        if (!ReadConsoleInputW(input_.get(), &record, 1, &read)) {
            throw std::system_error(std::bit_cast<int>(GetLastError()),
                std::system_category(), "could not read terminal input");
        }
        if (read != 1) {
            throw std::runtime_error("the terminal returned no input record");
        }
        return record;
    }

    // A VT query asks the displaying terminal to report its cursor position or
    // screen size. The reply arrives in the console input queue alongside user
    // input. This operation recognizes the requested report and saves the other
    // events for `ReadInput`, preserving their arrival order.
    [[nodiscard]] auto Query(const std::string_view request,
        const char terminator, const std::size_t fields)
        -> std::optional<std::array<int, 3>> {
        // Some terminals omit unsupported reports. A deadline lets the caller
        // regain control in that case. The timeout covers the whole query,
        // including time spent receiving keyboard input while awaiting a reply.
        constexpr auto kReplyTimeout = 5s;
        // Cursor and size reports contain two or three positive decimal
        // integers. Three 32-bit integers need at most 30 digits; adding two
        // separators, ESC and '[', and the final command letter gives the
        // longest report accepted by these queries.
        constexpr auto kMaximumReplyLength = 35uz;
        auto records = std::vector<std::list<INPUT_RECORD>::iterator>{};
        records.reserve(kMaximumReplyLength);
        auto reply = std::string{};
        reply.reserve(kMaximumReplyLength);
        const auto discard_candidate = [&] {
            records.clear();
            reply.clear();
        };
        const auto deadline = std::chrono::steady_clock::now() + kReplyTimeout;
        Write(request);
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= remaining.zero()) {
                return std::nullopt;
            }
            const auto wait = WaitForSingleObject(input_.get(),
                wil::safe_cast<DWORD>(
                    std::chrono::ceil<std::chrono::milliseconds>(remaining).count()));
            if (wait == WAIT_TIMEOUT) {
                continue;
            }
            if (wait == WAIT_FAILED) {
                throw std::system_error(std::bit_cast<int>(GetLastError()),
                    std::system_category(), "could not wait for a terminal reply");
            }
            // An escape character can begin either a report or user input.
            // Each event is saved until a complete report has been recognized;
            // its events can then be removed from `pending_`. Partial matches
            // stay available to `ReadInput` if the query ends before recognition.
            const auto position = pending_.insert(pending_.end(), ReadConsoleRecord());
            const auto &record = *position;
            if ((record.EventType != KEY_EVENT) ||
                !record.Event.KeyEvent.bKeyDown ||
                (record.Event.KeyEvent.wRepeatCount != 1)) {
                continue;
            }
            const auto character = record.Event.KeyEvent.uChar.UnicodeChar;
            if (character == L'\0') {
                continue;
            }
            if (character == L'\x1b') {
                discard_candidate();
                records.push_back(position);
                reply = "\x1b";
                continue;
            }
            if (reply.empty()) {
                continue;
            }
            if ((character > 0x7f) || (reply.size() == kMaximumReplyLength)) {
                discard_candidate();
                continue;
            }
            records.push_back(position);
            reply.push_back(wil::safe_cast_failfast<char>(character));
            if (((reply.size() == 2) && (character != L'[')) ||
                ((reply.size() > 2) &&
                 !((character >= L'0') && (character <= L'9')) &&
                 (character != L';') && (character != terminator))) {
                discard_candidate();
                continue;
            }
            if (character != terminator) {
                continue;
            }
            auto values = std::array<int, 3>{};
            auto body = std::string_view{reply}.substr(2, reply.size() - 3);
            auto valid = true;
            for (auto index = 0uz; index < fields; ++index) {
                const auto [end, error] = std::from_chars(
                    body.data(), body.data() + body.size(), values.at(index));
                if ((error != std::errc{}) || (values.at(index) <= 0)) {
                    valid = false;
                    break;
                }
                body.remove_prefix(wil::safe_cast_failfast<std::size_t>(
                    end - body.data()));
                if ((index + 1) < fields) {
                    if (!body.starts_with(';')) {
                        valid = false;
                        break;
                    }
                    body.remove_prefix(1);
                }
            }
            if (valid && body.empty()) {
                for (const auto &position_to_erase : records) {
                    pending_.erase(position_to_erase);
                }
                return values;
            }
            discard_candidate();
        }
    }

    // Restoring console modes requires open handles. Members are destroyed in
    // reverse declaration order, so the handles precede their mode guards here.
    wil::unique_hfile input_ = detail::OpenConsole("CONIN$"_zv);
    wil::unique_hfile output_ = detail::OpenConsole("CONOUT$"_zv);
    // Keyboard input is delivered as structured events, including arrow keys,
    // by leaving `ENABLE_VIRTUAL_TERMINAL_INPUT` disabled. Microsoft documents
    // that terminal query replies enter the input buffer in either mode, so the
    // same input handle also receives the cursor and size reports used here.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) input_mode_ =
        detail::SetConsoleModeScoped(input_.get(), ENABLE_WINDOW_INPUT,
            ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                ENABLE_VIRTUAL_TERMINAL_INPUT);
    decltype(detail::SetConsoleModeScoped(nullptr, 0, 0)) output_mode_ =
        detail::SetConsoleModeScoped(output_.get(),
            ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT |
                ENABLE_VIRTUAL_TERMINAL_PROCESSING,
            0);
    std::list<INPUT_RECORD> pending_;
};

}
