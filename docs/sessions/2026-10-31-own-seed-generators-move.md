# 2026-10-31 — OWN.1: seed generators and the hash combiner move onto crd-core

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the second audit move after the
[FNV-1a move](2026-10-31-own-fnv1a-move.md). Built, tested and measured on `cerid-track-b`.

## The owner

`crd/core/seed_generators.hpp` (capability `random.seed-generators`, design decision 3), all constant expressions:

- `splitmix64_mix`: SplitMix64's finalizer (Stafford's variant 13), a bijective hash of one word;
- `splitmix64_next`: the stateful generator (add the gamma, then mix);
- `splitmix64_at(seed, n)`: output `n` (from 0) of the stream seeded with `seed`, computed directly;
- `mmix_lcg_next`: one step of Knuth's MMIX linear congruential generator, both constants named;
- `kGoldenRatio64`: 2^64 over the golden ratio, the gamma of SplitMix64 and the combiner's constant.

`hash_combine` (capability `hash.combine`) sits beside FNV-1a in `crd/core/fnv1a.hpp` (decision 7). Both capabilities
are `landed` in [cmake/capability-owners.json](../../cmake/capability-owners.json) with their API files and no pending
entry; the sandbox's FNV-1a entry went with them.

## The sites

Every SplitMix64 copy had one of four shapes, each now a composition of the owner's forms with the same bits
(the [design](../design/capability-ownership.md#the-seed-generator-and-combiner-move) lists them):

- the stateful step: `stats::SplitMix64` (now the bit-generator form of `splitmix64_next`, which xoshiro and PCG64
  seed from), ChaCha20's key expansion and the sandbox's box generator;
- one step from a value, `splitmix64_at(x, 0)`: CEIR's seeded input draws, crd-perf's fault-injection draws,
  crd-core's own workload probe (the owner's directory held a copy too) and the count-min sketch's row seeds;
- output `i` of a stream, `splitmix64_at(seed, i)`: hesap-eigen's starting vectors (`splitmix_pm1`, used by Lanczos,
  Arnoldi, Krylov-Schur, LOBPCG, Jacobi-Davidson, FEAST and the partial SVD);
- the finalizer after an xor: containers' `hash_u64`, `splitmix64_mix(x ^ kGoldenRatio64)`.

The MMIX step: hesap-dense's randomized-SVD sketches (two), hesap-fft's sparse-FFT permutation strides and
hesap-iterative's IDR(s) shadow space. The combiner: the LOD policy identity (cooked, bit-exact; its local
`hash_u64` wrapper is gone) and the archetype graph's mask hash.

The sandbox's BVH-viewer fingerprint, a runtime cache key nothing persists, mixed the FNV prime and offset with
SplitMix64 and a MurmurHash3 constant by hand; it is now FNV-1a over the slider tuple (`fnv1a_64_le` per field),
keeping its 0-means-no-cache sentinel. The audit also covered the golden-ratio multiplies that are not SplitMix64
(Fibonacci hashing in the diagnostic allocator, the jobs adapter and the frame template bridge; the `seed` defaults of
hesap-eigen, hesap-opt and IDR(s)): they stay, as constants of their own algorithms.

## Oracles

- `test_seed_generators.cpp` (crd-core-tests, five cases): SplitMix64's published seed-0 stream (0xE220A8397B1DCDAF,
  0x6E789E6AA1B965F4, 0x06C45D188009454F) through both the stepped and the indexed form at compile time; the indexed
  output against the stream for eight outputs of every seed; each of the four shapes written out as the copies had it
  against the owner's composition, over seven edge values and a 4096-value Weyl sweep of the 64-bit range; the MMIX
  step from 0, its wrap at the all-ones state and four steps against the formula; `hash_combine` against the formula in
  both argument orders and its order sensitivity.
- `test_containers.cpp`: `hash_u64(0)` is SplitMix64's first output from seed 0, which pins its bits.
- The moved sites' own suites pass unchanged, among them hesap-stats' SplitMix64 and ChaCha20 known-answer tests and
  the count-min and HyperLogLog cases.

