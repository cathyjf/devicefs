// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.menu;

import std;
import <wil/safecast.h>;
import devicefs.terminal;
import devicefs.terminal.frame;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

enum class MenuKey {
    Up, Down, PageUp, PageDown, Home, End, Accept, Back, Cancel, Details,
    Resize, Redraw,
};

struct MenuInput {
    MenuKey key;
    unsigned int repeat = 1;
};

// Arrow keys reveal the beginning of the selected entry when it leaves the
// viewport. Line scrolling moves only far enough to reveal that row; page
// scrolling moves by the viewport's height. Both policies count wrapped rows
// and apply the same rule when moving upward or downward.
enum class MenuScrollPolicy { Line, Page };

// A menu needs the writer's output and cursor operations, screen dimensions,
// and navigation events. `EnterMenu` returns an owner for the temporary screen;
// destroying that owner restores the screen when selection ends or fails.
// `Flip` presents a completed back buffer. The adapter chooses how that frame
// reaches the display. Layout sometimes measures text by writing to the
// terminal; `BeginUpdate` owns that temporary work, and `InvalidateFrameRows`
// identifies the rows those measurements may overwrite.
template <typename T>
concept MenuTerminal = Terminal<T> && requires(T &terminal,
    const FrameBuffer &frame, const std::string_view text) {
    { terminal.QuerySize() } -> std::same_as<std::optional<TerminalSize>>;
    { terminal.ReadMenuInput() } -> std::same_as<MenuInput>;
    terminal.EnterMenu();
    terminal.BeginUpdate();
    { terminal.template Flip<WidthPolicy::AllModes>(frame) } -> std::same_as<bool>;
    { terminal.template KnownTextWidths<WidthPolicy::AllModes>(text) } ->
        std::same_as<std::optional<std::vector<MeasuredCluster>>>;
    terminal.InvalidateFrameRows(1, 1);
    terminal.InvalidateFrame();
};

}

