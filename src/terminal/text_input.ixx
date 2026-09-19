// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.text_input;

import std;
import devicefs.terminal;
import devicefs.terminal.drawing;
import devicefs.terminal.frame;
import devicefs.terminal.formatting;
import devicefs.terminal.menu;
import devicefs.terminal.safecast;
import devicefs.terminal.transcoding;

export namespace devicefs::terminal {

struct TextInputOptions {
    // The dimensions include the border. An omitted width uses the columns
    // remaining after the frame's current position. Resizing the terminal
    // reduces the displayed rectangle when necessary.
    std::optional<int> columns = std::nullopt;
    int rows = 5;
    bool border = true;
};

// Edit UTF-8 text in a rectangular area of a frame. `Draw` places the control
// at the frame's current position, wraps the text, and scrolls vertically to
// keep the caret visible. Wrapping does not insert newlines into the value.
// `Handle` moves or edits at complete composed-character boundaries.
//
// Call `Draw` before handling navigation, and draw again after an edit or
// resize. `EditText` supplies that event loop for an individual field.
class TextInput {
public:
    explicit TextInput(std::string value = {}) noexcept
        : value_{std::move(value)}, cursor_{value_.size()} {}

    [[nodiscard]] auto Value() const noexcept -> const std::string & { return value_; }

    template <typename T, WidthPolicy Policy>
    [[nodiscard]] auto Draw(Frame<T, Policy> &frame, T &terminal,
        const TextInputOptions &options = {}) -> bool {
        const auto start = frame.Position();
        if (!start) {
            return false;
        }
        const auto border = options.border ? 1 : 0;
        const auto columns = std::min(options.columns.value_or(frame.Size().columns),
            frame.Size().columns - start->column + 1);
        const auto rows = std::min(options.rows, frame.Size().rows - start->row + 1);
        const auto width = columns - (2 * border);
        height_ = rows - (2 * border);
        if ((start->column < 1) || (start->row < 1) || (width < 1) || (height_ < 1)) {
            return false;
        }

        auto lines = std::vector<std::string>(1);
        positions_.clear();
        auto offset = std::size_t{};
        auto column = 1;
        // Display text can occupy more bytes than the value, for example when
        // a control character is shown as visible notation. Each position keeps
        // its offset in the original value so editing never uses display bytes.
        for (const auto &group : MeasureText<Policy>(value_)) {
            const auto newline = group.text == "\n";
            const auto display = newline ? std::string{} : PrepareTerminalText(group.text);
            const auto measured = terminal.template MeasureFrameLine<Policy>(
                {.text = display}, start->row, frame.Size());
            if (!measured || measured->shortened) {
                return false;
            }
            const auto cells = measured->groups.empty() ? 0 : measured->groups.back().end_column - 1;
            if (cells > width) {
                return false;
            }
            if (!newline && ((column + cells - 1) > width)) {
                lines.emplace_back();
                column = 1;
            }
            positions_.push_back({offset,
                FailFastCast<int>(lines.size()) - (column > width ? 0 : 1),
                column > width ? 1 : column});
            lines.back().append(display);
            offset += group.text.size();
            column += cells;
            if (newline) {
                lines.emplace_back();
                column = 1;
            }
        }
        if (column > width) {
            lines.emplace_back();
            column = 1;
        }
        positions_.push_back({offset, FailFastCast<int>(lines.size()) - 1, column});
        // Inserting a combining character can merge groups on either side of
        // the insertion. The caret then belongs after the resulting group.
        const auto caret = std::ranges::lower_bound(positions_, cursor_, {}, &Position::offset);
        cursor_ = caret->offset;
        top_ = std::clamp(top_, std::max(0, caret->row - height_ + 1), caret->row);
        top_ = std::min(top_, std::max(0, FailFastCast<int>(lines.size()) - height_));

        const auto edge = options.border
            ? std::views::repeat(std::string_view{"\u2500"}, width) | std::views::join |
                std::ranges::to<std::string>()
            : std::string{};
        for (auto row = 0; row < rows; ++row) {
            frame.MoveTo({start->row + row, start->column});
            if (options.border && ((row == 0) || (row == (rows - 1)))) {
                frame.WriteLine("{}{}{}", row == 0 ? "\u250C" : "\u2514", edge,
                    row == 0 ? "\u2510" : "\u2518");
                continue;
            }
            const auto index = FailFastCast<std::size_t>(top_ + row - border);
            const auto text = index < lines.size() ? std::string_view{lines.at(index)} : std::string_view{};
            frame.WriteLine("{}{}", options.border ? "\u2502" : "", PreparedText{std::string{text}});
            // Positioning the right edge lets the frame writer pad by displayed
            // columns, including when the row contains wide Unicode characters.
            if (options.border) {
                frame.MoveTo({start->row + row, start->column + columns - 1});
                frame.WriteLine("\u2502");
            }
        }
        frame.caret = CursorPosition{start->row + border + caret->row - top_,
            start->column + border + caret->column - 1};
        return frame.Ready();
    }

