# 2026-10-31 — OWN.1: the crd-compression module, and kernel units' FMA read from their level

<!-- doc-role: session -->
> Dated session note; no live status. Current work: [ROADMAP](../ROADMAP.md); current rules: [AGENTS](../../AGENTS.md).

Row: [OWN.1](../ROADMAP.md#slice-own.1), the compression decision after the
[biquad-step and walk-orientation move](2026-10-31-own-biquad-and-walk-orientation-move.md). Built, tested and
measured on `cerid-track-b`. The contract is
[capability ownership](../design/capability-ownership.md#the-compression-module).

## CI for the previous batches

The first GitHub Actions run to finish jobs past the build for this track's commits is 41231558128 (d5cc2ec0, the
ISA-read move); it was still running when this batch ended. Its failures:

- **This track's: win-tidy.** `tests/foundation/math/kernels/simd_level_probe.cpp:31` stopped the avx2 and avx512
  probe units: crd-core reported no FMA on those levels. clang-tidy drops MSVC's slash-spelled flags (the scar in the
  root CMakeLists.txt), so the analysis saw neither `/arch:AVX2` nor `__AVX2__`, and `CRD_TARGET_X64_FMA`, read from
  the compiler, was 0 there. Reproduced on `cerid-track-b` with the pinned clang-tidy 20 in `--driver-mode=cl`:
  `/arch:AVX2` and `/clang:-march=x86-64-v3` leave `-target-cpu x86-64`, only a dash-spelled `-march` reaches it. Fixed
  below.
- **Other tracks', each already fixed on main by its own track:** `durable_file.cpp:848`'s ignored `ftruncate` result
  (be0adf3f), `cpu_features.cpp:1500`/`:1513`'s shadowed `implementer` and `part` (1667ad60),
  `test_register_ids.cpp:51`'s signed/unsigned compare (2782ab9e) and `wire_bytes.hpp:26`'s stringop overflow under
  LTO (f635efb1). The fft SEGFAULT and sparse picks failures seen in older logs belong to a run (4ab1cd90) that
  predates this track's commits.

No complete run covers 70d2df36 (the biquad move) yet; its MSVC and clang-cl read carries forward.

## Kernel units read FMA from their level (capability `cpu.kernel-dispatch`)

`kernel_level.hpp` now gives a kernel unit `CRD_TARGET_X64_FMA` from `CRD_KERNEL_LEVEL` (1 on exactly avx2 and
avx512), a define the analysis sees, and general code keeps the compiler's reading. Every kernel unit compiled outside
the analysis (`__clang_analyzer__` undefined) refuses to compile when its compiler's instruction set disagrees with its
level, which subsumes the probe's check (removed from `simd_level_probe.cpp`; its `kFmaBody` assertion stays). The
analysis now reads the intrinsic fma body in MSVC's avx2 and avx512 units instead of the software one.

## The crd-compression module (capability `compression.codecs`)

Decision 1 of [capability ownership](../design/capability-ownership.md#decisions) is in place:
`engine/foundation/compression` (crd-compression, DEPENDS containers core memory, PACKAGES zstd) holds:

- `deflate.hpp`: resources' own raw DEFLATE and inflate, zlib and Adler-32, moved whole (`crd::compression`,
  clang-formatted, otherwise unchanged), with its suite (`test_deflate.cpp`, moved);
- `zstd.hpp`: the zstd binding, zstd's header private to the module. `zstd_compress_bound`, `zstd_compress`,
  `zstd_decompress_exact` and `zstd_max_decompressed_size` with `kZstdBlockMaxBytes` (moved from `crdr.hpp`), and
  `kZstdDefaultLevel`. `test_zstd.cpp` covers the bound at its saturation edge (checked at compile time), the compress
  bound and its refusal past one frame, round trips at five sizes, two kinds of input and five levels (clamped ones
  included), determinism and the replaced output, the refused oversize input, the exact-size refusals (one byte more,
  one less, none), a truncated, a corrupt and an empty frame, and concatenated frames.

Consumers: resources' PNG, EXR and zip codecs call `crd::compression`; the CRDR reader and writer call the binding
with their semantics unchanged (the claim bounded before anything is reserved, any other decoded size refused, a chunk
stored raw when zstd fails or does not shrink it); `crdr.hpp` names the writer's default level, so crd-resources links
crd-compression publicly and no longer names zstd; the resources fuzz target restates the layout over the binding and
drops its private zstd include path. The registry lands `compression.codecs` with both headers and three signatures
(the DEFLATE length base table, the Adler-32 modulus, a direct zstd call or include); a zstd call planted in
`crdr.cpp` is refused by line. The audit found a second inflate in hesap-tensor's npz reader (`io_zip.hpp`), now a
`pending` entry on OWN.1.

