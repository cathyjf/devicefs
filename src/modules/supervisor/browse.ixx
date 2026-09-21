// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.browse;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/winrt_imports.h>;
import devicefs.supervisor.winrt_apartment;
import devicefs.supervisor.native_backup;
import devicefs.terminal.menu;
import devicefs.terminal.windows;
import devicefs.terminal.transcoding;

#undef GetObject

namespace browse_detail {

using namespace std::string_view_literals;
using namespace devicefs::terminal;

struct Snapshot {
    std::string timestamp;
    std::vector<std::string> archives;
    bool has_manifest = false;
};

struct BackupGroup {
    std::u8string namespace_name;
    std::string name;
    std::vector<Snapshot> snapshots;
};

[[nodiscard]] auto ReadCatalogField(
    const winrt::Windows::Data::Json::JsonObject &object,
    const std::wstring_view name, const std::string_view location, const auto read) {
    try {
        return std::invoke(read, object.GetNamedValue(name));
    } catch (const winrt::hresult_error &error) {
        throw std::runtime_error(std::format(
            "could not read PBS backup catalog field '{}.{}': {}",
            location, Transcode<std::string>(name),
            Transcode<std::string>(error.message())));
    }
}

// The catalog already contains each snapshot's file list. Only image archives
// can supply the block device used by the selective-view operation.
[[nodiscard]] auto ParseCatalog(const std::u8string_view json) {
    const auto apartment = WinrtApartment{
        "could not initialize the Windows Runtime to read the backup catalog"};
    using winrt::Windows::Data::Json::JsonValue;
    using winrt::Windows::Data::Json::JsonValueType;
    auto catalog = winrt::Windows::Data::Json::JsonObject{nullptr};
    if (!winrt::Windows::Data::Json::JsonObject::TryParse(
            Transcode<std::wstring>(json), catalog)) {
        throw std::runtime_error(
            "The backup catalog returned by PBS is not a valid JSON object.");
    }
    const auto starting_namespace = Transcode<std::string>(ReadCatalogField(
        catalog, L"namespace", "catalog", &JsonValue::GetString));
    const auto entries = ReadCatalogField(
        catalog, L"snapshots", "catalog", &JsonValue::GetArray);
    auto groups = std::map<std::string, BackupGroup>{};
    for (const auto &[index, entry] : entries | std::views::enumerate) {
        const auto location = std::format("[{}]", index);
        if (entry.ValueType() != JsonValueType::Object) {
            throw std::runtime_error(std::format(
                "PBS backup catalog entry '{}' must be a snapshot object", location));
        }
        const auto object = entry.GetObject();
        const auto namespace_name = Transcode<std::string>(ReadCatalogField(
            object, L"namespace", location, &JsonValue::GetString));
        const auto group = std::format("{}/{}",
            Transcode<std::string>(ReadCatalogField(
                object, L"backup-type", location, &JsonValue::GetString)),
            Transcode<std::string>(ReadCatalogField(
                object, L"backup-id", location, &JsonValue::GetString)));
        const auto seconds = ReadCatalogField(
            object, L"backup-time", location, &JsonValue::GetNumber);
        // Check JSON's floating-point value before converting to integral
        // seconds. The upper comparison is strict because converting the
        // largest int64_t to double rounds upward to the first invalid value.
        if (!std::isfinite(seconds) ||
            (seconds < static_cast<double>(std::numeric_limits<std::int64_t>::min())) ||
            (seconds >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) ||
            (std::trunc(seconds) != seconds)) {
            throw std::runtime_error(std::format(
                "backup catalog group '{}' has an invalid snapshot time: {}",
                group, seconds));
        }
        auto snapshot = Snapshot{
            .timestamp = std::format("{:%FT%TZ}", std::chrono::sys_seconds{
                std::chrono::seconds{static_cast<std::int64_t>(seconds)}}),
        };
        const auto files = ReadCatalogField(
            object, L"files", location, &JsonValue::GetArray);
        for (const auto &[file_index, file] : files | std::views::enumerate) {
            const auto file_location = std::format("{}.files[{}]", location, file_index);
            if (file.ValueType() != JsonValueType::Object) {
                throw std::runtime_error(std::format(
                    "PBS backup catalog entry '{}' must be a file object", file_location));
            }
            auto name = Transcode<std::string>(
                ReadCatalogField(file.GetObject(), L"filename",
                    file_location, &JsonValue::GetString));
            if (name == "devicefs-manifest.conf.blob") {
                snapshot.has_manifest = true;
            }
            if (name.ends_with(".img.fidx")) {
                name.resize(name.size() - ".fidx"sv.size());
                snapshot.archives.push_back(std::move(name));
            }
        }
        const auto relative_namespace = starting_namespace.empty() ? namespace_name :
            namespace_name == starting_namespace ? std::string{} :
            namespace_name.substr(starting_namespace.size() + 1);
        const auto label = relative_namespace.empty() ? group :
            std::format("{}/{}", relative_namespace, group);
        const auto position = groups.try_emplace(label, BackupGroup{
            .namespace_name = Transcode<std::u8string>(namespace_name),
            .name = group,
        }).first;
        position->second.snapshots.push_back(std::move(snapshot));
    }
    for (auto &[label, group] : groups) {
        std::ranges::sort(group.snapshots, std::greater{}, &Snapshot::timestamp);
    }
    return groups;
}

[[nodiscard]] auto Choose(WindowsConsole &terminal, const std::string_view title,
    const std::span<const std::string_view> labels, const std::size_t selected) {
    return SelectMenuItem(terminal, [title](auto &frame) {
        frame.Write("DeviceFs Backup Supervisor\n{}\n\n", title);
    }, labels, {}, selected);
}

// Return display labels, or no value if the manifest retrieval was cancelled.
[[nodiscard]] auto MakeArchiveLabels(const std::u8string_view namespace_name,
    const std::string_view snapshot,
    const std::span<const std::string> archives,
    const auto &retrieve_manifest) -> std::optional<std::vector<std::string>> {
    const auto manifest = retrieve_manifest(namespace_name, snapshot);
    if (!manifest) {
        return std::nullopt;
    }
    const auto descriptions = [&manifest] {
        try {
            return ReadBackupVolumeDescriptions(*manifest);
        } catch (const std::invalid_argument &) {
            // Invalid UTF-8 or an unpaired surrogate in a JSON string
            // leaves this menu with the original image names.
            return decltype(ReadBackupVolumeDescriptions(*manifest)){};
        }
    }();
    return archives | std::views::transform([&descriptions](const auto &archive) {
        const auto found = descriptions.find(archive);
        if (found == descriptions.end()) {
            return archive;
        }
        const auto mount_points = found->second.mount_points |
            std::views::filter([](const auto &mount) { return !mount.empty(); }) |
            std::views::join_with(", "sv) | std::ranges::to<std::string>();
        const auto label = !mount_points.empty() && !found->second.label.empty()
            ? std::format(" ({})", found->second.label) : found->second.label;
        const auto prefix = mount_points + label;
        if (!prefix.empty()) {
            return std::format("{} — {}", prefix, archive);
        }
        return archive;
    }) | std::ranges::to<std::vector<std::string>>();
}

} // namespace browse_detail

