crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(TARGET tomlplusplus::tomlplusplus)
    set(_crd_toml_target tomlplusplus::tomlplusplus)
else()
    set(_crd_toml_target tomlplusplus)
endif()

add_library(crd-lod STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-lod
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# REN-40-C1: the seam this module exists to hold. `crd-resources` is deliberately
# math-module-free, and `crd-geometry-mesh-processing` deliberately knows nothing
# about resources — so the code that turns a MeshResource into an LOD chain cannot
# live in either of them without inverting a dependency. It lives here.
target_link_libraries(crd-lod
    PUBLIC
        crd-core
        crd-math
        crd-memory
        crd-containers
        crd-resources
    PRIVATE
        crd-geometry-mesh-processing
        ${_crd_toml_target}
)

# ⛔⛔ TOML_EXCEPTIONS=0 — the checked-result toml++ API, and this is a SCAR not a
# style choice: a thrown `parse_error` unwinding while a live GPU device had the
# validation layer hooked into SEH dispatch KILLED THE PROCESS, so an authored-asset
# TYPO became a crash the moment disk-first loading made user edits reachable. It
# also kills the mixed-mode ODR hazard (some cookers throwing, some not).
target_compile_definitions(crd-lod PRIVATE TOML_EXCEPTIONS=0)

target_compile_features(crd-lod PUBLIC cxx_std_20)
crd_enable_default_pch(crd-lod)
