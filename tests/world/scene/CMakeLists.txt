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
    test_spatial_bvh_index.cpp
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
    crd-geometry-spatial   # transitive through crd-scene; explicit for clarity
    crd-jobs               # SceneJobsListener for the concurrent SpatialBVHIndex test
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-scene-tests)

crd_discover_tests(crd-scene-tests)

# The scene's contract fatals, one child process per mode (a fatal cannot run in-process): a lookup a stored id or a
# writer's contract makes certain is fatal on a miss in every build, and so is an allocation the world's allocator
# refuses (out of memory, never a pool, an index or a chunk built on null). Source and protocol:
# tests/support/diag/specimens/scene_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-scene-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/scene_fatal_specimen.cpp")
target_link_libraries(crd-scene-fatal-specimen PRIVATE crd-core crd-math crd-scene)
crd_diag_fatal_tests(crd-scene-fatal-specimen PREFIX crd-scene-fatal OWNER crd-scene-tests
    MODES registry-require-unregistered archetype-require-unissued transform-writer-without-transform
          transform-writer-dead-entity commands-add-unregistered sparse-pool-refused sparse-dense-refused
          shared-pool-refused shared-grow-refused relation-index-refused archetype-pool-refused chunk-pool-refused)
