// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.terminal.menu_tests;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import devicefs.terminal;
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
class MenuConsole {
public:
    explicit MenuConsole(const std::span<const MenuInput> input,
        const TerminalSize size = {.rows = 8, .columns = 80})
        : input_{input}, size_{size} {}

    [[nodiscard]] auto EnterMenu() noexcept {
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

    auto Write(std::string_view text) -> void {
        Require(!active || updating, "menu output occurred outside a screen update"sv);
        if (fail_next_write) {
            fail_next_write = false;
            throw OutputFailure{"the test terminal failed to write the menu"};
        }
        auto &screen = updating ? hidden_ : displayed_;
        auto wrote_text = false;
        while (!text.empty()) {
            if (text.front() == '\x1b') {
                Require(text.starts_with("\x1b["sv), "unexpected escape command"sv);
                text.remove_prefix(2);
                const auto end = text.find_first_of("HJKm@hl"sv);
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
                } else if (command == 'K') {
                    Require(parameters == "2"sv, "unexpected line-erasure command"sv);
                    screen.lines[screen.cursor.row].clear();
                } else if (command == '@') {
                    const auto columns = std::stoi(std::string{parameters});
                    auto &line = screen.lines[screen.cursor.row];
                    for (auto index = 0; index < columns; ++index) {
                        line.insert(std::next(line.begin(),
                            wil::safe_cast_failfast<std::ptrdiff_t>(screen.cursor.column) - 1), ' ');
                    }
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
            fail_next_write = true;
        }
    }

    [[nodiscard]] auto QueryCursor() const -> std::optional<CursorPosition> {
        Require(!active || updating,
            "menu layout queried the displayed cursor outside a hidden update"sv);
        return updating ? hidden_.cursor : displayed_.cursor;
    }

    [[nodiscard]] auto QuerySize() const noexcept -> std::optional<TerminalSize> {
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
        if (input_.empty()) {
            throw InputFailure{"menu requested input after the test's keys were exhausted"};
        }
        const auto input = input_.front();
        input_ = input_.subspan(1);
        if ((input.key == MenuKey::Resize) && resize_to) {
            size_ = *resize_to;
        }
        return input;
    }

    bool active = false;
    bool updating = false;
    bool fail_next_write = false;
    bool fail_after_hidden_text = false;
    std::optional<TerminalSize> resize_to;
    std::vector<std::string> frames;
    std::vector<std::size_t> presentations;

private:
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
};

constexpr auto kHeader = std::array{"Menu regression test"sv};
constexpr auto kEntries = std::array{"Alpha"sv, "Bravo"sv, "Charlie"sv, "Delta"sv};

}

export [[nodiscard]] auto TestMenu() -> bool {
    auto passed = Test("wrapped-row notifications identifying each displayed suffix"sv, [] {
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
    passed &= Test("selection changes publishing one complete frame per update"sv, [] {
        constexpr auto input = std::array{
            MenuInput{MenuKey::Down}, MenuInput{MenuKey::Up},
            MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input};
        Require(SelectMenuItem(terminal, kHeader, kEntries) == 0,
            "Down and Up did not return to the first entry"sv);
        for (auto index = std::size_t{}; index < terminal.frames.size(); ++index) {
            Require(terminal.presentations.at(index) == 1,
                "a menu update published an intermediate drawing or no completed frame"sv);
            const auto &frame = terminal.frames.at(index);
            Require(frame.contains(kHeader.front()) && frame.contains("Alpha"sv) &&
                frame.contains("Bravo"sv) && frame.contains("Charlie"sv) &&
                frame.contains("Delta"sv),
                std::format("a published selection frame was incomplete:\n{}", frame));
        }
        Require(!terminal.updating, "accepting an entry left an update unfinished"sv);
    });
    passed &= Test("scrolling erasing replaced rows and emptying unused rows"sv, [] {
        constexpr auto entries = std::array{
            "A long first entry with a suffix"sv, "Second"sv, "Third"sv, "D"sv};
        constexpr auto input = std::array{
            MenuInput{MenuKey::PageDown}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 6, .columns = 40}};
        Require(SelectMenuItem(terminal, kHeader, entries) == 3,
            "Page Down did not reach the final entry"sv);
        Require(terminal.Row(1) == kHeader.front(),
            "scrolling changed the fixed header"sv);
        Require(terminal.Row(2) == "> D"sv,
            "replacing the long first row left characters after the shorter entry"sv);
        Require(terminal.Row(3).empty() && terminal.Row(4).empty(),
            "scrolling to the final entry left old entries in unused rows"sv);
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
            constexpr auto input = std::array{
                MenuInput{MenuKey::Down, 3}, MenuInput{MenuKey::Down},
                MenuInput{MenuKey::Up}, MenuInput{MenuKey::Up},
                MenuInput{MenuKey::Up}, MenuInput{MenuKey::Accept}};
            auto terminal = MenuConsole{input, {.rows = 6, .columns = 40}};
            Require(SelectMenuItem<WidthPolicy::AllModes, Scroll>(terminal, kHeader, entries) == 1,
                "scrolling changed the original index selected by the arrow keys"sv);
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
        constexpr auto input = std::array{
            MenuInput{MenuKey::PageDown}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 6, .columns = 40}};
        Require(SelectMenuItem(terminal, kHeader, entries) == 0,
            "paging replaced a selected entry whose continuation remained visible"sv);
        Require(terminal.frames.back().contains(">   A"sv),
            "the wrapped continuation was not displayed with its selection marker"sv);
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
        constexpr auto input = std::array{
            MenuInput{MenuKey::Details}, MenuInput{MenuKey::End},
            MenuInput{MenuKey::Back}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 8, .columns = 80}};
        Require(SelectMenuItem(terminal, kHeader, entries) == 0,
            "returning from the full-name view changed the selection"sv);
        Require(terminal.frames.front().contains("1: View full name"sv),
            "the truncated preview did not offer the full-name view"sv);
        Require(terminal.frames.at(2).contains("END-OF-NAME"sv),
            "the full-name view could not reach the end of the name"sv);
        Require(std::ranges::all_of(terminal.presentations,
                [](const auto count) { return count == 1; }),
            "opening, scrolling, or closing the full-name view published a partial frame"sv);
    });
    passed &= Test("resizing recomputing wrapped rows while preserving selection"sv, [] {
        const auto long_name = std::format("{}END-OF-NAME", std::string(90, 'A'));
        const auto entries = std::array{"First"sv, std::string_view{long_name}};
        constexpr auto input = std::array{MenuInput{MenuKey::Resize}, MenuInput{MenuKey::Accept}};
        auto terminal = MenuConsole{input, {.rows = 8, .columns = 80}};
        terminal.resize_to = TerminalSize{.rows = 8, .columns = 40};
        Require(SelectMenuItem(terminal, kHeader, entries, {}, 1) == 1,
            "resizing changed the selected entry"sv);
        Require(terminal.frames.back().contains("END-OF-NAME"sv),
            "the narrowed viewport lost the end of a fitting label"sv);
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
