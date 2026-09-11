# simdutf

`simdutf.h` and `simdutf.cpp` come from the simdutf 9.1.2
[single-header release archive](https://github.com/simdutf/simdutf/releases/download/v9.1.2/singleheader.zip).
The header is under `include/simdutf/`, and `simdutf.cpp` includes it as
`simdutf/simdutf.h`.

The local adaptation imports `std` for standard-library declarations and uses
`wchar_t` as the UTF-16 code-unit type when `wchar_t` occupies two bytes, or as
the UTF-32 code-unit type when it occupies four bytes. This allows native wide
strings to be read and written directly by the converter. The code-unit types
are defined in `compat/utf_code_units.h` and shared with the width
detector. The header defines
the character-type substitutions for both declarations and implementation;
`transcoding.ixx` undefines them immediately after including the header.

Explicit C++ linkage keeps the library declarations attached to the global
module when the header is included by `transcoding.ixx`. Integer and library
feature macros come from `<cstdint>` and `<version>`; the implementation also
uses `<climits>` for its eight-bit-byte assertion. The amalgamated C API uses
the same character-type substitutions as the C++ implementation.

Archive SHA-256:
`631d95aaf39371505897b0916be2a60dd7c17ae16cec8bd5036d15d23bd0ac21`.

`LICENSE-MIT` and `LICENSE-APACHE` are copied from the
[same release tag](https://github.com/simdutf/simdutf/tree/v9.1.2).
