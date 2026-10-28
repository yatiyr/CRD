# 2026-10-28 — SIMD-DISPATCH: the eylem-rigid3d zmm body

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `5a55dd4d`.

## CI first

No completed run has a failure left to fix. Complete run 40649898318 (`3e849d96`) is still in progress (win-asan
running). Its three finished failures all have known causes:

- **win-clang-cl-shipping:** the fiber-keep stream race, fixed in `6210ab6f`.
- **linux-gcc-debug-scalar:** the pinned download read timed out, fixed in `137b89f1`.
- **linux-clang-tsan:** five tests. Three are TSan races at `worker_pool.cpp:165` in `job_fiber_trampoline`: the
  generalized LOBPCG and momentum moats and a fiber-keep test. The other two crash without a race report.
  `counter_wait: two sequential waits on renewed counter` dies with a bare SIGSEGV. `RingAllocator concurrent
  producers` (which runs on the fiber pool) gets a TSan SEGV on a write at `0x7f5470100ff8`, 8 bytes below a page
  boundary. That is the shadow-stack underflow from
  [2026-10-06](2026-10-05-clang-werror-and-tsan-inventory.md#second-complete-run-2026-10-06): an out-of-line switch
  hook pops one frame too many, and the fiber's first entry writes below its mapping. All five come from the same
  hooks, which `38b982f2` keeps inline at -O0. That commit is newer than `3e849d96`, and no TSan run after it has
  finished yet (push runs since were cancelled; 40662109393 and the scheduled 40663338177 are queued), so the fix
  still needs its hosted run.

DIAG.11c and DIAG.12 still wait on a finished complete-tier run.

## The body

`rigid3d_kernels.cpp` writes the body step once over a register type. A run hoists its constants and steps one chunk
of eight on Vec8f, or two on Vec16f at avx512 (chunk c in the low half, c + 1 in the high one, each column two aligned
256-bit loads, since the chunks are not adjacent in memory), with an odd last chunk on Vec8f. `BodyChunk` and
`kChunkLanes` are unchanged: one layout on every level (ADR-0062 §4). Every product and sum rounds on its own, the
division and square root are IEEE (`vdivps`, `vsqrtps` on zmm), and the lanes are independent, so every level gives
the portable bits.

## Tests

- The existing `[kernels]` case runs 0 to 5 chunks with guard chunks after them, so it covers the pair step, the odd
  Vec8f chunk and that nothing past `count` is touched.
- win-debug `[kernels]`: 165 assertions in 3 cases at portable, sse2 and avx2.
- SDE 10.13.1 `-skx`, `CRD_CPU_LEVEL=avx512`, `[kernels],[cpu-level-applied]`: 196 assertions in 4 cases, avx512
  included.

## Board candidate (row close, on AVX-512 hardware)

The two-chunk step against avx2's one.

## fft at avx512

After this body, hesap-fft is the only family whose avx512 unit still runs the avx2 code (compiled at x86-64-v4).
The [types note](2026-10-28-simd-dispatch-avx512-types.md) said that no zmm body is planned for fft. That was a
judgment in a session note, not a user decision, and the user's order names AVX-512 bodies for every family. So the
fft zmm codelets stay in the row's Left list: the complex helpers on Vec16f and Vec8d, and the generators
(`gen_fft_batched.py`, `gen_fft_codelets.py`) emitting a zmm edition.
