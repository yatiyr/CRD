# crd-fuzz-harness — the bounded fuzz harness shared by the ingestion fuzz targets (REPO.DEV.9).
# Header-only: the BudgetAllocator, the seed sink, the oracle macro and the target entry-point declarations. The replay
# driver (src/replay_main.cpp) is compiled into every replay executable by crd_fuzz_target() (cmake/CrdFuzz.cmake).
# Owned by every module whose tests register fuzz targets (execution/ceir, gpu/kir); the harness itself links only
# the base substrate, so a fuzz executable's link set is the target's own module plus core/containers/memory.
add_library(crd-fuzz-harness INTERFACE)
target_include_directories(crd-fuzz-harness INTERFACE include)
# A target's oracles restate the engine's arithmetic, so they round as the engine does: left alone, Clang on ARM64 fuses
# `a + b * c` into one FMA even at -O0 and an oracle disagrees with the kernels it holds.
target_compile_options(crd-fuzz-harness INTERFACE ${CRD_FP_CONTRACT_OPTIONS})
target_link_libraries(crd-fuzz-harness INTERFACE
    crd-core
    crd-containers
    crd-memory
)
