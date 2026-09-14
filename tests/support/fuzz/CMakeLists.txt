# crd-fuzz-harness — the bounded fuzz harness shared by the ingestion fuzz targets (REPO.DEV.9).
# Header-only: the BudgetAllocator, the seed sink, the oracle macro and the target entry-point declarations. The replay
# driver (src/replay_main.cpp) is compiled into every replay executable by crd_fuzz_target() (cmake/CrdFuzz.cmake).
# Owned by every module whose tests register fuzz targets (execution/ceir, gpu/kir); the harness itself links only
# the base substrate, so a fuzz executable's link set is the target's own module plus core/containers/memory.
add_library(crd-fuzz-harness INTERFACE)
target_include_directories(crd-fuzz-harness INTERFACE include)
target_link_libraries(crd-fuzz-harness INTERFACE
    crd-core
    crd-containers
    crd-memory
)
