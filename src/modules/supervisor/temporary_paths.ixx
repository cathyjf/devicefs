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

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.temporary_paths;

import std;
import <devicefs/windows_imports.h>;
import devicefs.common;

export [[nodiscard]] auto UniqueName() {
    auto id = GUID{};
    const auto result = CoCreateGuid(&id);
    if (FAILED(result)) {
        WinError("could not create a unique backup identifier",
            ExplicitWin32Error::FromHresult(result));
    }
    return std::format(
        "{:08x}{:04x}{:04x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
        id.Data1, id.Data2, id.Data3,
        id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
        id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]);
}

export [[nodiscard]] auto TemporarySystemDirectoryPath(
    const std::string_view prefix) {
    const auto windows_directory = [] {
        auto path = std::wstring{};
        const auto result = wil::GetWindowsDirectoryW(path);
        if (FAILED(result)) {
            WinError("could not obtain the Windows directory",
                ExplicitWin32Error::FromHresult(result));
        }
        return path;
    }();
    return std::filesystem::path{windows_directory} /
        "SystemTemp" /
        std::format("{}-{}", prefix, UniqueName());
}
