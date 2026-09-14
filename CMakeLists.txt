cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are unsupported. Use a preset under build/<preset>.")
endif()

project(CRD
    VERSION 0.1.0
    DESCRIPTION "CRD Game/Simulation Engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CrdIdeFolders.cmake)
include(cmake/CrdProjectSync.cmake)
include(cmake/CrdModules.cmake)

option(CRD_BUILD_TESTS      "Build test executables"           ON)
option(CRD_BUILD_BENCHMARKS "Build benchmark executable"       ON)
option(CRD_BUILD_SANDBOX    "Build crd-sandbox interactive app" ON)
# Reference-class shootout for crd-hesap (Phase 3.1.6 v0d-parallelism-scaling).
# When ON, fetches Eigen 3.4 + OpenBLAS via CPM and builds bench_hesap_gemm_vs_reference
# which times Cerid vs Eigen-MT vs OpenBLAS on identical workloads. Sources go to
# build/_deps/ (gitignored), never vendored into the repo. OFF by default — opt in
# via -DCRD_BUILD_HESAP_VS_REFERENCE=ON at configure time.
option(CRD_BUILD_HESAP_VS_REFERENCE "Build hesap-vs-Eigen/OpenBLAS comparison bench" OFF)
# v4j-2a: Cerid InverseBasedIlu vs ILUPACK V2.4 head-to-head. ILUPACK is non-
# commercial/binary-only ⇒ NOT a CPM dep; a precompiled lib under the gitignored
# external/ilupack (run scripts/setup-ilupack-ref.sh). WSL/Linux only (gfortran
# .a). NEVER built in CI, NEVER shipped — a local dev-machine benchmarking oracle.
option(CRD_BUILD_HESAP_VS_ILUPACK "Build hesap-vs-ILUPACK bench (local-only, non-commercial, Linux)" OFF)
# v5e-1d: Cerid HSS ULV vs STRUMPACK dense HSS+ULV (gold-standard rank-structured solver).
# STRUMPACK built from source to $HOME/strumpack/install (scripts/setup-strumpack-ref.sh).
# WSL/Linux only; NEVER built in CI, NEVER shipped — a local dev benchmarking oracle.
option(CRD_BUILD_HESAP_VS_STRUMPACK "Build hesap-vs-STRUMPACK HSS bench (local-only, Linux)" OFF)
option(CRD_ENABLE_ASSERTS   "Enable CRD_ASSERT macros"        ON)
option(CRD_ENABLE_PROFILING "Enable profiling instrumentation" OFF)
option(CRD_ENABLE_ASAN      "Enable Address Sanitizer"         OFF)
option(CRD_ENABLE_UBSAN     "Enable Undefined Behavior San."   OFF)
option(CRD_ENABLE_TSAN      "Enable Thread Sanitizer (GCC/clang; exclusive with ASan)" OFF)
option(CRD_ENABLE_FUZZER    "Build the libFuzzer fuzz targets (clang; coverage instrumentation tree-wide)" OFF)
option(CRD_SHIPPING         "Maximum optimizations + debug symbol generation for crash diagnostics" OFF)
option(CRD_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(CRD_CLANG_TIDY_WARNINGS_AS_ERRORS "Treat clang-tidy warnings as build errors (OFF only for diagnostic preview)" ON)
option(CRD_ENABLE_PCH       "Enable default precompiled headers" ON)
# Diagnostic builds of a compiler the tree is not qualified for (linux-clang-fuzz) show warnings instead of failing on
# them; every qualified configuration keeps warnings as errors (docs/design/test-instruments.md).
option(CRD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors (OFF only for diagnostic presets)" ON)
# Cerid is static-only: the modules are STATIC libraries, the installed package exports them as such, and a shared
# variant has no export contract yet (docs/design/public-consumption.md).
if(BUILD_SHARED_LIBS)
    message(FATAL_ERROR "BUILD_SHARED_LIBS=ON is unsupported: Cerid builds and packages static libraries only")
endif()

# SIMD level + ADR-0063 deterministic-FP contract live in their own module.
# Any target that links crd-math (everything, transitively) inherits the
# /arch:AVX2 (or -mavx2 / NEON / scalar) flags + the determinism guarantees.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CrdBuildProfiles)
include(CrdSimd)
# Opt-in compiler launcher and Ninja job pools; the default graph is unchanged. See cmake/CrdBuildPerformance.cmake.
include(CrdBuildPerformance)
# Opt-in public-header checks and the package consumer test; the default graph is unchanged. See cmake/CrdPublicChecks.cmake.
include(CrdPublicChecks)
# Relocatable package export (install rules only; no build statement). See cmake/CrdPackage.cmake.
include(CrdPackage)

# ---- Log compile-time minimum level ---------------------------------------
# Anything strictly below this level is compiled out to ((void)0).
# Valid values: Trace, Debug, Info, Warn, Error, Critical
set(CRD_LOG_MIN_LEVEL "" CACHE STRING
    "Compile-time minimum log level (Trace/Debug/Info/Warn/Error/Critical). Empty = auto.")

if(CRD_LOG_MIN_LEVEL STREQUAL "")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
        set(CRD_LOG_MIN_LEVEL_RESOLVED "Trace")
    else()
        set(CRD_LOG_MIN_LEVEL_RESOLVED "Info")
    endif()
else()
    set(CRD_LOG_MIN_LEVEL_RESOLVED "${CRD_LOG_MIN_LEVEL}")
endif()

if(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Trace")
    set(CRD_LOG_MIN_LEVEL_NUM 0)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Debug")
    set(CRD_LOG_MIN_LEVEL_NUM 1)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Info")
    set(CRD_LOG_MIN_LEVEL_NUM 2)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Warn")
    set(CRD_LOG_MIN_LEVEL_NUM 3)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Error")
    set(CRD_LOG_MIN_LEVEL_NUM 4)
elseif(CRD_LOG_MIN_LEVEL_RESOLVED STREQUAL "Critical")
    set(CRD_LOG_MIN_LEVEL_NUM 5)
else()
    message(FATAL_ERROR "Invalid CRD_LOG_MIN_LEVEL: '${CRD_LOG_MIN_LEVEL_RESOLVED}'")
endif()
if(CRD_NATIVE_PROFILES AND CRD_LOG_MIN_LEVEL STREQUAL "")
    message(STATUS "[crd] Log minimum follows each native profile: Trace for Debug/RelWithDebInfo; Info for Release/Shipping")
else()
    message(STATUS "[crd] Log compile-time min level: ${CRD_LOG_MIN_LEVEL_RESOLVED} (${CRD_LOG_MIN_LEVEL_NUM})")
endif()

# ---- NDEBUG for optimised configs ---------------------------------------
# CMake's MSVC toolchain may leave CMAKE_CXX_FLAGS_RELEASE empty (unlike the
# C toolchain), so NDEBUG is not always propagated automatically. Ensure the
# standard Release/MinSizeRel configs always get the define, matching the
# intent of every #if defined(NDEBUG) guard in the codebase.
add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)
if(CRD_NATIVE_PROFILES)
    foreach(config IN LISTS CRD_NATIVE_CONFIGS)
        if(NOT CRD_PROFILE_${config}_CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_definitions($<$<CONFIG:${config}>:NDEBUG>)
        endif()
    endforeach()
endif()

# ---- Sanitizers ---------------------------------------------------------

crd_profile_condition(_crd_asan CRD_ENABLE_ASAN)
if(CRD_ENABLE_ASAN OR CRD_NATIVE_PROFILES)
    if(MSVC)
        add_compile_options($<$<AND:${_crd_asan},$<COMPILE_LANGUAGE:C,CXX>>:/fsanitize=address>)
        add_link_options($<${_crd_asan}:/INCREMENTAL:NO>)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "[crd] Address Sanitizer condition: ${_crd_asan}")
endif()

# Instrument policy (REPO.DEV.9, docs/design/test-instruments.md): an instrument the toolchain cannot provide fails
# the configure, and a UBSan report fails the run (no recovery), so a lane can never print a report and stay green.
if(CRD_ENABLE_UBSAN)
    if(MSVC)
        message(FATAL_ERROR "[crd] CRD_ENABLE_UBSAN is not supported by MSVC; configure a GCC or clang preset")
    else()
        add_compile_options(-fsanitize=undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined -fno-sanitize-recover=undefined)
        message(STATUS "[crd] UB Sanitizer ENABLED (no recovery: a report fails the run)")
    endif()
endif()
if(CRD_ENABLE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "[crd] CRD_ENABLE_TSAN is not supported by MSVC; configure a GCC or clang preset")
    elseif(CRD_ENABLE_ASAN)
        message(FATAL_ERROR "[crd] CRD_ENABLE_TSAN and CRD_ENABLE_ASAN are exclusive instruments")
    else()
        add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
        add_link_options(-fsanitize=thread)
        message(STATUS "[crd] Thread Sanitizer ENABLED (fiber switches annotated; docs/design/test-instruments.md)")
    endif()
endif()
if(CRD_ENABLE_FUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR MSVC)
        message(FATAL_ERROR "[crd] CRD_ENABLE_FUZZER needs clang++ (libFuzzer through the clang driver); "
                            "the compiler is ${CMAKE_CXX_COMPILER_ID}${CMAKE_CXX_SIMULATE_ID}")
    endif()
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=fuzzer-no-link>)
    message(STATUS "[crd] libFuzzer targets ENABLED (coverage instrumentation tree-wide; cmake/CrdFuzz.cmake)")
endif()

# ----- Shipping optimizations + debug symbols --------------------------------

crd_profile_condition(_crd_shipping CRD_SHIPPING)
if(CRD_SHIPPING OR CRD_NATIVE_PROFILES)
    # Note: /arch:AVX2 / -march come from CrdSimd.cmake (CRD_SIMD_LEVEL).
    # Note: fast-math intentionally NOT used here — the eylem replay-hash CI
    #       (ADR-0063) requires bit-exact reproducibility, which fast-math
    #       breaks (operator reordering, FMA contraction, NaN assumptions).
    #       Performance gap vs fast-math is a few percent and not worth
    #       sacrificing determinism. Override with -DCRD_DETERMINISTIC_FP=OFF
    #       only for benchmark configs that explicitly opt out.
    # Note: stack-buffer security checks (/GS on MSVC, -fstack-protector
    #       on GCC/Clang) intentionally LEFT ON — disabling them is a real
    #       security regression for a few percent of perf, never worth it.
    if(MSVC)
        # /O2          — max optimisation (set by Release build type; restated
        #                here for documentation: /Og /Oi /Ot /Oy /Ob2 /GF /Gy)
        # /Gw          — each global in its own COMDAT for dead-stripping
        # /Gy          — function-level linking (already in /O2; restated)
        # /Zc:inline   — strip inline-only functions never called externally;
        #                helps the linker dead-strip more aggressively
        # /Zi          — emit a separate PDB (required for crash dump symbolication)
        #
        # Note: /Ob3 (extra-aggressive inlining) was tried as a shipping
        # flag but caused SEGFAULTs in the resource async-load path under
        # clang-cl thin-LTO via dead-store-propagation across inlined
        # `payload.store(nullptr)` callbacks. /Ob2 (the /O2 default) is the
        # ceiling that survives the elite-tier shipping bar across all
        # three compilers (MSVC / clang-cl / GCC). The cost: a few percent
        # of inlining the LTO pass would otherwise pick up — worth it for
        # shipping correctness under the strictest LTO settings the
        # toolchain offers.
        add_compile_options(
            $<$<AND:${_crd_shipping},$<COMPILE_LANGUAGE:CXX,C>>:/Gw>
            $<$<AND:${_crd_shipping},$<COMPILE_LANGUAGE:CXX,C>>:/Gy>
            $<$<AND:${_crd_shipping},$<COMPILE_LANGUAGE:CXX,C>>:/Zc:inline>
            $<$<AND:${_crd_shipping},$<COMPILE_LANGUAGE:CXX,C>>:/Zi>
        )
        # /DEBUG:FULL  — full PDB (not /DEBUG:FASTLINK), required for WinDbg post-mortem
        # /OPT:REF     — strip unreferenced functions (dead code elimination)
        # /OPT:ICF     — merge identical COMDATs (binary size reduction)
        # /LTCG        — link-time codegen (driven via CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        add_link_options($<${_crd_shipping}:/DEBUG:FULL> $<${_crd_shipping}:/OPT:REF> $<${_crd_shipping}:/OPT:ICF>)
    else()
        # -g                       — always emit DWARF debug info for crash symbolication
        # -ffunction-sections      — each function in its own section
        # -fdata-sections          — each global in its own section
        #                            (the two above let the linker drop unused
        #                            funcs/data via --gc-sections without LTO
        #                            having to fold them at IR time)
        # -funroll-loops           — aggressive loop unrolling; safe vs ADR-0063
        #                            because it does not reorder FP ops, only
        #                            duplicates the loop body
        # -Wl,--gc-sections        — discard unreferenced sections at link time
        # -Wl,--build-id           — embed a unique build ID so crash reports
        #                            can identify the binary
        add_compile_options(
            $<$<COMPILE_LANGUAGE:CXX,C>:-g>
            $<$<COMPILE_LANGUAGE:CXX,C>:-ffunction-sections>
            $<$<COMPILE_LANGUAGE:CXX,C>:-fdata-sections>
            $<$<COMPILE_LANGUAGE:CXX,C>:-funroll-loops>
        )
        add_link_options(-Wl,--build-id -Wl,--gc-sections)
    endif()
    message(STATUS "[crd] Shipping condition: ${_crd_shipping}; max optimizations + symbols, deterministic FP preserved")
endif()

# ----- Compiler Warnings (Cerid targets only — propagates via crd-core) -----
# Third-party CPM packages never see these flags; they inherit them only
# if they transitively link crd-core, which they don't.

add_library(crd-warnings INTERFACE)

if(MSVC)
    # Gate flags on CXX/C only — MASM (ASM_MASM) does not accept these options.
    target_compile_options(crd-warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX,C>:/W4>
        $<$<AND:$<COMPILE_LANGUAGE:CXX,C>,$<BOOL:${CRD_WARNINGS_AS_ERRORS}>>:/WX>
        $<$<COMPILE_LANGUAGE:CXX,C>:/permissive->
        $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:__cplusplus>
        $<$<COMPILE_LANGUAGE:CXX,C>:/utf-8>
        $<$<COMPILE_LANGUAGE:CXX,C>:/EHsc>
        # /wd4714 — silence "__forceinline function not inlined". MSVC LTCG
        # decides inlining at link time based on the whole-program TU mix; when
        # the cost model declines to inline a hot helper (e.g. crd-log's
        # should_log) the warning fires inside LNK1257 with no actionable fix
        # at the source level. Suppressing it globally is the correct call —
        # the prior per-target suppression on crd-bench just shifted the
        # symptom to other release-class targets (smoke_imgui_overlay,
        # smoke_renderer, crd-config-tests, crd-imgui-tests, ...).
        $<$<COMPILE_LANGUAGE:CXX,C>:/wd4714>
        # /wd4127 — silence "conditional expression is constant". Fires on
        # legitimate template code where a condition is compile-time-constant in
        # SOME instantiations but runtime in others — e.g. `if (!closed || i < n)`
        # in geometry-curves where `curve.closed` is `static constexpr` for an
        # open Bezier but a runtime member for a general spline, so `if constexpr`
        # is not robust across the template. Version-dependent: local MSVC 14.50
        # relaxed this warning, but CI's MSVC 14.44 still emits it under /WX
        # ([[feedback_msvc_c4127_ci_local_version_skew]]). Same low-value /
        # no-clean-source-fix class as /wd4714 above; suppress globally rather
        # than scatter per-site pragmas. Genuine compile-time branches still use
        # `if constexpr`; genuine pin checks still use `static_assert`.
        $<$<COMPILE_LANGUAGE:CXX,C>:/wd4127>
    )
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(crd-warnings INTERFACE
            $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:preprocessor>
        )
    endif()
else()
    # Gate on CXX/C so AT&T .S assembly files (ASM language) are not affected.
    target_compile_options(crd-warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wpedantic>
        $<$<AND:$<COMPILE_LANGUAGE:CXX,C>,$<BOOL:${CRD_WARNINGS_AS_ERRORS}>>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wshadow>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wdouble-promotion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wnull-dereference>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wformat=2>
    )
endif()

# ----- Module registry (REPO.DEV.4) ----------------------------------------------
# Every build directory is registered once with its declared direct dependencies; crd_add_modules() below adds
# the selected closure in this order and crd_verify_modules() checks the declarations against the real link
# graph at the end of configure. CRD_MODULES empty keeps the full configuration. See cmake/CrdModules.cmake.

crd_module(engine/foundation/core engine/core TESTS foundation/core)
# ADR-0085: OS virtual-memory leaf (page source for crd-memory's VirtualMemoryAllocator)
crd_module(engine/foundation/vm engine/vm DEPENDS core log TESTS foundation/vm)
crd_module(engine/foundation/units engine/units DEPENDS containers core memory TESTS foundation/units
    TEST_DEPENDS math)
# D-007 CEIR-1: Cerid Execution IR — host-only execution/orchestration IR (ADR-0109); the CEIR master spine
crd_module(engine/execution/ceir engine/ceir DEPENDS containers core log memory units TESTS execution/ceir support/fuzz)
# D-007 CEIR-6b: the crd-jobs execution-provider bridge (ADR-0109 §4.2) — crd-ceir + crd-jobs
crd_module(engine/execution/ceir-host engine/ceir-host DEPENDS ceir containers core jobs memory perf time
    TESTS execution/ceir-host)
# D-007 CEIR-7a: the CEIR asset cook bridge (ADR-0109 §4.2) — crd-ceir + crd-resources + crd-render-asset-core
crd_module(engine/execution/ceir-cook engine/ceir-cook DEPENDS ceir containers core memory render-asset-core resources
    TESTS execution/ceir-cook)
# D-007 CEIR-13d: the GPU lowering bridge (ADR-0125/0109 §4.2) — crd-ceir + crd-gpu-context
crd_module(engine/execution/ceir-gpu engine/ceir-gpu DEPENDS ceir containers core gpu-context kir memory
    TESTS execution/ceir-gpu TEST_DEPENDS hesap-autodiff)
# D-007 CEIR-32: CHIR — the Cerid high-level language layer (ADR-0128), host-only, lowers one-way into crd-ceir
crd_module(engine/execution/chir engine/chir DEPENDS ceir containers core log memory units TESTS execution/chir)
crd_module(engine/foundation/time engine/time DEPENDS core units TESTS foundation/time)
crd_module(engine/foundation/perf engine/perf DEPENDS containers core jobs memory time TESTS foundation/perf)
crd_module(engine/foundation/memory engine/memory DEPENDS core log vm TESTS foundation/memory TEST_DEPENDS jobs)
crd_module(engine/foundation/math engine/math DEPENDS core units TESTS foundation/math)
crd_module(engine/geometry/geometry-primitives engine/geometry-primitives DEPENDS containers core math
    TESTS geometry/geometry-primitives)
crd_module(engine/geometry/geometry-bvh engine/geometry-bvh DEPENDS containers core geometry-primitives jobs math
    TESTS geometry/geometry-bvh)
crd_module(engine/geometry/geometry-convex engine/geometry-convex
    DEPENDS containers core geometry-primitives math memory TESTS geometry/geometry-convex)
crd_module(engine/geometry/geometry-mesh engine/geometry-mesh
    DEPENDS containers core geometry-bvh geometry-primitives math memory units TESTS geometry/geometry-mesh)
crd_module(engine/geometry/geometry-spatial engine/geometry-spatial
    DEPENDS containers core geometry-primitives math memory units TESTS geometry/geometry-spatial TEST_DEPENDS jobs)
crd_module(engine/geometry/geometry-polygon engine/geometry-polygon
    DEPENDS containers core geometry-primitives math memory units TESTS geometry/geometry-polygon)
crd_module(engine/geometry/geometry-mesh-processing engine/geometry-mesh-processing
    DEPENDS containers core geometry-bvh geometry-mesh geometry-polygon geometry-primitives math memory units
    TESTS geometry/geometry-mesh-processing)
crd_module(engine/geometry/geometry-delaunay engine/geometry-delaunay
    DEPENDS containers core geometry-polygon geometry-primitives math memory units TESTS geometry/geometry-delaunay)
crd_module(engine/geometry/geometry-decomposition engine/geometry-decomposition
    DEPENDS containers core geometry-convex geometry-mesh geometry-primitives jobs math memory units
    TESTS support/test_helpers geometry/geometry-decomposition TEST_DEPENDS perf time)
crd_module(engine/geometry/geometry-bvh-gpu engine/geometry-bvh-gpu
    DEPENDS containers core geometry-bvh geometry-primitives gpu-context jobs math memory units
    TESTS support/test_helpers geometry/geometry-bvh-gpu TEST_DEPENDS gpu-context-vulkan perf platform time)
crd_module(engine/geometry/geometry-shader-helpers engine/geometry-shader-helpers
    DEPENDS containers core geometry-primitives math memory platform
    TESTS support/test_helpers geometry/geometry-shader-helpers TEST_DEPENDS gpu-context gpu-context-vulkan perf)
crd_module(engine/geometry/geometry-curves engine/geometry-curves
    DEPENDS containers core geometry-primitives math memory units TESTS support/test_helpers geometry/geometry-curves
    TEST_DEPENDS perf time)
# GEO-1 (D-007 row 66): OUR OWN external-format parsers (STL/OBJ/PLY/glTF/3MF) -> ImportedAsset seam -> decompose/cook
crd_module(engine/assets/asset-io engine/asset-io
    DEPENDS containers core geometry-bvh geometry-mesh geometry-primitives math memory time units
    TESTS assets/asset-io TEST_DEPENDS resources TEST_PACKAGES mikktspace)
crd_module(engine/numerics/hesap engine/hesap DEPENDS containers core math memory TESTS numerics/hesap)
# v12; special functions + orthogonal polynomials (leaf; substrate for stats/quadrature/dsp); ADR-0094
crd_module(engine/numerics/hesap-special engine/hesap-special DEPENDS containers core hesap jobs math memory
    TESTS numerics/hesap-special)
crd_module(engine/numerics/hesap-dense engine/hesap-dense DEPENDS containers core hesap jobs math memory
    TESTS numerics/hesap-dense)
# v12-c; Gauss quadrature via Golub-Welsch (REUSES hesap-dense eig_sym + hesap-special recurrences); ADR-0094
crd_module(engine/numerics/hesap-quadrature engine/hesap-quadrature
    DEPENDS containers core hesap hesap-dense hesap-special memory TESTS numerics/hesap-quadrature)
# v1a; links crd-jobs (declared below — CMake resolves target names at generation)
crd_module(engine/numerics/hesap-sparse engine/hesap-sparse DEPENDS containers core hesap jobs math memory
    TESTS numerics/hesap-sparse)
# v2; fill-reducing reorderings + symbolic factorisation (depends on hesap-sparse)
crd_module(engine/numerics/hesap-ordering engine/hesap-ordering DEPENDS containers core hesap hesap-sparse memory
    TESTS numerics/hesap-ordering)
# v4-corpus; matrices as cooked resources (depends on hesap-sparse + resources)
crd_module(engine/numerics/hesap-resources engine/hesap-resources
    DEPENDS containers core hesap hesap-sparse hesap-tensor math memory resources TESTS numerics/hesap-resources)
# v4a; Krylov solvers (CG/PCG) over LinearOp
crd_module(engine/numerics/hesap-iterative engine/hesap-iterative
    DEPENDS containers core hesap hesap-dense hesap-sparse math memory TESTS numerics/hesap-iterative
    TEST_DEPENDS hesap-preconditioners)
# v4a; Jacobi/SSOR preconditioners (one-way dep on hesap-iterative)
crd_module(engine/numerics/hesap-preconditioners engine/hesap-preconditioners
    DEPENDS containers core hesap hesap-dense hesap-iterative hesap-ordering hesap-sparse math memory)
# v4k; smoothed-aggregation AMG (LinearOp; one-way, Krylov consumes it)
crd_module(engine/numerics/hesap-amg engine/hesap-amg
    DEPENDS containers core hesap hesap-dense hesap-preconditioners hesap-sparse math memory TESTS numerics/hesap-amg)
crd_module(engine/foundation/containers engine/containers DEPENDS core memory TESTS foundation/containers)
crd_module(engine/foundation/toml engine/toml DEPENDS containers core TESTS foundation/toml)
# RAF-1 (D-007 "RAF band"): canonical render-asset identity/namespaces/diagnostics/deps (deps: core/containers/memory)
crd_module(engine/rendering/render-asset-core engine/render-asset-core DEPENDS containers core memory
    TESTS rendering/render-asset-core)
# RAF-4 (D-007 "RAF band"): the shader + program contract (deps: render-asset-core)
crd_module(engine/rendering/render-program engine/render-program DEPENDS containers core memory render-asset-core
    TESTS rendering/render-program)
# RAF-5 (D-007 "RAF band"): the material + technique contract (deps: render-asset-core, render-program)
crd_module(engine/rendering/render-material engine/render-material
    DEPENDS containers core memory render-asset-core render-program TESTS rendering/render-material)
# RAF-6 (D-007 "RAF band"): the pass-executor registry (deps: render-asset-core)
crd_module(engine/rendering/render-pass engine/render-pass DEPENDS containers core memory render-asset-core
    TESTS rendering/render-pass)
# RAF-7 (D-007 "RAF band"): the unified frame-graph runtime (deps: render-asset-core, render-pass, gpu-context)
crd_module(engine/rendering/render-graph engine/render-graph
    DEPENDS ceir-gpu containers core gpu-context memory render-asset-core render-pass TESTS rendering/render-graph
    TEST_DEPENDS gpu-context-dx12 gpu-context-vulkan)
crd_module(engine/foundation/log engine/log DEPENDS containers core TESTS foundation/log)
crd_module(engine/foundation/platform engine/platform DEPENDS containers core jobs log memory time PACKAGES glfw
    TESTS foundation/platform)
crd_module(engine/foundation/app engine/app DEPENDS containers core jobs platform TESTS foundation/app)
crd_module(engine/foundation/config engine/config DEPENDS containers core log math platform toml
    TESTS foundation/config)
crd_module(engine/ui/imgui engine/imgui
    DEPENDS config gpu-context gpu-context-dx12 gpu-context-vulkan log platform units PACKAGES glfw imgui
    TESTS ui/imgui)
# D-003 v0g -- ImGui frontend for crd-perf; needs crd-imgui above
crd_module(engine/ui/perf-ui engine/perf-ui DEPENDS core imgui memory perf PACKAGES imgui TESTS ui/perf-ui)
# REN-40-C1: the LOD chain builder (resources x mesh-processing seam)
crd_module(engine/geometry/lod engine/lod DEPENDS containers core geometry-mesh-processing math memory resources toml
    TESTS geometry/lod TEST_DEPENDS scene-render)
crd_module(engine/geometry/meshgen engine/meshgen DEPENDS core math memory resources TESTS geometry/meshgen)
# ADR-0099 v17-i-a: backend-agnostic GPU context foundation
crd_module(engine/gpu/gpu-context engine/gpu-context DEPENDS containers core render-asset-core TESTS gpu/gpu-context
    TEST_DEPENDS gpu-context-dx12 gpu-context-vulkan kir)
# ADR-0099 v17-i-a: headless Vulkan compute context
crd_module(engine/gpu/gpu-context-vulkan engine/gpu-context-vulkan
    DEPENDS containers core gpu-context kir memory platform TESTS gpu/gpu-context-vulkan
    TEST_DEPENDS asset_cooker ceir ceir-gpu draw frame-cook imgui scene-render shader-cook vertex-cook
    TEST_PACKAGES imgui)
# ADR-0100 v17-i: second IComputeContext backend (D3D12); Windows-only
crd_module(engine/gpu/gpu-context-dx12 engine/gpu-context-dx12 DEPENDS containers core gpu-context kir memory
    TESTS gpu/gpu-context-dx12 TEST_DEPENDS ceir ceir-gpu frame-cook shader-cook vertex-cook)
# ADR-0100 (2026-08-07, user-directed): third IComputeContext backend (CUDA); self-skips without CUDAToolkit
crd_module(engine/gpu/gpu-context-cuda engine/gpu-context-cuda DEPENDS containers core gpu-context memory
    TESTS gpu/gpu-context-cuda execution/ceir-gpu-cuda TEST_DEPENDS ceir ceir-gpu kir)
crd_module(engine/foundation/jobs engine/jobs DEPENDS containers core TESTS foundation/jobs)
# depends on crd-jobs above + crd-hesap (declared earlier)
crd_module(engine/numerics/hesap-sched engine/hesap-sched DEPENDS containers core hesap jobs memory
    TESTS numerics/hesap-sched)
# v5; sparse DIRECT factorization (depends on hesap-dense/ordering/sparse/sched)
crd_module(engine/numerics/hesap-direct engine/hesap-direct
    DEPENDS containers core hesap hesap-dense hesap-iterative hesap-ordering hesap-sched hesap-sparse jobs math memory
    TESTS numerics/hesap-direct)
# v6; sparse EIGENVALUE, matrix-free (Rayleigh-Ritz via hesap-dense; shift-invert via hesap-direct)
crd_module(engine/numerics/hesap-eigen engine/hesap-eigen
    DEPENDS containers core hesap hesap-dense hesap-direct hesap-iterative hesap-sparse jobs math memory
    TESTS numerics/hesap-eigen TEST_DEPENDS hesap-amg hesap-preconditioners)
# v15/16; AUTODIFF — forward Dual/Jet + reverse tape; Dual migrated from opt (v15-a); ADR-0097
crd_module(engine/numerics/hesap-autodiff engine/hesap-autodiff DEPENDS containers core hesap hesap-tensor jobs math memory
    TESTS numerics/hesap-autodiff)
# v17; CERID KERNEL IR (CKIR) — the GPU compute compiler substrate (IR -> tile -> 6 backends); ADR-0098
crd_module(engine/gpu/kir engine/kir DEPENDS containers core math memory TESTS gpu/kir support/fuzz)
# v17-b; CKIR Vulkan backend (KirBackend over gpu-context); ADR-0098
crd_module(engine/gpu/kir-vulkan engine/kir-vulkan DEPENDS containers core gpu-context gpu-context-vulkan kir memory
    TESTS gpu/kir-vulkan execution/ceir-gpu-vulkan TEST_DEPENDS ceir ceir-cook ceir-gpu hesap-autodiff)
# v17-c; CKIR CUDA backend (KirBackend over the CUDA driver + NVRTC); guarded on CUDAToolkit; ADR-0098
crd_module(engine/gpu/kir-cuda engine/kir-cuda DEPENDS containers core kir memory TESTS gpu/kir-cuda)
# v17-d; CKIR DirectX 12 backend (KirBackend over D3D12 + dxc); Windows-only; ADR-0098
crd_module(engine/gpu/kir-dx12 engine/kir-dx12 DEPENDS containers core kir memory
    TESTS gpu/kir-dx12 execution/ceir-gpu-dx12 TEST_DEPENDS ceir ceir-gpu gpu-context-dx12 hesap-autodiff)
# v17-d; CKIR WebGPU backend (KirBackend over wgpu-native); browser/everywhere; guarded on the prebuilt; ADR-0098
crd_module(engine/gpu/kir-webgpu engine/kir-webgpu DEPENDS containers core kir memory TESTS gpu/kir-webgpu)
# v17-d; CKIR HIP/ROCm backend (KirBackend over HIP + hiprtc); guarded on the HIP SDK; validated at Part C; ADR-0098
crd_module(engine/gpu/kir-hip engine/kir-hip DEPENDS containers core kir memory TESTS gpu/kir-hip)
# v17-d; CKIR Metal backend (KirBackend over Apple Metal, Objective-C++); APPLE-only; validated at Part C; ADR-0098
crd_module(engine/gpu/kir-metal engine/kir-metal DEPENDS containers core kir memory TESTS gpu/kir-metal)
# REN-36 (D-007 row 139): the AUTHORABLE FRAME GRAPH — render passes,
crd_module(engine/assets/frame-cook engine/frame-cook
    DEPENDS ceir ceir-gpu containers core gpu-context log memory render-asset-core render-graph render-pass toml
    TESTS assets/frame-cook TEST_DEPENDS platform)
# REN-37 (D-007 row 140): the LIGHTING TECHNIQUE as an authored `.crdt`
crd_module(engine/assets/technique-cook engine/technique-cook DEPENDS containers core frame-cook kir memory toml
    TESTS assets/technique-cook)
# REN-38-C (D-007 row 141): the MATERIAL as an authored `.crdm`
crd_module(engine/assets/material-cook engine/material-cook DEPENDS containers core kir memory toml
    TESTS assets/material-cook)
# REN-38-D (D-007 row 141): the VERTEX PROGRAM as an authored `.crdv`
crd_module(engine/assets/vertex-cook engine/vertex-cook DEPENDS containers core kir material-cook memory toml
    TESTS assets/vertex-cook)
# REN-38-E (D-007 row 141): the LIGHTING VOCABULARY as an authored asset
crd_module(engine/assets/light-cook engine/light-cook DEPENDS containers core kir memory toml
    TESTS assets/light-cook)
# D-007 D2; OFFLINE shader cook — CKIR graph -> .crdr bundle (IR + reflection + per-backend blobs); ADR-0104
crd_module(engine/assets/shader-cook engine/shader-cook
    DEPENDS containers core gpu-context gpu-context-dx12 gpu-context-vulkan jobs kir memory platform resources)
# v7; OPTIMISATION (unconstrained + constrained) over Objective<T>; ADR-0090 (now consumes hesap-autodiff)
crd_module(engine/numerics/hesap-opt engine/hesap-opt
    DEPENDS containers core hesap hesap-autodiff hesap-dense hesap-direct hesap-sparse hesap-stats jobs math memory
    TESTS numerics/hesap-opt TEST_DEPENDS hesap-ordering)
# v12 cluster (created early as a v12-pull: Philox counter-RNG for v7-i)
crd_module(engine/numerics/hesap-stats engine/hesap-stats
    DEPENDS containers core hesap-dense hesap-quadrature hesap-special jobs memory TESTS numerics/hesap-stats)
# v9; ODE/DAE — stepper kernel headers + driver substrate; ADR-0091
crd_module(engine/numerics/hesap-ode engine/hesap-ode
    DEPENDS containers core hesap hesap-dense hesap-direct hesap-iterative hesap-sparse memory
    TESTS numerics/hesap-ode)
# v10; FFT — Complex<T> transforms; gold standards FFTW/PocketFFT/MKL
crd_module(engine/numerics/hesap-fft engine/hesap-fft DEPENDS containers core hesap math memory
    TESTS numerics/hesap-fft)
# v11; DSP — filters/spectral/multirate (depends on hesap-fft/eigen/units); ADR-0093
crd_module(engine/numerics/hesap-dsp engine/hesap-dsp
    DEPENDS containers core hesap hesap-eigen hesap-fft hesap-special jobs math memory units TESTS numerics/hesap-dsp)
# v11w; WAVELETS — DWT/IDWT/SWT/WPT/CWT (depends on hesap-fft); ADR-0093
crd_module(engine/numerics/hesap-wavelet engine/hesap-wavelet DEPENDS containers core hesap hesap-fft jobs math memory
    TESTS numerics/hesap-wavelet)
# v11c; COMMS/SDR — modulation/sync/equalizers/OFDM (depends on hesap-dsp/fft/stats); ADR-0093
crd_module(engine/numerics/hesap-comms engine/hesap-comms
    DEPENDS containers core hesap hesap-dsp hesap-fft hesap-stats math memory TESTS numerics/hesap-comms)
# v13; INTERPOLATION — 1-D piecewise/spline/poly/rational + scattered N-D; ADR-0095
crd_module(engine/numerics/hesap-interp engine/hesap-interp
    DEPENDS containers core geometry-delaunay hesap hesap-dense hesap-fft math memory TESTS numerics/hesap-interp)
# v13-l/m; NUMERICAL DIFFERENTIATION — Fornberg/Ridders/complex-step/SavGol/spectral; ADR-0095
crd_module(engine/numerics/hesap-diff engine/hesap-diff
    DEPENDS containers core hesap hesap-dense hesap-fft math memory TESTS numerics/hesap-diff)
# v13-n/o/p/q; TRAJECTORY GENERATION — SQUAD/clothoid/NURBS/min-snap/S-curve/TCB; ADR-0095
crd_module(engine/numerics/hesap-motion engine/hesap-motion
    DEPENDS containers core hesap hesap-dense hesap-special math memory TESTS numerics/hesap-motion)
# v14; TENSORS — the N-D substrate (views/broadcast/einsum/sparse/decomp/nn); ADR-0096
crd_module(engine/numerics/hesap-tensor engine/hesap-tensor DEPENDS containers core jobs math memory platform
    TESTS numerics/hesap-tensor TEST_DEPENDS hesap hesap-autodiff hesap-dense hesap-resources)
crd_module(engine/assets/resources engine/resources DEPENDS containers core jobs log memory platform PACKAGES zstd
    TESTS assets/resources TEST_DEPENDS asset_cooker gpu-context gpu-context-vulkan)
crd_module(engine/world/scene engine/scene
    DEPENDS containers core geometry-primitives geometry-spatial math memory resources
    TESTS foundation/stress world/scene TEST_DEPENDS preset profile)
crd_module(engine/assets/preset engine/preset DEPENDS containers core memory resources TESTS assets/preset)
crd_module(engine/foundation/profile engine/profile DEPENDS containers core memory resources TESTS foundation/profile)
crd_module(engine/physics/eylem engine/eylem DEPENDS containers core math memory TESTS physics/eylem)
crd_module(engine/physics/eylem-rigid3d engine/eylem-rigid3d DEPENDS containers core eylem log math memory scene
    TESTS physics/eylem-rigid3d)
# GEO-8 (D-007 row 73): SKEL/ANIM resources + glTF-exact clip sampling on the hesap-interp curve engine
crd_module(engine/world/anim engine/anim DEPENDS containers core hesap-interp math memory resources TESTS world/anim
    TEST_DEPENDS asset-io asset_cooker)
# GEO-9 (D-007 row 74): the TIMELINE resource — OTIO-shaped, RATIONAL time, deterministic eval, EXR-sequence render
crd_module(engine/world/timeline engine/timeline DEPENDS containers core hesap-interp memory resources time
    TESTS world/timeline)
# GEO-10 (D-007 row 75): the DAW substrate — own WAV/AIFF/FLAC, audio-graph resource, MIDI 2.0, realtime
crd_module(engine/media/audio engine/audio
    DEPENDS ceir containers core hesap-dsp hesap-interp jobs memory resources time TESTS media/audio)
crd_module(engine/rendering/draw engine/draw
    DEPENDS containers core gpu-context gpu-context-vulkan kir log material-cook math memory scene vertex-cook
    TESTS rendering/draw)
# GEO-7 (D-007 row 72): scene <-> ECS <-> renderer integration on gpu-context (chunk-grain extract/cull/submit)
crd_module(engine/rendering/scene-render engine/scene-render
    DEPENDS anim containers core frame-cook geometry-primitives gpu-context kir light-cook lod log material-cook math
        memory render-asset-core resources scene vertex-cook TESTS rendering/scene-render
    TEST_DEPENDS gpu-context-dx12 gpu-context-vulkan meshgen)
crd_module(engine/rendering/draw-imgui engine/draw-imgui DEPENDS core draw imgui PACKAGES imgui)
crd_module(engine/physics/eylem-viz engine/eylem-viz DEPENDS core draw eylem eylem-rigid3d math scene
    TESTS physics/eylem-viz)
# The sandbox showcase tests link geometry-viz, draw and imgui, not the sandbox executable, so they stay owned here
# and build whether or not CRD_BUILD_SANDBOX is on.
crd_module(engine/geometry/geometry-viz engine/geometry-viz
    DEPENDS containers core draw geometry-bvh geometry-curves geometry-primitives math
    TESTS geometry/geometry-viz applications/sandbox TEST_DEPENDS imgui TEST_PACKAGES imgui)
# add_subdirectory(engine/graphics)
# add_subdirectory(engine/scripting)

# Host tools: cookers and generators run on the build host; CRD_HOST_TOOLS_DIR imports prebuilt executables
# instead (a target build consuming a host build), so host/target separation is declared here, not implied.
crd_module(tools/asset_cooker HOST EXECUTABLES asset_cooker
    DEPENDS anim asset-io audio containers core frame-cook gpu-context gpu-context-vulkan light-cook log material-cook
        math memory platform preset profile resources scene technique-cook timeline vertex-cook toml
    TESTS assets/scene_cooker assets/cooker)
# GEO-11 (D-007 row 76): the agent surface — CLI verbs + MCP stdio server
crd_module(tools/ceridc HOST EXECUTABLES ceridc
    DEPENDS asset-io asset_cooker audio containers core memory platform resources scene time timeline
    TESTS tools/ceridc)
# D-007 D2: the offline shader-cook CLI (.kgph -> .crdr); ADR-0104
crd_module(tools/shader-cook NAME shader_cook HOST EXECUTABLES shader_cook
    DEPENDS containers core kir memory platform resources shader-cook)
# ADR-0098 §4 AS-2b: the offline auto-scheduler CLI (GEMM schedule search -> tuning DB)
crd_module(tools/kir-autotune NAME kir_autotune HOST EXECUTABLES kir_autotune
    DEPENDS containers core kir kir-cuda memory)

crd_module(runtime
    DEPENDS app config containers core geometry-primitives hesap hesap-amg hesap-dense hesap-direct hesap-eigen
        hesap-iterative hesap-ode hesap-opt hesap-ordering hesap-preconditioners hesap-resources hesap-sched
        hesap-sparse hesap-special hesap-stats hesap-tensor jobs log math memory meshgen platform resources vm
    TESTS bench TEST_DEPENDS geometry-bvh geometry-convex)

if(CRD_BUILD_SANDBOX)
    # asset_cooker is a build-time dependency: the demo pack is cooked by $<TARGET_FILE:asset_cooker>.
    crd_module(sandbox
        DEPENDS anim app asset_cooker draw geometry-spatial gpu-context gpu-context-dx12 gpu-context-vulkan imgui jobs kir
            log memory perf perf-ui resources scene scene-render timeline
        PACKAGES glfw imgui)
endif()

crd_resolve_modules()

# ----- Dependencies (via CPM) ----------------------------------------------
# Each package is acquired only when a selected module declares it (CRD_SELECTED_PACKAGES); the full
# configuration selects every package, so the default acquisition is unchanged.

include(cmake/CPM.cmake)

if (CRD_BUILD_TESTS)
    # SYSTEM and EXCLUDE_FROM_ALL are what CPM's single-argument shorthand implied before the pins registry; the long
    # form implies neither, and the first complete-tier run (34821419392) failed clang-cl on a warning inside a Catch2
    # header once its includes stopped being system includes. Third-party headers never fail the warning gate.
    crd_add_pinned_package(Catch2 SYSTEM YES EXCLUDE_FROM_ALL YES)
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()

if("glfw" IN_LIST CRD_SELECTED_PACKAGES)
    # GLFW: window + input backend for crd-platform.
    # We pin the 3.4 tag and disable everything we don't ship.
    # GLFW 3.4 on Linux: disable the X11 backend so its GLX context code (which needs
    # GL/gl.h) is never compiled. Wayland packages are installed in CI and suffice for
    # headless test runs. X11 users can opt back in by overriding GLFW_BUILD_X11.
    set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
        set(GLFW_BUILD_X11     OFF CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND ON  CACHE BOOL "" FORCE)
    endif()
    crd_add_pinned_package(glfw SYSTEM YES EXCLUDE_FROM_ALL YES) # the shorthand's implied options, see Catch2
endif()

if("zstd" IN_LIST CRD_SELECTED_PACKAGES)
    # zstd 1.5.5 has cmake_minimum_required(VERSION 2.8.12.2) which modern CMake rejects.
    # Setting CMAKE_POLICY_VERSION_MINIMUM tells CMake to treat it as 3.5.
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
    crd_add_pinned_package(zstd
        SOURCE_SUBDIR build/cmake
        OPTIONS
            "ZSTD_BUILD_PROGRAMS OFF"
            "ZSTD_BUILD_TESTS OFF"
            "ZSTD_BUILD_SHARED OFF"
            "ZSTD_BUILD_STATIC ON"
    )
    unset(CMAKE_POLICY_VERSION_MINIMUM)

    # Phase 3.1.7.6 v0-close (2026-05-17) — disable IPO/LTCG on the
    # vendored zstd_static target. MSVC's LTCG link step recurrently
    # emits a C1001 internal compiler error at `zstd_decompress_block.c`
    # call-sites when LTCG-linked into `crd-sandbox.exe`
    # (`link!DllGetObjHandler()+0xc2654`) in the win-shipping-profile
    # preset. We can't `CRD_NOINLINE` third-party code, and the upstream
    # MSVC bug has no known fix. Disabling IPO on zstd's objects means
    # they're not `/GL`-tagged; the final link still uses `/LTCG` for
    # Cerid's own code but skips the un-tagged zstd objects — no link-
    # time codegen happens for zstd, breaking the ICE chain. Cerid's own
    # LTCG benefits (cross-TU inlining in the engine) are unaffected.
    if(TARGET zstd_static)
        set_target_properties(zstd_static PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    endif()
endif()

if("stb" IN_LIST CRD_SELECTED_PACKAGES)
    # stb: single-header image/font/audio utilities (DOWNLOAD_ONLY — no cmake build).
    # Pin the previously checked-out revision: configure must not follow a moving branch.
    crd_add_pinned_package(stb DOWNLOAD_ONLY YES)
    add_library(stb INTERFACE)
    target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
endif()

if("cgltf" IN_LIST CRD_SELECTED_PACKAGES)
    # cgltf: single-header glTF 2.0 parser (DOWNLOAD_ONLY — no cmake build).
    # Phase 2.7: used by the glTF mesh cook handler.
    crd_add_pinned_package(cgltf DOWNLOAD_ONLY YES)
    add_library(cgltf INTERFACE)
    target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")
endif()

if("mikktspace" IN_LIST CRD_SELECTED_PACKAGES)
    # MikkTSpace: tangent-space generation (two C files, MIT license).
    # Phase 2.7: used by the glTF mesh cook handler to generate tangents when glTF
    # meshes omit TANGENT data. Compiled as a STATIC C99 library.
    crd_add_pinned_package(MikkTSpace DOWNLOAD_ONLY YES)
    add_library(mikktspace INTERFACE)
    target_include_directories(mikktspace SYSTEM INTERFACE "${MikkTSpace_SOURCE_DIR}")
    # mikktspace.c is included directly inside mesh.cpp (within an extern "C" block)
    # so it compiles as C++ without triggering C-compiler detection in CMake.
endif()

if("imgui" IN_LIST CRD_SELECTED_PACKAGES)
    # The Vulkan backend's texture-upload flush is patched into a build-owned copy by engine/ui/imgui
    # (cmake/patches/imgui-vulkan-whole-size.cmake); the downloaded tree stays pristine.
    crd_add_pinned_package(imgui SYSTEM YES EXCLUDE_FROM_ALL YES) # the shorthand's implied options, see Catch2
endif()

# ----- Reference-class shootout deps (Phase 3.1.6 v0d-parallelism-scaling) -----
# Gated behind CRD_BUILD_HESAP_VS_REFERENCE — fetched on demand into build/_deps,
# never vendored into git. Both deps are bench-only.
if(CRD_BUILD_HESAP_VS_REFERENCE)
    message(STATUS "[hesap-vs-ref] Fetching Eigen 3.4 + OpenBLAS for shootout bench")

    # Eigen 3.4 — header-only template library. DOWNLOAD_ONLY since we only need
    # headers; we wrap them in a SYSTEM-include interface target.
    crd_add_pinned_package(eigen DOWNLOAD_ONLY YES)
    add_library(eigen_headers INTERFACE)
    target_include_directories(eigen_headers SYSTEM INTERFACE "${eigen_SOURCE_DIR}")
    # Eigen's parallel path: -DEIGEN_DONT_PARALLELIZE turns it OFF. We want it ON,
    # via OpenMP (Eigen has no native thread pool). On MSVC we pass /openmp.
    target_compile_definitions(eigen_headers INTERFACE EIGEN_USE_THREADS)
    find_package(OpenMP COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        target_link_libraries(eigen_headers INTERFACE OpenMP::OpenMP_CXX)
    endif()

    # OpenBLAS — built from source via FetchContent. Pin to a recent stable tag.
    # CRITICAL: OpenBLAS auto-detection on MSVC fails (no /proc/cpuinfo, no
    # assembler) and falls back to CORE_generic = a 2x2 scalar-C kernel that
    # runs at ~10 GFLOPS — not the asm-tuned reference we need. Force
    # TARGET=SKYLAKEX which has C-intrinsic AVX-512/AVX2 kernels that build
    # under MSVC without an external assembler. SKYLAKEX downgrades to AVX2
    # at runtime if AVX-512 is absent. DYNAMIC_ARCH=ON would build multiple
    # kernels and runtime-dispatch but takes 30+ min to build; we pin a
    # single target for bench parity instead.
    set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
    # OpenBLAS-on-MSVC is constrained to its CORE_generic 2x2 scalar kernel:
    # upstream prebuild.cmake hardcodes -DFORCE_GENERIC under MSVC, and even
    # patching that out leaves us with HASWELL .S kernels (GAS syntax) that
    # ml64 can't parse. Real-asm OpenBLAS requires Linux/MinGW or vcpkg
    # prebuilt. We accept the generic build as a baseline and document the
    # limitation — see docs/sessions/2026-05-19-hesap-vs-reference.md.
    # LAPACK is ENABLED as of v3 (SVD + dense-eigenvalue reference). OpenBLAS
    # bundles the f2c-translated C LAPACK (lapack-netlib): with C_LAPACK=ON +
    # NOFORTRAN=ON it builds under MSVC with NO Fortran toolchain. This gives us
    # the gold-standard accuracy oracle + the "floor" reference for v3
    # (dsyev/dsyevd/dsyevr, dgesvd/dgesdd, dgeev/dgehrd/dhseqr/dtrevc,
    # dgels/dgelsd) AND the reference source in build/_deps/ for the per-sub-slice
    # study pass (lapack-netlib/SRC/*.c + the netlib *.f). Caveat: same as the
    # BLAS kernels — LAPACK speed on MSVC rides the generic BLAS path, so it is an
    # accuracy oracle on Windows and a fair *speed* bench only on Linux CI.
    # Building LAPACK adds several minutes to this (gated, opt-in) OpenBLAS build;
    # the default DoD builds are unaffected (CRD_BUILD_HESAP_VS_REFERENCE is OFF
    # by default).
    set(BUILD_WITHOUT_LAPACK   OFF CACHE BOOL   "" FORCE)  # was ON pre-v3; flipped for eig/SVD reference
    set(NOFORTRAN              ON  CACHE BOOL   "" FORCE)   # use the f2c'd C LAPACK; no Fortran toolchain
    set(C_LAPACK               ON  CACHE BOOL   "" FORCE)   # compile lapack-netlib f2c C sources
    set(NO_LAPACKE             OFF CACHE BOOL   "" FORCE)   # build the LAPACKE C interface too
    set(BUILD_TESTING          OFF CACHE BOOL   "" FORCE)
    set(USE_THREAD             1   CACHE STRING "" FORCE)
    set(NUM_THREADS            32  CACHE STRING "" FORCE)
    set(_crd_saved_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
    set(BUILD_SHARED_LIBS      OFF CACHE BOOL   "" FORCE)
    crd_add_pinned_package(OpenBLAS)
    set(BUILD_SHARED_LIBS ${_crd_saved_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# ----- Helper: auto-collect sources with CONFIGURE_DEPENDS -----------------

function(crd_collect_sources out_var directory)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${directory}/*.cpp"
    "${directory}/*.hpp"
    "${directory}/*.h"
    )
    source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${sources})
    set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

function(crd_enable_default_pch target_name)
    if(NOT CRD_ENABLE_PCH)
        return()
    endif()

    target_precompile_headers(${target_name} PRIVATE
        <crd/core/pch.hpp>
    )
endfunction()

find_program(CRD_GLSLANG_VALIDATOR NAMES glslangValidator HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin")

function(crd_compile_glsl out_var output_dir)
    if(NOT CRD_GLSLANG_VALIDATOR)
        message(FATAL_ERROR "glslangValidator not found; required for Vulkan shader compilation")
    endif()

    file(MAKE_DIRECTORY "${output_dir}")
    set(outputs)
    foreach(shader IN LISTS ARGN)
        get_filename_component(shader_name "${shader}" NAME)
        set(spv "${output_dir}/${shader_name}.spv")
        add_custom_command(
            OUTPUT "${spv}"
            COMMAND "${CRD_GLSLANG_VALIDATOR}" -V --target-env vulkan1.3 -o "${spv}" "${shader}"
            DEPENDS "${shader}"
            VERBATIM
        )
        list(APPEND outputs "${spv}")
    endforeach()
    set(${out_var} ${outputs} PARENT_SCOPE)
endfunction()

# ---- CMAKE Tidy ---------------------------------------------------------
if(CRD_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(CLANG_TIDY_EXE)
        # ⛔ SCAR (2026-07-25): clang-tidy SILENTLY DROPS every `/`-spelled MSVC flag that reaches it through
        # the compile command (`cmake -E __run_co_compile --tidy=... -- cl.exe /EHsc /arch:AVX2 /DWIN32 ...`).
        # Measured with a `#error`-guarded probe through that exact CMake path: `/EHsc`, `/arch:AVX2` and
        # `/D<macro>` never reach the TU, while the SAME flags passed as `--extra-arg=` do. (CMake already
        # spells `-I` and `-D<user macro>` with dashes, which is why most of the build looked fine.) Two
        # silent consequences:
        #   1. Exceptions look DISABLED, so any `try` is a hard `clang-diagnostic-error` — which is exactly
        #      why `engine/foundation/platform/src/filesystem.cpp` (the std::filesystem noexcept wrappers) failed the
        #      win-tidy build. A real, deterministic failure that sat hidden behind an unrelated crash.
        #   2. `__AVX2__` is UNDEFINED, so every AVX2-guarded SIMD path is preprocessed out and NEVER
        #      ANALYSED — the gate reads green on code it never looked at. Same disease as the
        #      "tidy gate clean on unparsed files" scar (see scripts/cerid_dev/tidy.py, which restates the same flags).
        # Fix: re-state the analysis-relevant flags through `--extra-arg`, the one channel clang-tidy honors.
        # The ISA flag comes from CRD_SIMD_MSVC_ARCH_FLAG — set by the same CrdSimd.cmake branch that adds the
        # real compile option — so the two cannot drift apart.
        set(_crd_tidy_extra_args "")
        if(MSVC)
            list(APPEND _crd_tidy_extra_args --extra-arg=/EHsc)
            if(CRD_SIMD_MSVC_ARCH_FLAG)
                list(APPEND _crd_tidy_extra_args --extra-arg=${CRD_SIMD_MSVC_ARCH_FLAG})
            endif()
        endif()
        if(CRD_CLANG_TIDY_WARNINGS_AS_ERRORS)
            # Local strict mode: .clang-tidy's `WarningsAsErrors: '*'` is honored.
            set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE} ${_crd_tidy_extra_args})
            message(STATUS "[crd] clang-tidy ENABLED (warnings-as-errors=ON; strict mode)")
        else()
            # Explicit local diagnostic preview only. This mode cannot qualify a slice;
            # CI selects pinned LLVM and requires warnings-as-errors=ON.
            set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE} ${_crd_tidy_extra_args} --warnings-as-errors=-*)
            message(STATUS "[crd] clang-tidy diagnostic preview only (warnings-as-errors=OFF)")
        endif()
    else()
        message(FATAL_ERROR "CRD_ENABLE_CLANG_TIDY requires a clang-tidy executable; analysis cannot silently skip.")
    endif()
endif()


# ----- Engine modules, host tools, runtime and sandbox ----------------------

crd_add_modules()
crd_add_public_header_checks()
crd_package_config()

# Optional 'cook' target: cmake --build --preset win-debug --target cook
# Requires CRD_COOK_ROOT and CRD_COOK_OUT to be set (e.g. via -D flags).
if(CRD_COOK_ROOT AND CRD_COOK_OUT)
    add_custom_target(cook
        COMMAND asset_cooker cook --root "${CRD_COOK_ROOT}" --out "${CRD_COOK_OUT}"
        DEPENDS asset_cooker
        COMMENT "Cooking assets from ${CRD_COOK_ROOT} into ${CRD_COOK_OUT}"
    )
endif()

# ----- Tests ----------------------------------------------------------------

if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
crd_add_package_consumer_test()

crd_verify_modules()
crd_apply_project_structure()
if(CRD_PROJECT_SYNC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    add_custom_target(crd-project-sync-check
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/project-sync.py"
                --root "${CMAKE_SOURCE_DIR}" verify --build "${CMAKE_BINARY_DIR}"
        COMMENT "Verify durable source/CMake/Visual Studio project structure"
        VERBATIM
    )
endif()
crd_apply_native_profiles("${PROJECT_SOURCE_DIR}")
crd_organize_targets("${CMAKE_CURRENT_SOURCE_DIR}")
if(TARGET crd-sandbox)
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT crd-sandbox)
endif()
