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
import devicefs.terminal.menu;

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

    // A menu update measures text and draws its replacement frame. Synchronized
    // output asks Terminal to retain the previous display during that work;
    // `PresentFrame` releases the rendering hold when drawing is complete.
    // The guard also releases the hold if an operation throws. Windows Terminal
    // limits each hold to 100 ms, so a slower update can become visible early.
    // https://github.com/microsoft/terminal/blob/5a830b2bf7c053d5c7ac22208fe5a346cb5dd3dc/src/renderer/base/renderer.cpp#L191-L257
    [[nodiscard]] auto BeginUpdate() {
        auto finish = wil::scope_exit([this] {
            constexpr auto sequence = L"\x1b[?2026l"sv;
            auto written = DWORD{};
            std::ignore = WriteConsoleW(output_.get(), sequence.data(),
                wil::safe_cast_failfast<DWORD>(sequence.size()), &written, nullptr);
        });
        Write("\x1b[?2026h"sv);
        return finish;
    }

    auto PresentFrame() -> void {
        Write("\x1b[?2026l"sv);
    }

    // The alternate screen gives the menu a viewport without scrollback and
    // preserves the caller's screen for restoration. Terminal crops or extends
    // this screen during resize, leaving the menu to lay out its new frame.
    // This owner outlives frame updates and restores the caller's screen and
    // cursor on selection, cancellation, or an exception.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#alternate-screen-buffer
    [[nodiscard]] auto EnterMenu() {
        auto previous_cursor = CONSOLE_CURSOR_INFO{};
        if (!GetConsoleCursorInfo(output_.get(), &previous_cursor)) {
            throw std::system_error(std::bit_cast<int>(GetLastError()),
                std::system_category(), "could not read the console cursor visibility");
        }
        auto restore = wil::scope_exit([handle = output_.get(), previous_cursor] {
            constexpr auto sequence = L"\x1b[?1049l"sv;
            const auto length = wil::safe_cast_failfast<DWORD>(sequence.size());
            auto written = DWORD{};
            std::ignore = WriteConsoleW(
                handle, sequence.data(), length, &written, nullptr);
            std::ignore = SetConsoleCursorInfo(handle, &previous_cursor);
        });
        Write("\x1b[?1049h\x1b[?25l"sv);
        return restore;
    }

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
        const auto reply = Query("\x1b[6n"sv, "\x1b["sv, "R"sv, 2);
        if (!reply || ((*reply)[0] < 1) || ((*reply)[1] < 1)) {
            return std::nullopt;
        }
        return CursorPosition{.row = (*reply)[0], .column = (*reply)[1]};
    }

    [[nodiscard]] auto QuerySize() -> std::optional<TerminalSize> {
        const auto reply = Query("\x1b[18t"sv, "\x1b["sv, "t"sv, 3);
        if (!reply || ((*reply)[0] != 8) || ((*reply)[1] < 1) || ((*reply)[2] < 1)) {
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

    [[nodiscard]] auto ReadMenuInput() -> MenuInput {
        for (;;) {
            const auto record = ReadInput();
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                return {.key = MenuKey::Resize};
            }
            if ((record.EventType != KEY_EVENT) || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto &key = record.Event.KeyEvent;
            const auto action = [&key]() -> std::optional<MenuKey> {
                switch (key.wVirtualKeyCode) {
                case VK_UP:
                    return MenuKey::Up;
                case VK_DOWN:
                    return MenuKey::Down;
                case VK_PRIOR:
                    return MenuKey::PageUp;
                case VK_NEXT:
                    return MenuKey::PageDown;
                case VK_HOME:
                    return MenuKey::Home;
                case VK_END:
                    return MenuKey::End;
                case VK_RETURN:
                    return MenuKey::Accept;
                case VK_ESCAPE:
                    return MenuKey::Back;
                default:
                    break;
                }
                // Ctrl+C should cancel the menu instead of terminating the
                // process. With processed input disabled, Ctrl+C arrives as
                // ETX (0x03). Ctrl+L is form feed (0x0c) and requests a redraw.
                switch (key.uChar.UnicodeChar) {
                case L'\x03':
                    return MenuKey::Cancel;
                case L'\x0c':
                    return MenuKey::Redraw;
                case L'1':
                    return MenuKey::Details;
                default:
                    return std::nullopt;
                }
            }();
            if (action) {
                return {.key = *action, .repeat = key.wRepeatCount};
            }
        }
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

    // A VT query asks the displaying terminal to report its cursor position,
    // screen size, or page-coupling mode. The reply arrives alongside user
    // input. This operation recognizes the requested report and saves the other
    // events for `ReadInput`, preserving their arrival order.
    [[nodiscard]] auto Query(const std::string_view request,
        const std::string_view prefix, const std::string_view suffix,
        const std::size_t fields)
        -> std::optional<std::array<int, 3>> {
        // Some terminals omit unsupported reports. A deadline lets the caller
        // regain control in that case. The timeout covers the whole query,
        // including time spent receiving keyboard input while awaiting a reply.
        constexpr auto kReplyTimeout = 5s;
        // The reports contain at most three nonnegative decimal integers.
        // Thirty digits, two separators, a three-byte introducer, and a two-byte
        // terminator cover the largest report accepted by these queries.
        constexpr auto kMaximumReplyLength = 37uz;
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
            if (reply.size() <= prefix.size()) {
                if (!prefix.starts_with(reply)) {
                    discard_candidate();
                }
                continue;
            }
            if (!((character >= L'0') && (character <= L'9')) &&
                (character != L';') && !suffix.contains(reply.back())) {
                discard_candidate();
                continue;
            }
            if (!reply.ends_with(suffix)) {
                continue;
            }
            auto values = std::array<int, 3>{};
            auto body = std::string_view{reply}.substr(
                prefix.size(), reply.size() - prefix.size() - suffix.size());
            auto valid = true;
            for (auto index = 0uz; index < fields; ++index) {
                const auto [end, error] = std::from_chars(
                    body.data(), body.data() + body.size(), values.at(index));
                if ((error != std::errc{}) || (values.at(index) < 0)) {
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
