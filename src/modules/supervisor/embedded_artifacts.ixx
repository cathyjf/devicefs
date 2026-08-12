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

// wil/stl.h uses these facilities without including their standard headers.
#include <algorithm>
#include <cstdint>

#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <cstddef>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>

export module devicefs.supervisor.embedded_artifacts;

import devicefs.common;

export [[nodiscard]] auto SystemTempDirectory() {
    auto windows_directory = std::wstring{};
    const auto error = wil::AdaptFixedSizeToAllocatedResult(
        windows_directory,
        [](wchar_t *const buffer, const std::size_t capacity,
            std::size_t *const required) -> HRESULT {
            *required = GetSystemWindowsDirectoryW(
                buffer, static_cast<UINT>(capacity));
            RETURN_LAST_ERROR_IF(*required == 0);
            if (*required < capacity) {
                ++*required;
            }
            return S_OK;
        });
    if (FAILED(error)) {
        WinError(
            "could not obtain the system Windows directory",
            ExplicitWin32Error{
                static_cast<DWORD>(HRESULT_CODE(error))});
    }
    return std::filesystem::path(windows_directory) / L"SystemTemp";
}

export [[nodiscard]] auto EmbeddedProgram(
    const wil::zwstring_view resource_name) {
    const auto resource = FindResourceW(
        nullptr, resource_name.c_str(), L"DEVICEFS_ARTIFACT");
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
