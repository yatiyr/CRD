# ADR-0134 — Runtime CPU dispatch: an SSE2 floor and per-level kernels

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

**Status:** accepted direction. The user decided on 2026-10-26 that SSE2 stays the x64 floor through runtime dispatch
([decisions](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md)) and approved the research
amendment (Highway 1.3 as a design peer, AVX10 version and APX detection, SME beside SVE, one OWN.1 capability)
([research](../research/2026-10-26-state-of-the-art-2026-comparison.md)). Detection and selection landed on
2026-10-26 ([session](../sessions/2026-10-26-simd-dispatch-cpu-detection.md)), then the kernel units, the dispatch
table and the first family ([session](../sessions/2026-10-26-simd-dispatch-kernel-units.md)), then the level-aware
SIMD types ([session](../sessions/2026-10-27-simd-dispatch-level-aware-types.md)), then the hesap-stats family
([session](../sessions/2026-10-27-simd-dispatch-hesap-stats-kernels.md)), then the hesap-tensor permute and Tier R
family ([session](../sessions/2026-10-27-simd-dispatch-hesap-tensor-kernels.md)) and its nn routines
([session](../sessions/2026-10-27-simd-dispatch-hesap-tensor-nn-kernels.md)), then the feature variants with AVX-VNNI on
avx2 ([session](../sessions/2026-10-27-simd-dispatch-avx-vnni-variant.md)), then the tensor SR draws and sketches on a
kernel Philox block loop ([session](../sessions/2026-10-27-simd-dispatch-tensor-philox-blocks.md)), then its SSE2
body ([session](../sessions/2026-10-27-simd-dispatch-sse2-philox.md)), then the SSE2 Threefry body beside scalar
blocks ([session](../sessions/2026-10-27-simd-dispatch-sse2-threefry.md)), then the hesap-special Lanczos family
([session](../sessions/2026-10-27-simd-dispatch-special-kernels.md)), then the hesap-sparse SELL family
([session](../sessions/2026-10-27-simd-dispatch-sparse-kernels.md)), then the hesap-dense GEMM blocks
([session](../sessions/2026-10-27-simd-dispatch-dense-kernels.md)), then its BLAS-1/2 and Cholesky kernels on an SSE2
`Vec4d` ([session](../sessions/2026-10-27-simd-dispatch-dense-vector-kernels.md)), then its axpy, gemvt, ger,
rotation, reflector and LDLT updates behind one typed table
([session](../sessions/2026-10-27-simd-dispatch-dense-update-kernels.md)), then its split-complex sweeps
([session](../sessions/2026-10-27-simd-dispatch-dense-split-complex-kernels.md)), then the hesap-direct supernodal
solve family ([session](../sessions/2026-10-27-simd-dispatch-direct-kernels.md)), then the hesap-fft transform on the
plan's plain state ([session](../sessions/2026-10-27-simd-dispatch-fft-kernels.md)), then the geometry batch kernels,
hull support scan and mesh raycast on plain columns
([session](../sessions/2026-10-27-simd-dispatch-geometry-kernels.md)); the other clauses are open in the row.

**Extends:** [ADR-0063](0063-eylem-determinism-contract.md) (determinism),
[ADR-0082](0082-hesap-microkernel-intrinsics-strategy.md) (intrinsics first),
[ADR-0088](0088-gemm-runtime-dispatch-asm-microkernels.md) (the reverted asm dispatch).

## Context

`cmake/CrdSimd.cmake` decides one ISA for the whole program at configure time: `CRD_SIMD_LEVEL=auto` gives every
target that links crd-math `/arch:AVX2` or `-mavx2 -mfma -msse4.2 -mf16c`. An SSE2 machine cannot run that binary,
and the SSE2 lanes are separate builds. The mix also miscompiles: under MSVC LTCG an AVX2 unit's inline bodies
(`std::atomic` loads) are kept out of line when called from a baseline library, which cost TickClock 2.9%
(DIAG.11b (q)). ADR-0088 built a CPUID function-pointer dispatch for an asm GEMM microkernel; it was reverted with the
asm because a call per microkernel lost about 2% to the inlined intrinsic.

## Decisions

