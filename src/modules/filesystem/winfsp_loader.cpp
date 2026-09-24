// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

module devicefs.filesystem:winfsp_loader;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;

namespace devicefs::winfsp_loader {

auto LoadWinFspLibrary() {
    constexpr auto dll_name =
        L"" FSP_FSCTL_PRODUCT_FILE_NAME "-" FSP_FSCTL_PRODUCT_FILE_ARCH ".dll";

    // If loading is successful, the library handle is not released because
    // later WinFsp functions will use the loaded library.
    if (LoadLibraryW(dll_name)) {
        return;
    }

    const auto directory = [] {
        auto directory = wil::unique_cotaskmem_string{};
        if (const auto error = wil::reg::get_value_string_nothrow(HKEY_LOCAL_MACHINE,
                L"" FSP_FSCTL_PRODUCT_FULL_REGKEY, L"InstallDir", directory);
            FAILED(error)) {
            WinError("could not read the WinFsp installation directory from 'HKLM\\{}'",
                std::string_view{FSP_FSCTL_PRODUCT_FULL_REGKEY},
                ExplicitHresult{error});
        }
        return std::filesystem::path{directory.get()};
    }();
    const auto path = directory / L"bin" / dll_name;
    if (!LoadLibraryW(path.c_str())) {
        WinError("could not load WinFsp DLL: '{}'", std::wstring_view{path.native()});
    }
}

} // namespace devicefs::winfsp_loader
