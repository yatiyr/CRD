cmake_minimum_required(VERSION 3.25)

project(CRD
    VERSION 0.1.0
    DESCRIPTION "CRD Game/Simulation Engine"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
option(CRD_SHIPPING         "Maximum optimizations + debug symbol generation for crash diagnostics" OFF)
option(CRD_ENABLE_CLANG_TIDY "Run clang-tidy during build" OFF)
option(CRD_CLANG_TIDY_WARNINGS_AS_ERRORS "Treat clang-tidy warnings as build errors (local strict; CI relaxed via -DCRD_CLANG_TIDY_WARNINGS_AS_ERRORS=OFF)" ON)
option(CRD_ENABLE_PCH       "Enable default precompiled headers" ON)

# SIMD level + ADR-0063 deterministic-FP contract live in their own module.
# Any target that links crd-math (everything, transitively) inherits the
# /arch:AVX2 (or -mavx2 / NEON / scalar) flags + the determinism guarantees.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CrdSimd)

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
message(STATUS "[crd] Log compile-time min level: ${CRD_LOG_MIN_LEVEL_RESOLVED} (${CRD_LOG_MIN_LEVEL_NUM})")

# ---- NDEBUG for optimised configs ---------------------------------------
# CMake's MSVC toolchain may leave CMAKE_CXX_FLAGS_RELEASE empty (unlike the
# C toolchain), so NDEBUG is not always propagated automatically. Ensure the
# standard Release/MinSizeRel configs always get the define, matching the
# intent of every #if defined(NDEBUG) guard in the codebase.
add_compile_definitions(
    $<$<CONFIG:Release>:NDEBUG>
    $<$<CONFIG:MinSizeRel>:NDEBUG>
)

# ---- Sanitizers ---------------------------------------------------------

if(CRD_ENABLE_ASAN)
    if(MSVC)
        add_compile_options(/fsanitize=address)
    else()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif()
    message(STATUS "[crd] Address Sanitizer ENABLED")
endif()

if(CRD_ENABLE_UBSAN)
    if(MSVC)
        message(WARNING "[crd] UBSan not supported on MSVC, ignoring")
    else()
        add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=undefined)
        message(STATUS "[crd] UB Sanitizer ENABLED")
    endif()
endif()

# ----- Shipping optimizations + debug symbols --------------------------------

if(CRD_SHIPPING)
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
            $<$<COMPILE_LANGUAGE:CXX,C>:/Gw>
            $<$<COMPILE_LANGUAGE:CXX,C>:/Gy>
            $<$<COMPILE_LANGUAGE:CXX,C>:/Zc:inline>
            $<$<COMPILE_LANGUAGE:CXX,C>:/Zi>
        )
        # /DEBUG:FULL  — full PDB (not /DEBUG:FASTLINK), required for WinDbg post-mortem
        # /OPT:REF     — strip unreferenced functions (dead code elimination)
        # /OPT:ICF     — merge identical COMDATs (binary size reduction)
        # /LTCG        — link-time codegen (driven via CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        add_link_options(/DEBUG:FULL /OPT:REF /OPT:ICF)
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
    message(STATUS "[crd] Shipping: max optimizations + debug symbols ON (determinism preserved per ADR-0063)")
endif()

# ----- Compiler Warnings (Cerid targets only — propagates via crd-core) -----
# Third-party CPM packages never see these flags; they inherit them only
# if they transitively link crd-core, which they don't.

add_library(crd-warnings INTERFACE)

if(MSVC)
    # Gate flags on CXX/C only — MASM (ASM_MASM) does not accept these options.
    target_compile_options(crd-warnings INTERFACE
        $<$<COMPILE_LANGUAGE:CXX,C>:/W4>
        $<$<COMPILE_LANGUAGE:CXX,C>:/WX>
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
        $<$<COMPILE_LANGUAGE:CXX,C>:-Werror>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wconversion>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wshadow>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wdouble-promotion>
        $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor>
        $<$<COMPILE_LANGUAGE:CXX>:-Woverloaded-virtual>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wnull-dereference>
        $<$<COMPILE_LANGUAGE:CXX,C>:-Wformat=2>
    )
endif()

# ----- Dependencies (via CPM) ----------------------------------------------

include(cmake/CPM.cmake)

if (CRD_BUILD_TESTS)
    CPMAddPackage("gh:catchorg/Catch2@3.7.1")
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()

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
CPMAddPackage("gh:glfw/glfw#3.4")

CPMAddPackage("gh:marzer/tomlplusplus@3.4.0")

