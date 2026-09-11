# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The generated header combines upstream's declarations and implementation for
# compilation in `transcoding.ixx`. Standard-library declarations come from
# `import std` in our preamble. Removing the corresponding textual includes
# also prevents our character-type macros from changing those declarations.
file(READ "${SOURCE_DIRECTORY}/simdutf.h" header)
file(READ "${SOURCE_DIRECTORY}/simdutf.cpp" implementation)

# The C API supplies character-type definitions for C consumers. Our C++
# compilation uses the preamble's definitions throughout both upstream files.
# A changed fallback needs review because it could override those definitions.
set(character_fallback [=[#ifdef __has_include
  #if __has_include(<uchar.h>)
    #include <uchar.h>
  #else // __has_include(<uchar.h>)
    #define char16_t uint16_t
    #define char32_t uint32_t
  #endif // __has_include(<uchar.h>)
#else    // __has_include(<uchar.h>)
  #define char16_t uint16_t
  #define char32_t uint32_t
#endif // __has_include]=])
string(FIND "${implementation}" "${character_fallback}" fallback_position)
if(fallback_position EQUAL -1)
    message(FATAL_ERROR "simdutf's C character-type fallback changed; review GenerateSimdutf.cmake")
endif()
string(REPLACE "${character_fallback}" "" implementation "${implementation}")

string(FIND "${implementation}" "#include \"simdutf.h\"" include_position)
if(include_position EQUAL -1)
    message(FATAL_ERROR "simdutf.cpp's header inclusion changed; review GenerateSimdutf.cmake")
endif()
string(REPLACE "#include \"simdutf.h\"" "" implementation "${implementation}")
set(source "${header}\n${implementation}")

# Only standard headers are removed; platform and SIMD intrinsic headers keep
# their upstream conditions. Empty conditional blocks can remain unchanged.
set(standard_headers
    array atomic cfloat climits concepts cstddef cstdint cstdio cstdlib cstring
    initializer_list iostream limits span string_view text_encoding tuple
    type_traits utility vector version "iso646\\.h" "stdbool\\.h" "stddef\\.h" "stdint\\.h"
)
list(JOIN standard_headers "|" standard_header_pattern)
string(REGEX REPLACE
    "(^|\n)[ \t]*#[ \t]*include[ \t]*<(${standard_header_pattern})>[^\n]*"
    "\\1" source "${source}")

file(WRITE "${OUTPUT}" "#pragma once
#include \"simdutf_preamble.h\"
${source}
#undef char16_t
#undef char32_t
#ifdef _MSC_VER
#pragma warning(pop)
#endif
")
