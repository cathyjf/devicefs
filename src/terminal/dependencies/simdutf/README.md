# simdutf

`simdutf.h` and `simdutf.cpp` come from the simdutf 9.1.2
[single-header release archive](https://github.com/simdutf/simdutf/releases/download/v9.1.2/singleheader.zip).
Both files are preserved unchanged in their original layout.

The build runs [`GenerateSimdutf.cmake`](../../cmake/GenerateSimdutf.cmake) to
combine the two files into `generated/simdutf/combined.h` under the build
directory. The script removes standard-library includes and the C API's
character-type fallback. Platform and SIMD intrinsic includes remain intact.
The fallback and the implementation's header inclusion are checked before
replacement so changes to those upstream constructs produce a build error
requesting review of the generator.

Our [`preamble`](../../compat/simdutf_preamble.h) imports `std` for
standard-library declarations and uses `wchar_t` as the UTF-16 code-unit type
when `wchar_t` occupies two bytes, or as the UTF-32 code-unit type when it
occupies four bytes. This allows native wide
strings to be read and written directly by the converter. The code-unit types
are defined in `compat/utf_code_units.h` and shared with the width
detector. The preamble defines the character-type substitutions for both
declarations and implementation; the generated header undefines them afterward.

`transcoding.ixx` includes the preamble before its module declaration because
an import after that declaration cannot come from a header inclusion. It then
includes the combined header within the module. This compiles simdutf's
implementation once as part of the wrapper, with no separate simdutf target.
The preamble's include guard prevents the combined header from repeating the
standard-library import. On Clang 23, placing the implementation in the global
module fragment instead caused template instantiations to fail to find
`load_block` in the Base64 decoder.

Integer and library feature macros come from `<cstdint>` and `<version>`.
These headers precede the standard-library import because GCC rejects their
declarations when they follow the import. The preamble also uses
`<climits>` for its eight-bit-byte assertion. The amalgamated C API uses the
same character-type substitutions as the C++ implementation.

Archive SHA-256:
`631d95aaf39371505897b0916be2a60dd7c17ae16cec8bd5036d15d23bd0ac21`.

`LICENSE-MIT` and `LICENSE-APACHE` are copied from the
[same release tag](https://github.com/simdutf/simdutf/tree/v9.1.2).
