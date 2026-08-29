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

export module devicefs.supervisor.vshadow;

import std;
import <vshadow/shadow.h>;
import <devicefs/windows_imports.h>;
import devicefs.stream_writer;

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

class OperationError : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};

} // namespace devicefs::vshadow

namespace {

class VssClientOwner final : private VssClient {
  public:
    template <typename... Arguments>
        requires(sizeof...(Arguments) > 0)
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
        } catch (const HRESULT error) {
            if (completion_ == Completion::Success) {
                devicefs::WriteToStream(devicefs::stderr,
                    "backup-supervisor: VSS writer completion failed "
                    "(HRESULT 0x{:08X}); the backup succeeded and the "
                    "snapshot set was retained.\n",
                    std::bit_cast<unsigned int>(error));
            }
        } catch (...) {
            if (completion_ == Completion::Success) {
                devicefs::WriteToStream(devicefs::stderr,
                    "backup-supervisor: VSS writer completion failed with "
                    "an unexpected error; the backup succeeded and the "
                    "snapshot set was retained.\n");
            }
        }
    }

    [[nodiscard]] auto Run(
        const std::vector<std::wstring> &canonical_volumes,
        const std::function<int(
            const devicefs::vshadow::SnapshotSet &)> &operation) -> int {
        completion_ = Completion::Failure;
        const auto [snapshot_set_identifier, snapshot_identifiers] =
            client_.CreateSnapshotSet(canonical_volumes, L"", {}, {});

        const auto snapshot_devices = client_.GetLatestSnapshotDevices();
        auto snapshot_set = devicefs::vshadow::SnapshotSet{
            .identifier = snapshot_set_identifier,
        };
        for (auto &&[identifier, original_volume, device] :
            std::views::zip(
                snapshot_identifiers,
                canonical_volumes,
                snapshot_devices)) {
            snapshot_set.snapshots.push_back({
                .identifier = identifier,
                .original_volume = std::filesystem::path{
                    original_volume}.string(),
                .device = std::filesystem::path{device}.string(),
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
        const auto error = client_.TryDeleteCreatedSnapshotSet();
        if (FAILED(error)) {
            devicefs::WriteToStream(devicefs::stderr,
                "backup-supervisor: could not delete the persistent VSS "
                "snapshot set (HRESULT 0x{:08X}); the snapshot set may "
                "remain.\n",
                std::bit_cast<unsigned int>(error));
        }
    }

    VssClientOwner client_;
    const bool use_writers_;
    Completion completion_ = Completion::None;
};

[[noreturn]] auto TranslateVssError(const HRESULT error) {
    if (error == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        throw std::system_error(
            ERROR_CANCELLED, std::system_category(), "backup cancelled");
    }
    throw devicefs::vshadow::OperationError(std::format(
        "VSS operation failed with HRESULT 0x{:08X}",
        std::bit_cast<std::uint32_t>(error)));
}

} // namespace

export namespace devicefs::vshadow {

[[nodiscard]] auto QuerySnapshotProperties(
    const std::span<const GUID> snapshot_identifiers)
    -> std::vector<std::optional<SnapshotProperties>> {
    auto result = std::vector<std::optional<SnapshotProperties>>(
        snapshot_identifiers.size());
    try {
        auto client = VssClientOwner{VSS_CTX_ALL};
        for (auto &&[identifier, properties] :
            std::views::zip(snapshot_identifiers, result)) {
            try {
                auto snapshot_set_identifier = GUID{};
                auto original_volume = std::wstring{};
                auto device = std::wstring{};
                client.GetSnapshotProperties(identifier,
                    snapshot_set_identifier, original_volume, device);
                properties = SnapshotProperties{
                    .snapshot_set_identifier = snapshot_set_identifier,
                    .original_volume = std::filesystem::path{
                        original_volume}.string(),
                    .device = std::filesystem::path{device}.string(),
                };
            } catch (const HRESULT) {
                // One unavailable old snapshot does not affect the others.
            }
        }
    } catch (const HRESULT) {
        // Initialization failure leaves no old snapshot properties.
    }
    return result;
}

[[nodiscard]] auto Run(
    const HANDLE cancellation_event,
    const bool use_writers,
    const std::span<const std::string> volumes,
    const std::function<int(const SnapshotSet &)> &operation) -> int {
    try {
        auto canonical_volumes = volumes |
            std::views::transform([](const std::string &volume) {
                return GetUniqueVolumeNameForPath(
                    std::filesystem::path{volume}.wstring(), true);
            }) |
            std::ranges::to<std::vector<std::wstring>>();

        auto backup = Backup{cancellation_event, use_writers};
        return backup.Run(canonical_volumes, operation);
    } catch (const HRESULT error) {
        TranslateVssError(error);
    }
}

} // namespace devicefs::vshadow
