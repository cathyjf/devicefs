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

export module devicefs.supervisor.temporary_paths;

import std;
import <devicefs/common.h>;
import <devicefs/windows_imports.h>;
import devicefs.guid_formatter;
import devicefs.stream_writer;

export [[nodiscard]] auto UniqueName() {
    return FormatGuid<false>(GenerateRandomGuid());
}

// TODO: This function uses many heap allocations for a task that should really
//       only require one heap allocation.
export [[nodiscard]] auto TemporarySystemDirectoryPath(
    const std::string_view prefix) {
    const auto windows_directory = [] {
        auto path = std::wstring{};
        const auto result = wil::GetWindowsDirectoryW(path);
        if (FAILED(result)) {
            WinError("failed to obtain the path to the Windows directory",
                ExplicitHresult{result});
        }
        return path;
    }();
    return std::filesystem::path{windows_directory} /
        "SystemTemp" /
        std::format("{}-{}", prefix, UniqueName());
}

export class TemporaryDirectory {
public:
    explicit TemporaryDirectory(std::filesystem::path path)
        : path_{std::move(path)} {
        if (!CreateDirectoryW(path_.c_str(), nullptr)) {
            WinError("could not create temporary directory '{}'",
                std::wstring_view{path_.native()});
        }
    }

    TemporaryDirectory(const TemporaryDirectory &) = delete;
    auto operator=(const TemporaryDirectory &) -> TemporaryDirectory & = delete;
    TemporaryDirectory(TemporaryDirectory &&) = delete;
    auto operator=(TemporaryDirectory &&) -> TemporaryDirectory & = delete;

    ~TemporaryDirectory() noexcept {
        if (const auto result = wil::RemoveDirectoryRecursiveNoThrow(path_.c_str());
            FAILED(result)) {
            devicefs::WriteToStream(devicefs::stderr,
                L"backup-supervisor: could not remove temporary directory '{}': {}\n",
                std::wstring_view{path_.native()},
                TryConstructWinError("", ExplicitHresult{result}));
        }
    }

    [[nodiscard]] auto Path() const noexcept -> const std::filesystem::path & {
        return path_;
    }

private:
    std::filesystem::path path_;
};
