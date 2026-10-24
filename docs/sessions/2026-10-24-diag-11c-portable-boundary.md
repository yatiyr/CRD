# DIAG.11c: the portable boundary audit, 2026-10-24

<!-- doc-role: historical -->
> Dated evidence. Live owner: [DIAG.11c](../ROADMAP.md#slice-diag.11c). Contract:
> [runtime diagnostics](../design/runtime-diagnostics.md#diag-11c),
> [ADR-0133](../decisions/0133-runtime-diagnostics-and-instrumentation.md). Rules: [AGENTS](../../AGENTS.md).
> Previous batch: [DIAG.11c native workflow](2026-10-24-diag-11c-native-workflow.md).

## CI read at the start

`gh run list --branch main --limit 15`: no completed run has a failed job. Every completed push run since 40079623101
was cancelled by the next push, none with a failed job; 40080021621 (one job left) and 40103197370 were left alone.

## Audit

Each public schema and host adapter was read for pointer- or `long`-width fields, `wchar_t` or native path strings,
byte order, architecture-only fields and file-only routes.

| Contract | Finding | Result |
|---|---|---|
| `crash.hpp` (crash adapter) | `CrashReport::dump_path`, `capture_dump`'s out path and `read_dump_stream` used `const wchar_t*`, a Windows path type, in a header every platform compiles; every other engine path is UTF-8 (`platform::fs::Path`) | Fixed: UTF-8 both ways |
| `diag_path_is_safe` (diag command paths) | accepted `con`, `nul.json`, `com1`, `a.`. Probed on this Windows 11 host: a bundle published as `nul` (written to `nul.tmp`, renamed over `nul`) fails the rename onto the device and leaves `nul.tmp`; `nul.json` and `con.json` are plain files here but devices before Windows 11; bare `con` is the console | Fixed: refused on every host |
| Bundle, manifest, symbol index, CPROF | fixed-width fields, size and offset pins; copied as host bytes, little-endian only by comment | `static_assert(CRD_LITTLE_ENDIAN == 1)` beside the pins |
| Flight record, build identity | fixed-width fields written byte by byte, little-endian; `arch` is `crd::arch_name()` text | Portable |
| Linux crash record | text; registers for x86_64 and aarch64, `unsupported-arch` otherwise; tid as decimal | Portable |
| Minidump module parser | platform-neutral byte parser of an architecture-independent stream | Portable |
| DX12 removal and Vulkan loss evidence | build-local records inside a Windows dump; a reader refuses another build's size | Not a portable schema, by design |
| Bundle, capture and flight routes | each format has a byte-buffer writer and reader; `write_bundle_atomic`, `save_capture_to_file`, `crash::install` and bundle retention are file adapters over them; an empty capture directory keeps captures in memory; an empty command root refuses path commands | Portable; file adapters named for WEB.DIAG |

Named, owned, not changed here:
- wasm32: `platform.hpp` refuses any architecture but x64 and ARM64, so no 32-bit build exists; there a pointer-width
  field would fail the sizeof pins (JOBS-PORT.3, WEB.DIAG).
- macOS thread ids are 64-bit; `CrashReport::faulting_tid` and `platform::threading::current_thread_id()` are `u32`,
  enough for Windows and Linux. No on-disk schema stores an OS thread id in binary (MAC.DIAG).
- A Mach-O UUID symbol kind: `read_symbol_index` clamps an unknown kind to `None`, so an older reader refuses a newer
  kind instead of matching it (MAC.DIAG).

## Change

- `crash.hpp`/`crash.cpp`: the writer converts the dump path to UTF-8 once per write into a BSS buffer (Win32, no
  CRT lock), which the fatal-path stderr line now also prints (its own narrowing buffer is gone);
  `read_dump_stream` converts its UTF-8 argument under its own lock. `dx12_read_removal_bundle` and
  `vk_read_loss_bundle` take UTF-8. The tests pass `fs::path::u8string()`; `minidump_probe.hpp` takes
  `std::filesystem::path`.
- `diag_commands.cpp`: `portable_component` refuses a component ending in `.` and a Windows device name (CON, PRN,
  AUX, NUL, COM0-9, LPT0-9 before the first `.`, any case).
- Four schema headers assert a little-endian host. Strict tidy on the touched `capture.hpp` named its three
  `_pad_a` spare fields (identifier naming); they are `pad_a` now, with the same offsets and pins, not suppressed.

## Teeth (win-debug, each rebuilt, restored, rebuilt, rerun)

- New case `crash contract: dump paths are UTF-8 both ways, a non-ASCII directory included` (U+00FC U+011F in the
  directory name). The writer converting with `CP_ACP`: three checks fail (the path's bytes, `fs::exists`,
  the read). The reader converting with `CP_ACP`: the read check fails.
- `portable_component` bypassed: `diag commands: a command declaring a second class needs both grants` fails, and
  the service executed `con` and `NUL.json` with status Ok.

## Verification

win-debug: crd-core-tests, crd-jobs-tests, crd-perf-tests, crd-diag-crash-capture-specimen and both GPU test targets
build; 123 affected CTests pass (crash capture and contract, minidump, unloaded modules, live modules, symbolize,
DIAG.7b, DIAG.7c, device-loss injection, hang dump, diag commands, CPROF and capture). linux-gcc-debug
(`wsl-lane.sh build`, `test`): 98 pass. `check-headers.py` on the six headers, the ctest guards, tidy-files.py, clang-format `--dry-run` on added
lines, `check-allman-braces.py`, `check-master-plan.py`, `check-repository.py`.

Hosted once pushed: the new crash contract case on the Windows lanes and the diag commands case on every lane, beside
the row's earlier hosted clauses.

Follow-up for an extra batch: none.
