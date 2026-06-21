add_executable(crd-hesap-comms-tests
    test_modulation.cpp
    test_pulse_shaping.cpp
    test_sync.cpp
    test_equalizer.cpp
    test_channel.cpp
    test_ofdm.cpp
    test_cli.cpp
)

target_link_libraries(crd-hesap-comms-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-hesap
    crd-hesap-comms
    crd-hesap-stats
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-hesap-comms-tests)

catch_discover_tests(crd-hesap-comms-tests)
