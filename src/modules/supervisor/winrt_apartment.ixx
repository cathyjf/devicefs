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

export module devicefs.supervisor.winrt_apartment;

import std;
import <devicefs/windows_imports.h>;
import <sal.h>;

export class WinrtApartment {
  public:
    explicit WinrtApartment(
        _In_z_ const char *const initialization_error)
        : uninitialize_([initialization_error] {
            try {
                return wil::RoInitialize(RO_INIT_SINGLETHREADED);
            } catch (const wil::ResultException &) {
                throw std::runtime_error(initialization_error);
            }
        }()) {}

    WinrtApartment(const WinrtApartment &) = delete;
    auto operator=(const WinrtApartment &) -> WinrtApartment & = delete;
    WinrtApartment(WinrtApartment &&) = delete;
    auto operator=(WinrtApartment &&) -> WinrtApartment & = delete;

    ~WinrtApartment() {
        // When this->uninitialize_ is destroyed, it will call RoUninitialize.
        // RoUninitialize may unload the DLLs that implement the cached JSON factories.
        // Subsequently, a later attempt to use the JSON implementation will crash.
        // To avoid this, clear the factory cache before calling RoUninitialize.
        // See <https://devblogs.microsoft.com/oldnewthing/20211105-00/?p=105878>.
        winrt::clear_factory_cache();
    }

  private:
    wil::unique_rouninitialize_call uninitialize_;
};
