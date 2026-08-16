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

#include <windows.h>

#include <format>
#include <span>
#include <stdexcept>
#include <string_view>

export module devicefs.supervisor.embedded_artifacts;

import devicefs.common;

namespace {

[[nodiscard]] auto LoadProgram(
    const wchar_t *const name,
    const std::string_view description) {
    const auto resource = FindResourceW(
        nullptr, name, L"DEVICEFS_ARTIFACT");
    if (resource == nullptr) {
        WinError("could not find {}", description);
    }
    const auto loaded = LoadResource(nullptr, resource);
    if (loaded == nullptr) {
        WinError("could not load {}", description);
    }
    const auto size = SizeofResource(nullptr, resource);
    const auto *const data = LockResource(loaded);
    if (data == nullptr) {
        throw std::runtime_error(std::format(
            "could not access {}", description));
    }
    return std::span{
        static_cast<const char *>(data), size};
}

} // namespace

export [[nodiscard]] auto StartPbsProgram() {
    return LoadProgram(L"START_PBS", "an embedded backup program");
}

export [[nodiscard]] auto DeviceToFifoProgram() {
    return LoadProgram(
        L"DEVICE_TO_FIFO", "the embedded device-to-FIFO relay");
}
