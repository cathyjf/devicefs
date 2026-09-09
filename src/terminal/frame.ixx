// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.terminal.frame;

import std;
import <wil/resource.h>;
import <wil/safecast.h>;
import devicefs.terminal;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

// `FrameClipping` selects how text may be shortened to fit one display row.
// `None` means the caller has already fitted the text to that row. `IfNeeded`
// allows the presenter to shorten the text and add an ellipsis. `Ellipsis`
// always adds that marker, including when the row fits but the caller omitted
// later text, such as the remainder of a preview.
enum class FrameClipping { None, IfNeeded, Ellipsis };

struct FrameLine {
    std::string text;
    bool reverse = false;
    FrameClipping clipping = FrameClipping::None;
    auto operator==(const FrameLine &) const -> bool = default;
};

// MakeInformationLine builds a footer or status row from separate pieces of
// prepared terminal text. Empty pieces omit optional information without
// leaving extra separators. The completed row is clipped when space is limited.
[[nodiscard]] auto MakeInformationLine(const std::span<const std::string_view> items,
    const std::string_view separator = "    "sv) -> FrameLine {
    return {.text = items |
        std::views::filter([](const auto item) { return !item.empty(); }) |
        std::views::join_with(separator) | std::ranges::to<std::string>(),
        .clipping = FrameClipping::IfNeeded};
}

// `FrameBuffer` describes the desired screen without sending terminal output.
// Each element is one physical row; empty text requests a blank row. The caller
// uses `PrepareTerminalText` before storing text here so the rows contain no
// terminal commands.
// Missing dimensions allow the adapter to display a size-query failure message.
struct FrameBuffer {
    std::optional<TerminalSize> size;
    std::vector<FrameLine> rows;

    explicit FrameBuffer(const std::optional<TerminalSize> dimensions)
        : size{dimensions}, rows(dimensions ?
            wil::safe_cast_failfast<std::size_t>(dimensions->rows) : std::size_t{1}) {}
};

}

namespace devicefs::terminal::frame_detail {

constexpr auto kPositionCursor = "\x1b[{};{}H"sv;
constexpr auto kEraseCells = "\x1b[{}X"sv;
constexpr auto kEraseToEndOfLine = "\x1b[K"sv;
constexpr auto kResetAttributes = "\x1b[0m"sv;
constexpr auto kReverseAttributes = "\x1b[7m"sv;
constexpr auto kClearScreen = "\x1b[0m\x1b[2J\x1b[H"sv;

// FrameOutput collects a flip's drawing commands in a string. A cursor query
// flushes those commands first because the report must reflect preceding
// writes. Updates that use recorded positions can be sent in one write.
template <Terminal T>
class FrameOutput {
public:
    explicit FrameOutput(T &terminal) noexcept : terminal_{terminal} {}

    auto Write(const std::string_view text) -> void {
        pending_.append(text);
        written |= !text.empty();
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        Flush();
        return terminal_.QueryCursor();
    }

    auto Flush() -> void {
        if (!pending_.empty()) {
            terminal_.Write(pending_);
            pending_.clear();
        }
    }

    bool written = false;

private:
    T &terminal_;
    std::string pending_;
};

struct DisplayedGroup {
    std::string text;
    int column;
    int end_column;
    bool uncertain_width = false;
};

struct DisplayedRow {
    FrameLine source;
    std::vector<DisplayedGroup> groups;
    int columns = 0;
    bool shortened = false;
};

}

