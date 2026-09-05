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

export module devicefs.supervisor.process_launch;

import std;
import <devicefs/windows_imports.h>;
import devicefs.common;

export [[nodiscard]] auto DuplicateInheritableHandle(const HANDLE source) {
    if ((source == nullptr) || (source == INVALID_HANDLE_VALUE)) {
        throw std::runtime_error("a child-process standard handle is unavailable");
    }
    auto duplicate = wil::unique_handle{};
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(),
            duplicate.addressof(), 0, TRUE, DUPLICATE_SAME_ACCESS)) {
        WinError("could not duplicate a child-process standard handle");
    }
    return duplicate;
}

export template <typename Start>
[[nodiscard]] auto StartProcessWithHandles(
    const HANDLE standard_input,
    const HANDLE standard_output,
    const HANDLE standard_error,
    const Start &start,
    const wil::zstring_view operation) {
    const auto child_handles = std::array{
        DuplicateInheritableHandle(standard_input),
        DuplicateInheritableHandle(standard_output),
        DuplicateInheritableHandle(standard_error),
    };
    auto inherited_handles = std::array{
        child_handles[0].get(), child_handles[1].get(), child_handles[2].get(),
    };
    constexpr auto kAttributeCount = DWORD{1};
    auto attribute_bytes = SIZE_T{};
    InitializeProcThreadAttributeList(
        nullptr, kAttributeCount, 0, &attribute_bytes);
    if ((attribute_bytes == 0) || (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
        WinError("could not size the process attribute list");
    }
    const auto attribute_storage = [&] {
        try {
            return std::make_unique_for_overwrite<std::byte[]>(attribute_bytes);
        } catch (const std::bad_alloc &) {
            WinError("could not allocate the process attribute list",
                ExplicitWin32Error{ERROR_NOT_ENOUGH_MEMORY});
        }
    }();
    auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        static_cast<void *>(attribute_storage.get()));
    if (!InitializeProcThreadAttributeList(
            attributes, kAttributeCount, 0, &attribute_bytes)) {
        WinError("could not initialize the process attribute list");
    }
    const auto delete_attributes = wil::scope_exit(
        [=] { DeleteProcThreadAttributeList(attributes); });
    if (!UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            sizeof(inherited_handles), nullptr, nullptr)) {
        WinError("could not restrict inherited process handles");
    }

    auto startup = STARTUPINFOEXA{
        .StartupInfo = {
            .cb = sizeof(STARTUPINFOEXA),
            .dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES,
            .wShowWindow = SW_HIDE,
            .hStdInput = inherited_handles[0],
            .hStdOutput = inherited_handles[1],
            .hStdError = inherited_handles[2],
        },
        .lpAttributeList = attributes,
    };
    auto process = wil::unique_process_information{};
    if (!start(&startup.StartupInfo, &process)) {
        WinError("{}", operation);
    }
    return process;
}
