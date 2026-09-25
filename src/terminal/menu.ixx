// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include "compat/gsl_suppress.h"

export module devicefs.terminal.menu;

import std;
import devicefs.terminal.safecast;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.layout;
import devicefs.terminal.drawing;
import devicefs.terminal.formatting;
import devicefs.terminal.vt;

using namespace std::string_view_literals;

export namespace devicefs::terminal {

enum class MenuKey {
    Up, Down, PageUp, PageDown, Home, End, Accept, Back, Cancel, Details,
    Resize, Redraw, SwitchArea, Timeout,
    Left, Right, Backspace, Delete, Newline, Text,
};

struct MenuInput {
    MenuKey key;
    unsigned int repeat = 1;
    char32_t character = U'\0';
};

// Arrow keys reveal the beginning of the selected entry when it leaves the
// viewport. Line scrolling moves only far enough to reveal that row; page
// scrolling moves by the viewport's height. Both policies count wrapped rows
// and apply the same rule when moving upward or downward.
enum class MenuScrollPolicy { Line, Page };

// A menu needs the writer's output and cursor operations, screen dimensions,
// and navigation events. The caller owns the screen lifetime across menu calls.
// `Flip` presents a completed back buffer. The adapter chooses how that frame
// reaches the display. Layout sometimes measures text by writing to the
// terminal; `BeginUpdate` owns that temporary work, and `InvalidateFrameRows`
// identifies the rows those measurements may overwrite.
template <typename T, WidthPolicy Policy = WidthPolicy::AllModes>
concept MenuTerminal = FrameTerminal<T> && requires(T &terminal,
    const FrameBuffer &frame, const FrameLine &line, const std::string_view text) {
    { terminal.QuerySize() } -> std::same_as<std::optional<TerminalSize>>;
    { terminal.ReadMenuInput() } -> std::same_as<MenuInput>;
    { terminal.BeginUpdate() } -> std::destructible;
    { terminal.template Flip<Policy>(frame) } -> std::same_as<bool>;
    { terminal.template KnownTextWidths<Policy>(text) } ->
        std::same_as<std::optional<std::vector<MeasuredCluster>>>;
    terminal.template MeasureFrameLine<Policy>(line, 1, TerminalSize{});
    { terminal.InvalidateFrameRows(1, 1) } -> std::same_as<void>;
    { terminal.InvalidateFrame() } -> std::same_as<void>;
};

}

namespace devicefs::terminal::menu_detail {

constexpr auto kPreviewRows = std::size_t{8};
constexpr auto kTextColumn = 3;
constexpr auto kContinuationColumn = 5;
constexpr auto kMinimumColumns = 12;
constexpr auto kInformationRows = 2;
constexpr auto kLayoutUnavailable = std::array{
    "Layout unavailable."sv, "Ctrl+L: Redraw"sv, "Esc: Back"sv};

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

struct MenuText {
    std::vector<std::string> entries;
    std::vector<std::string> footer;
};

struct MenuViewport {
    TerminalSize size;
    int first_row;
    int rows;
};

[[nodiscard]] auto MakeMenuViewport(const TerminalSize size, const int first_row,
    const MenuText &text) noexcept -> std::optional<MenuViewport> {
    const auto fixed_rows = FailFastCast<int>(text.footer.size()) + kInformationRows;
    if ((size.columns < kMinimumColumns) ||
        ((size.rows - first_row + 1) <= fixed_rows)) {
        return std::nullopt;
    }
    return MenuViewport{.size = size, .first_row = first_row,
        .rows = size.rows - first_row + 1 - fixed_rows};
}

// `MenuListView` manages selection and scrolling through the menu's entries.
// One entry can occupy several rows, so moving through displayed rows is a
// different operation from selecting the next entry. Paging can leave only an
// entry's continuation visible. Preview layouts belong to this view and are
// calculated as scrolling encounters entries. `Prepare` finds the rows for the
// next frame; `Draw` writes those rows and hints into the complete frame.
class MenuListView {
public:
    MenuListView(const std::span<const std::string> entries, const std::size_t initial)
        : entries_(entries),
          selected_entry_(entries.empty() ? 0 : std::min(initial, entries.size() - 1)),
          viewport_begin_{.entry = selected_entry_},
          entry_layouts_(entries.size()) {}

    auto SetWidth(const int columns) -> void {
        if (layout_width_ != columns) {
            entry_layouts_.assign(entries_.size(), std::nullopt);
            viewport_begin_.row = 0;
            layout_width_ = columns;
        }
    }

    auto InvalidateLayout() noexcept -> void { layout_width_ = 0; }

    [[nodiscard]] auto SelectedIndex() const noexcept { return selected_entry_; }

    [[nodiscard]] auto CanViewFullName() const -> bool {
        return !entries_.empty() && entry_layouts_.at(selected_entry_) &&
            entry_layouts_.at(selected_entry_)->truncated;
    }

