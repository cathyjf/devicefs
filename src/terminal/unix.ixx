// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

export module devicefs.terminal.unix;

import std;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.scope_exit;

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace devicefs::terminal::unix_detail {

constexpr auto kReplyTimeout = 5s;
constexpr auto kInputPollInterval = 100ms;
constexpr auto kEscapeTimeout = 100ms;
constexpr auto kBeginUpdate = "\x1b[?2026h"sv;
constexpr auto kEndUpdate = "\x1b[?2026l"sv;
constexpr auto kEnterMenu = "\x1b[?1049h\x1b[?25s\x1b[?25l"sv;
constexpr auto kLeaveMenu = "\x1b[0m\x1b[?25h\x1b[?25r\x1b[?1049l"sv;

[[nodiscard]] auto OpenTerminal() -> int {
    const auto descriptor = open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not open the controlling terminal '/dev/tty'");
    }
    return descriptor;
}

[[nodiscard]] auto CloseTerminalScoped(const int descriptor) {
    return ScopeExit{[descriptor] { std::ignore = close(descriptor); }};
}

// Raw input delivers keys and terminal replies immediately, including Ctrl+C
// as an input byte that the menu can interpret as cancellation. Output retains
// newline-to-CRLF translation so ordinary line feeds start a new display row.
// The returned guard restores the caller's complete terminal configuration.
[[nodiscard]] auto SetTerminalModeScoped(const int descriptor) {
    auto previous = termios{};
    if (tcgetattr(descriptor, &previous) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not read the controlling terminal's modes");
    }
    auto restore = ScopeExit{[descriptor, previous] {
        std::ignore = tcsetattr(descriptor, TCSANOW, &previous);
    }};
    auto current = previous;
    cfmakeraw(&current);
    current.c_oflag = OPOST | ONLCR;
    if (tcsetattr(descriptor, TCSANOW, &current) < 0) {
        throw std::system_error(errno, std::generic_category(),
            "could not enable terminal byte input and output");
    }
    return restore;
}

// A terminal write may accept only a prefix or be interrupted by a signal.
// Continue from the accepted prefix so both drawing and screen restoration use
// the same completion policy. Cleanup callers can discard an error; ordinary
// output callers report the error with the operation they were performing.
[[nodiscard]] auto WriteTerminal(const int descriptor, std::string_view text)
    noexcept -> std::error_code {
    constexpr auto kMaximumWrite =
        CompileTimeCast<std::size_t, std::numeric_limits<ssize_t>::max()>();
    while (!text.empty()) {
        const auto written = write(descriptor, text.data(),
            std::min(text.size(), kMaximumWrite));
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return {errno, std::generic_category()};
        }
        if (written == 0) {
            return std::make_error_code(std::errc::io_error);
        }
        text.remove_prefix(FailFastCast<std::size_t>(written));
    }
    return {};
}