## Before/after board

`runtime/examples/bench_compression.cpp` (a retained bench) times each codec per KiB of a 256 KiB text-like input and
prints the XXH3-64 of every case's output. The engine before the move (main at 0fa75ccf with the bench added) and after it are two
builds, two commands of [ab-board.py](../../scripts/ab-board.py) with `--same` on the digests: both print the same
bytes at every case of every repetition, so the move is **bit-exact**. linux-gcc-release, GCC 13.3, `cerid-track-b`
(8 vCPUs, Intel Xeon), CPU 3 pinned, one warm-up, band 2%, the host idle before and after (`build-bench.py host`).
Times are ns per KiB; speed ratio = before over after, median with its bootstrap 95% interval.

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| deflate-raw | 63106 | 65591 | 0.9607 [0.9512-0.9712] | slower |
| inflate-raw | 4084 | 4038 | 1.011 [1.011-1.012] | level |
| zlib-deflate | 66671 | 69770 | 0.9558 [0.95-0.9761] | slower |
| zlib-inflate | 7122 | 7084 | 1.005 [1.005-1.006] | level |
| adler32 | 3036 | 3036 | 1 [0.9998-1] | level |
| zstd-compress | 2374 | 2372 | 1 [0.9972-1.003] | level |
| zstd-decompress | 927.3 | 930.7 | 0.9954 [0.992-1.001] | level |

The first board read the same (deflate-raw 0.9642, zlib-deflate 0.9708). `deflate_raw`'s 916 instructions are the
same in both binaries (only RIP-relative displacements differ); it starts at 0x8010 before and 0x8000 after. With every
function aligned to 64 bytes in both builds (`-falign-functions=64`, both at 0x8180) the deflate cases are level:

| case | before | after | speed ratio | verdict |
|---|---|---|---|---|
| deflate-raw | 66353 | 66761 | 0.989 [0.9782-1.004] | within noise of level (30 repetitions) |
| zlib-deflate | 69913 | 69866 | 1.001 [0.9917-1.01] | level |
| the other five | | | 0.9949 to 0.9998 | level |

So the 4% is the hot loop's placement, not the move; the 16-byte-offset placement is the faster one for this loop,
which is a code-layout question for the optimization track rather than an ownership one.

## Verification (cerid-track-b, linux-gcc-debug, GCC 13.3)

- Tree build after the rebase, no warnings.
- Test executables from a scratch directory: crd-compression-tests (13 cases), crd-resources-tests (137),
  crd-cooker-tests (30): all pass. The resources fuzz corpus replay and the resources fatal specimens (7 tests) pass.
- The guard CTests (`^crd-no-|^crd-check|^crd-diag-coverage$|^crd-diag-seeded-faults$`, with crd-repository-tools
  and crd-ci-platform: 22 tests) pass, crd-check-capability-owners, crd-check-source-routes and
  crd-check-include-budget among them; the forced-level arms (`kernels-at`, 986 tests) pass.
- The strict tidy helper (`tidy-files.py`, LLVM 20.1) on every changed C++ file: clean after two fixes (a local
  constant's case, an integer division in a floating-point context in the bench).
- The kernel-level fix: the pinned clang-tidy 20 in `--driver-mode=cl`, given win-tidy's flags, now reads no FMA at
  the sse2 and sse4.2 levels and FMA at avx2 and avx512, without the refusal; GCC refuses an avx2 unit built with
  `-march=x86-64-v2` and accepts it with `-march=x86-64-v3`. The tree rebuilt (1385 steps, no warnings), the
  forced-level arms with the math suites (989 tests), the 22 guards and crd-math-tests pass.
- A slip, recorded: an SSH error made one detached job start twice, and two copies of the alignment build shared their
  trees; one died on a bus error reading a PCH the other rewrote. The surviving copy's binaries were the measured ones,
  and `--same` held their bits.

## Remaining on OWN.1

- hesap-tensor's npz inflate (`io_zip.hpp`) merges with crd-compression's into one decoder with an exact-size form.
- crd-compression and its zstd join the relocatable package, then crd-perf's bundle reads and writes compressed
  sections through it (its `kSectionFlagCompressed` is refused today) with a bounded inflate.
- crd-perf's bundle publish onto crd-platform's durable files, once crd-perf can link crd-platform.
