// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

import std;
import devicefs.terminal.safecast;
import devicefs.asr_trace.macos;

auto main(const int argc, char *argv[]) -> int {
    try {
        return devicefs::asr_trace::RunMac(
            {argv, devicefs::terminal::FailFastCast<std::size_t>(argc)});
    } catch (const std::invalid_argument &error) {
        std::println(std::cerr, "asr-image-trace: {}", error.what());
        return 2;
    } catch (const std::exception &error) {
        std::println(std::cerr, "asr-image-trace: {}", error.what());
        return 1;
    }
}
