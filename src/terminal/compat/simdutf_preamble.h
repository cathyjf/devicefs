// SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// The generated simdutf header uses `import std` for library declarations and
// these headers for macros. GCC reports redefinitions when the textual headers
// follow the import, so they precede `utf_code_units.h`, which imports `std`.
// https://gcc.gnu.org/onlinedocs/gcc/C_002b_002b-Modules.html
#include <climits>
#include <cstdint>
#include <version>
#include "utf_code_units.h"

using std::size_t;
using std::ptrdiff_t;
using std::memcpy;
using std::memmove;
using std::memset;
using std::memcmp;
using std::strlen;
using std::printf;
using std::getenv;

// The transcoder uses simdutf's Unicode conversions. Its unused Base64 decoder
// fails to find `load_block` when Clang 23 compiles the combined implementation
// in a global module fragment. simdutf's feature switch omits that decoder.
#define SIMDUTF_FEATURE_BASE64 0

// Our adaptation gives simdutf the code-unit types shared by the transcoder and
// width detector. The macros cover both upstream files and are undefined at the
// end of the generated header.
#define char16_t devicefs::terminal::utf16_code_unit
#define char32_t devicefs::terminal::utf32_code_unit

#ifdef _MSC_VER
// ASCII expansion writes one UTF-16 or UTF-32 code unit for each input byte.
// In `store_ascii_as_utf16` and `store_ascii_as_utf32`, `sizeof(simd8<T>)`
// therefore gives the number of destination elements produced by one chunk.
// Pointer arithmetic already scales that count by the destination element size.
// C6305 mistakes this input-byte count for a destination-byte count.
//
// `is_eight_byte` returns unconditionally for one-byte character types, leaving
// its wider-character comparison unreachable in those instantiations. MSVC
// reports C4702 for that comparison during link-time optimization.
#pragma warning(push)
#pragma warning(disable : 6305 4702)
#endif
