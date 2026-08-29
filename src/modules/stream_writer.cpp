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

module devicefs.stream_writer;

import std;
import <cstdio>;

namespace devicefs::stream_writer_detail {
namespace {

auto Print(
    std::FILE &output,
    const std::string_view text) noexcept -> bool {
    try {
        std::print(&output, "{}", text);
        return (std::fflush(&output) == 0) &&
            (std::ferror(&output) == 0);
    } catch (...) {
        return false;
    }
}

} // namespace

auto Print(Stdout, const std::string_view text) noexcept -> bool {
    return Print(*stdout, text);
}

auto Print(Stderr, const std::string_view text) noexcept -> bool {
    return Print(*stderr, text);
}

} // namespace devicefs::stream_writer_detail
