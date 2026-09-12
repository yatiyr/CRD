add_executable(crd-profile-tests
    test_profile.cpp
    test_profile_resolver.cpp
)

target_link_libraries(crd-profile-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-resources
    crd-profile
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-profile-tests)

catch_discover_tests(crd-profile-tests)
