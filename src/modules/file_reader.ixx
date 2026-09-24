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

export module devicefs.file_reader;

import std;
import <share.h>;
import <devicefs/windows_imports.h>;

// Read an entire file as bytes, or return the Windows error from opening or
// reading it. Other readers are permitted, but writers are excluded while the
// file is open. `String` can select a secure allocator for sensitive contents.
export template <class String = std::string>
[[nodiscard]] auto ReadEntireFile(const std::filesystem::path &path)
    -> std::expected<String, DWORD> {
    const auto file = wil::unique_file{
        _wfsopen(path.c_str(), L"rb", _SH_DENYWR)};
    if (!file) {
        return std::unexpected{_doserrno};
    }
    // Iterator reads bypass the stream's error state, and MSVC's file buffer
    // reports both EOF and read failures as the end of the sequence. Keeping
    // the underlying `FILE` lets `ferror` distinguish those outcomes. MSVC's
    // `ifstream(FILE *)` constructor does not take ownership, so `file`
    // remains responsible for closing it after the stream is destroyed. See
    // <https://github.com/microsoft/STL/blob/main/stl/inc/fstream>.
    auto contents = String{
        std::istreambuf_iterator<char>{std::ifstream{file.get()}.rdbuf()}, {}};
    if (std::ferror(file.get())) {
        return std::unexpected{_doserrno};
    }
    return contents;
}
