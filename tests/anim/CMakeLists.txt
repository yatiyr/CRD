add_executable(crd-anim-tests
    test_anim.cpp # GEO-8: SKEL/ANIM round-trips, bind-pose identity, SLERP semantics, DQ oracle, THE FOX GATE
)

target_link_libraries(crd-anim-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-platform
    crd-hesap-interp
    crd-anim
    crd-asset-io
    crd-resources
    crd-cooker
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-anim-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # std::getenv (the fixture env)
endif()

crd_enable_default_pch(crd-anim-tests)

# the Fox fixture path (the io_corpus env pattern — ctest working dirs differ from the repo root)
catch_discover_tests(crd-anim-tests
    PROPERTIES ENVIRONMENT "CRD_ASSETS_SOURCE=${CMAKE_SOURCE_DIR}/assets/source"
)
