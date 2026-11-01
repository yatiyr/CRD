# REPO.3: the QFont line is Qt's resolveFont; the Shewchuk oracle under the tree's warnings

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3](../ROADMAP.md#slice-repo.3).
> Contract: [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` at 525c8417 plus this batch's commits. The track machine ran about 0.35 h
(builds, the oracle's teeth and a reading of the pinned installer as data; nothing of it was run).

## CI first

GitHub Actions started no new job between 10:39 and 13:46: the dispatched CI runs 41298463221, 41302856153 and
41311293265 and the scheduled 41295924613 stayed at `Preflight (tier resolution)`, queued, every push run was
cancelled by the next push before it started, and the one run in progress, 41295647725 at 43963cb2, held 12 jobs.
The status page reported no incident. The host-check run 41312886689 ran its emitter leg at 13:54 (below).

41295647725 predates a0c9d534: its four failing lanes (`linux-gcc-arm64-asan`, `linux-clang-arm64-asan`,
`linux-gcc-asan` shard 2, `win-asan` shard 1) fail only the `[shewchuk]` cases on the look-ahead read a0c9d534
guards, and shard 2 the registered [TP-5](../third-party-defects.md#tp-5). Its completed jobs carry no warning
annotation, but the five Clang jobs' logs (`win-clang-cl`, `win-clang-cl-shipping`, `linux-clang-tsan`,
`linux-clang-arm64-asan`, `linux-clang-arm64-debug`) print 33 lines each of `-Wdeprecated-non-prototype` from
`shewchuk-predicates.c`: the oracle this row routed, compiled without any warning set, so no lane could fail on it.

## The oracle under the tree's warnings

- Its 42 compiled definitions were K&R ones (an identifier list with declarations, or an empty list). Each is now
  the prototyped definition of the same types (a throwaway converter that moved the five header comments under the
  new headers; the commented-out `doubleprint` keeps its form). `floatprint`, whose `float` parameter K&R promoted,
  is never called.
- `crd-geometry-shewchuk-oracle` links `crd-warnings`, so it compiles with the tree's set, as errors. GCC at -O1 and
  above then asked for the tail lengths of `incircleadapt` (`-Wmaybe-uninitialized`: each is written under the same
  test that guards its read); the six start at zero. No other finding at -O0, -O2 or -O3 with GCC 13.3 or Clang 18.
- `crd-warnings` adds `-Wold-style-definition` and `-Wstrict-prototypes` for C on GCC and Clang: Clang reports a K&R
  definition by default and GCC never does, so both now hold one C contract (MSVC's /W4 has it as C4131). The tree's
  other C programs were checked: `bench_dsp_windows_vs_liquid.c`'s `now_ms()` and `main()` take `(void)`.
- On MSVC `/permissive-`, `/Zc:__cplusplus` and `/EHsc` now apply to C++ alone; the oracle is the first C unit that
  takes the tree's warnings there.
- The README beside the oracle names the second repair.

Evidence on the track machine: `linux-gcc-debug` builds whole with no warning; `crd-geometry-primitives-tests` passes
(118,529 assertions in 211 cases; `[shewchuk]` 73 in 4) and the 20 guard CTests pass; `linux-gcc-release` builds the
tests; the `peers-release` tree builds the five liquid-dsp peers and the ggml peer. Teeth: `orient2dfast` restored to
its K&R form fails the oracle's build (`-Werror=strict-prototypes`, `-Werror=old-style-definition`).

## The QFont line's emitter

Run 41312886689 (`vulkan-sdk-installer-emitter`, job 124866783846) installed the pinned SDK under `cdb` with the
production arguments: exit 0, the layout complete, one `setPixelSize` call, with `edx = ffffffff` (-1), logged before
`[485] Warning: QFont::setPixelSize: Pixel size <= 0 (-1)`. The leg passed, but its `result.json` named no caller,
for two defects of the parser: `cdb` pads the `k` columns with several spaces, which the frame pattern did not take,
and it names each site by the image's nearest export, `installerbase!qt_startup_hook+0x...`, which no RVA can be
read from without that export's address. From the log by hand: frame 0 is `qt_startup_hook+0x79ed10` = the
function at 0x806a90, so the export is at 0x67d80, and frame 1, `qt_startup_hook+0x8b6171`, returns to 0x91def1.

The census had 0x91deda and 0x91def1 as a size scaled "only after `pixelSize() > 0`". Reading the function again
(`.pdata`: 0x91dd20, its fragment from 0x91deab) corrects that: the branch at 0x91dec3 is `qRound`'s sign test, not a
guard. The function resolves a format's font, finds property 0x2002 (`QTextFormat::FontSizeAdjustment`) in its
property vector, clamps it plus 2 to `[0, 6]`, takes a scale from the table at 0x141733430 (read from the file: 0.7,
0.8, 1.0, 1.2, 1.5, 2.0, 2.4) and, when the default font's `pointSize()` is not positive, calls
`setPixelSize(qRound(scale * pixelSize()))`, otherwise `setPointSizeF(scale * pointSizeF())`. That is Qt 6.7.2's
`QTextFormatPrivate::resolveFont(const QFont &defaultFont)` line for line, the scale table included. A document
default font with neither size set (`pointSize() <= 0` and `pixelSize() == -1`) gives -1 for any scale up to 1.2.
Qt's `dev` branch has the same unguarded branch. The text it formats is the framework's own: both `LICENSE.txt`,
`Updates.xml`, `config.xml` and the scripts carry no heading or size markup. The markdown importer (0xa623f5) is not
the emitter.

The script now asks the debugger for the caller instead of reading it from the frames: the breakpoint runs
`r $t0 = poi(@rsp)-(@$exentry-<entry RVA>); r $t0` at the function's entry, where `[rsp]` is the return address, so
the log carries each call's return RVA whatever symbols `cdb` finds; the frames stay as evidence with the padded
columns parsed. A return address outside the image is reported as such. The census names 0x91deda and 0x91def1 as
`resolveFont`, and the leg now passes only when the census names the caller of every call with a size <= 0 (the
`unnamed` list is empty). `test_vulkan_sdk_installer_trace_names_the_qfont_warnings_caller` takes the hosted log's
form (padded columns, export-relative sites) and covers a census caller, the emitter, an RVA outside the census, a
return outside the image and a call with no return line; teeth: the old single-space frame pattern, a missing
return line, the emitter left out of the census and a pass rule that ignores unnamed calls each fail it. Parsed with
the new code, the hosted log gives the call's 18 frames.

[TP-8](../third-party-defects.md#tp-8) registers the line: Qt's code inside the pinned installer, one log line, no
effect on the installed tree, nothing the tree can change.

### The upstream report

No report exists (the Qt tracker's QTBUG and QTIFW projects, searched for the warning, `resolveFont` and
`FontSizeAdjustment`). Filing one needs a Qt account, so it is the user's. The text:

> **QTextFormatPrivate::resolveFont passes -1 to QFont::setPixelSize when the default font has no size**
>
> Component: GUI: Text handling. Versions: 6.7.2 (in Qt Installer Framework 4.9.0), still on `dev`.
>
> For a format with `QTextFormat::FontSizeAdjustment`, `QTextFormatPrivate::resolveFont()` takes the pixel branch
> whenever the default font's `pointSize()` is not positive and sets the font's pixel size to the rounded product of
> the scale factor and `defaultFont.pixelSize()`, without checking that pixel size. When the default font has
> neither a point size nor a pixel size
> (`pointSize() == -1` and `pixelSize() == -1`), the scaled value rounds to -1 (or -2 for the larger factors) and
> `QFont::setPixelSize` prints `QFont::setPixelSize: Pixel size <= 0 (-1)`. Seen in the Vulkan SDK 1.4.341.1
> installer (IFW 4.9.0) on every unattended install. Expected: the branch is taken only when
> `defaultFont.pixelSize() > 0`, leaving the size unresolved otherwise.

## Remaining on REPO.3

- Dispatch the emitter leg again at this batch's revision: the leg passes only with the caller named
  (`emitters` holds `resolveFont`, `unnamed` is empty).
- The Qt report above (the user's, with a Qt account).
- The first complete run at a0c9d534 or later: the ASan lanes' `[shewchuk]` cases, the cold `win-debug-scalar`
  install with the local repository, and the oracle with the tree's warnings on MSVC, clang-cl and the Linux Clang
  lanes (no `-Wdeprecated-non-prototype` line).
- The Linux repository-checks job of 41298463221 (queued since 10:39).
- The CUDA run on the measuring PC's NVIDIA GPU; then the close after REPO.3d.
