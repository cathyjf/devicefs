// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <devicefs/strsafe_compat.h>

export module devicefs.supervisor.browse;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import <devicefs/winrt_imports.h>;
import devicefs.supervisor.winrt_apartment;
import devicefs.supervisor.native_backup;
import devicefs.supervisor.process_privileges;
import devicefs.terminal.menu;
import devicefs.terminal.windows;
import devicefs.terminal.transcoding;

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
        WinError("could not read PBS backup catalog field '{}.{}': {}",
            location, name, std::wstring_view{error.message()},
            ExplicitHresult{error.code()});
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

[[nodiscard]] auto LookupAccountNameFromSid(const PSID user)
    -> std::expected<std::string, std::unique_ptr<std::runtime_error>> {
    const auto get_sid_text = [user] {
        const auto last_error = wil::last_error_context{};
        auto sid_text = wil::unique_hlocal_ansistring{};
        if (!ConvertSidToStringSidA(user, sid_text.addressof())) {
            return wil::make_hlocal_ansistring("[unformattable SID]");
        }
        return sid_text;
    };
    auto name_length = DWORD{};
    auto domain_length = DWORD{};
    auto use = SID_NAME_USE{};
    if (!LookupAccountSidA(nullptr, user, nullptr, &name_length,
            nullptr, &domain_length, &use) &&
        (GetLastError() != ERROR_INSUFFICIENT_BUFFER)) {
        return std::unexpected{ConstructWinError(
            "failed to determine the account name buffer sizes for SID '{}'",
            get_sid_text().get())};
    }
    auto name = std::string(name_length, '\0');
    auto domain = std::string(domain_length, '\0');
    if (!LookupAccountSidA(nullptr, user, name.data(), &name_length,
            domain.data(), &domain_length, &use)) {
        return std::unexpected{ConstructWinError(
            "failed to look up the account name for SID '{}'",
            get_sid_text().get())};
    }
    const auto name_view = std::string_view{name.data(), name_length};
    const auto domain_view = std::string_view{domain.data(), domain_length};
    return domain_view.empty() ? std::string{name_view} :
        std::format("{}\\{}", domain_view, name_view);
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

export struct BackupViewUser {
    struct {
        wil::unique_tokeninfo_ptr<TOKEN_USER> information;
        std::string account_name;
    } invoking_user;
    std::expected<std::optional<decltype(invoking_user)>,
        std::unique_ptr<std::runtime_error>> shell_user;
};

export struct BackupSelection {
    std::u8string namespace_name;
    std::string snapshot;
    std::string archive;
    BackupViewUser view_user;
};

// No value means that the user cancelled the permission menu.
export [[nodiscard]] auto SelectBackupViewUser(
    devicefs::terminal::WindowsConsole &terminal,
    const std::string_view namespace_name, const std::string_view backup,
    const std::string_view archive_label)
    -> std::optional<BackupViewUser> {
    using namespace browse_detail;
    auto user = wil::unique_tokeninfo_ptr<TOKEN_USER>{};
    if (const auto result = wil::get_token_information_nothrow(
            user, GetCurrentProcessToken()); FAILED(result)) {
        WinError("could not identify the invoking user", ExplicitHresult{result});
    }
    // Discovering the desktop process's account is optional. Lookup failures are
    // retained for the scrolling view while access is granted to the invoking account.
    auto shell_user = [sid = user->User.Sid] -> decltype(BackupViewUser::shell_user) {
        const auto shell_window = GetShellWindow();
        if (shell_window == nullptr) {
            return std::nullopt;
        }
        auto process_id = DWORD{};
        if (GetWindowThreadProcessId(shell_window, &process_id) == 0) {
            return std::unexpected(ConstructWinError(
                "failed to identify the desktop process"));
        }
        auto error = std::unique_ptr<std::runtime_error>{};
        const auto privileges = ProcessPrivilegeEnabler{
            GetCurrentProcess(), std::array{wil::zwstring_view(SE_DEBUG_NAME)},
            "SeDebugPrivilege for querying the desktop process"sv, std::ref(error)};
        if (error) {
            return std::unexpected(std::move(error));
        }
        const auto process = wil::unique_handle{
            OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id)};
        if (!process) {
            return std::unexpected(ConstructWinError(
                "failed to open desktop process {}",
                CompileTimeCast<std::uint32_t>(process_id)));
        }
        auto token = wil::unique_handle{};
        if (!OpenProcessToken(process.get(), TOKEN_QUERY, token.addressof())) {
            return std::unexpected(ConstructWinError(
                "failed to open the token for desktop process {}",
                CompileTimeCast<std::uint32_t>(process_id)));
        }
        auto information = wil::unique_tokeninfo_ptr<TOKEN_USER>{};
        if (const auto result = wil::get_token_information_nothrow(
                information, token.get()); FAILED(result)) {
            return std::unexpected(ConstructWinError(
                "failed to identify the user of desktop process {}",
                CompileTimeCast<std::uint32_t>(process_id), ExplicitHresult{result}));
        }
        if (EqualSid(sid, information->User.Sid)) {
            return std::nullopt;
        }
        auto shell_account = LookupAccountNameFromSid(information->User.Sid);
        if (!shell_account) {
            return std::unexpected(std::move(shell_account.error()));
        }
        return decltype(BackupViewUser::invoking_user){
            .information = std::move(information),
            .account_name = std::move(*shell_account)};
    }();
    auto view_user = BackupViewUser{
        .invoking_user = {.information = std::move(user)},
        .shell_user = std::move(shell_user)};
    if (!view_user.shell_user || !*view_user.shell_user) {
        return view_user;
    }
    auto account = LookupAccountNameFromSid(
        view_user.invoking_user.information->User.Sid);
    if (!account) {
        view_user.shell_user = std::unexpected(std::move(account.error()));
        return view_user;
    }
    const auto &shell_account = (**view_user.shell_user).account_name;
    const auto choices = std::array{
        std::format("Grant permission to '{}'", shell_account),
        std::format("Continue without granting permission to '{}'", shell_account)};
    const auto labels = std::array{
        std::string_view{choices[0]}, std::string_view{choices[1]}};
    const auto selected = SelectMenuItem(terminal,
        [&](auto &frame) {
            frame.Write(
                "DeviceFs Backup Supervisor\nBrowse backup\n\n"
                "Namespace: {}\nBackup: {}\nImage: {}\n\n",
                namespace_name, backup, archive_label);
            frame.Write(
                "This program is running as '{}'.\n"
                "Windows File Explorer is running as '{}'.\n\n"
                "You can grant inspection permission to '{}' to enable browsing\n"
                "this backup using Windows File Explorer, if desired.\n\n"
                "Grant permission to '{}' to inspect this backup?",
                *account, shell_account, shell_account, shell_account);
        }, labels);
    if (!selected) {
        return std::nullopt;
    }
    if (*selected != 0) {
        view_user.shell_user = std::nullopt;
    }
    return view_user;
}