## Before/after board

`runtime/examples/bench_hash.cpp` (a retained bench, `CRD_BUILD_RETAINED_BENCHES`) times each moved function
(`owner`) against the copy it replaced (`reference`, written out in the program) in one process, alternating which
runs first; [ab-board.py](../../scripts/ab-board.py) reads both arms from each run. It also gives the
[FNV-1a move](2026-10-31-own-fnv1a-move.md)'s board, which that batch owed. linux-gcc-release, GCC 13.3, GCE
n2-standard-8 (Intel Xeon @ 2.80 GHz, model 85), CPU 3 pinned, 10 repetitions after one warm-up, band 2%, idle host
before and after (`build-bench.py host`). Times are ns per byte (FNV-1a) or per value; speed ratio = reference over
owner, median with its bootstrap 95% interval:

| case | reference | owner | speed ratio | verdict |
|---|---|---|---|---|
| fnv1a64, 16 B | 1.405 | 1.398 | 1.005 [1.005-1.005] | level |
| fnv1a64, 256 B | 1.257 | 1.256 | 1.001 [1-1.001] | level |
| fnv1a64, 64 KiB | 1.246 | 1.246 | 1 [0.9995-1.001] | level |
| splitmix64 | 1.18 | 1.181 | 0.9996 [0.9989-1.001] | level |
| mmix-lcg | 1.206 | 1.205 | 1 [0.9996-1.003] | level |
| hash-combine | 1.35 | 1.35 | 1 [0.9996-1.001] | level |

Every cell is level: the owner's loops are the copies', so the moves cost nothing, which is what a parity board can
show (the bits are proven by the oracles above, not by the board).

## CI for the previous batch

No complete GitHub Actions run exists yet for 20629f41, a89b18a4 or 0a456bd8: every run of the day was cancelled by a
newer push or is still queued behind the runners. The one risk the previous batch named, `test_dx12_rt.cpp` (which the
Linux tree does not compile) calling `crd::renderpass::pass_param_id`, was checked by reading: the D3D12 test target
links `crd-frame-cook`, which links `crd-render-pass` publicly, so the declaration and its definition both reach it.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- Full tree build, no warnings (1245 steps).
- Test executables, each from a scratch directory: crd-core (103 cases), crd-containers (128), crd-hesap-stats (141),
  crd-hesap-dense (382), crd-hesap-eigen (44), crd-hesap-fft (52), crd-hesap-iterative (149), crd-ceir (580),
  crd-perf (310), crd-lod (18), crd-scene (300): all pass.
- The guard CTests (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, 20 tests) pass, among them
  crd-check-capability-owners, crd-check-source-routes and crd-check-include-budget.
- The strict tidy helper (`tidy-files.py`, LLVM 20.1) on every changed C++ file: clean, after one finding it raised in
  `lanczos.hpp` (a residual norm `rn` confusable with `m`). Three more of the same family sat in hesap-eigen headers
  this move does not touch (`arnoldi.hpp` and `svds.hpp`: `rn`/`rn2` beside `m`/`m2`, and two nested conditionals);
  they are fixed, every hesap-eigen header now analyses clean (12 files) and crd-hesap-eigen-tests (44) passes.
- `check-capability-owners.py`: 66 capabilities, 3 files pending (libm.hpp's ISA read, geometry's transform.hpp,
  crd-perf's bundle publish).

## Remaining on OWN.1

`libm.hpp`'s ISA read behind a crd-core kernel-level macro; geometry's `det3`, inverse-transpose and the Cramer solves
(`transform.hpp`, `circumcenter.hpp`, mesh-processing's `quadric.hpp`) onto crd-math; the audio biquad loop onto
hesap-dsp; the crd-compression module; crd-perf's bundle publish onto durable files once crd-perf can link
crd-platform; the Clough-Tocher orientation. Each with its oracle and before/after board.
