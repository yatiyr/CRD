add_executable(crd-toml-tests test_toml.cpp)

target_link_libraries(crd-toml-tests PRIVATE
    crd-core
    crd-containers
    crd-toml
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-toml-tests)
crd_discover_tests(crd-toml-tests)

# The child lookup's contract fatal, in a child process (a fatal cannot run in-process): an index past the last child
# is fatal in every build. Source and protocol: tests/support/diag/specimens/toml_fatal_specimen.cpp.
include(CrdDiag)
crd_diag_specimen(crd-toml-fatal-specimen
    SOURCES "${CMAKE_SOURCE_DIR}/tests/support/diag/specimens/toml_fatal_specimen.cpp")
target_link_libraries(crd-toml-fatal-specimen PRIVATE crd-core crd-containers crd-toml)
crd_diag_fatal_tests(crd-toml-fatal-specimen PREFIX crd-toml-fatal OWNER crd-toml-tests MODES child-at-past-end)