    [[nodiscard]] auto Navigate(const MenuInput input) -> bool {
        if (entries_.empty()) {
            return false;
        }
        const std::size_t repeat = input.repeat;
        switch (input.key) {
        case MenuKey::Up:
            selected_entry_ -= std::min(selected_entry_, repeat);
            break;
        case MenuKey::Down:
            selected_entry_ += std::min(entries_.size() - 1 - selected_entry_, repeat);
            break;
        case MenuKey::Home:
            selected_entry_ = 0;
            break;
        case MenuKey::End:
            selected_entry_ = entries_.size() - 1;
            break;
        case MenuKey::PageUp:
        case MenuKey::PageDown:
            return true;
        default:
            return false;
        }
        if (((input.key == MenuKey::Home) || (input.key == MenuKey::End)) &&
            !std::ranges::contains(visible_positions_, Position{.entry = selected_entry_})) {
            viewport_begin_ = {.entry = selected_entry_};
        }
        return true;
    }

    template <WidthPolicy Policy, MenuScrollPolicy Scrolling>
    [[nodiscard]] auto Prepare(MenuTerminal<Policy> auto &terminal,
        const MenuViewport viewport, const MenuInput input) -> bool {
        if (entries_.empty()) {
            return true;
        }
        if (initial_viewport_pending_) {
            if (!PrepareInitialViewport<Policy>(terminal, viewport)) {
                return false;
            }
            initial_viewport_pending_ = false;
            return true;
        }
        const auto paging = input.key == MenuKey::PageDown ? 1 :
            (input.key == MenuKey::PageUp ? -1 : 0);
        if (paging != 0) {
            Scroll<Policy>(terminal, viewport, paging,
                input.repeat * FailFastCast<std::size_t>(viewport.rows));
        }
        const auto reveal_selection = (input.key == MenuKey::Up) ||
            (input.key == MenuKey::Down) || (input.key == MenuKey::Resize) ||
            (input.key == MenuKey::Redraw);
        const auto target = Position{.entry = selected_entry_};
        for (;;) {
            if (!FillViewport<Policy>(terminal, viewport)) {
                return false;
            }
            if (!reveal_selection || std::ranges::contains(visible_positions_, target)) {
                break;
            }
            const auto previous_begin = viewport_begin_;
            Scroll<Policy>(terminal, viewport, target < viewport_begin_ ? -1 : 1,
                Scrolling == MenuScrollPolicy::Line ? 1 : viewport.rows);
            if (viewport_begin_ == previous_begin) {
                break;
            }
        }
        if ((paging != 0) && !std::ranges::any_of(visible_positions_,
                [this](const auto position) { return position.entry == selected_entry_; })) {
            selected_entry_ = paging > 0 ?
                visible_positions_.front().entry : visible_positions_.back().entry;
        }
        return true;
    }

    auto DrawEntries(auto &frame, const MenuViewport viewport, const bool focused = true) const -> void {
        frame.MoveTo({.row = viewport.first_row});
        if (entries_.empty()) {
            frame.WriteLine("No entries are available."sv);
        }
        for (auto index = std::size_t{}; index < visible_positions_.size(); ++index) {
            const auto position = visible_positions_.at(index);
            const auto &layout = *entry_layouts_.at(position.entry);
            frame.MoveTo({.row = viewport.first_row + FailFastCast<int>(index)});
            frame.WriteLine("{}{}{}",
                position.entry == selected_entry_ ? "> "sv : "  "sv,
                position.row == 0 ? ""sv : "  "sv,
                PreparedText{.text = std::string{layout.rows.at(position.row)}},
                FrameLineOptions{.clipping = layout.truncated &&
                        ((position.row + 1) == layout.rows.size()) ?
                            FrameClipping::Ellipsis : FrameClipping::None,
                    .highlight = focused && (position.entry == selected_entry_)});
        }
    }