export namespace devicefs::terminal {

// DeltaFramePresenter turns a complete desired frame into terminal updates.
// It remembers the text and highlighting drawn at each position, so matching
// character groups need no output. Changed groups are overwritten; if a row
// becomes shorter, its blank tail is erased through the right margin. The
// comparison uses complete composed characters for every label.
//
// While drawing previously unseen groups, `Flip` uses cursor movement to measure
// their widths. Recorded widths serve subsequent frames and layouts; upper
// bounds on unmeasured widths determine where text may be shortened. Rows marked
// `FrameClipping::None` rely on the caller's completed layout. `Flip` returns
// false if a cursor report cannot establish a usable position; the affected row
// is retried on the next frame. Terminal I/O exceptions propagate to the caller.
//
// The presenter retains measurements for one terminal session. A font or
// character-width-policy change requires `Invalidate` to discard those
// measurements. A window resize preserves widths but changes which text fits.
class DeltaFramePresenter {
public:
    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto KnownTextWidths(const std::string_view text) const
        -> std::optional<std::vector<MeasuredCluster>> {
        auto groups = MeasureText<Policy>(text);
        for (auto &group : groups) {
            const auto known = widths_.find(group.text);
            if (known == widths_.end()) {
                return std::nullopt;
            }
            group.width_bound = known->second;
        }
        return groups;
    }

    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto Flip(Terminal auto &terminal, const FrameBuffer &frame) -> bool {
        using namespace frame_detail;
        auto failed = wil::scope_exit([this] { Invalidate(); });
        auto output = FrameOutput{terminal};
        if (!initialized_ || !frame.size) {
            output.Write(kClearScreen);
            displayed_.assign(frame.rows.size(), DisplayedRow{});
        }
        // Reducing the height can remove rows from the top of the terminal.
        // Every surviving row may therefore have moved, so the complete frame
        // must be repainted. Character widths remain valid for that repaint.
        if (frame.rows.size() < displayed_.size()) {
            std::ranges::fill(displayed_, std::nullopt);
        }
        displayed_.resize(frame.rows.size(), DisplayedRow{});
        auto complete = true;
        for (auto index = std::size_t{}; index < frame.rows.size(); ++index) {
            const auto &line = frame.rows.at(index);
            auto &previous = displayed_.at(index);
            if (!frame.size) {
                output.Write(line.text);
                continue;
            }
            const auto columns = frame.size->columns;
            if (previous && (previous->source == line) &&
                ((previous->columns == columns) ||
                    (!previous->shortened && (line.clipping != FrameClipping::Ellipsis) &&
                        (previous->groups.empty() ||
                            (!previous->groups.back().uncertain_width &&
                                (previous->groups.back().end_column <= columns)))))) {
                continue;
            }
            auto old = std::exchange(previous, std::nullopt);
            previous = PaintRow<Policy>(output, line,
                wil::safe_cast_failfast<int>(index) + 1, columns, old);
            complete &= previous.has_value();
        }
        if (output.written) {
            output.Write(kResetAttributes);
            output.Flush();
            terminal.PresentFrame();
        }
        initialized_ = true;
        failed.release();
        return complete;
    }

    // `InvalidateRows` discards the cached contents of a range of screen rows,
    // numbered from one. The next `Flip` repaints those rows even if their desired
    // text has not changed. Callers use this when writing outside the presenter,
    // for example when measuring how text wraps.
    auto InvalidateRows(const int first_row, const int count) -> void {
        const auto first = wil::safe_cast_failfast<std::size_t>(first_row - 1);
        const auto end = first + wil::safe_cast_failfast<std::size_t>(count);
        displayed_.resize(std::max(displayed_.size(), end));
        for (auto index = first; index < end; ++index) {
            displayed_.at(index).reset();
        }
    }

