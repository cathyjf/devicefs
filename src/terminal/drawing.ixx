// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.drawing;

import std;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.formatting;
import devicefs.terminal.layout;
import devicefs.terminal.safecast;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

// Controls highlighting and truncation for `Frame::WriteLine`.
struct FrameLineOptions {
    // Select whether to add an ellipsis only when text exceeds the row,
    // always add one, or use text already fitted by the caller's layout.
    FrameClipping clipping = FrameClipping::IfNeeded;
    // If true, display affected rows in reverse video (foreground and background
    // swapped), including any text already present on those rows.
    bool highlight = false;
};

// Controls continuation indentation and selection highlighting for `Frame::Write`.
struct FrameWriteOptions {
    // One-based starting column after an automatic wrap. Explicit line feeds
    // return to column one; the initial text starts at the frame's current cursor.
    int continuation_column = 1;
    // If true, display affected rows in reverse video (foreground and background
    // swapped), including any text already present on those rows.
    bool highlight = false;
};

// `FrameWritingResult` identifies why writing stopped and contains the text
// left to process. Pass `remaining` to a later write, such as
// `Write("{}", result.remaining)`, to continue displaying that text.
struct FrameWritingResult {
    PreparedText remaining;
    int rows;
    WrappingStop stop;
};

// `Frame` builds the desired screen through text-writing operations and a
// cursor. The caller passes the completed frame to the adapter's `Flip`
// operation, which compares it with the preceding screen and presents changes.
// Each write begins where preceding operations ended and supplies complete
// composed characters: a letter and its combining accent, or the components
// of a joined emoji, belong in the same call. Highlighting covers whole rows.
//
// Text layout uses `LayoutText` and may write to the terminal to measure where
// text wraps or ends. The adapter must outlive the frame. Build the frame while
// holding the owner returned by `BeginUpdate`, so those measurements and the
// subsequent `Flip` belong to the same display update.
//
// Wrapping stops at the bottom of the frame and returns any unwritten suffix.
// An unavailable layout report makes `Ready` false so the caller can rebuild
// the frame with fresh dimensions. Formatting, allocation, and terminal I/O
// exceptions propagate.
template <typename T, WidthPolicy Policy = WidthPolicy::AllModes>
class Frame : public FrameBuffer {
public:
    Frame(T &terminal, const TerminalSize dimensions)
        : FrameBuffer{dimensions}, terminal_{terminal} {}

    [[nodiscard]] auto Size() const noexcept -> TerminalSize { return *size; }
    [[nodiscard]] auto Ready() const noexcept -> bool { return ready_; }
    [[nodiscard]] auto CurrentRow() const noexcept -> int { return row_; }

    auto MoveTo(const CursorPosition position) noexcept -> void {
        row_ = position.row;
        column_ = position.column;
    }

    // Menus begin at column one and replace the current row and everything
    // below it. Earlier rows remain in the same complete frame for delta comparison.
    auto ClearFromCurrentRow() -> void {
        if ((row_ >= 1) && (row_ <= Size().rows)) {
            std::ranges::fill(std::span{rows}.subspan(
                FailFastCast<std::size_t>(row_ - 1)), FrameLine{});
        }
        column_ = 1;
    }

    // `WriteLine` places formatted text at the current cursor and clips it at
    // the right margin. Explicit line feeds begin new rows at column one;
    // clipped text is discarded rather than wrapped onto those rows. No line
    // feed is added implicitly. For example, `WriteLine("Hello {}\n", name)`
    // writes one clipped line and positions the next write on the following row.
    // Arguments use the same formatting and preparation as `Write`. An optional
    // trailing `FrameLineOptions` selects highlighting and clipping.
    // The result identifies why writing stopped and contains subsequent text
    // left to process.
    template <typename... Arguments>
    auto WriteLine(const TerminalFormatStringWithOptions<FrameLineOptions, Arguments...> format,
        Arguments &&...arguments) -> FrameWritingResult {
        const auto [text, options] = FormatTerminalTextWithOptions<FrameLineOptions>(
            format, std::forward<Arguments>(arguments)...);
        return WritePreparedText(text, [this, options](const std::string_view paragraph) {
            AppendLine({.text = std::string{paragraph}, .reverse = options.highlight,
                .clipping = options.clipping});
            return WrappingResult{.remaining = ready_ ? ""sv : paragraph,
                .rows = ready_ ? 1 : 0,
                .stop = ready_ ? WrappingStop::EndOfText : WrappingStop::RedrawRequired};
        });
    }

