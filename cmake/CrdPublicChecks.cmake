# Opt-in public-surface checks (REPO.DEV.8). Contract: docs/design/public-consumption.md.
# CRD_PUBLIC_CHECKS=ON compiles every public header of every selected module as its own translation unit through the
# module's consumer view (the PUBLIC/INTERFACE usage requirements of the module's library targets: no private include
# directory, no precompiled header, no earlier include that happened to provide a declaration) and registers the
# relocatable package consumer test. OFF, the default, adds nothing: the graph is byte-identical.
include_guard(GLOBAL)

option(CRD_PUBLIC_CHECKS
       "Compile every public header standalone through its module's consumer view and run the package consumer test"
       OFF)

# A documented fragment that is not a self-contained header (an X-macro table, a platform-only header without its own
# guard) is excluded by one call carrying the reason. A stale exclusion (unknown module, header no longer there)
# fails the configure, so the list cannot outlive the headers it names.
function(crd_public_header_exclude module header)
    cmake_parse_arguments(ARG "" "REASON" "" ${ARGN})
    if(NOT ARG_REASON)
        message(FATAL_ERROR "crd_public_header_exclude(${module} ${header}) needs REASON <text>")
    endif()
    if(ARG_REASON MATCHES ";")
        message(FATAL_ERROR "crd_public_header_exclude(${module} ${header}): the reason cannot contain ';'")
    endif()
    set_property(GLOBAL APPEND PROPERTY CRD_PUBLIC_HEADER_EXCLUSION_KEYS "${module}|${header}")
    set_property(GLOBAL APPEND PROPERTY CRD_PUBLIC_HEADER_EXCLUSION_REASONS "${ARG_REASON}")
endfunction()

# The non-imported library targets a module directory builds: the consumer view of the module is their PUBLIC and
# INTERFACE usage requirements, which is what linking them PRIVATE from another target receives.
function(_crd_public_library_targets directory out_var)
    _crd_collect_build_targets("${directory}" targets)
    set(libraries "")
    foreach(target IN LISTS targets)
        get_target_property(type "${target}" TYPE)
        get_target_property(imported "${target}" IMPORTED)
        if(NOT imported AND type MATCHES "^(STATIC|SHARED|OBJECT|INTERFACE)_LIBRARY$")
            list(APPEND libraries "${target}")
        endif()
    endforeach()
    set(${out_var} "${libraries}" PARENT_SCOPE)
endfunction()