// Select a group, snapshot, image archive, and the account allowed to browse it.
// The caller keeps `terminal` and its `EnterScreen` owner alive through selection
// and the selected view. Cancellation returns no selection.
// `retrieve_manifest` receives the full namespace and selected snapshot identifier
// and returns its manifest, or no value if retrieval was cancelled.
export template <class RetrieveManifest>
[[nodiscard]] auto SelectBackup(devicefs::terminal::WindowsConsole &terminal,
    const std::u8string_view catalog,
    const RetrieveManifest retrieve_manifest)
    -> std::optional<BackupSelection> {
    using namespace browse_detail;
    const auto groups = ParseCatalog(catalog);
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
            auto archive_index = 0uz;
            while (const auto archive = Choose(terminal,
                    std::format("{} / {} — {}", name, selected.timestamp,
                        archives.empty() ? "This snapshot contains no image archives." :
                        "Choose an image archive"), archives, archive_index)) {
                archive_index = *archive;
                auto view_user = SelectBackupViewUser(terminal,
                    selected_group.namespace_name.empty() ? std::string{"(root)"} :
                        Transcode<std::string>(selected_group.namespace_name),
                    identifier, choices.at(*archive).first);
                if (!view_user) {
                    continue;
                }
                return BackupSelection{
                    .namespace_name = selected_group.namespace_name,
                    .snapshot = identifier,
                    .archive = std::string{choices.at(*archive).second},
                    .view_user = std::move(*view_user),
                };
            }
        }
    }
    return std::nullopt;
}
