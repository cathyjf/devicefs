# Transcoding capacity benchmark

This benchmark compares output-sizing policies for DeviceFs's simdutf wrapper.
It measures complete conversions, including destruction of the result, and
separately measures counting, allocation/free, and conversion into preallocated
storage. It checks that every policy produces the same text before timing it.

## Build and run

From `src/terminal`, enable the optional target with the existing configure
preset. For native x64 Windows:

```powershell
cmake --preset windows-x64 -DDEVICEFS_TERMINAL_BUILD_BENCHMARKS=ON
cmake --build --preset windows-x64-release
cmake "-DEXECUTABLE=../../build/terminal/windows-x64/Release/devicefs-transcoding-benchmark.exe" -DOUTPUT=sizing-msvc-x64-1.csv -DSEED=20260912 -P tests/transcoding_benchmark/Run.cmake
cmake "-DEXECUTABLE=../../build/terminal/windows-x64/Release/devicefs-transcoding-benchmark.exe" -DOUTPUT=sizing-msvc-x64-2.csv -DSEED=391472 -P tests/transcoding_benchmark/Run.cmake
```

PowerShell needs the quotes around `"-DEXECUTABLE=../../..."`: without them,
it passes `-DEXECUTABLE=` and the relative path as separate arguments.

For ARM64 Windows, substitute `windows-arm64` in the preset names and output
path. For Clang/libc++ on GNU/Linux or macOS:

```sh
cmake --preset unix -DDEVICEFS_TERMINAL_BUILD_BENCHMARKS=ON
cmake --build --preset unix
cmake -DEXECUTABLE=../../build/terminal/unix/Release/devicefs-transcoding-benchmark -DOUTPUT=sizing-clang-1.csv -DSEED=20260912 -P tests/transcoding_benchmark/Run.cmake
cmake -DEXECUTABLE=../../build/terminal/unix/Release/devicefs-transcoding-benchmark -DOUTPUT=sizing-clang-2.csv -DSEED=391472 -P tests/transcoding_benchmark/Run.cmake
```

The `unix-gcc` preset selects GCC/libstdc++ instead. The CSV identifies the
compiler/version, target architecture, build configuration, selected simdutf
implementation, processor display name, and shuffle seed. Run on a machine that
executes the target architecture natively and keep other substantial workloads
idle during timing.

`Run.cmake` supplies the processor's display name for the CSV's `processor`
column. The name is queried on the machine executing the benchmark, independent
of where it was built.
If automatic identification returns no name, it records `unavailable`;
`-DPROCESSOR=...` supplies the name explicitly. `-DRETRY_ONLY=ON` restricts a
run to UTF-16 to UTF-8.

`--verify-only` checks every fixture without timing. `--retry-only` restricts
the experiment to UTF-16 to UTF-8, the direction with a bounded converter.
`--help` lists the options. A full timing run usually takes tens of seconds.
The executable returns a nonzero status if a policy produces different text
or another error prevents completion.

## Policies

| CSV operation | Work measured |
|---|---|
| `exact` | Current `TranscodedText`: count the output, select inline or heap storage, then convert. |
| `worst` | The same owner, selecting capacity from the worst-case expansion. |
| `hybrid` | Count only when that might avoid a heap allocation. |
| `retry_exact` | Attempt conversion into the inline buffer; on insufficient space, count, allocate, and reconvert. |
| `retry_worst` | The same attempt, followed by worst-case allocation and reconversion when necessary. |
| `string_exact` / `string_worst` | Return a `basic_string`, including its capacity initialization and destruction. |
| `count` | Calculate output length. |
| `alloc_exact` / `alloc_worst` | Allocate and free uninitialized storage of the corresponding capacity. |
| `convert_only` | Convert into preallocated storage. |
| `control` | Pass the input to the benchmark's observer without converting it. |

The inline buffer holds 255 output code units plus a terminator. Worst-case
capacity is three bytes per UTF-16 unit for UTF-8, four bytes per UTF-32 unit
for UTF-8, and two UTF-16 units per UTF-32 unit. These three expanding directions
are the ones for which the current wrapper counts output. UTF-8 input already
uses its length as an output bound and is outside this comparison.

The hybrid policy uses the bound immediately when it fits inline or when the
input has at least 256 code units. In the latter case, all three conversions
necessarily require heap storage. Otherwise it counts to determine whether
the actual result fits inline.

### What the retry comparison means

The ordinary converter requires sufficient storage and does not detect an
undersized output buffer. The retry policies therefore use simdutf's bounded
UTF-16-to-UTF-8 converter. That function repeatedly submits smaller chunks to
the ordinary converter and finishes with a scalar loop. It can consequently
be slower even when the complete result fits inline. The measurement compares
these different conversion algorithms as well as the sizing policies.

The bounded API returns only a written-byte count and discards whether it
finished. The benchmark's generated copy retains that internal completion
status. It maps an invalid SIMD chunk to a generic error; all timed inputs
are valid Unicode. The conversion and chunking algorithm is otherwise unchanged.

## Interpreting the CSV

The fixtures combine six text mixes—ASCII, mostly ASCII, accented Latin, CJK,
supplementary emoji, and mixed text—with 25 lengths from empty to 1,048,576 code
points. Extra lengths cover the inline-buffer boundaries. The complete profile
has 450 conversion cases and 4,800 measurement rows, including retry operations
only for UTF-16 to UTF-8.

`input_units`, `output_units`, and `worst_units` count code units in their
respective encodings. `iterations` is the repetition count per timed sample.
Each operation takes seven samples; `median_ns` is the middle sample,
`p14_ns` the second, and `p86_ns` the sixth after sorting. All times are per
operation in nanoseconds. Sample batches aim for 0.75 ms, following calibration.
Case order and operation order are shuffled using the supplied seed.

Inputs and comparison results are prepared outside timing. Every result escapes
to an observer compiled without LTO, preventing the optimizer from removing
conversions and allocation/free pairs. The input address is reloaded through a
volatile pointer on each iteration so counting cannot be hoisted out of a loop.

Allocations are repeatedly reused by a warm allocator. The allocation-only
measurements do not touch the allocated storage; the complete conversions do
write their output. These measurements do not reproduce memory pressure or
the cost of first touching fresh pages.

Compare policies within each build. Comparing absolute times between MSVC and
Clang also changes the generated code, standard library, allocator, and build
settings; it does not isolate an operating-system effect. Keep the raw CSV
files from multiple seeds so unusually slow runs remain visible.

## Experimental source generation

`Generate.cmake` creates a separately named module from the production
`transcoding.ixx`, inserting the capacity policies and retry attempt. This keeps
the production owner as the source of its buffer lifetime and conversion code
without adding experimental options to its public interface. Generated files
live under the build directory and are linked only into the benchmark.
The generator also prepares the bounded-converter experiment described above.