    auto Draw(auto &frame, const MenuViewport viewport) const -> void {
        DrawEntries(frame, viewport);
        frame.MoveTo({.row = viewport.size.rows - kInformationRows + 1});
        frame.WriteLine("{}", MakeInformationLine(
            std::array{"Up/Down: Select"sv, "Enter: Choose"sv, "Esc: Back"sv}));
        frame.Write("\n"sv);
        frame.WriteLine("{}", [this] {
            if (entries_.empty()) {
                return MakeInformationLine(std::array{"Esc: Back"sv});
            }
            const auto position = std::format("Entry {} of {}", selected_entry_ + 1, entries_.size());
            return MakeInformationLine(std::array{
                std::string_view{position},
                CanViewFullName() ? "1: View full name"sv : ""sv,
                "PgUp/PgDn: Scroll"sv,
                "Home/End: First/Last"sv,
            });
        }());
    }

private:
    // The initial menu view displays every entry when the complete list fits.
    // Otherwise, center the selected entry's preview. If exact centering is
    // impossible, leave one more row below the preview than above it. Near
    // either end of the list, shift the view to fill the available rows.
    template <WidthPolicy Policy>
    [[nodiscard]] auto PrepareInitialViewport(MenuTerminal<Policy> auto &terminal,
        const MenuViewport viewport) -> bool {
        if (!EnsureLayout<Policy>(terminal, viewport, selected_entry_)) {
            return false;
        }
        viewport_begin_ = {.entry = selected_entry_};
        const auto available_rows = FailFastCast<std::size_t>(viewport.rows);
        const auto selected_rows = entry_layouts_.at(selected_entry_)->rows.size();
        Scroll<Policy>(terminal, viewport, -1,
            (available_rows - std::min(available_rows, selected_rows)) / 2);
        if (!FillViewport<Policy>(terminal, viewport)) {
            return false;
        }
        if ((visible_positions_.size() < available_rows) && (viewport_begin_ != Position{})) {
            Scroll<Policy>(terminal, viewport, -1, available_rows - visible_positions_.size());
            return FillViewport<Policy>(terminal, viewport);
        }
        return true;
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto EnsureLayout(MenuTerminal<Policy> auto &terminal,
        const MenuViewport viewport, const std::size_t entry) -> bool {
        auto &layout = entry_layouts_.at(entry);
        if (!layout) {
            layout = LayoutText<Policy>(terminal, entries_[entry],
                CursorPosition{.row = viewport.first_row,
                    .column = kTextColumn},
                WrappingOptions{.size = viewport.size,
                    .continuation_column = kContinuationColumn,
                    .maximum_rows = viewport.rows}, kPreviewRows);
        }
        return layout.has_value();
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto Advance(MenuTerminal<Policy> auto &terminal, const MenuViewport viewport,
        Position position, const int direction) -> Position {
        if (direction > 0) {
            if (EnsureLayout<Policy>(terminal, viewport, position.entry)) {
                if ((position.row + 1) < entry_layouts_.at(position.entry)->rows.size()) {
                    ++position.row;
                } else if ((position.entry + 1) < entries_.size()) {
                    position = {.entry = position.entry + 1};
                }
            }
        } else if (position.row != 0) {
            --position.row;
        } else if ((position.entry != 0) &&
            EnsureLayout<Policy>(terminal, viewport, position.entry - 1)) {
            --position.entry;
            position.row = entry_layouts_.at(position.entry)->rows.size() - 1;
        }
        return position;
    }

    template <WidthPolicy Policy>
    auto Scroll(MenuTerminal<Policy> auto &terminal, const MenuViewport viewport,
        const int direction, const std::size_t rows) -> void {
        for (auto count = std::size_t{}; count < rows; ++count) {
            const auto next = Advance<Policy>(terminal, viewport, viewport_begin_, direction);
            if (next == viewport_begin_) {
                break;
            }
            viewport_begin_ = next;
        }
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto FillViewport(MenuTerminal<Policy> auto &terminal, const MenuViewport viewport) -> bool {
        visible_positions_.clear();
        auto position = viewport_begin_;
        for (auto row = 0; row < viewport.rows; ++row) {
            if (!EnsureLayout<Policy>(terminal, viewport, position.entry)) {
                return false;
            }
            visible_positions_.push_back(position);
            const auto next = Advance<Policy>(terminal, viewport, position, 1);
            if (next == position) {
                break;
            }
            position = next;
        }
        return true;
    }

    std::span<const std::string> entries_;
    std::size_t selected_entry_;
    Position viewport_begin_;
    std::vector<std::optional<TextLayout>> entry_layouts_;
    std::vector<Position> visible_positions_;
    bool initial_viewport_pending_ = true;
    int layout_width_ = 0;
};

// The full-name view lets the user read text omitted by an entry's preview.
// Scrolling here moves within one name rather than selecting another entry.
// Each opening binds the view and its cached rows to that entry's display text;
// closing the view leaves the list's selection and scroll position intact.
class FullNameView {
public:
    explicit FullNameView(const std::string_view text) noexcept : text_(text) {}

    auto SetWidth(const int columns) noexcept -> void {
        if (layout_width_ != columns) {
            layout_.reset();
            layout_width_ = columns;
        }
    }

    auto InvalidateLayout() noexcept -> void { layout_width_ = 0; }

    template <WidthPolicy Policy>
    [[nodiscard]] auto Prepare(MenuTerminal<Policy> auto &terminal, const MenuViewport viewport) -> bool {
        if (!layout_) {
            layout_ = LayoutText<Policy>(terminal, text_,
                CursorPosition{.row = viewport.first_row,
                    .column = kTextColumn},
                WrappingOptions{.size = viewport.size,
                    .continuation_column = kContinuationColumn,
                    .maximum_rows = viewport.rows});
        }
        if (!layout_) {
            return false;
        }
        const auto after_position = std::ranges::upper_bound(layout_->rows, text_offset_, {},
            [this](const auto row) { return row.data() - text_.data(); });
        first_row_ = FailFastCast<std::size_t>(after_position - layout_->rows.begin() - 1);
        return true;
    }

    [[nodiscard]] auto Navigate(const MenuInput input, const int content_rows) noexcept -> bool {
        const auto amount = input.repeat *
            ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown) ?
                FailFastCast<std::size_t>(content_rows) : 1);
        switch (input.key) {
        case MenuKey::Up:
        case MenuKey::PageUp:
            first_row_ -= std::min(first_row_, amount);
            break;
        case MenuKey::Down:
        case MenuKey::PageDown:
            first_row_ += std::min(layout_->rows.size() - 1 - first_row_, amount);
            break;
        case MenuKey::Home:
            first_row_ = 0;
            break;
        case MenuKey::End:
            first_row_ = std::cmp_greater(layout_->rows.size(), content_rows) ?
                layout_->rows.size() - content_rows : 0;
            break;
        default:
            return false;
        }
        text_offset_ = layout_->rows.at(first_row_).data() - text_.data();
        return true;
    }

    auto Draw(auto &frame, const MenuViewport viewport) const -> void {
        const auto count = std::min(FailFastCast<std::size_t>(viewport.rows),
            layout_->rows.size() - first_row_);
        for (auto index = std::size_t{}; index < count; ++index) {
            frame.MoveTo({.row = viewport.first_row + FailFastCast<int>(index)});
            frame.WriteLine("{}{}",
                (first_row_ + index) == 0 ? "  "sv : "    "sv,
                PreparedText{.text = std::string{layout_->rows.at(first_row_ + index)}},
                FrameLineOptions{.clipping = FrameClipping::None});
        }
        frame.MoveTo({.row = viewport.size.rows - kInformationRows + 1});
        frame.WriteLine("{}", MakeInformationLine(
            std::array{"Up/Down/PgUp/PgDn: Scroll"sv, "Home/End"sv, "Esc: Back"sv}));
        frame.Write("\n"sv);
        const auto information = layout_->oversized ?
            std::string{"Enlarge the window to fit the next composed character."} :
            std::format("Full name: lines {}-{} of {}", first_row_ + 1,
                std::min(layout_->rows.size(), first_row_ + viewport.rows), layout_->rows.size());
        frame.WriteLine("{}", MakeInformationLine(std::array{std::string_view{information}}));
    }

private:
    std::string_view text_;
    std::optional<TextLayout> layout_;
    std::size_t first_row_ = 0;
    // Preserve the reader's position across changes in line wrapping. Navigation
    // saves the byte offset of the top row; rewrapping finds the row containing
    // that offset. Keeping the offset unchanged during resizing prevents drift.
    std::ptrdiff_t text_offset_ = 0;
    int layout_width_ = 0;
};

template <typename T, WidthPolicy Policy>
struct MenuPresentation {
    std::optional<TerminalSize> terminal_size;
    std::optional<Frame<T, Policy>> header;
    std::optional<MenuViewport> viewport;
};

// Recovery cannot require the cursor reports that just failed. Fit the message
// using width bounds and send the completed screen without another query.
template <WidthPolicy Policy>
auto ShowMenuRecovery(auto &terminal, const std::optional<TerminalSize> size,
    const std::span<const std::string_view> items) -> void {
    auto message = MakeInformationLine(items).text;
    if (size) {
        const auto groups = MeasureText<Policy>(message);
        if (std::ranges::fold_left(groups, 0,
                [](const auto width, const auto &group) { return width + group.width_bound; }) >
            size->columns) {
            const auto marker_width = std::min(3, size->columns);
            auto remaining_columns = size->columns - marker_width;
            auto retained_bytes = std::size_t{};
            for (const auto &group : groups) {
                if (group.width_bound > remaining_columns) {
                    break;
                }
                remaining_columns -= group.width_bound;
                retained_bytes += group.text.size();
            }
            message.resize(retained_bytes);
            message.append(FailFastCast<std::size_t>(marker_width), '.');
        }
    }
    terminal.InvalidateFrame();
    terminal.Write(std::format("{}{}", vt::kClearScreen, message));
    terminal.PresentFrame();
}

// `PresentMenu` builds a complete frame around the active list or full-name
// view. The header callback supplies the preceding content and the row where
// the menu begins. A saved header avoids repeating that work on navigation;
// resizing or explicit redrawing starts with a fresh header. `BeginUpdate`
// covers both layout measurements and the completed frame's presentation.
// The result retains the header and usable viewport for the next interaction.
// Missing dimensions or layout reports produce recovery instructions instead.
template <WidthPolicy Policy, MenuScrollPolicy Scrolling, typename T>
[[nodiscard]] auto PresentMenu(T &terminal, auto &draw_header, const MenuText &text,
    MenuListView &list, const std::optional<std::reference_wrapper<FullNameView>> full_name,
    const MenuPresentation<T, Policy> &cached, const MenuInput input) -> MenuPresentation<T, Policy> {
    const auto update = terminal.BeginUpdate();
    const auto terminal_size = cached.terminal_size ? cached.terminal_size : terminal.QuerySize();
    const auto show_message = [&](const std::span<const std::string_view> items) {
        ShowMenuRecovery<Policy>(terminal, terminal_size, items);
        return MenuPresentation<T, Policy>{.terminal_size = terminal_size,
            .header = std::nullopt, .viewport = std::nullopt};
    };
    if (!terminal_size) {
        return show_message(std::array{
            "Terminal size unavailable."sv, "Ctrl+L: Retry"sv, "Esc: Back"sv});
    }
    auto frame = [&] {
        if (cached.header) {
            return *cached.header;
        }
        auto header = Frame<T, Policy>{terminal, *terminal_size};
        std::invoke(draw_header, header);
        header.ClearFromCurrentRow();
        return header;
    }();
    if (!frame.Ready()) {
        return show_message(kLayoutUnavailable);
    }
    const auto viewport = MakeMenuViewport(*terminal_size, frame.CurrentRow(), text);
    if (!viewport) {
        constexpr auto instructions = std::array{"Enlarge the window."sv, "Esc: Back"sv};
        auto message = Frame<T, Policy>{terminal, *terminal_size};
        message.WriteLine("{}", MakeInformationLine(instructions));
        if (!terminal.template Flip<Policy>(message)) {
            return show_message(instructions);
        }
        return MenuPresentation<T, Policy>{.terminal_size = terminal_size,
            .header = std::nullopt, .viewport = std::nullopt};
    }
    list.SetWidth(viewport->size.columns);
    if (full_name) {
        full_name->get().SetWidth(viewport->size.columns);
    }
    if (!(full_name ? full_name->get().Prepare<Policy>(terminal, *viewport) :
            list.Prepare<Policy, Scrolling>(terminal, *viewport, input))) {
        return show_message(kLayoutUnavailable);
    }
    const auto header = frame;
    if (full_name) {
        full_name->get().Draw(frame, *viewport);
    } else {
        list.Draw(frame, *viewport);
    }
    frame.MoveTo({.row = terminal_size->rows - FailFastCast<int>(text.footer.size()) -
        kInformationRows + 1});
    for (const auto &line : text.footer) {
        frame.WriteLine("{}", PreparedText{.text = line});
        frame.Write("\n"sv);
    }
    if (!frame.Ready() || !terminal.template Flip<Policy>(frame)) {
        return show_message(kLayoutUnavailable);
    }
    return {.terminal_size = terminal_size, .header = header, .viewport = viewport};
}

}

export namespace devicefs::terminal {

// A command's identifier is returned to the caller when selected. Keep the
// identifier stable when changing its label or replacing the available commands.
struct OutputCommand {
    std::size_t id;
    std::string_view label;
};

enum class OutputCommandPosition { AboveOutput, BelowOutput };

// An output menu displays arriving logical lines and a selectable command list.
// AppendText, AppendLine, and SetCommands copy their inputs. Use this object on
// the UI thread; the update callback can drain a producer's queue without holding its lock
// during rendering. Retain the object across Select calls to preserve scrollback,
// focus, and selection while the caller handles a command.
//
// AppendText continues the current line until a newline ends it. AppendLine
// adds a separate complete line; embedded controls in that line receive the
// same visible notation as menu labels.
class OutputMenu {
public:
    // Retain at most this many logical lines, discarding the oldest first.
    // Individual lines are retained in full and wrap when displayed.
    GSL_SUPPRESS("26455", "Constructing the retained-line deque can allocate storage and throw.")
    explicit OutputMenu(const std::size_t retained_lines = 4096)
        : retained_lines_(retained_lines) {}

