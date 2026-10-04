// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#ifndef _WIN32
    #include <cstdio>
#endif

export module devicefs.terminal.test_support;

import std;
#ifdef _WIN32
    import <cstdio>;
#endif
import devicefs.terminal;
import devicefs.terminal.menu;

using namespace std::string_view_literals;

export namespace devicefs::terminal::tests {

inline auto output_prefix = std::string{};

template <class... Argument>
auto Println(std::FILE *const stream,
    const std::format_string<Argument...> format, Argument &&...argument)
    -> void {
    std::println(stream, "{}{}", output_prefix,
        std::format(format, std::forward<Argument>(argument)...));
    std::ignore = std::fflush(stream);
}

template <class... Argument>
auto Println(const std::format_string<Argument...> format,
    Argument &&...argument) -> void {
    Println(stdout, format, std::forward<Argument>(argument)...);
}

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
    Println("Testing {}.", name);
    try {
        std::invoke(operation);
        Println("PASS: {}.", name);
        return true;
    } catch (const std::exception &error) {
        Println(stderr, "FAIL: {}: {}", name, error.what());
        return false;
    }
}

}
