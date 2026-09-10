// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.menu;

import std;
import devicefs.terminal.safecast;
import devicefs.terminal;
import devicefs.terminal.frame;
import devicefs.terminal.layout;
import devicefs.terminal.drawing;
import devicefs.terminal.formatting;

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

    auto Draw(auto &frame, const MenuViewport viewport) const -> void {
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
                    .highlight = position.entry == selected_entry_});
        }
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
            first_row_ = 0;
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
    int layout_width_ = 0;
};

template <typename T, WidthPolicy Policy>
struct MenuPresentation {
    std::optional<TerminalSize> terminal_size;
    std::optional<Frame<T, Policy>> header;
    std::optional<MenuViewport> viewport;
};

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
        // Recovery instructions must be readable when cursor reports are
        // unavailable. Fit the message using width bounds, then send the
        // completed screen in one write without querying the terminal.
        auto message = MakeInformationLine(items).text;
        if (terminal_size) {
            const auto groups = MeasureText<Policy>(message);
            if (std::ranges::fold_left(groups, 0,
                    [](const auto width, const auto &group) { return width + group.width_bound; }) >
                terminal_size->columns) {
                const auto marker_width = std::min(3, terminal_size->columns);
                auto remaining_columns = terminal_size->columns - marker_width;
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
        constexpr auto kMessageScreen = "\x1b[0m\x1b[2J\x1b[H{}"sv;
        terminal.InvalidateFrame();
        terminal.Write(std::format(kMessageScreen, message));
        terminal.PresentFrame();
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
