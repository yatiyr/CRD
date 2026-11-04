# 2026-11-04: HESAP-NET, the erasure fuzz target and the ISA-L oracle (Track C batch 95)

<!-- doc-role: evidence -->
> Dated evidence; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [HESAP-NET](../ROADMAP.md#slice-hesap-net) (Partial). CI first: nothing to audit. 41766487849 (33858212) was
cancelled by a newer push; dispatches 41754591577 (13 jobs passed, none failed) and 41763564869 are still running.

## What landed

- `crd-fuzz-hesap-erasure` (tests/numerics/hesap-comms/fuzz): decode and encode requests and kernel requests. Its
  oracles: the refusal order (now stated in erasure.hpp) restated; every encoded block held to the n - k checks of
  RFC 5510's dual code (sum_j u_j x_j^m c_j = 0) with a shift-and-add multiply; every decoded symbol equal to its
  source; no byte written by a refusal or past a symbol; every runnable kernel level, at any alignment, against a
  shift-and-add dot product. 24 seeds; registered in scripts/fuzz.py and test-instruments.md.
- ISA-L 2.32.1 pinned (cmake/pins.json `isa-l`, commit 7c3479e0, SHA-256 591b0d3e..., 843144 bytes; BSD-3-Clause,
  read from the archive), gated by `CRD_BUILD_ERASURE_VS_ISAL` (on in linux-gcc-public-checks). Its
  erasure_code/ec_base.c is `crd-isal-base-peer`; `crd-hesap-erasure-isal-tests` holds to it all 65536 products and
  255 inverses, GM = V(k,k)^-1 V(k,n) formed with ISA-L's gf_mul and gf_invert_matrix (13 shapes up to (255, 255)),
  every repair symbol against `ec_encode_data_base` over GM's columns (5 lengths), and 8 recovery patterns per shape
  against `ec_encode_data_base` over ISA-L's inverse of the received columns. ec_base.c confirms ISA-L's field is 0x11D.

## Evidence

- win-debug: the corpus replays (24 inputs). Teeth: a wrong decode weight fails the lost-source oracle; a wrong
  nibble in the scalar kernel fails the dual-code oracle; both restored.
- linux-gcc-public-checks (WSL): the four ISA-L cases, the erasure cases, the kernel arms at portable to avx512 and the
  corpus pass (23 + 10 CTests), no warnings; its 19 `crd-check`/`crd-no-` guards pass with the peer compiled.
- linux-clang-fuzz (WSL), `fuzz.py run`: 6 jobs, 582 s, 302k executions, 3 findings (two dual-check failures, one
  SEGV) that pass replayed alone three times and in one process over all 1652 grown inputs twice; in the same hour
  clang 18 crashed three times on different hesap-fft files (stack smashing, codegen SEGVs), each compiling on
  rerun: this host's recorded misexecution under load. Rerun once at 2 jobs: 408 s, 356k executions, no finding.

## Next

The throughput budget against ISA-L's NASM `ec_encode_data` (pin a Linux NASM), the oracle against OpenFEC's RFC 5510
codec (openfec.org reachable), then FlexFEC (RFC 8627), RaptorQ (RFC 6330) and the interleavers.
