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

export module devicefs.supervisor.native_backup:manifest;

import std;
import <devicefs/windows_imports.h>;
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.Collections.h>;
import :internal;
import :pbs;
import devicefs.supervisor.vshadow;
import devicefs.supervisor.winrt_apartment;

// The Windows GetObject macro conflicts with C++/WinRT IJsonValue::GetObject.
#undef GetObject

export struct PreviousBackupManifestResult {
    int exit_code;
    std::u8string manifest;

    struct SnapshotManifest {
        using GuidLess = decltype([](
            const GUID &left, const GUID &right) noexcept {
                return std::memcmp(&left, &right, sizeof(GUID)) < 0;
            });

        struct SnapshotVolume {
            GUID snapshot_identifier{};
            std::string device;
        };

        using SnapshotVolumes = std::map<GUID, SnapshotVolume, GuidLess>;

        GUID snapshot_set_identifier{};
        std::map<GUID, GUID, GuidLess> volumes;

        [[nodiscard]] auto QuerySnapshotVolumes() const -> SnapshotVolumes;
    };

    [[nodiscard]] auto ParseManifest() const -> SnapshotManifest;
};

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
            wil::safe_cast_failfast<DWORD>(label.size()),
            nullptr, nullptr, nullptr, nullptr, 0)) {
        return std::wstring{};
    }
    return std::wstring{label.data()};
}

[[nodiscard]] auto ParseSnapshotManifest(
    const std::u8string_view manifest) {
    const auto apartment = WinrtApartment{
        "could not initialize the Windows Runtime while parsing "
        "the backup manifest"};
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValueType;

    const auto required_value = [](
        const JsonObject &object, const wil::zwstring_view name,
        const JsonValueType type, const wil::zstring_view error) {
        if (!object.HasKey(name.c_str())) {
            throw std::runtime_error(error.c_str());
        }
        const auto value = object.GetNamedValue(name.c_str());
        if (value.ValueType() != type) {
            throw std::runtime_error(error.c_str());
        }
        return value;
    };
    const auto parse_identifier = [](
        const wil::zwstring_view value,
        const wil::zstring_view error) -> GUID {
        auto result = GUID{};
        if (IIDFromString(value.c_str(), &result) != S_OK) {
            throw std::runtime_error(error.c_str());
        }
        return result;
    };

    auto root = JsonObject{nullptr};
    if (!JsonObject::TryParse(
            std::filesystem::path{manifest}.wstring(), root)) {
        throw std::runtime_error(
            "the backup manifest is not a JSON object");
    }

    const auto snapshot_set = required_value(
        root, L"snapshot-set", JsonValueType::String,
        "the backup manifest does not contain a snapshot-set string");
    const auto volumes = required_value(
        root, L"volumes", JsonValueType::Object,
        "the backup manifest does not contain a volumes object");
    auto result = PreviousBackupManifestResult::SnapshotManifest{
        .snapshot_set_identifier = parse_identifier(
            snapshot_set.GetString().c_str(),
            "the backup manifest contains an invalid snapshot-set identifier"),
    };
    constexpr auto volume_prefix = std::wstring_view{L"volume-"};
    constexpr auto invalid_volume_identifier = wil::zstring_view{
        "the backup manifest contains an invalid volume identifier"};
    for (const auto &entry : volumes.GetObject()) {
        if (entry.Value().ValueType() != JsonValueType::Object) {
            throw std::runtime_error(
                "a backup-manifest volume is not an object");
        }
        const auto snapshot = required_value(
            entry.Value().GetObject(), L"snapshot-id", JsonValueType::String,
            "a backup-manifest volume does not contain a snapshot-id string");
        const auto image_name = std::filesystem::path{
            entry.Key().c_str()}.stem().wstring();
        if (image_name.size() < volume_prefix.size()) {
            throw std::runtime_error(invalid_volume_identifier.c_str());
        }
        const auto volume_identifier = std::format(
            L"{{{}}}", std::wstring_view{image_name}.substr(
                volume_prefix.size()));
        result.volumes.emplace(
            parse_identifier(
                volume_identifier.c_str(),
                invalid_volume_identifier),
            parse_identifier(
                snapshot.GetString().c_str(),
                "a backup-manifest volume contains an invalid snapshot identifier"));
    }
    return result;
}

} // namespace