namespace devicefs::terminal::menu_detail {

constexpr auto kPreviewRows = std::size_t{8};
constexpr auto kTextColumn = 3;
constexpr auto kContinuationColumn = 5;
constexpr auto kMinimumColumns = 12;

struct TextLayout {
    std::vector<std::string_view> rows;
    bool truncated = false;
    bool oversized = false;
};

struct Position {
    std::size_t entry = 0;
    std::size_t row = 0;
    auto operator<=>(const Position &) const = default;
};

[[nodiscard]] auto PrepareLines(const std::span<const std::string_view> lines) {
    auto prepared = std::vector<std::string>{};
    prepared.reserve(lines.size());
    for (const auto line : lines) {
        prepared.push_back(PrepareTerminalText(line));
    }
    return prepared;
}

auto MoveTo(Terminal auto &terminal, const int row, const int column) -> void {
    constexpr auto kPositionCursor = "\x1b[{};{}H"sv;
    terminal.Write(std::format(kPositionCursor, row, column));
}

// Scrolling needs the boundaries of the rows displayed by the terminal. Exact
// group widths retained from earlier drawings let layout find those boundaries
// in memory. A label whose conservative bound fits also needs no measurement.
// Other labels are written to discover where the terminal wraps them. Before
// those writes, `before_measure` lets the caller mark the measurement area for
// repainting. Cached row boundaries remain usable until the width changes.
// Only entries needed for the viewport are laid out; the full-name view lays
// out the selected name when the user asks to see it.
template <WidthPolicy Policy, typename BeforeMeasure>
[[nodiscard]] auto LayoutText(MenuTerminal auto &terminal,
    const std::string_view text, const TerminalSize size,
    const int first_row, const int available_rows, const std::size_t limit,
    const BeforeMeasure &before_measure)
    -> std::optional<TextLayout> {
    auto layout = TextLayout{};
    auto remaining = text;
    if (text.empty()) {
        layout.rows.push_back(text);
        return layout;
    }
    if (const auto known_widths = terminal.template KnownTextWidths<Policy>(text)) {
        auto groups = std::span<const MeasuredCluster>{*known_widths};
        while (!groups.empty() && (layout.rows.size() < limit)) {
            auto available_columns = size.columns -
                (layout.rows.empty() ? kTextColumn : kContinuationColumn) + 1;
            if (groups.front().width_bound > available_columns) {
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
    if ((limit != 0) && (std::ranges::fold_left(measured, 0,
            [](const auto width, const auto &group) { return width + group.width_bound; }) <=
            (size.columns - kTextColumn + 1))) {
        layout.rows.push_back(text);
        return layout;
    }
    auto clusters = std::span<const MeasuredCluster>{measured};
    while (!remaining.empty() && (layout.rows.size() < limit)) {
        if (layout.rows.empty()) {
            std::invoke(before_measure);
        }
        const auto measured_rows = layout.rows.size();
        const auto rows_to_measure = std::min(
            wil::safe_cast_failfast<std::size_t>(available_rows), limit - measured_rows);
        MoveTo(terminal, first_row,
            measured_rows == 0 ? kTextColumn : kContinuationColumn);
        const auto result = WriteWrappingText(terminal, clusters,
            WrappingOptions{.size = size,
                .continuation_column = kContinuationColumn,
                .maximum_rows = wil::safe_cast_failfast<int>(rows_to_measure)},
            [&layout](const std::string_view suffix) {
                layout.rows.push_back(suffix);
            });
        if (result.stop == WrappingStop::RedrawRequired) {
            return std::nullopt;
        }
        layout.rows.resize(measured_rows + wil::safe_cast_failfast<std::size_t>(result.rows));
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

export namespace devicefs::terminal {

// `SelectMenuItem` displays a scrolling list between fixed header and footer
// lines, then returns the original index of the chosen entry. Escape or Ctrl+C
// cancels selection. The caller supplies ordinary UTF-8 strings; the menu
// prepares its own display copies so filtering cannot change entry identity.
// An initial index beyond the list selects its last entry.
//
// Entries wrap with indented continuations. Previews occupy at most eight rows;
// pressing 1 opens a scrolling view of a truncated name. Paging moves through
// rendered rows, retaining selection while any of its rows remain visible.
// Changing the window width recalculates line boundaries. Resizing preserves
// the list's position as far as possible while keeping the selection visible.
// The menu builds each complete frame in a back buffer and gives that frame
// to the adapter's `Flip` operation for presentation.
//
// The adapter and caller's strings must remain alive for this synchronous call.
// The temporary screen is restored on selection, cancellation, or an exception.
// A missing layout report leaves a message and waits for resize, redraw, or
// cancellation; terminal I/O exceptions propagate to the caller.
template <WidthPolicy Policy = WidthPolicy::AllModes,
    MenuScrollPolicy Scrolling = MenuScrollPolicy::Line>
[[nodiscard]] auto SelectMenuItem(MenuTerminal auto &terminal,
    const std::span<const std::string_view> header,
    const std::span<const std::string_view> entries,
    const std::span<const std::string_view> footer = {},
    const std::size_t initial_selection = 0) -> std::optional<std::size_t> {
    using namespace menu_detail;
    const auto prepared_header = PrepareLines(header);
    const auto prepared_entries = PrepareLines(entries);
    const auto prepared_footer = PrepareLines(footer);
    const auto screen = terminal.EnterMenu();
    auto selected_entry = entries.empty() ? 0 : std::min(initial_selection, entries.size() - 1);
    auto viewport_begin = Position{.entry = selected_entry};
    auto entry_layouts = std::vector<std::optional<TextLayout>>(entries.size());
    auto full_name_layout = std::optional<TextLayout>{};
    auto showing_full_name = false;
    auto first_detail_row = std::size_t{};
    auto terminal_size = std::optional<TerminalSize>{};
    auto visible_positions = std::vector<Position>{};
    const auto first_content_row = wil::safe_cast_failfast<int>(header.size()) + 1;
    auto content_rows = 0;
    auto layout_width = 0;

    const auto draw_menu = [&](const int paging = 0, const std::size_t repeat = 1,
        const bool reveal_selection = false) -> bool {
        const auto update = terminal.BeginUpdate();
        if (!terminal_size) {
            terminal_size = terminal.QuerySize();
        }
        visible_positions.clear();
        const auto show_message = [&](const std::string_view text) {
            auto back_buffer = FrameBuffer{terminal_size};
            back_buffer.rows.front() = {.text = std::string{text},
                .clipping = FrameClipping::IfNeeded};
            std::ignore = terminal.template Flip<Policy>(back_buffer);
        };
        if (!terminal_size) {
            show_message("Terminal size unavailable. Ctrl+L: Retry  Esc: Back"sv);
            return false;
        }
        const auto fixed_rows = header.size() + footer.size() + 2;
        if ((terminal_size->columns < kMinimumColumns) ||
            (std::cmp_less_equal(terminal_size->rows, fixed_rows))) {
            show_message("Enlarge the window. Esc: Back"sv);
            return false;
        }
        content_rows = terminal_size->rows - wil::safe_cast_failfast<int>(fixed_rows);
        if (layout_width != terminal_size->columns) {
            entry_layouts.assign(entries.size(), std::nullopt);
            full_name_layout.reset();
            viewport_begin.row = 0;
            first_detail_row = 0;
            layout_width = terminal_size->columns;
        }
        // Some labels need terminal writes to discover their line breaks. Those
        // measurements can overwrite the list area, so the completed frame must
        // restore those rows even when their intended contents have not changed.
        const auto before_measure = [&] {
            terminal.InvalidateFrameRows(first_content_row, content_rows);
        };
        const auto layout_entry = [&](const std::size_t index) -> bool {
            auto &layout = entry_layouts.at(index);
            if (!layout) {
                layout = LayoutText<Policy>(terminal, prepared_entries.at(index), *terminal_size,
                    first_content_row, content_rows, kPreviewRows, before_measure);
            }
            return layout.has_value();
        };
        const auto advance = [&](Position position, const int direction) {
            if (direction > 0) {
                if (layout_entry(position.entry)) {
                    if ((position.row + 1) < entry_layouts.at(position.entry)->rows.size()) {
                        ++position.row;
                    } else if ((position.entry + 1) < entries.size()) {
                        position = {.entry = position.entry + 1};
                    }
                }
            } else if (position.row != 0) {
                --position.row;
            } else if ((position.entry != 0) && layout_entry(position.entry - 1)) {
                --position.entry;
                position.row = entry_layouts.at(position.entry)->rows.size() - 1;
            }
            return position;
        };
        const auto scroll = [&](const int direction, const std::size_t rows) {
            for (auto count = std::size_t{}; count < rows; ++count) {
                const auto next = advance(viewport_begin, direction);
                if (next == viewport_begin) {
                    break;
                }
                viewport_begin = next;
            }
        };
        if (showing_full_name) {
            if (!full_name_layout) {
                full_name_layout = LayoutText<Policy>(terminal, prepared_entries.at(selected_entry),
                    *terminal_size, first_content_row, content_rows,
                    std::numeric_limits<std::size_t>::max(), before_measure);
            }
            if (!full_name_layout) {
                show_message("Layout unavailable. Ctrl+L: Redraw  Esc: Back"sv);
                return false;
            }
            first_detail_row = std::min(first_detail_row, full_name_layout->rows.size() - 1);
        } else if (!entries.empty()) {
            if (paging != 0) {
                scroll(paging, repeat * content_rows);
            }
            const auto target = Position{.entry = selected_entry};
            for (;;) {
                visible_positions.clear();
                auto position = viewport_begin;
                for (auto row = 0; row < content_rows; ++row) {
                    if (!layout_entry(position.entry)) {
                        show_message("Layout unavailable. Ctrl+L: Redraw  Esc: Back"sv);
                        return false;
                    }
                    visible_positions.push_back(position);
                    const auto next = advance(position, 1);
                    if (next == position) {
                        break;
                    }
                    position = next;
                }
                if (!reveal_selection || std::ranges::contains(visible_positions, target)) {
                    break;
                }
                const auto previous_begin = viewport_begin;
                scroll(target < viewport_begin ? -1 : 1,
                    Scrolling == MenuScrollPolicy::Line ? 1 : content_rows);
                if (viewport_begin == previous_begin) {
                    break;
                }
            }
            if ((paging != 0) && !std::ranges::any_of(visible_positions,
                    [selected_entry](const auto position) { return position.entry == selected_entry; })) {
                selected_entry = paging > 0 ? visible_positions.front().entry : visible_positions.back().entry;
            }
        }
        auto back_buffer = FrameBuffer{terminal_size};
        for (auto index = std::size_t{}; index < header.size(); ++index) {
            back_buffer.rows.at(index) = {.text = prepared_header.at(index),
                .clipping = FrameClipping::IfNeeded};
        }
        if (showing_full_name) {
            const auto count = std::min(wil::safe_cast_failfast<std::size_t>(content_rows),
                full_name_layout->rows.size() - first_detail_row);
            for (auto index = std::size_t{}; index < count; ++index) {
                back_buffer.rows.at(header.size() + index) = {
                    .text = std::format("{}{}",
                        (first_detail_row + index) == 0 ? "  "sv : "    "sv,
                        full_name_layout->rows.at(first_detail_row + index))};
            }
        } else if (entries.empty()) {
            back_buffer.rows.at(header.size()) = {.text = "No entries are available.",
                .clipping = FrameClipping::IfNeeded};
        } else {
            for (auto index = std::size_t{}; index < visible_positions.size(); ++index) {
                const auto position = visible_positions.at(index);
                const auto &layout = *entry_layouts.at(position.entry);
                back_buffer.rows.at(header.size() + index) = {
                    .text = std::format("{}{}{}",
                        position.entry == selected_entry ? "> "sv : "  "sv,
                        position.row == 0 ? ""sv : "  "sv,
                        layout.rows.at(position.row)),
                    .reverse = position.entry == selected_entry,
                    .clipping = layout.truncated &&
                        ((position.row + 1) == layout.rows.size()) ?
                            FrameClipping::Ellipsis : FrameClipping::None};
            }
        }
        for (auto index = std::size_t{}; index < footer.size(); ++index) {
            back_buffer.rows.at(back_buffer.rows.size() - footer.size() - 2 + index) = {
                .text = prepared_footer.at(index), .clipping = FrameClipping::IfNeeded};
        }
        back_buffer.rows.at(back_buffer.rows.size() - 2) = {.text = showing_full_name ?
            "Up/Down/PgUp/PgDn: Scroll  Home/End  Esc: Back" :
            "Up/Down: Select  Enter: Choose  Esc: Back",
            .clipping = FrameClipping::IfNeeded};
        back_buffer.rows.back() = {.text = [&] {
            if (showing_full_name) {
                return full_name_layout->oversized ?
                    std::string{"Enlarge the window to fit the next composed character."} :
                    std::format("Full name: lines {}-{} of {}", first_detail_row + 1,
                        std::min(full_name_layout->rows.size(), first_detail_row + content_rows),
                        full_name_layout->rows.size());
            }
            if (entries.empty()) {
                return std::string{"Esc: Back"};
            }
            return std::format("Entry {} of {}{}  PgUp/PgDn: Scroll  Home/End: First/Last", selected_entry + 1,
                entries.size(), entry_layouts.at(selected_entry) && entry_layouts.at(selected_entry)->truncated ?
                    "  1: View full name"sv : ""sv);
        }(), .clipping = FrameClipping::IfNeeded};
        return terminal.template Flip<Policy>(back_buffer);
    };

    auto ready = draw_menu();
    for (;;) {
        const auto input = terminal.ReadMenuInput();
        if ((input.key == MenuKey::Cancel) ||
            ((input.key == MenuKey::Back) && !showing_full_name)) {
            return std::nullopt;
        }
        if ((input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw)) {
            terminal_size.reset();
            if (input.key == MenuKey::Redraw) {
                layout_width = 0;
                terminal.InvalidateFrame();
            }
            ready = draw_menu(0, 1, !showing_full_name);
            continue;
        }
        if (input.key == MenuKey::Back) {
            showing_full_name = false;
            ready = draw_menu();
            continue;
        }
        if (!ready) {
            continue;
        }
        const auto repeat = wil::safe_cast_failfast<std::size_t>(input.repeat);
        auto paging = 0;
        if (showing_full_name) {
            const auto amount = repeat *
                ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown) ?
                    wil::safe_cast_failfast<std::size_t>(content_rows) : 1);
            switch (input.key) {
            case MenuKey::Up:
            case MenuKey::PageUp:
                first_detail_row -= std::min(first_detail_row, amount);
                break;
            case MenuKey::Down:
            case MenuKey::PageDown:
                first_detail_row += std::min(full_name_layout->rows.size() - 1 - first_detail_row, amount);
                break;
            case MenuKey::Home:
                first_detail_row = 0;
                break;
            case MenuKey::End:
                first_detail_row = std::cmp_greater(full_name_layout->rows.size(), content_rows) ?
                    full_name_layout->rows.size() - content_rows : 0;
                break;
            default:
                continue;
            }
        } else if (!entries.empty()) {
            if (input.key == MenuKey::Accept) {
                return selected_entry;
            }
            if ((input.key == MenuKey::Details) && entry_layouts.at(selected_entry) &&
                entry_layouts.at(selected_entry)->truncated) {
                showing_full_name = true;
                first_detail_row = 0;
            } else if ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown)) {
                paging = input.key == MenuKey::PageDown ? 1 : -1;
            } else {
                switch (input.key) {
                case MenuKey::Up:
                    selected_entry -= std::min(selected_entry, repeat);
                    break;
                case MenuKey::Down:
                    selected_entry += std::min(entries.size() - 1 - selected_entry, repeat);
                    break;
                case MenuKey::Home:
                    selected_entry = 0;
                    break;
                case MenuKey::End:
                    selected_entry = entries.size() - 1;
                    break;
                default:
                    continue;
                }
                if (((input.key == MenuKey::Home) || (input.key == MenuKey::End)) &&
                    !std::ranges::contains(visible_positions, Position{.entry = selected_entry})) {
                    viewport_begin = {.entry = selected_entry};
                }
            }
        } else {
            continue;
        }
        ready = draw_menu(paging, repeat,
            (input.key == MenuKey::Up) || (input.key == MenuKey::Down));
    }
}

}
