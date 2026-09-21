// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

import std;
import <clocale>;
import <sal.h>;
import <devicefs/common.h>;
import devicefs.stream_writer;
import devicefs.supervisor;

// Once MSVC++ implements P3618R0 ("Allow attaching main to the lobal module"),
// the `main` function can move into the supervisor module file. Until then,
// this separate `main.cpp` file remains necessary.

[[gsl::suppress("26429",
    justification:
        "C++ [basic.start.main] guarantees that `argv` is not null.")]]
auto main(
    _Pre_satisfies_(argc > 0) const int argc,
    _In_reads_(argc) char **const argv) -> int {
    if (std::setlocale(LC_CTYPE, ".UTF8") == nullptr) {
        devicefs::WriteToStream(
            devicefs::stderr,
            "{}: could not set the CRT locale to UTF-8; continuing anyway",
            argv[0]);
    }
    try {
        HardenProcess();
        const auto arguments =
            std::span{argv, argv + argc} |
            std::ranges::to<std::vector<std::string_view>>();
        return BackupSupervisorMain(std::span{arguments}.subspan(1));
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(
            devicefs::stderr, "{}: {}\n", argv[0], error.what());
        return 1;
    }
}
