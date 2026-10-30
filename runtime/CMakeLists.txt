add_executable(crd-runtime
    src/main.cpp
)

target_link_libraries(crd-runtime PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(crd-runtime)

add_executable(smoke_log
    examples/smoke_log.cpp
)

target_link_libraries(smoke_log PRIVATE
    crd-core
    crd-log
)
crd_enable_default_pch(smoke_log)

add_executable(smoke_memory
    examples/smoke_memory.cpp
)

target_link_libraries(smoke_memory PRIVATE
    crd-core
    crd-log
    crd-memory
)
crd_enable_default_pch(smoke_memory)

add_executable(smoke_containers
    examples/smoke_containers.cpp
)

target_link_libraries(smoke_containers PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
)
crd_enable_default_pch(smoke_containers)

add_executable(smoke_math
    examples/smoke_math.cpp
)

target_link_libraries(smoke_math PRIVATE
    crd-core
    crd-math
    crd-geometry-primitives
)
crd_enable_default_pch(smoke_math)

# The CPU level line every bench board records (ADR-0134 §8): the harnesses run it beside each measured program.
add_executable(cpu_level_record examples/cpu_level_record.cpp)
target_link_libraries(cpu_level_record PRIVATE crd-core crd-math)
crd_enable_default_pch(cpu_level_record)

add_executable(smoke_window
    examples/smoke_window.cpp
)

target_link_libraries(smoke_window PRIVATE
    crd-core
    crd-log
    crd-containers
    crd-platform
)
crd_enable_default_pch(smoke_window)

add_executable(smoke_config
    examples/smoke_config.cpp
)

target_link_libraries(smoke_config PRIVATE
    crd-core
    crd-log
    crd-config
)
crd_enable_default_pch(smoke_config)

# RET-7 (ADR-0105): the retiring-stack smokes are DELETED -- smoke_shader / smoke_renderer /
# smoke_imgui_overlay / smoke_rhi_api / smoke_resources_render / smoke_material /
# smoke_rhi_vulkan_bootstrap / smoke_asset_import / smoke_depth_prepass. Their living coverage:
# the sandbox smoke (boot+render+present on gpu-context), the GEO-3 import->cook->RENDER gates,
# the RET-5 ImGui gates, and the D1-D12 deploy suites. The modules' own tests die at RET-8.

add_executable(smoke_frame_clock
    examples/smoke_frame_clock.cpp
)

target_link_libraries(smoke_frame_clock PRIVATE
    crd-core
    crd-log
    crd-platform
)
crd_enable_default_pch(smoke_frame_clock)

add_executable(smoke_filesystem
    examples/smoke_filesystem.cpp
)

target_link_libraries(smoke_filesystem PRIVATE
    crd-core
    crd-log
    crd-platform
)
crd_enable_default_pch(smoke_filesystem)

# ADR-0085 S1: virtual-memory reserve/commit/decommit (open-world streaming substrate).
add_executable(smoke_virtual_memory
    examples/smoke_virtual_memory.cpp
)
target_link_libraries(smoke_virtual_memory PRIVATE
    crd-core
    crd-log
    crd-vm
)
crd_enable_default_pch(smoke_virtual_memory)

# ADR-0085 S2: VirtualMemoryAllocator (stable-address bump arena over crd::vm).
add_executable(smoke_virtual_memory_allocator
    examples/smoke_virtual_memory_allocator.cpp
)
target_link_libraries(smoke_virtual_memory_allocator PRIVATE
    crd-core
    crd-log
    crd-vm
    crd-memory
)
crd_enable_default_pch(smoke_virtual_memory_allocator)

add_executable(smoke_app
    examples/smoke_app.cpp
)

target_link_libraries(smoke_app PRIVATE
    crd-core
    crd-log
    crd-app
)
crd_enable_default_pch(smoke_app)

add_executable(smoke_jobs
    examples/smoke_jobs.cpp
)

target_link_libraries(smoke_jobs PRIVATE
    crd-core
    crd-jobs
)
crd_enable_default_pch(smoke_jobs)

add_executable(smoke_resources
    examples/smoke_resources.cpp
)

target_link_libraries(smoke_resources PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_resources)

add_executable(smoke_resources_async
    examples/smoke_resources_async.cpp
)

target_link_libraries(smoke_resources_async PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-jobs
    crd-resources
)
crd_enable_default_pch(smoke_resources_async)

add_executable(smoke_resources_reload
    examples/smoke_resources_reload.cpp
)

target_link_libraries(smoke_resources_reload PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_resources_reload)

add_executable(smoke_resources_stream
    examples/smoke_resources_stream.cpp
)

target_link_libraries(smoke_resources_stream PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-jobs
    crd-resources
)
crd_enable_default_pch(smoke_resources_stream)

add_executable(smoke_texture
    examples/smoke_texture.cpp
)

target_link_libraries(smoke_texture PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_texture)

add_executable(smoke_mesh
    examples/smoke_mesh.cpp
)

target_link_libraries(smoke_mesh PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-platform
    crd-resources
)
crd_enable_default_pch(smoke_mesh)

add_executable(smoke_meshgen
    examples/smoke_meshgen.cpp
)

target_link_libraries(smoke_meshgen PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-resources
    crd-meshgen
)
crd_enable_default_pch(smoke_meshgen)

add_executable(smoke_hesap_substrate
    examples/smoke_hesap_substrate.cpp
)

target_link_libraries(smoke_hesap_substrate PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_substrate)

# v14-a smoke + THE LINK-ISOLATION GATE (ADR-0096 §1): links ONLY the tensor
# module's public closure — deliberately NO crd-hesap-stats (Philox is
# include-only) / hesap-dense / hesap-sparse. A future edit that link-drags
# any of them into the substrate breaks THIS link — that is the test.
add_executable(smoke_hesap_tensor
    examples/smoke_hesap_tensor.cpp
)

target_link_libraries(smoke_hesap_tensor PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap-tensor
)
crd_enable_default_pch(smoke_hesap_tensor)

add_executable(smoke_hesap_blas1
    examples/smoke_hesap_blas1.cpp
)

target_link_libraries(smoke_hesap_blas1 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas1)

add_executable(smoke_hesap_blas2
    examples/smoke_hesap_blas2.cpp
)

target_link_libraries(smoke_hesap_blas2 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas2)

add_executable(smoke_hesap_blas3
    examples/smoke_hesap_blas3.cpp
)

target_link_libraries(smoke_hesap_blas3 PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(smoke_hesap_blas3)

add_executable(smoke_hesap_sparse
    examples/smoke_hesap_sparse.cpp
)

target_link_libraries(smoke_hesap_sparse PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-sparse
)
crd_enable_default_pch(smoke_hesap_sparse)

add_executable(smoke_hesap_matrix_resource
    examples/smoke_hesap_matrix_resource.cpp
)
target_link_libraries(smoke_hesap_matrix_resource PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-math
    crd-platform
    crd-hesap
    crd-hesap-sparse
    crd-hesap-resources
    crd-resources
)
crd_enable_default_pch(smoke_hesap_matrix_resource)

add_executable(smoke_hesap_solve_cli
    examples/smoke_hesap_solve_cli.cpp
)
target_link_libraries(smoke_hesap_solve_cli PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-math
    crd-platform
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-resources
    crd-hesap-iterative
    crd-resources
)
crd_enable_default_pch(smoke_hesap_solve_cli)

# v7-e-2: generate representative NLS-derived JᵀJ matrices (3D elastic lattice) as Matrix-Market files for the
# SupernodalCholesky-vs-CHOLMOD head-to-head. Pure crd (no external deps) ⇒ always built.
add_executable(dump_nls_lattice_jtj
    examples/dump_nls_lattice_jtj.cpp
)
target_link_libraries(dump_nls_lattice_jtj PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-math
    crd-platform
    crd-jobs
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-ordering
    crd-hesap-direct
)
crd_enable_default_pch(dump_nls_lattice_jtj)

add_executable(bench_hesap_gemm
    examples/bench_hesap_gemm.cpp
)

target_link_libraries(bench_hesap_gemm PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
)
crd_enable_default_pch(bench_hesap_gemm)

add_executable(bench_hesap_gemm_parallel
    examples/bench_hesap_gemm_parallel.cpp
)

target_link_libraries(bench_hesap_gemm_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-jobs
)
crd_enable_default_pch(bench_hesap_gemm_parallel)

add_executable(bench_hesap_factor_parallel
    examples/bench_hesap_factor_parallel.cpp
)

target_link_libraries(bench_hesap_factor_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-jobs
)
crd_enable_default_pch(bench_hesap_factor_parallel)

add_executable(bench_hesap_eig_parallel
    examples/bench_hesap_eig_parallel.cpp
)

target_link_libraries(bench_hesap_eig_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-jobs
)
crd_enable_default_pch(bench_hesap_eig_parallel)

add_executable(bench_hesap_direct_parallel
    examples/bench_hesap_direct_parallel.cpp
)

target_link_libraries(bench_hesap_direct_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-ordering
    crd-hesap-direct
    crd-jobs
)
crd_enable_default_pch(bench_hesap_direct_parallel)

add_executable(bench_hesap_amg_parallel
    examples/bench_hesap_amg_parallel.cpp
)

target_link_libraries(bench_hesap_amg_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-amg
    crd-jobs
)
crd_enable_default_pch(bench_hesap_amg_parallel)

add_executable(bench_hesap_operator_parallel
    examples/bench_hesap_operator_parallel.cpp
)

target_link_libraries(bench_hesap_operator_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-iterative
    crd-jobs
)
crd_enable_default_pch(bench_hesap_operator_parallel)

add_executable(bench_hesap_autodiff_parallel
    examples/bench_hesap_autodiff_parallel.cpp
)

target_link_libraries(bench_hesap_autodiff_parallel PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-hesap-autodiff
    crd-jobs
)
crd_enable_default_pch(bench_hesap_autodiff_parallel)

# v5f-c mixed-precision INTERNAL crush gate — f32-factor + f64-IR vs f64-full. No external peer (just Cerid),
# so it builds in the standard runtime build (run in win-release). The MUMPS-single+IR crush peer is WSL-gated.
add_executable(bench_hesap_mixed_internal examples/bench_hesap_mixed_internal.cpp)
target_link_libraries(bench_hesap_mixed_internal PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-jobs
    crd-hesap
    crd-hesap-sparse
    crd-hesap-ordering
    crd-hesap-direct
)
crd_enable_default_pch(bench_hesap_mixed_internal)

add_executable(bench_hesap_eigen_lobpcg examples/bench_hesap_eigen_lobpcg.cpp)
target_link_libraries(bench_hesap_eigen_lobpcg PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-jobs
    crd-hesap
    crd-hesap-sparse
    crd-hesap-dense
    crd-hesap-eigen
    crd-hesap-amg
)
crd_enable_default_pch(bench_hesap_eigen_lobpcg)

add_executable(bench_multivariate examples/bench_multivariate.cpp)
target_link_libraries(bench_multivariate PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-jobs
    crd-hesap
    crd-hesap-dense
    crd-hesap-special
    crd-hesap-stats
)
crd_enable_default_pch(bench_multivariate)

add_executable(bench_hesap_eigen_gen_lobpcg examples/bench_hesap_eigen_gen_lobpcg.cpp)
target_link_libraries(bench_hesap_eigen_gen_lobpcg PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-jobs
    crd-hesap
    crd-hesap-sparse
    crd-hesap-dense
    crd-hesap-eigen
    crd-hesap-amg
)
crd_enable_default_pch(bench_hesap_eigen_gen_lobpcg)

add_executable(bench_hesap_svds examples/bench_hesap_svds.cpp)
target_link_libraries(bench_hesap_svds PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-math
    crd-jobs
    crd-hesap
    crd-hesap-sparse
    crd-hesap-dense
    crd-hesap-eigen
)
crd_enable_default_pch(bench_hesap_svds)

# crd_add_bench_peer(<name> [DISPATCHES] SOURCES <source>... [LIBRARIES <library>...]): a third-party peer of the
# boards built from source (Eigen, pocketfft) as a shared library of its own at the measuring host's best ISA
# (crd_peer_isa_flags, ADR-0134 §8), apart from the bench units, which build at the baseline like all Cerid code. Its
# symbols are hidden but for its header's operations (examples/peers/peer_api.hpp), it holds no link-time IL and links
# no Cerid library, so no inline body compiled at the peer's ISA is shared with Cerid code; the ISA isolation guard
# leaves it out. DISPATCHES: the peer selects its own ISA at run time (Highway), so it builds at the baseline, as its
# own build does.
function(crd_add_bench_peer name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "DISPATCHES" "" "SOURCES;LIBRARIES")
    set(flags "")
    if(NOT arg_DISPATCHES)
        crd_peer_isa_flags(flags)
    endif()
    add_library(${name} SHARED ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE crd-warnings ${arg_LIBRARIES})
    target_compile_features(${name} PRIVATE cxx_std_20)
    # A peer unit instantiates a header library's templates by the thousand: past COFF's 2^16 sections at /Od.
    target_compile_options(${name} PRIVATE ${flags} $<$<BOOL:${MSVC}>:/bigobj>)
    string(REPLACE ";" " " isa "${flags}")
    if(isa STREQUAL "")
        set(isa "the baseline ISA")
    endif()
    target_compile_definitions(${name} PRIVATE CRD_BENCH_PEER_BUILD "CRD_BENCH_PEER_ISA=\"${isa}\"")
    set_target_properties(${name} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
                          INTERPROCEDURAL_OPTIMIZATION OFF CRD_ISA_ROLE peer)
    message(STATUS "[bench-peer] ${name}: ${isa}")
endfunction()

# Reference-class shootout, built only when CRD_BUILD_HESAP_VS_REFERENCE=ON: Eigen 3.4 (fetched by CPM at configure
# time, built as the crd-bench-eigen-peer library) and OpenBLAS (built by its own CMake, called through cblas.h and
# LAPACK's symbols).
#
# crd_add_hesap_vs_ref_bench(<name> [USE_JOBS] [USE_OPENBLAS]): the bench examples/<name>.cpp, which calls Eigen
# through crd-bench-eigen-peer (examples/peers/eigen_peer.hpp) and builds with the tree's warnings.
#   USE_JOBS     also link crd-jobs (benches that start the job pool).
#   USE_OPENBLAS also link openblas and its generated-header directories (see the note below).
# No precompiled header: the default one has not been proven beside OpenBLAS's cblas.h.
# Eigen's parallel products expand `#pragma omp parallel` in the unit that instantiates them, which is the peer's.
function(crd_add_hesap_vs_ref_bench name)
    cmake_parse_arguments(ARG "USE_JOBS;USE_OPENBLAS" "" "" ${ARGN})
    add_executable(${name} examples/${name}.cpp)
    target_link_libraries(${name} PRIVATE
        crd-core crd-memory crd-containers crd-math crd-platform crd-hesap crd-hesap-dense)
    if(ARG_USE_JOBS)
        target_link_libraries(${name} PRIVATE crd-jobs)
    endif()
    if(ARG_USE_OPENBLAS)
        target_link_libraries(${name} PRIVATE openblas)
        # OpenBLAS does NOT export BUILD_INTERFACE include dirs (only INSTALL),
        # and its CMakeLists writes the generated cblas.h + openblas_config.h to
        # the TOP-LEVEL ${CMAKE_BINARY_DIR}, not the CPM sub-build dir.
        target_include_directories(${name} SYSTEM PRIVATE
            "${OpenBLAS_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}"
            "${CMAKE_BINARY_DIR}/generated"
            "${CMAKE_BINARY_DIR}/_deps/openblas-build")
    endif()
    target_link_libraries(${name} PRIVATE crd-bench-eigen-peer)
    target_include_directories(${name} PRIVATE examples/peers)
endfunction()

if(CRD_BUILD_HESAP_VS_REFERENCE)
    crd_add_bench_peer(crd-bench-eigen-peer
        SOURCES examples/peers/eigen_peer_dense.cpp examples/peers/eigen_peer_sparse.cpp
        examples/peers/eigen_peer_direct.cpp examples/peers/eigen_peer_iterative.cpp LIBRARIES eigen_headers)

    crd_add_hesap_vs_ref_bench(bench_hesap_gemm_vs_reference    USE_JOBS USE_OPENBLAS)
    crd_add_hesap_vs_ref_bench(bench_hesap_blas1_vs_reference   USE_OPENBLAS)
    crd_add_hesap_vs_ref_bench(bench_hesap_blas2_vs_reference   USE_OPENBLAS)
    # v0e-g: solvers shootout (LU / Cholesky / LDLT / QR) — Eigen-only.
    crd_add_hesap_vs_ref_bench(bench_hesap_solvers_vs_reference USE_JOBS)
    # v3a-1: symmetric eig (eig_sym) vs Eigen SelfAdjointEigenSolver + LAPACK dsyev.
    crd_add_hesap_vs_ref_bench(bench_hesap_eig_vs_reference USE_JOBS USE_OPENBLAS)
    # v3b-1b: SVD (svd) vs Eigen JacobiSVD + BDCSVD AND LAPACK dgesvd + dgesdd.
    crd_add_hesap_vs_ref_bench(bench_hesap_svd_vs_reference USE_JOBS USE_OPENBLAS)
    # v3c-1b: least-squares (lstsq QR/COD/SVD + pinv) vs Eigen HouseholderQR /
    # CompleteOrthogonalDecomposition / BDCSVD AND LAPACK dgels / dgelsy / dgelsd.
    crd_add_hesap_vs_ref_bench(bench_hesap_lstsq_vs_reference USE_JOBS USE_OPENBLAS)
    # v3d-1a: Hessenberg reduction (blocked dgehrd) vs Eigen HessenbergDecomposition
    # AND LAPACK dgehrd.
    crd_add_hesap_vs_ref_bench(bench_hesap_eig_nonsym_vs_reference USE_JOBS USE_OPENBLAS)
    # v1a-3: sparse assembly shootout (TripletBuilder::compress vs setFromTriplets).
    crd_add_hesap_vs_ref_bench(bench_hesap_sparse_assembly_vs_reference)
    target_link_libraries(bench_hesap_sparse_assembly_vs_reference PRIVATE crd-hesap-sparse)
    # v1b-1/v1b-2: spmv CSR baseline + SELL primary (ST + MT) vs Eigen (ST + MT).
    crd_add_hesap_vs_ref_bench(bench_hesap_spmv_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_spmv_vs_reference PRIVATE crd-hesap-sparse)

    # v1d-2: spgemm C=A*A vs Eigen on REAL SuiteSparse matrices. Fetch a few
    # representative square matrices at configure time (gated; fetched, never
    # committed — same posture as Eigen/OpenBLAS). The bench reads the .mtx via
    # a minimal Matrix-Market reader and dispatches to CRD_SUITESPARSE_DIR.
    # -DCRD_HESAP_MATRIX_DIR=<dir> names a corpus another tree already holds; only its missing matrices are fetched.
    if(CRD_HESAP_MATRIX_DIR)
        set(CRD_SS_DIR "${CRD_HESAP_MATRIX_DIR}")
    else()
        set(CRD_SS_DIR "${CMAKE_BINARY_DIR}/_deps/suitesparse-mm")
    endif()
    file(MAKE_DIRECTORY "${CRD_SS_DIR}")
    set(CRD_SS_BASE "https://suitesparse-collection-website.herokuapp.com/MM")
    foreach(ss_pair "HB/bcsstk13" "HB/bcsstk24" "HB/west2021" "HB/bcsstk25" "HB/sherman3" "HB/gemat11" "HB/bcsstk30" "HB/bcsstk32" "GHS_psdef/bmwcra_1" "GHS_psdef/hood" "GHS_psdef/ldoor" "Hamm/add32" "Hamm/memplus" "Bai/af23560" "Wang/wang3" "Simon/bbmat" "Garon/garon2" "FEMLAB/ns3Da" "Simon/raefsky3" "HB/well1850" "HB/illc1850" "HB/well1033" "HB/illc1033")
        string(REGEX REPLACE ".*/" "" ss_name "${ss_pair}")
        if(NOT EXISTS "${CRD_SS_DIR}/${ss_name}/${ss_name}.mtx")
            set(ss_tgz "${CRD_SS_DIR}/${ss_name}.tar.gz")
            message(STATUS "[hesap-vs-reference] fetching SuiteSparse ${ss_pair}")
            file(DOWNLOAD "${CRD_SS_BASE}/${ss_pair}.tar.gz" "${ss_tgz}" STATUS ss_status TIMEOUT 600)
            list(GET ss_status 0 ss_code)
            if(ss_code EQUAL 0)
                file(ARCHIVE_EXTRACT INPUT "${ss_tgz}" DESTINATION "${CRD_SS_DIR}")
            else()
                message(WARNING "[hesap-vs-reference] SuiteSparse ${ss_pair} download failed (${ss_status}); bench will SKIP it")
            endif()
        endif()
    endforeach()
    crd_add_hesap_vs_ref_bench(bench_hesap_spgemm_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_spgemm_vs_reference PRIVATE crd-hesap-sparse)
    target_compile_definitions(bench_hesap_spgemm_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v1e-1: spmm (sparse x dense, multi-RHS) vs Eigen sparse*dense (ST + MT).
    crd_add_hesap_vs_ref_bench(bench_hesap_spmm_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_spmm_vs_reference PRIVATE crd-hesap-sparse)
    target_compile_definitions(bench_hesap_spmm_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v1e-2: SDDMM vs Eigen per-entry dot (same flops) + dense-then-mask illustration.
    crd_add_hesap_vs_ref_bench(bench_hesap_sddmm_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_sddmm_vs_reference PRIVATE crd-hesap-sparse)
    target_compile_definitions(bench_hesap_sddmm_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v1f-1: BSR block-spmv vs Eigen scalar-CSR (synthetic native block patterns).
    crd_add_hesap_vs_ref_bench(bench_hesap_bsr_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_bsr_vs_reference PRIVATE crd-hesap-sparse)
    # v1f-2: ELL (regular) + DIA (banded) spmv vs Cerid + Eigen scalar CSR.
    crd_add_hesap_vs_ref_bench(bench_hesap_ell_dia_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_ell_dia_vs_reference PRIVATE crd-hesap-sparse)
    # v1g-2: adversarial spgemm stress corpus vs Eigen-ST (find any losing/infeasible regime).
    crd_add_hesap_vs_ref_bench(bench_hesap_spgemm_stress_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_spgemm_stress_vs_reference PRIVATE crd-hesap-sparse)
    # v2a: ordering — symbolic-Cholesky port validation + RCM fill/bandwidth + v2b AMD target.
    crd_add_hesap_vs_ref_bench(bench_hesap_ordering_vs_reference)
    target_link_libraries(bench_hesap_ordering_vs_reference PRIVATE crd-hesap-sparse crd-hesap-ordering)
    target_compile_definitions(bench_hesap_ordering_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v5a-1b step 6 / v5a-3: TREE-PARALLEL supernodal Cholesky factor+solve vs Eigen SimplicialLLT.
    crd_add_hesap_vs_ref_bench(bench_hesap_cholesky_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_cholesky_vs_reference PRIVATE crd-hesap-sparse crd-hesap-ordering crd-hesap-direct)
    target_compile_definitions(bench_hesap_cholesky_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v5b-1: Gilbert-Peierls sparse LU (SERIAL correctness oracle) factor+solve vs Eigen SparseLU.
    crd_add_hesap_vs_ref_bench(bench_hesap_lu_vs_reference)
    target_link_libraries(bench_hesap_lu_vs_reference PRIVATE crd-hesap-sparse crd-hesap-ordering crd-hesap-direct)
    target_compile_definitions(bench_hesap_lu_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v5e-1d: HSS ULV (build_hss_from_dense + factor_hss_ulv + solve) vs Eigen dense LLT — the
    # O(r^2 N) factor + O(rN) per-solve PREMISE (matrix generated internally; no SuiteSparse dep).
    crd_add_hesap_vs_ref_bench(bench_hesap_hss_vs_reference)
    target_link_libraries(bench_hesap_hss_vs_reference PRIVATE crd-hesap-direct)

    # v5c-1d: Multifrontal QR factor+solve vs Eigen SparseQR (NaturalOrdering on the same
    # AMD(AᵀA)-column-ordered matrix). v5c-1 correctness-first baseline; the crush lands with
    # the perf levers (blocked-WY + staircase + implicit AᵀA-free symbolic + tree-parallel).
    crd_add_hesap_vs_ref_bench(bench_hesap_qr_vs_reference)
    target_link_libraries(bench_hesap_qr_vs_reference PRIVATE crd-hesap-sparse crd-hesap-ordering crd-hesap-direct)
    target_compile_definitions(bench_hesap_qr_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")

    # v5d-g: multifrontal LDLᵀ (Bunch-Kaufman) vs Eigen SimplicialLDLT (pivot-free; honest peer note in-file).
    crd_add_hesap_vs_ref_bench(bench_hesap_ldlt_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_ldlt_vs_reference PRIVATE crd-hesap-sparse crd-hesap-direct)

    # v5b-2c: SUPERNODAL LU crush — Cerid SupernodalLU vs Eigen SparseLU on the same AMD-permuted
    # UNSYMMETRIC matrices. UMFPACK (the multifrontal gold standard) folds in when
    # CRD_BUILD_HESAP_VS_UMFPACK=ON (WSL/Linux; GPL → dev-only, never shipped) — it links the
    # SuiteSparse umfpack apt-installed by scripts/setup-cholmod-ref.sh.
    crd_add_hesap_vs_ref_bench(bench_hesap_lu_supernodal_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_lu_supernodal_vs_reference PRIVATE crd-hesap-sparse crd-hesap-ordering crd-hesap-direct crd-hesap-iterative)
    target_compile_definitions(bench_hesap_lu_supernodal_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    if(CRD_BUILD_HESAP_VS_UMFPACK)
        find_library(CRD_UMFPACK_LIB umfpack)
        if(NOT CRD_UMFPACK_LIB)
            message(FATAL_ERROR "[hesap-vs-umfpack] libumfpack not found — run scripts/setup-cholmod-ref.sh")
        endif()
        # Ubuntu's libsuitesparse-dev installs umfpack.h under /usr/include/suitesparse, which is not
        # on the default include path; locate it explicitly so `#include <umfpack.h>` resolves.
        find_path(CRD_UMFPACK_INCLUDE umfpack.h PATH_SUFFIXES suitesparse)
        if(NOT CRD_UMFPACK_INCLUDE)
            message(FATAL_ERROR "[hesap-vs-umfpack] umfpack.h not found — install libsuitesparse-dev")
        endif()
        target_include_directories(bench_hesap_lu_supernodal_vs_reference PRIVATE ${CRD_UMFPACK_INCLUDE})
        target_link_libraries(bench_hesap_lu_supernodal_vs_reference PRIVATE umfpack)
        target_compile_definitions(bench_hesap_lu_supernodal_vs_reference PRIVATE CRD_HESAP_VS_UMFPACK)
        message(STATUS "[hesap-vs-umfpack] UMFPACK bench enabled (${CRD_UMFPACK_LIB}, hdr ${CRD_UMFPACK_INCLUDE})")
    endif()
    # MUMPS (the PARALLEL multifrontal gold standard, OpenMP seq build) folds in when
    # CRD_BUILD_HESAP_VS_MUMPS=ON — the only HONEST parallel-peer comparison (UMFPACK is serial). GPL → dev-only.
    if(CRD_BUILD_HESAP_VS_MUMPS)
        find_library(CRD_DMUMPS_LIB dmumps_seq)
        find_path(CRD_MUMPS_INCLUDE dmumps_c.h)
        if(NOT CRD_DMUMPS_LIB OR NOT CRD_MUMPS_INCLUDE)
            message(FATAL_ERROR "[hesap-vs-mumps] libdmumps_seq / dmumps_c.h not found — apt install libmumps-seq-dev")
        endif()
        target_include_directories(bench_hesap_lu_supernodal_vs_reference PRIVATE ${CRD_MUMPS_INCLUDE})
        # v5f-c: also link smumps_seq (single-precision MUMPS) for the mixed-precision (factor-f32 + f64-IR) peer.
        target_link_libraries(bench_hesap_lu_supernodal_vs_reference PRIVATE
            dmumps_seq smumps_seq mumps_common_seq pord_seq mpiseq_seq)
        target_compile_definitions(bench_hesap_lu_supernodal_vs_reference PRIVATE CRD_HESAP_VS_MUMPS)
        # v5d-g: LDLᵀ vs MUMPS SYM=2 (the real same-class indefinite gold standard — 2×2 Bunch-Kaufman).
        # Also link LAPACK explicitly (dpotrf/dsytrf front-kernel ceiling probe; MUMPS pulls it but only as a
        # private DSO dep so our own calls need it on the command line).
        find_library(CRD_LAPACK_LIB lapack)
        target_include_directories(bench_hesap_ldlt_vs_reference PRIVATE ${CRD_MUMPS_INCLUDE})
        target_link_libraries(bench_hesap_ldlt_vs_reference PRIVATE
            dmumps_seq mumps_common_seq pord_seq mpiseq_seq ${CRD_LAPACK_LIB})
        target_compile_definitions(bench_hesap_ldlt_vs_reference PRIVATE CRD_HESAP_VS_MUMPS)
        message(STATUS "[hesap-vs-mumps] MUMPS bench enabled (${CRD_DMUMPS_LIB})")
    endif()
    # v4-corpus: real SuiteSparse matrices through the cooked 'HMTX' resource path
    # (cook + read_matrix_resource + build_csr timed vs direct read; each matrix also
    # verified through ResourceManager mount + load_sync). No peer library, so a plain bench of the tree.
    add_executable(bench_hesap_matrix_resource_vs_reference examples/bench_hesap_matrix_resource_vs_reference.cpp)
    target_link_libraries(bench_hesap_matrix_resource_vs_reference PRIVATE
        crd-core crd-memory crd-containers crd-hesap-sparse crd-hesap-resources crd-resources crd-platform)
    crd_enable_default_pch(bench_hesap_matrix_resource_vs_reference)
    target_compile_definitions(bench_hesap_matrix_resource_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4a-2: CG / Jacobi-PCG vs Eigen ConjugateGradient + DiagonalPreconditioner
    # on real SuiteSparse SPD matrices (parallel SELL spmv = the DRAM-bound win).
    crd_add_hesap_vs_ref_bench(bench_hesap_cg_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_cg_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_cg_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4b: FGMRES vs Eigen BiCGSTAB (main) + Eigen GMRES (unsupported) on nonsym SuiteSparse.
    crd_add_hesap_vs_ref_bench(bench_hesap_gmres_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_gmres_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_gmres_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4d-2b: IDR(s=4) vs Eigen IDRS (unsupported), apples-to-apples on nonsym SuiteSparse.
    crd_add_hesap_vs_ref_bench(bench_hesap_idrs_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_idrs_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_idrs_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4e-2: GCROT(m,k) vs Cerid GMRES(m) [recycling baseline] + Eigen GMRES(m) [frontier ref].
    crd_add_hesap_vs_ref_bench(bench_hesap_gcrot_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_gcrot_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_gcrot_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4f-1: nested inner-Krylov-as-preconditioner (FGMRES + inner GMRES) vs flat GMRES + Eigen GMRES.
    crd_add_hesap_vs_ref_bench(bench_hesap_nested_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_nested_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_nested_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4f-2: block-CG (multi-RHS) vs Eigen ConjugateGradient per-column on SPD.
    crd_add_hesap_vs_ref_bench(bench_hesap_block_cg_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_block_cg_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_block_cg_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4f-3: block-GMRES + block-BiCGSTAB (multi-RHS) vs Eigen GMRES/BiCGSTAB per-column on nonsym.
    crd_add_hesap_vs_ref_bench(bench_hesap_block_gmres_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_block_gmres_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_block_gmres_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4g: IC(0)-PCG vs Eigen IncompleteCholesky-CG; ILU(0)-BiCGSTAB vs Eigen IncompleteLUT-BiCGSTAB.
    crd_add_hesap_vs_ref_bench(bench_hesap_ilu_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_ilu_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners crd-hesap-ordering)
    target_compile_definitions(bench_hesap_ilu_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
    # v4i-1: SPAI (classical, M ≈ A⁻¹) + FSPAI (factored, M = L·Lᴴ, SPD) vs Eigen IncompleteCholesky / ILUT.
    crd_add_hesap_vs_ref_bench(bench_hesap_spai_vs_reference USE_JOBS)
    target_link_libraries(bench_hesap_spai_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners)
    target_compile_definitions(bench_hesap_spai_vs_reference PRIVATE "CRD_SUITESPARSE_DIR=\"${CRD_SS_DIR}\"")
endif()

# --- v7-d: Cerid L-BFGS vs liblbfgs (the reference C L-BFGS; its default line search IS More-Thuente) ---
# liblbfgs is MIT (permissive), built local-only by scripts/setup-lbfgs-ref.sh into a gitignored deps dir. Gated
# behind CRD_BUILD_HESAP_VS_LBFGS (default OFF); never CI/shipped. The verdict is iteration + point-eval count at
# matched m + matched More-Thuente + matched accuracy (the ‖g‖ achieved is reported for both). No Eigen/SuiteSparse.
if(CRD_BUILD_HESAP_VS_LBFGS)
    set(CRD_LBFGS_DIR "$ENV{HOME}/cerid-deps/liblbfgs/install" CACHE PATH "liblbfgs install prefix (lib/ + include/)")
    if(NOT EXISTS "${CRD_LBFGS_DIR}/lib/liblbfgs.a")
        message(FATAL_ERROR "[hesap-vs-lbfgs] ${CRD_LBFGS_DIR}/lib/liblbfgs.a not found — run scripts/setup-lbfgs-ref.sh")
    endif()
    add_executable(bench_hesap_lbfgs_vs_reference examples/bench_hesap_lbfgs_vs_reference.cpp)
    target_link_libraries(bench_hesap_lbfgs_vs_reference PRIVATE
        crd-core crd-log crd-memory crd-containers crd-math crd-jobs
        crd-hesap crd-hesap-dense crd-hesap-sparse crd-hesap-opt
        "${CRD_LBFGS_DIR}/lib/liblbfgs.a")
    target_include_directories(bench_hesap_lbfgs_vs_reference PRIVATE "${CRD_LBFGS_DIR}/include")
    crd_enable_default_pch(bench_hesap_lbfgs_vs_reference)
    message(STATUS "[hesap-vs-lbfgs] L-BFGS bench enabled (${CRD_LBFGS_DIR}/lib/liblbfgs.a)")
endif()

# --- v7-d-3: L-BFGS-B DIFFERENTIAL TEST vs the reference C (Zhu-Byrd-Lu-Nocedal, via stephenbeckr/L-BFGS-B-C) ---
# The faithful L-BFGS-B port is verified by diffing EVERY routine (foundation + cauchy/formk/subsm + the mainlb
# driver end-to-end vs setulb) against the reference on identical inputs. Local-only oracle (the reference is a TEST
# oracle, not a dependency — Cerid ships its own from-principles L-BFGS-B). Gated behind CRD_BUILD_HESAP_VS_LBFGSB;
# --- v9-b: scipy step-sequence difftest (headless; the python side is scripts/ode_scipy_ref.py) ---
add_executable(ode_scipy_difftest examples/ode_scipy_difftest.cpp)
target_link_libraries(ode_scipy_difftest PRIVATE crd-core crd-containers crd-memory crd-hesap-ode)
crd_enable_default_pch(ode_scipy_difftest)

# build the oracle with scripts/setup-lbfgsb-ref.sh. Never CI/shipped.
if(CRD_BUILD_HESAP_VS_LBFGSB)
    set(CRD_LBFGSB_REF_DIR "$ENV{HOME}/cerid-deps/lbfgsb-ref/install" CACHE PATH "L-BFGS-B reference install prefix")
    if(NOT EXISTS "${CRD_LBFGSB_REF_DIR}/lib/liblbfgsb_ref.a")
        message(FATAL_ERROR "[hesap-vs-lbfgsb] ${CRD_LBFGSB_REF_DIR}/lib/liblbfgsb_ref.a not found — run scripts/setup-lbfgsb-ref.sh")
    endif()
    add_executable(lbfgsb_difftest examples/lbfgsb_difftest.cpp)
    target_link_libraries(lbfgsb_difftest PRIVATE
        crd-core crd-log crd-memory crd-containers crd-math crd-jobs
        crd-hesap crd-hesap-dense crd-hesap-sparse crd-hesap-opt
        "${CRD_LBFGSB_REF_DIR}/lib/liblbfgsb_ref.a")
    crd_enable_default_pch(lbfgsb_difftest)
    message(STATUS "[hesap-vs-lbfgsb] L-BFGS-B differential test enabled (${CRD_LBFGSB_REF_DIR}/lib/liblbfgsb_ref.a)")
endif()

# --- v4j-2a: Cerid InverseBasedIlu vs ILUPACK V2.4 (local-only, non-commercial) ---
# ILUPACK is non-commercial/binary-only ⇒ a precompiled lib under the gitignored
# external/ilupack (run scripts/setup-ilupack-ref.sh first). WSL/Linux only
# (gfortran-built .a). NEVER in CI, NEVER shipped. Static-lib link order follows
# the dist's simple_examples/makefile; gfortran+m supply the Fortran runtime.
if(CRD_BUILD_HESAP_VS_ILUPACK)
    if(NOT UNIX)
        message(WARNING "[hesap-vs-ilupack] ILUPACK libs are Linux/WSL-only; skipping on this platform")
    else()
        set(CRD_ILUPACK_DIR "${CMAKE_SOURCE_DIR}/external/ilupack/dist/ilupackV2.4_GNU64_MUMPS"
            CACHE PATH "ILUPACK V2.4 precompiled distribution root")
        set(_ilu_lib "${CRD_ILUPACK_DIR}/lib/GNU64")
        if(NOT EXISTS "${_ilu_lib}/libilupack.a")
            message(FATAL_ERROR "[hesap-vs-ilupack] ${_ilu_lib}/libilupack.a not found — run scripts/setup-ilupack-ref.sh")
        endif()
        add_executable(bench_hesap_mlilu_vs_ilupack examples/bench_hesap_mlilu_vs_ilupack.cpp)
        target_link_libraries(bench_hesap_mlilu_vs_ilupack PRIVATE
            crd-core crd-memory crd-containers crd-math crd-jobs
            crd-hesap crd-hesap-dense crd-hesap-sparse crd-hesap-iterative crd-hesap-preconditioners crd-hesap-amg
            "${_ilu_lib}/libilupack.a" "${_ilu_lib}/libmumps.a" "${_ilu_lib}/libamd.a"
            "${_ilu_lib}/libmetis.a" "${_ilu_lib}/libsparspak.a" "${_ilu_lib}/liblapack.a"
            "${_ilu_lib}/libblaslike.a" "${_ilu_lib}/libblas.a"
            gfortran m)
        target_include_directories(bench_hesap_mlilu_vs_ilupack SYSTEM PRIVATE "${CRD_ILUPACK_DIR}/include")
        target_compile_definitions(bench_hesap_mlilu_vs_ilupack PRIVATE __UNDERSCORE__)
        set_source_files_properties(examples/bench_hesap_mlilu_vs_ilupack.cpp PROPERTIES COMPILE_FLAGS "-Wno-error")
        message(STATUS "[hesap-vs-ilupack] bench_hesap_mlilu_vs_ilupack enabled (ILUPACK at ${CRD_ILUPACK_DIR})")
    endif()
endif()

# --- v5a-2: Cerid supernodal Cholesky vs SuiteSparse CHOLMOD (the gold-standard
# supernodal peer). System libcholmod is apt-installed (run scripts/setup-cholmod-ref.sh
# first). WSL/Linux only; CHOLMOD's Supernodal module is GPL ⇒ dev-only, NEVER shipped,
# NEVER in CI release. Needs CRD_BUILD_HESAP_VS_REFERENCE=ON for the matrix corpus (CRD_SS_DIR).
if(CRD_BUILD_HESAP_VS_CHOLMOD)
    if(NOT UNIX)
        message(WARNING "[hesap-vs-cholmod] CHOLMOD oracle is Linux/WSL-only; skipping on this platform")
    else()
        find_library(CRD_CHOLMOD_LIB cholmod)
        if(NOT CRD_CHOLMOD_LIB)
            message(FATAL_ERROR "[hesap-vs-cholmod] libcholmod not found — run scripts/setup-cholmod-ref.sh")
        endif()
        # Matrix corpus: reuse VS_REFERENCE's CRD_SS_DIR when that path is enabled,
        # else accept an explicit -DCRD_HESAP_MATRIX_DIR=<dir-with-<name>/<name>.mtx>
        # (e.g. an already-downloaded build/win-*/_deps/suitesparse-mm). This lets the
        # CHOLMOD oracle build WITHOUT fetching Eigen/OpenBLAS — those are unrelated to
        # this bench (it uses SYSTEM cholmod + SYSTEM OpenBLAS).
        if(DEFINED CRD_SS_DIR)
            set(_cholmod_ss "${CRD_SS_DIR}")
        elseif(CRD_HESAP_MATRIX_DIR)
            set(_cholmod_ss "${CRD_HESAP_MATRIX_DIR}")
        else()
            message(FATAL_ERROR "[hesap-vs-cholmod] set -DCRD_HESAP_MATRIX_DIR=<suitesparse-mm dir> (or enable VS_REFERENCE)")
        endif()
        add_executable(bench_hesap_cholesky_vs_cholmod examples/bench_hesap_cholesky_vs_cholmod.cpp)
        target_link_libraries(bench_hesap_cholesky_vs_cholmod PRIVATE
            crd-core crd-memory crd-containers crd-math crd-jobs crd-platform
            crd-hesap crd-hesap-sparse crd-hesap-ordering crd-hesap-dense crd-hesap-sched crd-hesap-direct
            cholmod amd colamd suitesparseconfig openblas)
        target_compile_definitions(bench_hesap_cholesky_vs_cholmod PRIVATE "CRD_SUITESPARSE_DIR=\"${_cholmod_ss}\"")
        set_source_files_properties(examples/bench_hesap_cholesky_vs_cholmod.cpp PROPERTIES COMPILE_FLAGS "-Wno-error")
        message(STATUS "[hesap-vs-cholmod] bench enabled (${CRD_CHOLMOD_LIB}); matrices=${_cholmod_ss}")
    endif()
endif()

# --- v5c-1d: Cerid MultifrontalQR vs SuiteSparse SPQR (SuiteSparseQR — the gold-standard
# multifrontal sparse QR). System libspqr is apt-installed (libsuitesparse-dev). WSL/Linux
# only; SPQR is GPL ⇒ dev-only, NEVER shipped, NEVER in CI release. Reuses VS_REFERENCE's
# CRD_SS_DIR matrix corpus, else accepts -DCRD_HESAP_MATRIX_DIR.
if(CRD_BUILD_HESAP_VS_SPQR)
    if(NOT UNIX)
        message(WARNING "[hesap-vs-spqr] SPQR oracle is Linux/WSL-only; skipping on this platform")
    else()
        find_library(CRD_SPQR_LIB spqr)
        if(NOT CRD_SPQR_LIB)
            message(FATAL_ERROR "[hesap-vs-spqr] libspqr not found — apt install libsuitesparse-dev")
        endif()
        if(DEFINED CRD_SS_DIR)
            set(_spqr_ss "${CRD_SS_DIR}")
        elseif(CRD_HESAP_MATRIX_DIR)
            set(_spqr_ss "${CRD_HESAP_MATRIX_DIR}")
        else()
            message(FATAL_ERROR "[hesap-vs-spqr] set -DCRD_HESAP_MATRIX_DIR=<suitesparse-mm dir> (or enable VS_REFERENCE)")
        endif()
        add_executable(bench_hesap_qr_vs_spqr examples/bench_hesap_qr_vs_spqr.cpp)
        target_link_libraries(bench_hesap_qr_vs_spqr PRIVATE
            crd-core crd-memory crd-containers crd-math crd-jobs crd-platform
            crd-hesap crd-hesap-sparse crd-hesap-ordering crd-hesap-dense crd-hesap-sched crd-hesap-direct
            spqr cholmod amd colamd suitesparseconfig)
        target_compile_definitions(bench_hesap_qr_vs_spqr PRIVATE "CRD_SUITESPARSE_DIR=\"${_spqr_ss}\"")
        set_source_files_properties(examples/bench_hesap_qr_vs_spqr.cpp PROPERTIES COMPILE_FLAGS "-Wno-error")
        message(STATUS "[hesap-vs-spqr] bench enabled (${CRD_SPQR_LIB}); matrices=${_spqr_ss}")
    endif()
endif()

# --- v5e-1d: Cerid HSS ULV vs STRUMPACK dense HSS+ULV (Ghysels-Li — the gold-standard
# rank-structured solver). STRUMPACK built from source to $HOME/strumpack/install (see
# scripts/setup-strumpack-ref.sh). WSL/Linux only; dev-only, NEVER shipped/CI. The matrix is
# generated internally (no SuiteSparse dep).
if(CRD_BUILD_HESAP_VS_STRUMPACK)
    if(NOT UNIX)
        message(WARNING "[hesap-vs-strumpack] STRUMPACK oracle is Linux/WSL-only; skipping")
    else()
        if(NOT CRD_STRUMPACK_DIR)
            set(CRD_STRUMPACK_DIR "$ENV{HOME}/strumpack/install")
        endif()
        find_library(CRD_STRUMPACK_LIB strumpack HINTS "${CRD_STRUMPACK_DIR}/lib")
        find_package(MPI REQUIRED COMPONENTS CXX)
        find_package(OpenMP COMPONENTS CXX)
        find_library(CRD_SCALAPACK_LIB NAMES scalapack-openmpi scalapack)
        find_library(CRD_METIS_LIB metis)
        if(NOT CRD_STRUMPACK_LIB)
            message(FATAL_ERROR "[hesap-vs-strumpack] libstrumpack not found in ${CRD_STRUMPACK_DIR}/lib — run scripts/setup-strumpack-ref.sh")
        endif()
        add_executable(bench_hesap_hss_vs_strumpack examples/bench_hesap_hss_vs_strumpack.cpp)
        target_include_directories(bench_hesap_hss_vs_strumpack SYSTEM PRIVATE "${CRD_STRUMPACK_DIR}/include")
        target_link_libraries(bench_hesap_hss_vs_strumpack PRIVATE
            crd-core crd-memory crd-containers crd-math crd-platform crd-hesap crd-hesap-dense crd-hesap-direct
            "${CRD_STRUMPACK_LIB}" "${CRD_SCALAPACK_LIB}" openblas "${CRD_METIS_LIB}" MPI::MPI_CXX)
        if(OpenMP_CXX_FOUND)
            target_link_libraries(bench_hesap_hss_vs_strumpack PRIVATE OpenMP::OpenMP_CXX)
        endif()
        set_source_files_properties(examples/bench_hesap_hss_vs_strumpack.cpp PROPERTIES COMPILE_FLAGS "-Wno-error")
        message(STATUS "[hesap-vs-strumpack] bench enabled (${CRD_STRUMPACK_LIB})")
    endif()
endif()

# --- The hesap-fft boards against peers installed on the measuring host (never fetched, never in CI) ---
# FFTW 3 and Intel MKL from the system (libfftw3-dev, oneAPI MKL), the pocketfft header from CRD_POCKETFFT_DIR (built
# as the crd-bench-pocketfft-peer library) and FINUFFT built from source in CRD_FINUFFT_DIR. scripts/run-lane-bench.sh
# builds and runs them at each CPU level. The benches build with the tree's warnings as errors: no peer header library
# reaches them, and FFTW's, MKL's and FINUFFT's C declarations come in as system headers.
function(crd_add_fft_peer_bench name)
    add_executable(${name} examples/${name}.cpp)
    target_link_libraries(${name} PRIVATE crd-core crd-memory crd-containers crd-math crd-hesap crd-hesap-fft ${ARGN})
endfunction()
if(CRD_BUILD_HESAP_VS_FFT_REFS OR CRD_BUILD_HESAP_VS_FINUFFT)
    find_library(CRD_FFTW_LIB fftw3)
    if(NOT CRD_FFTW_LIB)
        message(FATAL_ERROR "[hesap-vs-fft] FFTW 3 not found (libfftw3-dev)")
    endif()
endif()
if(CRD_BUILD_HESAP_VS_FFT_REFS)
    set(CRD_POCKETFFT_DIR "$ENV{HOME}/fft_refs" CACHE PATH "The directory holding pocketfft_hdronly.h")
    find_path(CRD_POCKETFFT_INCLUDE pocketfft_hdronly.h HINTS "${CRD_POCKETFFT_DIR}")
    find_path(CRD_FFTW_INCLUDE fftw3.h)
    find_path(CRD_MKL_INCLUDE mkl_dfti.h PATH_SUFFIXES mkl)
    find_library(CRD_MKL_LIB mkl_rt)
    foreach(found IN ITEMS CRD_POCKETFFT_INCLUDE CRD_FFTW_INCLUDE CRD_MKL_INCLUDE CRD_MKL_LIB)
        if(NOT ${found})
            message(FATAL_ERROR "[hesap-vs-fft-refs] ${found} not found (FFTW 3, Intel MKL, pocketfft_hdronly.h in "
                                "CRD_POCKETFFT_DIR)")
        endif()
    endforeach()
    # The pocketfft header reaches only its peer library, built without its thread pool (every board is one thread),
    # which names the measured copy by its SHA-256 prefix.
    file(SHA256 "${CRD_POCKETFFT_INCLUDE}/pocketfft_hdronly.h" pocketfft_sha)
    string(SUBSTRING "${pocketfft_sha}" 0 12 pocketfft_sha)
    add_library(pocketfft_headers INTERFACE)
    target_include_directories(pocketfft_headers SYSTEM INTERFACE "${CRD_POCKETFFT_INCLUDE}")
    target_compile_definitions(pocketfft_headers INTERFACE
        POCKETFFT_NO_MULTITHREADING "CRD_POCKETFFT_SHA=\"${pocketfft_sha}\"")
    crd_add_bench_peer(crd-bench-pocketfft-peer SOURCES examples/peers/pocketfft_peer.cpp LIBRARIES pocketfft_headers)
    add_library(crd-fft-peers INTERFACE)
    target_include_directories(crd-fft-peers SYSTEM INTERFACE "${CRD_FFTW_INCLUDE}" "${CRD_MKL_INCLUDE}")
    target_link_libraries(crd-fft-peers INTERFACE "${CRD_FFTW_LIB}" "${CRD_MKL_LIB}")
    crd_add_fft_peer_bench(bench_fft_vs_refs crd-fft-peers crd-bench-pocketfft-peer)
    crd_add_fft_peer_bench(bench_fft_f32 crd-fft-peers)
    crd_add_fft_peer_bench(bench_dct_vs_refs crd-fft-peers crd-bench-pocketfft-peer)
    target_include_directories(bench_fft_vs_refs PRIVATE examples/peers)
    target_include_directories(bench_dct_vs_refs PRIVATE examples/peers)
    message(STATUS "[hesap-vs-fft-refs] FFTW ${CRD_FFTW_LIB}, MKL ${CRD_MKL_LIB}, pocketfft ${CRD_POCKETFFT_INCLUDE} "
                   "(${pocketfft_sha})")
endif()
if(CRD_BUILD_HESAP_VS_FINUFFT)
    set(CRD_FINUFFT_DIR "$ENV{HOME}/finufft" CACHE PATH "The FINUFFT source tree, built (lib-static/libfinufft.a)")
    find_library(CRD_FINUFFT_LIB NAMES libfinufft.a finufft
                 HINTS "${CRD_FINUFFT_DIR}/lib-static" "${CRD_FINUFFT_DIR}/lib" NO_DEFAULT_PATH)
    find_library(CRD_FFTW_OMP_LIB fftw3_omp)
    find_library(CRD_FFTWF_LIB fftw3f)
    find_library(CRD_FFTWF_OMP_LIB fftw3f_omp)
    find_package(OpenMP COMPONENTS CXX)
    foreach(found IN ITEMS CRD_FINUFFT_LIB CRD_FFTW_OMP_LIB CRD_FFTWF_LIB CRD_FFTWF_OMP_LIB OpenMP_CXX_FOUND)
        if(NOT ${found})
            message(FATAL_ERROR "[hesap-vs-finufft] ${found} not found (FINUFFT in CRD_FINUFFT_DIR, FFTW 3 with its "
                                "single-precision and OpenMP libraries)")
        endif()
    endforeach()
    # FINUFFT carries both precisions and plans through FFTW's OpenMP interface.
    crd_add_fft_peer_bench(bench_nufft_vs_finufft "${CRD_FINUFFT_LIB}" "${CRD_FFTW_OMP_LIB}" "${CRD_FFTWF_OMP_LIB}"
                           "${CRD_FFTW_LIB}" "${CRD_FFTWF_LIB}" OpenMP::OpenMP_CXX)
    target_include_directories(bench_nufft_vs_finufft SYSTEM PRIVATE "${CRD_FINUFFT_DIR}/include")
    message(STATUS "[hesap-vs-finufft] FINUFFT ${CRD_FINUFFT_LIB}")
endif()

# --- The hesap-ode boards against SUNDIALS installed on the measuring host (never fetched, never in CI) ---
# SUNDIALS is a compiled C library: its declarations come in as system headers and the benches link its libraries, so
# no peer library is needed (ADR-0134 §8) and the benches build with the tree's warnings as errors.
if(CRD_BUILD_HESAP_VS_SUNDIALS)
    find_path(CRD_SUNDIALS_INCLUDE sundials/sundials_types.h)
    if(NOT CRD_SUNDIALS_INCLUDE)
        message(FATAL_ERROR "[hesap-vs-sundials] SUNDIALS headers not found (libsundials-dev)")
    endif()
    foreach(part IN ITEMS cvode cvodes arkode sunlinsolspgmr sunlinsoldense sunmatrixdense nvecserial generic)
        find_library(CRD_SUNDIALS_${part}_LIB sundials_${part})
        if(NOT CRD_SUNDIALS_${part}_LIB)
            message(FATAL_ERROR "[hesap-vs-sundials] libsundials_${part} not found (libsundials-dev)")
        endif()
    endforeach()
    function(crd_add_sundials_bench name)
        add_executable(${name} examples/${name}.cpp)
        target_include_directories(${name} SYSTEM PRIVATE "${CRD_SUNDIALS_INCLUDE}")
        set(parts)
        foreach(part IN LISTS ARGN)
            list(APPEND parts "${CRD_SUNDIALS_${part}_LIB}")
        endforeach()
        target_link_libraries(${name} PRIVATE crd-core crd-memory crd-containers crd-math crd-hesap-ode ${parts}
                                              "${CRD_SUNDIALS_generic_LIB}")
    endfunction()
    crd_add_sundials_bench(bench_ode_krylov_vs_cvode_spgmr cvode sunlinsolspgmr nvecserial)
    crd_add_sundials_bench(bench_ode_sens_vs_cvodes cvodes sunmatrixdense sunlinsoldense nvecserial)
    crd_add_sundials_bench(bench_ode_imex_vs_arkode arkode sunmatrixdense sunlinsoldense nvecserial)
    message(STATUS "[hesap-vs-sundials] SUNDIALS ${CRD_SUNDIALS_INCLUDE}")
endif()

# --- The Boost boards against the pinned Boost release (fetched on demand, never in CI) ---
# Boost reaches only crd-bench-boost-peer (examples/peers/boost_peer.hpp), so the benches build with the tree's
# warnings as errors at the baseline ISA and call one exported evaluation per array or per integration.
if(CRD_BUILD_HESAP_VS_BOOST)
    crd_add_bench_peer(crd-bench-boost-peer SOURCES examples/peers/boost_peer_special.cpp
                       examples/peers/boost_peer_dist.cpp examples/peers/boost_peer_interp.cpp
                       examples/peers/boost_peer_odeint.cpp LIBRARIES boost_headers)
    foreach(bench IN ITEMS bench_special_vs_refs bench_bessel_vs_refs bench_special_v12d bench_dist_boost
                           bench_interp_boost)
        add_executable(${bench} examples/${bench}.cpp)
        target_include_directories(${bench} PRIVATE examples/peers)
        target_link_libraries(${bench} PRIVATE crd-core crd-memory crd-containers crd-math crd-jobs crd-hesap-special
                                               crd-bench-boost-peer)
    endforeach()
    target_link_libraries(bench_dist_boost PRIVATE crd-hesap-stats)
    target_link_libraries(bench_interp_boost PRIVATE crd-hesap-interp)
endif()

# --- The ODE board: Cerid's stiff and nonstiff solvers against SUNDIALS CVODE (dense and KLU) and Boost.Odeint ---
# Both peers' options: SUNDIALS and SuiteSparse's KLU from the measuring host, odeint through crd-bench-boost-peer.
if(CRD_BUILD_HESAP_VS_SUNDIALS AND CRD_BUILD_HESAP_VS_BOOST)
    foreach(part IN ITEMS sunlinsolklu sunmatrixsparse)
        find_library(CRD_SUNDIALS_${part}_LIB sundials_${part})
        if(NOT CRD_SUNDIALS_${part}_LIB)
            message(FATAL_ERROR "[hesap-vs-sundials] libsundials_${part} not found (libsundials-dev)")
        endif()
    endforeach()
    find_path(CRD_KLU_INCLUDE klu.h PATH_SUFFIXES suitesparse)
    find_library(CRD_KLU_LIB klu)
    if(NOT CRD_KLU_INCLUDE OR NOT CRD_KLU_LIB)
        message(FATAL_ERROR "[hesap-vs-sundials] SuiteSparse KLU not found (libsuitesparse-dev)")
    endif()
    crd_add_sundials_bench(bench_ode_vs_refs cvode sunlinsoldense sunmatrixdense sunlinsolklu sunmatrixsparse
                           nvecserial)
    target_include_directories(bench_ode_vs_refs PRIVATE examples/peers)
    target_include_directories(bench_ode_vs_refs SYSTEM PRIVATE "${CRD_KLU_INCLUDE}")
    target_link_libraries(bench_ode_vs_refs PRIVATE crd-bench-boost-peer "${CRD_KLU_LIB}")
endif()

# --- The retained programs that need no peer library (CRD_BUILD_RETAINED_BENCHES) ---
# Measurement and differential-test programs of dated boards. Until 2026-10-19 no target compiled them (they were
# built by hand or by the scripts that also run their peers, scripts/run_bench_*.sh and v14*_peers.py), so an API
# change broke them unseen. The option builds each with the warning set of the tree; linux-gcc-debug and the presets
# inheriting it turn it on, and scripts/source-routes.json names it for these sources, which crd-check-source-routes
# holds to every configured tree. Their peer halves keep their scripts.
if(CRD_BUILD_RETAINED_BENCHES)
    function(crd_add_retained_bench name)
        add_executable(${name} examples/${name}.cpp)
        target_link_libraries(${name} PRIVATE
            crd-core crd-memory crd-containers crd-math crd-jobs crd-hesap crd-hesap-dense ${ARGN})
        crd_enable_default_pch(${name})
    endfunction()
    crd_add_retained_bench(bench_comms_vs_refs crd-hesap-comms crd-hesap-stats)
    crd_add_retained_bench(bench_decomp crd-hesap-tensor)
    crd_add_retained_bench(bench_distributions crd-hesap-stats)
    crd_add_retained_bench(bench_dsp_adaptive_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_conv_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_fir_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_hilbert_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_measure_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_multirate_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_nopq_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_sosfilt_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_welch_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dsp_windows_vs_refs crd-hesap-dsp)
    crd_add_retained_bench(bench_dtypes crd-hesap-tensor)
    crd_add_retained_bench(bench_einsum crd-hesap-tensor)
    crd_add_retained_bench(bench_elementwise crd-hesap-tensor)
    crd_add_retained_bench(bench_hyperopt crd-hesap-tensor)
    crd_add_retained_bench(bench_hypothesis crd-hesap-stats)
    crd_add_retained_bench(bench_interp crd-hesap-interp)
    crd_add_retained_bench(bench_logpdf_grad crd-hesap-stats)
    crd_add_retained_bench(bench_mcmc crd-hesap-stats)
    crd_add_retained_bench(bench_nn crd-hesap-stats crd-hesap-tensor)
    crd_add_retained_bench(bench_nonparametric crd-hesap-stats)
    crd_add_retained_bench(bench_permute crd-hesap-tensor)
    crd_add_retained_bench(bench_quadrature crd-hesap-quadrature)
    crd_add_retained_bench(bench_reduce crd-hesap-tensor)
    crd_add_retained_bench(bench_regression crd-hesap-stats)
    crd_add_retained_bench(bench_resampling crd-hesap-stats)
    crd_add_retained_bench(bench_rng crd-hesap-stats)
    crd_add_retained_bench(bench_samplers crd-hesap-stats)
    crd_add_retained_bench(bench_sparse_tensor crd-hesap-tensor)
    crd_add_retained_bench(bench_special_batch crd-hesap-special)
    crd_add_retained_bench(bench_tensor_io crd-hesap-tensor)
    crd_add_retained_bench(bench_tt crd-hesap-stats crd-hesap-tensor)
    crd_add_retained_bench(bench_wavelet_vs_refs crd-hesap-wavelet)
    crd_add_retained_bench(opt_scoreboard crd-hesap-opt)
    crd_add_retained_bench(prof_fft_kernel crd-hesap-fft)
endif()

# --- The permute board's HPTT peer (CRD_BUILD_HESAP_VS_HPTT; local only, never in CI or a shipped build) ---
# HPTT's four sources build as crd-hptt at the measuring host's ISA with OpenMP, its AVX kernels when that ISA has AVX
# and its portable one otherwise, not through its own CMakeLists (-march=native and -mavx whatever the host), inside
# crd-bench-hptt-peer, whose version line names the pinned commit. x64 Linux only (the root CMakeLists.txt refuses
# others): HPTT's ARM kernels are built with AArch32's -mfpu=neon, and its sources spell GCC's __restrict__ and OpenMP.
if(CRD_BUILD_HESAP_VS_HPTT)
    crd_pin(hptt_commit packages hptt commit)
    string(SUBSTRING "${hptt_commit}" 0 12 hptt_commit)
    set(hptt_sources hptt.cpp plan.cpp transpose.cpp utils.cpp)
    list(TRANSFORM hptt_sources PREPEND "${hptt_SOURCE_DIR}/src/")
    find_package(OpenMP REQUIRED COMPONENTS CXX)
    crd_peer_isa_flags(hptt_flags)
    include(CheckCXXSourceCompiles)
    string(REPLACE ";" " " CMAKE_REQUIRED_FLAGS "${hptt_flags}")
    check_cxx_source_compiles([[
#ifndef __AVX__
#error the host's ISA has no AVX
#endif
int main() { return 0; }
]] CRD_HPTT_HOST_AVX)
    unset(CMAKE_REQUIRED_FLAGS)
    add_library(crd-hptt STATIC ${hptt_sources})
    target_include_directories(crd-hptt SYSTEM PUBLIC "${hptt_SOURCE_DIR}/include")
    target_compile_definitions(crd-hptt PUBLIC $<$<BOOL:${CRD_HPTT_HOST_AVX}>:HPTT_ARCH_AVX>)
    # GCC's default -Wformat reports HPTT's one diagnostic printf (src/transpose.cpp, selectPlan, a size_t for %d, at
    # info level 1 only, which the peer never sets) once per element type; HPTT's code is not ours to edit.
    target_compile_options(crd-hptt PRIVATE ${hptt_flags} -Wno-format)
    target_link_libraries(crd-hptt PUBLIC OpenMP::OpenMP_CXX)
    set_target_properties(crd-hptt PROPERTIES POSITION_INDEPENDENT_CODE ON INTERPROCEDURAL_OPTIMIZATION OFF
                          CRD_ISA_ROLE peer)
    crd_add_bench_peer(crd-bench-hptt-peer SOURCES examples/peers/hptt_peer.cpp LIBRARIES crd-hptt)
    target_compile_definitions(crd-bench-hptt-peer PRIVATE "CRD_HPTT_COMMIT=\"${hptt_commit}\"")
    target_link_libraries(bench_permute PRIVATE crd-bench-hptt-peer)
    target_include_directories(bench_permute PRIVATE examples/peers)
    target_compile_definitions(bench_permute PRIVATE CRD_BENCH_PERMUTE_HPTT)
    message(STATUS "[hesap-vs-hptt] HPTT ${hptt_commit}, AVX kernels: ${CRD_HPTT_HOST_AVX}")
endif()

# --- The dispatch-cost board (CRD_BUILD_DISPATCH_VS_HIGHWAY; local only, never in CI or a shipped build) ---
# bench_dispatch_cost times the f16 to f32 conversion of crd-math's kernel table, through its production entry and
# through the selected body's pointer, against Highway's HWY_DYNAMIC_DISPATCH of the same conversion and its
# HWY_DYNAMIC_POINTER (examples/peers/highway_peer.hpp), on short and long arrays.
if(CRD_BUILD_DISPATCH_VS_HIGHWAY)
    crd_pin(highway_commit packages highway commit)
    string(SUBSTRING "${highway_commit}" 0 12 highway_commit)
    crd_add_bench_peer(crd-bench-highway-peer DISPATCHES SOURCES examples/peers/highway_peer.cpp LIBRARIES hwy)
    # foreach_target.h includes the unit once per compiled target, by its name on the include path.
    target_include_directories(crd-bench-highway-peer PRIVATE examples/peers)
    target_compile_definitions(crd-bench-highway-peer PRIVATE "HWY_TARGET_INCLUDE=\"highway_peer.cpp\""
                               "CRD_HIGHWAY_COMMIT=\"${highway_commit}\"")
    add_executable(bench_dispatch_cost examples/bench_dispatch_cost.cpp)
    target_include_directories(bench_dispatch_cost PRIVATE examples/peers)
    target_link_libraries(bench_dispatch_cost PRIVATE crd-core crd-memory crd-math crd-bench-highway-peer)
    message(STATUS "[dispatch-vs-highway] Highway ${highway_commit}")
endif()
