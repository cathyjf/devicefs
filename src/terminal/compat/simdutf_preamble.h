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

// Our adaptation gives simdutf the code-unit types shared by the transcoder and
// width detector. The macros cover both upstream files and are undefined at the
// end of the generated header.
#define char16_t devicefs::terminal::utf16_code_unit
#define char32_t devicefs::terminal::utf32_code_unit

#ifdef _MSC_VER
    #pragma warning(push)
    // ASCII expansion writes one UTF-16 or UTF-32 code unit for each input byte.
    // In `store_ascii_as_utf16` and `store_ascii_as_utf32`, `sizeof(simd8<T>)`
    // therefore gives the number of destination elements produced by one chunk.
    // Pointer arithmetic already scales that count by the destination element size.
    // C6305 mistakes this input-byte count for a destination-byte count.
    //
    // `is_eight_byte` returns unconditionally for one-byte character types, leaving
    // its wider-character comparison unreachable in those instantiations. MSVC
    // reports C4702 for that comparison during link-time optimization.
    #pragma warning(disable : 6305 4702)

    // These warnings are raised when the simdutf code is imported as a header unit,
    // because doing so causes MSVC++ to apply this project's strict first-party
    // warning policy to the third-party code. The third-party code was not
    // designed to comply with our strict warning policy.
    #pragma warning(disable : 4310 5260 4505 4324)
#elifdef __clang__
    // The transcoder uses simdutf's Unicode conversions. Its unused Base64 decoder
    // fails to find `load_block` when Clang 23 compiles the combined implementation
    // in a global module fragment. simdutf's feature switch omits that decoder.
    // GCC does not produce require disabling this feature.
    #define SIMDUTF_FEATURE_BASE64 0
#else
    // In the Clang and GCC builds, we textually include simdutf in our module
    // interface. With its constant-evaluation support enabled, the span overloads
    // expose references to scalar helpers with internal linkage. Those references
    // are invalid C++ in the Clang and GCC builds, although only GCC objects to
    // them. Beacuse the simdutf code is imported as a header unit on MSVC++, the
    // MSVC++ build does not involve any invalid C++.
    //
    // Our transcoder currently needs only runtime conversions and length
    // calculations. `ConvertText` calls
    // its `convert_utf*_to_utf*_with_errors` and `validate_utf*_with_errors`
    // overloads from non-constexpr functions. `TranscodedCapacity` calls
    // `utf8_length_from_utf16`, `utf8_length_from_utf32`, and
    // `utf16_length_from_utf32`; its current callers are the non-constexpr
    // `TranscodedText` constructor and `Transcode`.
    //
    // Disabling simdutf's constant evaluation feature removes the potentially-invalid
    // code that GCC finds objectionable without removing functionality that our
    // current callers use.
    #define SIMDUTF_CPLUSPLUS23 0
#endif
