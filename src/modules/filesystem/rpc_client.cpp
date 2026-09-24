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

module devicefs.filesystem:rpc_client;

import std;
import <devicefs/windows_imports.h>;
import <devicefs/rpc_block_device.h>;
import :internal;
import <devicefs/common.h>;
import devicefs.rpc_constants;
import devicefs.stream_writer;
import devicefs.terminal.transcoding;

#pragma warning(push)
#pragma warning(disable : 5244, \
    justification : \
        "The symbols defined in the `midl_compat.h` header have `C` linkage " \
        "and therefore are not attached to the named module declared in " \
        "this file.")
#include <devicefs/midl_compat.h>
#pragma warning(pop)

using devicefs::terminal::Transcode;

namespace internal {
auto CheckNt(NTSTATUS, wil::zstring_view) -> void;
}

namespace rpc_client {

using namespace std::string_view_literals;

constexpr auto kTcpPrefix = "tcp:"sv;
constexpr auto kTcpUsername = "devicefs"sv;

[[nodiscard]] auto IsRpcDevice(const internal::Mapping &mapping) noexcept {
    return mapping.device.starts_with(internal::kRpcDevicePrefix);
}

[[nodiscard]] auto IsTcpDevice(const internal::Mapping &mapping) noexcept {
    return IsRpcDevice(mapping) &&
        std::string_view{mapping.device}.substr(
            internal::kRpcDevicePrefix.size()).starts_with(kTcpPrefix);
}

[[nodiscard]] auto MakeRpcBinding(const std::string_view binding) {
    auto string_binding = Transcode<std::wstring>(binding);
    auto result = wil::unique_rpc_binding{};
    const auto error = RpcBindingFromStringBindingW(
        string_binding.data(), result.put());
    if (error != RPC_S_OK) {
        WinError("could not create RPC block-device binding '{}'", binding,
            ExplicitWin32Error{std::bit_cast<DWORD>(error)});
    }
    return wil::shared_rpc_binding{std::move(result)};
}

[[nodiscard]] auto LocalRpcBinding() -> const wil::shared_rpc_binding & {
    static const auto binding = [] {
        auto endpoint = std::wstring{};
        const auto result = wil::GetEnvironmentVariableW(
            Transcode<wchar_t>(devicefs::rpc::kEndpointEnvironmentVariable).data(),
            endpoint);
        if (FAILED(result)) {
            WinError("could not obtain RPC block-device endpoint from environment variable '{}'",
                devicefs::rpc::kEndpointEnvironmentVariable,
                ExplicitHresult{result});
        }
        if (endpoint.empty()) {
            throw std::runtime_error(std::format(
                "environment variable '{}' contains an empty RPC block-device endpoint",
                devicefs::rpc::kEndpointEnvironmentVariable));
        }
        return MakeRpcBinding(std::format(
            "{}:[{}]", devicefs::rpc::kProtocolSequence,
            Transcode<std::string>(endpoint)));
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
            throw std::runtime_error{std::format(
                "invalid TCP RPC source '{}'", source)};
        }
        const auto field = *position;
        ++position;
        return std::string_view{field.begin(), field.end()};
    };
    const auto address = next();
    const auto port = next();
    if (address.empty() || port.empty() || (position != fields.end())) {
        throw std::runtime_error{std::format(
            "invalid TCP RPC source '{}'", source)};
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
        .UserLength = wil::safe_cast_failfast<ULONG>(username.size()),
        .Domain = nullptr,
        .DomainLength = 0,
        .Password = encoded_password.data(),
        .PasswordLength = wil::safe_cast_failfast<ULONG>(encoded_password.size()),
        .Flags = SEC_WINNT_AUTH_IDENTITY_ANSI,
    };
    const auto error = RpcBindingSetAuthInfoA(
        result.get(), nullptr, RPC_C_AUTHN_LEVEL_CONNECT,
        RPC_C_AUTHN_WINNT, &identity, RPC_C_AUTHZ_NONE);
    if (error != RPC_S_OK) {
        WinError("could not authenticate RPC block-device binding to '{}:{}'",
            address, port,
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
        const auto length = [binding_ = binding.get(), &stored_symbol, symbol] {
            auto rpc_length = std::uint64_t{};
            auto status = NTSTATUS{};
            const auto result = wil::invoke_rpc_result_nothrow(
                status, DeviceFsRpcClient_GetLength,
                binding_, stored_symbol.c_str(), &rpc_length);
            if (FAILED(result)) {
                WinError("could not query the length of RPC block device '{}'",
                    symbol,
                    ExplicitHresult{result});
            }
            internal::CheckNt(status, "could not query the RPC block-device length");
            return rpc_length;
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
        const auto result = wil::invoke_rpc_result_nothrow(
            status, DeviceFsRpcClient_Read,
            binding_.get(), symbol_.c_str(), offset, wanted,
            &rpc_transferred, static_cast<BYTE *>(buffer));
        if (FAILED(result)) {
            const auto win32_error =
                CompileTimeCast<DWORD>(ExplicitHresult{result});
            devicefs::WriteToStream(devicefs::stderr,
                "devicefs: RPC read failed for '{:s}' at offset 0x{:x} "
                "for {} bytes: Windows error {}\n",
                symbol_ | std::views::transform(
                    [](const unsigned char byte) noexcept {
                        return std::bit_cast<char>(byte);
                    }), offset, wanted, win32_error);
            return FspNtStatusFromWin32(win32_error);
        }
        transferred = rpc_transferred;
        if (NT_SUCCESS(status)) {
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
