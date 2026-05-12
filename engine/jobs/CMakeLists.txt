# Enable platform-specific assembly language for the fiber context switch.
# This is the only non-portable code in crd-jobs; all other files are C++20.
if(WIN32)
    enable_language(ASM_MASM)
    set(CRD_JOBS_ASM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/fiber_switch_win64.asm")
else()
    enable_language(ASM)
    set(CRD_JOBS_ASM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/src/fiber_switch_lin64.S")
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-jobs STATIC ${SOURCES} ${HEADERS} ${CRD_JOBS_ASM_SRC})

target_include_directories(crd-jobs
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-jobs
    PUBLIC
        crd-core
)

target_compile_features(crd-jobs PUBLIC cxx_std_20)

# Hand-rolled asm context switch is incompatible with MSVC LTO (/GL+/LTCG):
# the linker can rewrite call sequences across TU boundaries in ways that break
# the fiber trampoline's assumptions about register state on entry.
set_target_properties(crd-jobs PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# Root cause of the historical win-release / linux-gcc-release SEGFAULT in
# "jobs: run_and_wait from inside a worker fiber" is now understood and fixed in
# source: job_fiber_trampoline reads thread-locals on the far side of a fiber
# resume that can migrate the fiber to a different OS thread, and the optimizer
# was caching the pre-resume TLS base across the hand-rolled fiber_switch. The
# fix is the CRD_JOBS_TLS_OPAQUE accessors in worker_pool.cpp (portable; covers
# every compiler). The /Od on MSVC below is retained as belt-and-suspenders for
# this context-switch-adjacent TU until a Windows sweep confirms it can be
# dropped — it is no longer load-bearing, just conservative.
if(MSVC)
    set_source_files_properties(
        src/worker_pool.cpp
        src/fiber_init.cpp
        PROPERTIES COMPILE_FLAGS "/Od /Y-"
    )
endif()

crd_enable_default_pch(crd-jobs)
