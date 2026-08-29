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
import devicefs.common;
#if defined(DEVICEFS_PROGRAM_DEVICEFS)
import devicefs.filesystem;
#endif
import devicefs.stream_writer;

auto BackupSupervisorMain(std::span<const std::string_view>) -> int;
auto VssDescriptorDumpMain(std::span<const std::string_view>) -> int;

[[gsl::suppress("26429",
    justification:
        "C++ [basic.start.main] guarantees that `argv` is not null.")]]
auto main(
    _Pre_satisfies_(argc > 0) const int argc,
    _In_reads_(argc) char **const argv) -> int {
    std::ignore = std::setlocale(LC_CTYPE, ".UTF8");
    try {
        HardenProcess();
        const auto arguments =
            std::span{argv, argv + argc} |
            std::ranges::to<std::vector<std::string_view>>();
#if defined(DEVICEFS_PROGRAM_DEVICEFS)
        return devicefs::Main(arguments);
#elif defined(DEVICEFS_PROGRAM_VSS_DESCRIPTOR_DUMP)
        return VssDescriptorDumpMain(std::span{arguments}.subspan(1));
#else
        return BackupSupervisorMain(std::span{arguments}.subspan(1));
#endif
    } catch (const std::runtime_error &error) {
        devicefs::WriteToStream(
            devicefs::stderr, "{}: {}\n", argv[0], error.what());
        return 1;
    }
}
