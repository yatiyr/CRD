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

# The worker-sleep Semaphore (src/semaphore.cpp) calls WaitOnAddress /
# WakeByAddress{Single,All} directly on Windows; they live in synchronization.lib
# (kernel32 API set), which is not linked by default.
if(WIN32)
    target_link_libraries(crd-jobs PRIVATE synchronization)
endif()

target_compile_features(crd-jobs PUBLIC cxx_std_20)

# Hand-rolled asm context switch is incompatible with MSVC LTO (/GL+/LTCG):
# the linker can rewrite call sequences across TU boundaries in ways that break
# the fiber trampoline's assumptions about register state on entry.
set_target_properties(crd-jobs PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

# worker_pool.cpp + fiber_init.cpp are the context-switch-adjacent TUs: they run
# code on the far side of the hand-rolled fiber_switch, where a fiber can resume
# on a *different* OS thread than it suspended on. The MSVC optimizer at /O2
# caches the thread-local segment base across the asm switch (which it treats as
# an ordinary call), so a post-resume TLS read returns the *pre-resume* thread's
# slot → wild pointer → SEGFAULT in essentially every job. The portable source
# guard (CRD_JOBS_TLS_OPAQUE accessors in worker_pool.cpp — `noipa` on GCC,
# `__declspec(noinline)` elsewhere) is necessary but, on the MSVC toolset GitHub
# `windows-latest` ships (VS 2022 / older optimizer), NOT sufficient: dropping
# the /Od pin in May 2026 turned every crd-jobs test SEGFAULT in win-release and
# win-shipping on CI even though a local VS 2026 sweep was clean. So the pin is
# back, and load-bearing again — do not remove it without a clean CI run on the
# *exact* runner toolset, not just the dev box. (INTERPROCEDURAL_OPTIMIZATION OFF
# above is separate — the asm context switch is also incompatible with LTO.)
if(MSVC)
    set_source_files_properties(
        src/worker_pool.cpp
        src/fiber_init.cpp
        PROPERTIES COMPILE_FLAGS "/Od /Y-"
    )
endif()

crd_enable_default_pch(crd-jobs)
