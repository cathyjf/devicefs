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

#include <cstdio>

export module devicefs.supervisor.vshadow;

import std;
import <vshadow/shadow.h>;
import <wil/resource.h>;

export namespace devicefs::vshadow {

struct Snapshot {
    GUID identifier{};
    std::wstring original_volume;
    std::wstring device;
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

class Backup {
    enum class Completion {
        None,
        Failure,
        Success,
    };

  public:
    Backup(const HANDLE cancellation_event, const bool use_writers)
        : use_writers_{use_writers} {
        client_.Initialize(
            use_writers ? VSS_CTX_APP_ROLLBACK : VSS_CTX_NAS_ROLLBACK,
            L"", false, cancellation_event);
    }

    Backup(const Backup &) = delete;
    auto operator=(const Backup &) -> Backup & = delete;
    Backup(Backup &&) = delete;
    auto operator=(Backup &&) -> Backup & = delete;

    [[gsl::suppress("26439",
        justification: "Normal destruction reports VSS completion failure; failure cleanup cannot replace an exception already unwinding.")]]
    ~Backup() noexcept(false) {
        const auto delete_snapshot_set = wil::scope_exit(
            [this]() noexcept { TryDeleteCreatedSnapshotSet(); });
        try {
            if (completion_ == Completion::Failure) {
                client_.CompleteFailedBackup();
            } else if ((completion_ == Completion::Success) && use_writers_) {
                client_.BackupComplete(true);
            }
        } catch (...) {
            if ((std::uncaught_exceptions() == 0) &&
                (completion_ == Completion::Success)) {
                throw;
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
        snapshot_set.snapshots.reserve(canonical_volumes.size());
        for (auto index = std::size_t{};
            index < canonical_volumes.size(); ++index) {
            snapshot_set.snapshots.push_back({
                .identifier = snapshot_identifiers[index],
                .original_volume = canonical_volumes[index],
                .device = snapshot_devices[index],
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
            std::fwprintf(stderr,
                L"backup-supervisor: could not delete the persistent VSS "
                L"snapshot set (HRESULT 0x%08X); the snapshot set may "
                L"remain.\n",
                std::bit_cast<unsigned int>(error));
        }
    }

    VssClient client_;
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

[[nodiscard]] auto Run(
    const HANDLE cancellation_event,
    const bool use_writers,
    const std::span<const std::wstring_view> volumes,
    const std::function<int(const SnapshotSet &)> &operation) -> int {
    try {
        auto canonical_volumes = std::vector<std::wstring>{};
        for (const auto volume : volumes) {
            canonical_volumes.push_back(
                GetUniqueVolumeNameForPath(std::wstring{volume}, true));
        }

        auto backup = Backup{cancellation_event, use_writers};
        return backup.Run(canonical_volumes, operation);
    } catch (const HRESULT error) {
        TranslateVssError(error);
    }
}

} // namespace devicefs::vshadow
