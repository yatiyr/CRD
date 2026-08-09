# crd-ceir-host — the crd-jobs EXECUTION PROVIDER bridge (D-007 CEIR-6b; ADR-0109 §4.2). Implements the abstract
# crd::ceir::IExecutionProvider by lowering ceir.task ops onto crd::jobs — CEIR becomes an authorable front-end to the
# existing fibers (§31: "do NOT create a second scheduler"). ⛔ This is a BRIDGE MODULE: it depends on BOTH crd-ceir AND
# crd-jobs (the dependency inversion). crd-ceir itself NEVER link-edges to crd-jobs — the crd-ceir-invariants gate
# (scripts/check_ceir_invariants) enforces that the core stays host-only + jobs-free.
crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-ceir-host STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-ceir-host
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-ceir-host
    PUBLIC
        crd-ceir      # the IR + the IExecutionProvider seam + the reference Interpreter (exec)
        crd-core      # types, asserts
        crd-containers
        crd-memory
    PRIVATE
        crd-jobs      # the execution provider lowers task.parallel_for onto crd::jobs::parallel_for (NOT in any public header)
        crd-warnings
)

target_compile_features(crd-ceir-host PUBLIC cxx_std_20)
crd_enable_default_pch(crd-ceir-host)
