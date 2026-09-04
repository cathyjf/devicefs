# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

param(
    [string] $ServerStub,
    [string] $TargetArchitecture
)

$ErrorActionPreference = 'Stop'

if ($TargetArchitecture -ne 'ARM64') {
    return
}

# DeviceFs supplies its RPC manager routines through an explicit entry-point
# vector, as required when using `/no_default_epv`, rather than defining global
# `GetLength` or `Read` implementations. This registration requirement is
# documented at <https://learn.microsoft.com/en-us/windows/win32/midl/-no-default-epv>.
#
# However, MIDL 8.01.0628 with `/Oicf` still references those functions in its
# generated server routine table despite `/no_default_epv`, causing LNK2001.
# Replacing these unused entries with null pointers removes the references
# while preserving the table and surrounding metadata. ARM64 needs this
# workaround because it cannot use `/Os`; x64 uses `/Os` instead.
[IO.File]::WriteAllText(
    $ServerStub,
    [IO.File]::ReadAllText($ServerStub).
        Replace('(SERVER_ROUTINE)GetLength', '0').
        Replace('(SERVER_ROUTINE)Read', '0'))
