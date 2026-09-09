// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.base_console;

import std;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.menu;
import devicefs.terminal.reports;
import devicefs.terminal.scope_exit;

using namespace std::string_view_literals;
using namespace std::chrono_literals;

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
            self.WriteControlSequenceNoThrow(kEndUpdate);
        }};
        self.Write(kBeginUpdate);
        return finish;
    }

    auto PresentFrame(this auto &self) -> void {
        self.Write(kEndUpdate);
    }

    // EnterMenu switches to an alternate screen without scrollback, preserving
    // the caller's screen for restoration. The terminal crops or extends the
    // alternate screen during resize, leaving the menu to lay out its new frame.
    // The returned owner must outlive frame updates and be destroyed before the
    // console. Destruction restores the caller's screen and cursor on selection,
    // cancellation, or an exception. Windows saves cursor shape and visibility
    // through its console API; Unix preserves cursor visibility through VT.
    // Each native adapter supplies the corresponding restoration guard.
    // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#alternate-screen-buffer
    [[nodiscard]] auto EnterMenu(this auto &self) {
        self.presenter_ = DeltaFramePresenter{};
        auto restore = self.RestoreScreenOnExit();
        self.Write(self.kEnterMenu);
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

protected:
    static constexpr auto kLeaveMenu = "\x1b[?1049l"sv;

    // Ctrl+C arrives as ETX when native input processing is disabled, allowing
    // the menu to cancel normally. Ctrl+L requests a redraw, and 1 opens the
    // selected entry's full text. Native navigation keys are decoded separately.
    [[nodiscard]] static auto CharacterMenuKey(const char32_t character) noexcept
        -> std::optional<MenuKey> {
        switch (character) {
        case U'\x03':
            return MenuKey::Cancel;
        case U'\x0c':
            return MenuKey::Redraw;
        case U'1':
            return MenuKey::Details;
        default:
            return std::nullopt;
        }
    }

private:
    static constexpr auto kBeginUpdate = "\x1b[?2026h"sv;
    static constexpr auto kEndUpdate = "\x1b[?2026l"sv;

    // Queries consume their reports while retaining interspersed user input.
    // Some terminals omit unsupported reports, so the deadline covers the whole
    // query, including time spent receiving other input. Native receivers keep
    // the input's original representation and remove only the matching report.
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
