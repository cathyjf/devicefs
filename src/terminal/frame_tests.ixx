// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#ifdef _WIN32
#include <devicefs/strsafe_compat.h>
#endif

export module devicefs.terminal.frame_tests;

import std;
import devicefs.terminal;
import devicefs.terminal.frame;

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

// This terminal has explicit widths for the Unicode text used by these tests.
// MeasureText identifies complete groups, but the fake never uses their width
// estimates. Each occupied cell records its entire glyph, allowing an overwrite
// of either half of a wide glyph to remove the old glyph's other half as well.
// Output records distinguish text repainted by Flip from unchanged screen cells.
class FrameConsole {
public:
    struct Cell {
        std::string text;
        int first_column;
        int width;
        bool reverse;
    };

    explicit FrameConsole(const TerminalSize size) noexcept : size_{size} {}

    auto Write(std::string_view text) -> void {
        while (!text.empty()) {
            if (text.starts_with("\x1b["sv)) {
                text.remove_prefix(2);
                const auto end = text.find_first_of("HJKXmhl"sv);
                Require(end != std::string_view::npos, "unterminated frame command"sv);
                const auto parameters = text.substr(0, end);
                const auto command = text.at(end);
                text.remove_prefix(end + 1);
                const auto number = parameters.empty() ? 0 :
                    std::stoi(std::string{parameters});
                if (command == 'H') {
                    const auto separator = parameters.find(';');
                    cursor_ = {.row = number == 0 ? 1 : number,
                        .column = separator == std::string_view::npos ? 1 :
                            std::stoi(std::string{parameters.substr(separator + 1)})};
                    pending_wrap_ = false;
                } else if (command == 'm') {
                    Require((number == 0) || (number == 7), "unexpected frame attribute"sv);
                    reverse_ = number == 7;
                } else if (command == 'J') {
                    Require(number == 2, "unexpected screen erasure"sv);
                    cells_.clear();
                    ++screen_erases;
                } else if ((command == 'X') || (command == 'K')) {
                    const auto first = ((command == 'K') && (number == 2)) ?
                        1 : cursor_.column;
                    const auto count = command == 'X' ? std::max(1, number) :
                        size_.columns - first + 1;
                    for (auto column = first; column < (first + count); ++column) {
                        ClearCell(cursor_.row, column);
                        erased.emplace_back(cursor_.row, column);
                    }
                }
                continue;
            }
            const auto end = text.find('\x1b');
            const auto printed = text.substr(0, end);
            for (const auto &group : MeasureText<WidthPolicy::WindowsTerminalGraphemes>(printed)) {
                const auto width = [&] {
                    if ((group.text == "\u0600"sv) || (group.text == "\u0601"sv)) {
                        return 0;
                    }
                    if ((group.text == "日"sv) || (group.text == "日\u0600"sv) ||
                        (group.text == "日\u0601"sv) ||
                        (group.text == "本"sv) ||
                        (group.text == "👩‍💻"sv) || (group.text == "😀"sv)) {
                        return 2;
                    }
                    Require((group.text == "é"sv) || (group.text == "©"sv) ||
                        (group.text.size() == 1),
                        std::format("the frame fixture has no width for {:?}", group.text));
                    return 1;
                }();
                if (pending_wrap_ && (width != 0)) {
                    cursor_ = {.row = cursor_.row + 1, .column = 1};
                    pending_wrap_ = false;
                }
                Require((cursor_.row >= 1) && (cursor_.row <= size_.rows) &&
                    ((cursor_.column + width - 1) <= size_.columns),
                    "frame text escaped its fitted row"sv);
                for (auto offset = 0; offset < width; ++offset) {
                    ClearCell(cursor_.row, cursor_.column + offset);
                }
                for (auto offset = 0; offset < width; ++offset) {
                    cells_.insert_or_assign({cursor_.row, cursor_.column + offset},
                        Cell{std::string{group.text}, cursor_.column, width, reverse_});
                }
                painted.append(group.text);
                cursor_.column += width;
                if (cursor_.column > size_.columns) {
                    cursor_.column = size_.columns;
                    pending_wrap_ = true;
                }
            }
            text.remove_prefix(printed.size());
        }
    }

