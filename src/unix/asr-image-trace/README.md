# ASR write-order experiment

This test checks whether `asr` writes an APFS snapshot to its destination from
beginning to end. It records read and write offsets to help determine whether
the restored image could be streamed to PBS.

The macOS test uses a virtual disk backed by a file on a macFUSE filesystem. It
can restore a snapshot from an automatically created test volume or use a
snapshot you supply.

A synthetic producer checks the analysis with known sequential and out-of-order
write patterns. It runs under WSL without FUSE and can also exercise the macFUSE
filesystem on a Mac.

## Build and test

Use the `gcc` preset, which selects the existing Unix GCC toolchain and the
same `import std;` settings as the terminal component.

From this directory, in Fish:

```fish
cmake --preset gcc
cmake --build --preset gcc
ctest --preset gcc
```

CTest runs the synthetic checks, which create their own fixtures and save traces
in a new run directory. To choose where those results go, run the test directly:

```fish
../../../build/asr-image-trace/gcc/Release/asr-image-trace-synthetic \
    --output-root ./results
```

## Native bundle and signing

The Mac build requires macFUSE, Fish, and jq. It assembles the executable,
metadata, and Fish controller into an app bundle, then signs the whole bundle
with `codesign`.

The default build omits `com.apple.developer.vfs.snapshot` and supports supplied
snapshots and synthetic producers. Automatic fixture snapshot creation requires
opting into this restricted entitlement in an AMFI-disabled macOS VM, as shown
below.

To select another signing identity:

```fish
cmake --preset gcc \
    -DDEVICEFS_ASR_SIGNING_IDENTITY='YOUR SIGNING IDENTITY'
cmake --build --preset gcc
```

## Run on macOS

Run from Fish as a user with permission to invoke `asr restore` through `sudo`.
With the default build, supply an existing volume path and snapshot's name or
UUID. This path skips snapshot creation and asks Apple's `asr` to perform the
restore. The runner leaves that volume's snapshots unchanged:

```fish
set native ../../../build/asr-image-trace/gcc/Release/asr-image-trace-macos.app/Contents/MacOS/asr-image-trace-macos
$native --output-root ./results \
    --source-volume /System/Volumes/Data --snapshot 'SNAPSHOT NAME OR UUID'
```

Use `--producer serial` or `--producer jump` to exercise macFUSE with the
synthetic producer. These runs also create their own fixtures and use the
default build.

To create the source volume and snapshot automatically in an AMFI-disabled
macOS VM, enable the snapshot entitlement:

```fish
cmake --preset gcc -DDEVICEFS_ASR_SNAPSHOT_ENTITLEMENT=ON
cmake --build --preset gcc
$native --output-root ./results
```

## Read the results

Each run prints its results directory. On macOS, `filesystem.log` contains the
summary and `trace.tsv` contains individual reads and writes. The producer's
output goes to `asr.log`, or to `producer.log` for a synthetic run.

The filesystem keeps two caches, each allocated on demand:

- The readback cache retains buffers that have been read and updates them on
  later writes. Exceeding its limit throws an exception describing the request
  and cache usage.
- The rolling write cache retains recent writes, dropping the oldest write
  buffers when space is needed. Reading those bytes also retains them in the
  readback cache, where they survive rolling eviction.

Both limits are 4 GiB, set by `kReadbackCacheLimit` and `kRollingWriteCacheLimit`
at the top of `trace.ixx`. Peak buffer storage is printed as `READBACK CACHE PEAK`
and `ROLLING WRITE CACHE PEAK`, and saved in `trace.tsv` as
`readback_cache_peak_bytes` and `rolling_write_cache_peak_bytes`.

The trace ends with summaries for `setup`, `restore`, `finalize`, and `detached`:

- `nonforward_requests` counts writes that start before the furthest end offset
  of an earlier write request. It detects both backwards writes and overlapping
  requests.
- `behind_completed_writes` counts writes that start before the furthest end
  offset of a write that has already completed.
- `reads` and `read_bytes` show how much the destination was read. Setup includes
  the reads needed to attach the initial image. Reads return the initial image's
  contents with later cached writes applied.

Setup is counted separately. Writes made during detachment are compared with
the restore's earlier writes, so late rewrites are included in the result.
The final `complete` field reports whether the capture finished successfully,
including target detachment.
