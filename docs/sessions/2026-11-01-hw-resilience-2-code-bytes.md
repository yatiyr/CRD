# 2026-11-01 — HW-RESILIENCE.2: the faulting instruction's bytes against the module's image

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2). Before: [hosted identity](2026-11-01-hw-resilience-2-ci-hosted-identity.md).

## CI read

No completed run has a failed job. Complete-tier run 41247045494 (67977fda, still running) fails clang-tidy,
win-asan, win-release, win-public-checks, win-debug, win-relwithdebinfo and win-debug-scalar on the screen test's
C2797, fixed by d9be0cfd. The queued complete-tier run 41254049879 (31404ec4) carries that fix and the ASan shards;
HW-RESILIENCE.1, DIAG.11c and DIAG.12 are audited against it. The Firebase Test Lab names are present; the AMD and
Mac cloud lanes wait on their accounts.

## Code no store wrote

Half of the row's "faults in code that cannot fault": the capture carries the faulting instruction and an analysis
compares it with the module's own file.

- **Capture** (crd-core): `kCodeWindowBytes` (16; the longest x86-64 instruction is 15 bytes). Windows: the dump
  callback hands DbgHelp the range at the exception address as a memory-list range (where WinDbg reads code), sized
  first with `ReadProcessMemory` (whole window, else up to the page end, else none) and dropped rather than failing
  the dump if DbgHelp cannot read it; the callback now runs for live and fatal dumps, not only snapshots. Linux: the
  record's `code <pc> <hex>` line after the registers, read with `pread` on `/proc/self/mem`, which returns short or
  fails instead of faulting (a jump to an unmapped address) and reads execute-only code.
- **Reading** (crd-perf): `CrashEvidence::code_bytes`/`code_size` from a dump's MINIDUMP_MEMORY_LIST or
  MEMORY64_LIST range holding the pc, or the record's code line for its own pc; every cut reads safely.
- **Comparison**: `compare_code_with_image` through an `ImageReader` (file offsets; nothing loads a whole binary). PE:
  TimeDateStamp, SizeOfImage and CheckSum must be the module's; the pc's section maps it, only file-backed bytes
  count, and the base relocations reaching the window (HIGH, LOW, HIGHLOW, DIR64) are applied for the load
  address; any other relocation type reaching it is not compared. ELF: GNU build-id and PT_LOAD span must be the
  module's; `DT_TEXTREL` or `DF_TEXTREL` is not compared. Result: matches, one bit flipped (signature
  `code-bit-flip`, "possible" alone), modified (a hook or breakpoint changes bytes, never judged) or not compared
  with its reason.
- **Finding the image**: `image_file_path` (symbol_index.hpp) gives the symsrv key of a PE binary or the debuginfod
  `.build-id/<nn>/<rest>` path; `bundle.inspect` looks under `<root>/images` and reports `code_bytes`,
  `code_image`, its reason, the compared count and the first differing byte.

## Evidence

win-debug, `crd-perf-tests "[hw-resilience]"`: 22 cases, 3,958 assertions pass. New cases: both memory-list forms
and every cut, the record line and its malformed forms, a PE image whose `mov rax, imm64` matches only through its
DIR64 relocation (without it the bytes differ widely), one flipped bit, a hook, an unapplied relocation type, another
build, a foreign file, every cut of the image; an ELF image by build-id, the file-backed limit, a wrong span and
text relocations; the sentence; `bundle.inspect` with and without the image in the store. The application's real
wild-write capture (`diagnostic host: the application's wild write ...`) carries the bytes and matches the
specimen's own file on win-debug. The Linux writer compiles clean under GCC and Clang (`-Wall -Wextra -Wconversion
-Wshadow`); the linux lanes run the same case on push.

Left on the row: the instruction decoder (x86-64 and AArch64) and the access and divisor signatures.
