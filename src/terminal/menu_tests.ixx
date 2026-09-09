// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.terminal.menu_tests;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.menu;

using namespace std::string_view_literals;
using namespace devicefs::terminal;

namespace {

auto Require(const bool condition, const std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] auto Test(const std::string_view name, const auto &operation) -> bool {
    std::println("Testing {}.", name);
    try {
        std::invoke(operation);
        std::println("PASS: {}.", name);
        return true;
    } catch (const std::exception &error) {
        std::println(std::cerr, "FAIL: {}: {}", name, error.what());
        return false;
    }
}

class InputFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class OutputFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The menu tests capture each displayed screen before supplying the next key.
// This adapter gives ASCII characters one column and implements the cursor,
// clearing, and insertion commands used by the menu and wrapping writer. The
// captured rows let tests examine what a user could select or read at that
// point. An update writes and measures a separate hidden screen; `PresentFrame`
// publishes that screen. Counting publications lets tests detect whether the
// menu exposed a measurement or an incomplete drawing before the finished
// frame. Unicode width behavior is covered by the wrapping tests separately.
// Row writes, screen clears, and cursor queries are recorded between input
// events so selection tests can distinguish a local repaint from a new layout.
// The final row also records individual changed columns to check status updates.
class MenuConsole {
public:
    explicit MenuConsole(const std::span<const MenuInput> input,
        const TerminalSize size = {.rows = 8, .columns = 80})
        : input_{input}, size_{size} {}

    [[nodiscard]] auto EnterMenu() noexcept {
        presenter_ = DeltaFramePresenter{};
        active = true;
        return wil::scope_exit([this]() noexcept { active = false; });
    }

    [[nodiscard]] auto BeginUpdate() {
        Require(!updating, "a screen update began before the previous update ended"sv);
        hidden_ = displayed_;
        updating = true;
        return wil::scope_exit([this]() noexcept { updating = false; });
    }