    OutputMenu(const OutputMenu &) = delete;
    auto operator=(const OutputMenu &) -> OutputMenu & = delete;

    // Place commands above or below the output, with a blank row between the
    // two areas. The default is BelowOutput. Changing the position retains
    // keyboard focus, command selection, and the passage being read.
    auto SetCommandPosition(const OutputCommandPosition position) noexcept -> void {
        if (position != command_position_) {
            command_position_ = position;
            dirty_ = true;
        }
    }

    // Request this many displayed rows for command text, including wrapped
    // continuations. Headings, the blank separator, and bottom controls occupy
    // additional rows. The view reduces this height when needed to leave at
    // least one output row; values below one request one row.
    // Without a request, commands use up to four rows and at most half the
    // available content area.
    auto SetCommandRows(const int rows) noexcept -> void {
        const auto requested = std::max(1, rows);
        if (command_rows_ != requested) {
            command_rows_ = requested;
            dirty_ = true;
        }
    }

    auto AppendLine(const std::string_view text) -> void {
        if (retained_lines_ == 0) {
            return;
        }
        unfinished_.reset();
        AddLine(PrepareTerminalText(text));
    }

    // Display text immediately, including a final line without a newline.
    // Later calls continue that line. LF and CRLF end a line.
    auto AppendText(const std::string_view text) -> void {
        if (retained_lines_ == 0) {
            return;
        }
        for (const auto part : text | std::views::split('\n')) {
            const auto complete = part.end() != text.end();
            if (part.empty() && !complete) {
                break;
            }
            if (!unfinished_) {
                AddLine({});
                unfinished_.emplace();
            }
            unfinished_->append(std::string_view{part});
            auto content = std::string_view{*unfinished_};
            if (complete && content.ends_with('\r')) {
                content.remove_suffix(1);
            }
            auto &line = lines_.back();
            line.text = PrepareTerminalText(content);
            line.layout.reset();
            dirty_ = true;
            if (complete) {
                unfinished_.reset();
            }
        }
    }

