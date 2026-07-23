add_executable(crd-asset-io-tests
    test_condition.cpp
    test_gltf.cpp
    test_json.cpp
    test_obj.cpp
    test_ply.cpp
    test_stl.cpp
)

target_link_libraries(crd-asset-io-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-math
    crd-units
    crd-geometry-primitives
    crd-geometry-bvh
    crd-geometry-mesh
    crd-asset-io
    mikktspace              # GEO-2: the REFERENCE tangent implementation as TEST ORACLE only (never in the product path)
    Catch2::Catch2WithMain
)

# test_condition.cpp compiles mikktspace.c (the oracle) inside its TU; that third-party code does benign `x << 32` hash
# mixing that trips UBSan's shift check on gcc-asan configs (same exemption as the cooker's mesh.cpp).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_condition.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<CONFIG:Asan>>:-fno-sanitize=shift-exponent>")

crd_enable_default_pch(crd-asset-io-tests)

catch_discover_tests(crd-asset-io-tests)
