# COMPLY.1: arguments and environment as UTF-8 on Windows

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Date: 2026-11-01 (UTC evening of 2026-10-31). Row: [COMPLY.1](../ROADMAP.md#slice-comply.1) (track C, batch 11; the
[parallel tracks](2026-10-31-parallel-tracks.md)). It closes the defect the
[screen verb's batch](2026-11-01-comply-1-screen-verb.md#found-windows-command-line-arguments-are-not-utf-8) found.

## CI first

Run 41231558128 (d5cc2ec0, batches 3 to 7) finished its linux-gcc-debug job since the last batch. All 8870 tests of
its first pass passed, crd-net, crd-crypto and crd-comply included. Its second pass failed one test, which is not this
track's: `crd-ci-orchestrator-e2e` (`test_2_mirror_checkout_and_cancellation`: "the build ended before the long step
ran"). That is the CI orchestrator's own end-to-end test, a timing race in its fixture, and it passed in the first
pass of the same job. linux-gcc-asan and linux-clang-tsan were still running. Every later push run was cancelled
before its jobs were scheduled, so batches 8 to 11 still have no hosted result.

## The defect

Windows hands `main(int, char**)` its arguments, and `GetEnvironmentVariableA` its values, in the ANSI code page.

- **Lost characters.** A character the code page lacks arrives as `?`, so a Turkish name such as `DENİZ` is screened
  as another name on a machine set to a Western code page.
- **Changed arguments.** The code page's best-fit mapping turns some characters into the quote or the backslash that
  delimit and escape arguments. U+FF02 FULLWIDTH QUOTATION MARK becomes `"`. A program can then be handed other
  arguments than it was passed (the "WorstFit" argument injection).

The same gap was in `read_environment`, so `APPDATA`, `HOME`, `VULKAN_SDK` and `CRD_ASSETS_DIR` under a user profile
with a Turkish name read wrong. Three modules carried their own UTF-16 conversions: crd-platform had two copies of
`utf8_to_wide` (filesystem and dynamic library), crd-perf a third UTF-16 decoder for minidump names, and crd-perf and
the DX12 adapter called the Win32 code-page conversions directly.

## What landed

**Transcoding, one owner** (`text.utf16`, crd-core, [utf16.hpp](../../engine/foundation/core/include/crd/core/utf16.hpp)):

- `utf16_decode`, `utf8_encode` and `utf16_encode`, plus `utf16_to_utf8` and `utf8_to_utf16`, which write into caller
  storage and return the length the whole form takes.
- Header-only, constexpr and allocation-free, so the environment reader before main and the crash path can use it.
- A code unit is any two-byte type, so a Win32 `wchar_t` buffer is read in place without casting between unrelated
  types.
- An unpaired surrogate reads as U+FFFD, and so does each byte that starts no well-formed UTF-8 sequence, the JSON
  writers' rule.
- A transcoding that does not fit writes the whole code points before the first that does not.
- Adopted by crd-platform (one private `wide_text.hpp` replaces the two `utf8_to_wide` copies and `wide_to_utf8`),
  crd-perf (the minidump UTF-16 names and the loaded module names) and the DX12 adapter name.
- The registry's signature refuses `MultiByteToWideChar` and `WideCharToMultiByte` outside crd-core. The crash path
  and the OS resource probe are crd-core's own.

**The environment** ([environment.cpp](../../engine/foundation/core/src/environment.cpp)): on Windows,
`read_environment` reads through `GetEnvironmentVariableW` and writes UTF-8 into the caller's buffer.

- A value of at most 255 UTF-16 units is read on the stack. A longer one is read into pages of its exact size, which
  are released before the call returns. There is no fixed limit, and the reader stays usable before main.
- UTF-8 takes at least one byte per UTF-16 unit, so a value of `capacity` units or more is reported as too long before
  it is read. `TooLong` is reported only when the UTF-8 form needs `capacity` bytes or more, so the doubling loop of
  `read_environment_text` always ends.
- If another thread lengthens the value between the two reads, the value is read again at its new size.

**The arguments** (`process.arguments`, crd-platform,
[arguments.hpp](../../engine/foundation/platform/include/crd/platform/arguments.hpp)): `Arguments(argc, argv)`
hands on `count()` and an argv-shaped `values()`.

- **POSIX:** main's argv, viewed in place.
- **Windows:** `GetCommandLineW` split by the Microsoft C runtime's rules (Visual Studio 2008 and later), not
  `CommandLineToArgvW`'s, then transcoded to UTF-8. The program sees the arguments its main would have seen, with
  every character intact.
- The splitter is portable (`from_windows_command_line`), so the Linux lanes test the exact code Windows runs.
- The registry's signature refuses `GetCommandLine`, `CommandLineToArgvW`, `__argv` and `__wargv` outside
  crd-platform.

**Adoption.** Every program `main` in the guard's scope takes its arguments through `Arguments` in three lines, and
none of its flag parsing changes:

- the five tools: `ceridc`, `asset_cooker`, `shader_cook`, `cprof_export` and `kir_autotune`;
- the sandbox and the gizmo probe. In the sandbox, the call comes after `request_statistics`, which must run before
  the first allocation.

`cprof_export` and `kir_autotune` now link and declare crd-platform. Their cryptography inventories gained glfw, so
both inventories were regenerated, their findings restated and reviewed again (unchanged: they carry no
cryptography), and the generated-sources manifest refreshed.

**Not adopted, recorded:**

- **The `runtime/examples` measurement programs** read numeric and ASCII flags only, and the capability guard leaves
  them out of scope.
- **The test-support mains** (the DIAG specimens, `main_diag`, `main_stress`, the overhead harness) receive their
  output paths from the DIAG specimen runner. The runner starts them with `CreateProcessA` from a UTF-8 command line,
  so a path under a profile with a non-ASCII name crosses the ANSI code page twice. The runner belongs to DIAG
  (track A). The fix is `CreateProcessW` from `utf8_to_utf16`, with `Arguments` in the specimens' mains. Track A
  should take it with the runner.

## Verification

linux-gcc-debug on the track machine. The build has no warnings. The focused CTest selection passes (180 tests: the
new cases, every ceridc case, the platform filesystem and dynamic-library cases, the perf minidump and module cases,
and the `crd-no-*` and `crd-check*` guards).

- **[utf16] (5 cases):** every length class both ways, surrogate pairs and unpaired surrogates (a lead followed by a
  unit past the trail range included), every class of malformed UTF-8 byte, partial writes that stop at the first
  code point that does not fit, and measuring with no storage.
- **[environment] (3 cases):** a Turkish value read as its UTF-8 bytes, refused whole one byte short, missing and
  empty variables, and a 1200-byte, 800-unit non-ASCII value through `read_environment_text`'s growth. The Windows
  half sets variables through `SetEnvironmentVariableW`.
- **[arguments] (6 cases):** the six documented C runtime examples, blanks and tabs, empty arguments, `""` inside and
  outside a quoted run, trailing backslashes, the program name's rule, an empty and a blank-led line, Turkish and
  supplementary characters, U+FF02 kept as itself, unpaired surrogates, a moved object, and POSIX's argv in place.
- **The real binary with a Turkish name:** `ceridc screen --name "KUZEY DENİZ TİCARET A.Ş."` writes the in-process
  report byte for byte and finds the listed party. On Windows the test hands cmd.exe the command as UTF-16
  (`_wsystem`), because `std::system` would cross the code page before ceridc ever ran. This case has no hosted
  evidence until a Windows run is allowed to finish.
- **Mutations, ten, each caught:**
  - in the splitter: no `""` rule, backslashes not halved, odd backslashes not escaping, quotes not grouping the
    program name, tabs not blank (the split never ends: caught as a killed process);
  - in the transcoder: no length check before reading a trail surrogate, no upper bound on the trail range, no
    U+10FFFF bound on encoding, a malformed byte that swallows the rest, an off-by-one fit;
  - the first form of the length-check mutation was refused by the compiler (an unused parameter), so it was
    restated to stay compilable and was caught.
- **Python guards:** the capability registry (74 capabilities), the environment reads, Allman braces, the registers
  (the crypto inventories current) and the generated sources pass.

The Windows halves (`GetEnvironmentVariableW` with its page buffer, `GetCommandLineW`, `_wsystem`, the platform's
`wide_text.hpp` and the DX12 and module-name call sites) compile only on the hosted Windows lanes. Every one of them
is a mechanical call of the transcoder that the Linux lanes test.

## Remaining in COMPLY.1

- **User action:** approve the download of the official lists' first read (OFAC's SDN and consolidated lists, the EU
  financial sanctions file, the UN Security Council consolidated list, the UK sanctions list, Turkey's asset-freeze
  decisions). The importers to `cerid-sanctions/1` and the dated embargo regimes land with that read.
- **User actions:** approve the first read of the instruments, and have counsel review the eight classifications.
- The screening over MCP, after [MCP-ADAPT](../ROADMAP.md#slice-mcp-adapt) (track A).
- The row is Done once [COMPLY.0](../ROADMAP.md#slice-comply.0) is Done.