    // Supply the commands currently available to the user. An existing selection
    // follows its identifier through reordering and label changes. If that command
    // disappears, select the nearest remaining entry without invoking it.
    auto SetCommands(const std::span<const OutputCommand> commands) -> void {
        auto labels = std::vector<std::string>{};
        auto ids = std::vector<std::size_t>{};
        for (const auto &command : commands) {
            if (std::ranges::contains(ids, command.id)) {
                throw std::invalid_argument("output menu command identifiers must be distinct");
            }
            labels.push_back(PrepareTerminalText(command.label));
            ids.push_back(command.id);
        }
        if ((ids == command_ids_) && (labels == command_labels_)) {
            return;
        }
        auto selected = list_ ? list_->SelectedIndex() : 0;
        if (!command_ids_.empty()) {
            const auto found = std::ranges::find(ids, command_ids_.at(selected));
            if (found != ids.end()) {
                selected = FailFastCast<std::size_t>(found - ids.begin());
            }
        }
        list_.reset();
        command_ids_ = std::move(ids);
        command_labels_ = std::move(labels);
        list_.emplace(command_labels_, selected);
        dirty_ = true;
    }

    // Request a new frame when caller-owned header content changes.
    auto Refresh() noexcept -> void { dirty_ = true; }

    // Display output and commands within the caller's existing EnterScreen
    // lifetime. draw_header receives the complete Frame and leaves its cursor
    // at column one where the first area begins. update receives this OutputMenu
    // before the first frame and after each input wait, so it can append output,
    // replace commands, or request a refreshed heading. It must return promptly.
    // Idle waits last at most 50 ms; unchanged state produces no terminal output.
    //
    // Tab or Shift+Tab changes area. Arrows, paging, and Home/End operate on that
    // area. Scrolling upward suspends following new output; reaching the bottom
    // resumes it. Resizing preserves the passage being read. Enter returns the
    // selected command's identifier only while commands have focus. Escape or
    // Ctrl+C returns an empty optional. The caller owns the operation's lifetime
    // and decides what selecting a command or cancelling should do.
    //
    // The console supplies ReadMenuInput(deadline), returning MenuKey::Timeout
    // when the deadline expires. I/O and callback exceptions propagate. Missing
    // layout reports show recovery instructions and await resize or Ctrl+L.
    template <WidthPolicy Policy = WidthPolicy::AllModes>
    [[nodiscard]] auto Select(MenuTerminal<Policy> auto &terminal,
        auto &&draw_header, auto &&update) -> std::optional<std::size_t> try {
        using namespace std::chrono_literals;
        constexpr auto kRefreshInterval = 50ms;
        auto size = std::optional<TerminalSize>{};
        auto input = MenuInput{MenuKey::Redraw};
        auto usable = false;
        auto retry = true;
        dirty_ = true;
        if (!list_) {
            list_.emplace(command_labels_, 0);
        }
        for (;;) {
            std::invoke(update, *this);
            if (retry || (usable && dirty_)) {
                const auto guard = terminal.BeginUpdate();
                if (!size) {
                    size = terminal.QuerySize();
                }
                usable = size && Present<Policy>(terminal, draw_header, *size, input);
                if (!size) {
                    menu_detail::ShowMenuRecovery<Policy>(terminal, size, std::array{
                        "Terminal size unavailable."sv, "Ctrl+L: Retry"sv, "Esc: Back"sv});
                }
                dirty_ = false;
                retry = false;
            }
            input = terminal.ReadMenuInput(std::chrono::steady_clock::now() + kRefreshInterval);
            if ((input.key == MenuKey::Back) || (input.key == MenuKey::Cancel)) {
                return std::nullopt;
            }
            if ((input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw)) {
                size.reset();
                retry = true;
                if (input.key == MenuKey::Redraw) {
                    terminal.InvalidateFrame();
                    list_->InvalidateLayout();
                    layout_width_ = 0;
                }
            } else if (usable) {
                if (input.key == MenuKey::SwitchArea) {
                    if ((input.repeat % 2) != 0) {
                        commands_focused_ = !commands_focused_;
                        dirty_ = true;
                    }
                } else if (commands_focused_) {
                    if ((input.key == MenuKey::Accept) && !command_ids_.empty()) {
                        return command_ids_.at(list_->SelectedIndex());
                    }
                    dirty_ |= list_->Navigate(input);
                } else {
                    dirty_ |= Scroll(input);
                }
            }
        }
    } catch (const InputCancelled &) {
        return std::nullopt;
    }

private:
    auto AddLine(std::string text) -> void {
        lines_.push_back({.text = std::move(text), .layout = std::nullopt});
        if (lines_.size() > retained_lines_) {
            lines_.pop_front();
            ++first_line_;
        }
        dirty_ = true;
    }