    // `Write` adds formatted text at the current cursor and wraps it across
    // subsequent rows. By default, automatic wraps and explicit line feeds
    // continue at column one. For example, `Write("Backup: {}\n", name)` displays
    // a name and positions the next write at the beginning of the following row.
    //
    // String-like and `char` arguments pass through `PrepareTerminalText`, so
    // a newline inside `name` appears as visible `\n` text. Other argument
    // types use their ordinary formatters. The format string supplies the
    // application's deliberate layout; `PreparedText` arguments retain any
    // preparation already performed. The result contains the text left to
    // process and identifies why writing stopped.
    //
    // An optional trailing `FrameWriteOptions` selects continuation indentation
    // and row highlighting. Explicit line feeds begin at column one regardless
    // of the indentation.
    // For example:
    //   frame.Write("Selected backup: {}", backup_name,
    //       FrameWriteOptions{.continuation_column = 3, .highlight = true});
    template <typename... Arguments>
    auto Write(const TerminalFormatStringWithOptions<FrameWriteOptions, Arguments...> format,
        Arguments &&...arguments) -> FrameWritingResult {
        const auto [text, options] = FormatTerminalTextWithOptions<FrameWriteOptions>(
            format, std::forward<Arguments>(arguments)...);
        return WritePreparedText(text, [this, options](const std::string_view paragraph) {
            return WriteParagraph(paragraph, options.continuation_column, options.highlight);
        });
    }

private:
    [[nodiscard]] auto WritePreparedText(const std::string_view text,
        const auto &write_paragraph) -> FrameWritingResult {
        const auto first_row = row_;
        auto remaining = text;
        while (!remaining.empty()) {
            if (!OnScreen()) {
                return {.remaining = {.text = std::string{remaining}},
                    .rows = remaining.size() == text.size() ? 0 : row_ - first_row,
                    .stop = WrappingStop::RowLimit};
            }
            const auto newline = remaining.find('\n');
            const auto paragraph = remaining.substr(0, newline);
            if (!paragraph.empty()) {
                const auto written = std::invoke(write_paragraph, paragraph);
                remaining.remove_prefix(paragraph.size() - written.remaining.size());
                if (written.stop != WrappingStop::EndOfText) {
                    return {.remaining = {.text = std::string{remaining}},
                        .rows = remaining.size() == text.size() ? 0 : row_ - first_row + 1,
                        .stop = written.stop};
                }
            }
            if (newline == std::string_view::npos) {
                break;
            }
            NewLine();
            remaining.remove_prefix(1);
        }
        return {.remaining = {.text = std::string{remaining}},
            .rows = text.empty() ? 0 : row_ - first_row + 1,
            .stop = WrappingStop::EndOfText};
    }

    auto NewLine() noexcept -> void {
        row_ = std::min(row_ + 1, Size().rows + 1);
        column_ = 1;
    }

    [[nodiscard]] auto OnScreen() const noexcept -> bool {
        return (row_ >= 1) && (row_ <= Size().rows);
    }

    [[nodiscard]] auto MeasureCurrentLine() {
        auto measured = terminal_.get().template MeasureFrameLine<Policy>(
            rows.at(FailFastCast<std::size_t>(row_ - 1)), row_, Size());
        ready_ &= measured.has_value();
        return measured;
    }

    [[nodiscard]] auto ResolveColumn() -> bool {
        if (!ready_) {
            return false;
        }
        if (column_) {
            return true;
        }
        const auto measured = MeasureCurrentLine();
        if (!measured) {
            return false;
        }
        auto &line = rows.at(FailFastCast<std::size_t>(row_ - 1));
        line.text.clear();
        for (const auto &group : measured->groups) {
            line.text.append(group.text);
        }
        line.clipping = FrameClipping::None;
        column_ = measured->groups.empty() ? 1 : measured->groups.back().end_column;
        return true;
    }

    auto AppendLine(const FrameLine &addition) -> void {
        if (addition.text.empty() || !OnScreen() || !ResolveColumn()) {
            return;
        }
        if ((*column_ < 1) || (*column_ > Size().columns)) {
            return;
        }
        auto &line = rows.at(FailFastCast<std::size_t>(row_ - 1));
        if (*column_ == 1) {
            line = addition;
        } else {
            const auto prefix = [&]() -> std::optional<std::string> {
                if (line.text.empty()) {
                    return std::string(FailFastCast<std::size_t>(*column_ - 1), ' ');
                }
                const auto measured = MeasureCurrentLine();
                if (!measured) {
                    return std::nullopt;
                }
                auto text = std::string{};
                auto end = 1;
                for (const auto &group : measured->groups) {
                    if (group.end_column > *column_) {
                        break;
                    }
                    text.append(group.text);
                    end = group.end_column;
                }
                text.append(FailFastCast<std::size_t>(*column_ - end), ' ');
                return text;
            }();
            if (!prefix) {
                return;
            }
            line.text = std::format("{}{}", *prefix, addition.text);
            line.clipping = addition.clipping;
        }
        line.reverse = addition.reverse;
        line.clipping_column = *column_;
        column_.reset();
    }

    [[nodiscard]] auto WriteParagraph(const std::string_view text,
        const int continuation_column, const bool reverse) -> WrappingResult {
        if (!ResolveColumn()) {
            return {.remaining = text, .rows = 0, .stop = WrappingStop::RedrawRequired};
        }
        if (*column_ > Size().columns) {
            NewLine();
            column_ = continuation_column;
        }
        if (!OnScreen()) {
            return {.remaining = text, .rows = 0, .stop = WrappingStop::RowLimit};
        }
        const auto layout = LayoutText<Policy>(terminal_.get(), text,
            {.row = row_, .column = *column_},
            {.size = Size(), .continuation_column = continuation_column,
                .maximum_rows = Size().rows - row_ + 1},
            FailFastCast<std::size_t>(Size().rows - row_ + 1));
        if (!layout) {
            ready_ = false;
            return {.remaining = text, .rows = 0, .stop = WrappingStop::RedrawRequired};
        }
        auto consumed = std::size_t{};
        for (auto index = std::size_t{}; index < layout->rows.size(); ++index) {
            if (index != 0) {
                NewLine();
                column_ = continuation_column;
            }
            AppendLine({.text = std::string{layout->rows.at(index)}, .reverse = reverse});
            if (!ready_) {
                break;
            }
            consumed += layout->rows.at(index).size();
        }
        return {.remaining = text.substr(consumed),
            .rows = FailFastCast<int>(layout->rows.size()),
            .stop = !ready_ ? WrappingStop::RedrawRequired :
                (layout->oversized ? WrappingStop::OversizedCluster :
                    (layout->truncated ? WrappingStop::RowLimit : WrappingStop::EndOfText))};
    }

    std::reference_wrapper<T> terminal_;
    int row_ = 1;
    std::optional<int> column_ = 1;
    bool ready_ = true;
};

}
