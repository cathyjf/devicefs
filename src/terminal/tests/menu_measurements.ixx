// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.menu_measurements;

import std;
import devicefs.terminal;
import devicefs.terminal.menu;

using namespace std::string_view_literals;

export namespace devicefs::terminal::tests {

struct MenuMeasurement {
    std::optional<MenuKey> input;
    std::size_t writes = 0;
    std::size_t bytes = 0;
    std::size_t cursor_queries = 0;
    std::size_t size_queries = 0;
    std::chrono::steady_clock::duration elapsed{};
};

// `MeasuringConsole` records output calls, supplied UTF-8 bytes, terminal queries,
// and elapsed time for each menu update. Both ordinary writes and nonthrowing
// control writes count, including the update guard's release of synchronized
// output. Reaching the next input request completes the measurement; waiting
// for the user is excluded. The initial measurement also includes preparation
// of the labels and entry into the menu screen. Results remain in memory until
// the caller has left the menu, so reporting cannot interfere with its drawing.
template <MenuTerminal Console>
class MeasuringConsole : public Console {
public:
    using Console::Console;

    auto Write(const std::string_view text) -> void {
        ++current_.writes;
        current_.bytes += text.size();
        Console::Write(text);
    }

    auto WriteControlSequenceNoThrow(const std::string_view sequence) noexcept -> void {
        ++current_.writes;
        current_.bytes += sequence.size();
        Console::WriteControlSequenceNoThrow(sequence);
    }

    [[nodiscard]] auto QueryCursor() -> std::optional<CursorPosition> {
        ++current_.cursor_queries;
        return Console::QueryCursor();
    }

    [[nodiscard]] auto QuerySize() noexcept(noexcept(Console::QuerySize()))
        -> std::optional<TerminalSize> {
        ++current_.size_queries;
        return Console::QuerySize();
    }

    [[nodiscard]] auto ReadMenuInput() -> MenuInput {
        current_.elapsed = std::chrono::steady_clock::now() - started_;
        measurements.push_back(current_);
        const auto input = Console::ReadMenuInput();
        current_ = {.input = input.key};
        started_ = std::chrono::steady_clock::now();
        return input;
    }

    std::vector<MenuMeasurement> measurements;

private:
    MenuMeasurement current_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

auto PrintMenuMeasurements(const std::span<const MenuMeasurement> measurements) -> void {
    for (const auto &measurement : measurements) {
        const auto operation = [&measurement] {
            if (!measurement.input) {
                return "Initial display"sv;
            }
            switch (*measurement.input) {
            case MenuKey::Up: return "Up"sv;
            case MenuKey::Down: return "Down"sv;
            case MenuKey::PageUp: return "Page Up"sv;
            case MenuKey::PageDown: return "Page Down"sv;
            case MenuKey::Home: return "Home"sv;
            case MenuKey::End: return "End"sv;
            case MenuKey::Resize: return "Resize"sv;
            case MenuKey::Redraw: return "Redraw"sv;
            case MenuKey::Details: return "Full name"sv;
            case MenuKey::Back: return "Back"sv;
            default: return "Input without a selection change"sv;
            }
        }();
        std::println("{}: {:.3f} ms, {} writes, {} bytes, {} cursor queries, {} size queries.",
            operation, std::chrono::duration<double, std::milli>{measurement.elapsed}.count(),
            measurement.writes, measurement.bytes, measurement.cursor_queries, measurement.size_queries);
    }
}

}
