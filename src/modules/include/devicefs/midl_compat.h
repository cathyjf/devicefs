// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

extern "C" {

_Use_decl_annotations_
void *__RPC_USER MIDL_user_allocate(const size_t size) {
    return HeapAlloc(GetProcessHeap(), 0, size);
}

_Use_decl_annotations_
void __RPC_USER MIDL_user_free(void *const memory) {
    HeapFree(GetProcessHeap(), 0, memory);
}

}
