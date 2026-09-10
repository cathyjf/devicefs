// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.layout;

import std;
import devicefs.terminal;
import devicefs.terminal.safecast;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

struct TextLayout {
    std::vector<std::string_view> rows;
    bool truncated = false;
    bool oversized = false;
};

template <typename T, WidthPolicy Policy = WidthPolicy::AllModes>
concept LayoutTerminal = Terminal<T> && requires(T &terminal, const std::string_view text) {
    { terminal.template KnownTextWidths<Policy>(text) }
        -> std::same_as<std::optional<std::vector<MeasuredCluster>>>;
    { terminal.InvalidateFrameRows(1, 1) } -> std::same_as<void>;
};

// `LayoutText` divides prepared text into display rows for frame construction
// and scrolling. The returned views identify each row within the supplied
// string, which must remain alive while the caller uses those views. `start`
// places the first row; `options.continuation_column` places later rows.
// An optional `row_limit` caps the layout's total rows;
// `options.trailing_columns` reserves space on that final row, and `truncated`
// records omitted text. Without a row limit, the complete text is laid out.
// An empty drawing area returns zero rows. An empty label occupies one blank
// row when a drawing area is available.
// The measurement area is capped at the bottom of the screen. Starting
// coordinates are positive; `WrappingOptions::CheckPreconditions` documents
// the row-count and column-arithmetic preconditions. Violations that reach
// those calculations throw `std::invalid_argument`.
//
// Previously measured character-group widths establish row boundaries in
// memory. Text whose complete width bound fits on one row also needs no output.
// Otherwise, `WriteWrappingText` writes text and observes its wrapping. The
// screen area beginning at `start.row`, with `options.maximum_rows` rows, is
// reused when the layout needs more rows than fit there. Measurements mark
// those screen rows for restoration by the next frame presentation.
// An unavailable or inconsistent cursor report returns `std::nullopt`, allowing
// the caller to obtain fresh dimensions and lay out the text again.
template <WidthPolicy Policy = WidthPolicy::AllModes>
[[nodiscard]] auto LayoutText(LayoutTerminal<Policy> auto &terminal,
    const std::string_view text, const CursorPosition start,
    const WrappingOptions &options, const std::optional<std::size_t> row_limit = std::nullopt)
    -> std::optional<TextLayout> {
    const auto limit = row_limit.value_or(std::numeric_limits<std::size_t>::max());
    if ((limit == 0) || (options.size.rows < 1) || (options.size.columns < 1) ||
        (options.maximum_rows == 0) ||
        (start.row > options.size.rows) || (start.column > options.size.columns)) {
        return TextLayout{.rows = {}, .truncated = !text.empty()};
    }
    if (text.empty()) {
        return TextLayout{.rows = {text}};
    }
    if ((start.row < 1) || (start.column < 1)) {
        throw std::invalid_argument(std::format(
            "text layout requires a positive starting row and column; received row {}, column {}",
            start.row, start.column));
    }
    options.CheckPreconditions();
    const auto scratch_rows = std::min(options.maximum_rows, options.size.rows - start.row + 1);
    auto layout = TextLayout{};
    auto remaining = text;
    if (const auto known_widths = terminal.template KnownTextWidths<Policy>(text)) {
        const auto continuation_capacity =
            std::max(0, options.size.columns - options.continuation_column + 1);
        auto groups = std::span<const MeasuredCluster>{*known_widths};
        while (!groups.empty() && (layout.rows.size() < limit)) {
            auto available_columns = layout.rows.empty() ?
                options.size.columns - start.column + 1 : continuation_capacity;
            const auto final_row = (layout.rows.size() + 1) == limit;
            if (final_row) {
                available_columns -= options.trailing_columns;
            }
            if (groups.front().width_bound > available_columns) {
                if (final_row && (groups.front().width_bound <= continuation_capacity)) {
                    break;
                }
                if (layout.rows.empty() && (groups.front().width_bound <= continuation_capacity)) {
                    layout.rows.push_back(text.substr(0, 0));
                    continue;
                }
                layout.oversized = true;
                break;
            }
            const auto row_begin = remaining;
            while (!groups.empty() && (groups.front().width_bound <= available_columns)) {
                available_columns -= groups.front().width_bound;
                remaining.remove_prefix(groups.front().text.size());
                groups = groups.subspan(1);
            }
            layout.rows.push_back(row_begin.substr(0, row_begin.size() - remaining.size()));
        }
        layout.truncated = !remaining.empty();
        if (layout.rows.empty()) {
            layout.rows.push_back(text.substr(0, 0));
        }
        return layout;
    }
    const auto measured = MeasureText<Policy>(text);
    if (std::ranges::fold_left(measured, 0,
            [](const auto width, const auto &group) { return width + group.width_bound; }) <=
            (options.size.columns - start.column + 1 -
                (limit == 1 ? options.trailing_columns : 0))) {
        layout.rows.push_back(text);
        return layout;
    }
    auto clusters = std::span<const MeasuredCluster>{measured};
    while (!remaining.empty() && (layout.rows.size() < limit)) {
        if (layout.rows.empty()) {
            terminal.InvalidateFrameRows(start.row, scratch_rows);
        }
        const auto measured_rows = layout.rows.size();
        const auto rows_to_measure = std::min(
            FailFastCast<std::size_t>(scratch_rows), limit - measured_rows);
        // CUP (`CSI row;column H`) positions the cursor at the measurement's
        // starting cell. Both coordinates are one-based.
        // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#cursor-positioning
        constexpr auto kPositionCursor = "\x1b[{};{}H"sv;
        terminal.Write(std::format(kPositionCursor, start.row,
            measured_rows == 0 ? start.column : options.continuation_column));
        const auto result = WriteWrappingText(terminal, clusters,
            WrappingOptions{.size = options.size,
                .continuation_column = options.continuation_column,
                .maximum_rows = FailFastCast<int>(rows_to_measure),
                .trailing_columns = rows_to_measure == (limit - measured_rows) ?
                    options.trailing_columns : 0},
            [&layout](const std::string_view suffix) {
                layout.rows.push_back(suffix);
            });
        if (result.stop == WrappingStop::RedrawRequired) {
            return std::nullopt;
        }
        layout.rows.resize(measured_rows + FailFastCast<std::size_t>(result.rows));
        for (auto index = measured_rows; index < layout.rows.size(); ++index) {
            const auto next_row_suffix = (index + 1) < layout.rows.size() ?
                layout.rows.at(index + 1) : result.remaining;
            layout.rows.at(index) = layout.rows.at(index).substr(
                0, layout.rows.at(index).size() - next_row_suffix.size());
        }
        if (result.stop == WrappingStop::OversizedCluster) {
            layout.oversized = true;
            break;
        }
        if (remaining == result.remaining) {
            break;
        }
        remaining = result.remaining;
        while (!clusters.empty() && (clusters.front().text.data() != remaining.data())) {
            clusters = clusters.subspan(1);
        }
    }
    layout.truncated = !remaining.empty();
    if (layout.rows.empty()) {
        layout.rows.push_back(text.substr(0, 0));
    }
    return layout;
}

}
