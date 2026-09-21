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

module devicefs.supervisor.native_backup:privileges;

import std;
import <cstddef>;
import <devicefs/windows_imports.h>;
import <devicefs/common.h>;

namespace internal {

template <std::size_t PrivilegeCount>
class ProcessPrivilegeEnabler {
    static constexpr auto kStateSize =
        offsetof(TOKEN_PRIVILEGES, Privileges) +
        PrivilegeCount * sizeof(LUID_AND_ATTRIBUTES);

public:
    explicit ProcessPrivilegeEnabler(
        const HANDLE process,
        const std::span<const wil::zwstring_view, PrivilegeCount> privilege_names,
        const std::string_view description)
        : description_{description} {
        if (!OpenProcessToken(process,
                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                token_.addressof())) {
            WinError("could not open the backup-supervisor process token");
        }

        alignas(TOKEN_PRIVILEGES) auto state_storage = std::array<std::byte, kStateSize>{};
        const auto entries = std::span{
            std::start_lifetime_as_array<LUID_AND_ATTRIBUTES>(
                state_storage.data() +
                    offsetof(TOKEN_PRIVILEGES, Privileges),
                privilege_names.size()),
            privilege_names.size(),
        };
        for (auto &&[entry, name] : std::views::zip(entries, privilege_names)) {
            if (!LookupPrivilegeValueW(nullptr, name.c_str(), &entry.Luid)) {
                WinError("could not identify {} ('{}')",
                    description_,
                    std::wstring_view{name.data(), name.length()});
            }
            entry.Attributes = SE_PRIVILEGE_ENABLED;
        }
        auto *const state =
            std::start_lifetime_as<TOKEN_PRIVILEGES>(state_storage.data());
        state->PrivilegeCount = CompileTimeCast<DWORD, PrivilegeCount>();

        static_cast<void>(std::start_lifetime_as_array<LUID_AND_ATTRIBUTES>(
            previous_state_storage_.data() +
                offsetof(TOKEN_PRIVILEGES, Privileges),
            privilege_names.size()));
        auto *const previous_state = std::start_lifetime_as<TOKEN_PRIVILEGES>(
            previous_state_storage_.data());
        auto previous_state_size = DWORD{};
        if (!AdjustTokenPrivileges(
                token_.get(), FALSE, state,
                CompileTimeCast<DWORD, kStateSize>(), previous_state,
                &previous_state_size)) {
            WinError("could not enable {}", description_);
        }
        previous_state_ = previous_state;
        const auto error = GetLastError();
        if (error != ERROR_SUCCESS) {
            std::ignore = RestoreNoThrow();
            WinError("could not enable {}", description_,
                ExplicitWin32Error{error});
        }
    }

    ProcessPrivilegeEnabler(const ProcessPrivilegeEnabler &) = delete;
    auto operator=(const ProcessPrivilegeEnabler &)
        -> ProcessPrivilegeEnabler & = delete;
    ProcessPrivilegeEnabler(ProcessPrivilegeEnabler &&) = delete;
    auto operator=(ProcessPrivilegeEnabler &&)
        -> ProcessPrivilegeEnabler & = delete;

    ~ProcessPrivilegeEnabler() {
        std::ignore = RestoreNoThrow();
    }

    auto Restore() {
        const auto error = RestoreNoThrow();
        if (error != ERROR_SUCCESS) {
            WinError("could not restore {}", description_,
                ExplicitWin32Error{error});
        }
    }

private:
    [[nodiscard]] auto RestoreNoThrow() noexcept -> DWORD {
        if (previous_state_ == nullptr) {
            return DWORD{ERROR_SUCCESS};
        }
        if (!AdjustTokenPrivileges(
                token_.get(), FALSE, previous_state_,
                0, nullptr, nullptr)) {
            return GetLastError();
        }
        const auto error = GetLastError();
        if (error == ERROR_SUCCESS) {
            previous_state_ = nullptr;
        }
        return error;
    }

    const std::string_view description_;
    wil::unique_handle token_;
    alignas(TOKEN_PRIVILEGES) std::array<std::byte, kStateSize> previous_state_storage_{};
    TOKEN_PRIVILEGES *previous_state_ = nullptr;
};

template <std::size_t PrivilegeCount>
ProcessPrivilegeEnabler(HANDLE,
    const std::array<wil::zwstring_view, PrivilegeCount> &,
    std::string_view) -> ProcessPrivilegeEnabler<PrivilegeCount>;

} // namespace internal
