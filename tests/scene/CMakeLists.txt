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
    test_relations.cpp
    test_query.cpp
    test_schedule.cpp
    test_index_framework.cpp
    test_transform.cpp
    test_scene_resource.cpp
    test_obek.cpp
    test_shared_component_pool.cpp
    test_world_tlsf.cpp
    test_phase_3_0_freeze.cpp
)

target_link_libraries(crd-scene-tests PRIVATE
    crd-core
    crd-containers
    crd-math
    crd-memory
    crd-preset
    crd-profile
    crd-scene
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-tests)

catch_discover_tests(crd-scene-tests)
