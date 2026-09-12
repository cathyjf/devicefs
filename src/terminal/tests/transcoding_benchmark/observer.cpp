// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

import std;
namespace {
const void *volatile pointer_seen;
volatile std::size_t size_seen;
}
auto Observe(const void *data, const std::size_t size) noexcept -> void {
    pointer_seen = data;
    size_seen = size;
}
