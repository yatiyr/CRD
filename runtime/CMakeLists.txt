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

add_executable(smoke_shader
    examples/smoke_shader.cpp
)

target_link_libraries(smoke_shader PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-shader
)
target_compile_definitions(smoke_shader PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(smoke_shader)

add_executable(smoke_renderer
    examples/smoke_renderer.cpp
)

target_link_libraries(smoke_renderer PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
    crd-shader
    crd-renderer
)
target_compile_definitions(smoke_renderer PRIVATE CRD_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
crd_enable_default_pch(smoke_renderer)

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

add_executable(smoke_app
    examples/smoke_app.cpp
)

target_link_libraries(smoke_app PRIVATE
    crd-core
    crd-log
    crd-app
)
crd_enable_default_pch(smoke_app)

add_executable(smoke_imgui_overlay
    examples/smoke_imgui_overlay.cpp
)

add_dependencies(smoke_imgui_overlay smoke_rhi_vulkan_shaders)

set(SMOKE_IMGUI_CONFIGS
    "${CMAKE_SOURCE_DIR}/runtime/configs/imgui_layer.toml"
)

set(SMOKE_IMGUI_CONFIG_OUTPUTS)
foreach(config_file IN LISTS SMOKE_IMGUI_CONFIGS)
    get_filename_component(config_name "${config_file}" NAME)
    set(config_copy "${CMAKE_CURRENT_BINARY_DIR}/configs/${config_name}")
    add_custom_command(
        OUTPUT "${config_copy}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/configs"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${config_file}" "${config_copy}"
        DEPENDS "${config_file}"
        VERBATIM
    )
    list(APPEND SMOKE_IMGUI_CONFIG_OUTPUTS "${config_copy}")
endforeach()

add_custom_target(smoke_imgui_configs DEPENDS ${SMOKE_IMGUI_CONFIG_OUTPUTS})
add_dependencies(smoke_imgui_overlay smoke_imgui_configs)

target_link_libraries(smoke_imgui_overlay PRIVATE
    crd-core
    crd-log
    crd-app
    crd-config
    crd-imgui
    crd-rhi
    crd-rhi-vulkan
)
crd_enable_default_pch(smoke_imgui_overlay)

add_executable(smoke_rhi_api
    examples/smoke_rhi_api.cpp
)

target_link_libraries(smoke_rhi_api PRIVATE
    crd-core
    crd-log
    crd-rhi
)
crd_enable_default_pch(smoke_rhi_api)

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

add_executable(smoke_resources_render
    examples/smoke_resources_render.cpp
)

target_link_libraries(smoke_resources_render PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-shader
    crd-renderer
)
crd_enable_default_pch(smoke_resources_render)

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
    crd-renderer
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
    crd-renderer
)
crd_enable_default_pch(smoke_mesh)

add_executable(smoke_material
    examples/smoke_material.cpp
)

target_link_libraries(smoke_material PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-platform
    crd-resources
    crd-renderer
    crd-shader
)
crd_enable_default_pch(smoke_material)

add_executable(smoke_meshgen
    examples/smoke_meshgen.cpp
)

target_link_libraries(smoke_meshgen PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-renderer
    crd-meshgen
)
crd_enable_default_pch(smoke_meshgen)

add_executable(smoke_rhi_vulkan_bootstrap
    examples/smoke_rhi_vulkan_bootstrap.cpp
)

set(SMOKE_RHI_VULKAN_SHADERS
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.vert"
    "${CMAKE_SOURCE_DIR}/runtime/examples/shaders/triangle.frag"
)
crd_compile_glsl(SMOKE_RHI_VULKAN_SPV "${CMAKE_CURRENT_BINARY_DIR}/shaders" ${SMOKE_RHI_VULKAN_SHADERS})

add_custom_target(smoke_rhi_vulkan_shaders DEPENDS ${SMOKE_RHI_VULKAN_SPV})
add_dependencies(smoke_rhi_vulkan_bootstrap smoke_rhi_vulkan_shaders)

target_link_libraries(smoke_rhi_vulkan_bootstrap PRIVATE
    crd-core
    crd-log
    crd-platform
    crd-rhi
    crd-rhi-vulkan
)
crd_enable_default_pch(smoke_rhi_vulkan_bootstrap)

add_executable(smoke_asset_import
    examples/smoke_asset_import.cpp
)

target_link_libraries(smoke_asset_import PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-rhi
    crd-rhi-vulkan
    crd-renderer
)
crd_enable_default_pch(smoke_asset_import)

add_executable(smoke_depth_prepass
    examples/smoke_depth_prepass.cpp
)

target_link_libraries(smoke_depth_prepass PRIVATE
    crd-core
    crd-log
    crd-memory
    crd-rhi
    crd-rhi-vulkan
    crd-renderer
)
crd_enable_default_pch(smoke_depth_prepass)

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

# Reference-class shootout — built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
# Eigen 3.4 + OpenBLAS are fetched via CPM at configure time.
#
# crd_add_hesap_vs_ref_bench(<name> [USE_JOBS] [USE_OPENBLAS]) — collapses the
# add_executable + link + include-dir + OpenMP + /WX-suppression boilerplate
# shared by every vs-reference bench. Source is examples/<name>.cpp.
#   USE_JOBS     — also link crd-jobs (benches that call jobs::frame_reset).
#   USE_OPENBLAS — also link openblas + its hard-coded generated-header dirs
#                  (see note below). Omit for Eigen-only shootouts.
# Eigen's MatrixXd*MatrixXd parallelism in 3.4 needs OpenMP in the CALLER's TU
# (`#pragma omp parallel` is expanded inline); without it Eigen silently runs
# serial regardless of setNbThreads(). /WX- is set via source-file property so
# it comes AFTER crd-warnings' INTERFACE /WX and wins. PCH is skipped (conflicts
# with eigen + cblas system headers).
function(crd_add_hesap_vs_ref_bench name)
    cmake_parse_arguments(ARG "USE_JOBS;USE_OPENBLAS" "" "" ${ARGN})
    add_executable(${name} examples/${name}.cpp)
    target_link_libraries(${name} PRIVATE
        crd-core crd-memory crd-containers crd-math crd-hesap crd-hesap-dense eigen_headers)
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
    if(OpenMP_CXX_FOUND)
        target_link_libraries(${name} PRIVATE OpenMP::OpenMP_CXX)
    endif()
    if(MSVC)
        set_source_files_properties(examples/${name}.cpp PROPERTIES
            COMPILE_FLAGS "/WX- /wd4242 /wd4244 /wd4267 /wd4127 /wd4146")
    else()
        set_source_files_properties(examples/${name}.cpp PROPERTIES COMPILE_FLAGS "-Wno-error")
    endif()
endfunction()

if(CRD_BUILD_HESAP_VS_REFERENCE)
    find_package(OpenMP COMPONENTS CXX)
    if(OpenMP_CXX_FOUND)
        message(STATUS "[hesap-vs-reference] OpenMP enabled for Eigen-MT")
    else()
        message(WARNING "[hesap-vs-reference] OpenMP NOT found — Eigen will run SERIAL")
    endif()

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
    set(CRD_SS_DIR "${CMAKE_BINARY_DIR}/_deps/suitesparse-mm")
    file(MAKE_DIRECTORY "${CRD_SS_DIR}")
    set(CRD_SS_BASE "https://suitesparse-collection-website.herokuapp.com/MM")
    foreach(ss_pair "HB/bcsstk13" "HB/bcsstk24" "HB/west2021" "HB/bcsstk25" "HB/sherman3" "HB/gemat11")
        string(REGEX REPLACE ".*/" "" ss_name "${ss_pair}")
        if(NOT EXISTS "${CRD_SS_DIR}/${ss_name}/${ss_name}.mtx")
            set(ss_tgz "${CRD_SS_DIR}/${ss_name}.tar.gz")
            message(STATUS "[hesap-vs-reference] fetching SuiteSparse ${ss_pair}")
            file(DOWNLOAD "${CRD_SS_BASE}/${ss_pair}.tar.gz" "${ss_tgz}" STATUS ss_status TIMEOUT 60)
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
    # v4-corpus: real SuiteSparse matrices through the cooked 'HMTX' resource path
    # (cook + read_matrix_resource + build_csr timed vs direct read; each matrix also
    # verified through ResourceManager mount + load_sync). No Eigen in the body.
    crd_add_hesap_vs_ref_bench(bench_hesap_matrix_resource_vs_reference)
    target_link_libraries(bench_hesap_matrix_resource_vs_reference PRIVATE
        crd-hesap-sparse crd-hesap-resources crd-resources crd-platform)
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
