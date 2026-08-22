// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <winternl.h>

// The supported Windows SDK does not declare PNTSTATUS where WinFsp expects it.
using PNTSTATUS = NTSTATUS *;
#include <winfsp/winfsp.h>
