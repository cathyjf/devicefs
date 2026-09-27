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
    using VssClient::GetLatestSnapshotDevices;
    using VssClient::GetSnapshotProperties;
    using VssClient::TryDeleteCreatedSnapshotSet;
};

namespace {

auto TryFormatHresult(const HRESULT result) noexcept -> std::string {
    try {
        return ConstructWinError("", ExplicitHresult{result})->what();
    } catch (...) {
        try {
            return "unexpected error";
        } catch (...) {
            // This nonthrowing fallback is reached if the `std::string`
            // constructor throws while copying the above string literal.
            return {};
        }
    }
}

} // namespace

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
                "completion failed: {}\n", TryFormatHresult(result));
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
        const auto [snapshot_set_identifier, snapshot_identifiers] =
            client_.CreateSnapshotSet(canonical_volumes, L"", {}, {});

        [[gsl::suppress("6001",
            justification:
                "This expression initializes `snapshot_set`; it does not read it.")]]
        const auto snapshot_set = devicefs::vshadow::SnapshotSet{
            .identifier = snapshot_set_identifier,
            // The three ranges supplied here to `zip_transform` are guaranteed
            // by construction to have the same length.
            .snapshots = std::views::zip_transform(
                [](const auto &identifier, const auto &volume, const auto &device) {
                    return devicefs::vshadow::Snapshot{
                        .identifier = identifier,
                        .original_volume = Transcode<std::string>(volume),
                        .device = Transcode<std::string>(device),
                    };
                },
                snapshot_identifiers,
                canonical_volumes,
                client_.GetLatestSnapshotDevices()) | std::ranges::to<std::vector>(),
        };

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
            TryFormatHresult(result));
    }

    VssClientOwner client_;
    const bool use_writers_;
    Completion completion_ = Completion::None;
};

export namespace devicefs::vshadow {

[[nodiscard]] auto QuerySnapshotProperties(
    const std::span<const GUID> snapshot_identifiers)
    -> std::vector<std::optional<SnapshotProperties>> {
    auto client = [] -> std::optional<VssClientOwner> {
        try {
            return std::optional<VssClientOwner>{std::in_place, VSS_CTX_ALL};
        } catch (HRESULT) {
            return std::nullopt;
        }
    }();
    if (!client) {
        return std::vector<std::optional<SnapshotProperties>>(
            snapshot_identifiers.size());
    }
    return std::views::zip_transform([&client_ = *client](
        const GUID &identifier) -> std::optional<SnapshotProperties> {
        auto snapshot_set_identifier = GUID{};
        auto original_volume = std::wstring{};
        auto device = std::wstring{};
        try {
            client_.GetSnapshotProperties(identifier,
                snapshot_set_identifier, original_volume, device);
        } catch (HRESULT) {
            return std::nullopt;
        }
        return SnapshotProperties{
            .snapshot_set_identifier = snapshot_set_identifier,
            .original_volume = Transcode<std::string>(
                original_volume),
            .device = Transcode<std::string>(device),
        };
    }, snapshot_identifiers) | std::ranges::to<std::vector>();
}

[[nodiscard]] auto Run(
    const HANDLE cancellation_event,
    const bool use_writers,
    const std::span<const std::string> volumes,
    SnapshotOperation auto &&operation) -> int {
    const auto canonical_volumes = volumes |
        std::views::transform([](const std::string &volume) {
            try {
                return GetUniqueVolumeNameForPath(
                    Transcode<std::wstring>(volume), true);
            } catch (const HRESULT result) {
                WinError("could not resolve backup volume '{}'",
                    volume, ExplicitHresult{result});
            }
        }) | std::ranges::to<std::vector<std::wstring>>();
    try {
        return Backup{cancellation_event, use_writers}.Run(
            canonical_volumes, operation);
    } catch (const HRESULT result) {
        WinError("VSS operation failed", ExplicitHresult{result});
    }
}

} // namespace devicefs::vshadow
