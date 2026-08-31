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

#include <winsock2.h>
#include <wil/network.h>
#include <wil/safecast.h>

module devicefs.supervisor.native_backup:port_selection;

import std;
import devicefs.common;

namespace internal {

[[nodiscard]] auto SelectTcpPortCandidate() -> std::uint16_t {
    static_assert(sizeof(int) == sizeof(DWORD));
    auto data = WSADATA{};
    const auto startup = WSAStartup(MAKEWORD(2, 2), &data);
    auto cleanup = wil::network::unique_wsacleanup_call{startup == 0};
    if (startup != 0) {
        WinError("could not initialize view port selection",
            ExplicitWin32Error{std::bit_cast<DWORD>(startup)});
    }

    auto socket_handle = wil::unique_socket{
        socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (!socket_handle) {
        WinError("could not create a view port-selection socket",
            ExplicitWin32Error{
                std::bit_cast<DWORD>(WSAGetLastError())});
    }

    auto endpoint = sockaddr_in{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_addr.s_addr = htonl(INADDR_ANY);
    [[gsl::suppress("type.1",
        justification:
            "Winsock represents an IPv4 sockaddr_in through its sockaddr "
            "pointer ABI.")]]
    if (bind(socket_handle.get(),
            reinterpret_cast<const sockaddr *>(&endpoint),
            wil::safe_cast_failfast<int>(sizeof(endpoint))) == SOCKET_ERROR) {
        WinError("could not select a view TCP port candidate",
            ExplicitWin32Error{
                std::bit_cast<DWORD>(WSAGetLastError())});
    }
    auto endpoint_size = wil::safe_cast_failfast<int>(sizeof(endpoint));
    [[gsl::suppress("type.1",
        justification:
            "Winsock represents an IPv4 sockaddr_in through its sockaddr "
            "pointer ABI.")]]
    if (getsockname(socket_handle.get(),
            reinterpret_cast<sockaddr *>(&endpoint),
            &endpoint_size) == SOCKET_ERROR) {
        WinError("could not obtain the selected view TCP port candidate",
            ExplicitWin32Error{
                std::bit_cast<DWORD>(WSAGetLastError())});
    }
    return ntohs(endpoint.sin_port);
}

} // namespace internal
