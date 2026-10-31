// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

export module devicefs.filesystem.overlapped;

import std;
import <devicefs/windows_imports.h>;

namespace devicefs::filesystem {

export struct Overlapped {
    ULONG_PTR _[2];
    alignas(PVOID) UINT64 offset;
    HANDLE event;

    // Replace this object with an `OVERLAPPED`.
    [[msvc::forceinline]]
    auto Transmogrify() noexcept -> OVERLAPPED * {
        return std::start_lifetime_as<OVERLAPPED>(this);
    }
};

} // namespace devicefs::filesystem
