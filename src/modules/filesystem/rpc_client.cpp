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
#include <windows.h>

#include <devicefs/rpc_block_device.h>
#include <devicefs/midl_compat.h>
#include <devicefs/winfsp_compat.h>
#include <wil/resource.h>
#include <wil/stl.h>
#include <wil/rpc_helpers.h>

module devicefs.filesystem:rpc_client;

import std;
import :internal;
import devicefs.common;
import devicefs.rpc_constants;
import devicefs.stream_writer;

namespace internal {
auto CheckNt(NTSTATUS, wil::zstring_view) -> void;
}

namespace rpc_client {

[[nodiscard]] auto IsRpcDevice(const internal::Mapping &mapping) noexcept {
    return mapping.device.starts_with(internal::kRpcDevicePrefix);
}

[[nodiscard]] auto MakeRpcBinding(const wil::zwstring_view endpoint) {
    auto endpoint_text = std::wstring{endpoint};
    auto protocol_sequence = std::wstring{
        devicefs::rpc::kProtocolSequence};
    auto string_binding = wil::unique_rpc_wstr{};
    const auto compose_error = RpcStringBindingComposeW(
        nullptr, protocol_sequence.data(), nullptr,
        endpoint_text.data(), nullptr, string_binding.put());
    if (compose_error != RPC_S_OK) {
        WinError("could not compose the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(compose_error)});
    }

    auto result = wil::unique_rpc_binding{};
    const auto error = RpcBindingFromStringBindingW(
        string_binding.get(), result.put());
    if (error != RPC_S_OK) {
        WinError("could not create the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(error)});
    }
    return wil::shared_rpc_binding{std::move(result)};
}

struct RPCBlockDevice {
    const std::uint64_t length;

    [[nodiscard]] static auto FromSymbol(
        const wil::shared_rpc_binding &binding,
        const std::wstring_view symbol) {
        auto stored_symbol = std::wstring{symbol};
        const auto length = [&] {
            auto result = std::uint64_t{};
            auto status = NTSTATUS{};
            const auto error = wil::invoke_rpc_result_nothrow(
                status, DeviceFsRpcClient_GetLength,
                binding.get(), stored_symbol.c_str(), &result);
            if (FAILED(error)) {
                WinError("could not query the RPC block-device length",
                    ExplicitWin32Error::FromHresult(error));
            }
            internal::CheckNt(status, "could not query the RPC block-device length");
            return result;
        }();
        return RPCBlockDevice{
            length, std::move(stored_symbol), binding};
    }

    template <typename... Observers>
    _Success_(return >= 0)
    auto Read(
        _Out_writes_bytes_to_(wanted, transferred) void *const buffer,
        _In_range_(0, length - 1) const std::uint64_t offset,
        _In_range_(1, length - offset) const ULONG wanted,
        _Pre_equal_to_(0) ULONG &transferred,
        Observers &...observers) const noexcept -> NTSTATUS {
        auto status = NTSTATUS{};
        auto rpc_transferred = ULONG{};
        (observers.BeginSourceRead(), ...);
        const auto error = wil::invoke_rpc_result_nothrow(
            status, DeviceFsRpcClient_Read,
            binding_.get(), symbol_.c_str(), offset, wanted,
            &rpc_transferred, static_cast<BYTE *>(buffer));
        if (FAILED(error)) {
            const auto win32_error =
                ExplicitWin32Error::FromHresult(error).value;
            devicefs::WriteToStream(std::cerr,
                L"devicefs: RPC read failed for '{}': Windows error {}\n",
                symbol_, win32_error);
            return FspNtStatusFromWin32(win32_error);
        }
        transferred = rpc_transferred;
        if (status >= 0) {
            (observers.FinishSourceRead(rpc_transferred), ...);
        }
        return status;
    }

  private:
    RPCBlockDevice(
        const std::uint64_t length,
        std::wstring symbol,
        wil::shared_rpc_binding binding) noexcept
        : length(length), symbol_(std::move(symbol)),
          binding_(std::move(binding)) {}

    std::wstring symbol_;
    wil::shared_rpc_binding binding_;
};

} // namespace rpc_client
