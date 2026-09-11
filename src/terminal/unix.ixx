// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

// Waiting on a terminal descriptor above `FD_SETSIZE - 1` requires Apple's
// extended `pselect` wrapper. The ordinary wrapper rejects `nfds > FD_SETSIZE`
// with `EINVAL` before calling the kernel. Defining `_DARWIN_UNLIMITED_SELECT`
// makes `<sys/select.h>` select the `$DARWIN_EXTSN` symbol, whose wrapper omits
// that check and passes the descriptor count and sets to the kernel.
// Symbol selection:
//   https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/select.h
// Wrapper implementations:
//   https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/select-base.c
#define _DARWIN_UNLIMITED_SELECT

#include <cerrno>
#include <fcntl.h>
#include <sys/select.h>
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
import devicefs.terminal.vt;

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
    return ScopeExit{std::bind(close, descriptor)};
}

// This `TcSetAttrInvoker` type could be replaced with a lambda. However, doing
// so causes GCC 16.2.0 on macOS to crash when compiling main.cpp.
struct TcSetAttrInvoker {
    int descriptor;
    termios previous;
    auto operator()() {
        return tcsetattr(descriptor, TCSANOW, &previous);
    }
};

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
    auto restore = ScopeExit{TcSetAttrInvoker{descriptor, previous}};
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

