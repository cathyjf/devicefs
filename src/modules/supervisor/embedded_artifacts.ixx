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
#include <sddl.h>

// wil/stl.h uses these facilities without including their standard headers.
#include <algorithm>
#include <cstdint>

#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/win32_helpers.h>

#include <array>
#include <cstddef>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

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

namespace {

constexpr auto kArtifactSecurity = wil::zwstring_view(
    L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)");
constexpr auto kGuidBufferCharacters = 39;

struct Artifact {
    wil::zwstring_view resource;
    std::wstring_view filename;
};

constexpr auto kArtifacts = std::array{
    Artifact{L"ORCHESTRATE_BACKUP", L"Orchestrate-Backup.ps1"},
    Artifact{L"COMPLETE_BACKUP", L"Complete-Backup.ps1"},
    Artifact{L"VSHADOW_HELPER", L"VShadow-Helper.cmd"},
    Artifact{L"START_PBS", L"start-pbs.fish"},
};

[[nodiscard]] auto UniqueRunDirectory() {
    auto identifier = GUID{};
    const auto error = CoCreateGuid(&identifier);
    if (FAILED(error)) {
        WinError("could not create a unique backup directory name",
            ExplicitWin32Error{
                static_cast<DWORD>(HRESULT_CODE(error))});
    }
    auto text = std::array<wchar_t, kGuidBufferCharacters>{};
    if (StringFromGUID2(identifier, text.data(), kGuidBufferCharacters) == 0) {
        throw std::runtime_error(
            "could not format the unique backup directory name");
    }
    return SystemTempDirectory() /
        std::format(L"devicefs-supervisor-{}", text.data());
}

[[nodiscard]] auto ArtifactSecurityDescriptor() {
    auto descriptor = wil::unique_hlocal_security_descriptor{};
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            kArtifactSecurity.c_str(), SDDL_REVISION_1,
            descriptor.addressof(), nullptr)) {
        WinError("could not create the embedded-artifact security descriptor");
    }
    return descriptor;
}

auto ExtractArtifact(
    const Artifact &artifact,
    const std::filesystem::path &directory,
    SECURITY_ATTRIBUTES &security) {
    const auto resource = FindResourceW(
        nullptr, artifact.resource.c_str(), L"DEVICEFS_ARTIFACT");
    if (resource == nullptr) {
        WinError("could not find an embedded backup artifact");
    }
    const auto loaded = LoadResource(nullptr, resource);
    if (loaded == nullptr) {
        WinError("could not load an embedded backup artifact");
    }
    const auto size = SizeofResource(nullptr, resource);
    const auto *const data = LockResource(loaded);
    if ((size == 0) || (data == nullptr)) {
        throw std::runtime_error("an embedded backup artifact is empty");
    }

    auto file = wil::unique_hfile(CreateFileW(
        (directory / artifact.filename).c_str(), GENERIC_WRITE, 0,
        &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) {
        WinError("could not create an extracted backup artifact");
    }
    auto written = DWORD{};
    if (!WriteFile(file.get(), data, size, &written, nullptr)) {
        WinError("could not extract a backup artifact");
    }
    if (written != size) {
        WinError("could not extract the complete backup artifact",
            ExplicitWin32Error{ERROR_WRITE_FAULT});
    }
}

} // namespace

export [[nodiscard]] auto ExtractEmbeddedArtifacts() {
    auto descriptor = ArtifactSecurityDescriptor();
    auto security = SECURITY_ATTRIBUTES{
        .nLength = sizeof(SECURITY_ATTRIBUTES),
        .lpSecurityDescriptor = descriptor.get(),
        .bInheritHandle = FALSE,
    };
    const auto directory = UniqueRunDirectory();
    if (!CreateDirectoryW(directory.c_str(), &security)) {
        WinError("could not create the temporary backup directory");
    }
    auto remove_incomplete_directory = wil::scope_exit([&] {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(directory, ignored);
    });
    for (const auto &artifact : kArtifacts) {
        ExtractArtifact(artifact, directory, security);
    }
    remove_incomplete_directory.release();
    return directory;
}
