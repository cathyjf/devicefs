The width detector and its header originate in
[Windows Terminal](https://github.com/microsoft/terminal/tree/eacba3e348be877ce98061e5811a6af91fb66cc8),
revision `eacba3e348be877ce98061e5811a6af91fb66cc8`. The accompanying `LICENSE`
is from the same revision. The files retain their upstream paths and CRLF line
endings. The following hashes identify the pristine upstream files before the
adaptations described below. The two source blobs are also stored in the local
Git object database for comparison without putting the dependency into history.

| File | Upstream Git blob |
|---|---|
| `src/types/CodepointWidthDetector.cpp` | `c0676ffc8c8161053ecc9cc260a4430ad7427877` |
| `src/types/inc/CodepointWidthDetector.hpp` | `3b18ce205714768266fd87bc3a7041d86b191a58` |
| `LICENSE` | `017b9885a46ad4c7f4367ad2949fb66fb56f977f` |

The copied detector uses the shared `devicefs::terminal::utf16_code_unit` type
and `std::basic_string_view<utf16_code_unit>` for its UTF-16 input. The type is
`wchar_t` when `wchar_t` has the same size as `char16_t`, and otherwise is
`char16_t`. simdutf uses the same definition, so the detector can read its
converted buffer directly. Upstream uses `wchar_t` unconditionally, which
would interpret UTF-32 as UTF-16 on platforms with 32-bit `wchar_t`. The
Unicode tables and segmentation algorithms retain their upstream implementation.

The header imports `std` on both platforms. Upstream obtains its
standard-library declarations through a precompiled header. The MSVC build
imports `CodepointWidthDetector.hpp` as a header unit, while the Clang build
includes the detector header textually. The compatibility header supplies
Clang with the `<cwchar>` declarations needed by that textual inclusion.

The component's [CMake configuration](../../CMakeLists.txt) builds the detector
as a separate library with the project's analysis and compiler settings.
On MSVC, C26494 is disabled through the compatibility header textually included
by the vendored source: the rule reports four declarations without initializers,
but every value is assigned before reading. The reason is explained beside the
pragma. First-party code retains the rule. The detector includes `terminal_compat.h`
for its integer typedefs and for other compatibility shims. That header also
defines `LOG_CAUGHT_EXCEPTION()` as an empty macro: caught fallback-callback
exceptions use the detector's existing recovery behavior without WIL logging.
