// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.menu;

import std;
import devicefs.terminal.safecast;
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
constexpr auto kInformationRows = 2;
constexpr auto kLayoutUnavailable = std::array{
    "Layout unavailable."sv, "Ctrl+L: Redraw"sv, "Esc: Back"sv};

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

// `LayoutText` divides an entry's text into the rows needed to display it in the
// menu. The returned views identify each row within the supplied string, allowing
// the menu to scroll through long names one displayed row at a time. `limit`
// caps the number of rows in a preview; `truncated` records whether text remains.
// The supplied string must outlive those views.
//
// Row boundaries depend on how the terminal renders Unicode text. Previously
// measured character-group widths let this function calculate boundaries in
// memory. If an upper bound on the entire text's width fits on one row, no
// wrapping measurement is needed.
// Otherwise, `WriteWrappingText` writes the text and observes the terminal's
// wrapping. Measuring can overwrite the menu's content area, so that area is
// marked for restoration when the completed frame is presented. If cursor
// reports cannot establish the row boundaries, the function returns `std::nullopt`;
// the menu then displays a message offering a redraw.
template <WidthPolicy Policy>
[[nodiscard]] auto LayoutText(MenuTerminal auto &terminal,
    const std::string_view text, const TerminalSize size,
    const int first_row, const int available_rows, const std::size_t limit)
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
            terminal.InvalidateFrameRows(first_row, available_rows);
        }
        const auto measured_rows = layout.rows.size();
        const auto rows_to_measure = std::min(
            FailFastCast<std::size_t>(available_rows), limit - measured_rows);
        MoveTo(terminal, first_row,
            measured_rows == 0 ? kTextColumn : kContinuationColumn);
        const auto result = WriteWrappingText(terminal, clusters,
            WrappingOptions{.size = size,
                .continuation_column = kContinuationColumn,
                .maximum_rows = FailFastCast<int>(rows_to_measure)},
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

struct MenuText {
    std::vector<std::string> header;
    std::vector<std::string> entries;
    std::vector<std::string> footer;
};

struct MenuViewport {
    TerminalSize size;
    int first_row;
    int rows;
};

[[nodiscard]] auto MakeMenuViewport(const TerminalSize size, const MenuText &text) noexcept
    -> std::optional<MenuViewport> {
    const auto fixed_rows = text.header.size() + text.footer.size() + kInformationRows;
    if ((size.columns < kMinimumColumns) || (std::cmp_less_equal(size.rows, fixed_rows))) {
        return std::nullopt;
    }
    return MenuViewport{.size = size,
        .first_row = FailFastCast<int>(text.header.size()) + 1,
        .rows = size.rows - FailFastCast<int>(fixed_rows)};
}

// `MenuListView` manages selection and scrolling through the menu's entries.
// One entry can occupy several rows, so moving through displayed rows is a
// different operation from selecting the next entry. Paging can leave only an
// entry's continuation visible. Preview layouts belong to this view and are
// calculated as scrolling encounters entries. `Prepare` finds the rows for the
// next frame; `BuildFrame` returns a back buffer containing those rows and hints.
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
    [[nodiscard]] auto Prepare(MenuTerminal auto &terminal,
        const MenuViewport viewport, const MenuInput input) -> bool {
        if (entries_.empty()) {
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

    [[nodiscard]] auto BuildFrame(const MenuViewport viewport) const -> FrameBuffer {
        auto frame = FrameBuffer{viewport.size};
        const auto first_row = FailFastCast<std::size_t>(viewport.first_row - 1);
        if (entries_.empty()) {
            frame.rows.at(first_row) = {.text = "No entries are available.",
                .clipping = FrameClipping::IfNeeded};
        }
        for (auto index = std::size_t{}; index < visible_positions_.size(); ++index) {
            const auto position = visible_positions_.at(index);
            const auto &layout = *entry_layouts_.at(position.entry);
            frame.rows.at(first_row + index) = {
                .text = std::format("{}{}{}",
                    position.entry == selected_entry_ ? "> "sv : "  "sv,
                    position.row == 0 ? ""sv : "  "sv,
                    layout.rows.at(position.row)),
                .reverse = position.entry == selected_entry_,
                .clipping = layout.truncated &&
                    ((position.row + 1) == layout.rows.size()) ?
                        FrameClipping::Ellipsis : FrameClipping::None};
        }
        frame.rows.at(frame.rows.size() - kInformationRows) = MakeInformationLine(
            std::array{"Up/Down: Select"sv, "Enter: Choose"sv, "Esc: Back"sv});
        frame.rows.back() = [this] {
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
        }();
        return frame;
    }

private:
    template <WidthPolicy Policy>
    [[nodiscard]] auto EnsureLayout(MenuTerminal auto &terminal,
        const MenuViewport viewport, const std::size_t entry) -> bool {
        auto &layout = entry_layouts_.at(entry);
        if (!layout) {
            layout = LayoutText<Policy>(terminal, entries_[entry], viewport.size,
                viewport.first_row, viewport.rows, kPreviewRows);
        }
        return layout.has_value();
    }

    template <WidthPolicy Policy>
    [[nodiscard]] auto Advance(MenuTerminal auto &terminal, const MenuViewport viewport,
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
    auto Scroll(MenuTerminal auto &terminal, const MenuViewport viewport,
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
    [[nodiscard]] auto FillViewport(MenuTerminal auto &terminal, const MenuViewport viewport) -> bool {
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
            first_row_ = 0;
            layout_width_ = columns;
        }
    }

    auto InvalidateLayout() noexcept -> void { layout_width_ = 0; }

    template <WidthPolicy Policy>
    [[nodiscard]] auto Prepare(MenuTerminal auto &terminal, const MenuViewport viewport) -> bool {
        if (!layout_) {
            layout_ = LayoutText<Policy>(terminal, text_, viewport.size,
                viewport.first_row, viewport.rows, std::numeric_limits<std::size_t>::max());
        }
        if (!layout_) {
            return false;
        }
        first_row_ = std::min(first_row_, layout_->rows.size() - 1);
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
        return true;
    }

    [[nodiscard]] auto BuildFrame(const MenuViewport viewport) const -> FrameBuffer {
        auto frame = FrameBuffer{viewport.size};
        const auto count = std::min(FailFastCast<std::size_t>(viewport.rows),
            layout_->rows.size() - first_row_);
        for (auto index = std::size_t{}; index < count; ++index) {
            frame.rows.at(FailFastCast<std::size_t>(viewport.first_row - 1) + index) = {
                .text = std::format("{}{}", (first_row_ + index) == 0 ? "  "sv : "    "sv,
                    layout_->rows.at(first_row_ + index))};
        }
        frame.rows.at(frame.rows.size() - kInformationRows) = MakeInformationLine(
            std::array{"Up/Down/PgUp/PgDn: Scroll"sv, "Home/End"sv, "Esc: Back"sv});
        const auto information = layout_->oversized ?
            std::string{"Enlarge the window to fit the next composed character."} :
            std::format("Full name: lines {}-{} of {}", first_row_ + 1,
                std::min(layout_->rows.size(), first_row_ + viewport.rows), layout_->rows.size());
        frame.rows.back() = MakeInformationLine(std::array{std::string_view{information}});
        return frame;
    }

private:
    std::string_view text_;
    std::optional<TextLayout> layout_;
    std::size_t first_row_ = 0;
    int layout_width_ = 0;
};

// A frame contains everything that should be visible at once: the caller's
// fixed header and footer, the active view, and that view's navigation hints.
// Frame construction only fills memory. The adapter's `Flip` operation presents
// the completed frame.
[[nodiscard]] auto BuildMenuFrame(const MenuText &text,
    const MenuViewport viewport, const auto &view) -> FrameBuffer {
    auto frame = view.BuildFrame(viewport);
    for (auto index = std::size_t{}; index < text.header.size(); ++index) {
        frame.rows.at(index) = {.text = text.header.at(index), .clipping = FrameClipping::IfNeeded};
    }
    for (auto index = std::size_t{}; index < text.footer.size(); ++index) {
        frame.rows.at(frame.rows.size() - text.footer.size() - kInformationRows + index) = {
            .text = text.footer.at(index), .clipping = FrameClipping::IfNeeded};
    }
    return frame;
}

struct MenuPresentation {
    std::optional<TerminalSize> terminal_size;
    bool ready;
};

// `PresentMenu` prepares and displays one menu frame. Layout may need to write
// text to measure wrapping, so the update lifetime covers both measurement and
// presentation. The result supplies the dimensions used and whether the menu is
// ready for navigation. Cached dimensions avoid repeating a terminal query on
// every key press; the caller omits them when a resize or redraw requires a fresh
// query. Missing dimensions or unusable layout produce a message with `ready`
// set to false. The input loop then waits for resizing, redrawing, leaving the
// full-name view, or cancellation.
template <WidthPolicy Policy, MenuScrollPolicy Scrolling>
[[nodiscard]] auto PresentMenu(MenuTerminal auto &terminal, const MenuText &text,
    MenuListView &list, const std::optional<std::reference_wrapper<FullNameView>> full_name,
    const std::optional<TerminalSize> cached_size, const MenuInput input) -> MenuPresentation {
    const auto update = terminal.BeginUpdate();
    const auto terminal_size = cached_size ? cached_size : terminal.QuerySize();
    const auto show_message = [&](const std::span<const std::string_view> items) {
        auto frame = FrameBuffer{terminal_size};
        frame.rows.front() = MakeInformationLine(items);
        std::ignore = terminal.template Flip<Policy>(frame);
        return MenuPresentation{.terminal_size = terminal_size, .ready = false};
    };
    if (!terminal_size) {
        return show_message(std::array{
            "Terminal size unavailable."sv, "Ctrl+L: Retry"sv, "Esc: Back"sv});
    }
    const auto viewport = MakeMenuViewport(*terminal_size, text);
    if (!viewport) {
        return show_message(std::array{"Enlarge the window."sv, "Esc: Back"sv});
    }
    list.SetWidth(viewport->size.columns);
    if (full_name) {
        full_name->get().SetWidth(viewport->size.columns);
    }
    if (!(full_name ? full_name->get().Prepare<Policy>(terminal, *viewport) :
            list.Prepare<Policy, Scrolling>(terminal, *viewport, input))) {
        return show_message(kLayoutUnavailable);
    }
    return {.terminal_size = terminal_size,
        .ready = terminal.template Flip<Policy>(full_name ?
            BuildMenuFrame(text, *viewport, full_name->get()) : BuildMenuFrame(text, *viewport, list))};
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
    const auto text = MenuText{.header = PrepareLines(header),
        .entries = PrepareLines(entries), .footer = PrepareLines(footer)};
    const auto screen = terminal.EnterMenu();
    auto list = MenuListView{text.entries, initial_selection};
    auto full_name = std::optional<FullNameView>{};
    const auto present = [&](const MenuInput input,
        const std::optional<TerminalSize> cached_size = std::nullopt) {
        return PresentMenu<Policy, Scrolling>(terminal, text, list,
            full_name.transform([](auto &view) { return std::ref(view); }), cached_size, input);
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
            presentation = present(input, presentation.terminal_size);
            continue;
        }
        if (!presentation.ready) {
            continue;
        }
        if (full_name) {
            if (!full_name->Navigate(input, MakeMenuViewport(*presentation.terminal_size, text)->rows)) {
                continue;
            }
        } else if ((input.key == MenuKey::Accept) && !text.entries.empty()) {
            return list.SelectedIndex();
        } else if ((input.key == MenuKey::Details) && list.CanViewFullName()) {
            full_name.emplace(text.entries.at(list.SelectedIndex()));
        } else if (!list.Navigate(input)) {
            continue;
        }
        presentation = present(input, presentation.terminal_size);
    }
}

}