# One shim translation unit per public header (include/**/*.hpp and *.h; *.in and *.inc are not headers), collected
# into an OBJECT library per module that links only the module's own library targets. The shims live under the build
# tree and are rewritten only when their content changes. Call after crd_add_modules().
function(crd_add_public_header_checks)
    if(NOT CRD_PUBLIC_CHECKS)
        return()
    endif()
    get_property(names GLOBAL PROPERTY CRD_MODULE_NAMES)
    get_property(selected GLOBAL PROPERTY CRD_SELECTED_MODULES)
    get_property(exclusion_keys GLOBAL PROPERTY CRD_PUBLIC_HEADER_EXCLUSION_KEYS)
    get_property(exclusion_reasons GLOBAL PROPERTY CRD_PUBLIC_HEADER_EXCLUSION_REASONS)
    foreach(key IN LISTS exclusion_keys)
        string(REGEX MATCH "^([^|]+)\\|(.+)$" _ "${key}")
        set(module "${CMAKE_MATCH_1}")
        set(header "${CMAKE_MATCH_2}")
        if(NOT module IN_LIST names)
            message(FATAL_ERROR "crd_public_header_exclude(${module} ${header}): ${module} is not a registered module")
        endif()
        get_property(source GLOBAL PROPERTY CRD_MODULE_${module}_SOURCE)
        if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${source}/include/${header}")
            message(FATAL_ERROR "crd_public_header_exclude(${module} ${header}): ${source}/include/${header} does not "
                                "exist; remove the stale exclusion")
        endif()
    endforeach()

    set(checked 0)
    set(excluded 0)
    set(modules 0)
    set(checks "")
    set(unchecked "")
    foreach(name IN LISTS names)
        if(NOT name IN_LIST selected)
            continue()
        endif()
        get_property(source GLOBAL PROPERTY CRD_MODULE_${name}_SOURCE)
        set(include_dir "${CMAKE_SOURCE_DIR}/${source}/include")
        if(NOT IS_DIRECTORY "${include_dir}")
            continue()
        endif()
        file(GLOB_RECURSE headers RELATIVE "${include_dir}" CONFIGURE_DEPENDS "${include_dir}/*.hpp" "${include_dir}/*.h")
        if(NOT headers)
            continue()
        endif()
        list(SORT headers)
        _crd_public_library_targets("${CMAKE_SOURCE_DIR}/${source}" libraries)
        if(NOT libraries)
            list(APPEND unchecked "${name}")
            continue()
        endif()
        set(shims "")
        foreach(header IN LISTS headers)
            list(FIND exclusion_keys "${name}|${header}" position)
            if(position GREATER -1)
                math(EXPR excluded "${excluded} + 1")
                continue()
            endif()
            set(shim "${CMAKE_BINARY_DIR}/public-checks/${name}/${header}.cpp")
            file(CONFIGURE OUTPUT "${shim}" @ONLY CONTENT
                 "// cmake/CrdPublicChecks.cmake: <${header}> compiled alone through the consumer view of module ${name}.\n#include <${header}>\n")
            list(APPEND shims "${shim}")
            math(EXPR checked "${checked} + 1")
        endforeach()
        if(NOT shims)
            continue()
        endif()
        set(target "crd-${name}-header-check")
        add_library("${target}" OBJECT ${shims})
        target_link_libraries("${target}" PRIVATE ${libraries})
        list(APPEND checks "${target}")
        math(EXPR modules "${modules} + 1")
    endforeach()
    add_custom_target(crd-header-checks)
    if(checks)
        add_dependencies(crd-header-checks ${checks})
    endif()
    set(note "")
    if(unchecked)
        list(JOIN unchecked ", " unchecked_list)
        set(note "; no library target to view through: ${unchecked_list}")
    endif()
    message(STATUS "[crd] Public header checks: ${checked} headers of ${modules} modules compiled standalone "
                   "(${excluded} excluded with a reason${note})")
endfunction()

# The relocatable package consumer test: scripts/test-package-consumer.py configures a core-only build with this
# configuration's generator, compiler, build type and profile switches, installs it, moves the prefix, builds and
# runs a downstream project against the moved prefix and checks the generated profile header and relocatability.
# Registered only under the option and with tests enabled; the lane that owns the option qualifies its own toolchain.
function(crd_add_package_consumer_test)
    if(NOT CRD_PUBLIC_CHECKS OR NOT CRD_BUILD_TESTS)
        return()
    endif()
    find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
    set(arguments
        --source "${CMAKE_SOURCE_DIR}"
        --scratch "${CMAKE_BINARY_DIR}/public-checks/package"
        --generator "${CMAKE_GENERATOR}"
        --build-type "${CMAKE_BUILD_TYPE}"
        --cxx-compiler "${CMAKE_CXX_COMPILER}")
    if(CMAKE_MAKE_PROGRAM)
        list(APPEND arguments --make-program "${CMAKE_MAKE_PROGRAM}")
    endif()
    foreach(setting CRD_ENABLE_ASSERTS CRD_ENABLE_PROFILING CRD_SHIPPING CRD_LOG_MIN_LEVEL CRD_SIMD_LEVEL
                    CRD_DETERMINISTIC_FP CRD_ENABLE_ASAN CRD_ENABLE_UBSAN)
        list(APPEND arguments --define "${setting}=${${setting}}")
    endforeach()
    add_test(NAME crd-package-consumer
        COMMAND Python3::Interpreter "${CMAKE_SOURCE_DIR}/scripts/test-package-consumer.py" ${arguments})
    set_tests_properties(crd-package-consumer PROPERTIES TIMEOUT 1800 ENVIRONMENT "PYTHONUTF8=1")
endfunction()
