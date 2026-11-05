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
export auto WriteEntireBuffer(const HANDLE file, const auto &contents) noexcept
    -> std::expected<void, ExplicitWin32Error> {
    const auto size_in_bytes = sizeof(*contents.data()) * contents.size();
    const auto buffer = ByteViewCast<const std::byte *>(contents.data());
    using ApiType = DWORD;
    constexpr auto max_chunk_size =
        CompileTimeCast<std::size_t, std::numeric_limits<ApiType>::max()>();
    for (auto i = 0uz; i < size_in_bytes;) {
        const auto chunk_size = static_cast<ApiType>(
            std::min(max_chunk_size, size_in_bytes - i));
        auto written = ApiType{};
        if (!WriteFile(file, buffer + i, chunk_size, &written, nullptr)) {
            return std::unexpected{ExplicitWin32Error{GetLastError()}};
        }
        if (written != chunk_size) {
            return std::unexpected{ExplicitWin32Error{ERROR_WRITE_FAULT}};
        }
        i += chunk_size;
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

// Create a delete-on-close temporary file, and return a read/delete handle
// followed by a write handle.
//
// Separate handles allow the write handle to be closed without deleting the
// file. The written file can then be consumed by another read operation while
// the read/delete handle remains open.
export [[nodiscard]] auto CreateTemporaryFileWithSeparateHandles(
    const std::filesystem::path &path, const DWORD extra_reader_flags = 0)
    -> std::pair<wil::unique_hfile, wil::unique_hfile> {
    auto file = wil::unique_hfile{CreateFileW(path.c_str(),
        GENERIC_READ | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, CREATE_NEW,
        FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN |
            extra_reader_flags,
        nullptr)};
    if (!file) {
        WinError("failed to create temporary file '{}'",
            std::wstring_view{path.native()});
    }
    auto writer = wil::unique_hfile{ReOpenFile(file.get(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_DELETE, 0)};
    if (!writer) {
        WinError("failed to open temporary file '{}' for writing",
            std::wstring_view{path.native()});
    }
    return {std::move(file), std::move(writer)};
}
