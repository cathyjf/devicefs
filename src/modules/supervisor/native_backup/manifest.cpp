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
import <devicefs/winrt_imports.h>;
import :internal;
import :pbs;
import devicefs.supervisor.installation;
import devicefs.supervisor.vshadow;
import devicefs.supervisor.winrt_apartment;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

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

using namespace std::string_view_literals;
using namespace wil::literals;

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
            throw std::runtime_error(std::format(
                "{}: '{}'", error,
                Transcode<std::string>(std::wstring_view{
                    value.c_str(), value.size()})));
        }
        return result;
    };

    auto root = JsonObject{nullptr};
    if (!JsonObject::TryParse(
            Transcode<std::wstring>(manifest), root)) {
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
    constexpr auto volume_prefix = L"volume-"sv;
    constexpr auto invalid_volume_identifier =
        "the backup manifest contains an invalid volume identifier"_zv;
    for (const auto &entry : volumes.GetObject()) {
        if (entry.Value().ValueType() != JsonValueType::Object) {
            throw std::runtime_error(std::format(
                "backup-manifest volume '{}' is not an object",
                Transcode<std::string>(entry.Key())));
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
                std::format(
                    "backup-manifest volume '{}' contains an invalid "
                    "snapshot identifier",
                    Transcode<std::string>(entry.Key()))));
    }
    return result;
}

} // namespace

auto PreviousBackupManifestResult::ParseManifest() const
    -> SnapshotManifest {
    try {
        return ParseSnapshotManifest(manifest);
    } catch (const winrt::hresult_error &error) {
        throw std::runtime_error(std::format(
            "the Windows Runtime failed while parsing the backup manifest: {}",
            Transcode<std::string>(error.message())));
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
        constexpr auto prefix = R"(\\?\Volume)"sv;
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

export struct BackupVolumeDescription {
    std::vector<std::string> mount_points;
    std::string label;
};

// Read display notes by image archive name. Optional notes that are absent or
// have an unrecognized type contribute no description. Invalid JSON also
// contributes no descriptions. Invalid text encoding throws std::invalid_argument.
export [[nodiscard]] auto ReadBackupVolumeDescriptions(const std::u8string_view manifest)
    -> std::map<std::string, BackupVolumeDescription> {
    const auto apartment = WinrtApartment{
        "could not initialize the Windows Runtime to read backup volume descriptions"};
    using winrt::Windows::Data::Json::JsonObject;
    using winrt::Windows::Data::Json::JsonValueType;
    auto root = JsonObject{nullptr};
    if (!JsonObject::TryParse(Transcode<std::wstring>(manifest), root)) {
        return {};
    }
    const auto note = [](const JsonObject &object, const std::wstring_view name,
        const JsonValueType type) {
        const auto value = object.GetNamedValue(name, nullptr);
        return value && (value.ValueType() == type) ? value : nullptr;
    };
    auto result = std::map<std::string, BackupVolumeDescription>{};
    const auto volumes = note(root, L"volumes", JsonValueType::Object);
    if (!volumes) {
        return result;
    }
    for (const auto &entry : volumes.GetObject()) {
        if (entry.Value().ValueType() != JsonValueType::Object) {
            continue;
        }
        const auto notes = note(entry.Value().GetObject(), L"notes", JsonValueType::Object);
        if (!notes) {
            continue;
        }
        auto &description = result[Transcode<std::string>(entry.Key())];
        if (const auto label = note(notes.GetObject(), L"volume-label", JsonValueType::String)) {
            description.label = Transcode<std::string>(label.GetString());
        }
        if (const auto mounts = note(notes.GetObject(), L"mount-points", JsonValueType::Array)) {
            for (const auto &mount : mounts.GetArray()) {
                if (mount.ValueType() == JsonValueType::String) {
                    description.mount_points.push_back(Transcode<std::string>(mount.GetString()));
                }
            }
        }
    }
    return result;
}

// Without a snapshot, PBS selects the current host's latest backup. An explicit
// snapshot identifies the exact backup whose manifest should be restored.
export [[nodiscard]] auto RetrieveBackupManifest(
    const HANDLE cancellation_event,
    const std::optional<std::u8string> &namespace_override,
    const std::optional<std::string_view> snapshot = std::nullopt)
    -> std::optional<PreviousBackupManifestResult> {
    const auto arguments =
        std::array{"--print-manifest"sv, "--"sv, snapshot.value_or(""sv)};
    auto result = internal::RunPbsFish(
        cancellation_event,
        namespace_override,
        internal::PbsFishRequest{
            .additional_arguments = arguments,
            .send_encryption_key = true,
            .standard_output = internal::PbsStandardOutput::Capture,
        });
    if (!result) {
        return std::nullopt;
    }
    return PreviousBackupManifestResult{
        .exit_code = result->exit_code,
        .manifest = std::move(result->standard_output.value()),
    };
}

namespace internal {

[[nodiscard]] auto SerializeSnapshotManifest(
    const devicefs::vshadow::SnapshotSet &snapshot_set,
    const std::string_view wsl_oci_layer) -> std::u8string {
    try {
        const auto apartment = WinrtApartment{
            "could not initialize the Windows Runtime while serializing "
            "the backup manifest"};
        using winrt::Windows::Data::Json::JsonObject;
        using winrt::Windows::Data::Json::JsonArray;
        using winrt::Windows::Data::Json::JsonValue;

        auto volumes = JsonObject{};
        for (const auto &snapshot : snapshot_set.snapshots) {
            const auto original_volume = Transcode<std::wstring>(
                snapshot.original_volume);
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
            volumes.SetNamedValue(Transcode<std::wstring>(
                SnapshotImageName(snapshot)), volume);
        }

        auto result = JsonObject{};
        result.SetNamedValue(
            L"version", JsonValue::CreateNumberValue(1));
        result.SetNamedValue(L"snapshot-set", JsonValue::CreateStringValue(
            winrt::to_hstring(snapshot_set.identifier)));
        result.SetNamedValue(L"volumes", volumes);

        const auto notes = [wsl_oci_layer] {
            auto notes = JsonObject{};
            if (const auto version = CurrentProductVersion<std::wstring>()) {
                notes.SetNamedValue(L"devicefs-supervisor-version",
                    JsonValue::CreateStringValue(*version));
            }
            if (!wsl_oci_layer.empty()) {
                notes.SetNamedValue(L"wsl-oci", JsonValue::CreateStringValue(
                    Transcode<std::wstring>(wsl_oci_layer)));
            }
            return notes;
        }();
        if (notes.Size() != 0) {
            result.SetNamedValue(L"notes", notes);
        }

        return Transcode<std::u8string>(result.Stringify());
    } catch (const winrt::hresult_error &error) {
        throw std::runtime_error(std::format(
            "could not serialize the backup manifest: {}",
            Transcode<std::string>(error.message())));
    }
}

} // namespace internal
