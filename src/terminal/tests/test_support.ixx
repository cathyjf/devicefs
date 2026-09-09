// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.terminal.test_support;

import std;

export namespace devicefs::terminal::tests {

auto Require(const bool condition, const std::string_view message) -> void {
    if (!condition) {
        throw std::runtime_error(std::string{message});
    }
}

[[nodiscard]] auto Test(const std::string_view name, const auto &operation) -> bool {
    std::println("Testing {}.", name);
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
