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

#define SECURITY_WIN32
#include <sspi.h>

#include <devicefs/rpc_block_device.h>
#include <devicefs/midl_compat.h>
#include <devicefs/winfsp_compat.h>
#include <wil/resource.h>
#include <wil/safecast.h>
#include <wil/stl.h>
#include <wil/rpc_helpers.h>
#include <wil/win32_helpers.h>

#undef stderr
#undef stdout

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

constexpr auto kTcpPrefix = std::string_view{"tcp:"};
constexpr auto kTcpUsername = std::string_view{"devicefs"};

[[nodiscard]] auto IsRpcDevice(const internal::Mapping &mapping) noexcept {
    return mapping.device.starts_with(internal::kRpcDevicePrefix);
}

[[nodiscard]] auto IsTcpDevice(const internal::Mapping &mapping) noexcept {
    return IsRpcDevice(mapping) &&
        std::string_view{mapping.device}.substr(
            internal::kRpcDevicePrefix.size()).starts_with(kTcpPrefix);
}

[[nodiscard]] auto MakeRpcBinding(const std::string_view binding) {
    auto string_binding = std::filesystem::path{binding}.wstring();
    auto result = wil::unique_rpc_binding{};
    const auto error = RpcBindingFromStringBindingW(
        string_binding.data(), result.put());
    if (error != RPC_S_OK) {
        WinError("could not create the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(error)});
    }
    return wil::shared_rpc_binding{std::move(result)};
}

[[nodiscard]] auto LocalRpcBinding() -> const wil::shared_rpc_binding & {
    static const auto binding = [] {
        auto endpoint = std::wstring{};
        const auto error = wil::GetEnvironmentVariableW(
            std::filesystem::path{
                devicefs::rpc::kEndpointEnvironmentVariable}.c_str(),
            endpoint);
        if (FAILED(error)) {
            WinError("could not obtain the RPC block-device endpoint",
                ExplicitWin32Error::FromHresult(error));
        }
        if (endpoint.empty()) {
            throw std::runtime_error(
                "the RPC block-device endpoint is empty");
        }
        return MakeRpcBinding(std::format(
            "{}:[{}]", devicefs::rpc::kProtocolSequence,
            std::filesystem::path{endpoint}.string()));
    }();
    return binding;
}

[[nodiscard]] auto MakeTcpRpcBinding(
    const internal::Mapping &mapping,
    const std::string_view password) {
    const auto source = std::string_view{mapping.device}.substr(
        internal::kRpcDevicePrefix.size() + kTcpPrefix.size());
    auto fields = source | std::views::split(':');
    auto position = fields.begin();
    const auto next = [&]() {
        if (position == fields.end()) {
            throw std::runtime_error{"invalid TCP RPC source"};
        }
        const auto field = *position;
        ++position;
        return std::string_view{field.begin(), field.end()};
    };
    const auto address = next();
    const auto port = next();
    if (address.empty() || port.empty() || (position != fields.end())) {
        throw std::runtime_error{"invalid TCP RPC source"};
    }

    auto result = MakeRpcBinding(
        std::format("ncacn_ip_tcp:{}[{}]", address, port));
    auto username = std::basic_string<unsigned char>{
        kTcpUsername.begin(), kTcpUsername.end()};
    auto encoded_password = std::basic_string<unsigned char,
        std::char_traits<unsigned char>,
        wil::secure_allocator<unsigned char>>{
            password.begin(), password.end()};
    auto identity = SEC_WINNT_AUTH_IDENTITY_A{
        .User = username.data(),
        .UserLength = wil::safe_cast<ULONG>(username.size()),
        .Domain = nullptr,
        .DomainLength = 0,
        .Password = encoded_password.data(),
        .PasswordLength = wil::safe_cast<ULONG>(encoded_password.size()),
        .Flags = SEC_WINNT_AUTH_IDENTITY_ANSI,
    };
    const auto error = RpcBindingSetAuthInfoA(
        result.get(), nullptr, RPC_C_AUTHN_LEVEL_CONNECT,
        RPC_C_AUTHN_WINNT, &identity, RPC_C_AUTHZ_NONE);
    if (error != RPC_S_OK) {
        WinError("could not authenticate the RPC block-device binding",
            ExplicitWin32Error{std::bit_cast<DWORD>(error)});
    }
    return result;
}

struct RPCBlockDevice {
    const std::uint64_t length;

    [[nodiscard]] static auto FromSymbol(
        const wil::shared_rpc_binding &binding,
        const std::string_view symbol) {
        auto stored_symbol = std::basic_string<unsigned char>{
            symbol.begin(), symbol.end()};
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
            devicefs::WriteToStream(devicefs::stderr,
                "devicefs: RPC read failed for '{:s}': Windows error {}\n",
                symbol_ | std::views::transform(
                    [](const unsigned char byte) noexcept {
                        return std::bit_cast<char>(byte);
                    }), win32_error);
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
        std::basic_string<unsigned char> symbol,
        wil::shared_rpc_binding binding) noexcept
        : length(length), symbol_(std::move(symbol)),
          binding_(std::move(binding)) {}

    std::basic_string<unsigned char> symbol_;
    wil::shared_rpc_binding binding_;
};

} // namespace rpc_client