# zstd 1.5.5 has cmake_minimum_required(VERSION 2.8.12.2) which modern CMake rejects.
# Setting CMAKE_POLICY_VERSION_MINIMUM tells CMake to treat it as 3.5.
set(CMAKE_POLICY_VERSION_MINIMUM "3.5")
CPMAddPackage(
    NAME zstd
    GITHUB_REPOSITORY facebook/zstd
    GIT_TAG v1.5.5
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

# stb: single-header image/font/audio utilities (DOWNLOAD_ONLY — no cmake build).
# Phase 2.7: stb_image used by the texture cook handler. GIT_TAG master because
# stb has no semver tags (see docs/debt.md).
CPMAddPackage(
    NAME stb
    GITHUB_REPOSITORY nothings/stb
    GIT_TAG master
    DOWNLOAD_ONLY YES
)
add_library(stb INTERFACE)
target_include_directories(stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")

# cgltf: single-header glTF 2.0 parser (DOWNLOAD_ONLY — no cmake build).
# Phase 2.7: used by the glTF mesh cook handler.
CPMAddPackage(
    NAME cgltf
    GITHUB_REPOSITORY jkuhlmann/cgltf
    GIT_TAG v1.14
    DOWNLOAD_ONLY YES
)
add_library(cgltf INTERFACE)
target_include_directories(cgltf SYSTEM INTERFACE "${cgltf_SOURCE_DIR}")

# MikkTSpace: tangent-space generation (two C files, MIT license).
# Phase 2.7: used by the glTF mesh cook handler to generate tangents when glTF
# meshes omit TANGENT data. Compiled as a STATIC C99 library.
CPMAddPackage(
    NAME MikkTSpace
    GITHUB_REPOSITORY mmikk/MikkTSpace
    GIT_TAG master
    DOWNLOAD_ONLY YES
)
add_library(mikktspace INTERFACE)
target_include_directories(mikktspace SYSTEM INTERFACE "${MikkTSpace_SOURCE_DIR}")
# mikktspace.c is included directly inside mesh.cpp (within an extern "C" block)
# so it compiles as C++ without triggering C-compiler detection in CMake.

CPMAddPackage("gh:ocornut/imgui#v1.92.0-docking")

# ----- Reference-class shootout deps (Phase 3.1.6 v0d-parallelism-scaling) -----
# Gated behind CRD_BUILD_HESAP_VS_REFERENCE — fetched on demand into build/_deps,
# never vendored into git. Both deps are bench-only.
if(CRD_BUILD_HESAP_VS_REFERENCE)
    message(STATUS "[hesap-vs-ref] Fetching Eigen 3.4 + OpenBLAS for shootout bench")

    # Eigen 3.4 — header-only template library. DOWNLOAD_ONLY since we only need
    # headers; we wrap them in a SYSTEM-include interface target.
    CPMAddPackage(
        NAME eigen
        GITLAB_REPOSITORY libeigen/eigen
        GIT_TAG 3.4.0
        DOWNLOAD_ONLY YES
    )
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
    CPMAddPackage(
        NAME OpenBLAS
        GITHUB_REPOSITORY OpenMathLib/OpenBLAS
        GIT_TAG v0.3.27
    )
    set(BUILD_SHARED_LIBS ${_crd_saved_BUILD_SHARED_LIBS} CACHE BOOL "" FORCE)
    unset(CMAKE_POLICY_VERSION_MINIMUM)
endif()

# Phase 3.1.7 v1-close debt payment: patch ImGui's Vulkan backend so
# `vkFlushMappedMemoryRanges` in the texture-upload path doesn't trip the
# Vulkan validation layer's VUID-VkMappedMemoryRange-size-01390 (size must
# either be a multiple of `nonCoherentAtomSize` or `VK_WHOLE_SIZE`).
# Upstream uses `range[0].size = upload_size;` which is the raw unaligned
# upload size — we switch to `VK_WHOLE_SIZE` since the upload buffer is
# allocated single-use for that texture and a whole-flush is always safe.
# Idempotent: if the patch is already applied (re-configure with a populated
# build dir) the string-find misses and the write is a no-op. See
# docs/debt.md for the original diagnostic.
if(imgui_SOURCE_DIR)
    set(_crd_imgui_vk_file "${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp")
    if(EXISTS "${_crd_imgui_vk_file}")
        file(READ "${_crd_imgui_vk_file}" _crd_imgui_vk_text)
        # Patch 1 — flush size: VK_WHOLE_SIZE means "from offset to end-of-
        # mapping". Combined with patch 2 below (the map call uses
        # VK_WHOLE_SIZE), end-of-mapping equals the memory size which IS
        # aligned (req.size is buffer-alignment-padded). Satisfies VUID
        # 01389/01390 both.
        string(FIND "${_crd_imgui_vk_text}" "range[0].size = upload_size;" _crd_imgui_vk_idx1)
        if(NOT _crd_imgui_vk_idx1 EQUAL -1)
            string(REPLACE
                "range[0].size = upload_size;"
                "range[0].size = VK_WHOLE_SIZE; // crd-patch v1-close: nonCoherentAtomSize alignment"
                _crd_imgui_vk_text "${_crd_imgui_vk_text}"
            )
        endif()
        # Patch 2 — map call: map the whole allocation so end-of-mapping
        # coincides with end-of-memory (which IS atom-aligned via
        # req.size). The map'd extra bytes past `upload_size` are never
        # written, which is fine.
        string(FIND "${_crd_imgui_vk_text}" "vkMapMemory(v->Device, upload_buffer_memory, 0, upload_size, 0, (void**)(&map))" _crd_imgui_vk_idx2)
        if(NOT _crd_imgui_vk_idx2 EQUAL -1)
            string(REPLACE
                "vkMapMemory(v->Device, upload_buffer_memory, 0, upload_size, 0, (void**)(&map))"
                "vkMapMemory(v->Device, upload_buffer_memory, 0, VK_WHOLE_SIZE, 0, (void**)(&map)) /* crd-patch v1-close */"
                _crd_imgui_vk_text "${_crd_imgui_vk_text}"
            )
        endif()
        if(NOT _crd_imgui_vk_idx1 EQUAL -1 OR NOT _crd_imgui_vk_idx2 EQUAL -1)
            file(WRITE "${_crd_imgui_vk_file}" "${_crd_imgui_vk_text}")
            message(STATUS "[crd] Patched imgui_impl_vulkan.cpp texture-upload map/flush -> VK_WHOLE_SIZE (VUID 01389/01390)")
        endif()
    endif()
endif()

# ----- Helper: auto-collect sources with CONFIGURE_DEPENDS -----------------

function(crd_collect_sources out_var directory)
    file(GLOB_RECURSE sources CONFIGURE_DEPENDS
    "${directory}/*.cpp"
    "${directory}/*.hpp"
    "${directory}/*.h"
    )
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
        if(CRD_CLANG_TIDY_WARNINGS_AS_ERRORS)
            # Local strict mode: .clang-tidy's `WarningsAsErrors: '*'` is honored.
            set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE})
            message(STATUS "[crd] clang-tidy ENABLED (warnings-as-errors=ON; strict mode)")
        else()
            # CI relaxed mode: override `.clang-tidy` WarningsAsErrors via
            # `--warnings-as-errors=-*` (command-line wins; tidy still RUNS and
            # reports warnings to the log, but does not fail the build). Used
            # by CI to bypass LLVM-version-skew false positives (LLVM 19 vs 20
            # categorise function-local `constexpr` differently — see
            # .github/workflows/ci.yml clang-tidy job notes). Local pre-slice
            # DoD via `scripts/per-slice-check.ps1 -Parallel` still gates on
            # the strict `win-tidy` preset, so violations are caught
            # pre-merge by the user's local gate.
            set(CMAKE_CXX_CLANG_TIDY ${CLANG_TIDY_EXE} --warnings-as-errors=-*)
            message(STATUS "[crd] clang-tidy ENABLED (warnings-as-errors=OFF; CI relaxed mode — warnings reported but non-fatal)")
        endif()
    endif()
