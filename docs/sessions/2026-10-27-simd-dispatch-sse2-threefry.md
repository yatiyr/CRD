# 2026-10-27 — SIMD-DISPATCH: the SSE2 Threefry body and the scalar Threefry rounds

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). Decision: [ADR-0134](../decisions/0134-runtime-cpu-dispatch.md).

## CI read

- Complete run [40434280766](https://github.com/yatiyr/CRD/actions/runs/40434280766) (`d6528387`, after C2) finished
  green on all 26 jobs; win-shipping and win-relwithdebinfo finished since the last read. Every other completed run
  since was cancelled with no failed job; push run 40455017769 (`09ba6a3a`) and 40458470799 (`c65799a6`) are in
  progress. Nothing was dispatched.
- Needs CI audit, from the job logs: on all 20 test lanes (11 Windows, 9 Linux) the after-C2 flight clauses of
  [DIAG.12](../ROADMAP.md#slice-diag.12) pass: `flight record: a restricted event or trigger keeps its class and
  scalars, never its text`, `crd-fuzz-perf-flight-corpus`, the `flight rings:` and `flight recorder:` cases (6 and 5
  where asserts are compiled, 5 and 4 on the shipping and release lanes, whose two missing cases sit under
  `#if CRD_ENABLE_ASSERTS`). The wild-read case passed on win-shipping (7,739 tests) and win-relwithdebinfo (7,928).
  [DIAG.11c](../ROADMAP.md#slice-diag.11c) and DIAG.12 stay Needs CI: they follow
  [DIAG.11b](../ROADMAP.md#slice-diag.11b), which waits on the user's re-record (8).

## Mechanism

- `threefry4x64` ran its 20 rounds in a runtime loop, so every rotation was a shift by a register, and a portable or
  SSE2 CPU ran 5.3 ns/u64. `counter_rng_core.hpp` now unfolds the rounds and injections by template on G independent
  blocks (`threefry4x64_rounds_of<G>`), still constexpr and intrinsic-free; the single-block function is its G = 1
  case, so `ThreefryRng::next_u64` gains as well.
- The blocks unfold by template recursion, not a loop: GCC 13 at -O3 vectorized a loop over three blocks into shuffled
  128-bit registers (330 `punpck` in one step, 3.7x slower). An empty-asm register pin made it worse (19.8 ns/u64: the
  block array was no longer scalarized) and was dropped.
- The kernel adds the key to the counters itself (`threefry_block_start`), so the words the blocks of a step share
  fold together; word 0 alone moves unless it carries, and a carry takes the 256-bit `threefry_counter_add`.
- `ThreefryAvx2` and `ThreefrySse2` give one step template (`threefry_step<Isa, G, GS>`) its operations: G vector
  groups, GS scalar blocks interleaved round by round, so the vector and the integer ports both work. SSE2 rotates by
  32 with `pshufd`, by 16 with `pshuflw`/`pshufhw`, otherwise by shifts (no `pshufb` before SSSE3).
- Fill: AVX2 three groups of four (12 blocks), then one group; SSE2 two groups of two beside three scalar blocks (7);
  then three scalar blocks at a time and single blocks, which is the portable body.

Variant probe (scratch, 150 KiB per fill, best of 7, ns/u64; GCC and Clang `taskset -c 4`, cl pinned to core 4):

| Body | GCC x86-64 | Clang 18 x86-64 | cl | GCC v3 | Clang v3 | cl AVX2 |
|---|---|---|---|---|---|---|
| HEAD scalar loop | 4.989 | 4.576 | 6.680 | - | - | - |
| scalar, 1 / 2 / 3 / 4 blocks | 1.995 / 1.756 / 1.476 / 1.597 | 2.088 / 1.763 / 1.796 / 2.323 | 1.998 / 1.670 / 1.460 / 1.688 | - | - | - |
| vector, 1 / 2 / 3 / 4 groups | 2.097 / 1.724 / 1.644 / 1.671 | 2.078 / 1.739 / 1.658 / 1.603 | 2.205 / 1.896 / 1.729 / 1.876 | 1.025 / 0.843 / 0.820 / 0.828 | 1.044 / 0.858 / 0.820 / 0.833 | 1.125 / 0.938 / 0.957 / 1.628 |
| 2 groups + 2 scalar | 1.187 | 1.841 | 1.422 | 0.797 | 1.170 | 0.997 |
| 2 groups + 3 scalar | 1.186 | 1.624 | 1.395 | 0.751 | 1.029 | 1.033 |
| 3 groups + 2 scalar | 1.287 | 1.645 | 1.526 | 0.742 | 0.914 | 1.022 |

Two groups beside three scalar blocks is within 1.3% of each compiler's best at SSE2; on AVX2 three pure vector
groups are the best or near it on all three, where a mix gains on GCC only.

## Verification

The differential case adds twelve counters whose word 0 wraps at blocks 4 to 15, inside every vector group and scalar
block of a step; the KAT case adds Random123's all-ones and pi-digits vectors (checked first against HEAD's function)
and evaluates the zero vector at compile time. A scratch A/B of the new `threefry4x64` against HEAD's loop matched
400,000 random blocks (every seventh across a word-0 carry).

| Tooth | win-debug | linux-gcc-debug |
|---|---|---|
| SSE2 store swaps a group's two blocks | differential at sse2 (every arm) | the same |
| a step's second vector group starts one block late | differential at sse2 and avx2 | - |
| a vector group takes the fast lanes across a carry | differential at sse2 and avx2 | - |
| a block past the fourth ignores a carry | differential at sse2 and avx2 (only the new counters reach it) | the same |
| SSE2 rotation by 16 rotates right | differential at sse2 | - |
| the three-block scalar step skips a counter | differential at every level | - |
| a rotation constant off by one | KAT case (12 of 13 assertions) | - |

Each restore was rebuilt and passed. Both lanes pass crd-hesap-stats-tests (141 cases) and the five
`crd-hesap-stats-kernels-at-<level>` arms; strict tidy (which runs every level's entry of a kernel unit, checked with a
finding planted in the AVX2-only code), clang-format (no finding above HEAD's) and Allman are clean.

## Board

linux-gcc-release, forced `CRD_CPU_LEVEL`, `taskset -c 4`, seven interleaved rounds (medians), host idle before and
after. The HEAD binary links HEAD's three stats kernel objects and bench object, compiled from the lane's commands
with HEAD's `counter_rng_core.hpp` first on the path, into the same archives. `bench_rng` (4 Mi u64 per fill):
HEAD `4b3c7d1d`, new `1d1d22b5`; the L2 rows time the kernel table on 150 KiB.

| Routine, ns/u64 | portable | sse2 | avx2 | Peer (same host) |
|---|---|---|---|---|
| Threefry fill, HEAD | 5.274 | 5.271 | 1.149 | MATLAB R2026a threefry4x64_20 3.661 / 3.706 ns/double |
| Threefry fill, new | 1.854 | 1.323 | 0.914 | |
| Threefry, L2, HEAD / new | 4.927 / 1.760 | 4.906 / 1.243 | 1.098 / 0.860 | |
| Philox fill, HEAD / new | 2.523 / 2.503 | 1.644 / 1.635 | 0.949 / 0.956 | MATLAB philox4x32_10 6.17 to 10.96 |

Threefry is 2.84x HEAD on a portable CPU, 3.98x on an SSE2 one and 1.26x on AVX2; MATLAB (the only Threefry4x64 peer
here) is 2.0x behind the portable body and 4.0x behind avx2. The sse2 body is 1.45x behind avx2 (4.6x before).
Philox is unchanged. MATLAB's threefry row was 7.02 on the 2026-10-27 stats-kernels board and 3.66 to 3.71 twice
today.

## Left in the row

The fft, dense, sparse, special and geometry kernels as per-level units first, then the rest the row lists.
