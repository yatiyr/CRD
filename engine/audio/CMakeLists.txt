# crd-audio — GEO-10 (D-007 row 75): the DAW substrate — OUR OWN WAV/AIFF/FLAC codecs (the MED-5 lossless
# floor, built once, consumed here), AudioBufferResource + the audio-graph-as-resource, hesap-dsp kernels
# (RBJ biquads, aryule LPC — never a 3rd-party DSP), MIDI 2.0-native MidiResource, and the realtime layer
# (WASAPI device + lock-free command queue; the callback thread never allocates/locks).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-audio STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-audio
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-audio
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-time
        crd-resources
        crd-hesap-dsp
        crd-hesap-interp
        crd-jobs
)

target_compile_features(crd-audio PUBLIC cxx_std_20)
crd_enable_default_pch(crd-audio)