    [[nodiscard]] auto QueryCursor() const noexcept -> std::optional<CursorPosition> {
        return cursor_;
    }
    auto PresentFrame() noexcept -> void { ++presentations; }
    auto ResetActivity() noexcept -> void {
        painted.clear();
        erased.clear();
        presentations = 0;
        screen_erases = 0;
    }
    auto Resize(const TerminalSize size) -> void {
        const auto removed_rows = std::max(0, size_.rows - size.rows);
        auto retained = decltype(cells_){};
        for (auto &[position, cell] : cells_) {
            const auto row = position.first - removed_rows;
            if ((row >= 1) && (row <= size.rows) && (position.second <= size.columns)) {
                retained.emplace(std::pair{row, position.second}, std::move(cell));
            }
        }
        cells_ = std::move(retained);
        size_ = size;
    }
    [[nodiscard]] auto Row(const int row) const -> std::string {
        auto text = std::string{};
        for (auto column = 1; column <= size_.columns; ++column) {
            const auto found = cells_.find({row, column});
            if (found == cells_.end()) {
                text.push_back(' ');
            } else if (found->second.first_column == column) {
                text.append(found->second.text);
            }
        }
        while (!text.empty() && (text.back() == ' ')) {
            text.pop_back();
        }
        return text;
    }
    [[nodiscard]] auto AllReversed() const -> bool {
        return std::ranges::all_of(cells_, [](const auto &entry) {
            return entry.second.reverse;
        });
    }

    std::string painted;
    std::vector<std::pair<int, int>> erased;
    int presentations = 0;
    int screen_erases = 0;

private:
    auto ClearCell(const int row, const int column) -> void {
        if (const auto found = cells_.find({row, column}); found != cells_.end()) {
            const auto first = found->second.first_column;
            const auto width = found->second.width;
            for (auto offset = 0; offset < width; ++offset) {
                cells_.erase({row, first + offset});
            }
        }
    }
    TerminalSize size_;
    CursorPosition cursor_;
    std::map<std::pair<int, int>, Cell> cells_;
    bool reverse_ = false;
    bool pending_wrap_ = false;
};

}

