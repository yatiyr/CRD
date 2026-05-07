add_executable(crd-scene-tests
    test_entity.cpp
    test_slot_map.cpp
    test_world.cpp
    test_component_registry.cpp
    test_archetype_chunk.cpp
    test_archetype_graph.cpp
    test_archetype_storage.cpp
    test_sparse_set_storage.cpp
    test_mixed_chunk_visitor.cpp
    test_world_tlsf.cpp
)

target_link_libraries(crd-scene-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-scene
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-tests)

catch_discover_tests(crd-scene-tests)
