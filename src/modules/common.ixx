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

#include <bit>
#include <system_error>

export module devicefs.common;

export [[noreturn]] auto WinError(
    const wil::zstring_view operation,
    const DWORD error = GetLastError()) {
    throw std::system_error(
        std::bit_cast<int>(error), std::system_category(),
        operation.c_str());
}

export auto HardenProcess() {
    if (!SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        WinError("could not restrict DLL search directories");
    }

    auto dynamic_code = PROCESS_MITIGATION_DYNAMIC_CODE_POLICY{};
    dynamic_code.ProhibitDynamicCode = 1;
    if (!SetProcessMitigationPolicy(
            ProcessDynamicCodePolicy, &dynamic_code, sizeof(dynamic_code))) {
        WinError("could not prohibit dynamic code");
    }

    auto strict_handles = PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY{};
    strict_handles.RaiseExceptionOnInvalidHandleReference = 1;
    strict_handles.HandleExceptionsPermanentlyEnabled = 1;
    if (!SetProcessMitigationPolicy(
            ProcessStrictHandleCheckPolicy, &strict_handles, sizeof(strict_handles))) {
        WinError("could not enable strict handle checking");
    }

    auto extension_points = PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY{};
    extension_points.DisableExtensionPoints = 1;
    if (!SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy,
            &extension_points, sizeof(extension_points))) {
        WinError("could not disable legacy extension points");
    }

    auto image_load = PROCESS_MITIGATION_IMAGE_LOAD_POLICY{};
    image_load.NoRemoteImages = 1;
    image_load.NoLowMandatoryLabelImages = 1;
    if (!SetProcessMitigationPolicy(
            ProcessImageLoadPolicy, &image_load, sizeof(image_load))) {
        WinError("could not restrict image loading");
    }
}
