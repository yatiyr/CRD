# 2026-10-29 — SIMD-DISPATCH: specimen null stores GCC deleted, the probe's level namespace, ARM64 debug lanes

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch);
contract: [ADR-0134 §6](../decisions/0134-runtime-cpu-dispatch.md);
follows [the frame arena ownership fix](2026-10-29-jobs-frame-arena-ownership.md).

## CI first

Scheduled run 40849112570 (fea7ee0a) is still running; its completed failures were read from their logs.

- Every lane: `crd-generated-sources` and the provenance case of `crd-repository-tools` (the refs manifest), fixed at
  HEAD by 74a95125.
- linux-gcc-release and linux-gcc-relwithdebinfo: seven and eight Linux crash-capture cases and the diagnostic host's
  wild-write case saw exit 0 and no record. d44c6730 had made the specimens' pointee non-volatile for Clang's UBSan,
  and GCC then deleted the store (below).
- The same two lanes: `crd-check-isa-isolation` failed 159 times (140 to 146 s), all in `crd-math-tests`: the SIMD
  probe kept its helpers in an anonymous namespace outside the level namespace, so the image route could not attribute
  their VEX code. It is the only kernel unit that did; it now follows the other units.
- The push runs since 01443d86 were replaced before any job started (0 jobs each). Push run 40826432863's failures
  were all traced by earlier batches.

DIAG.11c and DIAG.12 stay Needs CI: both wait on the final complete-tier run.

## The deleted null store

A volatile local keeps its load, not its value: GCC's points-to analysis reads the stored null straight through,
finds that `*p = 42` writes no object and deletes it in `dse1` (`-O2`, `-O3` and `-flto` alike; `-fno-tree-pta`
keeps it). An integer address read from a volatile local is deleted the same way. Probed with GCC 13.3, Clang 18.1
and MSVC (VS 18):

| form | GCC -O2/-O3/LTO | Clang -O2/-O3/LTO | UBSan (both) | MSVC /O2 /GL |
| --- | --- | --- | --- | --- |
| `int* volatile p` | survives (store deleted) | SIGSEGV | reports | access violation |
| empty asm `"+r"(p)` | SIGSEGV | SIGSEGV | reports | (no inline asm) |

`crd_diag_opaque` (specimen_common.hpp) is the one form: the empty asm on GCC and Clang, a volatile local on MSVC.
Both null-store specimens use it; `crash_specimen.cpp` keeps its volatile pointee, since its route disables UBSan's
null check on purpose.

## ARM64 debug lanes on real NEON hardware

In the same run, linux-gcc-arm64-debug (123363958993), linux-clang-arm64-debug (123363959290) and win-arm64-debug
(123363958999) passed every test but the refs manifest pair: 2475 and 2476 of 2477 and 2478, the portable and neon
arms included, and the ISA guard (31, 30 and 23 s). The ARM64 ASan lanes are still running.

## Local checks

win-debug: crd-math-tests `[simd]` and `[simd-levels]`, the two wild-write cases, tidy, the ctest guards. One guard
run failed `crd-check-environment-reads` without its output captured; it passed alone and in a full parallel
rerun, so it is recorded as a host event (as in [the minimax fit](2026-10-29-simd-dispatch-minimax-coefficients.md)).

## Left

- A fully green ARM64 job at or after 74a95125, the ASan lanes included.
- The first green guard run on the GCC IPO lanes, which this fix makes possible.
