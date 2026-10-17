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

export module devicefs.supervisor.vshadow;

import std;
import <vshadow/shadow.h>;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;
import devicefs.guid_formatter;
import devicefs.stream_writer;
import devicefs.terminal.transcoding;

using devicefs::terminal::Transcode;

export namespace devicefs::vshadow {

struct Snapshot {
    GUID identifier{};
    std::string original_volume;
    std::string device;
};

struct SnapshotProperties {
    GUID snapshot_set_identifier{};
    std::string original_volume;
    std::string device;
};

struct SnapshotSet {
    GUID identifier{};
    std::vector<Snapshot> snapshots;
};

} // namespace devicefs::vshadow

auto &GetSnapshotPropertiesCache() {
    static auto cache = std::map<
        GUID, devicefs::vshadow::SnapshotProperties, GuidLess>{};
    return cache;
}

template <class Operation>
concept SnapshotOperation = std::is_invocable_r_v<
    int, Operation &, const devicefs::vshadow::SnapshotSet &>;

class VssClientOwner final : private VssClient {
public:
    template <typename... Arguments>
        requires (sizeof...(Arguments) > 0)
    [[gsl::suppress("26455",
        justification:
            "VssClientOwner does not have a default constructor. This variadic "
            "constructor requires at least one argument, but the analyzer "
            "incorrectly classifies it as a default constructor.")]]
    explicit VssClientOwner(Arguments &&...arguments)
        : VssClient{[](const std::wstring_view text) noexcept {
              devicefs::WriteToStream(devicefs::stdout, L"{}\n", text);
          }} {
        Initialize(std::forward<Arguments>(arguments)...);
    }

    using VssClient::BackupComplete;
    using VssClient::CompleteFailedBackup;
    using VssClient::CreateSnapshotSet;
    using VssClient::GetSnapshotProperties;
    using VssClient::TryDeleteCreatedSnapshotSet;
};

class Backup {
    enum class Completion {
        None,
        Failure,
        Success,
    };

public:
    Backup(const HANDLE cancellation_event, const bool use_writers)
        : client_{
              use_writers ? VSS_CTX_APP_ROLLBACK : VSS_CTX_NAS_ROLLBACK,
              L"", false, cancellation_event},
          use_writers_{use_writers} {}

    Backup(const Backup &) = delete;
    auto operator=(const Backup &) -> Backup & = delete;
    Backup(Backup &&) = delete;
    auto operator=(Backup &&) -> Backup & = delete;

    ~Backup() {
        auto delete_snapshot_set = wil::scope_exit(
            [this]() noexcept { TryDeleteCreatedSnapshotSet(); });
        if (completion_ == Completion::Success) {
            delete_snapshot_set.release();
        }
        try {
            if (completion_ == Completion::Failure) {
                client_.CompleteFailedBackup();
            } else if ((completion_ == Completion::Success) && use_writers_) {
                client_.BackupComplete(true);
            }
        } catch (const HRESULT result) {
            if (completion_ != Completion::Success) {
                return;
            }
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: the backup succeeded but VSS writer "
                "completion failed: {}\n",
                TryConstructWinError("", ExplicitHresult{result}));
        } catch (...) {
            if (completion_ != Completion::Success) {
                return;
            }
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: the backup succeeded but VSS writer "
                "completion failed with an unexpected error; the snapshot "
                "set was retained.\n");
        }
    }

    [[nodiscard]] auto Run(
        const std::vector<std::wstring> &canonical_volumes,
        SnapshotOperation auto &&operation) -> int {
        completion_ = Completion::Failure;
        const auto [snapshot_set_identifier, snapshots] =
            client_.CreateSnapshotSet(canonical_volumes, L"", {}, {});
        if (snapshots.size() != canonical_volumes.size()) {
            throw std::runtime_error(std::format(
                "VSS failed to return information for {} of the {} newly created "
                "snapshots. The operation cannot continue. Aborting.",
                canonical_volumes.size() - snapshots.size(), canonical_volumes.size()));
        }

        [[gsl::suppress("6001",
            justification:
                "This expression initializes `snapshot_set`; it does not read it.")]]
        const auto snapshot_set = devicefs::vshadow::SnapshotSet{
            .identifier = snapshot_set_identifier,
            .snapshots = snapshots | std::views::transform(
                [](const auto &snapshot) {
                    return devicefs::vshadow::Snapshot{
                        .identifier = snapshot.snapshotID,
                        .original_volume = Transcode<std::string>(
                            snapshot.originalVolumeName),
                        .device = Transcode<std::string>(
                            snapshot.snapshotDeviceObject),
                    };
                }) | std::ranges::to<std::vector>(),
        };

        for (const auto &snapshot : snapshot_set.snapshots) {
            GetSnapshotPropertiesCache().emplace(snapshot.identifier,
                devicefs::vshadow::SnapshotProperties{
                    .snapshot_set_identifier = snapshot_set.identifier,
                    .original_volume = snapshot.original_volume,
                    .device = snapshot.device,
                });
        }
        const auto result = operation(snapshot_set);
        if (result == 0) {
            completion_ = Completion::Success;
        }
        return result;
    }

