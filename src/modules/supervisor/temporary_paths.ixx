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
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import devicefs.guid_formatter;
import devicefs.stream_writer;
import devicefs.terminal.safecast;

export [[nodiscard]] auto UniqueName() {
    return FormatGuid<false>(GenerateRandomGuid());
}

auto WriteWindowsDirectoryToBuffer(std::wstring &path, auto &&resize) {
    using throw_error = decltype([] static {
        WinError("failed to obtain the path to the Windows directory");
    });
    using windows_dir = decltype([](auto &path) static {
        const auto length = GetWindowsDirectoryW(
            path.data(), devicefs::terminal::FailFastCast<UINT>(
                path.length() + 1));
        if (length == 0) {
            throw_error::operator()();
        }
        return length;
    });
    const auto reported_windows_length = windows_dir::operator()(path);
    const bool retry = reported_windows_length > path.size();
    const auto windows_length =
        retry ? (reported_windows_length - 1) : reported_windows_length;
    std::invoke(std::forward<decltype(resize)>(resize), windows_length);
    if (retry && (windows_dir::operator()(path) != windows_length)) {
        throw_error::operator()();
    }
    return path.begin() + windows_length;
}

// Generate a pathname of the form
//   `$WINDOWS\SystemTemp\$PREFIX-$GUID`
// where
//   `$WINDOWS` is the path to the Windows directory,
//   `$PREFIX` is a caller-supplied prefix, and
//   `$GUID` is a randomly-generated GUID suffix.
//
// The function does not create an object in the file system.
export [[nodiscard]] auto TemporarySystemDirectoryPath(
    const std::wstring_view prefix) {
    using namespace std::string_view_literals;
    constexpr auto likely_windows_length = L"C:\\Windows\\"sv.length();
    constexpr auto system_temp = L"SystemTemp\\"sv;
    const auto caller_prefix_length = prefix.length() + 1;
    constexpr auto guid_length = GuidLength<false>;
    const auto suffix_length =
        system_temp.length() + caller_prefix_length + guid_length;
    const auto minimum_length = likely_windows_length + suffix_length;

    auto path = std::wstring(minimum_length, L'\0');
    const auto cursor = WriteWindowsDirectoryToBuffer(path,
        [&path, suffix_length](const auto windows_length) {
        path.resize(windows_length + suffix_length + 1);
    });
    *cursor = L'\\';
    const auto caller_prefix_cursor =
        std::copy(system_temp.begin(), system_temp.end(), cursor + 1);
    const auto guid_cursor =
        std::copy(prefix.begin(), prefix.end(), caller_prefix_cursor);
    *guid_cursor = L'-';
    FormatGuidToBuffer<false>(GenerateRandomGuid(),
        std::span<wchar_t, guid_length>{guid_cursor + 1, guid_length});

    return std::filesystem::path{std::move(path)};
}

extern "C" auto NTAPI RtlDosPathNameToNtPathName_U_WithStatus(
    _In_ PCWSTR windows_path_name, _Out_ PUNICODE_STRING nt_path_name,
    _Outptr_opt_result_maybenull_ PWSTR *, _Reserved_ PVOID) -> NTSTATUS;

auto GetNtPathFromWin32Path(const std::filesystem::path &path) {
    static const auto dos_path_to_nt_path = [] static {
        const auto handle = LoadLibraryExA(
            "ntdll.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!handle) {
            std::_Exit(GetLastError());
        }
        const auto address = GetProcAddressByFunctionDeclaration(
            handle, RtlDosPathNameToNtPathName_U_WithStatus);
        if (!address) {
            std::_Exit(GetLastError());
        }
        return address;
    }();
    using UniqueUnicodeString = wil::unique_struct<UNICODE_STRING,
        decltype(&::RtlFreeUnicodeString), ::RtlFreeUnicodeString>;
    auto nt_name = UniqueUnicodeString{};
    const auto status = dos_path_to_nt_path(
        path.c_str(), nt_name.addressof(), nullptr, nullptr);
    if (!NT_SUCCESS(status)) {
        WinError("failed to convert Win32 path '{}' to an NT path",
            std::wstring_view{path.native()},
            ExplicitHresult{HRESULT_FROM_NT(status)});
    }
    return nt_name;
}

export auto MakeTemporaryDirectory(std::filesystem::path path) {
    auto nt_name = GetNtPathFromWin32Path(path);
    auto attributes = OBJECT_ATTRIBUTES{
        .Length = sizeof(OBJECT_ATTRIBUTES),
        .ObjectName = nt_name.addressof(),
        .Attributes = OBJ_CASE_INSENSITIVE,
    };
    auto io_status = IO_STATUS_BLOCK{};
    auto handle = wil::unique_hfile{};
    const auto status = NtCreateFile(
        handle.put(), DELETE, &attributes, &io_status,
        nullptr, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        FILE_CREATE, FILE_DIRECTORY_FILE | FILE_DELETE_ON_CLOSE,
        nullptr, 0);
    if (!NT_SUCCESS(status)) {
        WinError("failed to create temporary directory '{}'",
            std::wstring_view{path.native()},
            ExplicitHresult{HRESULT_FROM_NT(status)});
    }
    struct {
        wil::unique_hfile handle;
        std::filesystem::path path;
    } directory = {std::move(handle), std::move(path)};
    return directory;
}
