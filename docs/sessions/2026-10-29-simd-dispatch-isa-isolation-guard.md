# 2026-10-29 — SIMD-DISPATCH: the ISA isolation guard

<!-- doc-role: session -->
Row: [SIMD-DISPATCH](../ROADMAP.md#slice-simd-dispatch). HEAD before: `69bba253`. ADR-0134 §6 (amended).

## CI first

Push run 40748658864 (`f7304724`) is still running; every finished failure is `fft_kernels.cpp`'s `std::memcpy`
(all GCC and Clang lanes, x64 and ARM64) or `clocks.cpp`'s `ARM64_CNTVCT` (win-arm64-debug), both fixed in
`fcb3bb42`. Its win-tidy job then failed on four findings in tests at HEAD: local `constexpr` constants named
`kPlanes`, `kImages` (`test_tensor_kernels.cpp`) and `kEntities` (`test_schedule.cpp`), which the naming rule spells
lower_case, and `SelfAnchored`'s copy assignment without a self-assignment test; fixed in the next commit. The flip's
run 40761837951 was cancelled before any job started. No run completed, so no Needs CI row has new hosted evidence.

## What landed

- `scripts/check-isa-isolation.py`, registered as `crd-check-isa-isolation` on every lane with tests. CMake writes
  `<build>/isa-isolation/<config>.manifest` (`crd_add_isa_isolation_check`): each kernel object with its level,
  feature and archive, each general object with its target and artifact, each image. A built artifact with a
  missing object fails (a stale tree); objects of unbuilt targets are counted, not judged.
- Symbols: a kernel object's non-local symbols name its level namespace (any symbol whose mangled name names it is
  unique to the level), or are its table or reviewed compiler data; a general object defines nothing in a level
  namespace and no level table. MSVC operators put the namespace right after their code (`??Havx2@`).
- Floor and ceiling: VEX, EVEX, REX2 and XOP from the instruction bytes after segment and 67 prefixes, and the
  legacy-encoded instructions of later levels by mnemonic (not TZCNT; on Windows CMPXCHG16B, PREFETCHW and
  LAHF/SAHF are the OS's floor). Each kernel unit stays within its level; each family level holds its table and VEX
  (avx2) or EVEX (avx512) code. Unoptimized FFT codelet units run on scalars by design, so presence is judged per
  object library, not per source. ARM64: SVE/SME registers, LSE, RCpc, dot product, i8mm, BF16, FP16 arithmetic,
  complex, JSCVT, CRC32, crypto and SHA3.
- MSVC places jump tables in `.text`; a byte index table decoded as `62` in `dx12_compute_context.cpp.obj`. In
  objects a table is a `Static` `$LN` symbol (case labels are `Label`), matched by section and offset; in images the
  map's `$LN` static whose first entry points into its function. Both are data to the next function.
- IL lanes: `/MAP` on every MSVC and lld-link IPO link. The map gives each function's address and object under
  LTCG and ThinLTO (`<image>.lto.<archive>.lib<member>`), so win-release needs no PDB; GCC slim LTO objects list
  their symbols through `gcc-nm`, ELF images through their symbol tables. Image dumps stream in 32 MB chunks and
  object batches stop at 8 MB: 32 workers on 16-object batches of unoptimized FFT objects ran Python's regex engine
  out of memory once ("internal error in regular expression engine").
- An object that defines functions, or an image, whose dump yields no instruction line the guard reads fails, so a
  decoder shape it does not know (llvm-objdump, a new dumpbin layout) cannot pass; Linux takes GNU binutils only.
- `crd-simd-emission-check`, its two scripts and its fixture test are gone; `read_xcr0` is out of line so the
  reviewed XGETBV exception names one function.

## Findings

- MSVC 14.51's vectorizer emits `__isa_available`-dispatched paths under every `/arch` at `/O2`: EVEX `vpmullq` for
  64-bit index products in the sse2, portable and avx2 units of hesap-direct, hesap-fft and hesap-tensor, and
  SSE4.1 `pmaxud`/`pmovzxdq` in zstd. They are runtime-guarded and reviewed (`msvc-isa-dispatch`), but
  `CRD_CPU_LEVEL` does not lower `__isa_available`, so a forced arm on an AVX-512 host runs them: the next item.
- The MSVC STL's out-of-line `_Countl_zero_lzcnt` and `_Sqr_error_x86_x64_fma` (`<complex>`) hold LZCNT and FMA3
  behind `__isa_available` tests in their callers; `msvcprt:vector_algorithms.obj` holds SSE4.2 and AVX2 bodies.

## Checks

- win-debug, full tree rebuilt: 133 kernel and 1,885 general objects pass in 22 s under CTest (the guard set 16/16).
- win-release (IPO): `crd-core-tests`, `crd-math-tests` and `crd-hesap-autodiff-tests` with their maps, 1,096 IL
  objects, pass.
- Teeth: the tree's pre-flip objects failed with 782,374 hits; relabelling an avx2 unit sse2 and an avx512 unit
  avx2 failed the namespace, ceiling and table checks; a map whose kernel objects were renamed to a general object
  failed 1,058 times; the stale WSL mirror's pre-flip `deterministic.cpp.o` failed on ELF, its 22 kernel objects
  passed.
- `test-repository-tools.py IsaIsolation` (15 cases: encodings, both dump shapes, decode counts, symbols, levels,
  families, jump tables, maps, formats, streaming, decoder failures), `test-dev-workflow.py`.

## First hosted runs

Push run 40784988994 (92db578d) ran the guard first, on the lanes that got past their builds.
- win-arm64-debug (object route, 16 kernel and 565 general objects, 21 s) failed 779 times on literal data: MSVC
  keeps a function's constants in ARM64 `.text` (SIZE_MAX/56 `0x0492492492492492`, FNV-1a's offset basis, 1e8,
  STATUS_INFO_LENGTH_MISMATCH, "TYPE") and its switch tables under a `__swt` label, and dumpbin decodes them as SVE.
  The words an LDR (literal) loads and the `__swt` tables are now data (fixture cases; no ARM64 compiler on this
  host, so the next win-arm64 run judges it).
- linux-gcc-debug-scalar stopped at once: GCC's precompiled header (`cmake_pch.hxx.gch`) is among a target's objects;
  the manifest now leaves `.gch` and `.pch` out.
- The same ARM64 run failed `crd-check-diag-runbooks` 44 times: a module selection (the ARM64 lanes leave crd-jobs
  out) registers part of the tests the runbook commands' full presets do. CMake passes the omitted modules, and such
  a tree checks everything but the test references (x64's full trees resolve them on every push).

## Left

The guard's first green hosted run on every lane, with its runtime per lane (Clang's `__clang_call_terminate` and the
coverage records are reviewed in advance; libgcc's `__cpu_indicator_init` or Tracy's timer may surface on the GCC IPO
images and are reviewed there if so). The forced levels and MSVC's dispatch landed next
([session](2026-10-29-simd-dispatch-forced-levels-crt-dispatch.md)).
