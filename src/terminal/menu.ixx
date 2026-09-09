// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.menu;

import std;
import <wil/safecast.h>;
import devicefs.terminal;

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
// `BeginUpdate` starts a frame update and returns its cleanup owner.
// `PresentFrame` completes the drawing. The adapter controls how rendering is
// buffered during the update and released when the update ends or fails.
template <typename T>
concept MenuTerminal = Terminal<T> && requires(T &terminal) {
    { terminal.QuerySize() } -> std::same_as<std::optional<TerminalSize>>;
    { terminal.ReadMenuInput() } -> std::same_as<MenuInput>;
    terminal.EnterMenu();
    terminal.BeginUpdate();
    terminal.PresentFrame();
};

}

namespace devicefs::terminal::menu_detail {

constexpr auto kPreviewRows = std::size_t{8};
constexpr auto kTextColumn = 3;
constexpr auto kContinuationColumn = 5;
constexpr auto kEllipsisColumns = 3;
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
    terminal.Write(std::format("\x1b[{};{}H", row, column));
}

// Scrolling needs the boundaries of the rows displayed by the terminal. Layout
// writes a label during the frame update and records those boundaries. The
// menu clears the measurement text before drawing its completed frame.
// The cached rows remain usable until a resize, allowing subsequent navigation
// to replay them without measuring again.
// Only entries needed for the viewport are measured; the full-name view measures
// the selected name when the user asks to see it.
template <WidthPolicy Policy>
[[nodiscard]] auto LayoutText(Terminal auto &terminal,
    const std::string_view text, const TerminalSize size,
    const int first_row, const int available_rows, const std::size_t limit)
    -> std::optional<TextLayout> {
    auto layout = TextLayout{};
    auto remaining = text;
    if (text.empty()) {
        layout.rows.push_back(text);
        return layout;
    }
    const auto measured = MeasureText<Policy>(text);
    auto clusters = std::span<const MeasuredCluster>{measured};
    while (!remaining.empty() && (layout.rows.size() < limit)) {
        const auto first = layout.rows.size();
        const auto count = std::min(
            wil::safe_cast_failfast<std::size_t>(available_rows), limit - first);
        MoveTo(terminal, first_row,
            first == 0 ? kTextColumn : kContinuationColumn);
        const auto result = WriteWrappingText(terminal, clusters,
            WrappingOptions{.size = size,
                .continuation_column = kContinuationColumn,
                .maximum_rows = wil::safe_cast_failfast<int>(count)},
            [&layout](const std::string_view suffix) {
                layout.rows.push_back(suffix);
            });
        if (result.stop == WrappingStop::RedrawRequired) {
            return std::nullopt;
        }
        layout.rows.resize(first + wil::safe_cast_failfast<std::size_t>(result.rows));
        for (auto index = first; index < layout.rows.size(); ++index) {
            const auto next = (index + 1) < layout.rows.size() ?
                layout.rows.at(index + 1) : result.remaining;
            layout.rows.at(index) = layout.rows.at(index).substr(
                0, layout.rows.at(index).size() - next.size());
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

// A header or footer occupies exactly one row. Reserving the ellipsis before
// writing keeps a shortened line's final composed character intact.
template <WidthPolicy Policy>
auto WriteFixedLine(Terminal auto &terminal, const std::string_view text,
    const TerminalSize size, const int row, const int column = 1,
    const bool truncated = false) -> void {
    MoveTo(terminal, row, column);
    const auto result = WriteWrappingText<Policy>(terminal, text,
        WrappingOptions{.size = size, .maximum_rows = 1,
            .trailing_columns = std::min(kEllipsisColumns, size.columns - 1)});
    if ((truncated || !result.remaining.empty()) &&
        (result.stop != WrappingStop::RedrawRequired)) {
        terminal.Write(size.columns >= kEllipsisColumns ? "..."sv : "."sv);
    }
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
// Resizing discards cached line boundaries and reveals the selected entry again.
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
    auto selected = entries.empty() ? 0 : std::min(initial_selection, entries.size() - 1);
    auto top = Position{.entry = selected};
    auto layouts = std::vector<std::optional<TextLayout>>(entries.size());
    auto full_name = std::optional<TextLayout>{};
    auto details = false;
    auto detail_top = std::size_t{};
    auto size = std::optional<TerminalSize>{};
    auto visible = std::vector<Position>{};
    auto first_row = 1;
    auto viewport_rows = 0;

    const auto layout_entry = [&](const std::size_t index) -> bool {
        auto &layout = layouts.at(index);
        if (!layout) {
            layout = LayoutText<Policy>(terminal, prepared_entries.at(index), *size,
                first_row, viewport_rows, kPreviewRows);
        }
        return layout.has_value();
    };
    const auto advance = [&](Position position, const int direction) {
        if (direction > 0) {
            if (layout_entry(position.entry)) {
                if ((position.row + 1) < layouts.at(position.entry)->rows.size()) {
                    ++position.row;
                } else if ((position.entry + 1) < entries.size()) {
                    position = {.entry = position.entry + 1};
                }
            }
        } else if (position.row != 0) {
            --position.row;
        } else if ((position.entry != 0) && layout_entry(position.entry - 1)) {
            --position.entry;
            position.row = layouts.at(position.entry)->rows.size() - 1;
        }
        return position;
    };
    const auto scroll = [&](const int direction, const std::size_t rows) {
        for (auto count = std::size_t{}; count < rows; ++count) {
            const auto next = advance(top, direction);
            if (next == top) {
                break;
            }
            top = next;
        }
    };
    const auto draw = [&](const int paging = 0, const std::size_t repeat = 1,
        const bool reveal_selection = false) -> bool {
        const auto update = terminal.BeginUpdate();
        if (!size) {
            size = terminal.QuerySize();
        }
        visible.clear();
        const auto message = [&](const std::string_view text) {
            terminal.Write("\x1b[0m\x1b[2J\x1b[H"sv);
            if (size) {
                WriteFixedLine<Policy>(terminal, text, *size, 1);
            } else {
                terminal.Write(text);
            }
            terminal.PresentFrame();
        };
        if (!size) {
            message("Terminal size unavailable. Ctrl+L: Retry  Esc: Back"sv);
            return false;
        }
        const auto fixed_rows = header.size() + footer.size() + 2;
        if ((size->columns < kMinimumColumns) ||
            (std::cmp_less_equal(size->rows, fixed_rows))) {
            message("Enlarge the window. Esc: Back"sv);
            return false;
        }
        first_row = wil::safe_cast_failfast<int>(header.size()) + 1;
        viewport_rows = size->rows - wil::safe_cast_failfast<int>(fixed_rows);
        if (details) {
            if (!full_name) {
                full_name = LayoutText<Policy>(terminal, prepared_entries.at(selected),
                    *size, first_row, viewport_rows,
                    std::numeric_limits<std::size_t>::max());
            }
            if (!full_name) {
                message("Layout unavailable. Ctrl+L: Redraw  Esc: Back"sv);
                return false;
            }
            detail_top = std::min(detail_top, full_name->rows.size() - 1);
        } else if (!entries.empty()) {
            if (paging != 0) {
                scroll(paging, repeat * viewport_rows);
            }
            const auto target = Position{.entry = selected};
            for (;;) {
                visible.clear();
                auto position = top;
                for (auto row = 0; row < viewport_rows; ++row) {
                    if (!layout_entry(position.entry)) {
                        message("Layout unavailable. Ctrl+L: Redraw  Esc: Back"sv);
                        return false;
                    }
                    visible.push_back(position);
                    const auto next = advance(position, 1);
                    if (next == position) {
                        break;
                    }
                    position = next;
                }
                if (!reveal_selection || std::ranges::contains(visible, target)) {
                    break;
                }
                const auto old_top = top;
                scroll(target < top ? -1 : 1,
                    Scrolling == MenuScrollPolicy::Line ? 1 : viewport_rows);
                if (top == old_top) {
                    break;
                }
            }
            if ((paging != 0) && !std::ranges::any_of(visible,
                    [selected](const auto position) { return position.entry == selected; })) {
                selected = paging > 0 ? visible.front().entry : visible.back().entry;
            }
        }
        terminal.Write("\x1b[0m\x1b[2J\x1b[H"sv);
        for (auto index = std::size_t{}; index < header.size(); ++index) {
            WriteFixedLine<Policy>(terminal,
                prepared_header.at(index), *size,
                wil::safe_cast_failfast<int>(index) + 1);
        }
        if (details) {
            const auto count = std::min(wil::safe_cast_failfast<std::size_t>(viewport_rows),
                full_name->rows.size() - detail_top);
            for (auto index = std::size_t{}; index < count; ++index) {
                MoveTo(terminal, first_row + wil::safe_cast_failfast<int>(index),
                    (detail_top + index) == 0 ? kTextColumn : kContinuationColumn);
                terminal.Write(full_name->rows.at(detail_top + index));
            }
        } else if (entries.empty()) {
            WriteFixedLine<Policy>(terminal,
                "No entries are available."sv, *size, first_row);
        } else {
            for (auto index = std::size_t{}; index < visible.size(); ++index) {
                const auto position = visible.at(index);
                const auto &layout = *layouts.at(position.entry);
                const auto row = first_row + wil::safe_cast_failfast<int>(index);
                MoveTo(terminal, row, 1);
                terminal.Write(position.entry == selected ? "\x1b[7m> "sv : "  "sv);
                if (position.row != 0) {
                    terminal.Write("  "sv);
                }
                if (layout.truncated && ((position.row + 1) == layout.rows.size())) {
                    WriteFixedLine<Policy>(terminal, layout.rows.at(position.row),
                        *size, row,
                        position.row == 0 ? kTextColumn : kContinuationColumn, true);
                } else {
                    terminal.Write(layout.rows.at(position.row));
                }
                terminal.Write("\x1b[0m"sv);
            }
        }
        for (auto index = std::size_t{}; index < footer.size(); ++index) {
            WriteFixedLine<Policy>(terminal, prepared_footer.at(index), *size,
                size->rows - wil::safe_cast_failfast<int>(footer.size()) - 1 +
                    wil::safe_cast_failfast<int>(index));
        }
        WriteFixedLine<Policy>(terminal, details ?
            "Up/Down/PgUp/PgDn: Scroll  Home/End  Esc: Back"sv :
            "Up/Down: Select  Enter: Choose  Esc: Back"sv,
            *size, size->rows - 1);
        const auto status = [&] {
            if (details) {
                return full_name->oversized ?
                    std::string{"Enlarge the window to fit the next composed character."} :
                    std::format("Full name: lines {}-{} of {}", detail_top + 1,
                        std::min(full_name->rows.size(), detail_top + viewport_rows),
                        full_name->rows.size());
            }
            if (entries.empty()) {
                return std::string{"Esc: Back"};
            }
            return std::format("Entry {} of {}{}  PgUp/PgDn: Scroll  Home/End: First/Last", selected + 1,
                entries.size(), layouts.at(selected) && layouts.at(selected)->truncated ?
                    "  1: View full name"sv : ""sv);
        }();
        WriteFixedLine<Policy>(terminal, status, *size, size->rows);
        terminal.PresentFrame();
        return true;
    };

    auto ready = draw();
    for (;;) {
        const auto input = terminal.ReadMenuInput();
        if ((input.key == MenuKey::Cancel) ||
            ((input.key == MenuKey::Back) && !details)) {
            return std::nullopt;
        }
        if ((input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw)) {
            size.reset();
            layouts.assign(entries.size(), std::nullopt);
            full_name.reset();
            top = {.entry = selected};
            detail_top = 0;
            ready = draw();
            continue;
        }
        if (input.key == MenuKey::Back) {
            details = false;
            ready = draw();
            continue;
        }
        if (!ready) {
            continue;
        }
        const auto repeat = wil::safe_cast_failfast<std::size_t>(input.repeat);
        auto paging = 0;
        if (details) {
            const auto amount = repeat *
                ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown) ?
                    wil::safe_cast_failfast<std::size_t>(viewport_rows) : 1);
            switch (input.key) {
            case MenuKey::Up:
            case MenuKey::PageUp:
                detail_top -= std::min(detail_top, amount);
                break;
            case MenuKey::Down:
            case MenuKey::PageDown:
                detail_top += std::min(full_name->rows.size() - 1 - detail_top, amount);
                break;
            case MenuKey::Home:
                detail_top = 0;
                break;
            case MenuKey::End:
                detail_top = std::cmp_greater(full_name->rows.size(), viewport_rows) ?
                    full_name->rows.size() - viewport_rows : 0;
                break;
            default:
                continue;
            }
        } else if (!entries.empty()) {
            if (input.key == MenuKey::Accept) {
                return selected;
            }
            if ((input.key == MenuKey::Details) && layouts.at(selected) &&
                layouts.at(selected)->truncated) {
                details = true;
                detail_top = 0;
            } else if ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown)) {
                paging = input.key == MenuKey::PageDown ? 1 : -1;
            } else {
                switch (input.key) {
                case MenuKey::Up:
                    selected -= std::min(selected, repeat);
                    break;
                case MenuKey::Down:
                    selected += std::min(entries.size() - 1 - selected, repeat);
                    break;
                case MenuKey::Home:
                    selected = 0;
                    top = {.entry = selected};
                    break;
                case MenuKey::End:
                    selected = entries.size() - 1;
                    top = {.entry = selected};
                    break;
                default:
                    continue;
                }
            }
        } else {
            continue;
        }
        ready = draw(paging, repeat,
            (input.key == MenuKey::Up) || (input.key == MenuKey::Down));
    }
}

}
