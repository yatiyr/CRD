# crd-technique-cook tests — REN-37.2/37.4 (D-007 row 140): the LIGHTING TECHNIQUE as an authored asset, its
# round-trip, the BINDING CONTRACT verified against a frame-graph pass, and the SPLICE that lets a technique ship
# as data with no engine code.
add_executable(crd-technique-cook-tests
    test_technique_asset.cpp
)

target_link_libraries(crd-technique-cook-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-frame-cook
    crd-technique-cook
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-technique-cook-tests)

catch_discover_tests(crd-technique-cook-tests)
