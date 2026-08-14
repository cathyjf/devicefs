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

#include <windows.h>

#include <span>
#include <stdexcept>

export module devicefs.supervisor.embedded_artifacts;

import devicefs.common;

export [[nodiscard]] auto StartPbsProgram() {
    const auto resource = FindResourceW(
        nullptr, L"START_PBS", L"DEVICEFS_ARTIFACT");
    if (resource == nullptr) {
        WinError("could not find an embedded backup program");
    }
    const auto loaded = LoadResource(nullptr, resource);
    if (loaded == nullptr) {
        WinError("could not load an embedded backup program");
    }
    const auto size = SizeofResource(nullptr, resource);
    const auto *const data = LockResource(loaded);
    if (data == nullptr) {
        throw std::runtime_error("could not access an embedded backup program");
    }
    return std::span{
        static_cast<const char *>(data), size};
}
