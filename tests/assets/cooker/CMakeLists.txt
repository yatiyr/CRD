add_executable(crd-cooker-tests
    test_mesh_cook_options.cpp
    test_mesh_wave1.cpp
    test_texture_cook.cpp
    test_scene_decompose.cpp
    test_gltf_roundtrip.cpp # GEO-4 pt 3: import -> cook -> native edit -> export -> re-import
    test_timeline_cook.cpp  # GEO-9: .otio -> TIML through the real handler; exact edit survival; determinism
    test_audio_cook.cpp     # GEO-10: .flac -> ABUF exact normalization + .mid -> MIDI; determinism
)

target_link_libraries(crd-cooker-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-platform
    crd-resources
    crd-timeline # GEO-9: the TIML loader + eval under the cook gate
    crd-audio    # GEO-10: the ABUF/MIDI loaders under the cook gate
    crd-cooker
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-cooker-tests)

catch_discover_tests(crd-cooker-tests)