    auto PresentFrame() -> void {
        Require(updating, "a frame was published outside a screen update"sv);
        displayed_ = hidden_;
        ++pending_presentations_;
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    auto Flip(const FrameBuffer &frame) -> bool {
        return presenter_.Flip<Policy>(*this, frame);
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto KnownTextWidths(const std::string_view text) const {
        return presenter_.KnownTextWidths<Policy>(text);
    }

    auto InvalidateFrameRows(const int first_row, const int count) -> void {
        ++frame_row_invalidations;
        presenter_.InvalidateRows(first_row, count);
    }

    auto InvalidateFrame() -> void {
        presenter_.Invalidate();
    }

    auto Write(std::string_view text) -> void {
        ++write_calls;
        Require(!active || updating, "menu output occurred outside a screen update"sv);
        auto &screen = updating ? hidden_ : displayed_;
        auto wrote_text = false;
        while (!text.empty()) {
            if (text.front() == '\x1b') {
                Require(text.starts_with("\x1b["sv), "unexpected escape command"sv);
                text.remove_prefix(2);
                const auto end = text.find_first_of("HJKXm@hl"sv);
                Require(end != std::string_view::npos, "unterminated cursor command"sv);
                const auto parameters = text.substr(0, end);
                const auto command = text[end];
                text.remove_prefix(end + 1);
                if (command == 'H') {
                    const auto separator = parameters.find(';');
                    screen.cursor = parameters.empty() ? CursorPosition{} : CursorPosition{
                        .row = std::stoi(std::string{parameters.substr(0, separator)}),
                        .column = separator == std::string_view::npos ? 1 :
                            std::stoi(std::string{parameters.substr(separator + 1)})};
                    screen.pending_wrap = false;
                } else if (command == 'J') {
                    screen.lines.clear();
                    ++pending_clears_;
                } else if (command == 'K') {
                    Require(parameters.empty() || (parameters == "0"sv) || (parameters == "2"sv),
                        "unexpected line-erasure command"sv);
                    const auto first_column = parameters == "2"sv ? 1 : screen.cursor.column;
                    auto &line = screen.lines[screen.cursor.row];
                    line.resize(std::min(line.size(),
                        wil::safe_cast_failfast<std::size_t>(first_column - 1)));
                    if (screen.cursor.row == size_.rows) {
                        for (auto column = first_column; column <= size_.columns; ++column) {
                            pending_status_columns_.insert(column);
                        }
                    }
                    screen.pending_wrap = false;
                    pending_touched_rows_.insert(screen.cursor.row);
                } else if (command == 'X') {
                    const auto count = parameters.empty() ? 1 : std::stoi(std::string{parameters});
                    const auto end_column = screen.cursor.column +
                        std::min(count, size_.columns - screen.cursor.column + 1);
                    auto &line = screen.lines[screen.cursor.row];
                    for (auto column = screen.cursor.column; column < end_column; ++column) {
                        if (std::cmp_less_equal(column, line.size())) {
                            line.at(wil::safe_cast_failfast<std::size_t>(column - 1)) = ' ';
                        }
                        if (screen.cursor.row == size_.rows) {
                            pending_status_columns_.insert(column);
                        }
                    }
                    while (!line.empty() && (line.back() == ' ')) {
                        line.pop_back();
                    }
                    screen.pending_wrap = false;
                    pending_touched_rows_.insert(screen.cursor.row);
                } else if (command == '@') {
                    const auto columns = std::stoi(std::string{parameters});
                    auto &line = screen.lines[screen.cursor.row];
                    for (auto index = 0; index < columns; ++index) {
                        line.insert(std::next(line.begin(),
                            wil::safe_cast_failfast<std::ptrdiff_t>(screen.cursor.column) - 1), ' ');
                    }
                    pending_touched_rows_.insert(screen.cursor.row);
                }
                continue;
            }
            wrote_text = true;
            if (screen.pending_wrap) {
                ++screen.cursor.row;
                screen.cursor.column = 1;
                screen.pending_wrap = false;
            }
            Require((text.front() >= ' ') && (text.front() <= '~'),
                "the menu fixture received a non-ASCII display character"sv);
            Require((screen.cursor.row >= 1) && (screen.cursor.row <= size_.rows) &&
                (screen.cursor.column >= 1) && (screen.cursor.column <= size_.columns),
                "menu output escaped the available screen rows or columns"sv);
            auto &line = screen.lines[screen.cursor.row];
            pending_touched_rows_.insert(screen.cursor.row);
            if (screen.cursor.row == size_.rows) {
                pending_status_columns_.insert(screen.cursor.column);
            }
            while (std::cmp_less(line.size(), screen.cursor.column)) {
                line.push_back(' ');
            }
            *std::next(line.begin(),
                wil::safe_cast_failfast<std::ptrdiff_t>(screen.cursor.column) - 1) = text.front();
            text.remove_prefix(1);
            if (screen.cursor.column == size_.columns) {
                screen.pending_wrap = true;
            } else {
                ++screen.cursor.column;
            }
        }
        if (updating && wrote_text && fail_after_hidden_text) {
            throw OutputFailure{"the test terminal failed after writing the hidden frame"};
        }
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        ++cursor_query_calls;
        Require(!active || updating,
            "menu layout queried the displayed cursor outside a hidden update"sv);
        ++pending_cursor_queries_;
        if (std::exchange(fail_next_cursor_query, false)) {
            return std::nullopt;
        }
        return updating ? hidden_.cursor : displayed_.cursor;
    }

    [[nodiscard]] auto QuerySize() const noexcept -> std::optional<TerminalSize> {
        ++size_query_calls;
        return size_;
    }

    [[nodiscard]] auto Row(const int row) const -> std::string_view {
        const auto found = displayed_.lines.find(row);
        return found == displayed_.lines.end() ? ""sv : std::string_view{found->second};
    }

    [[nodiscard]] auto ReadMenuInput() -> MenuInput {
        Require(!updating, "the menu waited for input with a screen update unfinished"sv);
        auto frame = std::string{};
        for (const auto &[row, line] : displayed_.lines) {
            frame.append(line);
            frame.push_back('\n');
        }
        frames.push_back(std::move(frame));
        presentations.push_back(std::exchange(pending_presentations_, 0));
        touched_rows.push_back(std::exchange(pending_touched_rows_, {}));
        status_columns.push_back(std::exchange(pending_status_columns_, {}));
        clears.push_back(std::exchange(pending_clears_, 0));
        cursor_queries.push_back(std::exchange(pending_cursor_queries_, 0));
        if (input_.empty()) {
            throw InputFailure{"menu requested input after the test's keys were exhausted"};
        }
        const auto input = input_.front();
        input_ = input_.subspan(1);
        if ((input.key == MenuKey::Resize) && resize_to) {
            size_ = *resize_to;
            std::erase_if(displayed_.lines, [this](const auto &line) {
                return line.first > size_.rows;
            });
            for (auto &[row, line] : displayed_.lines) {
                line.resize(std::min(line.size(),
                    wil::safe_cast_failfast<std::size_t>(size_.columns)));
            }
            displayed_.cursor.row = std::min(displayed_.cursor.row, size_.rows);
            displayed_.cursor.column = std::min(displayed_.cursor.column, size_.columns);
            displayed_.pending_wrap = false;
        }
        return input;
    }

    bool active = false;
    bool updating = false;
    bool fail_next_cursor_query = false;
    bool fail_after_hidden_text = false;
    std::size_t write_calls = 0;
    std::size_t cursor_query_calls = 0;
    mutable std::size_t size_query_calls = 0;
    std::size_t frame_row_invalidations = 0;
    std::optional<TerminalSize> resize_to;
    std::vector<std::string> frames;
    std::vector<std::size_t> presentations;
    std::vector<std::set<int>> touched_rows;
    std::vector<std::set<int>> status_columns;
    std::vector<std::size_t> clears;
    std::vector<std::size_t> cursor_queries;

private:
    DeltaFramePresenter presenter_;

    struct Screen {
        CursorPosition cursor;
        bool pending_wrap = false;
        std::map<int, std::string> lines;
    };

    std::span<const MenuInput> input_;
    TerminalSize size_;
    Screen displayed_;
    Screen hidden_;
    std::size_t pending_presentations_ = 0;
    std::set<int> pending_touched_rows_;
    std::set<int> pending_status_columns_;
    std::size_t pending_clears_ = 0;
    std::size_t pending_cursor_queries_ = 0;
};

class CapturingMenuConsole : public MenuConsole {
public:
    using MenuConsole::MenuConsole;

    template <WidthPolicy = WidthPolicy::AllModes>
    auto Flip(const FrameBuffer &frame) -> bool {
        submitted_frames.push_back(frame);
        return true;
    }

    std::vector<FrameBuffer> submitted_frames;
};

constexpr auto kHeader = std::array{"Menu regression test"sv};
constexpr auto kEntries = std::array{"Alpha"sv, "Bravo"sv, "Charlie"sv, "Delta"sv};

}

export [[nodiscard]] auto TestMenu() -> bool {
    auto passed = Test("a back buffer leaving terminal output untouched until Flip"sv, [] {
        constexpr auto input = std::array{MenuInput{MenuKey::Accept}};
        constexpr auto size = TerminalSize{.rows = 3, .columns = 30};
        auto terminal = MenuConsole{input, size};
        const auto session = terminal.EnterMenu();
        auto frame = FrameBuffer{size};
        frame.rows.front() = {.text = "Frame heading"};
        frame.rows.at(1) = {.text = "Selected row", .reverse = true};
        frame.rows.back() = {.text = "Status: ready"};
        Require((terminal.write_calls == 0) && (terminal.cursor_query_calls == 0) &&
            (terminal.size_query_calls == 0),
            "preparing the back buffer performed terminal output or queried the terminal"sv);
        Require(terminal.Row(1).empty() && terminal.Row(2).empty() && terminal.Row(3).empty(),
            "preparing the back buffer changed the displayed screen"sv);
        {
            const auto update = terminal.BeginUpdate();
            Require(terminal.Flip(frame), "the prepared frame could not be presented"sv);
        }
        std::ignore = terminal.ReadMenuInput();
        Require((terminal.Row(1) == "Frame heading"sv) &&
            (terminal.Row(2) == "Selected row"sv) && (terminal.Row(3) == "Status: ready"sv),
            "Flip did not display the complete prepared frame"sv);
        Require(terminal.presentations.front() == 1,
            "Flip did not publish exactly one drawing for the prepared frame"sv);
    });
    passed &= Test("frame comparison skipping identical output and replacing one changed cell"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Accept}, MenuInput{MenuKey::Accept}, MenuInput{MenuKey::Accept}};
        constexpr auto size = TerminalSize{.rows = 2, .columns = 30};
        auto terminal = MenuConsole{input, size};
        const auto session = terminal.EnterMenu();
        auto frame = FrameBuffer{size};
        frame.rows.front() = {.text = "Unchanged heading"};
        frame.rows.back() = {.text = "Progress: 1 of 9"};
        {
            const auto update = terminal.BeginUpdate();
            Require(terminal.Flip(frame), "the prepared frame could not be presented"sv);
        }
        std::ignore = terminal.ReadMenuInput();
        const auto previous_write_calls = terminal.write_calls;
        {
            const auto update = terminal.BeginUpdate();
            Require(terminal.Flip(frame), "the prepared frame could not be presented"sv);
        }
        std::ignore = terminal.ReadMenuInput();
        Require((terminal.write_calls == previous_write_calls) &&
            (terminal.presentations.at(1) == 0),
            "flipping the unchanged frame wrote to the terminal or published a drawing"sv);
        frame.rows.back().text = "Progress: 2 of 9";
        {
            const auto update = terminal.BeginUpdate();
            Require(terminal.Flip(frame), "the prepared frame could not be presented"sv);
        }
        std::ignore = terminal.ReadMenuInput();
        Require((terminal.Row(1) == "Unchanged heading"sv) &&
            (terminal.Row(2) == "Progress: 2 of 9"sv),
            "the changed frame damaged the heading or failed to update the status"sv);
        Require((terminal.touched_rows.at(2) == std::set{2}) &&
            (terminal.status_columns.at(2) == std::set{11}) &&
            (terminal.clears.at(2) == 0) && (terminal.presentations.at(2) == 1),
            "a one-cell frame change repainted other cells or cleared the screen"sv);
    });
    passed &= Test("clipped frames staying inside one-, two-, and three-column rows"sv, [] {
        constexpr auto input = std::array{MenuInput{MenuKey::Accept}};
        for (const auto columns : std::array{1, 2, 3}) {
            const auto size = TerminalSize{.rows = 1, .columns = columns};
            auto terminal = MenuConsole{input, size};
            const auto session = terminal.EnterMenu();
            auto frame = FrameBuffer{size};
            frame.rows.front() = {.text = "Enlarge the window to use this menu.",
                .clipping = FrameClipping::IfNeeded};
            {
                const auto update = terminal.BeginUpdate();
                Require(terminal.Flip(frame), "the prepared frame could not be presented"sv);
            }
            std::ignore = terminal.ReadMenuInput();
            Require(std::cmp_less_equal(terminal.Row(1).size(), columns) &&
                terminal.Row(1).ends_with('.'),
                std::format("the clipped message or ellipsis did not fit in {} columns", columns));
            Require((terminal.touched_rows.front() == std::set{1}) && terminal.Row(2).empty(),
                std::format("clipping a {}-column frame wrote outside its only row", columns));
        }
    });
    passed &= Test("a menu submitting complete frames to an adapter that only stores them"sv, [] {
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{MenuInput{MenuKey::Down}, MenuInput{MenuKey::Accept}};
        auto terminal = CapturingMenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries, footer) == 1,
            "the menu could not select an entry through the frame-capturing adapter"sv);
        Require((terminal.write_calls == 0) && (terminal.cursor_query_calls == 0),
            "the menu painted terminal text instead of submitting its prepared frame"sv);
        Require(terminal.submitted_frames.size() == 2,
            "the adapter did not receive the initial frame and changed selection"sv);
        const auto &frame = terminal.submitted_frames.back();
        constexpr auto expected = std::array{
            kHeader.front(), "  Alpha"sv, "> Bravo"sv, "  Charlie"sv, "  Delta"sv,
            footer.front(), "Up/Down: Select  Enter: Choose  Esc: Back"sv,
            "Entry 2 of 4  PgUp/PgDn: Scroll  Home/End: First/Last"sv};
        Require(std::ranges::equal(frame.rows, expected, {}, &FrameLine::text),
            "the submitted frame omitted or misplaced menu entries, fixed text, or status"sv);
        Require(terminal.submitted_frames.front().rows.at(1).reverse &&
            !frame.rows.at(1).reverse && frame.rows.at(2).reverse,
            "the submitted frames did not transfer highlighting to the new selection"sv);
        Require(!terminal.active && !terminal.updating,
            "the frame-capturing adapter retained menu ownership after selection"sv);
    });
    passed &= Test("wrapped-row notifications identifying each displayed suffix"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}, {.rows = 3, .columns = 10}};
        constexpr auto text = "abcdefghijklmnop"sv;
        auto lines = std::vector<std::string_view>{};
        const auto result = WriteWrappingText(terminal, text,
            WrappingOptions{.size = {.rows = 3, .columns = 10},
                .continuation_column = 3, .maximum_rows = 3},
            [&lines](const std::string_view suffix) { lines.push_back(suffix); });
        Require(std::ranges::equal(lines, std::array{text, "klmnop"sv}),
            "the row notifications did not identify the first line and its continuation"sv);
        Require(result.remaining.empty() && (result.rows == 2) &&
            (result.stop == WrappingStop::EndOfText),
            "the notified two-row label did not finish"sv);
        Require((terminal.Row(1) == "abcdefghij"sv) && (terminal.Row(2) == "  klmnop"sv),
            "the notified rows did not match the displayed text and indentation"sv);
    });
    passed &= Test("a single-row label reserving space for a caller's ellipsis"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}, {.rows = 1, .columns = 10}};
        const auto result = WriteWrappingText(terminal, "abcdefghijk"sv,
            WrappingOptions{.size = {.rows = 1, .columns = 10},
                .maximum_rows = 1, .trailing_columns = 3});
        Require((result.remaining == "hijk"sv) && (result.rows == 1) &&
            (result.stop == WrappingStop::RowLimit),
            "the writer consumed text reserved for the ellipsis"sv);
        terminal.Write("..."sv);
        Require((terminal.Row(1) == "abcdefg..."sv) &&
            (terminal.QueryCursor() == CursorPosition{1, 10}),
            "the caller's ellipsis did not fit on the same row"sv);
    });
    passed &= Test("a wrap probe preserving ellipsis space on the final indented row"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}, {.rows = 2, .columns = 10}};
        const auto result = WriteWrappingText(terminal, "abcdefghijklmnopqrs"sv,
            WrappingOptions{.size = {.rows = 2, .columns = 10},
                .continuation_column = 3, .maximum_rows = 2, .trailing_columns = 3});
        Require((result.remaining == "pqrs"sv) && (result.rows == 2) &&
            (result.stop == WrappingStop::RowLimit),
            "the wrapped label consumed the final row's reserved columns"sv);
        terminal.Write("..."sv);
        Require((terminal.Row(2) == "  klmno..."sv) &&
            (terminal.QueryCursor() == CursorPosition{2, 10}),
            "the caller's ellipsis did not fit after the wrapped text"sv);
    });
    passed &= Test("a wrap probe stopping when indentation and ellipsis fill the next row"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}, {.rows = 2, .columns = 10}};
        const auto result = WriteWrappingText(terminal, "abcdefghijk"sv,
            WrappingOptions{.size = {.rows = 2, .columns = 10},
                .continuation_column = 8, .maximum_rows = 2, .trailing_columns = 3});
        Require((result.remaining == "k"sv) && (result.rows == 1) &&
            (result.stop == WrappingStop::RowLimit),
            "a probe wrote into columns reserved for indentation and ellipsis"sv);
        Require(terminal.QueryCursor() == CursorPosition{1, 10},
            "the writer probed a continuation row with no text capacity"sv);
    });
    passed &= Test("menu selection preserving the caller's original index"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Down, 2}, MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries) == 1,
            "Down, Down, Up did not select the second entry"sv);
        Require(terminal.frames.back().contains("> Bravo"sv),
            "the selected entry was not marked on screen"sv);
        Require(!terminal.active, "accepting an entry did not release the menu screen"sv);
    });
    passed &= Test("visible selections repainting only changed rows and status"sv, [] {
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::Up}, MenuInput{MenuKey::Down, 2},
            MenuInput{MenuKey::Up}, MenuInput{MenuKey::End},
            MenuInput{MenuKey::Home}, MenuInput{MenuKey::Home},
            MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries, footer) == 0,
            "visible Home and End navigation did not return to the first entry"sv);
        const auto expected_rows = std::array{
            std::set<int>{}, std::set{2, 4, 8}, std::set{3, 4, 8},
            std::set{3, 5, 8}, std::set{2, 5, 8}, std::set<int>{}};
        Require(terminal.clears.front() == 1,
            "the initial menu did not start with a cleared screen"sv);
        for (auto index = std::size_t{}; index < terminal.frames.size(); ++index) {
            const auto &frame = terminal.frames.at(index);
            Require(frame.contains(kHeader.front()) && frame.contains("Alpha"sv) &&
                frame.contains("Bravo"sv) && frame.contains("Charlie"sv) &&
                frame.contains("Delta"sv) && frame.contains(footer.front()),
                std::format("a published selection frame was incomplete:\n{}", frame));
            if (index == 0) {
                continue;
            }
            const auto &expected = expected_rows.at(index - 1);
            Require(terminal.touched_rows.at(index) == expected,
                std::format("selection step {} repainted rows outside the changed entries "
                    "and status, or missed a required row", index));
            Require(terminal.clears.at(index) == 0,
                "a visible selection change cleared the whole screen"sv);
            Require(terminal.status_columns.at(index) ==
                    (expected.empty() ? std::set<int>{} : std::set{7}),
                "a single-digit selection change repainted more than its status digit"sv);
            Require(terminal.presentations.at(index) == (expected.empty() ? 0 : 1),
                "a selection update published the wrong number of frames"sv);
        }
        Require(terminal.frames.at(2).contains("> Charlie"sv) &&
            terminal.frames.at(3).contains("> Bravo"sv) &&
            terminal.frames.at(4).contains("> Delta"sv) &&
            terminal.frames.at(5).contains("> Alpha"sv),
            "selection markers did not follow repeated arrows, Home, and End"sv);
        Require((terminal.frames.at(1) == terminal.frames.at(0)) &&
            (terminal.frames.at(6) == terminal.frames.at(5)),
            "clamped navigation changed the displayed frame"sv);
        Require(!terminal.updating, "accepting an entry left an update unfinished"sv);
    });
    passed &= Test("status entry numbers growing and shrinking across a digit boundary"sv, [] {
        constexpr auto entries = std::array{
            "One"sv, "Two"sv, "Three"sv, "Four"sv, "Five"sv,
            "Six"sv, "Seven"sv, "Eight"sv, "Nine"sv, "Ten"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::Down}, MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 6, .columns = 120}};
        Require(SelectMenuItem(terminal, kHeader, entries, {}, 8) == 8,
            "returning from entry ten did not select entry nine"sv);
        Require(terminal.frames.at(1).contains(
                "Entry 10 of 10  PgUp/PgDn: Scroll  Home/End: First/Last"sv),
            "adding a digit corrupted the status text following the entry number"sv);
        Require(terminal.Row(6) == "Entry 9 of 10  PgUp/PgDn: Scroll  Home/End: First/Last"sv,
            "removing a digit left stale text at the end of the status line"sv);
    });
    passed &= Test("wrapped selection repainting every visible continuation and replacing its hint"sv, [] {
        const auto long_name = std::format("Wrapped entry: {}", std::string(650, 'A'));
        const auto entries = std::array{std::string_view{long_name}, "Second"sv};
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::Down}, MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 13, .columns = 80}};
        Require(SelectMenuItem(terminal, kHeader, entries, footer) == 0,
            "returning to the wrapped entry changed its index"sv);
        const auto expected_rows = std::set{2, 3, 4, 5, 6, 7, 8, 9, 10, 13};
        for (const auto index : std::array{1, 2}) {
            Require(terminal.touched_rows.at(index) == expected_rows,
                "a wrapped selection change omitted a continuation or repainted fixed content"sv);
            Require((terminal.clears.at(index) == 0) &&
                (terminal.presentations.at(index) == 1),
                "a wrapped selection change cleared the screen or published more than once"sv);
        }
        Require(terminal.frames.at(1).contains("> Second"sv) &&
            !terminal.frames.at(1).contains("1: View full name"sv) &&
            !terminal.frames.at(1).contains(">   A"sv),
            "deselecting the wrapped entry left continuation markers or its full-name hint"sv);
        Require(terminal.frames.at(2).contains(">   A"sv) &&
            terminal.frames.at(2).contains("1: View full name"sv),
            "reselecting the wrapped entry did not restore its continuation markers and hint"sv);
        Require((terminal.Row(1) == kHeader.front()) &&
            (terminal.Row(11) == footer.front()),
            "wrapped selection changed the fixed header or footer"sv);
    });
    passed &= Test("scrolling erasing replaced rows and emptying unused rows"sv, [] {
        constexpr auto entries = std::array{
            "A long first entry with a suffix"sv, "Second"sv, "Third"sv, "D"sv};
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::PageDown}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 7, .columns = 40}};
        Require(SelectMenuItem(terminal, kHeader, entries, footer) == 3,
            "Page Down did not reach the final entry"sv);
        Require((terminal.Row(1) == kHeader.front()) && (terminal.Row(5) == footer.front()),
            "scrolling changed the fixed header or footer"sv);
        Require(terminal.Row(2) == "> D"sv,
            "replacing the long first row left characters after the shorter entry"sv);
        Require(terminal.Row(3).empty() && terminal.Row(4).empty(),
            "scrolling to the final entry left old entries in unused rows"sv);
        Require((terminal.clears.at(1) == 0) &&
            (terminal.touched_rows.at(1) == std::set{2, 3, 4, 7}),
            "paging repainted fixed content or failed to replace the viewport and status"sv);
        Require(terminal.frames.back().contains("Entry 4 of 4"sv),
            "the final screen's status did not match its selected entry"sv);
        Require(std::ranges::all_of(terminal.presentations,
                [](const auto count) { return count == 1; }),
            "scrolling published an intermediate drawing instead of one completed frame"sv);
    });
    const auto test_arrow_edges = []<MenuScrollPolicy Scroll>(const std::string_view name,
        const std::string_view first_down, const std::string_view second_down,
        const std::string_view first_up, const std::string_view second_up) {
        return Test(name, [&] {
            constexpr auto entries = std::array{
                "Alpha"sv, "Bravo"sv, "Charlie"sv, "Delta"sv,
                "Echo"sv, "Foxtrot"sv, "Golf"sv, "Hotel"sv};
            constexpr auto footer = std::array{"Fixed footer"sv};
            constexpr auto input = std::array{
                MenuInput{MenuKey::Down, 3}, MenuInput{MenuKey::Down},
                MenuInput{MenuKey::Up}, MenuInput{MenuKey::Up},
                MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
            auto terminal = MenuConsole{input, {.rows = 7, .columns = 40}};
            Require(SelectMenuItem<WidthPolicy::AllModes, Scroll>(
                    terminal, kHeader, entries, footer) == 1,
                "scrolling changed the original index selected by the arrow keys"sv);
            for (auto index = std::size_t{1}; index < terminal.frames.size(); ++index) {
                Require((terminal.clears.at(index) == 0) &&
                    std::ranges::all_of(terminal.touched_rows.at(index), [](const auto row) {
                        return ((row >= 2) && (row <= 4)) || (row == 7);
                    }),
                    "arrow scrolling repainted the header, footer, or controls"sv);
            }
            Require(terminal.frames.at(1).contains(first_down),
                std::format("crossing the lower edge used the wrong viewport offset; "
                    "expected rows:\n{}actual frame:\n{}", first_down, terminal.frames.at(1)));
            Require(terminal.frames.at(2).contains(second_down),
                std::format("the next Down key did not preserve the expected overlapping rows; "
                    "expected rows:\n{}actual frame:\n{}", second_down, terminal.frames.at(2)));
            Require(terminal.frames.at(4).contains(first_up),
                std::format("moving Up to Charlie used the wrong viewport offset; "
                    "expected rows:\n{}actual frame:\n{}", first_up, terminal.frames.at(4)));
            Require(terminal.frames.at(5).contains(second_up),
                std::format("crossing the upper edge did not use the same scrolling policy; "
                    "expected rows:\n{}actual frame:\n{}", second_up, terminal.frames.at(5)));
        });
    };
    passed &= test_arrow_edges.operator()<MenuScrollPolicy::Line>(
        "line scrolling preserving overlapping rows at both viewport edges"sv,
        "  Bravo\n  Charlie\n> Delta\n"sv,
        "  Charlie\n  Delta\n> Echo\n"sv,
        "> Charlie\n  Delta\n  Echo\n"sv,
        "> Bravo\n  Charlie\n  Delta\n"sv);
    passed &= test_arrow_edges.operator()<MenuScrollPolicy::Page>(
        "page scrolling moving the same viewport height at both edges"sv,
        "> Delta\n  Echo\n  Foxtrot\n"sv,
        "  Delta\n> Echo\n  Foxtrot\n"sv,
        "  Alpha\n  Bravo\n> Charlie\n"sv,
        "  Alpha\n> Bravo\n  Charlie\n"sv);
    const auto test_wrapped_edges = []<MenuScrollPolicy Scroll>(const std::string_view name,
        const std::string_view first_down, const std::string_view second_down,
        const std::string_view back_up) {
        return Test(name, [&] {
            constexpr auto entries = std::array{
                "Alpha"sv, "0123456789012345678continued"sv,
                "Bravo"sv, "Charlie"sv, "Delta"sv};
            constexpr auto input = std::array{
                MenuInput{MenuKey::Down, 2}, MenuInput{MenuKey::Down},
                MenuInput{MenuKey::Up, 2}, MenuInput{MenuKey::Accept}};
            auto terminal = MenuConsole{input, {.rows = 6, .columns = 20}};
            Require(SelectMenuItem<WidthPolicy::AllModes, Scroll>(terminal, kHeader, entries) == 1,
                "returning to the wrapped entry changed its original index"sv);
            Require(terminal.frames.at(1).contains(first_down),
                std::format("scrolling past a wrapped entry counted entries instead of displayed rows; "
                    "expected rows:\n{}actual frame:\n{}", first_down, terminal.frames.at(1)));
            Require(terminal.frames.at(2).contains(second_down),
                std::format("scrolling down lost the expected continuation-row offset; "
                    "expected rows:\n{}actual frame:\n{}", second_down, terminal.frames.at(2)));
            Require(terminal.frames.at(3).contains(back_up),
                std::format("scrolling up did not restore the wrapped entry's first row and marker; "
                    "expected rows:\n{}actual frame:\n{}", back_up, terminal.frames.at(3)));
        });
    };
    passed &= test_wrapped_edges.operator()<MenuScrollPolicy::Line>(
        "line scrolling moving through individual wrapped rows in both directions"sv,
        "  012345678901234567\n    8continued\n> Bravo\n"sv,
        "    8continued\n  Bravo\n> Charlie\n"sv,
        "> 012345678901234567\n>   8continued\n  Bravo\n"sv);
    passed &= test_wrapped_edges.operator()<MenuScrollPolicy::Page>(
        "page scrolling counting wrapped rows in both directions"sv,
        "> Bravo\n  Charlie\n  Delta\n"sv,
        "  Bravo\n> Charlie\n  Delta\n"sv,
        "  Alpha\n> 012345678901234567\n>   8continued\n"sv);
    passed &= Test("held arrow keys clamping at both ends of a menu"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Down, 65535}, MenuInput{MenuKey::Up, 65535},
            MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries) == 0,
            "repeated navigation crossed a list boundary"sv);
        Require(terminal.frames.at(1).contains("> Delta"sv),
            "the repeated Down key did not stop at the final entry"sv);
    });
    passed &= Test("Home and End selecting the first and last entries"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Home}, MenuInput{MenuKey::End}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries, {}, 2) == 3,
            "End did not select the final entry"sv);
        Require(terminal.frames.at(1).contains("> Alpha"sv),
            "Home did not select the first entry"sv);
    });
    passed &= Test("an empty menu ignoring Enter and allowing cancellation"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Accept}, MenuInput{MenuKey::Cancel}};
        auto terminal = MenuConsole{input};
        Require(!SelectMenuItem(terminal, kHeader, {}), "an empty menu selected an entry"sv);
        Require(terminal.frames.front().contains("No entries are available."sv),
            "the empty menu did not explain why nothing could be selected"sv);
        Require(!terminal.active, "cancellation did not release the menu screen"sv);
    });
    passed &= Test("a small window remaining cancellable"sv, [] {
        constexpr auto input = std::array{MenuInput{MenuKey::Back}};
        auto terminal = MenuConsole{input, {.rows = 2, .columns = 20}};
        Require(!SelectMenuItem(terminal, kHeader, kEntries),
            "Back did not cancel the unusably small menu"sv);
    });
    passed &= Test("paging by wrapped rows retaining a partly visible selection"sv, [] {
        const auto long_name = std::string(160, 'A');
        const auto entries = std::array{std::string_view{long_name}, "Second"sv, "Third"sv};
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::PageDown}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 7, .columns = 40}};
        Require(SelectMenuItem(terminal, kHeader, entries, footer) == 0,
            "paging replaced a selected entry whose continuation remained visible"sv);
        Require(terminal.frames.back().contains(">   A"sv),
            "the wrapped continuation was not displayed with its selection marker"sv);
        Require((terminal.clears.at(1) == 0) &&
            (terminal.touched_rows.at(1) == std::set{2, 3, 4}),
            "paging with the same selection repainted the unchanged status or fixed content"sv);
    });
    passed &= Test("a held Page Down key stopping at the final entry"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::PageDown, 65535}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries) == 3,
            "repeated Page Down did not select the final entry"sv);
        Require(terminal.frames.back().contains("> Delta"sv),
            "the final entry was not visible after repeated Page Down"sv);
    });
    passed &= Test("the full-name view returning to the selected menu entry"sv, [] {
        const auto long_name = std::format("{}END-OF-NAME", std::string(650, 'A'));
        const auto entries = std::array{std::string_view{long_name}, "Second"sv};
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::Details}, MenuInput{MenuKey::End},
            MenuInput{MenuKey::Home},
            MenuInput{MenuKey::Back}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 9, .columns = 80}};
        Require(SelectMenuItem(terminal, kHeader, entries, footer) == 0,
            "returning from the full-name view changed the selection"sv);
        Require(terminal.frames.front().contains("1: View full name"sv),
            "the truncated preview did not offer the full-name view"sv);
        Require(terminal.frames.at(2).contains("END-OF-NAME"sv),
            "the full-name view could not reach the end of the name"sv);
        Require(!terminal.frames.at(3).contains("END-OF-NAME"sv),
            "returning to the beginning left the name's previous ending on screen"sv);
        for (const auto index : std::array{2, 3}) {
            Require((terminal.clears.at(index) == 0) &&
                std::ranges::all_of(terminal.touched_rows.at(index), [](const auto row) {
                    return ((row >= 2) && (row <= 6)) || (row == 9);
                }),
                "scrolling the full name repainted the header, footer, or controls"sv);
        }
        Require(std::ranges::all_of(terminal.presentations,
                [](const auto count) { return count == 1; }),
            "opening, scrolling, or closing the full-name view published a partial frame"sv);
    });
    passed &= Test("redrawing after an unavailable cursor report"sv, [] {
        constexpr auto header = std::array{"abcdefghijklmnopqrst café"sv};
        constexpr auto input = std::array{MenuInput{MenuKey::Redraw}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 8, .columns = 12}};
        terminal.fail_next_cursor_query = true;
        Require(SelectMenuItem(terminal, header, kEntries) == 0,
            "an unavailable cursor report changed the selected entry"sv);
        Require(!terminal.frames.front().contains("abcdefghi..."sv),
            "the test's unavailable cursor report did not interrupt header clipping"sv);
        Require((terminal.cursor_queries.at(1) != 0) &&
            (terminal.Row(1) == "abcdefghi..."sv),
            "an incompletely drawn header was cached instead of retried on the next frame"sv);
    });
    passed &= Test("widening a menu leaving an unchanged drawing untouched"sv, [] {
        constexpr auto input = std::array{MenuInput{MenuKey::Resize}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        terminal.resize_to = TerminalSize{.rows = 8, .columns = 90};
        Require(SelectMenuItem(terminal, kHeader, kEntries) == 0,
            "widening changed the selected entry"sv);
        Require((terminal.clears.at(1) == 0) && terminal.touched_rows.at(1).empty() &&
            (terminal.presentations.at(1) == 0),
            "widening repainted a menu whose text still occupied the same positions"sv);
        Require(terminal.frames.at(1) == terminal.frames.at(0),
            "widening changed the contents of the unchanged drawing"sv);
    });
    passed &= Test("resizing a menu to one, two, or three columns remaining cancellable"sv, [] {
        constexpr auto input = std::array{MenuInput{MenuKey::Resize}, MenuInput{MenuKey::Cancel}};
        for (const auto columns : std::array{1, 2, 3}) {
            auto terminal = MenuConsole{input};
            terminal.resize_to = TerminalSize{.rows = 8, .columns = columns};
            Require(!SelectMenuItem(terminal, kHeader, kEntries),
                std::format("the {}-column menu did not allow cancellation", columns));
            Require(!terminal.Row(1).empty() &&
                std::cmp_less_equal(terminal.Row(1).size(), columns),
                std::format("the resize message escaped its {}-column row", columns));
            for (auto row = 2; row <= 8; ++row) {
                Require(terminal.Row(row).empty(),
                    std::format("resizing to {} columns left old menu text on row {}", columns, row));
            }
            Require(!terminal.active && !terminal.updating,
                "cancelling the narrow menu retained the temporary screen"sv);
        }
    });
    passed &= Test("increasing menu height moving only the footer and controls"sv, [] {
        constexpr auto footer = std::array{"Fixed footer"sv};
        constexpr auto input = std::array{MenuInput{MenuKey::Resize}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        terminal.resize_to = TerminalSize{.rows = 9, .columns = 80};
        Require(SelectMenuItem(terminal, kHeader, kEntries, footer) == 0,
            "increasing menu height changed the selected entry"sv);
        Require((terminal.clears.at(1) == 0) &&
            (terminal.touched_rows.at(1) == std::set{6, 7, 8, 9}) &&
            (terminal.presentations.at(1) == 1),
            "increasing height repainted the unchanged header or entries"sv);
        Require(terminal.Row(6).empty() && (terminal.Row(7) == footer.front()) &&
            terminal.Row(8).starts_with("Up/Down: Select"sv) &&
            terminal.Row(9).starts_with("Entry 1 of 4"sv),
            "increasing height left the footer, controls, or status in their old rows"sv);
    });
    passed &= Test("resizing a long name using cached widths and preserving selection"sv, [] {
        const auto long_name = std::format("{}END-OF-NAME", std::string(90, 'A'));
        const auto entries = std::array{"First"sv, std::string_view{long_name}};
        constexpr auto input = std::array{MenuInput{MenuKey::Resize}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 8, .columns = 80}};
        terminal.resize_to = TerminalSize{.rows = 8, .columns = 40};
        Require(SelectMenuItem(terminal, kHeader, entries, {}, 1) == 1,
            "resizing changed the selected entry"sv);
        Require(terminal.frames.back().contains("END-OF-NAME"sv),
            "the narrowed viewport lost the end of a fitting label"sv);
        Require((terminal.clears.at(1) == 0) && !terminal.touched_rows.at(1).contains(1) &&
            terminal.touched_rows.at(1).contains(4),
            "narrowing repainted the fixed header or omitted the label's new continuation row"sv);
        Require(terminal.frame_row_invalidations == 1,
            "resizing rewrote the viewport to measure a name whose widths were already cached"sv);
        Require(std::ranges::all_of(terminal.presentations,
                [](const auto count) { return count == 1; }),
            "initial layout or resize published an intermediate measurement or partial frame"sv);
    });
    passed &= Test("an input exception releasing the menu screen"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}};
        auto received_failure = false;
        try {
            std::ignore = SelectMenuItem(terminal, kHeader, kEntries);
        } catch (const InputFailure &) {
            received_failure = true;
        }
        Require(received_failure, "the terminal's input exception did not reach the caller"sv);
        Require(!terminal.active, "exception unwinding did not release the menu screen"sv);
    });
    passed &= Test("an output exception discarding a partly drawn hidden frame"sv, [] {
        auto terminal = MenuConsole{std::span<const MenuInput>{}};
        terminal.Write("Original screen"sv);
        terminal.fail_after_hidden_text = true;
        auto received_failure = false;
        try {
            std::ignore = SelectMenuItem(terminal, kHeader, kEntries);
        } catch (const OutputFailure &) {
            received_failure = true;
        }
        Require(received_failure, "the terminal's output exception did not reach the caller"sv);
        Require(!terminal.active && !terminal.updating,
            "exception unwinding left the menu screen or an update owned"sv);
        Require(terminal.Row(1) == "Original screen"sv,
            "a failed hidden drawing changed the displayed screen"sv);
        Require(terminal.QueryCursor() == CursorPosition{1, 16},
            "discarding the failed frame did not restore the original output cursor"sv);
    });
    return passed;
}