    struct Line {
        std::string text;
        std::optional<TextLayout> layout;
    };

    struct Row {
        std::size_t line;
        std::size_t offset;
        std::size_t length;
        bool truncated;
    };

    [[nodiscard]] auto Scroll(const MenuInput input) -> bool {
        if (rows_.empty()) {
            return false;
        }
        const auto last = rows_.size() - std::min(rows_.size(), output_rows_);
        const auto amount = input.repeat *
            ((input.key == MenuKey::PageUp) || (input.key == MenuKey::PageDown) ? output_rows_ : 1);
        switch (input.key) {
        case MenuKey::Up:
        case MenuKey::PageUp:
            top_ -= std::min(top_, amount);
            break;
        case MenuKey::Down:
        case MenuKey::PageDown:
            top_ += std::min(last - std::min(top_, last), amount);
            top_ = std::min(top_, last);
            break;
        case MenuKey::Home:
            top_ = 0;
            break;
        case MenuKey::End:
            top_ = last;
            break;
        default:
            return false;
        }
        following_ = top_ == last;
        anchor_line_ = rows_.at(top_).line;
        anchor_offset_ = rows_.at(top_).offset;
        history_lost_ = false;
        return true;
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto PrepareOutput(MenuTerminal<Policy> auto &terminal,
        const menu_detail::MenuViewport viewport) -> bool {
        if (layout_width_ != viewport.size.columns) {
            for (auto &line : lines_) {
                line.layout.reset();
            }
            layout_width_ = viewport.size.columns;
        }
        rows_.clear();
        auto id = first_line_;
        for (auto &line : lines_) {
            if (!line.layout) {
                line.layout = LayoutText<Policy>(terminal, line.text,
                    {.row = viewport.first_row},
                    {.size = viewport.size, .continuation_column = 1,
                        .maximum_rows = viewport.rows});
            }
            if (!line.layout) {
                return false;
            }
            for (const auto row : line.layout->rows) {
                rows_.push_back({.line = id,
                    .offset = FailFastCast<std::size_t>(row.data() - line.text.data()),
                    .length = row.size(), .truncated = false});
            }
            if (line.layout->truncated) {
                rows_.back().truncated = true;
            }
            ++id;
        }
        output_rows_ = FailFastCast<std::size_t>(viewport.rows);
        if (following_) {
            top_ = rows_.size() - std::min(rows_.size(), output_rows_);
        } else {
            history_lost_ |= anchor_line_ < first_line_;
            const auto after = std::ranges::upper_bound(rows_, std::pair{anchor_line_, anchor_offset_}, {},
                [](const Row &row) { return std::pair{row.line, row.offset}; });
            top_ = after == rows_.begin() ? 0 :
                FailFastCast<std::size_t>(after - rows_.begin() - 1);
        }
        return true;
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto Present(MenuTerminal<Policy> auto &terminal, auto &draw_header,
        const TerminalSize size, const MenuInput input) -> bool {
        auto frame = Frame<std::remove_reference_t<decltype(terminal)>, Policy>{terminal, size};
        std::invoke(draw_header, frame);
        frame.ClearFromCurrentRow();
        const auto first = frame.CurrentRow();
        // Reserve two area headings, two control rows, and a blank row between
        // the areas.
        const auto available = size.rows - first - 4;
        const auto message = [&](const std::string_view text) {
            auto recovery = Frame<std::remove_reference_t<decltype(terminal)>, Policy>{terminal, size};
            recovery.WriteLine("{}", text);
            if (!terminal.template Flip<Policy>(recovery)) {
                menu_detail::ShowMenuRecovery<Policy>(terminal, size, std::array{text});
            }
            return false;
        };
        if (!frame.Ready()) {
            return message("Layout unavailable. Ctrl+L: Retry. Esc: Back."sv);
        }
        if ((size.columns < menu_detail::kMinimumColumns) || (available < 2)) {
            return message("Enlarge the window. Esc: Back."sv);
        }
        const auto command_rows = std::min(
            command_rows_.value_or(std::min(4, available / 2)), available - 1);
        const auto output_rows = available - command_rows;
        const auto commands_above = command_position_ == OutputCommandPosition::AboveOutput;
        const auto output = menu_detail::MenuViewport{
            .size = size, .first_row = commands_above ? first + command_rows + 3 : first + 1,
            .rows = output_rows};
        const auto commands = menu_detail::MenuViewport{
            .size = size, .first_row = commands_above ? first + 1 : first + output_rows + 3,
            .rows = command_rows};
        list_->SetWidth(size.columns);
        if (!PrepareOutput<Policy>(terminal, output) ||
            !list_->Prepare<Policy, MenuScrollPolicy::Line>(terminal, commands,
                commands_focused_ || (input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw) ?
                    input : MenuInput{MenuKey::Timeout})) {
            return message("Layout unavailable. Ctrl+L: Retry. Esc: Back."sv);
        }
        frame.MoveTo({.row = output.first_row - 1});
        frame.WriteLine("Output{} - {}", commands_focused_ ? ""sv : " (active)"sv,
            history_lost_ ? "older lines discarded"sv :
            (following_ ? "following newest lines"sv : "reading history; End: follow newest"sv));
        const auto count = std::min(output_rows_, rows_.size() - top_);
        for (auto index = 0uz; index < count; ++index) {
            const auto &row = rows_.at(top_ + index);
            frame.MoveTo({.row = output.first_row + FailFastCast<int>(index)});
            frame.WriteLine("{}", PreparedText{.text = lines_.at(row.line - first_line_).text.substr(
                row.offset, row.length)}, FrameLineOptions{
                .clipping = row.truncated ? FrameClipping::Ellipsis : FrameClipping::None});
        }
        frame.MoveTo({.row = commands.first_row - 1});
        frame.WriteLine("Commands{}", commands_focused_ ? " (active)"sv : ""sv);
        list_->DrawEntries(frame, commands, commands_focused_);
        frame.MoveTo({.row = size.rows - 1});
        frame.WriteLine("Tab: Change area    Up/Down: {}    Enter: Choose\n",
            commands_focused_ ? "Select"sv : "Scroll"sv);
        frame.WriteLine("PgUp/PgDn: Page    Home/End    Ctrl+L: Redraw    Esc: Back"sv);
        if (!frame.Ready() || !terminal.template Flip<Policy>(frame)) {
            return message("Layout unavailable. Ctrl+L: Retry. Esc: Back."sv);
        }
        return true;
    }

    std::size_t retained_lines_;
    std::deque<Line> lines_;
    // Preparing the whole unfinished line lets text split inside a UTF-8
    // character or terminal command be interpreted together on the next append.
    std::optional<std::string> unfinished_;
    std::size_t first_line_ = 0;
    std::vector<Row> rows_;
    std::size_t top_ = 0;
    std::size_t output_rows_ = 0;
    // The logical line and byte offset identify the passage being read even
    // after a width change gives it a different wrapped row number.
    std::size_t anchor_line_ = 0;
    std::size_t anchor_offset_ = 0;
    int layout_width_ = 0;
    bool following_ = true;
    bool history_lost_ = false;
    bool commands_focused_ = true;
    bool dirty_ = true;
    OutputCommandPosition command_position_ = OutputCommandPosition::BelowOutput;
    std::optional<int> command_rows_;
    std::vector<std::size_t> command_ids_;
    std::vector<std::string> command_labels_;
    std::optional<menu_detail::MenuListView> list_;
};

// `SelectMenuItem` displays a scrolling list after caller-drawn content and
// returns the original index of the chosen entry. Escape or Ctrl+C cancels
// selection. `draw_header` receives the complete frame and leaves its cursor
// at column one of the row where the menu should begin. Content before that
// row remains in every menu frame; content from that row onward is replaced.
// The callback runs initially and again on resize or explicit redraw, allowing
// the caller to adapt its text to the frame's dimensions.
//
// Entry and footer strings are ordinary UTF-8; the menu prepares its own display
// copies so filtering cannot change entry identity. The callback uses the
// frame's formatted writing operations, which prepare string arguments for
// display. An initial index beyond the list selects its last entry.
// On the first rendering, a list that fits is shown in full. Longer lists
// center the selected preview, with any extra row below it, and shift the
// viewport at the list boundaries to use the available space.
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
// With a native adapter, the caller retains the owner returned by `EnterScreen`
// across menu calls. Returning leaves the last frame displayed so the next
// menu's `Flip` can compare against it; destroying the screen owner restores
// the invoking shell.
// A missing layout report leaves a message and waits for resize, redraw, or
// cancellation; terminal I/O exceptions propagate to the caller.
template <WidthPolicy Policy = WidthPolicy::AllModes,
    MenuScrollPolicy Scrolling = MenuScrollPolicy::Line>
[[nodiscard]] auto SelectMenuItem(MenuTerminal<Policy> auto &terminal,
    auto &&draw_header,
    const std::span<const std::string_view> entries,
    const std::span<const std::string_view> footer = {},
    const std::size_t initial_selection = 0) -> std::optional<std::size_t> try {
    using namespace menu_detail;
    const auto text = MenuText{.entries = PrepareLines(entries), .footer = PrepareLines(footer)};
    auto list = MenuListView{text.entries, initial_selection};
    auto full_name = std::optional<FullNameView>{};
    const auto present = [&](const MenuInput input,
        const MenuPresentation<std::remove_reference_t<decltype(terminal)>, Policy> &cached = {}) {
        return PresentMenu<Policy, Scrolling>(terminal, draw_header, text, list,
            full_name.transform([](auto &view) { return std::ref(view); }), cached, input);
    };
    auto presentation = present({MenuKey::Redraw});
    for (;;) {
        const auto input = terminal.ReadMenuInput();
        if ((input.key == MenuKey::Cancel) ||
            ((input.key == MenuKey::Back) && !full_name)) {
            return std::nullopt;
        }
        if ((input.key == MenuKey::Resize) || (input.key == MenuKey::Redraw)) {
            if (input.key == MenuKey::Redraw) {
                list.InvalidateLayout();
                if (full_name) {
                    full_name->InvalidateLayout();
                }
                terminal.InvalidateFrame();
            }
            presentation = present(input);
            continue;
        }
        if (input.key == MenuKey::Back) {
            full_name.reset();
            presentation = present(input, presentation);
            continue;
        }
        if (!presentation.viewport) {
            continue;
        }
        if (full_name) {
            if (!full_name->Navigate(input, presentation.viewport->rows)) {
                continue;
            }
        } else if ((input.key == MenuKey::Accept) && !text.entries.empty()) {
            return list.SelectedIndex();
        } else if ((input.key == MenuKey::Details) && list.CanViewFullName()) {
            full_name.emplace(text.entries.at(list.SelectedIndex()));
        } else if (!list.Navigate(input)) {
            continue;
        }
        presentation = present(input, presentation);
    }
} catch (const InputCancelled &) {
    return std::nullopt;
}

}
