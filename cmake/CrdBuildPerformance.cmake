# Opt-in compiler caching and memory-bounded Ninja job pools (REPO.DEV.7). Contract: docs/design/build-performance.md.
# Nothing here changes a configuration unless CRD_COMPILER_LAUNCHER, CRD_COMPILE_JOBS or CRD_LINK_JOBS is set; the
# default graph stays byte-identical. Generator truth: CMake applies <LANG>_COMPILER_LAUNCHER only for Makefile and
# Ninja generators and JOB_POOLS only for Ninja, so a Visual Studio solution is reported as uncached and unbounded
# rather than silently pretending otherwise.
include_guard(GLOBAL)

set(CRD_COMPILER_LAUNCHER "" CACHE FILEPATH
    "Compiler launcher (the pinned sccache) for Ninja/Makefile generators; empty keeps the uncached compile")
set(CRD_COMPILE_JOBS "" CACHE STRING "Ninja compile pool depth (memory-bounded); empty keeps Ninja's -j for compiles")
set(CRD_LINK_JOBS "" CACHE STRING "Ninja link pool depth (memory-bounded); empty keeps Ninja's -j for links")

if(CMAKE_GENERATOR MATCHES "Ninja|Makefiles")
    set(_crd_launcher_generator ON)
else()
    set(_crd_launcher_generator OFF)
endif()

if(CRD_COMPILER_LAUNCHER)
    if(NOT _crd_launcher_generator)
        message(STATUS "[crd] Compiler launcher ignored: ${CMAKE_GENERATOR} runs uncached "
                       "(CMake applies compiler launchers to Makefile and Ninja generators only)")
    elseif(CRD_SHIPPING)
        message(STATUS "[crd] Compiler launcher ignored: Shipping keeps its explicit /Zi PDB and LTCG uncached")
    else()
        set(CMAKE_C_COMPILER_LAUNCHER "${CRD_COMPILER_LAUNCHER}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CRD_COMPILER_LAUNCHER}")
        if(MSVC)
            # sccache refuses a compile whose debug information goes to a per-target PDB (/Zi, "shared pdb"); the
            # documented cacheable form embeds it in the object (/Z7). Policy CMP0141 is NEW under 3.25, so the
            # generator expression replaces CMake's default ProgramDatabase for the configurations that carry symbols.
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
            if(CRD_ENABLE_PCH)
                message(WARNING "[crd] Compiler launcher ${CRD_COMPILER_LAUNCHER} with CRD_ENABLE_PCH=ON: every "
                                "translation unit that consumes an MSVC precompiled header (/Yu /Fp) is non-cacheable; "
                                "configure with -DCRD_ENABLE_PCH=OFF to cache (docs/design/build-performance.md)")
            endif()
        endif()
        set(_crd_launcher_note "")
        if(MSVC)
            set(_crd_launcher_note "; debug information embedded")
        endif()
        message(STATUS "[crd] Compiler launcher: ${CRD_COMPILER_LAUNCHER} (${CMAKE_GENERATOR}${_crd_launcher_note})")
        unset(_crd_launcher_note)
    endif()
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
