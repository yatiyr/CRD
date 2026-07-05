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
)

target_link_libraries(crd-hesap-tensor-einsum-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap-tensor
    crd-hesap-dense
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-tensor-einsum-tests)

catch_discover_tests(crd-hesap-tensor-einsum-tests)
