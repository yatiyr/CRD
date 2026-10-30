# ADR-0134 — Runtime CPU dispatch: an SSE2 floor and per-level kernels

<!-- doc-role: decision -->
> Direction and acceptance contract. Live work: [ROADMAP](../ROADMAP.md#slice-simd-dispatch).

**Status:** accepted direction. The user decided on 2026-10-26 that SSE2 stays the x64 floor through runtime dispatch
([decisions](../sessions/2026-10-26-user-decisions-platform-floors-and-research.md)) and approved the research
amendment (Highway as a design peer, 1.4.0 since 2026-10-30 by the user's choice, AVX10 version and APX detection, SME beside SVE, one OWN.1 capability)
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
([session](../sessions/2026-10-27-simd-dispatch-geometry-kernels.md)), then the eylem-rigid3d body step on plain chunk
columns ([session](../sessions/2026-10-28-simd-dispatch-eylem-body-step.md)), then the baseline build of §4
([session](../sessions/2026-10-28-simd-dispatch-baseline-flip.md)); the other clauses are open in the row.

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
   for the oracle lanes); the `-sse2` lanes become forced-level arms of one binary. Landed 2026-10-28: GCC and Clang
   general code is pinned to `-march=x86-64` whatever the toolchain defaults to, cl and clang-cl take no `/arch`,
   `CRD_SIMD_LEVEL` is `auto|scalar|sse2|avx2|neon` (`native` is refused) and `CRD_SIMD_TARGET=2` is an error; the
   win-debug and linux-gcc-debug jobs run their suite a second time with `CRD_CPU_LEVEL=sse2`.
5. **Kernel units.** A hot kernel is a translation unit compiled once per level with that level's flags (MSVC
   `/arch:AVX2`, `/arch:AVX512`; GCC/Clang `-march=x86-64-v2`/`-v3`/`-v4`), its code inside a per-level namespace (the
   Highway pattern), so no inline function or template instantiated there shares a mangled name, and so a COMDAT
   body, with baseline code or another level. The crd-math SIMD types follow the unit's level, not a global macro;
   raw `__AVX2__` tests go. Headers shared with kernel units call no inline function of the standard library
   (`crd/math/libm.hpp`); the scalar `crd::math` surface (select, the exp/log, trig, power and hyperbolic families) is
   one and lives in the level namespace (amended 2026-10-28). A carrier whose register layout follows the level (the
   forward jets, `JetPackD`) lives in the level namespace, and a functor over it runs at the level of the unit that
   instantiates it: general code at the baseline, a module's kernel family at each level (hesap-autodiff's canned
   gradients); a caller's jets and the code evaluating them are one unit, no cooked data holds a jet, and no generic jet
   workspace ABI exists until a second consumer needs one (amended 2026-10-28). Layout constants that reach data
   (`kNativeLaneWidth` for AoSoA) become level-independent, so one binary and its cooked data agree on every level.
   Dispatch is one indirect call per kernel invocation at the routine level (a whole GEMM panel loop), never per
   microkernel (ADR-0088's measured loss). Each table is resolved
   once from `cpu_selection()` (the last whose level the selection and the build's cap include and whose features
   `kernel_features` holds), and every table stays callable for the differential tests. A unit compiles to machine
   code on every lane, never to link-time IL (amended 2026-10-28): entered only through its table, it has no call for
   link-time code generation to inline across, and IL made every image that links the archive generate the unit's
   code again. A wide type is aligned to min(its size, the level's widest register) (amended 2026-10-28): split into
   halves below its native level, it carries the halves' alignment, so no by-value argument is aligned above the
   unit's registers (GCC's realigned argument path, `-Wpsabi`).
6. **Isolation guard.** A CTest on every lane disassembles the baseline code and fails on any instruction above the
   floor (VEX or EVEX encodings on x64) outside the per-level namespaces, and on any per-level symbol a baseline unit
   defines. LTCG objects of general code hold IL, so MSVC trees judge those on the linked executables with their PDB
   symbols; kernel objects hold their instructions (§5). Landed 2026-10-29 as `crd-check-isa-isolation`
   (`scripts/check-isa-isolation.py`, over a per-configuration manifest of kernel objects with their level, general
   objects and images), amended: the floor also bars REX2 and XOP and every legacy-encoded instruction of a later
   level (SSE3 to SSE4.2, POPCNT, LZCNT, MOVBE, AES, PCLMUL, SHA, GFNI, RDRAND, ADX, XSAVE and XGETBV, RDTSCP,
   CLFLUSHOPT, CLWB, PREFETCHW; not TZCNT, which every x86-64 CPU runs as BSF; on Windows CMPXCHG16B, PREFETCHW and
   LAHF/SAHF are inside the OS's own floor), and on ARM64 SVE, SME, LSE and the later-revision instructions; each
   kernel unit is held to its level, with VEX code at avx2 and EVEX at avx512 (this retires `crd-simd-emission-check`);
   a symbol is per-level when its mangled name names a level namespace, and a kernel object defines no other
   non-local symbol than its table and reviewed compiler constants. The linked-image route covers every IL lane, MSVC
   and clang-cl through the `/MAP` map of each IPO link (each function's address and object, so win-release needs no
   PDB; MSVC's jump tables in `.text` are data from their `$LN` label to the next function, and a function start the
   linear sweep did not decode as an instruction is decoded again from itself), GCC slim LTO through the
   images' symbol tables and its objects' symbols through `gcc-nm`. MSVC's ARM64 objects keep constants in `.text`:
   the words an LDR (literal) loads, the `__swt` switch tables and the compact ones (the address an ADR forms for an
   indexed load, to the next function) are data. `scripts/isa-isolation.json` holds the
   reviewed exceptions, each naming its runtime check (the MSVC STL's and the CRT's vector math `__isa_available`
   tests, zstd's dynamic BMI2, XGETBV behind OSXSAVE).
7. **Determinism.** A kernel declared bit-exact (ADR-0063) produces the same bits on every level: the same operation
   order, and single-rounded FMA only where every level computes it single-rounded. The others declare tolerances
   against the portable reference. Each kernel's differential test runs every level the host executes, in one process.
   The hesap-dense GEMM blocks are the first tolerance-declared family: each level is bit-exact with its declared
   arithmetic (two roundings on portable, where SSE2 has no FMA; single-rounded FMA from avx2, ADR-0082's 2026-05-20
   choice), and the levels agree within the inner-product bound. Its tile shape is per level, since packed panels are
   per-call scratch that each level packs itself (ADR-0082's backend-coupled packing; cooked data stays
   level-independent), and every GEMM path sums the depth in the same 256-value groups, so no path, worker count or
   tile shape changes an element's bits. Its vector kernels add a reduction shape per level
   (the accumulator and row counts and the vector width, 512 bits at avx512), which `dense_kernels.hpp` declares and
   the differential tests model; its
   elementwise kernels give every element the level's step, and its rotation rounds twice on every level. The
   hesap-fft transform is bit-exact: one plan's algorithms run on the level-aware SIMD types, whose
   single-rounded fma and two-rounded products give every level the same bits (NaN payloads aside); its generated
   codelets compute tiles of four f64 or eight f32 columns on every level, two tiles a register at avx512. Which plan
   a level runs follows the process's determinism mode (amended 2026-10-30, the user's choice "C" of both plans,
   chosen explicitly): `crd::math::DeterminismMode` (crd-math, the ADR-0063 modes eylem's scene configuration names)
   is set by code through `set_kernel_determinism` and read when a plan is built. Under `Default` each level runs its
   fastest measured forward per size, the saved board's table (`level_plans_gen.hpp`, generated by
   `scripts/gen_fft_level_plans.py` from the board's data file: the fastest median where its every pass beat every pass
   of the level's planned arm); levels then agree within twice the ADR-0092 §5 class each plan meets against the
   radix-2 reference (f64 1e-12, f32 2e-4 and 1e-3 from 512K, on max |a - b| / (1 + max |b|)). Under `CrossPlatform`
   and `BackwardCompat` every level runs the canonical planner, so its bits are identical across levels (replays,
   lockstep, golden tests); a scene's eylem configuration names its own mode and does not set the process's. The
   four-step's sub-plans are canonical in both, the composition the board measured. A plan keys on the kernel unit it
   runs (`fft_kernels().level`; fft builds portable, sse2, avx2 and avx512, so an sse4.2 selection runs the sse2 unit
   and its plans), and the board's logs name that unit. The 2026-10-30 board measured the f32 forwards at portable,
   sse2 and avx2 (avx2 is the canonical planner at every size); f64, every inverse, avx512 and neon run the canonical
   plan until a board measures them. The geometry
   families are bit-exact the same way: two roundings a step, the IEEE min/max ordering and scalar lane decisions; so
   is the eylem-rigid3d body step, with true divisions and square roots. The forward jets are bit-exact too:
   per-lane chains in which only the product rule fuses (one fma), the quotient and scale steps round each operation
   and the value lane is scalar, whatever the register width.
8. **Qualification.** One binary per lane runs forced-level arms (`CRD_CPU_LEVEL=sse2`, `sse4.2`, `avx2`, and `avx512`
   where the runner has it, `portable` and `neon` on the hosted ARM64 lanes, which give general code the NEON backend
   of the SIMD types); AVX-512 and AVX10 kernels run under Intel SDE until such hardware exists (10.13.1,
   approved 2026-10-27 and pinned, `scripts/install-sde.py`): with `CRD_SDE`, win-release and linux-gcc-release run
   `<suite>-kernels-at-avx512-sde-<cpu>` per SDE CPU model whose features a body selects on, and those arms fail
   when the level is refused (the hidden `[cpu-level-applied]` case), as do the portable, sse2 and neon arms, which
   every CPU of their architecture runs. Amended 2026-10-29: an arm's differential cases run portable and its applied
   level, not every level again (the lane's unforced run has the §7 every-level proof, and N arms of N levels made
   the suites quadratic), and the lane's `-at-sse2` pass leaves out the arms and the guards (first green on push run
   40826432863: 7,997 tests on win-debug, 7,671 on linux-gcc-debug). On MSVC and clang-cl x64
   an applied level also lowers the CRT's own dispatch in each image (`__isa_available`, which the vectorizer's
   paths and the STL test, with `__isa_enabled`, `__isa_inverted` and `__avx10_version`) and below avx2 turns the
   UCRT libm's FMA3 bodies off, from a `.CRT$XCL` initializer that crd-core's interface keeps in every image, before
   any user initializer; it never raises them, and `cpu_runtime_dispatch()` reports them. The UCRT's software fma,
   which such an arm and a CPU without FMA3 run, does not round once (the first run showed lost signs of underflowing
   products and sums rounded twice), so `crd::math`'s fma calls the C library only while its fma is the instruction
   (`crd::detail::libc_fma_fused`: the UCRT's FMA3 state; glibc's FMA or FMA4 feature active, its resolver's choice),
   issues the instruction in avx2 and avx512 units, and otherwise rounds once itself (an integer f64 body, f32 by
   rounding an exact f64 sum to odd, always on glibc, whose fused fmaf call is no faster). glibc's software fma
   rounds once but took 119 ns (f64) and 70 ns (f32) against 10 and 1.3 ns (amended 2026-10-30); engine code calls
   `crd::math::fma`, never `std::fma` (crd-no-std-transcendental-check).
   The DLL runtimes' string routines (vcruntime140, ucrtbase), glibc's ifuncs and zstd's dynamic BMI2 decoders keep
   dispatching on the hardware with the same results. The doctor, the specimen harness's host tuple
   and every board record the detected and selected level and the override status. Landed 2026-10-29:
   `format_cpu_selection` (crd-core) writes the one line
   `cpu detected=<level> selected=<level> override=<status>`, with ` crt=<level> fma3=<0|1>` where the image's CRT
   dispatches and ` extra=<features>` for the kernel features above the selected level; the doctor carries the
   selection (`diag.capabilities`: `cpu_detected`, `cpu_selected`, `cpu_override`, and `crt_level`, `ucrt_fma3`
   where the CRT dispatches; additive, so `cerid-diagnostics/1` and command schema 1 stay), and the harness's host
   tuple ends with the line of the child it spawns, under `CRD_CPU_LEVEL` as set at the spawn. A board's arm is
   preceded by `runtime/cpu_level_record`'s line (with the build's `kernel_cap`) under the same `CRD_CPU_LEVEL` and
   CPU list, a function of the same inputs as the measured program's selection, which stops the board when the level
   did not apply or exceeds the cap (`run-lane-bench.sh`, whose default arms are every level the pinned CPUs run,
   `cpu_level_record --levels`); crd-bench prints it before its run and a
   diag-overhead record carries it (`cpu_level`), refused by `diag-overhead.py --cpu-level` when it differs from the
   asked level or between arms. The diag-overhead,
   hesap, geometry and jobs boards are re-measured at each level against their peers, Highway 1.4.0 included for the
   dispatch cost (`bench_dispatch_cost`: a crd table's call against `HWY_DYNAMIC_DISPATCH`, each against its
   selected body's pointer; Highway, which dispatches itself, builds at the baseline as its own build does). A peer runs as its own build would on the measuring host: one number per workload, which
   `CRD_CPU_LEVEL` never lowers, and every Cerid level is measured against it (amended 2026-10-29). A peer compiled
   from source in a board (Eigen, pocketfft, Boost) is a shared library of its own at the host's best ISA
   (`crd_add_bench_peer`, `crd_peer_isa_flags`: `-march=native`/`-mcpu=native` on GCC and Clang, and on cl the
   `/arch` of the level `detect_cpus()` reports at configure time), with hidden symbols, no link-time IL and no Cerid
   library, so no inline body at its ISA reaches the bench units at the baseline; plain data crosses its interface and
   the bench times both sides with one loop; `crd-check-peer-headers` fails any include of such a header library
   outside `runtime/examples/peers/`. The ISA isolation guard leaves peers out (`CRD_ISA_ROLE peer`, OpenBLAS's
   targets too). An applied level's UCRT FMA3 switch is process-wide on MSVC, so a board takes the peer's number from
   its detected-level arm. On Linux a forced x64 arm hides the features above its level from glibc's ifuncs
   (`glibc.cpu.hwcaps` in `GLIBC_TUNABLES`, `run-lane-bench.sh`, 2026-10-30), so libm's `fma` and the string routines
   run the bodies a CPU of that level gets; portable keeps the host's glibc (its targets, architectures without x64
   levels, have the FPU's fma), and the peers dispatch on their own CPUID.
9. **Ownership.** CPU ISA detection and kernel dispatch is one OWN.1 capability: detection in crd-core, the dispatch
   tables and level-parameterised SIMD types in crd-math, kernels in their owning modules.

## Consequences

The x64 binary runs on every x86-64 CPU and uses its best level; one build qualifies every level. Kernel code is
compiled up to four times on x64; general code loses AVX2 autovectorisation, which the boards measure per level.
