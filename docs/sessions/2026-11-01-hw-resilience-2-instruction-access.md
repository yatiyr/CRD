# 2026-11-01 — HW-RESILIENCE.2: the faulting instruction's accesses and divide operands

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2). Before: [code bytes](2026-11-01-hw-resilience-2-code-bytes.md).

## CI read

No completed run has a failed job (push runs 41254371565 to 41261715955 were cancelled by concurrency). Complete-tier
run 41247045494 (`67977fda`) is still running; its failed jobs (clang-tidy, win-shipping, win-shipping-profile,
win-asan, win-release, win-public-checks, win-debug, win-relwithdebinfo, win-debug-scalar) all stop on track C's
`test_ceridc_screen.cpp` C2797, fixed by d9be0cfd, which the queued complete-tier run 41254049879 (`31404ec4`) carries.
The Firebase Test Lab variable names are present; the AMD and Mac cloud lanes wait on their accounts.

Needs CI audit:

- [HW-RESILIENCE.1](../ROADMAP.md#slice-hw-resilience.1) closes on run 41247045494 (after the close-out, 0fc32e70):
  jobs 124672594118 (win-arm64-debug), 124672594192 (linux-gcc-arm64-debug) and 124672594115
  (linux-clang-arm64-debug) passed with no junit failure, and each one's unforced `checksum kernels: the process runs
  the levels its CPU selection resolves` printed `checksum kernels ran: crc neon +crc32 +pmull, hash neon` (the forced
  arms: `crc portable, hash portable` and `crc portable, hash neon`). The run's red Windows jobs are the C2797 above.
- [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) wait on 41254049879.

## Code that cannot fault

- **Registers** (`CrashEvidence`): a dump's processor architecture (system information, stream 7) and the exception
  stream's thread context (AMD64 or ARM64 CONTEXT, trusted only with CONTROL and INTEGER set and its pc equal to the
  exception's); a record's named registers, `efl`, `trapno` and `err`. The access kind: an access violation's first
  parameter (read, write, execute), a record's page fault error code (W, I/D). The Windows tests pin every CONTEXT and
  exception-stream offset against the SDK's structures with `static_assert`.
- **Decoder** (`crd/perf/instruction_access.hpp`, x86-64): legacy prefixes, REX, VEX and EVEX; one-byte, 0F, 0F38 and
  0F3A maps with exact lengths (RIP-relative counts from the end, immediates included); ModRM and SIB with r12, r13,
  the no-base and no-index forms and 32-bit address size; implicit stack, string (REP by rcx, DF downward, a zero
  count touching nothing), XLAT, absolute, bit-offset, cache-line and masked-move accesses; x87, SSE, VEX and EVEX
  bounded by the vector length, EVEX's scaled 8-bit displacement as one range per factor. Refused with the reason:
  FS and GS bases, VSIB, AMX tile loads, APX registers, near calls and returns (SSP is not captured), XSAVE areas,
  system and I/O instructions, vendor-dependent lengths, XOP, 3DNow!, unlisted opcodes, cuts and 16-byte encodings.
- **Divide**: `x86_divide_faults` decides DIV and IDIV by comparison (an unsigned quotient fits when the high half is
  below the divisor; a signed one when |dividend| is below (2^(n-1) plus one if negative) times |divisor|).
- **Signatures**: `access-not-made` (a page fault on an address outside the instruction's own bytes and every range,
  or of a kind it does not make) and `divide-without-cause` (a divide error on another instruction, or a register
  divisor that is nonzero with a quotient that fits); judged only when the code's image comparison matches, never on
  a protection fault, a sent or kernel-sent signal, or a memory divisor. `bundle.inspect` reports the instruction set,
  registers and access with the crash, and the check, reason, length and access count with the classification.

## Evidence

win-debug, `crd-perf-tests "[hw-resilience]"`: 31 cases, 404,748 assertions pass. The 52 decoded encodings, the
refused ones and the divide forms were each checked once with LLVM 20.1.8's `llvm-mc --disassemble` (each consumed
whole as the instruction named; `62 f9 ...` reads `[r17]`, an APX register), and every cut of each is refused. The divide decision matches plain arithmetic exhaustively at 8 bits, over 200,000
sweeps at 16 and 32 bits and at the 64-bit boundaries; the 128-bit sweep (GCC and Clang lanes) agreed on 2,000,000
cases in a WSL run with both sides of each limit reached. The application's real wild-write dump decodes with its
registers to the store that faulted (`consistent`) and, reported at another address, to `access-not-made`.

Teeth (each broken, rebuilt, failed, restored, rebuilt, passed): RIP-relative from the instruction's start (3 cases
fail), the negative quotient's extra unit (the 8-bit sweep and 64-bit boundaries fail), a write accepted as any kind
(the write check fails), the AMD64 context read one register off (the register and divide cases fail). Writing the
TEST case found a defect before the push: `F7 /0` with REX.W took an 8-byte immediate; it is 32 bits, and two cases
now hold it. The decoder, the classification and the tests compile clean under GCC and Clang with the project's
warning set (`-fsyntax-only`); tidy-files clean.

Left on the row: the AArch64 decoder (loads, stores, pairs, exclusives, atomics, SIMD structures, literal and
register-offset forms; SVE, SME, MOPS, PAC loads and GCS-writing branches refused; the top byte ignored).
