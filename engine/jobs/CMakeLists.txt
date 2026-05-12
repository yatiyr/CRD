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
    PRIVATE
        crd-containers  # internal: scheduler injection queues use crd::containers::ConcurrentQueue
)

target_compile_features(crd-jobs PUBLIC cxx_std_20)

# Hand-rolled asm context switch is incompatible with MSVC LTO (/GL+/LTCG):
# the linker can rewrite call sequences across TU boundaries in ways that break
# the fiber trampoline's assumptions about register state on entry.
set_target_properties(crd-jobs PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# Historical note: worker_pool.cpp + fiber_init.cpp used to carry a `/Od /Y-`
# pin on MSVC, guarding the long-standing win-release / linux-gcc-release
# SEGFAULT in "jobs: run_and_wait from inside a worker fiber". That root cause —
# job_fiber_trampoline reading thread-locals on the far side of a fiber resume
# that can migrate the fiber to a different OS thread, with the optimizer caching
# the pre-resume TLS base across the hand-rolled fiber_switch — is now fixed in
# source (the CRD_JOBS_TLS_OPAQUE accessors in worker_pool.cpp; portable, covers
# every compiler: `noipa` on GCC, `__declspec(noinline)` elsewhere). The /Od was
# dropped 2026-05-12 after a full Windows+Linux sweep plus a Windows jobs stress
# run confirmed the source fix holds without it. INTERPROCEDURAL_OPTIMIZATION OFF
# above stays — the hand-rolled asm context switch is still incompatible with LTO.

crd_enable_default_pch(crd-jobs)
