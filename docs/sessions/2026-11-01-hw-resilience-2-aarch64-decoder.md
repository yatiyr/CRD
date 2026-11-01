# 2026-11-01 — HW-RESILIENCE.2: the AArch64 decoder

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HW-RESILIENCE.2](../ROADMAP.md#slice-hw-resilience.2). Before: [code that cannot fault](2026-11-01-hw-resilience-2-instruction-access.md).

## CI read

Complete-tier run 41254049879 (`31404ec4`, in progress) failed linux-gcc-shipping (job 124693382915): GCC's LTO link
of crd-compression-tests stopped on `-Werror=null-dereference` at `test_deflate.cpp:755-756`, the zlib header test
copying the encoded stream into an empty array and then writing its two header bytes (an empty stream leaves the
array null). Reproduced on the local linux-gcc-shipping lane, fixed in 95ec6965 (the stream now starts with the two
replaced bytes), rebuilt clean on that lane and on win-debug; `[deflate]` passes on both. Every other completed run
since the last read was cancelled by concurrency with no failed job.

Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) need a green
complete-tier run; 41254049879 cannot be one (the link above), so the next one is dispatched at or after 95ec6965 when
it ends. The Firebase Test Lab variable names are present; the AMD and Mac cloud lanes wait on their accounts.

## The decoder

`instruction_access_a64.cpp` (crd-perf, beside the x86-64 decoder; `instruction_access_detail.hpp` holds what both
share) reads the little-endian word and its top-level group:

- **Loads and stores**, by bits 29:24: exclusives, ordered, CAS and CASP; FEAT_LSUI's LDTXR, STTXR, CAST, CASPT; SIMD
  multiple and single structures (LD1R to LD4R, FEAT_LRCPC3's LDAP1 and STL1); literal; the 011001 class (RCpc
  unscaled, LRCPC3 pairs and writeback forms, GCSSTR, LSE128, RCW, LSUI atomics, MTE tag loads and stores); SIMD
  LDAPUR and STLUR; pairs (LDPSW, STGP, LSUI's LDTP and STTP); immediate, unprivileged, register-offset (UXTW, LSL,
  SXTW, SXTX) and unsigned-offset forms; LSE, RCW, LDAPR, LS64 and FEAT_LSFE atomics. Rn 31 is SP; Rm and Rt 31 are
  XZR. A store with writeback in LRCPC3 stores below its base.
- **Branches and system**: B, B.cond, BC.cond, CBZ, TBZ, FEAT_CMPBR, BR and its keyed forms, SVC, BRK, HLT, hints,
  barriers, PSTATE, WFET, MRS and MSR make no access; DC ZVA, GVA, GZVA write, and DC CVAC, CVAU, CVAP, CVADP, CIVAC,
  CVAOC, CIVAOC and their tag forms and IC IVAU read or write, the aligned 2 KiB around the address (DCZID_EL0 and
  CTR_EL0 are not captured; the architecture bounds both at 2 KiB).
- **Data processing** (immediate, register, SIMD and FP) makes none.
- **Refused**: SVE, SME, FEAT_MOPS, LDRAA and LDRAB, BL, BLR, RET, RETA*SPPC and the GCS SYS operations (their GCS
  access at GCSPR_EL0 is not captured), ERET, system instructions, unlisted encodings and cut words.
- **Top Byte Ignore**: every range keeps address bits 55:0 (`kAArch64AddressBits`), and the classifier compares a
  fault's address in the same 56 bits, since Linux clears a tag from `si_addr` and Windows' FAR may keep it.

## Evidence

The load and store group was swept against LLVM 20.1.8: every word with bit 27 set and bit 25 clear, Rt = 0, once
with Rn = x1 and once with Rn = SP (1,048,576 each), assembled with `.inst`, disassembled by `llvm-objdump
--mattr=+all` and compared with a scratch driver over `decode_a64` by an independent oracle that derives the address
and size from the disassembly text. Per pass: 613,222 known instructions decode to exactly that range, the 7,603
(SP: 7,699) MOPS, PAC and EL1 tag forms are refused, 401,603 (SP: 401,507) unallocated words are refused, and
26,148 that LLVM rejects are decoded: each is a CONSTRAINED UNPREDICTABLE register constraint (CAS and CASP with an
Rt2 field other than 31, odd CASP, CASPT and RCWCASP pairs, LSE128 and RCW pairs naming XZR), decoded as the access
they make when they execute. A branch and system sweep (1,188,076 words) and 400,000 random words found no word decoded as making no
access whose disassembly names a memory operand. The test cases' encodings come from `llvm-mc`.

win-debug: `crd-perf-tests "[hw-resilience]"`, 33 cases, 405,358 assertions pass, among them 65 AArch64 accesses,
13 forms with no access, 12 refusals, cut words, and the classifier's top-byte check on an ARM64 dump (tagged and
untagged fault addresses consistent, one byte past the load `access-not-made`). linux-gcc-debug: the instruction,
classification and wild-write cases pass. Teeth (broken, rebuilt, failed, restored, rebuilt, passed): SXTW read as
UXTW (the register-offset case fails), the classifier comparing all 64 bits (the tagged case fails).
The win-debug guards pass; `crd-check-capability-owners` stopped once on CPython's "internal error in regular
expression engine" (this host's known misexecution) and passed when run again.

Hosted, once pushed: the wild-write case's AArch64 branch now expects its real capture to decode `consistent` and,
moved by 0x1000, `access-not-made`, on the ARM64 lanes (linux-gcc-arm64-debug, linux-clang-arm64-debug,
win-arm64-debug).