export [[nodiscard]] auto RunFrameTests() -> bool {
    auto passed = true;
    passed &= Test("changing one digit beside Japanese, joined emoji and a combining accent"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 32}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 32}};
        frame.rows.at(0).text = "日本 👩‍💻 é 1 end";
        Require(presenter.Flip(terminal, frame), "the initial frame was not presented"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "日本 👩‍💻 é 2 end";
        Require(presenter.Flip(terminal, frame), "the changed digit was not presented"sv);
        Require((terminal.painted == "2") && terminal.erased.empty(),
            "changing a digit repainted or erased unchanged neighboring text"sv);
        Require(terminal.Row(1) == frame.rows.at(0).text, "the Unicode row was corrupted"sv);
        terminal.ResetActivity();
        Require(presenter.Flip(terminal, frame) && terminal.painted.empty() &&
            (terminal.presentations == 0), "an unchanged frame produced output"sv);
        frame.rows.at(0).reverse = true;
        Require(presenter.Flip(terminal, frame) && terminal.AllReversed(),
            "changing selection did not update every glyph's attributes"sv);
        Require(terminal.erased.empty() && (terminal.screen_erases == 0),
            "changing attributes erased existing glyphs"sv);
    });
    passed &= Test("shortening a Unicode row clearing displaced text from its blank tail"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 16}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 16}};
        frame.rows.at(0).text = "日本 éXY";
        std::ignore = presenter.Flip(terminal, frame);
        // A console relay can place text beyond the end reported to the
        // application. The next shorter frame must leave that tail blank too.
        terminal.Write("\x1b[1;14Ht)"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "日本";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(1) == "日本", "removed text remained on screen"sv);
        Require(terminal.painted.empty() && !terminal.erased.empty() &&
            std::ranges::all_of(terminal.erased, [](const auto &cell) {
                return (cell.first == 1) && (cell.second > 4);
            }), "tail removal modified the retained Japanese glyphs"sv);
    });
    passed &= Test("replacing a wide glyph preserving a suffix at the same columns"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 16}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 16}};
        frame.rows.at(0).text = "日xZ";
        std::ignore = presenter.Flip(terminal, frame);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abxZ";
        std::ignore = presenter.Flip(terminal, frame);
        Require((terminal.Row(1) == "abxZ") && (terminal.painted == "ab"),
            "replacing a wide glyph damaged or repainted the positioned suffix"sv);
    });
    passed &= Test("a cached wide glyph at the last column and a later terminal resize"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 6}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 6}};
        frame.rows.at(0).text = "😀";
        frame.rows.at(1).text = "日ab😀";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(2) == "日ab😀", "the wide glyph did not fill the final two cells"sv);
        terminal.ResetActivity();
        frame.rows.at(1).text = "日abZ";
        std::ignore = presenter.Flip(terminal, frame);
        Require(terminal.Row(2) == "日abZ", "replacing the final glyph left a stale tail"sv);
        terminal.Resize({.rows = 3, .columns = 8});
        auto resized = FrameBuffer{TerminalSize{.rows = 3, .columns = 8}};
        resized.rows.at(0).text = "😀";
        resized.rows.at(1).text = "日abZ";
        resized.rows.at(2).text = "日本 é";
        std::ignore = presenter.Flip(terminal, resized);
        Require((terminal.Row(1) == "😀") && (terminal.Row(2) == "日abZ") &&
            (terminal.Row(3) == "日本 é"), "resizing corrupted the desired frame"sv);
    });
    passed &= Test("height reduction removing top rows and restoring the complete frame"sv, [] {
        auto terminal = FrameConsole{{.rows = 6, .columns = 24}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 6, .columns = 24}};
        frame.rows.at(0).text = "Header";
        frame.rows.at(1).text = "日本";
        frame.rows.at(3).text = "Old long entry";
        frame.rows.at(4).text = "Old other entry";
        frame.rows.at(5).text = "Footer";
        Require(presenter.Flip(terminal, frame), "the initial frame was not presented"sv);
        terminal.Resize({.rows = 3, .columns = 24});
        Require(terminal.Row(1) == "Old long entry",
            "the fixture did not reproduce removal of the top rows"sv);
        terminal.ResetActivity();
        auto resized = FrameBuffer{TerminalSize{.rows = 3, .columns = 24}};
        resized.rows.front().text = "Header";
        resized.rows.back().text = "Footer";
        Require(presenter.Flip(terminal, resized), "the shorter frame was not presented"sv);
        Require((terminal.Row(1) == "Header") && terminal.Row(2).empty() &&
            (terminal.Row(3) == "Footer"),
            "shrinking the terminal left shifted text or stale tails on screen"sv);
        Require(presenter.KnownTextWidths("日本"sv).has_value(),
            "height reduction discarded character-width measurements"sv);
        Require(terminal.screen_erases == 0,
            "height reduction cleared the screen before repainting"sv);
    });
    passed &= Test("a narrow ambiguous-width character beside the right margin"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 8}};
        frame.rows.at(0).text = "abcdef©X";
        Require(presenter.Flip(terminal, frame) &&
            (terminal.Row(1) == frame.rows.at(0).text),
            "an ambiguous cursor report omitted the fitted row's final character"sv);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abcdef©Y";
        Require(presenter.Flip(terminal, frame) && (terminal.painted == "Y"),
            "changing the last cell repainted the unchanged copyright character"sv);
    });
    passed &= Test("an unmeasured narrow final glyph replacing an old wide glyph"sv, [] {
        auto terminal = FrameConsole{{.rows = 2, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 2, .columns = 8}};
        frame.rows.at(0).text = "abcdef日";
        std::ignore = presenter.Flip(terminal, frame);
        terminal.ResetActivity();
        frame.rows.at(0).text = "abcdef©";
        Require(presenter.Flip(terminal, frame) &&
            (terminal.Row(1) == frame.rows.at(0).text),
            "replacing an unmeasured final glyph damaged it or left the old tail"sv);
        Require(terminal.painted == "©", "the unchanged prefix was repainted"sv);
    });
    passed &= Test("changing and removing zero-width text after a full-width final glyph"sv, [] {
        auto terminal = FrameConsole{{.rows = 1, .columns = 8}};
        auto presenter = DeltaFramePresenter{};
        auto frame = FrameBuffer{TerminalSize{.rows = 1, .columns = 8}};
        frame.rows.front().text = "abcdef日\u0600";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "the fitted row could not be painted"sv);
        terminal.ResetActivity();
        frame.rows.front().text = "abcdef日\u0601";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "changing the zero-width suffix failed"sv);
        Require(terminal.Row(1) == frame.rows.front().text,
            "replacing a zero-width suffix erased the retained wide glyph"sv);
        Require(terminal.painted == "日\u0601",
            "replacing a zero-width suffix repainted the unchanged prefix"sv);
        frame.rows.front().text = "abcdef日";
        Require(presenter.Flip<WidthPolicy::WindowsTerminalGraphemes>(terminal, frame),
            "removing the zero-width suffix failed"sv);
        Require(terminal.Row(1) == "abcdef日", "tail erasure damaged the retained wide glyph"sv);
    });
    return passed;
}