1. **Levels.** x64: `Sse2` (x86-64-v1), `Sse42` (v2), `Avx2` (v3: AVX2, FMA, F16C, BMI1/2, LZCNT, MOVBE) and `Avx512`
   (v4: AVX-512 F, CD, BW, DQ, VL, or AVX10 at 512 bits). AVX-512 extensions, AVX10.2, AVX-VNNI, APX and AMX are
   features a level's kernel selects on; a new level above `Avx512` is added only for a measured kernel that needs it.
   A feature variant is the level's kernel unit built again with the feature (`crd_add_cpu_kernels` `VARIANTS`, its
   own level namespace and table), listed after the level's plain table with the features it needs; the first is
   AVX-VNNI on avx2 (hesap-tensor's integer dots).
   ARM64: `Neon` is architectural; SVE, SVE2 and SME detection and levels come with the ARM64 release rows
   (`getauxval` HWCAP/HWCAP2, `IsProcessorFeaturePresent`). WebAssembly SIMD128 tiers belong to WEB.1.
2. **One detection owner.** `crd/core/cpu_features.hpp` (crd-core, below crd-time and crd-math) is the engine's only
   CPUID and XGETBV site; TickClock reads its invariant-TSC bit. A feature counts only when CPUID reports it and XCR0
   holds the state its registers need (YMM; opmask and both ZMM components; the tile state; the APX GPRs). On Linux an
   AMX kernel still requests the tile permission (`ARCH_REQ_XCOMP_PERM`) before its first tile instruction.
3. **One selection.** `cpu_selection()` runs once, at its first call: the calling thread is pinned to every logical CPU
   the process may run on, CPUID is read there, the thread gets its own affinity back, and the level is what all CPUs
   execute. Hybrid parts and virtual machines are not trusted to be uniform; the walk proves it and records the core
   types seen. It takes about 2 ms for 32 CPUs. `CRD_CPU_LEVEL` lowers the level for tests and qualification; a level
   above the hardware, of another architecture or unknown is refused, the detected level stays, and the status is
   reported. A forced level also limits the features variants select on (`CpuSelection::kernel_features`) to the
   ones that level is defined by, so a forced arm runs as the smallest CPU of its level and the unforced run takes the
   variants. The name is not `CRD_SIMD_LEVEL`, the build option. The walk moves whichever thread calls first
   (TickClock's first map can be a pinned job worker) and gives it back exactly its own affinity.
4. **Baseline build.** General engine code builds at the baseline ISA: crd-simd-flags keeps the determinism flags and
   loses the ISA flags. `CRD_SIMD_LEVEL` caps the levels a build compiles (`scalar` stays the portable reference build
   for the oracle lanes); the `-sse2` lanes become forced-level arms of one binary.
5. **Kernel units.** A hot kernel is a translation unit compiled once per level with that level's flags (MSVC
   `/arch:AVX2`, `/arch:AVX512`; GCC/Clang `-march=x86-64-v2`/`-v3`/`-v4`), its code inside a per-level namespace (the
   Highway pattern), so no inline function or template instantiated there shares a mangled name, and so a COMDAT
   body, with baseline code or another level. The crd-math SIMD types follow the unit's level, not a global macro;
   raw `__AVX2__` tests go. Headers shared with kernel units call no inline function of the standard library
   (`crd/math/libm.hpp`). Layout constants that reach data (`kNativeLaneWidth` for AoSoA) become level-independent,
   so one binary and its cooked data agree on every level. Dispatch is one indirect call per kernel invocation at the
   routine level (a whole GEMM panel loop), never per microkernel (ADR-0088's measured loss). Each table is resolved
   once from `cpu_selection()` (the last whose level the selection and the build's cap include and whose features
   `kernel_features` holds), and every table stays callable for the differential tests.
6. **Isolation guard.** A CTest on every lane disassembles the baseline code and fails on any instruction above the
   floor (VEX or EVEX encodings on x64) outside the per-level namespaces, and on any per-level symbol a baseline unit
   defines. LTCG objects hold IL, so MSVC trees are judged on the linked executables with their PDB symbols.
7. **Determinism.** A kernel declared bit-exact (ADR-0063) produces the same bits on every level: the same operation
   order, and single-rounded FMA only where every level computes it single-rounded. The others declare tolerances
   against the portable reference. Each kernel's differential test runs every level the host executes, in one process.
   The hesap-dense GEMM blocks are the first tolerance-declared family: each level is bit-exact with its declared
   arithmetic (two roundings on portable, where SSE2 has no FMA; single-rounded FMA from avx2, ADR-0082's 2026-05-20
   choice), and the levels agree within the inner-product bound. Its vector kernels add a reduction shape per level
   (the accumulator and row counts), which `dense_kernels.hpp` declares and the differential tests model; its
   elementwise kernels give every element the level's step, and its rotation rounds twice on every level. The
   hesap-fft transform is bit-exact: every level runs one algorithm per size on the level-aware SIMD types, whose
   single-rounded fma and two-rounded products give every level the same bits (NaN payloads aside). The geometry
   families are bit-exact the same way: two roundings a step, the IEEE min/max ordering and scalar lane decisions.
8. **Qualification.** One binary per lane runs forced-level arms (`CRD_CPU_LEVEL=sse2`, `sse4.2`, `avx2`, and `avx512`
   where the runner has it); AVX-512 and AVX10 kernels run under Intel SDE (a download the user approves) until such
   hardware exists. The doctor, the specimen harness's host tuple and every board record the detected and selected
   level and the override status. The diag-overhead, hesap, geometry and jobs boards are re-measured at each level
   against their peers, Highway 1.3 included for the dispatch cost.
9. **Ownership.** CPU ISA detection and kernel dispatch is one OWN.1 capability: detection in crd-core, the dispatch
   tables and level-parameterised SIMD types in crd-math, kernels in their owning modules.

## Consequences

The x64 binary runs on every x86-64 CPU and uses its best level; one build qualifies every level. Kernel code is
compiled up to four times on x64; general code loses AVX2 autovectorisation, which the boards measure per level.
