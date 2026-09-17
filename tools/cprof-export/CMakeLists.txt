# cprof_export — DIAG.6c(e): the offline CPROF -> Perfetto trace-events JSON export CLI.
# Links crd-perf ONLY (plus its foundation deps) — NOT crd-perf-ui; the native viewer stays optional.
add_executable(cprof_export
    src/main.cpp
)

target_link_libraries(cprof_export PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-perf
)

target_compile_features(cprof_export PRIVATE cxx_std_20)
crd_enable_default_pch(cprof_export)
