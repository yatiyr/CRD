add_executable(crd-audio-tests
    test_audio_codecs.cpp  # GEO-10: WAV/AIFF/FLAC bit-exact round-trips, MD5 vectors, CRC refusals, transcode
    test_audio_graph.cpp   # GEO-10: ABUF/AGRF round-trips + THE 4-TRACK bit-stable offline render gate
    test_midi_realtime.cpp # GEO-10: SMF -> MIDI 2.0 exact events + SPSC ring + THE device SOAK
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
    Catch2::Catch2WithMain
)

if(MSVC)
    target_compile_definitions(crd-audio-tests PRIVATE _CRT_SECURE_NO_WARNINGS) # snprintf hex in the MD5 gate
endif()

crd_enable_default_pch(crd-audio-tests)

catch_discover_tests(crd-audio-tests)
