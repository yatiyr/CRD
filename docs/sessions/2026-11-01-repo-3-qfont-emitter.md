# REPO.3: the Vulkan SDK installer's QFont line comes from the framework's Qt, not LunarG's scripts

<!-- doc-role: historical -->
> Dated evidence. Live owner: [REPO.3](../ROADMAP.md#slice-repo.3).
> Contract: [test instruments](../design/test-instruments.md#host-separation-diag10a). Rules: [AGENTS](../../AGENTS.md).

Track B, 2026-11-01, on `cerid-track-b` at e834f58d plus this batch's commit. The track machine ran about 0.27 h: it
downloaded the pinned installer (`vulkansdk-windows-X64-1.4.341.1.exe`, SHA-256 `bcf2d75a...5bfe`, verified) and
read it as data; nothing of it was run.

## CI first

No hosted run covers this track's last commits yet. Every push run from 41298857174 to 41308959544 was cancelled by
the next push; 41310778561 at 3988e430 has no jobs; the GitHub mirror stops at 3988e430, the parent of a0c9d534 (the
Shewchuk guard), so no run contains the guard or the local-repository argument (5e4093d9). The dispatch run
41298463221 at 13453acb (track C's) has 26 of its 29 jobs queued; its `Repository checks (windows-2025-vs2026)`
(job 124824411345) passed, the first Windows repository-checks job to run the checkout-path and text-encoding
guards. When its ASan lanes fail the `[shewchuk]` cases or its `win-debug-scalar` prints `Retrieving meta information
from remote repository`, that is the state before a0c9d534 and 5e4093d9, not a regression. The host-check run
41298875777 still has `minimal-updates` and `temp-repository-empty` queued.

## What the installer carries

The installer is the Qt Installer Framework's `installerbase` (`IFW Version: 4.9.0, built with Qt 6.7.2.`, a static
Qt) with one binary resource collection appended (the Qt `rcc` format, at file offset 30,487,552), read with a
throwaway parser on the track machine. It holds ten files: `installer-config/config.xml`, the control script
`installscript_qs.qs`, two icons, `Updates.xml`, `config/config-internal.ini` (`offlineOnly=false`), the two
components' `LICENSE.txt` and `com.lunarg.vulkan.core/operations.qs`, and the `.qrc` listing. No `.ui` file: the core
component's constructor calls `installer.addWizardPage(component, "deprecationNotice", ...)`, but no user interface of
that name exists, so the call adds nothing.

Neither script touches a font: the control script hides pages and sets labels in GUI callbacks, `operations.qs` adds
the VC runtime check, the layer registry operations and the `PATH` helper. REPO.3's text, which placed the emitter in
"the pinned installer's control and component scripts", was wrong.

## The census of QFont::setPixelSize

The warning's format string `QFont::setPixelSize: Pixel size <= 0 (%d)` is loaded by one function, at RVA 0x806a90
(the image's `.pdata` bounds it): `QFont::setPixelSize`. Every direct `call`/`jmp` to it in `.text` was found and the
instructions before each read (`objdump` on the raw bytes). The 15 callers, by how the size argument is formed:

| Return RVA | Caller | The size passed |
|---|---|---|
| 0x209c3a | the framework's `ComponentSelectionPagePrivate` (its strings: `ComponentsTreeView`, `Create Offline Installer`) | the constant 16 (`captionFont.setPixelSize(16)` in IFW 4.9.0's `componentselectionpage_p.cpp`) |
| 0x2e864b | unnamed | a positive size scaled by a constant, after a `jle` guard |
| 0x36fad8 | unnamed | a rectangle's height (`bottom - top + 1`) |
| 0x3a1b8a, 0x3a1eca, 0x3a3640 | Qt SVG text layout (`qt.svg.draw`) | a point size times a scale, truncated |
| 0x803003 | unnamed, beside QFont | a parsed integer, or a fallback when it does not fit an `int` |
| 0x91cb52 | unnamed | a parsed integer |
| 0x91deda, 0x91def1 | unnamed | `pixelSize()` times a factor, only after `pixelSize() > 0` |
| 0x963175 | unnamed | a parsed integer clamped to `[0, 0xffffff]` |
| 0x9b900d, 0x9b946c | unnamed | `pixelSize() * 2 / 3` (an unset `-1` gives 0, not -1) |
| 0xa56bb1 | unnamed | a size clamped to `0x3fffffff` |
| 0xa623f5 | Qt's `QTextMarkdownImporter::import` (strings `qt.text.markdown`, `default font`, `mono font`) | the document default font's `pixelSize()`, unguarded |

Qt 6.7.2's `qtextmarkdownimporter.cpp` reads `if (defaultFont.pointSize() != -1)
m_monoFont.setPointSize(defaultFont.pointSize()); else m_monoFont.setPixelSize(defaultFont.pixelSize());`: a default
font with neither size set passes exactly -1, the logged value. The same lines are in Qt 6.8.0 and 6.10.0; Qt's `dev`
branch no longer sets the mono font's size in `import()`. Only two callers can pass exactly -1: the markdown importer
and the parsed integers at 0x803003 and 0x91cb52 (a font string's `-1` pixel-size field). Static reading cannot say
which runs between `Loading component scripts...` and the component selection, so the emitter is not yet named.

## The emitter leg

[vulkan-sdk-installer-check.py](../../scripts/vulkan-sdk-installer-check.py) `trace` runs the production install
(`install-vulkan-sdk.py`'s arguments, the local repository included) under `cdb -G -cf <commands>`, the commands
`bp @$exentry-<entry RVA>+0x806a90 "r edx; k 12; gc"`, `g`, `q`: the breakpoint is placed by RVA from the image's
entry point, wherever ASLR loads it. It records the log, every call's signed size and frames, and names each caller
from the census (`CALLERS`; a return address outside it is reported by RVA, a caller outside the image as such). The
census is bound to the pinned digest (`CENSUS_SHA256`): a new pin is refused until the census is taken again. `cdb` is
found on `PATH` or in the Windows SDK's Debugging Tools. `cdb`'s exit is the debugger's, so the install's outcome is
its layout; the leg passes only when the layout is complete and a call passed a size of zero or less.
`host-check.yml` runs it as the dispatch-only experiment `vulkan-sdk-installer-emitter` on a fresh `windows-2025`
machine (artifact `host-check-installer-emitter`, outside the arms' judge pattern).

`test_vulkan_sdk_installer_trace_names_the_qfont_warnings_caller` holds the entry-point reader (and a non-PE refusal),
the debugger lookup, the command and commands file, the parse (sizes 16, -1, 0, -2; the census caller, one outside
it, one outside the image), the leg's pass rule and the digest refusal; teeth: an unsigned size (`4294967295 != -1`)
and frame 0 as the caller (`0x806a90` for every call) each fail it.

## Evidence

- Track machine: the download, its digest, the resource listing, the format-string reference and the 15 call sites.
- Local: `test-repository-tools.py -k vulkan` (4 tests) passes; the validators pass.

## Remaining

- Dispatch the emitter leg and read its `result.json`: the named caller is the emitter. Then the repair: when it is
  Qt's markdown importer, the line is Qt's (fixed on `dev`), which LunarG ships inside IFW 4.9.0, and the register
  entry and the upstream report to LunarG follow from that; when it is another caller, its code decides.
- The cold `win-debug-scalar` install with the local repository, and the ASan lanes' `[shewchuk]` cases, on the first
  complete run at a0c9d534 or later.
- The CUDA run on the measuring PC's NVIDIA GPU; then the close after REPO.3d.