private:
    auto TryDeleteCreatedSnapshotSet() noexcept -> void {
        const auto result = client_.TryDeleteCreatedSnapshotSet();
        if (!FAILED(result)) {
            return;
        }
        devicefs::WriteToStream(devicefs::stderr,
            "backup-supervisor: failed to delete the VSS snapshot set: {}\n",
            TryConstructWinError("", ExplicitHresult{result}));
    }

    VssClientOwner client_;
    const bool use_writers_;
    Completion completion_ = Completion::None;
};

export namespace devicefs::vshadow {

[[nodiscard]] auto QuerySnapshotProperties(
    const std::span<const GUID> snapshot_identifiers) {
    if (snapshot_identifiers.empty()) {
        return std::vector<std::optional<SnapshotProperties>>{};
    }
    auto &cache = GetSnapshotPropertiesCache();
    auto client = [snapshot_identifiers, &cache] -> std::optional<VssClientOwner> {
        if (std::ranges::all_of(snapshot_identifiers,
                [&cache](const GUID &identifier) {
                    return cache.contains(identifier);
                })) {
            return std::nullopt;
        }
        try {
            return std::optional<VssClientOwner>{std::in_place, VSS_CTX_ALL};
        } catch (HRESULT) {
            return std::nullopt;
        }
    }();
    return std::views::zip_transform([&client, &cache](
        const GUID &identifier) -> std::optional<SnapshotProperties> {
        if (const auto cached = cache.find(identifier); cached != cache.end()) {
            return cached->second;
        }
        if (!client) {
            return std::nullopt;
        }
        devicefs::WriteToStream(devicefs::stdout,
            "Retrieving the snapshot set ID, original volume, and device "
            "path for VSS snapshot '{}'.\n", FormatGuid(identifier));
        auto snapshot_set_identifier = GUID{};
        auto original_volume = std::wstring{};
        auto device = std::wstring{};
        try {
            client->GetSnapshotProperties(identifier,
                snapshot_set_identifier, original_volume, device);
        } catch (HRESULT) {
            return std::nullopt;
        }
        return cache.emplace(identifier, SnapshotProperties{
            .snapshot_set_identifier = snapshot_set_identifier,
            .original_volume = Transcode<std::string>(
                original_volume),
            .device = Transcode<std::string>(device),
        }).first->second;
    }, snapshot_identifiers) | std::ranges::to<std::vector>();
}

// Resolve a drive, mount path, or volume name using the same VShadow rules
// used when creating snapshots. The result can also be compared with the
// original volume reported by `QuerySnapshotProperties`.
[[nodiscard]] auto ResolveVolumeName(const std::string &volume) {
    try {
        return GetUniqueVolumeNameForPath(
            Transcode<std::wstring>(volume), true);
    } catch (const HRESULT result) {
        WinError("could not resolve backup volume '{}'",
            volume, ExplicitHresult{result});
    }
}

[[nodiscard]] auto Run(
    const HANDLE cancellation_event,
    const bool use_writers,
    const std::span<const std::string> volumes,
    SnapshotOperation auto &&operation) -> int {
    const auto canonical_volumes = volumes |
        std::views::transform(ResolveVolumeName) |
        std::ranges::to<std::vector<std::wstring>>();
    try {
        return Backup{cancellation_event, use_writers}.Run(
            canonical_volumes, operation);
    } catch (const HRESULT result) {
        WinError("VSS operation failed", ExplicitHresult{result});
    }
}

} // namespace devicefs::vshadow
