// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.test_support;

import std;
import devicefs.terminal;
import devicefs.terminal.menu;

using namespace std::string_view_literals;

export namespace devicefs::terminal::tests {

// The regression fixtures describe headers as fixed rows so their expected
// screens can identify exactly which rows changed. This callback writes those
// rows through the frame's text interface when the menu lays out its header.
template <WidthPolicy Policy = WidthPolicy::AllModes,
    MenuScrollPolicy Scrolling = MenuScrollPolicy::Line>
[[nodiscard]] auto SelectTestMenuItem(MenuTerminal<Policy> auto &terminal,
    const std::span<const std::string_view> header,
    const std::span<const std::string_view> entries,
    const std::span<const std::string_view> footer = {},
    const std::size_t initial_selection = 0) -> std::optional<std::size_t> {
    return SelectMenuItem<Policy, Scrolling>(terminal, [&header](auto &frame) {
        for (const auto line : header) {
            frame.WriteLine("{}", line);
            frame.Write("\n"sv);
        }
    }, entries, footer, initial_selection);
}

auto Require(const bool condition, const std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] auto Test(const std::string_view name, const auto &operation) -> bool {
    std::println("Testing {}.", name);
    std::cout.flush();
    try {
        std::invoke(operation);
        std::println("PASS: {}.", name);
        return true;
    } catch (const std::exception &error) {
        std::println(std::cerr, "FAIL: {}: {}", name, error.what());
        return false;
    }
}

}
