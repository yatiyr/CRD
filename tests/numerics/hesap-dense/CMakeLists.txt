add_executable(crd-hesap-dense-tests
    test_matrix_catalog.cpp
    test_vector.cpp
    test_blas1_real.cpp
    test_blas1_complex.cpp
    test_blas1_cli.cpp
    test_matrix.cpp
    test_blas2_real.cpp
    test_blas2_complex.cpp
    test_blas2_banded.cpp
    test_blas3_real.cpp
    test_blas3_complex.cpp
    test_blas3_parallel.cpp
    test_dense_kernels.cpp
    test_dense_vector_kernels.cpp
    test_parallel_triangular.cpp
    test_lu.cpp
    test_cholesky.cpp
    test_ldlt.cpp
    test_qr.cpp
    test_cod.cpp
    test_interp_decomp.cpp
    test_randomized_range.cpp
    test_lstsq.cpp
    test_nnls_tls.cpp
    test_eig_nonsym.cpp
    test_linear_op_condition.cpp
    test_refinement.cpp
    test_solvers_cli.cpp
    test_random_property.cpp
    test_eig_sym.cpp
    test_eig_dc.cpp
    test_mrrr_eigenvalues.cpp
    test_svd.cpp
)

target_link_libraries(crd-hesap-dense-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-dense
    crd-jobs
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-dense-tests)

crd_discover_tests(crd-hesap-dense-tests)

# ADR-0134 §8: the [kernels] cases once per compiled level (crd-hesap-dense-kernels-at-<level>).
crd_add_kernel_level_tests(crd-hesap-dense-tests "[kernels]")

# The owning types' and the GEMM-family products' out-of-memory fatals, one child process per mode (a fatal cannot run
# in-process): an allocator that refuses their storage or a packing buffer, or a size that overflows, leaves them out
# of memory, fatal in every build, never a null or short buffer filled. Source and protocol:
# tests/support/diag/specimens/hesap_dense_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-hesap-dense-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/hesap_dense_fatal_specimen.cpp")
target_link_libraries(crd-hesap-dense-fatal-specimen PRIVATE crd-core crd-memory crd-jobs crd-hesap-dense)
crd_diag_fatal_tests(crd-hesap-dense-fatal-specimen PREFIX crd-hesap-dense-fatal OWNER crd-hesap-dense-tests
    MODES matrix-elements-refused matrix-size-overflow vector-elements-refused permutation-refused
          structured-storage-refused banded-height-overflow gemm-pack-refused gemm-small-pack-refused
          gemm-parallel-pack-refused syrk-pack-refused)
