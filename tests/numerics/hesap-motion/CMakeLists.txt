add_executable(crd-hesap-motion-tests
    test_motion.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-motion-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-hesap
    crd-hesap-special
    crd-hesap-motion
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-motion-tests)

catch_discover_tests(crd-hesap-motion-tests)