// `KeySequenceLength` finds a complete CSI or SS3 sequence at the beginning of
// queued input. Legacy navigation keys and kitty keyboard protocol reports
// share this framing. Navigation and cancellation decoding both use the result
// to distinguish a complete key sequence from one split between reads.
// CSI (`ESC [`) and SS3 (`ESC O`) end with a byte from '@' through '~'.
// https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
[[nodiscard]] auto KeySequenceLength(const std::string_view input) -> std::size_t {
    if (!input.starts_with("\x1b["sv) && !input.starts_with("\x1bO"sv)) {
        return 0;
    }
    const auto body = input.substr(2);
    const auto final = std::ranges::find_if(body, [](const char value) {
        return (value >= '@') && (value <= '~');
    });
    return final == body.end() ? 0 : 3 + FailFastCast<std::size_t>(final - body.begin());
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
        const auto parameters = sequence.substr(2, sequence.size() - 3);
        const auto parameter = parameters.substr(0, parameters.find(';'));
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

// `UnixConsole` connects the text writer and menu to the process's controlling
// terminal. The adapter writes UTF-8, receives cursor and size reports through
// VT, and translates keyboard sequences into menu operations. Opening `/dev/tty`
// keeps this connection independent of redirected standard streams.
//
// Interactive screens request kitty's keyboard protocol with flag 1,
// "Disambiguate escape codes". A supporting terminal encodes the Escape key as
// `CSI 27 u`, so `ReadEscape` can recognize the complete key without waiting to
// see whether a lone ESC byte begins a longer sequence. `KittyKey` decodes these
// reports alongside the legacy input accepted from other terminals. This is a
// terminal-emulator protocol; the Unix driver transports the resulting bytes.
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
//
// The console owns its descriptor and temporary `termios` settings. Menu and
// update guards must be destroyed before the console; those guards restore the
// screen and keyboard mode, while console destruction restores `termios` and
// closes the descriptor.
// Unavailable reports return an empty optional. Terminal I/O failures throw;
// Ctrl+C during a query raises `InputCancelled`.
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
            // ESC (0x1B) begins a terminal sequence; a lone ESC is the Escape key.
            // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
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

    // Kitty's keyboard-mode stack belongs to the active screen. Enter the
    // alternate screen before pushing flag 1, and pop that screen's saved mode
    // before returning to the main screen. The screen guard owns this lifetime,
    // so successive menus in one session use the same keyboard mode.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#progressive-enhancement
    static constexpr auto kEnterScreen = vt::Concatenate<
        vt::kEnterAlternateScreen, vt::kPushDisambiguatedKeys,
        vt::kSaveCursorVisibility, vt::kHideCursor>();
    static constexpr auto kLeaveScreen = vt::Concatenate<vt::kPopKeyboardMode, vt::kResetAttributes,
        vt::kShowCursor, vt::kRestoreCursorVisibility, vt::kLeaveAlternateScreen>();

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
        return ScopeExit{std::bind(
            &UnixConsole::WriteControlSequenceNoThrow, this, kLeaveScreen)};
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
        // `pselect` reads a bitmap sized by `descriptor_ + 1`. An array of
        // `fd_set` objects supplies enough bits for high-numbered descriptors.
        // Each `FD_SET` operates within one element; concatenating the elements
        // extends the bitmap using the platform's own bit layout.
        // Linux:
        //   https://man7.org/linux/man-pages/man2/select.2.html#NOTES
        // Apple:
        //   https://github.com/apple-oss-distributions/xnu/blob/main/bsd/kern/sys_generic.c#L1227-L1250
        // Each element represents `FD_SETSIZE` consecutive descriptors. Its
        // storage must therefore occupy exactly that many bits: any padding
        // would shift the following element's bits, making `pselect` watch
        // different descriptors from those selected by `FD_SET`.
        static_assert(
            (sizeof(fd_set) * std::numeric_limits<unsigned char>::digits) == FD_SETSIZE);
        auto readable = std::vector<fd_set>{(descriptor_ / FD_SETSIZE) + 1uz};
        FD_SET(descriptor_ % FD_SETSIZE, &readable.back());
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            if (remaining <= remaining.zero()) {
                return false;
            }
            // `ReceiveUntil` waits for readable input only until the supplied
            // deadline, so a missing terminal reply cannot block a query
            // indefinitely. For `/dev/tty`, macOS `poll` reports `POLLNVAL`.
            // `pselect` supports this device and waits up to the given timeout.
            // https://www.gnu.org/software/gnulib/manual/html_node/poll.html
            const auto result = [descriptor = descriptor_, remaining, &readable] {
                const auto duration = std::chrono::ceil<std::chrono::nanoseconds>(remaining);
                const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
                const auto timeout = timespec{
                    .tv_sec = seconds.count(),
                    .tv_nsec = (duration - seconds).count(),
                };
                return pselect(
                    descriptor + 1, readable.data(), nullptr, nullptr, &timeout, nullptr);
            }();
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

    [[nodiscard]] auto ReceiveReport(const detail::TerminalReport report,
        const std::chrono::steady_clock::time_point deadline)
        -> std::optional<std::array<int, 3>> {
        // Only newly received bytes can answer this query. Earlier input may
        // include a report left over from a request that timed out.
        auto position = pending_.size();
        auto reader = detail::ReportReader<std::size_t>{report};
        for (;;) {
            // Ctrl+C can already be queued after a preceding reply, or arrive
            // while this query waits. Consume only that key so other keys keep
            // their order when the caller handles cancellation.
            if (ConsumeCancellation()) {
                throw InputCancelled{};
            }
            if (!ReceiveUntil(deadline)) {
                return std::nullopt;
            }
            const auto cancellation = FindCancellation();
            for (; position < pending_.size(); ++position) {
                if (cancellation && (position >= cancellation->position)) {
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

    // `KittyKey` translates kitty keyboard protocol reports into menu actions.
    // Flag 1, requested on screen entry, supplies `CSI key;modifiers u` for
    // Escape and modified keys. For example, `CSI 27 u` is Escape and
    // `CSI 99;5 u` is Ctrl+C. The modifier field defaults to 1 when omitted and
    // otherwise contains one plus the modifier bits. Ctrl+letter is translated
    // to the legacy control character so `CharacterMenuKey` owns the shortcuts
    // for both encodings. Unrecognized reports return an empty optional.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
    [[nodiscard]] static auto KittyKey(const std::string_view sequence)
        -> std::optional<MenuKey> {
        if (!sequence.starts_with(detail::kReportPrefix) || !sequence.ends_with('u')) {
            return std::nullopt;
        }
        auto body = sequence.substr(2, sequence.size() - 3);
        auto key = unsigned{};
        const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), key);
        if (error != std::errc{}) {
            return std::nullopt;
        }
        body.remove_prefix(FailFastCast<std::size_t>(end - body.data()));
        if (body.starts_with(';')) {
            body.remove_prefix(1);
        } else if (!body.empty()) {
            return std::nullopt;
        }
        auto modifiers = 1u;
        if (!body.empty()) {
            const auto parsed = std::from_chars(body.data(), body.data() + body.size(), modifiers);
            if ((parsed.ec != std::errc{}) || (parsed.ptr != (body.data() + body.size())) ||
                (modifiers == 0)) {
                return std::nullopt;
            }
        }
        // Lock states do not change menu shortcuts. Shift is also accepted with
        // Ctrl+letter, whose reported key code is the unshifted lowercase letter.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        constexpr auto kShift = 1u;
        constexpr auto kControl = 4u;
        constexpr auto kCapsLock = 64u;
        constexpr auto kNumLock = 128u;
        const auto active = (modifiers - 1) & ~(kCapsLock | kNumLock);
        if (((active & ~kShift) == kControl) && (key >= U'a') && (key <= U'z')) {
            key -= U'a' - 1;
        } else if ((active != 0) && (key < 128) && (key != U'\x1b') && (key != U'\r')) {
            return std::nullopt;
        }
        // The protocol assigns distinct codes to non-text keypad keys. Give
        // those keys the same menu actions as the corresponding ordinary keys.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
        switch (key) {
        case U'\x1b': return MenuKey::Back;
        case U'\n':
        case U'\r':
        case 57414: return MenuKey::Accept; // `KP_ENTER`
        case 57419: return MenuKey::Up; // `KP_UP`
        case 57420: return MenuKey::Down; // `KP_DOWN`
        case 57421: return MenuKey::PageUp; // `KP_PAGE_UP`
        case 57422: return MenuKey::PageDown; // `KP_PAGE_DOWN`
        case 57423: return MenuKey::Home; // `KP_HOME`
        case 57424: return MenuKey::End; // `KP_END`
        default: return CharacterMenuKey(key);
        }
    }

    struct CancellationRange {
        std::size_t position;
        std::size_t length;
    };

    // `FindCancellation` locates Ctrl+C in either keyboard encoding: the legacy
    // ETX byte (0x03), or a kitty keyboard protocol report decoded by `KittyKey`.
    // The returned range covers the entire key, allowing cancellation to remove
    // that key without consuming neighboring input. Queries also use its start
    // position to return a reply that arrived before Ctrl+C. ESC (0x1b) begins
    // a kitty key report or another terminal sequence.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#legacy-ctrl-mapping-of-ascii-keys
    [[nodiscard]] auto FindCancellation() const -> std::optional<CancellationRange> {
        for (auto position = pending_.find_first_of("\x03\x1b"sv);
            position != std::string::npos;
            position = pending_.find_first_of("\x03\x1b"sv, position + 1)) {
            if (pending_[position] == '\x03') {
                return CancellationRange{position, 1};
            }
            const auto sequence = std::string_view{pending_}.substr(position);
            const auto length = unix_detail::KeySequenceLength(sequence);
            if ((length != 0) && (KittyKey(sequence.substr(0, length)) == MenuKey::Cancel)) {
                return CancellationRange{position, length};
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto ConsumeCancellation() -> bool {
        if (const auto cancellation = FindCancellation()) {
            pending_.erase(cancellation->position, cancellation->length);
            return true;
        }
        return false;
    }

    // `ReadEscape` decodes input beginning with ESC: legacy navigation sequences,
    // kitty keyboard protocol reports, or the legacy Escape key itself. Complete
    // sequences are decoded immediately, including kitty's `CSI 27 u` Escape.
    // An incomplete sequence gets up to `kEscapeTimeout` to receive more bytes;
    // if that wait expires, the leading ESC is consumed as the Back action.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
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
                // Navigation keys start with CSI (`ESC [`) or SS3 (`ESC O`),
                // depending on normal or application cursor-key mode.
                // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
                if ((pending_[1] != '[') && (pending_[1] != 'O')) {
                    pending_.erase(0, 1);
                    return MenuKey::Back;
                }
                if (const auto length = unix_detail::KeySequenceLength(pending_)) {
                    const auto sequence = std::string_view{pending_}.substr(0, length);
                    const auto action = sequence.ends_with('u') ? KittyKey(sequence) :
                        unix_detail::NavigationKey(sequence);
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
