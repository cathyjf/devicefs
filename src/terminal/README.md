# DeviceFs terminal library

The DeviceFs terminal library learns text layout from the terminal displaying
it. It provides Unicode-aware wrapping, incremental screen rendering, and
scrolling menus for modern C++ on Windows, GNU/Linux, and macOS.

Its central idea is to use the terminal's own cursor position as a measurement.
The library writes text, observes where the terminal placed it, and uses that
information to find line breaks and position later output. Conservative width
bounds let it write text in batches; recorded measurements let subsequent
updates reuse what the terminal has already established. This connects Unicode
layout and efficient repainting: the positions learned while drawing become
the positions used to preserve unchanged text.

DeviceFs implements the observed-wrapping algorithm, continuation indentation,
measurement cache, frame comparison, and menu behavior. An adapted copy of
Microsoft's Windows Terminal character-width detector supplies Unicode grouping
and the conservative estimates used to decide how much text can be written
before another observation is needed.

The library is developed alongside [DeviceFs](../../README.md). Its
[independent build and demonstrations](#building-and-trying-it) make it possible
to exercise the terminal component separately from the backup application.

## Learning layout from the displaying terminal

A terminal interface needs to know where text ends. That determines where the
next entry begins, which cells belong to a selection highlight, and what can
be erased when a label becomes shorter. A wrong width can therefore do more
than produce an untidy line break: it can make one item's text interfere with
another item's display.

Unicode makes width a property of rendered text rather than its byte count.
For example, `e` followed by a combining acute accent has three UTF-8 bytes and
two code points, but a terminal can display the composition in one cell. A
joined emoji such as 👩‍💻 has several components that can form one symbol.
Keeping those components together matters both when wrapping and when
repainting part of a line. Unicode's
[grapheme-cluster rules](https://www.unicode.org/reports/tr29/#Grapheme_Cluster_Boundaries)
describe these boundaries between user-perceived characters.

A tool that may seem relevant to this problem is the POSIX
[`wcwidth`](https://man7.org/linux/man-pages/man3/wcwidth.3.html) facility. It
returns the column width of one wide character according to the process's
locale. It can account for a two-column ideograph and a zero-width combining
accent, so adding its results is more useful than counting code points. But
each call sees just one character. Adding the widths of the woman, joiner, and
laptop in 👩‍💻 treats the components independently, whereas a terminal that
supports the [joined emoji](https://www.unicode.org/reports/tr51/#Emoji_ZWJ_Sequences)
displays them as one symbol. Summing individual widths therefore does not
establish the width of the composition.

Even a calculation that recognizes whole compositions must agree with the
displaying terminal's width policy. Some characters have an ambiguous width,
and terminal settings can change how compositions are rendered. The process's
locale does not establish those settings, so a local width calculation can
disagree with the terminal. Unicode explicitly explains that its
[East Asian Width property requires tailoring for terminal emulators](https://www.unicode.org/reports/tr11/#Scope).

This library makes the display itself part of the calculation. Cursor-position
reports identify the row and column reached after output. Those observations
establish where a name actually wraps and where its character groups end.
The same mechanism works through the native adapters on all three platforms.

### Conservative batches, observed line breaks

`WriteWrappingText` displays a prepared label from the current cursor position,
with caller-selected continuation indentation and a limit on the rows it may
occupy. Its algorithm combines computation with observation:

1. **Keep compositions together.** `MeasureText` divides the label into groups
   and assigns each group a conservative width bound under the selected width
   policy. Every write ends at a group boundary.
2. **Write a batch that fits.** Starting from the reported cursor position, the
   writer adds groups while their combined bounds fit in the available columns.
   If the batch completes the label, the operation is finished. A fitting label
   therefore needs just its initial cursor query and one text write.
3. **Observe the uncertain part.** When text remains, the writer queries the
   position reached. A conservative estimate may have reserved more space than
   the terminal used, leaving room for another batch. Near the margin, the
   writer submits one complete group and observes whether it wrapped.
4. **Indent the continuation.** If the group reached the next row, the writer
   inserts space before that already-rendered group and positions the cursor
   after it. The next batch continues from there.

This allows a long label to use the space the terminal actually provides,
while batching reduces the number of exchanges needed to discover its layout.
The bound answers how much text can safely be submitted together; the cursor
report answers where that output went. Microsoft's
[VT reference](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#query-state)
documents the position query, and its
[text-modification reference](https://learn.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences#text-modification)
describes the insertion operation used for indentation.

The last permitted row has a stricter rule. A probe that wrapped beyond that
row could overwrite a footer or scroll the screen. The writer stops when the
next group's bound no longer establishes that it fits, and returns the exact
unwritten suffix. The caller can also reserve columns for an ellipsis on that
final row. The result distinguishes completion, a row limit, a group too large
for the available width, and a layout that needs redrawing.

### Width policies

The default `WidthPolicy::AllModes` reserves space for the grapheme, `wcswidth`,
and legacy console modes represented by the Windows Terminal detector. It also
keeps together compositions whose group boundaries differ between those modes.
A caller targeting Windows Terminal's grapheme mode can select
`WidthPolicy::WindowsTerminalGraphemes` for tighter bounds. Both policies reserve
two columns for ambiguous-width characters.

These are bounds for the selected measurement modes. An unusually long
composition can fit visually in a small space but exceed an entire row's
`AllModes` bound because the legacy console mode can display its components
separately. The writer then returns that group as part of the unwritten suffix.
The caller can provide more space or select the narrower policy when it matches
the displaying terminal.

## Reusing observations to repaint less

The application describes the desired screen in a `FrameBuffer`.
`DeltaFramePresenter` turns that description into terminal output. It retains
the previously displayed text, highlighting, and character-group positions,
then compares them with the next frame.

An unchanged group at the same columns needs no output. Changing a selection
updates the affected highlighting; changing a digit in a status line can leave
the surrounding text untouched. Shortening a row erases the obsolete tail.
The comparison works with complete character groups, including combining text
and joined emoji.

The presenter records group widths as it observes their ending positions.
Those measurements are also available to menu layout, so returning to familiar
text can calculate its wrapped rows in memory. This makes a terminal exchange
useful beyond the particular frame that required it: later layout and painting
share the result.

Resizing changes which text fits, while character widths remain available for
the new layout. Rows whose contents and positions remain valid can be retained.
A reduction in height can move the terminal's existing contents upward; the
presenter accounts for that by repainting the affected screen. **Ctrl+L**
discards both layout and width measurements, allowing a fresh display after a
font or width-mode change.

Drawing commands are collected into batches. When the positions needed for an
update are already known, its drawing can be sent in one write. The menu also
requests synchronized output around layout and presentation, allowing terminals
that support it to keep the preceding display visible during the update. A
terminal may limit that hold's duration, so reducing output remains useful.

The frame boundary keeps these decisions in the presenter. Menu navigation
constructs the next desired screen; the presenter decides how to make it
visible. A different presentation implementation can use the same frames.

## Menus with readable long entries

`SelectMenuItem` presents a list between caller-supplied header and footer
lines. The selected entry has a `>` marker and reverse-video highlighting.
Long entries wrap, with their continuation lines indented beyond the first
line's text. Scrolling counts displayed rows, so a multiline entry remains one
selectable item even when only part of it fits in the viewport.

Entry previews occupy at most eight rows. A longer name ends with `...`, and
the footer offers **1: View full name**. That opens a scrolling view of the
complete prepared text. Returning from the view preserves the menu selection
and list position.

| Control | Action |
|---|---|
| Up / Down | Select the preceding or following entry. |
| Page Up / Page Down | Scroll by a viewport of displayed rows. |
| Home / End | Reach the first or last entry. |
| Enter | Return the selected entry. |
| 1 | Open the full-text view of a truncated entry. |
| Escape | Return from the full-text view or cancel the menu. |
| Ctrl+C | Cancel, including while a terminal query is waiting. |
| Ctrl+L | Discard cached layout and measurements and redraw. |

Arrow-key scrolling reveals one additional row at a time by default; a template
policy also provides page scrolling. Resizing recalculates line boundaries and
preserves the selection as far as the new dimensions allow. Empty lists and
windows too small for the menu have explanatory displays and remain cancellable.

The result is the selected entry's original index, or an empty `std::optional`
on cancellation. The menu prepares its own copies of the labels. The caller's
original data and the identifiers associated with those labels remain available
for the selected operation.

## Preparing application data for display

Filenames and server-supplied labels can contain bytes that a terminal treats
as commands. `PrepareTerminalText` removes those commands and their payloads
before layout. This includes cursor movement, formatting, title changes,
hyperlinks, and control strings. Hyperlink text remains visible after its
surrounding commands are removed.

Individual controls become readable notation. A newline appears as `\n`, a
tab as `\t`, and an invalid UTF-8 byte as `\xHH`. Literal backslashes are
doubled, distinguishing a real newline from a name containing the two
characters `\n`. Each label has independent parser state, so an unfinished
command in one entry cannot consume the following entry.

Unicode directional controls are also represented visibly: a left-to-right
mark, for example, appears as `\u{200E}`. This is a known limitation for
mixed-direction text: an Arabic or Hebrew filename can rely on those controls
to position a Latin word, number, or punctuation correctly. Replacing them can
change the name's intended presentation. The
[Unicode Bidirectional Algorithm](https://www.unicode.org/reports/tr9/#Directional_Formatting_Characters)
explains their legitimate role in filenames and labels. The
[text module](text.ixx) documents the complete filtering policy.

For captured logs, `VtFilter` provides an in-place byte-buffer interface with
parser state retained between reads. Commands and payloads can cross buffer
boundaries. That interface retains tabs, line feeds, and ordinary non-ASCII
bytes while removing carriage returns and other ASCII controls. It filters
bytes without decoding UTF-8; `PrepareTerminalText` supplies the additional
decoding and control representation needed for labels.

## Using the C++ interfaces

The library exposes C++ modules and concepts. The interfaces can be used at
several levels:

| Interface | Purpose |
|---|---|
| [`PrepareTerminalText`](text.ixx) | Prepare a label for display, removing terminal commands and representing controls. |
| [`MeasureText` and `WriteWrappingText`](terminal.ixx) | Group prepared text, establish width bounds, and display it across a bounded set of rows. |
| [`FrameBuffer` and `DeltaFramePresenter`](frame.ixx) | Describe a complete screen and update the terminal to match it. |
| [`SelectMenuItem`](menu.ixx) | Run a scrolling selection interface with headers, footers, and full-name inspection. |

Applications can use `WindowsConsole` or `UnixConsole`, or provide an adapter
that satisfies the relevant concept. A wrapping adapter needs only text output
and cursor observation; menus add input, screen dimensions, presentation, and
the associated lifetimes. The [menu demonstration](tests/main.cpp) shows a
complete selection operation and how to scope the console around it.

The caller selects a UTF-8 `LC_CTYPE` locale before preparing or measuring text
on GNU/Linux and macOS. The demonstration checks that selecting `C.UTF-8`
succeeds; its Windows branch selects `.UTF8`. The Windows executable also uses
the project's [UTF-8 process manifest](../resources/devicefs.manifest).
Returned text views borrow their original storage; the module interfaces
document the lifetimes required by each operation.

### Native I/O and recovery

Shared code owns layout, rendering, and menu behavior. `WindowsConsole` owns
Windows console handles and reads native input records. `UnixConsole` owns a
connection to `/dev/tty` and its terminal settings. Both connections are
independent of redirected standard streams, allowing an application to keep
interactive display separate from data written to a file or pipeline.
Windows text output converts UTF-8 to UTF-16 for `WriteConsoleW`, preserving
the console code page shared with other programs.

Scoped owners manage temporary input modes, the alternate screen, cursor
restoration, and synchronized updates. Returning a selection, cancelling, or
unwinding an exception releases those owners in the required order. I/O errors
propagate to the caller, and restoration preserves an error already in flight.

Terminal reports and keyboard input share an input channel. The adapters
recognize fragmented replies and retain unrelated keys. Queries have a bounded
wait and can be interrupted by Ctrl+C. If layout cannot obtain a usable report,
the menu displays recovery instructions without making another query to draw
them. The user can then retry, resize, or cancel.

The native adapters require a terminal that implements the VT operations and
reports used by the library. Actual rendering has been exercised in Windows
Terminal, Windows Console Host, and macOS Terminal, including a connection
through SSH. The automated tests described below exercise the shared algorithms
and native input behavior separately.

## Building and trying it

Run these commands from `src/terminal` in a complete DeviceFs checkout. The
component has its own [CMake project](CMakeLists.txt) and
[presets](CMakePresets.json); the commands build the library and its test
executable.

On GNU/Linux or macOS:

```console
cmake --preset unix
cmake --build --preset unix
ctest --preset unix
../../build/terminal/unix/Release/devicefs-terminal-test --menu
```

The Unix build uses LLVM/Clang, libc++ with its standard-library module files,
Ninja, and LLD. Current Unix development builds use Clang 23 and CMake 4.4.3.
The [toolchain file](../unix/cmake/llvm.cmake) also searches Homebrew's LLVM and
LLD installations on macOS.

On x64 Windows, with Visual Studio 2026 and PowerShell:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64-release
ctest --preset windows-x64
..\..\build\terminal\windows-x64\Release\devicefs-terminal-test.exe --menu
```

For ARM64 Windows, use `windows-arm64` for configuration and tests,
`windows-arm64-release` for the build, and `windows-arm64` in the executable
path. The Windows build uses the checkout's WIL dependency and native-analysis
settings. The presets enable the optional test executable; a direct CMake
configuration can select it with `DEVICEFS_TERMINAL_BUILD_TESTS`.

The executable also accepts `--text TEXT` and `--file FILENAME` to exercise
wrapping with supplied input. With no arguments, it writes sample text from
the current cursor position. Use `--measure-menu` to try the menu and report
its interaction costs afterward.

## Verification and performance measurements

The self-tests exercise grouping, observed wrapping, clipping, selective
repainting, multiline navigation, resizing, and recovery. Simulated displays
record the affected cells, allowing tests to check that a changed value
preserves surrounding Unicode text and that an unchanged frame produces no
drawing output. Reproducible generated cases combine malformed UTF-8 and
terminal commands; log-filter tests vary the input chunk boundaries.

Native tests use an isolated Windows console or Unix pseudoterminal. They
exercise fragmented replies, keys around replies, cancellation, late reports,
query deadlines, disconnection, and restoration after errors. Unix tests also
interrupt waits with signals. These tests complement the simulated displays
by exercising the operating system's actual input and lifetime behavior.

`--measure-menu` records elapsed time, output calls, supplied UTF-8 bytes, and
cursor and size queries for each update. Measurements include both ordinary
output and the update guard's control output. Waiting for user input is
excluded, and results are printed after the menu exits. The measurements expose
the difference between first presentation of unfamiliar text and later
selection changes, scrolling, and resizing that can reuse observed widths.

First-party builds treat warnings as errors. Windows builds also run the
repository's native code analysis. CTest runs both suites when the test
executable is enabled.

## Licensing

DeviceFs code is licensed under the
[GNU General Public License, version 3 or later](../../LICENSE.txt). The adapted
Windows Terminal detector retains its upstream MIT license. Its
[dependency notes](dependencies/terminal/README.md) identify the source revision
and describe the changes that let the UTF-16 detector run on all three
platforms.
