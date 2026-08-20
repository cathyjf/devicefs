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

#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>

#include <wil/safecast.h>

#include <array>
#include <format>
#include <ranges>
#include <stdexcept>
#include <string>
#include <vector>

// The imported winrt_apartment module transitively references
// `StringValidateDestW`, a function declared and defined in `strsafe.h`.
// Because of an apparent MSVC++ compiler bug, the compiler is able to find the
// definition of `StringValidateDestW` but not its declaration, even though
// both should already be visible. Explicitly including `strsafe.h` here is a
// workaround that satisfies the compiler.
#include <strsafe.h>

module devicefs.supervisor.native_backup;

import devicefs.supervisor.vshadow;
import devicefs.supervisor.winrt_apartment;

namespace {

[[nodiscard]] auto VolumeMountPoints(const std::wstring &volume)
    -> std::vector<std::wstring> {
    auto required = DWORD{};
    if (GetVolumePathNamesForVolumeNameW(
            volume.c_str(), nullptr, 0, &required) ||
        (GetLastError() != ERROR_MORE_DATA)) {
        return {};
    }
    auto paths = std::vector<wchar_t>(required);
    if (!GetVolumePathNamesForVolumeNameW(
            volume.c_str(), paths.data(), required, &required)) {
        return {};
    }
    auto result = std::vector<std::wstring>{};
    for (const auto path : paths | std::views::split(L'\0')) {
        if (path.empty()) {
            break;
        }
        result.emplace_back(path.begin(), path.end());
    }
    return result;
}

[[nodiscard]] auto VolumeLabel(const std::wstring &volume) {
    auto label = std::array<wchar_t, MAX_PATH + 1>{};
    if (!GetVolumeInformationW(
            volume.c_str(), label.data(),
            wil::safe_cast<DWORD>(label.size()),
            nullptr, nullptr, nullptr, nullptr, 0)) {
        return std::wstring{};
    }
    return std::wstring{label.data()};
}

} // namespace

namespace internal {

[[nodiscard]] auto SnapshotImageName(
    const devicefs::vshadow::Snapshot &snapshot) -> std::wstring {
    return std::format(
        L"volume-{}.img", snapshot.original_volume.substr(11, 36));
}

[[nodiscard]] auto SerializeSnapshotManifest(
    const devicefs::vshadow::SnapshotSet &snapshot_set) -> std::u8string {
    try {
        const auto apartment = WinrtApartment{
            "could not initialize the Windows Runtime while serializing "
            "the backup manifest"};
        using winrt::Windows::Data::Json::JsonObject;
        using winrt::Windows::Data::Json::JsonArray;
        using winrt::Windows::Data::Json::JsonValue;

        auto volumes = JsonObject{};
        for (const auto &snapshot : snapshot_set.snapshots) {
            auto mount_points = JsonArray{};
            for (const auto &mount_point :
                VolumeMountPoints(snapshot.original_volume)) {
                mount_points.Append(
                    JsonValue::CreateStringValue(mount_point));
            }
            auto notes = JsonObject{};
            notes.SetNamedValue(L"mount-points", mount_points);
            notes.SetNamedValue(L"volume-label", JsonValue::CreateStringValue(
                VolumeLabel(snapshot.original_volume)));

            auto volume = JsonObject{};
            volume.SetNamedValue(L"snapshot-id", JsonValue::CreateStringValue(
                winrt::to_hstring(snapshot.identifier)));
            volume.SetNamedValue(L"notes", notes);
            volumes.SetNamedValue(SnapshotImageName(snapshot), volume);
        }

        auto result = JsonObject{};
        result.SetNamedValue(
            L"version", JsonValue::CreateNumberValue(1));
        result.SetNamedValue(L"snapshot-set", JsonValue::CreateStringValue(
            winrt::to_hstring(snapshot_set.identifier)));
        result.SetNamedValue(L"volumes", volumes);
        const auto encoded = winrt::to_string(result.Stringify());
        return std::u8string{encoded.begin(), encoded.end()};
    } catch (const winrt::hresult_error &) {
        throw std::runtime_error(
            "could not serialize the backup manifest");
    }
}

} // namespace internal
