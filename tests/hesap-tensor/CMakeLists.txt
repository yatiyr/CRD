add_executable(crd-hesap-tensor-tests
    test_tensor_view.cpp
    test_dtypes.cpp
    test_elementwise.cpp
    test_reduce.cpp
    test_dtype_set.cpp
    test_reduce_axes.cpp
    test_einsum_path.cpp
)

target_link_libraries(crd-hesap-tensor-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-tensor
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-tests)

catch_discover_tests(crd-hesap-tensor-tests)

# --- v14-d permute gates (separate executable: keeps parallel work on the ---
# --- main test target conflict-free)                                      ---
add_executable(crd-hesap-tensor-permute-tests
    test_permute.cpp
)

target_link_libraries(crd-hesap-tensor-permute-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-tensor
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-permute-tests)

catch_discover_tests(crd-hesap-tensor-permute-tests)

# --- v14-g hyper-optimizer gates (separate executable: keeps the large ---
# --- planner suites out of the main target's compile time)             ---
add_executable(crd-hesap-tensor-hyperopt-tests
    test_hyperopt.cpp
    test_hyperopt_corpus.cpp
)

target_link_libraries(crd-hesap-tensor-hyperopt-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap-tensor
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-hyperopt-tests)

catch_discover_tests(crd-hesap-tensor-hyperopt-tests)

# --- v14-l I/O gates (separate executable: links platform for filesystem) ---
add_executable(crd-hesap-tensor-io-tests
    test_io.cpp
)

target_link_libraries(crd-hesap-tensor-io-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-jobs
    crd-hesap-tensor
    crd-hesap-resources
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-io-tests)

# corpus path via test env: ctest working dirs differ from the repo root
catch_discover_tests(crd-hesap-tensor-io-tests
    PROPERTIES ENVIRONMENT "CRD_IO_CORPUS=${CMAKE_CURRENT_SOURCE_DIR}/io_corpus"
)

# --- v14-k tensor-train gates (separate executable: links hesap-dense) ---
add_executable(crd-hesap-tensor-tt-tests
    test_tt.cpp
)

target_link_libraries(crd-hesap-tensor-tt-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-tt-tests)

catch_discover_tests(crd-hesap-tensor-tt-tests)

# --- v14-i sparse-tensor gates (separate executable; + the sparse-CP glue) ---
add_executable(crd-hesap-tensor-sparse-tests
    test_sparse.cpp
    test_sparse_cp.cpp
)

target_link_libraries(crd-hesap-tensor-sparse-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-sparse-tests)

catch_discover_tests(crd-hesap-tensor-sparse-tests)

# --- v14-j decomposition gates (separate executable: links hesap-dense) ---
add_executable(crd-hesap-tensor-decomp-tests
    test_decomp.cpp
)

target_link_libraries(crd-hesap-tensor-decomp-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-decomp-tests)

catch_discover_tests(crd-hesap-tensor-decomp-tests)

# --- v14-m NN-inference gates (separate executable: loads the safetensors ---
# --- corpus via io.hpp => links platform)                                  ---
add_executable(crd-hesap-tensor-nn-tests
    test_nn.cpp
)

target_link_libraries(crd-hesap-tensor-nn-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-jobs
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-nn-tests)

# corpus path via test env: ctest working dirs differ from the repo root
catch_discover_tests(crd-hesap-tensor-nn-tests
    PROPERTIES ENVIRONMENT "CRD_NN_CORPUS=${CMAKE_CURRENT_SOURCE_DIR}/nn_corpus"
)

# --- v14-z CLI gates (pulls the anchor => needs the full anchor-gated edge set) ---
add_executable(crd-hesap-tensor-cli-tests
    test_cli.cpp
)

target_link_libraries(crd-hesap-tensor-cli-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-jobs
    crd-hesap
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-cli-tests)

catch_discover_tests(crd-hesap-tensor-cli-tests)

# --- v14-h batched-LA gates (separate executable: links hesap-dense + jobs) ---
add_executable(crd-hesap-tensor-batched-tests
    test_batched.cpp
)

target_link_libraries(crd-hesap-tensor-batched-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-jobs
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-batched-tests)

catch_discover_tests(crd-hesap-tensor-batched-tests)

# --- v14-f einsum-execution gates (separate executable: einsum_exec.hpp is ---
# --- header-only so only EXECUTING targets pay the hesap-dense link edge)  ---
add_executable(crd-hesap-tensor-einsum-tests
    test_einsum_exec.cpp
    test_einsum_reverse.cpp  # v16-c: the einsum VJP, executed on the same EinsumPlan (autodiff header-only bridge)
)

target_link_libraries(crd-hesap-tensor-einsum-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-tensor
    crd-hesap-dense
    crd-hesap-autodiff  # v16-c: einsum_reverse.hpp — a header-only bridge; only this einsum-executing target pays it
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-einsum-tests)

catch_discover_tests(crd-hesap-tensor-einsum-tests)
