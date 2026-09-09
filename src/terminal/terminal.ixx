// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "compat/gsl_suppress.h"

#ifndef _MSC_VER
    #include <terminal/src/types/inc/CodepointWidthDetector.hpp>
#endif

export module devicefs.terminal;

import std;
#ifdef _MSC_VER
    import <terminal/src/types/inc/CodepointWidthDetector.hpp>;
#endif

export import devicefs.terminal.text;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

struct CursorPosition {
    int row = 1;
    int column = 1;
    auto operator==(const CursorPosition &) const -> bool = default;
};

struct TerminalSize {
    int rows;
    int columns;
};

// `WrappingOptions` specifies the space in which `WriteWrappingText` may display
// a label. The first line begins at the current cursor; later lines begin at
// `continuation_column`, counted from one.
// `size` gives the screen dimensions in character cells. `maximum_rows` counts
// the first line and all continuations, up to the bottom of that screen.
// `trailing_columns` reserves space at the end of the final permitted row for
// a caller-supplied continuation marker, such as an ellipsis.
struct WrappingOptions {
    TerminalSize size;
    int continuation_column = 1;
    int maximum_rows = 1;
    int trailing_columns = 0;
};

// `WidthPolicy` selects the width estimate that `MeasureText` assigns to each
// character group. `WriteWrappingText` uses those estimates to combine groups
// into writes that fit on the current row, reducing the number of cursor queries.
// The estimate must cover the width modes used by the displaying terminal.
// `AllModes` covers the Graphemes, Wcswidth, and Console modes of Microsoft's
// width detector. Console mode can give each part of a composed character its
// own column, making this estimate much wider than the displayed composition.
// A caller targeting Windows Terminal's Graphemes mode can instead select
// `WindowsTerminalGraphemes`, which measures the composition as one group.
// Some characters have an ambiguous width that the terminal may render as one
// or two columns. Both policies reserve two columns for those characters.
enum class WidthPolicy {
    AllModes,
    WindowsTerminalGraphemes,
};

enum class WrappingStop {
    EndOfText,
    RowLimit,
    OversizedCluster,
    RedrawRequired,
};

struct WrappingResult {
    std::string_view remaining;
    int rows;
    WrappingStop stop;
};

// The wrapping algorithm writes text and observes where the terminal placed
// the cursor to discover line breaks. An adapter supplies these two operations:
// `Write` accepts UTF-8 text and VT commands, and `QueryCursor` reports the
// displayed cursor's row and column, both counted from one. A report must
// reflect preceding writes so the writer can indent the line that just wrapped.
// An empty report means the cursor position is unavailable. Exceptions from
// either operation propagate to the caller of `WriteWrappingText`.
template <typename T>
concept Terminal = requires(T &terminal, std::string_view text) {
    { terminal.Write(text) } -> std::same_as<void>;
    { terminal.QueryCursor() } -> std::same_as<std::optional<CursorPosition>>;
};

}

