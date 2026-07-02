add_executable(crd-hesap-tensor-tests
    test_tensor_view.cpp
    test_dtypes.cpp
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
