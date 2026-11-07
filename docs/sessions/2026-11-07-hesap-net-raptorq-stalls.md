# 2026-11-07: HESAP-NET, RaptorQ's AVX-512 gap isolated and closed

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (stays Partial). Track C batch 111. Board:
[RaptorQ stalls](../bench/2026-11-07-hesap-net-raptorq-stalls.md).

## CI first

Nothing new from this track. Push run 42343637429 (229b06b2) has only `crd-fuzz-hesap-motion-corpus` failed on the clang ARM64
lanes, older than its fix 57c19b1e. Dispatch 42360379686 (8e86cf8f) is still running: 13 jobs passed, 19 not started or running.

## What landed

- The cause of the planned rows' 0.79x and 0.96x, by named PMU events on a real Xeon 8581C: `rep movsq` and `rep stosq` before wide
  loads (store-forward blocks), then line splits once those were gone.
- `ErasureKernels::sum`: up to 16 sources in one pass, each output vector stored once, at every level, known-answer checked. `add`
  left the table and is a one-source `sum`. RaptorQ's rows, its output symbols and FlexFEC's runs of equal length use it.
- raptorq.hpp names the cost of unaligned symbols; the board puts hesap's on 64-byte lines. Any alignment still works.
- The board takes `--k` and `--planned`, for a profile that holds one row only.

## Evidence

- Every row now leads: on AVX-512 1.26x to 1.93x over the best peer (1.26x to 1.34x at planned K = 10, was 0.79x), on the
  i9-14900K 1.22x to 2.05x in the clean run. FlexFEC 2.41x to 3.55x (was 1.92x to 3.35x).
- linux-gcc-release, levels native, avx2, sse2 and portable: `crd-hesap-net-tests`, `crd-hesap-raptorq-peer-tests` (nanorq and the
  raptorq crate), `crd-hesap-flexfec-webrtc-tests` (libwebrtc) all pass, bytes unchanged.
- 19 mutations of the kernel, solver and FlexFEC callers: all caught. Two survived at first because the tests' memory was zero, so
  `solve` now runs on garbage intermediate and scratch memory in every test, which caught them.
- libFuzzer on linux-clang-fuzz, 100 s each with the sum in `crd-fuzz-hesap-erasure`'s kernel requests: erasure 8,978, raptorq 171,057
  and flexfec 500,471 executions, no artifact.

## Cost

One spot c4-standard-8, created with `bench_vm.py create --once 2`, 43 minutes, about 0.12 USD by list price (not a billed figure),
deleted with its disk this batch; raw files were fetched first. Only the stopped launcher remains.

## Next

Read 42360379686 for the row's four remaining peers and the fuzz fix, then close HESAP-NET; NET.3.
