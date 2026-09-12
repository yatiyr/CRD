add_executable(crd-asset-io-tests
    test_condition.cpp
    test_gltf.cpp
    test_gltf_export.cpp # GEO-4: the exporter round-trip gate (our writer validated by our importer)
    test_json.cpp
    test_xml.cpp # GEO-5: the XML parser gate (the OPC/3MF foundation)
    test_threemf.cpp # GEO-5 pt 3: 3MF import + the watertight export gate + the OPC round-trip through our ZIP
    test_obj.cpp
    test_otio.cpp # GEO-9: the .otio interchange edge — exact rational import, export round-trip, refusal classes
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
    crd-resources           # GEO-4: png_encode_rgba + ldr_decode — the texture-embed round-trip gate
    mikktspace              # GEO-2: the REFERENCE tangent implementation as TEST ORACLE only (never in the product path)
    Catch2::Catch2WithMain
)

if(MSVC)
    # test_threemf.cpp: std::getenv/std::fopen for the CRD_3MF_DUMP conformance hook + the reference fixture
    target_compile_definitions(crd-asset-io-tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# test_condition.cpp compiles mikktspace.c (the oracle) inside its TU; that third-party code does benign `x << 32` hash
# mixing that trips UBSan's shift check on gcc-asan configs (same exemption as the cooker's mesh.cpp).
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_condition.cpp" PROPERTIES
    COMPILE_OPTIONS "$<$<AND:$<COMPILE_LANG_AND_ID:CXX,GNU>,$<CONFIG:Asan>>:-fno-sanitize=shift-exponent>")

crd_enable_default_pch(crd-asset-io-tests)

# fixture path via test env: ctest working dirs differ from the repo root (the io_corpus pattern)
catch_discover_tests(crd-asset-io-tests
    PROPERTIES ENVIRONMENT "CRD_ASSETIO_DATA=${CMAKE_CURRENT_SOURCE_DIR}/data"
)
