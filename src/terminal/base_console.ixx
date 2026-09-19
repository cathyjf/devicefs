// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.base_console;

import std;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.scope_exit;
import devicefs.terminal.safecast;
import devicefs.terminal.vt;

using namespace std::chrono_literals;
using namespace std::string_view_literals;

export namespace devicefs::terminal {

// BaseConsole owns the terminal protocol used for drawing frames and measuring
// their layout. Native consoles inherit these operations and supply terminal
// I/O, input queues, and restoration of the caller's screen and cursor.
class BaseConsole {
public:
    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto Flip(this auto &self, const FrameBuffer &frame) -> bool {
        return self.presenter_.template Flip<Policy>(self, frame);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto KnownTextWidths(const std::string_view text) const {
        return presenter_.KnownTextWidths<Policy>(text);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto MeasureFrameLine(this auto &self, const FrameLine &line,
        const int row, const TerminalSize size) {
        return self.presenter_.template MeasureLine<Policy>(self, line, row, size);
    }

    auto InvalidateFrameRows(const int first_row, const int count) -> void {
        presenter_.InvalidateRows(first_row, count);
    }

    auto InvalidateFrame() -> void {
        presenter_.Invalidate();
    }

    // BeginUpdate groups layout measurements and frame drawing into one
    // display update. On terminals that support synchronized output, the request
    // keeps the previous display visible during that work. PresentFrame
    // releases the rendering hold when drawing is complete.
    // The guard also releases the hold if an operation throws. Windows Terminal
    // limits each hold to 100 ms, so a slower update can become visible early.
    // https://github.com/microsoft/terminal/blob/5a830b2bf7c053d5c7ac22208fe5a346cb5dd3dc/src/renderer/base/renderer.cpp#L191-L257
    [[nodiscard]] auto BeginUpdate(this auto &self) {
        auto finish = ScopeExit{[&self] {
            self.WriteControlSequenceNoThrow(vt::kEndSynchronizedUpdate);
        }};
        self.Write(vt::kBeginSynchronizedUpdate);
        return finish;
    }

    auto PresentFrame(this auto &self) -> void {
        self.Write(vt::kEndSynchronizedUpdate);
    }

    // `EnterScreen` owns an interactive session on the alternate screen,
    // preserving the invoking shell's screen and cursor until the owner is
    // destroyed. Keep this owner alive across menu calls so each new frame is
    // compared with the previous menu and can reuse its measured text widths.
    // The owner must be destroyed before the console; exception unwinding then
    // restores the screen through the same live connection. Windows preserves
    // cursor shape and visibility through its console API, while Unix preserves
    // cursor visibility through VT. The native adapter supplies that guard.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#alternate-screen-buffer
    [[nodiscard]] auto EnterScreen(this auto &self) {
        self.presenter_ = DeltaFramePresenter{};
        auto restore = self.RestoreScreenOnExit();
        self.Write(self.kEnterScreen);
        if constexpr (requires { self.ConfigureKeyboard(); }) {
            self.ConfigureKeyboard();
        }
        return restore;
    }

    [[nodiscard]] auto QueryCursor(this auto &self) -> std::optional<CursorPosition> {
        const auto reply = self.Query(detail::TerminalReport::Cursor);
        if (!reply || ((*reply)[0] < 1) || ((*reply)[1] < 1)) {
            return std::nullopt;
        }
        return CursorPosition{.row = (*reply)[0], .column = (*reply)[1]};
    }

    [[nodiscard]] auto QuerySize(this auto &self) -> std::optional<TerminalSize> {
        const auto reply = self.Query(detail::TerminalReport::Size);
        if (!reply || ((*reply)[0] != 8) || ((*reply)[1] < 1) || ((*reply)[2] < 1)) {
            return std::nullopt;
        }
        return TerminalSize{.rows = (*reply)[1], .columns = (*reply)[2]};
    }

    // Read navigation through the same decoder used for text entry. Ordinary
    // characters are ignored by menus, except for the `1` details shortcut.
    [[nodiscard]] auto ReadMenuInput(this auto &self,
        const std::chrono::steady_clock::time_point deadline =
            std::chrono::steady_clock::time_point::max()) -> MenuInput {
        for (;;) {
            auto input = self.ReadTextInput(deadline);
            if (input.key == MenuKey::Newline) {
                input.key = MenuKey::Accept;
            }
            if (input.key != MenuKey::Text) {
                return input;
            }
            if (input.character == U'1') {
                input.key = MenuKey::Details;
                return input;
            }
        }
    }

protected:
    // A legacy Escape key sends the same first byte as a terminal sequence.
    // Waiting briefly for another byte lets the native readers distinguish
    // the key from a sequence that arrives in separate reads.
    static constexpr auto kEscapeTimeout = 30ms;

    // Read a keyboard sequence beginning with the queued ESC, or return Back
    // for a lone Escape. Complete sequences are decoded immediately. An
    // incomplete sequence gets up to `kEscapeTimeout` to receive more input.
    // If the caller's refresh deadline comes first, the queued characters and
    // the original Escape deadline remain available to the next call.
    //
    // Native adapters expose the queued sequence characters and remove only
    // the characters consumed here. Windows can therefore retain resize and
    // native key records that arrive between characters of a sequence.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
    [[nodiscard]] auto ReadEscape(this auto &self,
        const std::chrono::steady_clock::time_point deadline)
        -> std::optional<MenuInput> {
        if (!self.escape_deadline_) {
            self.escape_deadline_ = std::chrono::steady_clock::now() + kEscapeTimeout;
        }
        for (;;) {
            // Ctrl+C can arrive inside a fragmented terminal reply. Consuming
            // only the cancellation leaves the reply and other keys queued.
            if (self.ConsumeCancellation()) {
                self.escape_deadline_.reset();
                return MenuInput{MenuKey::Cancel};
            }
            auto sequence = std::string{};
            for (const auto character : self.SequenceCharacters()) {
                // Navigation keys start with CSI (`ESC [`) or SS3 (`ESC O`).
                // `kMetaReturn` also starts with ESC. Any other second character
                // belongs to the input after a separately pressed Escape.
                // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
                if ((sequence.size() == 1) && (character != '[') && (character != 'O') &&
                    (character != '\r')) {
                    self.DiscardSequenceCharacters(1);
                    self.escape_deadline_.reset();
                    return MenuInput{MenuKey::Back};
                }
                // Keyboard sequences contain ASCII. A Unicode character after
                // an unfinished sequence remains queued for the text decoder.
                if ((character < 0) || (character >= 128)) {
                    self.DiscardSequenceCharacters(sequence.size());
                    self.escape_deadline_.reset();
                    return std::nullopt;
                }
                sequence.push_back(FailFastCast<char>(+character));
                if (sequence == vt::kMetaReturn) {
                    self.DiscardSequenceCharacters(sequence.size());
                    self.escape_deadline_.reset();
                    return MenuInput{MenuKey::Newline};
                }
                if (KeySequenceLength(sequence) != 0) {
                    if constexpr (requires { self.HandleTerminalReply(sequence); }) {
                        self.HandleTerminalReply(sequence);
                    }
                    const auto action = SequenceInput(sequence);
                    self.DiscardSequenceCharacters(sequence.size());
                    self.escape_deadline_.reset();
                    return action;
                }
            }
            if (!self.ReceiveUntil(std::min(deadline, *self.escape_deadline_))) {
                if (std::chrono::steady_clock::now() < *self.escape_deadline_) {
                    return std::nullopt;
                }
                self.escape_deadline_.reset();
                self.DiscardSequenceCharacters(1);
                return MenuInput{MenuKey::Back};
            }
        }
    }

    std::optional<std::chrono::steady_clock::time_point> escape_deadline_;

    // Ctrl+C cancels and Ctrl+L requests a redraw. Text entry also uses Ctrl+J
    // to insert a line break, leaving Enter available to accept the text.
    // Native navigation keys are decoded separately.
    [[nodiscard]] static auto CharacterInput(const char32_t character) noexcept
        -> std::optional<MenuInput> {
        switch (character) {
        case U'\x03': return MenuInput{MenuKey::Cancel};
        case U'\x0c': return MenuInput{MenuKey::Redraw};
        case U'\t': return MenuInput{MenuKey::SwitchArea};
        case U'\r': return MenuInput{MenuKey::Accept};
        case U'\n': return MenuInput{MenuKey::Newline};
        case U'\b':
        case U'\x7f': return MenuInput{MenuKey::Backspace};
        default:
            return (character >= U' ') ? std::optional{MenuInput{
                .key = MenuKey::Text, .character = character}} : std::nullopt;
        }
    }
    // `KeySequenceLength` finds a complete CSI or SS3 sequence at the beginning of
    // queued input. Legacy navigation keys and kitty keyboard protocol reports
    // share this framing. Navigation and cancellation decoding both use the result
    // to distinguish a complete key sequence from one split between reads.
    // CSI (`ESC [`) and SS3 (`ESC O`) end with a byte from '@' through '~'.
    // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html
    [[nodiscard]] static auto KeySequenceLength(const std::string_view input) -> std::size_t {
        if (!input.starts_with(vt::kCsiPrefix) && !input.starts_with(vt::kSs3Prefix)) {
            return 0;
        }
        const auto body = input.substr(2);
        const auto final = std::ranges::find_if(body, [](const char value) {
            return (value >= '@') && (value <= '~');
        });
        return (final == body.end()) ? 0 : (3 + FailFastCast<std::size_t>(final - body.begin()));
    }

    // Navigation keys use CSI sequences, or SS3 sequences in application-cursor
    // mode. The final letter identifies an arrow, Home, or End; numeric tilde forms
    // identify the remaining navigation keys. Other complete sequences are ignored.
    // https://invisible-mirror.net/xterm/ctlseqs/ctlseqs.html#h2-PC-Style-Function-Keys
    [[nodiscard]] static auto NavigationKey(const std::string_view sequence)
        -> std::optional<MenuInput> {
        switch (sequence.back()) {
        case 'C':
            return MenuInput{MenuKey::Right};
        case 'D':
            return MenuInput{MenuKey::Left};
        case 'A':
            return MenuInput{MenuKey::Up};
        case 'B':
            return MenuInput{MenuKey::Down};
        case 'H':
            return MenuInput{MenuKey::Home};
        case 'F':
            return MenuInput{MenuKey::End};
        case 'Z':
            return MenuInput{MenuKey::SwitchArea};
        case '~': {
            const auto parameters = sequence.substr(2, sequence.size() - 3);
            const auto parameter = parameters.substr(0, parameters.find(';'));
            if ((parameter == "1"sv) || (parameter == "7"sv)) {
                return MenuInput{MenuKey::Home};
            }
            if ((parameter == "4"sv) || (parameter == "8"sv)) {
                return MenuInput{MenuKey::End};
            }
            if (parameter == "3"sv) {
                return MenuInput{MenuKey::Delete};
            }
            if (parameter == "5"sv) {
                return MenuInput{MenuKey::PageUp};
            }
            if (parameter == "6"sv) {
                return MenuInput{MenuKey::PageDown};
            }
            return std::nullopt;
        }
        default:
            return std::nullopt;
        }
    }

    // `KittyKey` translates kitty keyboard protocol reports into input events.
    // Flag 1, requested on screen entry, supplies `CSI key;modifiers u` for
    // Escape and modified keys. For example, `CSI 27 u` is Escape and
    // `CSI 99;5 u` is Ctrl+C. The modifier field defaults to 1 when omitted and
    // otherwise contains one plus the modifier bits. Ctrl+letter is translated
    // to the legacy control character so `CharacterInput` owns the shortcuts
    // for both encodings. Unrecognized reports return an empty optional.
    // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#disambiguate-escape-codes
    [[nodiscard]] static auto KittyKey(const std::string_view sequence,
        const unsigned meta_modifier = vt::kKittyMetaModifier)
        -> std::optional<MenuInput> {
        if (!sequence.starts_with(detail::kReportPrefix) || !sequence.ends_with('u')) {
            return std::nullopt;
        }
        auto body = sequence.substr(2, sequence.size() - 3);
        auto key = unsigned{};
        const auto [end, error] = std::from_chars(body.data(), body.data() + body.size(), key);
        if ((error != std::errc{}) || (key > 0x10ffff) || ((key >= 0xd800) && (key <= 0xdfff))) {
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
            // Explicit press and repeat events carry :1 and :2 after the
            // modifiers. A release (:3) must not insert another character.
            // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#event-types
            const auto colon = body.find(':');
            const auto field = body.substr(0, colon);
            if ((colon != std::string_view::npos) &&
                (body.substr(colon) != ":1"sv) && (body.substr(colon) != ":2"sv)) {
                return std::nullopt;
            }
            const auto parsed = std::from_chars(field.data(), field.data() + field.size(), modifiers);
            if ((parsed.ec != std::errc{}) || (parsed.ptr != (field.data() + field.size())) ||
                (modifiers == 0)) {
                return std::nullopt;
            }
        }
        // Lock states do not change menu shortcuts. Shift is also accepted with
        // Ctrl+letter, whose reported key code is the unshifted lowercase letter.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#modifiers
        constexpr auto kShift = 1u;
        constexpr auto kAlt = 2u;
        constexpr auto kControl = 4u;
        constexpr auto kCapsLock = 64u;
        constexpr auto kNumLock = 128u;
        const auto active = (modifiers - 1) & ~(kCapsLock | kNumLock);
        if (((active & ~kShift) == kControl) && (key >= U'a') && (key <= U'z')) {
            key -= U'a' - 1;
        } else if ((active == kShift) && (key == U'\t')) {
            return MenuInput{MenuKey::SwitchArea};
        } else if ((active != 0) && !((active == kShift) && (key >= U' ')) &&
            (key < 128) && (key != vt::kEscape) && (key != U'\r')) {
            return std::nullopt;
        }
        // The protocol assigns distinct codes to non-text keypad keys. Give
        // those keys the same menu actions as the corresponding ordinary keys.
        // https://sw.kovidgoyal.net/kitty/keyboard-protocol/#functional-key-definitions
        switch (key) {
        case vt::kEscape: return MenuInput{MenuKey::Back};
        case U'\r':
        case 57414: // `KP_ENTER`
            // Meta Return inserts a newline in both its legacy ESC+CR form and
            // its extended keyboard reports. Alt can also encode Meta Return.
            return MenuInput{((active & (kShift | kAlt | meta_modifier)) != 0) ?
                MenuKey::Newline : MenuKey::Accept};
        case 57350:
        case 57417: return MenuInput{MenuKey::Left}; // `LEFT` and `KP_LEFT`
        case 57351:
        case 57418: return MenuInput{MenuKey::Right}; // `RIGHT` and `KP_RIGHT`
        case 57349: return MenuInput{MenuKey::Delete}; // `DELETE`
        case 57419: return MenuInput{MenuKey::Up}; // `KP_UP`
        case 57420: return MenuInput{MenuKey::Down}; // `KP_DOWN`
        case 57421: return MenuInput{MenuKey::PageUp}; // `KP_PAGE_UP`
        case 57422: return MenuInput{MenuKey::PageDown}; // `KP_PAGE_DOWN`
        case 57423: return MenuInput{MenuKey::Home}; // `KP_HOME`
        case 57424: return MenuInput{MenuKey::End}; // `KP_END`
        default:
            // Kitty uses this private-use range for function keys. An
            // unhandled function key must not insert a private-use character.
            if ((key >= 57344) && (key <= 63743)) {
                return std::nullopt;
            }
            return CharacterInput(key);
        }
    }

    // Decode terminal keyboard reports on either host platform. Xterm's
    // `modifyOtherKeys` uses `CSI 27;modifiers;key~`; its alternate CSI-u form
    // and Kitty use `CSI key;modifiers u`. Both encode Shift as modifier 2.
    // Native Windows key records can report Shift independently of this protocol.
    // https://invisible-island.net/xterm/ctlseqs/ctlseqs.html#h2-Alt-and-Meta-Keys
    [[nodiscard]] static auto SequenceInput(const std::string_view sequence)
        -> std::optional<MenuInput> {
        if (sequence.starts_with(vt::kModifiedKeyPrefix) && sequence.ends_with('~')) {
            const auto body = sequence.substr(5, sequence.size() - 6);
            const auto separator = body.find(';');
            if (separator == std::string_view::npos) {
                return std::nullopt;
            }
            return KittyKey(vt::CsiKeyReport(
                body.substr(separator + 1), body.substr(0, separator)), vt::kXtermMetaModifier);
        }
        return sequence.ends_with('u') ? KittyKey(sequence) : NavigationKey(sequence);
    }

private:
    // Queries consume their reports while retaining interspersed user input.
    // Some terminals omit unsupported reports, so the deadline covers the whole
    // query, including time spent receiving other input. Native receivers keep
    // the input's original representation and remove only the matching report.
    // Ctrl+C raises InputCancelled so the caller can end the operation before
    // the reply arrives or the deadline expires.
    [[nodiscard]] auto Query(this auto &self, const detail::TerminalReport report)
        -> std::optional<std::array<int, 3>> {
        constexpr auto kReplyTimeout = 5s;
        const auto deadline = std::chrono::steady_clock::now() + kReplyTimeout;
        self.Write(detail::ReportRequest(report));
        return self.ReceiveReport(report, deadline);
    }

    DeltaFramePresenter presenter_;
};

}