auto PreviousBackupManifestResult::ParseManifest() const
    -> SnapshotManifest {
    try {
        return ParseSnapshotManifest(manifest);
    } catch (const winrt::hresult_error &) {
        throw std::runtime_error(
            "the Windows Runtime failed while parsing the backup manifest");
    }
}

auto PreviousBackupManifestResult::SnapshotManifest::QuerySnapshotVolumes() const
    -> SnapshotVolumes {
    const auto snapshot_identifiers = volumes |
        std::views::values |
        std::ranges::to<std::vector<GUID>>();
    auto snapshot_properties =
        devicefs::vshadow::QuerySnapshotProperties(snapshot_identifiers);
    const auto parse_volume_identifier = [](
        const std::string_view volume) -> std::optional<GUID> {
        constexpr auto prefix = std::string_view{R"(\\?\Volume)"};
        if (!volume.starts_with(prefix) || !volume.ends_with('\\')) {
            return std::nullopt;
        }
        const auto identifier = volume.substr(
            prefix.size(), volume.size() - prefix.size() - 1);
        try {
            return winrt::guid{identifier};
        } catch (const std::invalid_argument &) {
            return std::nullopt;
        }
    };

    auto result = SnapshotVolumes{};
    for (auto &&[volume, properties] :
        std::views::zip(volumes, snapshot_properties)) {
        const auto &[volume_identifier, snapshot_identifier] = volume;
        if (!properties ||
            (InlineIsEqualGUID(
                properties->snapshot_set_identifier,
                snapshot_set_identifier) == FALSE)) {
            continue;
        }
        const auto original_volume_identifier =
            parse_volume_identifier(properties->original_volume);
        if (!original_volume_identifier ||
            (InlineIsEqualGUID(
                *original_volume_identifier, volume_identifier) == FALSE)) {
            continue;
        }
        result.emplace(
            volume_identifier,
            SnapshotVolume{
                .snapshot_identifier = snapshot_identifier,
                .device = std::move(properties->device),
            });
    }
    return result;
}

export [[nodiscard]] auto RetrievePreviousBackupManifest(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override)
    -> std::optional<PreviousBackupManifestResult> {
    try {
        constexpr auto arguments =
            std::array{std::string_view{"--print-manifest"}};
        auto result = internal::RunPbsFish(
            cancellation_event,
            namespace_override,
            internal::PbsFishRequest{
                .additional_arguments = arguments,
                .standard_output = internal::PbsStandardOutput::Capture,
            });
        if (!result) {
            return std::nullopt;
        }
        return PreviousBackupManifestResult{
            .exit_code = result->exit_code,
            .manifest = std::move(result->standard_output.value()),
        };
    } catch (const wil::ResultException &error) {
        throw std::runtime_error(error.what());
    }
}

namespace internal {

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
            const auto original_volume = std::filesystem::path{
                snapshot.original_volume}.wstring();
            auto mount_points = JsonArray{};
            for (const auto &mount_point :
                VolumeMountPoints(original_volume)) {
                mount_points.Append(
                    JsonValue::CreateStringValue(mount_point));
            }
            auto notes = JsonObject{};
            notes.SetNamedValue(L"mount-points", mount_points);
            notes.SetNamedValue(L"volume-label", JsonValue::CreateStringValue(
                VolumeLabel(original_volume)));

            auto volume = JsonObject{};
            volume.SetNamedValue(L"snapshot-id", JsonValue::CreateStringValue(
                winrt::to_hstring(snapshot.identifier)));
            volume.SetNamedValue(L"notes", notes);
            volumes.SetNamedValue(std::filesystem::path{
                SnapshotImageName(snapshot)}.wstring(), volume);
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
