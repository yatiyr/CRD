# Opt-in compiler caching, an opt-in linker and memory-bounded Ninja job pools (REPO.DEV.7). Contract:
# docs/design/build-performance.md. Nothing here changes a configuration unless CRD_COMPILER_LAUNCHER, CRD_LINKER
# (with CRD_LINKER_THREADS), CRD_COMPILE_JOBS or CRD_LINK_JOBS is set; the default graph stays byte-identical.
# Generator truth: CMake applies <LANG>_COMPILER_LAUNCHER only for Makefile and Ninja generators and JOB_POOLS only for
# Ninja, so a Visual Studio solution is reported as uncached and unbounded rather than silently pretending otherwise.
# The local helpers (scripts/configure-preset.bat, scripts/wsl-lane.sh) turn the launcher and the linker on; the
# presets and the default graph never do.
include_guard(GLOBAL)

set(CRD_COMPILER_LAUNCHER "" CACHE FILEPATH
    "Compiler launcher (the pinned sccache) for Ninja/Makefile generators; empty keeps the uncached compile")
set(CRD_LINKER "" CACHE FILEPATH
    "The pinned mold for GCC and Clang links (scripts/install-mold.py); empty keeps the toolchain's default linker")
set(CRD_LINKER_THREADS "" CACHE STRING
    "Threads per mold link (-Wl,--threads=<n>); empty keeps mold's default of one thread per CPU")
set(CRD_COMPILE_JOBS "" CACHE STRING "Ninja compile pool depth (memory-bounded); empty keeps Ninja's -j for compiles")
set(CRD_LINK_JOBS "" CACHE STRING "Ninja link pool depth (memory-bounded); empty keeps Ninja's -j for links")

if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(_crd_launcher_generator ON)
else()
    set(_crd_launcher_generator OFF)
endif()

# CRD_EMBEDDED_DEBUG_INFO is the one place that says whether MSVC-compatible compiles embed their debug information
# in the objects (/Z7) instead of a per-target compile PDB (/Zi): the root build file picks the shipping flag from it
# and cmake/CrdPackage.cmake the packaged form. It is ON only under the compiler launcher.
set(CRD_EMBEDDED_DEBUG_INFO OFF)
if(CRD_COMPILER_LAUNCHER)
    if(NOT _crd_launcher_generator)
        message(STATUS "[crd] Compiler launcher ignored: ${CMAKE_GENERATOR} runs uncached "
                       "(CMake applies compiler launchers to Makefile and Ninja generators only)")
    else()
        set(CMAKE_C_COMPILER_LAUNCHER "${CRD_COMPILER_LAUNCHER}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CRD_COMPILER_LAUNCHER}")
        if(MSVC)
            # sccache refuses a compile whose debug information goes to a per-target PDB (/Zi, "shared pdb"); the
            # documented cacheable form embeds it in the object (/Z7). Policy CMP0141 is NEW under 3.25, so the
            # generator expression replaces CMake's default ProgramDatabase for the configurations that carry symbols;
            # Shipping (a Release build type) takes its explicit flag from CRD_EMBEDDED_DEBUG_INFO and still links a
            # full PDB (/DEBUG:FULL). clang-cl writes the same objects for /Zi and /Z7 (CodeView is always embedded).
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
            set(CRD_EMBEDDED_DEBUG_INFO ON)
        endif()
        if(CRD_ENABLE_PCH)
            # MSVC's precompiled header (/Yu /Fp) is not cacheable. A GCC or Clang one is cached by its preprocessed
            # text, but it records its inputs' bytes (GCC: each #pragma once header's size and MD5), so an edit that
            # keeps the text (a comment, line endings) is served a header whose consumers re-include those headers.
            message(FATAL_ERROR "[crd] Compiler launcher ${CRD_COMPILER_LAUNCHER} needs -DCRD_ENABLE_PCH=OFF: a "
                                "cached tree has no precompiled header (docs/design/build-performance.md)")
        endif()
        set(_crd_launcher_note "")
        if(MSVC)
            set(_crd_launcher_note "; debug information embedded")
        endif()
        message(STATUS "[crd] Compiler launcher: ${CRD_COMPILER_LAUNCHER} (${CMAKE_GENERATOR}${_crd_launcher_note})")
        unset(_crd_launcher_note)
    endif()
endif()

