// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

module devicefs.noinline_invoker;

namespace devicefs::noinline_invoker_detail {

[[gsl::suppress("26440", justification:
    "This function cannot be declared `noexcept` because the type-erased "
    "callable object, when invoked, may throw exceptions.")]]
auto InvokeOutOfLineErased(
    const volatile void *const context, void *const output,
    auto (&invoke)(const volatile void *, void *) -> void) -> void {
    invoke(context, output);
}

} // namespace devicefs::noinline_invoker_detail
