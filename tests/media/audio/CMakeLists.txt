add_executable(crd-audio-tests
    test_audio_codecs.cpp  # GEO-10: WAV/AIFF/FLAC bit-exact round-trips, MD5 vectors, CRC refusals, transcode
    test_audio_graph.cpp   # GEO-10: ABUF/AGRF round-trips + THE 4-TRACK bit-stable offline render gate
    test_midi_realtime.cpp # GEO-10: SMF -> MIDI 2.0 exact events + SPSC ring + THE device SOAK
    test_ceir_audio_render.cpp # CEIR-31a-1b-ii: execute_audio_graph_ceir BIT-EXACT vs render_graph (committed .ceir vs AGRF)
)

target_link_libraries(crd-audio-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-time
    crd-hesap-dsp
    crd-hesap-interp
    crd-math
    crd-jobs
    crd-resources
    crd-audio
    crd-ceir # CEIR-31a-1b-ii: the render test parses the committed .ceir + calls execute_audio_graph_ceir
    Catch2::Catch2WithMain
)

# CEIR-31a-1b-ii: the render test slurps the committed assets/ceir/*.ceir -- CRD_REPO_DIR is the repo-root anchor (the
# house pattern; ⛔ NO `#ifndef "."` TU fallback -- the cwd-luck scar, feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck).
target_compile_definitions(crd-audio-tests PRIVATE CRD_REPO_DIR="${CMAKE_SOURCE_DIR}")

if(MSVC)
    target_compile_definitions(crd-audio-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # snprintf hex in the MD5 gate
endif()

crd_enable_default_pch(crd-audio-tests)

crd_discover_tests(crd-audio-tests)