endif()


# ----- Engine Modules ------------------------------------------------------

add_subdirectory(engine/core)
add_subdirectory(engine/vm)  # ADR-0085: OS virtual-memory leaf (page source for crd-memory's VirtualMemoryAllocator)
add_subdirectory(engine/units)
add_subdirectory(engine/time)
add_subdirectory(engine/perf)
add_subdirectory(engine/memory)
# crd-perf-ui depends on crd-imgui (which depends on crd-rhi-vulkan/crd-app/crd-config);
# declared further below after those modules are added.
add_subdirectory(engine/math)
add_subdirectory(engine/geometry-primitives)
add_subdirectory(engine/geometry-bvh)
add_subdirectory(engine/geometry-convex)
add_subdirectory(engine/geometry-mesh)
add_subdirectory(engine/geometry-spatial)
add_subdirectory(engine/geometry-polygon)
add_subdirectory(engine/geometry-mesh-processing)
add_subdirectory(engine/geometry-delaunay)
add_subdirectory(engine/geometry-decomposition)
add_subdirectory(engine/geometry-bvh-gpu)
add_subdirectory(engine/geometry-shader-helpers)
add_subdirectory(engine/geometry-curves)
add_subdirectory(engine/hesap)
add_subdirectory(engine/hesap-dense)
add_subdirectory(engine/hesap-sparse)  # v1a; links crd-jobs (declared below — CMake resolves target names at generation)
add_subdirectory(engine/hesap-ordering)  # v2; fill-reducing reorderings + symbolic factorisation (depends on hesap-sparse)
add_subdirectory(engine/hesap-resources)  # v4-corpus; matrices as cooked resources (depends on hesap-sparse + resources)
add_subdirectory(engine/hesap-iterative)  # v4a; Krylov solvers (CG/PCG) over LinearOp
add_subdirectory(engine/hesap-preconditioners)  # v4a; Jacobi/SSOR preconditioners (one-way dep on hesap-iterative)
add_subdirectory(engine/hesap-amg)  # v4k; smoothed-aggregation AMG (LinearOp; one-way, Krylov consumes it)
# crd-hesap-sched depends on crd-jobs (declared below); declared after jobs.
# crd-geometry-viz must come AFTER crd-draw (its declaration is below) — moved
# down to engine/eylem-viz neighbourhood. See the second add_subdirectory below.
add_subdirectory(engine/containers)
add_subdirectory(engine/log)
add_subdirectory(engine/platform)
add_subdirectory(engine/app)
add_subdirectory(engine/config)
add_subdirectory(engine/imgui)
add_subdirectory(engine/perf-ui)  # D-003 v0g -- ImGui frontend for crd-perf; needs crd-imgui above
add_subdirectory(engine/renderer)
add_subdirectory(engine/meshgen)
add_subdirectory(engine/shader)
add_subdirectory(engine/rhi)
add_subdirectory(engine/rhi-vulkan)
add_subdirectory(engine/jobs)
add_subdirectory(engine/hesap-sched)  # depends on crd-jobs above + crd-hesap (declared earlier)
add_subdirectory(engine/hesap-direct)  # v5; sparse DIRECT factorization (depends on hesap-dense/ordering/sparse/sched)
add_subdirectory(engine/hesap-eigen)  # v6; sparse EIGENVALUE, matrix-free (Rayleigh-Ritz via hesap-dense; shift-invert via hesap-direct)
add_subdirectory(engine/hesap-opt)  # v7; OPTIMISATION (unconstrained + constrained) over Objective<T>; ADR-0090
add_subdirectory(engine/hesap-stats)  # v12 cluster (created early as a v12-pull: Philox counter-RNG for v7-i)
add_subdirectory(engine/hesap-ode)  # v9; ODE/DAE — stepper kernel headers + driver substrate; ADR-0091
add_subdirectory(engine/hesap-fft)  # v10; FFT — Complex<T> transforms; gold standards FFTW/PocketFFT/MKL
add_subdirectory(engine/hesap-dsp)  # v11; DSP — filters/spectral/multirate (depends on hesap-fft/eigen/units); ADR-0093
add_subdirectory(engine/resources)
add_subdirectory(engine/scene)
add_subdirectory(engine/preset)
add_subdirectory(engine/profile)
add_subdirectory(engine/eylem)
add_subdirectory(engine/eylem-rigid3d)
add_subdirectory(engine/draw)
add_subdirectory(engine/draw-imgui)
add_subdirectory(engine/eylem-viz)
add_subdirectory(engine/geometry-viz)
# add_subdirectory(engine/graphics)
# add_subdirectory(engine/scripting)

# ----- Tools ----------------------------------------------------------------

add_subdirectory(tools/asset_cooker)

# Optional 'cook' target: cmake --build --preset win-debug --target cook
# Requires CRD_COOK_ROOT and CRD_COOK_OUT to be set (e.g. via -D flags).
if(CRD_COOK_ROOT AND CRD_COOK_OUT)
    add_custom_target(cook
        COMMAND asset_cooker cook --root "${CRD_COOK_ROOT}" --out "${CRD_COOK_OUT}"
        DEPENDS asset_cooker
        COMMENT "Cooking assets from ${CRD_COOK_ROOT} into ${CRD_COOK_OUT}"
    )
endif()

# ----- Runtime executable ---------------------------------------------------

add_subdirectory(runtime)

# ----- Sandbox --------------------------------------------------------------

if(CRD_BUILD_SANDBOX)
    add_subdirectory(sandbox)
endif()

# ----- Tests ----------------------------------------------------------------

if(CRD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