    auto Invalidate() -> void {
        std::ranges::fill(displayed_, std::nullopt);
        widths_.clear();
    }

private:
    template <WidthPolicy Policy>
    [[nodiscard]] auto PaintRow(Terminal auto &output, const FrameLine &line,
        const int row, const int columns,
        const std::optional<frame_detail::DisplayedRow> &previous)
        -> std::optional<frame_detail::DisplayedRow> {
        using namespace frame_detail;
        auto next = DisplayedRow{.source = line, .columns = columns};
        const auto old_end = previous ?
            (previous->groups.empty() ? 1 : previous->groups.back().end_column) : columns + 1;
        const auto move = [&](const int column) {
            output.Write(std::format(kPositionCursor, row, column));
        };
        const auto erase = [&](const int column, const int count) {
            if (count > 0) {
                move(column);
                output.Write(kResetAttributes);
                output.Write(std::format(kEraseCells, count));
            }
        };
        auto column = 1;
        const auto paint = [&](const MeasuredCluster &group, const bool final_group)
            -> std::optional<DisplayedGroup> {
            const auto old = previous ? std::ranges::find(previous->groups,
                column, &DisplayedGroup::column) : std::vector<DisplayedGroup>::const_iterator{};
            // `uncertain_width` marks a group whose cursor report could not tell
            // whether the last cell was filled. The cached ending column remains
            // usable while the group is last in a row of the same width. If more
            // text follows, another observation must find its starting column.
            const auto same_text = previous && (old != previous->groups.end()) &&
                (old->text == group.text) && (old->end_column <= (columns + 1)) &&
                (!old->uncertain_width || ((previous->columns == columns) && final_group));
            auto end_column = same_text ? old->end_column : column;
            auto uncertain_width = same_text && old->uncertain_width;
            if (!same_text || (previous->source.reverse != line.reverse)) {
                const auto known = widths_.find(group.text);
                // The old row may have occupied the last cell. A replacement
                // group's cursor report at that column cannot distinguish a free
                // cell from a filled cell awaiting a wrap. Clearing the old cell
                // before drawing the replacement leaves a blank tail when the
                // replacement turns out to be narrower.
                if (!same_text && (known == widths_.end()) &&
                    (group.width_bound >= (columns - column + 1)) &&
                    (old_end > columns)) {
                    erase(columns, 1);
                }
                move(column);
                output.Write(line.reverse ? kReverseAttributes : kResetAttributes);
                output.Write(group.text);
                if (!same_text && (known == widths_.end())) {
                    const auto observed = output.QueryCursor();
                    if (!observed || (observed->row != row) ||
                        (observed->column < column) || (observed->column > columns)) {
                        return std::nullopt;
                    }
                    // A group followed by more text in a fitted row must leave
                    // room for that text. `MeasureText` attaches zero-width text
                    // to visible groups, so each following group needs a cell.
                    // A nonfinal group's last-column report therefore identifies
                    // a free cell rather than a pending wrap.
                    if (!final_group || (observed->column < columns) ||
                        (group.width_bound < (columns - column + 1))) {
                        end_column = observed->column;
                        widths_.emplace(group.text, end_column - column);
                    } else {
                        end_column = columns + 1;
                        uncertain_width = true;
                    }
                } else if (!same_text) {
                    end_column = column + known->second;
                }
            }
            return DisplayedGroup{.text = std::string{group.text},
                .column = column, .end_column = end_column,
                .uncertain_width = uncertain_width};
        };
        const auto groups = MeasureText<Policy>(line.text);
        const auto reserve = [&] {
            if (line.clipping == FrameClipping::None) {
                return 0;
            }
            if (line.clipping == FrameClipping::IfNeeded) {
                auto remaining = columns;
                for (const auto &group : groups) {
                    const auto known = widths_.find(group.text);
                    const auto width = known == widths_.end() ? group.width_bound : known->second;
                    if (width > remaining) {
                        return std::min(3, columns);
                    }
                    remaining -= width;
                }
                return 0;
            }
            return std::min(3, columns);
        }();
        for (auto index = std::size_t{}; index < groups.size(); ++index) {
            const auto &group = groups.at(index);
            const auto known = widths_.find(group.text);
            const auto width = known == widths_.end() ? group.width_bound : known->second;
            if ((column > columns) ||
                ((width > (columns - reserve - column + 1)) &&
                    ((line.clipping != FrameClipping::None) || (known != widths_.end())))) {
                next.shortened = true;
                break;
            }
            auto painted = paint(group, (index + 1) == groups.size());
            if (!painted) {
                return std::nullopt;
            }
            column = painted->end_column;
            next.groups.push_back(std::move(*painted));
        }
        if ((line.clipping == FrameClipping::Ellipsis) ||
            (next.shortened && (line.clipping == FrameClipping::IfNeeded))) {
            for (auto index = 0; index < reserve; ++index) {
                auto painted = paint({.text = "."sv, .width_bound = 1}, (index + 1) == reserve);
                if (!painted) {
                    return std::nullopt;
                }
                column = painted->end_column;
                next.groups.push_back(std::move(*painted));
            }
        }
        // Each frame owns the blank tail through the right margin. Console Host
        // can rewrite our output for an SSH terminal using its own character
        // widths. If the receiving terminal gives the text a different width,
        // a counted erase can leave stray characters. Erase in Line expresses
        // the blank tail without depending on that count.
        // https://github.com/microsoft/terminal/blob/v1.19.10821.0/src/renderer/vt/paint.cpp#L622-L658
        if (column < std::min(old_end, columns + 1)) {
            move(column);
            output.Write(kResetAttributes);
            output.Write(kEraseToEndOfLine);
        }
        return next;
    }

    std::vector<std::optional<frame_detail::DisplayedRow>> displayed_;
    std::map<std::string, int, std::less<>> widths_;
    bool initialized_ = false;
};

}
