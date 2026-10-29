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
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;

// Read an entire file as bytes, or return the captured CRT I/O error from opening or
// reading it. Other readers are permitted, but writers are excluded while the
// file is open. If `temporary` is true, use delete-on-close mode with delete
// sharing. `String` can select a secure allocator for sensitive contents.
export template <class String = std::string>
[[nodiscard]] auto ReadEntireFile(
    const std::filesystem::path &path, const bool temporary = false)
    -> std::expected<String, ExplicitCrtIoError> {
    ClearCrtIoError();
    const auto file = wil::unique_file{
        _wfsopen(path.c_str(), temporary ? L"rbD" : L"rb", _SH_DENYWR)};
    if (!file) {
        return std::unexpected{ExplicitCrtIoError{}};
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
        return std::unexpected{ExplicitCrtIoError{}};
    }
    return contents;
}

// Write the entire buffer to a synchronous handle.
// Precondition: `contents.data()` is not null.
export auto WriteEntireBuffer(const HANDLE file, const auto &contents) noexcept
    -> std::expected<void, ExplicitWin32Error> {
    const auto size_in_bytes = [element_count = contents.size(),
        element_size = sizeof(*contents.data())]<class ApiType = DWORD> noexcept
        -> std::optional<ApiType> {
        const auto max_element_count =
            std::numeric_limits<ApiType>::max() / element_size;
        if (element_count > max_element_count) {
            return std::nullopt;
        }
        [[gsl::suppress("26472", justification:
            "The preceding check establishes that the following product fits "
            "in `ApiType`.")]]
        return static_cast<ApiType>(element_count * element_size);
    }();
    if (!size_in_bytes) {
        return std::unexpected{ExplicitWin32Error{ERROR_ARITHMETIC_OVERFLOW}};
    }
    auto written = DWORD{};
    if (!WriteFile(file, contents.data(), *size_in_bytes, &written, nullptr)) {
        return std::unexpected{ExplicitWin32Error{GetLastError()}};
    }
    if (written != *size_in_bytes) {
        return std::unexpected{ExplicitWin32Error{ERROR_WRITE_FAULT}};
    }
    return {};
}

export struct FileWriteOptions {
    bool overwrite = true;
    // If true, the file may also be opened for reading while the returned
    // write handle remains open.
    bool allow_readers = false;
    bool inheritable = false;
    bool delete_on_close = false;
    __pragma(warning(suppress : 26477, justification :
        "The following declaration already uses `nullptr`."))
    void *security_descriptor = nullptr;
};

// Create or truncate a file and then write its complete contents.
// On success, returns a `unique_hfile` containing a handle to the file.
// Precondition: `contents.data()` is not null.
export auto WriteEntireFile(const std::filesystem::path &path,
    const auto &contents, const FileWriteOptions options = {}) noexcept
    -> std::expected<wil::unique_hfile, ExplicitWin32Error> {
    auto attributes = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = options.security_descriptor,
        .bInheritHandle = options.inheritable,
    };
    auto file = wil::unique_hfile{CreateFileW(path.c_str(), GENERIC_WRITE,
        options.allow_readers ? FILE_SHARE_READ : 0, &attributes,
        options.overwrite ? CREATE_ALWAYS : CREATE_NEW,
        options.delete_on_close
            ? FILE_FLAG_DELETE_ON_CLOSE : FILE_ATTRIBUTE_NORMAL,
        nullptr)};
    if (!file) {
        return std::unexpected{ExplicitWin32Error{GetLastError()}};
    }
    if (const auto result = WriteEntireBuffer(file.get(), contents); !result) {
        return std::unexpected{result.error()};
    }
    return file;
}
