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

The header now imports `std`. Upstream obtains its standard-library
declarations through a precompiled header, but importing
`CodepointWidthDetector.hpp` as a C++ header unit compiles the header separately
from its importer. Importing `std` gives the header those declarations directly.

Importing `std` is the only change to the upstream files. The implementation
and license remain identical to the recorded upstream blobs.

The component's [CMake configuration](../../CMakeLists.txt) builds the detector
as a separate library with the project's analysis and compiler settings.
C26494 is disabled through the compatibility header textually included by the
vendored source: the rule reports four declarations without initializers, but
every value is assigned before reading. The reason is explained beside the
pragma. First-party code retains the rule. The detector includes `precomp.h`
for its integer typedefs and WIL's `LOG_CAUGHT_EXCEPTION`. The
[replacement header](../../compat/precomp.h) supplies those dependencies so
the copied source can compile without the rest of Windows Terminal.
