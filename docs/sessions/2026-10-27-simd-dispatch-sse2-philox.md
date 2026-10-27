# 2026-10-27 — SIMD-DISPATCH: the SSE2 Philox block body

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- Completed runs 40424260124 to 40451010209 were cancelled with no failed job (40451010209 ran none). Push run
  40455017769 (`09ba6a3a`) is pending; nothing was dispatched.
- Complete run 40434280766 (`d6528387`, after C2): every job but win-relwithdebinfo has finished, all green;
  win-shipping finished since the last read.
- Needs CI audit: [DIAG.11c](../ROADMAP.md#slice-diag.11c) and [DIAG.12](../ROADMAP.md#slice-diag.12) stay Needs CI:
  they require [DIAG.11b](../ROADMAP.md#slice-diag.11b), which waits on the user's re-record (8).

## Mechanism

- `philox_blocks.hpp` gains `philox4x32_x4<G>`: four blocks per 128-bit register on `pmuludq` (both product halves from
  two multiplies, merged by `pshufd` and `punpck`), the 4x4 transpose and unaligned stores. The fill runs G = 4
  independent groups (16 blocks) at a time on the SSE2 backend, then single groups, then the scalar loop.
- One group is latency-bound: its ten rounds form one dependency chain, so the 128-bit body alone was only 1.10x the
  scalar loop (the scalar loop gets both halves from one 64-bit `imul`). Independent groups fill the pipeline, and the
  AVX2 body takes the same lever: `philox4x32_x8<G>` runs three groups (24 blocks), then single groups of eight, and
  its four-to-seven-block tails now run the 128-bit body.
- The stats and tensor families build an sse2 level (`LEVELS portable sse2 avx2`); the tensor unit's other sse2 entries
  are its portable loops until their own bodies land. No sse4.2 unit: the SSE4.1 `pblendw` merge (the AVX2 body's
  form) measured the same as the SSE2 merge, so an SSE4.2 CPU runs the sse2 level.

Variant board (scratch probe, `taskset -c 4`, best of 7, ns per u64; cl unpinned):

| Body | GCC x86-64 | GCC x86-64-v2 (`pblendw`) | cl |
|---|---|---|---|
| scalar | 2.494 | 2.532 | 2.618 |
| 128-bit, 1 group | 2.243 | 2.213 | 2.262 |
| 128-bit, 3 groups | 1.626 | 1.661 | 1.727 |
| 128-bit, 4 groups | 1.607 | 1.618 | 1.646 |
| one word per 64-bit lane, 6 registers | 1.666 | 1.694 | 2.533 |
| 128-bit group plus 2 scalar blocks | 1.746 | 1.762 | 2.112 |
| AVX2 1 / 2 / 3 groups | 1.165 / 1.008 / 0.951 | - | 1.163 / 1.110 / 1.057 |

## Verification

The stats and tensor Philox cases now run 0 to 39 blocks (24 + 8 + 4 + 3 and 16 + 16 + 4 + 3 reach every loop of both
bodies), and both "archive holds" cases check the sse2 table on x64.

| Tooth | win-debug | linux-gcc-debug |
|---|---|---|
| 128-bit body stores blocks 1 and 2 swapped | stats and tensor Philox cases (every arm), `[dtypes][kernels]` batch converts, `[sketch]` at sse2 and sse4.2 | the same arms |
| AVX2 third group starts one block late | the same cases at avx2, `rng: SIMD bulk fill` | - |
| fourth 128-bit group starts one block late | stats and tensor Philox cases, `[sketch]` at sse2 and sse4.2 | - |
| stats family built without sse2 | "archive holds" case | - |
| tensor family built without sse2 | "archive holds" case | - |

Each restore was rebuilt and passed. Both lanes pass crd-hesap-stats-tests (141 cases), crd-hesap-tensor-tests (44),
decomp (9), permute (12) and the 18 nn cases, every stats and tensor forced arm and `smoke_hesap_tensor`; the win-debug
guards pass. Strict tidy, clang-format (no file above HEAD's count) and Allman are clean.

After the tooth that removed a family's sse2 level was restored, the first win-debug incremental link of that
family's test program failed with `LNK1236` (corrupt COFF sections) in the re-added kernel object; the next link of the
same inputs succeeded, for stats and tensor alike. It follows the incremental linker, not the source; recorded, not
chased.

## Board

linux-gcc-release archives, forced `CRD_CPU_LEVEL`, `taskset -c 4`, seven interleaved rounds (medians), host idle. The
HEAD binaries link the same program objects and archives with the stats avx2 and tensor avx2 units recompiled from the
lane's commands with HEAD's `philox_blocks.hpp` first on the path. Binaries: bench_rng `df75a9b1`, HEAD `44fd6b0d`;
bench_decomp `08b11934`, HEAD `52ecfa82`. Before this batch an SSE2 CPU ran the portable column.

| Routine | portable | sse2 | HEAD avx2 | avx2 | Peers (same host) |
|---|---|---|---|---|---|
| Philox fill, ns/u64 | 2.528 | 1.661 | 1.247 | 0.951 | NumPy 2.4.6 Philox 2.603 |
| hooi_rand 64^3 rank 16, ms | 14.99 | 11.70 | 10.14 | 9.04 | TensorLy 0.9.0 15.66 |
| hooi_rand 32^4 rank 8, ms | 87.81 | 65.32 | 55.00 | 48.35 | 102.56 |
| hooi_rand 128^3 rank 32, ms | 128.52 | 107.04 | 95.72 | 89.21 | 203.29 |

The fits are identical on every arm. The sse2 fill is 1.52x the portable loop and 1.57x NumPy; avx2 is 1.31x HEAD and
2.74x NumPy. hooi_rand at sse2 is 1.20x to 1.34x portable and 1.34x to 1.90x TensorLy (the previous board's peer
rows); avx2 1.07x to 1.14x HEAD. Threefry is unchanged (HEAD 1.205, new 1.153 ns/u64 at avx2; portable 5.321, sse2
5.306): its SSE2 body is the next lever for the stats family.

## Left in the row

The SSE2 Threefry blocks first (portable 4.6x behind avx2), then the rest the row lists.