    // Apply one input event. Enter and cancellation are left to the caller.
    // Return true when the value or caret changed and needs to be drawn again.
    [[nodiscard]] auto Handle(const MenuInput input) -> bool {
        if (positions_.empty()) {
            return false;
        }
        const auto index = FailFastCast<std::size_t>(
            std::ranges::lower_bound(positions_, cursor_, {}, &Position::offset) - positions_.begin());
        const auto &position = positions_.at(index);
        const auto before = cursor_;
        switch (input.key) {
        case MenuKey::Text:
        case MenuKey::Newline: {
            const auto character = input.key == MenuKey::Newline ? U'\n' : input.character;
            const auto text = Transcode<char>(std::u32string_view{&character, 1});
            for (auto repeat = 0u; repeat < input.repeat; ++repeat) {
                value_.insert(cursor_, text.data(), text.size());
                cursor_ += text.size();
            }
            preferred_column_.reset();
            return true;
        }
        case MenuKey::Left:
            cursor_ = positions_.at(index - std::min<std::size_t>(index, input.repeat)).offset;
            break;
        case MenuKey::Right:
            cursor_ = positions_.at(index +
                std::min<std::size_t>(positions_.size() - index - 1, input.repeat)).offset;
            break;
        case MenuKey::Backspace: {
            const auto begin = positions_.at(index - std::min<std::size_t>(index, input.repeat)).offset;
            value_.erase(begin, cursor_ - begin);
            cursor_ = begin;
            preferred_column_.reset();
            return cursor_ != before;
        }
        case MenuKey::Delete: {
            const auto end = positions_.at(index +
                std::min<std::size_t>(positions_.size() - index - 1, input.repeat)).offset;
            value_.erase(cursor_, end - cursor_);
            preferred_column_.reset();
            return end != cursor_;
        }
        case MenuKey::Home:
        case MenuKey::End:
        case MenuKey::Up:
        case MenuKey::Down:
        case MenuKey::PageUp:
        case MenuKey::PageDown: {
            const auto vertical = (input.key != MenuKey::Home) && (input.key != MenuKey::End);
            if (vertical && !preferred_column_) {
                preferred_column_ = position.column;
            }
            const auto distance = std::min<std::int64_t>(positions_.back().row,
                std::int64_t{input.repeat} *
                    (((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown)) ? height_ : 1));
            const auto row = FailFastCast<int>(std::clamp<std::int64_t>(position.row +
                (((input.key == MenuKey::Up) || (input.key == MenuKey::PageUp)) ? -distance :
                    (vertical ? distance : 0)), 0, positions_.back().row));
            const auto desired = input.key == MenuKey::Home ? 1 :
                (input.key == MenuKey::End ? std::numeric_limits<int>::max() : *preferred_column_);
            const auto candidates = std::ranges::equal_range(positions_, row, {}, &Position::row);
            cursor_ = std::ranges::min_element(candidates, {}, [desired](const auto &candidate) {
                return std::abs(candidate.column - desired);
            })->offset;
            if (vertical) {
                return cursor_ != before;
            }
            break;
        }
        default:
            return false;
        }
        preferred_column_.reset();
        return cursor_ != before;
    }

private:
    struct Position {
        std::size_t offset;
        int row;
        int column;
    };
    std::string value_;
    std::size_t cursor_;
    std::vector<Position> positions_;
    int top_ = 0;
    int height_ = 0;
    std::optional<int> preferred_column_;
};

// Display an editable UTF-8 value and return the accepted text, or no value for
// Escape or Ctrl+C. Enter accepts; Shift+Enter inserts a newline. Ctrl+J also
// inserts a newline for terminals that cannot distinguish Shift+Enter.
// Arrow keys, Home/End, Page Up/Down, Backspace, and Delete operate within the
// wrapped text. The initial caret is at the end of `initial`.
//
// `draw_background` receives a `Frame` after each edit or resize. Draw the
// surrounding instructions and leave its cursor where the input area should
// begin. `options` specifies the area's dimensions and optional border; the
// control computes wrapping, caret placement, and scrolling. For example:
//   EditText(terminal, [](auto &frame) { frame.Write("Server:\n"); }, server);
// The caller keeps its existing `EnterScreen` owner alive across this call.
template <WidthPolicy Policy = WidthPolicy::AllModes>
[[nodiscard]] auto EditText(MenuTerminal<Policy> auto &terminal,
    const auto &draw_background, std::string initial = {},
    const TextInputOptions &options = {}) -> std::optional<std::string> try {
    auto editor = TextInput{std::move(initial)};
    auto redraw = true;
    auto ready = false;
    for (;;) {
        if (redraw) {
            const auto update = terminal.BeginUpdate();
            const auto size = terminal.QuerySize();
            if (size) {
                auto frame = Frame<std::remove_reference_t<decltype(terminal)>, Policy>{terminal, *size};
                std::invoke(draw_background, frame);
                ready = editor.Draw(frame, terminal, options);
                ready = ready && terminal.template Flip<Policy>(frame);
                if (!ready) {
                    auto recovery = FrameBuffer{size};
                    recovery.rows.front() = {
                        .text = "Enlarge the window or press Ctrl+L to redraw. Esc: Cancel",
                        .clipping = FrameClipping::IfNeeded};
                    std::ignore = terminal.template Flip<Policy>(recovery);
                }
            } else {
                ready = false;
                auto frame = FrameBuffer{std::nullopt};
                frame.rows.front().text = "Terminal size unavailable. Ctrl+L: Retry    Esc: Cancel";
                std::ignore = terminal.template Flip<Policy>(frame);
            }
            redraw = false;
        }
        const auto input = terminal.ReadTextInput();
        if ((input.key == MenuKey::Back) || (input.key == MenuKey::Cancel)) {
            return std::nullopt;
        }
        if ((input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw)) {
            if (input.key == MenuKey::Redraw) {
                terminal.InvalidateFrame();
            }
            redraw = true;
        } else if (ready && (input.key == MenuKey::Accept)) {
            return editor.Value();
        } else if (ready) {
            redraw = editor.Handle(input);
        }
    }
} catch (const InputCancelled &) {
    return std::nullopt;
}

}