namespace devicefs::terminal {

export struct MeasuredCluster {
    std::string_view text;
    int width_bound;
};

// `MeasureText` determines which parts of a label can be written together and
// how much space to reserve for them. Characters can combine into one displayed
// symbol, such as a letter with an accent or an emoji joined from several
// components. Each returned group keeps those components together in one write
// and supplies the width estimate chosen by `Policy`.
//
// Microsoft's detector identifies groups in UTF-16. Decoding the original UTF-8
// alongside those results locates the corresponding bytes, so the returned
// views refer to the caller's original text.
export template <WidthPolicy Policy = WidthPolicy::AllModes>
GSL_SUPPRESS("26496",
    "The analyzer recommends const for locals that are unchanged in the "
    "WindowsTerminalGraphemes instantiation. The AllModes instantiation "
    "updates the same locals while finding group boundaries and summing "
    "widths, so their declarations must permit those updates.")
[[nodiscard]] auto MeasureText(const std::string_view prepared)
    -> std::vector<MeasuredCluster> {
    const auto wide = std::filesystem::path{prepared}.u16string();
    // Windows Terminal's detector uses int for cluster lengths and intermediate
    // width sums. Each UTF-16 code unit can contribute at most two columns to
    // those sums, even when the final composed character is much narrower.
    // Limiting the input length here keeps both quantities within int's range.
    if (wide.size() > (std::numeric_limits<int>::max() / 2)) {
        throw std::length_error("terminal text exceeds the width detector's input limit");
    }
    auto detector = CodepointWidthDetector{};
    detector.SetAmbiguousWidth(2);
    auto wcswidth = std::conditional_t<Policy == WidthPolicy::AllModes,
        CodepointWidthDetector, std::monostate>{};
    if constexpr (Policy == WidthPolicy::AllModes) {
        wcswidth.Reset(TextMeasurementMode::Wcswidth);
    }
    auto cluster = GraphemeState{.beg = wide.data()};
    auto wcs_cluster = GraphemeState{.beg = wide.data()};
    auto grapheme_end = 0;
    auto wcs_end = 0;
    auto conversion = std::mbstate_t{};
    auto remaining = prepared;
    // MSVC 19.52.36725 crashes when an imported function template contains a
    // size_t literal such as 0uz. Explicit size_t initializers in this module's
    // templates preserve the types and values without triggering that defect.
    auto wide_offset = std::size_t{};
    auto measured = std::vector<MeasuredCluster>{};
    auto leading_zero_group = false;
    while (!remaining.empty()) {
        const auto cluster_begin = grapheme_end;
        std::ignore = detector.GraphemeNext(cluster, wide);
        grapheme_end += cluster.len;
        auto grapheme_width = cluster.width;
        if constexpr (Policy == WidthPolicy::AllModes) {
            std::ignore = wcswidth.GraphemeNext(wcs_cluster, wide);
            wcs_end += wcs_cluster.len;
            // Graphemes and Wcswidth can group the same text differently. A
            // write must contain the whole group recognized by either mode;
            // otherwise, the terminal could receive an accent or emoji modifier
            // after already wrapping its base character. Extending the group
            // until both detectors agree on its end keeps those parts together.
            while (grapheme_end != wcs_end) {
                if (grapheme_end < wcs_end) {
                    std::ignore = detector.GraphemeNext(cluster, wide);
                    grapheme_end += cluster.len;
                    grapheme_width += cluster.width;
                } else {
                    std::ignore = wcswidth.GraphemeNext(wcs_cluster, wide);
                    wcs_end += wcs_cluster.len;
                }
            }
        }
        const auto before = remaining;
        auto width_bound = Policy == WidthPolicy::WindowsTerminalGraphemes ?
            std::max(1, cluster.width) : 0;
        auto units = 0;
        while (units < (grapheme_end - cluster_begin)) {
            auto character = char32_t{};
            const auto length = std::mbrtoc32(
                &character, remaining.data(), remaining.size(), &conversion);
            if ((length == 0) || (length > remaining.size())) {
                throw std::invalid_argument(
                    "WriteWrappingText requires UTF-8 text from PrepareTerminalText");
            }
            const auto wide_length = character <= U'\uffff' ?
                std::size_t{1} : std::size_t{2};
            if constexpr (Policy == WidthPolicy::AllModes) {
                const auto scalar = std::u16string_view{wide}.substr(wide_offset, wide_length);
                auto scalar_state = GraphemeState{.beg = scalar.data()};
                std::ignore = detector.GraphemeNext(scalar_state, scalar);
                // The AllModes estimate reserves space for every component of
                // a composition because Console mode can display those parts
                // separately. Each code point contributes at least one column.
                // The detector supplies two where needed, including ambiguous
                // characters and the emoji variation selector that can widen
                // the preceding symbol. This sum covers the space needed when
                // components are separate as well as when they are combined.
                width_bound += std::max(1, scalar_state.width);
            }
            remaining.remove_prefix(length);
            wide_offset += wide_length;
            units += character <= U'\uffff' ? 1 : 2;
        }
        const auto group_text = before.substr(0, before.size() - remaining.size());
        // In grapheme mode, zero-width text shares a position with a visible
        // character at the same cell. Attach a leading zero-width group to the
        // following group, and a trailing one to the preceding group. Repainting
        // either part then preserves the complete cell contents. AllModes already
        // joins trailing zero-width text through its Wcswidth boundaries above.
        if (!measured.empty() && (leading_zero_group ||
                ((Policy == WidthPolicy::WindowsTerminalGraphemes) && (grapheme_width == 0)))) {
            auto &previous = measured.back();
            previous.text = std::string_view{previous.text.data(),
                previous.text.size() + group_text.size()};
            if constexpr (Policy == WidthPolicy::AllModes) {
                previous.width_bound += width_bound;
            } else if (leading_zero_group) {
                previous.width_bound = width_bound;
            }
            leading_zero_group &= grapheme_width == 0;
            continue;
        }
        leading_zero_group = measured.empty() && (grapheme_width == 0);
        measured.push_back({
            .text = group_text,
            .width_bound = width_bound,
        });
    }
    return measured;
}

// `WriteWrappingText` displays a label across a limited number of screen rows,
// indenting every continuation line. The first line begins at the current
// cursor, allowing the caller to place a selection marker or other prefix
// before the label. The caller supplies the screen dimensions, indentation,
// and row limit in `options`.
//
// Labels must first pass through `PrepareTerminalText`. That operation removes
// embedded terminal commands and escapes controls that could disrupt layout.
// The caller must keep the input storage alive while using the result's
// `remaining` view.
//
// Character widths depend on the displaying terminal and its settings. The
// writer therefore observes cursor positions to discover where text wraps.
// To reduce query overhead, text whose width estimate fits the available space
// is written in a batch. Near the right margin, the writer submits one complete
// character group, observes any wrap, and inserts indentation before the text
// on the new line. A label whose entire estimate fits needs only the initial
// cursor query.
//
// The result identifies the exact unwritten suffix and why writing stopped.
// `EndOfText` means the label is complete. `RowLimit` lets the caller continue
// the suffix on another page. `OversizedCluster` means the next complete group
// has an estimated width greater than the continuation line's available columns;
// displaying that group calls for more space or a less conservative width policy.
// `rows` counts the first line and its continuations, and is zero when nothing
// was written.
//
// A resize or an unavailable cursor report can prevent the writer from placing
// further text reliably. The writer then returns `RedrawRequired`. The caller
// can obtain fresh dimensions and redraw the original label, including any
// prefix already displayed, because resizing may have moved earlier text.
// Exceptions from terminal operations, text conversion, or allocation propagate
// to the caller.
//
// A caller that needs to revisit individual rows can supply `line_started`.
// The callback receives the suffix beginning each displayed row; the first
// notification precedes output, and later notifications identify observed or
// explicitly positioned continuations. The result's `rows` counts the rows
// actually written. These positions let a menu cache wrapped rows for scrolling.
//
// This overload accepts consecutive groups returned by `MeasureText`. Keeping
// that measurement allows a caller to resume output across several pages with
// one width calculation. The string overload below measures a label for a
// single call. Both overloads borrow the original text's storage.
export template <typename LineStarted = std::nullptr_t>
[[nodiscard]] auto WriteWrappingText(
    Terminal auto &terminal, const std::span<const MeasuredCluster> measured,
    const WrappingOptions &options, const LineStarted &line_started = nullptr)
    -> WrappingResult {
    const auto prepared = measured.empty() ? ""sv : std::string_view{
        measured.front().text.begin(), measured.back().text.end()};
    const auto &[size, continuation_column, requested_rows, trailing_columns] = options;
    if ((size.rows < 1) || (size.columns < 1) ||
        (continuation_column < 1) || (continuation_column > size.columns) ||
        (requested_rows < 1) || (trailing_columns < 0) ||
        (trailing_columns >= size.columns)) {
        return {.remaining = prepared, .rows = 0, .stop = WrappingStop::RedrawRequired};
    }
    if (prepared.empty()) {
        return {.remaining = prepared, .rows = 0, .stop = WrappingStop::EndOfText};
    }

    const auto start = terminal.QueryCursor();
    if (!start || (start->row < 1) || (start->row > size.rows) ||
        (start->column < 1) || (start->column > size.columns)) {
        return {.remaining = prepared, .rows = 0, .stop = WrappingStop::RedrawRequired};
    }
    const auto maximum_rows = std::min(requested_rows, size.rows - start->row + 1);
    const auto continuation_capacity = size.columns - continuation_column + 1;
    auto remaining = prepared;
    auto clusters = measured;
    auto cursor = *start;
    const auto notify_line = [&](const std::string_view suffix) {
        if constexpr (!std::is_null_pointer_v<LineStarted>) {
            std::invoke(line_started, suffix);
        }
    };
    notify_line(remaining);
    // The last column needs special handling when deciding whether more text
    // fits. With delayed wrapping, the terminal leaves the cursor in the filled
    // cell until the next printable character arrives. A report of that column
    // can therefore mean the cell is empty or already filled. `Uncertain`
    // records that ambiguity so the next write is observed individually.
    // `Filled` records a row completed by inserting indentation, which requires
    // an explicit move to the next row.
    enum class RightMargin { Available, Uncertain, Filled };
    auto margin = cursor.column == size.columns ?
        RightMargin::Uncertain : RightMargin::Available;
    const auto result = [&](const WrappingStop stop) {
        return WrappingResult{
            .remaining = remaining,
            .rows = remaining.size() == prepared.size() ? 0 : cursor.row - start->row + 1,
            .stop = stop,
        };
    };
    const auto observe = [&]() -> std::optional<CursorPosition> {
        const auto observed = terminal.QueryCursor();
        if (!observed ||
            (observed->column < 1) || (observed->column > size.columns) ||
            (observed->row < cursor.row) || ((observed->row - cursor.row) > 1)) {
            return std::nullopt;
        }
        return observed;
    };

    while (!clusters.empty()) {
        // Inserting indentation can push the text to the last column. The
        // cursor-position command used after insertion cancels pending wrapping,
        // so moving to the next row here makes room for the following text.
        if (margin == RightMargin::Filled) {
            if (clusters.front().width_bound > continuation_capacity) {
                return result(WrappingStop::OversizedCluster);
            }
            if ((cursor.row - start->row + 1) == maximum_rows) {
                return result(WrappingStop::RowLimit);
            }
            if (((cursor.row - start->row + 2) == maximum_rows) &&
                (clusters.front().width_bound > (continuation_capacity - trailing_columns))) {
                return result(WrappingStop::RowLimit);
            }
            ++cursor.row;
            cursor.column = continuation_column;
            terminal.Write(std::format("\x1b[{};{}H", cursor.row, cursor.column));
            notify_line(remaining);
            margin = RightMargin::Available;
        }

        auto capacity = margin == RightMargin::Uncertain ? 0 :
            size.columns - cursor.column + 1;
        if ((cursor.row - start->row + 1) == maximum_rows) {
            capacity = std::max(0, capacity - trailing_columns);
        }
        auto batch_size = std::size_t{};
        auto batch_clusters = std::size_t{};
        for (const auto &cluster : clusters) {
            if (cluster.width_bound > capacity) {
                break;
            }
            capacity -= cluster.width_bound;
            batch_size += cluster.text.size();
            ++batch_clusters;
        }
        if (batch_size != 0) {
            terminal.Write(remaining.substr(0, batch_size));
            remaining.remove_prefix(batch_size);
            clusters = clusters.subspan(batch_clusters);
            if (clusters.empty()) {
                return result(WrappingStop::EndOfText);
            }
            const auto observed = observe();
            if (!observed || (observed->row != cursor.row)) {
                return result(WrappingStop::RedrawRequired);
            }
            cursor = *observed;
            margin = cursor.column == size.columns ?
                RightMargin::Uncertain : RightMargin::Available;
            continue;
        }

        if (clusters.front().width_bound > continuation_capacity) {
            return result(WrappingStop::OversizedCluster);
        }
        // The row limit reserves the rest of the screen for the caller. Probing
        // a possible wrap from the final permitted row could overwrite a footer
        // or scroll the display. The writer therefore returns the remaining text
        // once its width estimate no longer guarantees a fit on that row.
        if ((cursor.row - start->row + 1) == maximum_rows) {
            return result(WrappingStop::RowLimit);
        }
        if (((cursor.row - start->row + 2) == maximum_rows) &&
            (clusters.front().width_bound > (continuation_capacity - trailing_columns))) {
            return result(WrappingStop::RowLimit);
        }

        const auto before = remaining;
        terminal.Write(clusters.front().text);
        remaining.remove_prefix(clusters.front().text.size());
        clusters = clusters.subspan(1);
        const auto observed = observe();
        if (!observed) {
            return result(WrappingStop::RedrawRequired);
        }
        if (observed->row != cursor.row) {
            notify_line(before);
        }
        if ((observed->row != cursor.row) && (continuation_column > 1)) {
            // A continuation row needs indentation, but the terminal has already
            // wrapped the new text to column one. Move to that row's beginning
            // and use the VT insert-character command (ICH) to shift the text
            // right by the indentation width. ICH does not move the cursor, so
            // a cursor-position command (CUP) then places the cursor after the
            // shifted text. The group's width bound reserved enough space for
            // this shift. Microsoft's text-modification reference defines ICH:
            // https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification
            if (observed->column > (continuation_capacity + 1)) {
                return result(WrappingStop::RedrawRequired);
            }
            margin = observed->column == (continuation_capacity + 1) ?
                RightMargin::Filled : RightMargin::Available;
            cursor = {.row = observed->row, .column = margin == RightMargin::Filled ?
                size.columns : observed->column + (continuation_column - 1)};
            terminal.Write(std::format("\x1b[{};1H\x1b[{}@\x1b[{};{}H",
                cursor.row, continuation_column - 1,
                cursor.row, cursor.column));
        } else {
            cursor = *observed;
            margin = cursor.column == size.columns ?
                RightMargin::Uncertain : RightMargin::Available;
        }
    }
    return result(WrappingStop::EndOfText);
}

export template <WidthPolicy Policy = WidthPolicy::AllModes,
    typename LineStarted = std::nullptr_t>
[[nodiscard]] auto WriteWrappingText(Terminal auto &terminal,
    const std::string_view prepared, const WrappingOptions &options,
    const LineStarted &line_started = nullptr) -> WrappingResult {
    const auto measured = MeasureText<Policy>(prepared);
    return WriteWrappingText(terminal, std::span<const MeasuredCluster>{measured},
        options, line_started);
}

}
