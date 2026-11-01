# 2026-11-01 — SIMD-PERF: the fft passes' AoSoA blocks stored whole at avx512

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md#slice-simd-perf).

Row: [SIMD-PERF](../ROADMAP.md#slice-simd-perf), Left item 1 (fft's f32 forward where MKL leads), following the
32K and 64K profile's finding that the three-pass shapes are bound by their stores. Board:
[2026-11-01-simd-perf-fft-whole-block-stores](../bench/2026-11-01-simd-perf-fft-whole-block-stores.md).

## CI first

- Every push run on main is still cancelled by the next push; push run 41286750175 (`8311a54e`) was pending.
- Manual dispatch 41269105413 (`b0b1b245`, covering `b0640d0c` to `6b4cf519`): its win-tidy built all four fft
  levels' units, `b0640d0c`'s `transpose8x8_halves` and `de0ae783`'s avx512 pass-3 transpose among them, with no
  finding; its two failures were `filesystem.cpp`'s `min`/`max` macro collision (fixed on main at `9723ac75`) and
  `test_filesystem.cpp`'s `kUnixNs` (fixed at `4b9bc837`). Its Windows build lanes failed on the same
  `filesystem.cpp` collision and win-arm64-debug on `checksum.cpp` (C2220), neither this track's.
- Manual dispatch 41274380605 (`de5e10b0`): win-clang-cl and win-clang-cl-shipping compiled every unit, the fft
  avx512 units with `b0640d0c` and `4fdad24a` among them, and failed only to link `crd-perf-tests` on `__udivti3`
  and `__divti3` (128-bit division at `tests/foundation/perf/test_instruction_access.cpp:515` and `:522`, the serial
  workflow's HW-RESILIENCE file, still on main); win-tidy failed only on `kUnixNs`; linux-gcc-release and
  linux-gcc-shipping only on `test_multivariate.cpp:125`, fixed at `76330344`, which that dispatch predates. No
  completed lane has yet built `bcec86fd` or later.

## Profile first

- **Where the stores came from.** `objdump` of the avx512 `Vec16f` codelets: a step of the 32-point deep-split pass
  2 issued 221 store instructions, 128 for its 64 output registers (a half store and an extract to memory each),
  79 spills and 14 others. At avx512 a Lane of two tiles holds two AoSoA blocks' real halves in one register, so
  `store_block` stored four halves where two whole blocks would do. The spills are 25% above the minimum a Belady
  replacement of the generator's schedule needs with 32 registers (63), so the store halves, not the allocator, were
  the larger share.
- **The bound past the L1d.** HEAD's store buffer was full for 52% of the 8K forward's cycles, 43% of the 32K
  deep-split-3's and 52% to 61% at 128K (`resource_stalls.sb`).
- **The bound in it.** At 2K HEAD dispatched 4,006 port-5 uops in 4,221 cycles a transform: whatever moves the
  passes' blocks there must not add to port 5.

## The change

- `join_low_halves` and `join_high_halves` (`Vec16f`, `Vec8d`): the first an insert (port 0 or 5), the second a
  256-bit lane shuffle (port 5); the SIMD-type probe holds both to portable's bits on every level.
- `store_block` stores a paired lane's two blocks whole (its halves joined); `store_block_halves` keeps each half's
  own store. Stage 1's `store_transposed` takes a group's real and imaginary parts together, its `Whole` choosing
  each column's whole block or its halves. The loads keep their halves: a whole load's lane shuffle moved the
  inserts' port 0 share onto port 5 (candidate c1: 2K 0.89x HEAD, port 5 4,006 to 4,545 a transform).
- The generator picks the form: the in-place passes' 2K and 4K codelets keep halves (the tabled pass 1, which runs
  in place at 2K, pass 2 at B = 8 and 16, and a new `codelet16_inplace_first_halves` the engine runs below
  `kInPlace3WorkFrom`), whole blocks from 8K; stage 1's factored edition (n2 of 1024 and up) and the tiled
  four-step's P1 whole, its full-table edition (the 1K standalone-hier) halves. With whole stores everywhere
  (candidate c2) 2K and 4K lost 5% to 6% on a 64-byte boundary, pass 1 and pass 2 each slower on port 5.
- The same operations on the same values: the two builds printed the same bits at every size on every f32 board.

## Results (c3, kept)

- f32 planned against HEAD: 8K 1.21x aligned and 1.14x off a line, 16K 1.17x and 1.13x, 128K to 512K 1.17x to 1.29x,
  1M 1.12x and 1.14x; 1K to 4K level (HEAD's code), 32K and 64K (IP4-AoS) and 2M and 4M level or unresolved.
- Against MKL: 8K 1.06x aligned and 1.03x off a line (from 0.89x and 0.91x), 16K 1.02x and 1.07x (from 0.87x and
  0.95x); 128K to 1M 1.57x to 2.11x.
- Counts: stores -16% to -27%, store-buffer stalls -34% to -50%, cycles -12% to -22%, instructions level, port 5
  +3% to +6%.
- f64 against HEAD: 128K 1.13x, 256K 1.16x, 512K 1.19x; the rest level or unresolved at the program's 1 us print
  resolution on paths that call none of the changed stores.
- The 32K and 64K deep-splits now lead IP4-AoS by 1.24x to 1.36x at avx512: planned there, 32K would run 0.99x MKL
  aligned and 1.11x off a line, 64K 1.01x and 1.06x.

## Checks

- On the machine (linux-gcc-release, the tree of the kept change): the whole tree built with `-k 0`, no failure; CTest's
  `fft|dsp|stft|cwt|conv|nn|simd|math` cases with the `crd-no-*`, `crd-check*` and diag-coverage guards, 988 of 988
  in 609 s: every `crd-hesap-fft-kernels-at-<level>` and `crd-math-kernels-at-<level>` arm (the SIMD-type probe's
  joined halves among them) and 18 repository guards.
- The codelets regenerated on WSL, the reference host (`gen_fft_batched.py hybrid2`), their manifest entries
  refreshed (`check-generated.py`); locally the plan, repository and Allman checks.
- The machine ran 1.89 h in one start and is stopped.

## Left

- The avx512 plan rows from this machine: the deep-split-4 at 32K and 64K (the canonical plan serves avx2, whose
  code this change left as it was), from an arms board of every size at avx512 through `gen_fft_level_plans.py`.
- The loads' halves (four load uops a complex value a step) where port 5 has room, the 32-point leaves' spills
  (79 a step against 63), and the rest of the 32K and 64K session's list.
