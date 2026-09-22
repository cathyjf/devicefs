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

export module devicefs.supervisor.process_attribute_list;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;

// Each tuple `Attribute` supplies an attribute key, value pointer, and length.
// Construction calls `UpdateProcThreadAttribute` for each `Attribute`.
// Attribute values must outlive this object.
// https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute
export class ProcessAttributeList {
public:
    template <typename... Attributes>
        requires (sizeof...(Attributes) > 0)
    [[gsl::suppress("26455",
        justification:
            "ProcessAttributeList does not have a default constructor. This "
            "variadic constructor requires at least one argument, but the "
            "analyzer incorrectly classifies it as a default constructor.")]]
    explicit ProcessAttributeList(Attributes &&...entries) :
        storage_([] {
            constexpr auto attribute_count =
                CompileTimeCast<DWORD, sizeof...(Attributes)>();
            auto attribute_bytes = SIZE_T{};
            InitializeProcThreadAttributeList(
                nullptr, attribute_count, 0, &attribute_bytes);
            if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                WinError("could not size the process attribute list");
            }
            auto storage =
                std::make_unique_for_overwrite<std::byte[]>(attribute_bytes);
            auto *const attributes = static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
                CompileTimeCast<LPVOID>(storage.get()));
            _Analysis_assume_(attributes != nullptr);
            if (!InitializeProcThreadAttributeList(
                    attributes, attribute_count, 0, &attribute_bytes)) {
                WinError("could not initialize the process attribute list");
            }
            return storage;
        }()),
        attributes_(static_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
            CompileTimeCast<LPVOID>(storage_.get()))) {
        const auto update = [attributes = get()](
            const DWORD_PTR key, void *const value, const SIZE_T size) {
            if (!UpdateProcThreadAttribute(
                    attributes, 0, key, value, size, nullptr, nullptr)) {
                WinError("could not set process attribute 0x{:x}", key);
            }
        };
        (std::apply(update, std::forward<Attributes>(entries)), ...);
    }

    _Ret_notnull_ [[nodiscard]] auto get() const noexcept {
        return attributes_.get();
    }

private:
    // The `attributes_` object is destroyed before `storage_` so that Windows
    // cleanup runs, if at all, while the `storage_` buffer exists.
    const std::unique_ptr<std::byte[]> storage_;
    const wil::unique_any<PPROC_THREAD_ATTRIBUTE_LIST,
        decltype(&::DeleteProcThreadAttributeList),
        ::DeleteProcThreadAttributeList> attributes_;
};