export struct BackupSelection {
    std::u8string namespace_name;
    std::string snapshot;
    std::string archive;
};

// Select a group, snapshot, and image archive from RetrieveBackupCatalog's JSON.
// All menus share one screen lifetime. Returning restores the ordinary console
// so the caller can start the selected view. Cancellation returns no selection.
// retrieve_manifest receives the full namespace and selected snapshot identifier
// and returns its manifest, or no value if retrieval was cancelled.
export template <class RetrieveManifest>
[[nodiscard]] auto SelectBackup(const std::u8string_view catalog,
    const RetrieveManifest retrieve_manifest)
    -> std::optional<BackupSelection> {
    using namespace browse_detail;
    const auto groups = ParseCatalog(catalog);
    auto terminal = WindowsConsole{};
    const auto screen = terminal.EnterScreen();
    const auto group_names = groups | std::views::keys |
        std::ranges::to<std::vector<std::string_view>>();
    auto group_index = 0uz;
    while (const auto group = Choose(terminal,
            groups.empty() ? "No backups were found in this namespace or its descendants." : "Choose a backup group",
            group_names, group_index)) {
        group_index = *group;
        const auto name = group_names.at(*group);
        const auto &selected_group = groups.at(std::string{name});
        const auto &snapshots = selected_group.snapshots;
        const auto times = snapshots | std::views::transform(&Snapshot::timestamp) |
            std::ranges::to<std::vector<std::string_view>>();
        auto snapshot_index = 0uz;
        while (const auto snapshot = Choose(terminal,
                std::format("{} — choose a snapshot (UTC)", name), times, snapshot_index)) {
            snapshot_index = *snapshot;
            const auto &selected = snapshots.at(*snapshot);
            const auto identifier = std::format("{}/{}", selected_group.name, selected.timestamp);
            const auto labels = [&] {
                if (!selected.has_manifest) {
                    return std::optional{selected.archives};
                }
                // Retrieval can write diagnostics to the console. The next menu
                // must repaint those cells while retaining this screen owner.
                terminal.InvalidateFrame();
                return MakeArchiveLabels(selected_group.namespace_name,
                    identifier, selected.archives, retrieve_manifest);
            }();
            if (!labels) {
                return std::nullopt;
            }
            const auto choices = [&labels_ = *labels, &archives = selected.archives] {
                auto choices = std::views::zip_transform(
                    [](const auto &label, const auto &archive) {
                        return std::pair{std::string_view{label}, std::string_view{archive}};
                    }, labels_, archives) | std::ranges::to<std::vector>();
                std::ranges::stable_sort(choices, {}, [](const auto &choice) {
                    return choice.first;
                });
                return choices;
            }();
            const auto archives = choices | std::views::keys |
                std::ranges::to<std::vector<std::string_view>>();
            const auto archive = Choose(terminal,
                std::format("{} / {} — {}", name, selected.timestamp,
                    archives.empty() ? "This snapshot contains no image archives." :
                    "Choose an image archive"), archives, 0);
            if (!archive) {
                continue;
            }
            return BackupSelection{
                .namespace_name = selected_group.namespace_name,
                .snapshot = identifier,
                .archive = std::string{choices.at(*archive).second},
            };
        }
    }
    return std::nullopt;
}
