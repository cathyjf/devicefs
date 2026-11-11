// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <print>
#include <tuple>

extern "C" {
#include <core/ntstatus.h>
}

// Samba exports this function but does not install its declaration. The hook
// changes only the flags argument, so the state can remain opaque.
// https://github.com/samba-team/samba/blob/samba-4.24.6/auth/ntlmssp/ntlmssp_util.c
struct ntlmssp_state;

extern "C" auto ntlmssp_handle_neg_flags(
    ntlmssp_state *const state,
    const std::uint32_t flags,
    const char *const name) -> NTSTATUS {
    static const auto original = [] static noexcept {
        std::ignore = dlerror();
        auto *const address = dlsym(RTLD_NEXT, "ntlmssp_handle_neg_flags");
        if (const auto *const error = dlerror(); error != nullptr) {
            std::println(stderr, "rpcd_devicefs: failed to resolve "
                "`ntlmssp_handle_neg_flags`: {}",
                error);
            std::exit(1);
        }
        return reinterpret_cast<decltype(&ntlmssp_handle_neg_flags)>(address);
    }();

    // This worker uses authenticated RPC at the `CONNECT` level for the local
    // Windows/WSL connection. Packet signing adds overhead without protecting
    // against a privileged user who could already alter the source data.
    //
    // Windows update KB5132061 makes clients offer NTLM message signing even
    // when the application does not request it. Accepting that offer breaks
    // this `CONNECT`-level connection. Clearing `NTLMSSP_NEGOTIATE_SIGN` here
    // makes Samba decline signing in its NTLM challenge message, allowing
    // Windows to authenticate the connection without requiring packet integrity.
    //
    // Samba generates its challenge message from the negotiated flags. It
    // retains the client's original negotiate message and the challenge
    // message it actually sends for authentication checks.
    // https://support.microsoft.com/en-us/servicing/os/windows/docs/2026/11/kb5135557-ntlm-signing-security-improvements
    constexpr auto kNtlmsspNegotiateSign = 0x00000010u;
    return original(state, flags & ~kNtlmsspNegotiateSign, name);
}