# The linker touches link lines only: no compile command changes, so a cached tree keeps every cache key. GCC takes no
# path in -fuse-ld, so the driver finds `ld.mold` (the installer's link beside the binary) in the -B directory; Clang
# looks it up the same way. Gated on the link language like the warning options of the root build file. CMake 3.29's
# CMAKE_LINKER_TYPE does the same, but the WSL reference host runs CMake 3.28.
if(CRD_LINKER)
    if(MSVC)
        message(STATUS "[crd] Linker override ignored: ${CMAKE_CXX_COMPILER_ID} links through link.exe or lld-link "
                       "(CRD_LINKER is for GCC and Clang)")
    else()
        get_filename_component(_crd_linker_name "${CRD_LINKER}" NAME)
        get_filename_component(_crd_linker_dir "${CRD_LINKER}" DIRECTORY)
        if(NOT _crd_linker_name STREQUAL "mold" OR NOT EXISTS "${CRD_LINKER}")
            message(FATAL_ERROR "CRD_LINKER must name an installed mold binary (scripts/install-mold.py), got "
                                "'${CRD_LINKER}'")
        endif()
        if(NOT EXISTS "${_crd_linker_dir}/ld.mold")
            message(FATAL_ERROR "CRD_LINKER: ${_crd_linker_dir} has no ld.mold, which -fuse-ld=mold looks for "
                                "(scripts/install-mold.py creates it)")
        endif()
        add_link_options("$<$<LINK_LANGUAGE:C,CXX>:-fuse-ld=mold>" "$<$<LINK_LANGUAGE:C,CXX>:-B${_crd_linker_dir}>")
        # mold starts a pool of one thread per CPU for every link, so the 16 links of a parallel build ran 512 threads
        # on 32 CPUs and gained nothing over GNU ld; a bounded pool (lane jobs times threads at most the CPUs) is what
        # made it faster (docs/design/build-performance.md#the-linker). The option names mold, so it is only ever on
        # a mold link line; CMake's compiler probes link with the toolchain's linker and never see it.
        set(_crd_linker_threads "")
        # Tested as text: CMake reads "0" as false, and an explicit zero is a mistake, not "no bound".
        if(NOT CRD_LINKER_THREADS STREQUAL "")
            if(NOT CRD_LINKER_THREADS MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "CRD_LINKER_THREADS must be a positive integer, got '${CRD_LINKER_THREADS}'")
            endif()
            add_link_options("$<$<LINK_LANGUAGE:C,CXX>:-Wl,--threads=${CRD_LINKER_THREADS}>")
            set(_crd_linker_threads ", ${CRD_LINKER_THREADS} threads per link")
        endif()
        message(STATUS "[crd] Linker: ${CRD_LINKER} (GCC and Clang links${_crd_linker_threads})")
        unset(_crd_linker_threads)
        unset(_crd_linker_name)
        unset(_crd_linker_dir)
    endif()
elseif(NOT CRD_LINKER_THREADS STREQUAL "")
    message(STATUS "[crd] CRD_LINKER_THREADS ignored: it bounds mold's threads and CRD_LINKER is empty")
endif()

set(_crd_pools)
if(CRD_COMPILE_JOBS)
    if(NOT CRD_COMPILE_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "CRD_COMPILE_JOBS must be a positive integer, got '${CRD_COMPILE_JOBS}'")
    endif()
    list(APPEND _crd_pools "crd_compile=${CRD_COMPILE_JOBS}")
    set(CMAKE_JOB_POOL_COMPILE crd_compile)
endif()
if(CRD_LINK_JOBS)
    if(NOT CRD_LINK_JOBS MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "CRD_LINK_JOBS must be a positive integer, got '${CRD_LINK_JOBS}'")
    endif()
    list(APPEND _crd_pools "crd_link=${CRD_LINK_JOBS}")
    set(CMAKE_JOB_POOL_LINK crd_link)
endif()
if(_crd_pools)
    if(CMAKE_GENERATOR MATCHES "Ninja")
        set_property(GLOBAL APPEND PROPERTY JOB_POOLS ${_crd_pools})
        message(STATUS "[crd] Ninja job pools: ${_crd_pools}")
    else()
        unset(CMAKE_JOB_POOL_COMPILE)
        unset(CMAKE_JOB_POOL_LINK)
        message(STATUS "[crd] Job pools ignored: ${CMAKE_GENERATOR} has no pools (CRD_COMPILE_JOBS/CRD_LINK_JOBS are Ninja-only)")
    endif()
endif()
unset(_crd_pools)
unset(_crd_launcher_generator)
