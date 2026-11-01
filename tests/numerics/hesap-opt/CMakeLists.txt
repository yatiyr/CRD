add_executable(crd-hesap-opt-tests
    test_gradient_descent.cpp
    test_derivatives.cpp
    test_line_search.cpp
    test_conjugate_gradient.cpp
    test_momentum.cpp
    test_lbfgs.cpp
    test_lbfgsb.cpp
    test_lm.cpp
    test_lm_sparse.cpp
    test_newton.cpp
    test_newton_sparse.cpp
    test_trust_region.cpp
    test_constrained.cpp
    test_stochastic.cpp
    test_qp.cpp
    test_nlp.cpp
    test_lp.cpp
    test_conic.cpp
    test_model.cpp
    test_direct_search.cpp
    test_cobyla.cpp
    test_newuoa.cpp
    test_bobyqa.cpp
    test_global.cpp
    test_mip.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-opt-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dense
    crd-hesap-sparse
    crd-hesap-ordering   # SupernodalCholesky symbolic (sparse-LM normal-equations factor, v7-e-2)
    crd-hesap-direct     # SupernodalCholesky (the JᵀJ crush vehicle)
    crd-hesap-stats      # Philox counter-RNG (v7-i minibatch sampler)
    crd-hesap-opt
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-opt-tests)

crd_discover_tests(crd-hesap-opt-tests)

# The model's out-of-memory fatal, one child process per mode (a fatal cannot run in-process): an allocator that
# refuses the objective's or a constraint's function leaves the model out of memory, fatal in every build, never a
# null function stored. Source and protocol: tests/support/diag/specimens/hesap_opt_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-opt-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_opt_fatal_specimen.cpp")
target_link_libraries(crd-hesap-opt-fatal-specimen PRIVATE crd-core crd-memory crd-hesap-opt)
crd_diag_fatal_tests(crd-hesap-opt-fatal-specimen PREFIX crd-hesap-opt-fatal OWNER crd-hesap-opt-tests
    MODES model-objective-refused model-eq-refused model-ineq-refused)

# The Powell-method differential tests (CRD_BUILD_HESAP_VS_NLOPT; local only, never in CI or a shipped build).
# cobyla_difftest, newuoa_difftest and bobyqa_difftest compare hesap-opt's ports with NLopt bit for bit: each routine on
# identical inputs, then whole minimizations with their evaluation counts. NLopt's library builds by its own CMake as a
# separate project (its install rules, rpath cache entries and options stay out of this tree), static, at the baseline
# ISA, without its C++ (StoGO, AGS), LGPL (Luksan) and binding parts. Each oracle unit (nlopt/<algorithm>_oracle.c)
# compiles one algorithm's source again with its file-scope functions external, under the tree's floating-point
# contract, as NLopt's own build compiles it (contraction off; newuoa.c without _FORTIFY_SOURCE on GCC 12 and later).
if(CRD_BUILD_HESAP_VS_NLOPT)
    include(ExternalProject)
    crd_pin(nlopt_version packages nlopt version)
    set(nlopt_prefix "${CMAKE_BINARY_DIR}/_deps/nlopt-oracle")
    set(nlopt_library
        "${nlopt_prefix}/install/lib/${CMAKE_STATIC_LIBRARY_PREFIX}nlopt${CMAKE_STATIC_LIBRARY_SUFFIX}")
    ExternalProject_Add(crd-nlopt-oracle-build
        SOURCE_DIR "${nlopt_SOURCE_DIR}"
        PREFIX "${nlopt_prefix}"
        BINARY_DIR "${nlopt_prefix}/build"
        INSTALL_DIR "${nlopt_prefix}/install"
        CMAKE_ARGS
            "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
            "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>
            -DCMAKE_INSTALL_LIBDIR=lib
            -DBUILD_SHARED_LIBS=OFF
            -DNLOPT_CXX=OFF -DNLOPT_FORTRAN=OFF -DNLOPT_LUKSAN=OFF -DNLOPT_TESTS=OFF
            -DNLOPT_PYTHON=OFF -DNLOPT_OCTAVE=OFF -DNLOPT_MATLAB=OFF -DNLOPT_GUILE=OFF -DNLOPT_JAVA=OFF
            -DNLOPT_SWIG=OFF
        BUILD_BYPRODUCTS "${nlopt_library}")
    add_library(crd-nlopt-oracle STATIC IMPORTED GLOBAL)
    set_target_properties(crd-nlopt-oracle PROPERTIES IMPORTED_LOCATION "${nlopt_library}")
    add_dependencies(crd-nlopt-oracle crd-nlopt-oracle-build)
    find_library(nlopt_libm m)
    if(nlopt_libm)
        set_property(TARGET crd-nlopt-oracle PROPERTY INTERFACE_LINK_LIBRARIES "${nlopt_libm}")
    endif()
    foreach(algorithm IN ITEMS cobyla newuoa bobyqa)
        add_library(crd-nlopt-oracle-${algorithm} OBJECT nlopt/${algorithm}_oracle.c)
        # NLopt's headers and the configuration header its build writes; SYSTEM, since the included reference
        # source is NLopt's code, compiled as NLopt's build compiles it.
        target_include_directories(crd-nlopt-oracle-${algorithm} SYSTEM PRIVATE
            "${nlopt_SOURCE_DIR}/src/algs/${algorithm}" "${nlopt_SOURCE_DIR}/src/util" "${nlopt_SOURCE_DIR}/src/api"
            "${nlopt_prefix}/build")
        target_link_libraries(crd-nlopt-oracle-${algorithm} PRIVATE crd-simd-flags)
        add_dependencies(crd-nlopt-oracle-${algorithm} crd-nlopt-oracle-build)
        set_target_properties(crd-nlopt-oracle-${algorithm} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
        add_executable(${algorithm}_difftest nlopt/${algorithm}_difftest.cpp)
        target_link_libraries(${algorithm}_difftest PRIVATE
            crd-warnings crd-hesap-opt crd-nlopt-oracle-${algorithm} crd-nlopt-oracle)
        crd_enable_default_pch(${algorithm}_difftest)
        add_test(NAME crd-nlopt-${algorithm}-difftest COMMAND ${algorithm}_difftest)
        set_tests_properties(crd-nlopt-${algorithm}-difftest PROPERTIES TIMEOUT 300 LABELS "hesap;nlopt-oracle")
        crd_test_target(crd-nlopt-${algorithm}-difftest ${algorithm}_difftest)
    endforeach()
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 12)
        set_source_files_properties(nlopt/newuoa_oracle.c PROPERTIES COMPILE_OPTIONS -U_FORTIFY_SOURCE)
    endif()
    message(STATUS "[hesap-vs-nlopt] COBYLA, NEWUOA and BOBYQA differential tests against NLopt ${nlopt_version}")
endif()
