// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.browse;

import std;
import <devicefs/windows_imports.h>;
import <winrt/Windows.Data.Json.h>;
import <winrt/Windows.Foundation.Collections.h>;
import devicefs.supervisor.winrt_apartment;
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
    auto entries = winrt::Windows::Data::Json::JsonArray{nullptr};
    if (!winrt::Windows::Data::Json::JsonArray::TryParse(
            Transcode<std::wstring>(json), entries)) {
        throw std::runtime_error(
            "The backup catalog returned by PBS is not a valid JSON array.");
    }
    auto groups = std::map<std::string, std::vector<Snapshot>>{};
    for (const auto &[index, entry] : entries | std::views::enumerate) {
        const auto location = std::format("[{}]", index);
        if (entry.ValueType() != JsonValueType::Object) {
            throw std::runtime_error(std::format(
                "PBS backup catalog entry '{}' must be a snapshot object", location));
        }
        const auto object = entry.GetObject();
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
            if (name.ends_with(".img.fidx")) {
                name.resize(name.size() - ".fidx"sv.size());
                snapshot.archives.push_back(std::move(name));
            }
        }
        std::ranges::sort(snapshot.archives);
        groups[group].push_back(std::move(snapshot));
    }
    for (auto &[group, snapshots] : groups) {
        std::ranges::sort(snapshots, std::greater{}, &Snapshot::timestamp);
    }
    return groups;
}

[[nodiscard]] auto Choose(WindowsConsole &terminal, const std::string_view title,
    const std::span<const std::string_view> labels, const std::size_t selected) {
    return SelectMenuItem(terminal, [title](auto &frame) {
        frame.Write("DeviceFs Backup Supervisor\n{}\n\n", title);
    }, labels, {}, selected);
}

} // namespace browse_detail

export struct BackupSelection {
    std::string snapshot;
    std::string archive;
};

// Select a group, snapshot, and image archive from RetrieveBackupCatalog's JSON.
// All menus share one screen lifetime. Returning restores the ordinary console
// so the caller can start the selected view. Cancellation returns no selection.
export [[nodiscard]] auto SelectBackup(const std::u8string_view catalog)
    -> std::optional<BackupSelection> {
    using namespace browse_detail;
    const auto groups = ParseCatalog(catalog);
    auto terminal = WindowsConsole{};
    const auto screen = terminal.EnterScreen();
    const auto group_names = groups | std::views::keys |
        std::ranges::to<std::vector<std::string_view>>();
    auto group_index = 0uz;
    while (const auto group = Choose(terminal,
            groups.empty() ? "No backups were found in this namespace." : "Choose a backup group",
            group_names, group_index)) {
        group_index = *group;
        const auto name = group_names.at(*group);
        const auto &snapshots = groups.at(std::string{name});
        const auto times = snapshots | std::views::transform(&Snapshot::timestamp) |
            std::ranges::to<std::vector<std::string_view>>();
        auto snapshot_index = 0uz;
        while (const auto snapshot = Choose(terminal,
                std::format("{} — choose a snapshot (UTC)", name), times, snapshot_index)) {
            snapshot_index = *snapshot;
            const auto &selected = snapshots.at(*snapshot);
            const auto archives = selected.archives |
                std::ranges::to<std::vector<std::string_view>>();
            if (const auto archive = Choose(terminal,
                    std::format("{} / {} — {}", name, selected.timestamp,
                        archives.empty() ? "This snapshot contains no image archives." :
                        "Choose an image archive"), archives, 0)) {
                return BackupSelection{
                    .snapshot = std::format("{}/{}", name, selected.timestamp),
                    .archive = std::string{archives.at(*archive)},
                };
            }
        }
    }
    return std::nullopt;
}
