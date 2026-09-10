// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <cerrno>
#include <fcntl.h>
#ifdef __APPLE__
    #include <sys/select.h>
#else
    #include <poll.h>
#endif
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

export module devicefs.terminal.unix;

import std;
import devicefs.terminal;
import devicefs.terminal.base_console;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.safecast;
import devicefs.terminal.scope_exit;

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace devicefs::terminal::unix_detail {

constexpr auto kInputPollInterval = 100ms;
constexpr auto kEscapeTimeout = 30ms;

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
// Unavailable reports return an empty optional. Terminal I/O failures throw;
// Ctrl+C during a query raises InputCancelled.
class UnixConsole : public BaseConsole {
public:
    UnixConsole() = default;
    UnixConsole(const UnixConsole &) = delete;
    auto operator=(const UnixConsole &) -> UnixConsole & = delete;

    auto Write(const std::string_view text) -> void {
        if (const auto error = unix_detail::WriteTerminal(descriptor_, text)) {
            throw std::system_error(error,
                "could not write text to the controlling terminal");
        }
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
            if ((character == '\r') || (character == '\n')) {
                return {.key = MenuKey::Accept};
            }
            if (const auto action = CharacterMenuKey(std::bit_cast<unsigned char>(character))) {
                return {.key = *action};
            }
        }
    }

private:
    friend class BaseConsole;
    static constexpr auto kEnterScreen = "\x1b[?1049h\x1b[?25s\x1b[?25l"sv;
    static constexpr auto kLeaveScreen = "\x1b[0m\x1b[?25h\x1b[?25r\x1b[?1049l"sv;

protected:
    auto WriteControlSequenceNoThrow(const std::string_view sequence) const noexcept -> void {
        std::ignore = unix_detail::WriteTerminal(descriptor_, sequence);
    }

private:
    // Saving private mode 25 on entry preserves cursor visibility on terminals
    // supporting `XTSAVE`/`XTRESTORE`. Restoration first shows the cursor, then
    // restores the saved setting; terminals that ignore those extensions are
    // therefore left with a visible cursor when the interactive session ends.
    // https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html
    [[nodiscard]] auto RestoreScreenOnExit() {
        return ScopeExit{[this] { WriteControlSequenceNoThrow(kLeaveScreen); }};
    }

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
#ifdef __APPLE__
        if (descriptor_ >= FD_SETSIZE) {
            throw std::runtime_error(std::format(
                "controlling-terminal descriptor {} exceeds pselect's maximum descriptor {}",
                descriptor_, FD_SETSIZE - 1));
        }
#endif
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= remaining.zero()) {
                return false;
            }
#ifdef __APPLE__
            // `ReceiveUntil` waits for readable input only until the supplied
            // deadline, so a missing terminal reply cannot block a query
            // indefinitely. For `/dev/tty`, macOS `poll` reports `POLLNVAL`.
            // `pselect` supports this device and waits up to the given timeout.
            // https://www.gnu.org/software/gnulib/manual/html_node/poll.html
            const auto result = [descriptor = descriptor_, remaining] {
                const auto duration = std::chrono::ceil<std::chrono::nanoseconds>(remaining);
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
                const auto timeout = timespec{
                    .tv_sec = seconds.count(),
                    .tv_nsec = (duration - seconds).count(),
                };
                auto readable = fd_set{};
                FD_SET(descriptor, &readable);
                return pselect(
                    descriptor + 1, &readable, nullptr, nullptr, &timeout, nullptr);
            }();
#else
            auto readiness = pollfd{.fd = descriptor_, .events = POLLIN, .revents = 0};
            const auto result = poll(&readiness, 1,
                std::chrono::ceil<std::chrono::duration<int, std::milli>>(
                    remaining).count());
#endif
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
#ifndef __APPLE__
            if ((readiness.revents & POLLNVAL) != 0) {
                throw std::system_error(std::make_error_code(std::errc::bad_file_descriptor),
                    "could not wait for controlling-terminal input");
            }
#endif
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

    [[nodiscard]] auto ReceiveReport(const detail::TerminalReport report,
        const std::chrono::steady_clock::time_point deadline)
        -> std::optional<std::array<int, 3>> {
        // Only newly received bytes can answer this query. Earlier input may
        // include a report left over from a request that timed out.
        auto position = pending_.size();
        auto reader = detail::ReportReader<std::size_t>{report};
        for (;;) {
            // Ctrl+C can already be queued after a preceding reply, or arrive
            // while this query waits. Consume only that byte so other keys keep
            // their order when the caller handles cancellation.
            if (ConsumeCancellation()) {
                throw InputCancelled{};
            }
            if (!ReceiveUntil(deadline)) {
                return std::nullopt;
            }
            for (; position < pending_.size(); ++position) {
                if (pending_[position] == '\x03') {
                    break;
                }
                if (const auto values = reader.Push(
                        std::bit_cast<unsigned char>(pending_[position]), position)) {
                    pending_.erase(reader.Positions().front(), reader.Positions().size());
                    return values;
                }
            }
        }
    }

    [[nodiscard]] auto ConsumeCancellation() -> bool {
        const auto cancellation = pending_.find('\x03');
        if (cancellation == std::string::npos) {
            return false;
        }
        pending_.erase(cancellation, 1);
        return true;
    }

    // Escape is both a key and the first byte of navigation sequences. A short
    // wait admits a fragmented sequence; a lone Escape becomes Back when that
    // wait expires. Other keys already in the input queue retain their order.
    [[nodiscard]] auto ReadEscape() -> std::optional<MenuKey> {
        const auto deadline = std::chrono::steady_clock::now() + unix_detail::kEscapeTimeout;
        for (;;) {
            // Ctrl+C is user input even when a fragmented terminal reply
            // surrounds it. Consume cancellation before removing a complete
            // sequence, leaving the reply and other keys queued for later input.
            if (ConsumeCancellation()) {
                return MenuKey::Cancel;
            }
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
};

}
