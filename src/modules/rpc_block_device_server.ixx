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

#define RPC_USE_NATIVE_WCHAR

#include <sal.h>
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <rpc.h>
#include <rpcasync.h>
#include <intrin.h>

#include <devicefs/rpc_block_device.h>

export module devicefs.rpc_block_device_server;

import std;
import devicefs.common;
import devicefs.filesystem;
import devicefs.rpc_constants;

export namespace devicefs {

template <BlockDevice DeviceType>
class RpcBlockDeviceServer {
  public:
    using Devices =
        std::vector<std::pair<
            std::basic_string<unsigned char>, DeviceType>>;

    [[nodiscard]] static auto Start(
        std::wstring endpoint,
        const DWORD client_process_identifier,
        Devices devices) {
        return RpcBlockDeviceServer{
            std::move(endpoint), client_process_identifier,
            std::move(devices)};
    }

    RpcBlockDeviceServer(const RpcBlockDeviceServer &) = delete;
    auto operator=(const RpcBlockDeviceServer &)
        -> RpcBlockDeviceServer & = delete;
    RpcBlockDeviceServer(RpcBlockDeviceServer &&) = delete;
    auto operator=(RpcBlockDeviceServer &&)
        -> RpcBlockDeviceServer & = delete;

    ~RpcBlockDeviceServer() {
        const auto status = RpcServerUnregisterIf(
            DeviceFsBlockDevice_v1_0_s_ifspec, nullptr, TRUE);
        if (status != RPC_S_OK) [[unlikely]] {
            __fastfail(FAST_FAIL_FATAL_APP_EXIT);
        }
        active_.reset();
    }

  private:
    RpcBlockDeviceServer(
        std::wstring endpoint,
        const DWORD client_process_identifier,
        Devices devices)
        : devices_{std::move(devices)},
          client_process_identifier_{client_process_identifier} {
        if (active_) [[unlikely]] {
            __fastfail(FAST_FAIL_INVALID_ARG);
        }
        active_.emplace(*this);

        auto protocol_sequence = std::filesystem::path{
            devicefs::rpc::kProtocolSequence}.wstring();
        const auto protocol_status = RpcServerUseProtseqEpW(
            protocol_sequence.data(),
            RPC_C_PROTSEQ_MAX_REQS_DEFAULT,
            endpoint.data(), nullptr);
        if (protocol_status != RPC_S_OK) {
            active_.reset();
            WinError("could not create RPC block-device endpoint '{}' with protocol '{}'",
                std::wstring_view{endpoint},
                std::wstring_view{protocol_sequence},
                ExplicitWin32Error{
                    std::bit_cast<DWORD>(protocol_status)});
        }

        const auto registration_status = RpcServerRegisterIfEx(
            DeviceFsBlockDevice_v1_0_s_ifspec,
            nullptr,
            std::addressof(manager_routines_),
            RPC_IF_AUTOLISTEN,
            RPC_C_LISTEN_MAX_CALLS_DEFAULT,
            nullptr);
        if (registration_status != RPC_S_OK) {
            active_.reset();
            WinError("could not register the RPC block-device interface",
                ExplicitWin32Error{
                    std::bit_cast<DWORD>(registration_status)});
        }
    }

    [[nodiscard]] static auto ClientAuthorized(
        const handle_t binding) noexcept {
        auto attributes = RPC_CALL_ATTRIBUTES_V2_W{
            .Version = 2,
            .Flags = RPC_QUERY_CLIENT_PID,
        };
        if (RpcServerInqCallAttributesW(
                binding,
                std::addressof(attributes)) != RPC_S_OK) {
            return false;
        }

        return HandleToULong(attributes.ClientPID) ==
            active_->get().client_process_identifier_;
    }

    [[nodiscard]] auto FindDevice(
        const std::basic_string_view<unsigned char> symbol) const noexcept
        -> const DeviceType * {
        for (const auto &[candidate, device] : devices_) {
            if (std::basic_string_view<unsigned char>{candidate} == symbol) {
                return std::addressof(device);
            }
        }
        return nullptr;
    }

    [[gsl::suppress("26429",
        justification:
            "The generated RPC callback signature requires a pointer. The "
            "`_Out_` annotation reflects that `length` cannot be null.")]]
    static auto GetLength(const handle_t binding,
        _In_z_ const unsigned char *const symbol,
        _Out_ std::uint64_t *const length) noexcept -> NTSTATUS {
        *length = 0;
        if (!ClientAuthorized(binding)) {
            return STATUS_ACCESS_DENIED;
        }
        const auto device = active_->get().FindDevice(symbol);
        if (device == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        *length = device->length;
        return STATUS_SUCCESS;
    }

    [[gsl::suppress("26429",
        justification:
            "The generated RPC callback signature requires pointers. The "
            "output annotations reflect that `transferred` and `buffer` "
            "cannot be null.")]]
    static auto Read(const handle_t binding,
        _In_z_ const unsigned char *const symbol,
        const std::uint64_t offset, const ULONG wanted,
        _Out_ ULONG *const transferred,
        _Out_writes_bytes_to_(wanted, *transferred)
            BYTE *const buffer) noexcept -> NTSTATUS {
        *transferred = 0;
        if (!ClientAuthorized(binding)) {
            return STATUS_ACCESS_DENIED;
        }
        const auto device = active_->get().FindDevice(symbol);
        if (device == nullptr) {
            return STATUS_INVALID_PARAMETER;
        }
        return device->Read(
            buffer, offset, wanted, *transferred);
    }

    inline static auto active_ = std::optional<
        std::reference_wrapper<RpcBlockDeviceServer>>{};
    inline static auto manager_routines_ =
        DeviceFsBlockDevice_v1_0_epv_t{
            .GetLength = GetLength,
            .Read = Read,
        };

    Devices devices_;
    const DWORD client_process_identifier_;
};

} // namespace devicefs
