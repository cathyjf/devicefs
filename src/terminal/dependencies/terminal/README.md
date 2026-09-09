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

The copied detector uses `char16_t` and `std::u16string_view` for its UTF-16
input. Upstream uses `wchar_t`, which has 16 bits on Windows but 32 bits on the
supported Unix platforms. Explicit UTF-16 types let the same decoder process
surrogate pairs correctly on every platform. The Unicode tables and
segmentation algorithms retain their upstream implementation.

The header imports `std` when built with MSVC. Upstream obtains its
standard-library declarations through a precompiled header, but the MSVC build
imports `CodepointWidthDetector.hpp` as a header unit, which is compiled
separately from its importer. The Clang build includes the detector header
textually, and the header includes its standard-library dependencies there.
Clang's `-fms-extensions` option enables the source's `msvc::forceinline` and
`__declspec(noinline)` annotations. For Clang, the compatibility header maps
`__assume` to `__builtin_assume`, preserving the decoder's optimization assumptions.

The component's [CMake configuration](../../CMakeLists.txt) builds the detector
as a separate library with the project's analysis and compiler settings.
On MSVC, C26494 is disabled through the compatibility header textually included
by the vendored source: the rule reports four declarations without initializers,
but every value is assigned before reading. The reason is explained beside the
pragma. First-party code retains the rule. The detector includes `precomp.h`
for its integer typedefs and, on Windows, WIL's `LOG_CAUGHT_EXCEPTION`. The
[replacement header](../../compat/precomp.h) supplies those dependencies so
the copied source can compile without the rest of Windows Terminal.
