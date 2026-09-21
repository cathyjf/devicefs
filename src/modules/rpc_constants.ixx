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

export module devicefs.rpc_constants;

export namespace devicefs::rpc {

// This file uses string literals rather than string view literals to avoid
// introducing a dependency on `std.ixx` too early in the project's
// compile-time dependency graph.

constexpr auto kEndpointEnvironmentVariable = "DEVICEFS_RPC_ENDPOINT";
constexpr auto kProtocolSequence = "ncalrpc";

} // namespace devicefs::rpc