// Navigation keys use CSI sequences, or SS3 sequences in application-cursor
// mode. The final letter identifies an arrow, Home, or End; numeric tilde forms
// identify the remaining navigation keys. Other complete sequences are ignored.
// https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
[[nodiscard]] auto NavigationKey(const std::string_view sequence)
    -> std::optional<MenuKey> {
    switch (sequence.back()) {
    case 'A':
        return MenuKey::Up;
    case 'B':
        return MenuKey::Down;
    case 'H':
        return MenuKey::Home;
    case 'F':
        return MenuKey::End;
    case '~': {
        const auto parameter = sequence.substr(2, sequence.size() - 3);
        if ((parameter == "1"sv) || (parameter == "7"sv)) {
            return MenuKey::Home;
        }
        if ((parameter == "4"sv) || (parameter == "8"sv)) {
            return MenuKey::End;
        }
        if (parameter == "5"sv) {
            return MenuKey::PageUp;
        }
        if (parameter == "6"sv) {
            return MenuKey::PageDown;
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

}

export namespace devicefs::terminal {

// UnixConsole connects the text writer and menu to the process's controlling
// terminal. The adapter writes UTF-8, receives cursor and size reports through
// VT, and translates keyboard sequences into menu operations. Opening /dev/tty
// keeps this connection independent of redirected standard streams.
//
// The console owns its descriptor and temporary termios settings. Menu and
// update guards must be destroyed before the console; those guards restore the
// screen, while console destruction restores termios and closes the descriptor.
// Unavailable reports return an empty optional. Terminal I/O failures throw.
class UnixConsole {
public:
    UnixConsole() = default;
    UnixConsole(const UnixConsole &) = delete;
    auto operator=(const UnixConsole &) -> UnixConsole & = delete;

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto Flip(const FrameBuffer &frame) -> bool {
        return presenter_.Flip<Policy>(*this, frame);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto KnownTextWidths(const std::string_view text) const {
        return presenter_.KnownTextWidths<Policy>(text);
    }

    auto InvalidateFrameRows(const int first_row, const int count) -> void {
        presenter_.InvalidateRows(first_row, count);
    }

    auto InvalidateFrame() -> void {
        presenter_.Invalidate();
    }

    // Synchronized output lets supporting terminals display a completed update
    // together. The guard releases the hold even when layout or drawing fails.
    [[nodiscard]] auto BeginUpdate() {
        auto finish = ScopeExit{[descriptor = descriptor_] {
            std::ignore = unix_detail::WriteTerminal(
                descriptor, unix_detail::kEndUpdate);
        }};
        Write(unix_detail::kBeginUpdate);
        return finish;
    }

    auto PresentFrame() -> void {
        Write(unix_detail::kEndUpdate);
    }

    // The alternate screen preserves the caller's display while the menu runs.
    // On terminals supporting XTSAVE/XTRESTORE, saving private mode 25 also
    // preserves cursor visibility. Restoration first shows the cursor, then
    // restores the saved setting; terminals that ignore those extensions are
    // therefore left with a visible cursor when the menu ends.
    // https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html
    [[nodiscard]] auto EnterMenu() {
        presenter_ = DeltaFramePresenter{};
        auto restore = ScopeExit{[descriptor = descriptor_] {
            std::ignore = unix_detail::WriteTerminal(
                descriptor, unix_detail::kLeaveMenu);
        }};
        Write(unix_detail::kEnterMenu);
        return restore;
    }

    auto Write(const std::string_view text) -> void {
        if (const auto error = unix_detail::WriteTerminal(descriptor_, text)) {
            throw std::system_error(error,
                "could not write text to the controlling terminal");
        }
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        const auto reply = Query(detail::TerminalReport::Cursor);
        if (!reply || ((*reply)[0] < 1) || ((*reply)[1] < 1)) {
            return std::nullopt;
        }
        return CursorPosition{.row = (*reply)[0], .column = (*reply)[1]};
    }

    [[nodiscard]] auto QuerySize() -> std::optional<TerminalSize> {
        const auto reply = Query(detail::TerminalReport::Size);
        if (!reply || ((*reply)[0] != 8) || ((*reply)[1] < 1) || ((*reply)[2] < 1)) {
            return std::nullopt;
        }
        return TerminalSize{.rows = (*reply)[1], .columns = (*reply)[2]};
    }

    [[nodiscard]] auto ReadMenuInput() -> MenuInput {
        for (;;) {
            // The driver records window-size changes even while no key is
            // pressed. Checking that record between bounded input waits lets
            // the menu notice resizes without installing a process-wide signal
            // handler. The next frame obtains its dimensions through VT.
            const auto size = ReadWindowSize();
            if ((size.rows != window_size_.rows) || (size.columns != window_size_.columns)) {
                window_size_ = size;
                return {.key = MenuKey::Resize};
            }
            if (pending_.empty()) {
                std::ignore = ReceiveUntil(std::chrono::steady_clock::now() +
                    unix_detail::kInputPollInterval);
                continue;
            }
            if (pending_.front() == '\x1b') {
                if (const auto action = ReadEscape()) {
                    return {.key = *action};
                }
                continue;
            }
            const auto character = pending_.front();
            pending_.erase(0, 1);
            switch (character) {
            case '\r':
            case '\n':
                return {.key = MenuKey::Accept};
            case '\x03':
                return {.key = MenuKey::Cancel};
            case '\x0c':
                return {.key = MenuKey::Redraw};
            case '1':
                return {.key = MenuKey::Details};
            default:
                break;
            }
        }
    }

private:
    [[nodiscard]] auto ReadWindowSize() const -> TerminalSize {
        auto size = winsize{};
        if (ioctl(descriptor_, TIOCGWINSZ, &size) < 0) {
            throw std::system_error(errno, std::generic_category(),
                "could not read the controlling terminal's window size");
        }
        return {.rows = size.ws_row, .columns = size.ws_col};
    }

    // Terminal reports and keystrokes share one byte stream. This is its only
    // reader; every received byte enters `pending_` before either consumer
    // interprets it. Interrupted waits retry against the original deadline.
    [[nodiscard]] auto ReceiveUntil(const std::chrono::steady_clock::time_point deadline)
        -> bool {
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= remaining.zero()) {
                return false;
            }
            auto readiness = pollfd{.fd = descriptor_, .events = POLLIN, .revents = 0};
            const auto result = poll(&readiness, 1,
                std::chrono::ceil<std::chrono::duration<int, std::milli>>(
                    remaining).count());
            if (result < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                    "could not wait for controlling-terminal input");
            }
            if (result == 0) {
                return false;
            }
            auto bytes = std::array<char, 4096>{};
            const auto count = read(descriptor_, bytes.data(), bytes.size());
            if (count < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw std::system_error(errno, std::generic_category(),
                    "could not read controlling-terminal input");
            }
            if (count == 0) {
                throw std::runtime_error("the controlling terminal closed its input");
            }
            pending_.append(bytes.data(), FailFastCast<std::size_t>(count));
            return true;
        }
    }

    // A query consumes only its matching report. Bytes typed while the reply is
    // arriving remain in `pending_` for menu input. Searching from the old end
    // avoids accepting a leftover report from an earlier, timed-out query.
    [[nodiscard]] auto Query(const detail::TerminalReport report)
        -> std::optional<std::array<int, 3>> {
        auto search_from = pending_.size();
        const auto deadline = std::chrono::steady_clock::now() + unix_detail::kReplyTimeout;
        Write(detail::ReportRequest(report));
        while (ReceiveUntil(deadline)) {
            for (;;) {
                const auto start = pending_.find(detail::kReportPrefix, search_from);
                if (start == std::string::npos) {
                    search_from = pending_.size() - 1;
                    break;
                }
                const auto end = pending_.find(detail::ReportSuffix(report), start + 2);
                if ((end != std::string::npos) &&
                    ((end - start + 1) <= detail::kMaximumReportLength)) {
                    const auto text = std::string_view{pending_}.substr(start, end - start + 1);
                    if (const auto values = detail::ParseTerminalReport(text, report)) {
                        pending_.erase(start, text.size());
                        return values;
                    }
                    search_from = start + 1;
                    continue;
                }
                if (const auto next = pending_.find(detail::kReportPrefix, start + 2);
                    next != std::string::npos) {
                    search_from = next;
                    continue;
                }
                if ((pending_.size() - start) < detail::kMaximumReportLength) {
                    // A report can be divided across reads. Keep its candidate
                    // prefix until enough bytes arrive to accept or reject it.
                    search_from = start;
                    break;
                }
                search_from = start + 1;
            }
        }
        return std::nullopt;
    }

    // Escape is both a key and the first byte of navigation sequences. A short
    // wait admits a fragmented sequence; a lone Escape becomes Back when that
    // wait expires. Other keys already in the input queue retain their order.
    [[nodiscard]] auto ReadEscape() -> std::optional<MenuKey> {
        const auto deadline = std::chrono::steady_clock::now() + unix_detail::kEscapeTimeout;
        for (;;) {
            if (pending_.size() > 1) {
                if ((pending_[1] != '[') && (pending_[1] != 'O')) {
                    pending_.erase(0, 1);
                    return MenuKey::Back;
                }
                // CSI and SS3 end with a byte in the range '@' through '~'.
                const auto body = std::string_view{pending_}.substr(2);
                const auto final = std::ranges::find_if(
                    body, [](const char value) {
                        return (value >= '@') && (value <= '~');
                    });
                if (final != body.end()) {
                    const auto length = 3 + FailFastCast<std::size_t>(final - body.begin());
                    const auto action = unix_detail::NavigationKey(
                        std::string_view{pending_}.substr(0, length));
                    pending_.erase(0, length);
                    return action;
                }
            }
            if (!ReceiveUntil(deadline)) {
                pending_.erase(0, 1);
                return MenuKey::Back;
            }
        }
    }

    // Destruction restores terminal settings before closing their descriptor.
    const int descriptor_ = unix_detail::OpenTerminal();
    decltype(unix_detail::CloseTerminalScoped(0)) connection_ =
        unix_detail::CloseTerminalScoped(descriptor_);
    decltype(unix_detail::SetTerminalModeScoped(0)) modes_ =
        unix_detail::SetTerminalModeScoped(descriptor_);
    TerminalSize window_size_ = ReadWindowSize();
    std::string pending_;
    DeltaFramePresenter presenter_;
};

}
