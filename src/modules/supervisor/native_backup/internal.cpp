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

module devicefs.supervisor.native_backup:internal;

import std;
import <devicefs/windows_imports.h>;
import <sal.h>;
import devicefs.common;
import devicefs.stream_writer;
import devicefs.supervisor.vshadow;

namespace internal {

inline constexpr auto kCancelledExitCode = 130;

[[nodiscard]] auto CancellationRequested(
    const HANDLE cancellation_event) {
    const auto result = WaitForSingleObject(cancellation_event, 0);
    if (result == WAIT_FAILED) {
        WinError("could not inspect the backup cancellation event");
    }
    return result == WAIT_OBJECT_0;
}

template <typename Operation>
[[nodiscard]] auto RunVssOperation(
    const HANDLE cancellation_event,
    Operation &&operation) -> int {
    try {
        return std::invoke(std::forward<Operation>(operation));
    } catch (const std::system_error &error) {
        const auto cancellation_requested =
            CancellationRequested(cancellation_event);
        const auto cancelled = std::error_code(
            ERROR_CANCELLED, std::system_category());
        if (cancellation_requested && (error.code() == cancelled)) {
            return kCancelledExitCode;
        }
        if (error.code() == cancelled) {
            throw devicefs::vshadow::OperationError(error.what());
        }
        throw;
    }
}

[[nodiscard]] auto VolumeImageName(
    const std::string_view volume,
    const std::string_view extension) -> std::string {
    return std::format(
        "volume-{}{}", volume.substr(11, 36), extension);
}

[[nodiscard]] auto SnapshotImageName(
    const devicefs::vshadow::Snapshot &snapshot) -> std::string {
    return VolumeImageName(snapshot.original_volume, ".img");
}

[[nodiscard]] auto WaitForProcess(
    const HANDLE process,
    const std::chrono::milliseconds timeout) -> bool {
    const auto result = WaitForSingleObject(
        process, wil::safe_cast<DWORD>(timeout.count()));
    if (result == WAIT_FAILED) {
        WinError("could not wait for a backup process");
    }
    return result == WAIT_OBJECT_0;
}

[[nodiscard]] auto ProcessExitCode(const HANDLE process) {
    auto result = DWORD{};
    if (!GetExitCodeProcess(process, &result)) {
        WinError("could not obtain a backup process exit code");
    }
    return result;
}

auto TryWriteError(
    _In_z_ const char *const context,
    const std::exception &error) noexcept {
    [[gsl::suppress("26447",
        justification:
            "std::exception::what is noexcept. WriteToStream cannot "
            "throw.")]]
    devicefs::WriteToStream(
        devicefs::stderr, "backup-supervisor: {}: {}\n", context, error.what());
}

} // namespace internal
