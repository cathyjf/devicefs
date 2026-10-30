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

module devicefs.supervisor.native_backup:filesystem_verifier;

import std;
import <devicefs/windows_imports.h>;

struct FilesystemVerificationVolume {
    GUID volume_identifier;
    GUID payload_snapshot_identifier;
    std::string filename;
};

[[nodiscard]] auto VerifyFilesystemViews(
    const HANDLE cancellation_event,
    const std::span<const FilesystemVerificationVolume> volumes,
    const std::filesystem::path &synthetic_mount,
    const std::filesystem::path &real_mount,
    const double percentage,
    const std::size_t optimization_unavailable,
    const std::size_t preparation_failures) -> int;
