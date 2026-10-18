# Relocatable package export (REPO.DEV.8, widened by DIAG.11a). Contract: docs/design/public-consumption.md.
# crd_package_library() registers the install rules of one module library under the Cerid export set,
# crd_package_assets() installs authored assets a packaged library reads at run time, and crd_package_config() writes
# the package files (CeridConfig.cmake, its version file, the exported targets).
# Install rules add no build statement, so the build graph is unchanged; `cmake --install` produces the prefix.
# One installed prefix is one profile: the generated <crd/core/build_config.hpp> belongs to the configuration that
# installed it, and CeridConfig.cmake records that profile and refuses a consumer built for another one.
include_guard(GLOBAL)

set(CRD_PACKAGE_NAME Cerid)
set(CRD_PACKAGE_EXPORT CeridTargets)
set(CRD_PACKAGE_CMAKE_DIR lib/cmake/Cerid)
set(CRD_PACKAGE_ASSET_DIR share/cerid)

# How the packaged archives carry their debug information, decided once from the configuration (DIAG.11a: symbols are
# packaged intentionally, never by accident):
#   pdb       MSVC with /Zi: each archive's debug types live in its compile PDB, which does not travel with the .lib,
#             so the PDB is installed next to the archive (a consumer's /DEBUG link otherwise warns LNK4099);
#   embedded  CodeView inside the objects (clang-cl, or MSVC under the compiler launcher's /Z7, shipping included);
#   dwarf     DWARF inside the archive members (GCC and Clang with -g);
#   none      the configuration emits no debug information (Release without the shipping condition).
# Multi-configuration generators keep the compile PDB per configuration; their install is a declared gap
# (docs/design/public-consumption.md), so symbols there are reported as not installed rather than guessed.
function(_crd_package_debug_information out_var)
    set(debug_types Debug RelWithDebInfo)
    if(CMAKE_CONFIGURATION_TYPES)
        set(${out_var} "not-installed" PARENT_SCOPE)
        return()
    endif()
    set(with_symbols OFF)
    if(CMAKE_BUILD_TYPE IN_LIST debug_types OR CRD_SHIPPING)
        set(with_symbols ON)
    endif()
    if(NOT with_symbols)
        set(${out_var} "none" PARENT_SCOPE)
    elseif(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # The launcher switches Debug, RelWithDebInfo and Shipping to /Z7 (CRD_EMBEDDED_DEBUG_INFO,
        # cmake/CrdBuildPerformance.cmake); without it every configuration keeps a compile PDB.
        if(CRD_EMBEDDED_DEBUG_INFO)
            set(${out_var} "embedded" PARENT_SCOPE)
        else()
            set(${out_var} "pdb" PARENT_SCOPE)
        endif()
    elseif(MSVC)
        set(${out_var} "embedded" PARENT_SCOPE)
    else()
        set(${out_var} "dwarf" PARENT_SCOPE)
    endif()
endfunction()

# crd_package_library(<target> NAME <exported name> [INCLUDE <public include dir>]
#                     [GENERATED <file> DESTINATION <dir>] [INCLUDE_ONLY <module>...])
# Exports <target> as Cerid::<name>, installs the public headers (templates *.in stay out) and, when given, the
# generated header under include/<dir>. INCLUDE may be left out only for an interface library with no headers (a
# flags target). INCLUDE_ONLY names packaged libraries whose headers this one's public headers include without a link
# edge (units includes the header-only crd/math transcendental functions): installing without them is refused.
# The target's own usage requirements must be export-clean: source and build paths wrapped in $<BUILD_INTERFACE:...>,
# engine-only interface targets such as crd-warnings likewise. Under the `pdb` debug-information form the archive's
# compile PDB is installed beside it.
function(crd_package_library target)
    cmake_parse_arguments(ARG "" "NAME;INCLUDE;GENERATED;DESTINATION" "INCLUDE_ONLY" ${ARGN})
    if(NOT ARG_NAME)
        message(FATAL_ERROR "crd_package_library(${target}) needs NAME <exported name>")
    endif()
    get_target_property(type "${target}" TYPE)
    if(NOT ARG_INCLUDE AND NOT type STREQUAL "INTERFACE_LIBRARY")
        message(FATAL_ERROR "crd_package_library(${target}) needs INCLUDE <directory>")
    endif()
    if(ARG_GENERATED AND NOT ARG_DESTINATION)
        message(FATAL_ERROR "crd_package_library(${target}) GENERATED needs DESTINATION <directory under include/>")
    endif()
    set_target_properties("${target}" PROPERTIES EXPORT_NAME "${ARG_NAME}")
    install(TARGETS "${target}" EXPORT ${CRD_PACKAGE_EXPORT}
            ARCHIVE DESTINATION lib
            LIBRARY DESTINATION lib
            RUNTIME DESTINATION bin
            INCLUDES DESTINATION include)
    if(ARG_INCLUDE)
        install(DIRECTORY "${ARG_INCLUDE}/" DESTINATION include PATTERN "*.in" EXCLUDE)
    endif()
    if(ARG_GENERATED)
        install(FILES "${ARG_GENERATED}" DESTINATION "include/${ARG_DESTINATION}")
    endif()
    if(type STREQUAL "STATIC_LIBRARY")
        _crd_package_debug_information(debug_information)
        if(debug_information STREQUAL "pdb")
            # The compile PDB of a static library under Ninja and Makefiles (CMake's default /Fd for the target).
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${target}.dir/${target}.pdb" DESTINATION lib)
        endif()
    endif()
    foreach(partner IN LISTS ARG_INCLUDE_ONLY)
        set_property(GLOBAL APPEND PROPERTY CRD_PACKAGE_INCLUDE_ONLY "${target}:${partner}")
    endforeach()
    set_property(GLOBAL APPEND PROPERTY CRD_PACKAGE_LIBRARIES "${target}")
endfunction()

# crd_package_assets(<source dir> DESTINATION <dir under share/cerid>)
# Installs authored assets a packaged library reads at run time (DIAG.11a: the diagnostic profiles, policies and
# fault plans), so a consumer finds them through Cerid_ASSET_DIR instead of the engine's source tree.
function(crd_package_assets source)
    cmake_parse_arguments(ARG "" "DESTINATION" "" ${ARGN})
    if(NOT ARG_DESTINATION OR NOT IS_DIRECTORY "${source}")
        message(FATAL_ERROR "crd_package_assets(${source}) needs an existing directory and DESTINATION <dir>")
    endif()
    install(DIRECTORY "${source}/" DESTINATION "${CRD_PACKAGE_ASSET_DIR}/${ARG_DESTINATION}")
    set_property(GLOBAL APPEND PROPERTY CRD_PACKAGE_ASSETS "${ARG_DESTINATION}")
endfunction()

# Writes CeridConfig.cmake (profile record, mismatch refusal, the exported targets) and the version file, and
# installs them with the export set. Call once from the root after the modules are added.
function(crd_package_config)
    get_property(libraries GLOBAL PROPERTY CRD_PACKAGE_LIBRARIES)
    if(NOT libraries)
        return()
    endif()
    include(CMakePackageConfigHelpers)
    # A library whose public headers include another packaged library's headers without a link edge cannot be
    # installed without it: the install fails, naming both, rather than writing a package whose headers do not compile.
    get_property(include_only GLOBAL PROPERTY CRD_PACKAGE_INCLUDE_ONLY)
    foreach(pair IN LISTS include_only)
        string(REPLACE ":" ";" pair "${pair}")
        list(GET pair 0 owner)
        list(GET pair 1 partner)
        if(NOT "crd-${partner}" IN_LIST libraries)
            string(CONCAT refusal "[crd] the public headers of ${owner} include the headers of crd-${partner}, which "
                                  "this configuration does not package; configure ${partner} too (CRD_MODULES)")
            install(CODE "message(FATAL_ERROR \"${refusal}\")")
        endif()
    endforeach()
    foreach(setting ASSERTS PROFILING)
        if(CRD_ENABLE_${setting})
            set(CRD_PACKAGE_${setting} ON)
        else()
            set(CRD_PACKAGE_${setting} OFF)
        endif()
    endforeach()
    if(CRD_SHIPPING)
        set(CRD_PACKAGE_SHIPPING ON)
    else()
        set(CRD_PACKAGE_SHIPPING OFF)
    endif()
    if(CMAKE_BUILD_TYPE)
        string(CONCAT CRD_PACKAGE_PROFILE "${CMAKE_BUILD_TYPE}, asserts ${CRD_PACKAGE_ASSERTS}, profiling "
                                          "${CRD_PACKAGE_PROFILING}, shipping ${CRD_PACKAGE_SHIPPING}, "
                                          "log level ${CRD_LOG_MIN_LEVEL_NUM}")
    else()
        set(CRD_PACKAGE_PROFILE "multi-configuration build (the configuration that ran the install)")
    endif()
    _crd_package_debug_information(CRD_PACKAGE_DEBUG_INFORMATION)
    get_property(assets GLOBAL PROPERTY CRD_PACKAGE_ASSETS)
    if(assets)
        set(CRD_PACKAGE_HAS_ASSETS ON)
    else()
        set(CRD_PACKAGE_HAS_ASSETS OFF)
    endif()
    list(JOIN libraries " " CRD_PACKAGE_LIBRARY_LIST)
    list(JOIN assets " " CRD_PACKAGE_ASSET_LIST)
    set(package_dir "${CMAKE_BINARY_DIR}/package")
    configure_package_config_file("${CMAKE_SOURCE_DIR}/cmake/CeridConfig.cmake.in"
                                  "${package_dir}/${CRD_PACKAGE_NAME}Config.cmake"
                                  INSTALL_DESTINATION "${CRD_PACKAGE_CMAKE_DIR}"
                                  PATH_VARS CRD_PACKAGE_ASSET_DIR)
    write_basic_package_version_file("${package_dir}/${CRD_PACKAGE_NAME}ConfigVersion.cmake"
                                     VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
    install(EXPORT ${CRD_PACKAGE_EXPORT} NAMESPACE "${CRD_PACKAGE_NAME}::" DESTINATION "${CRD_PACKAGE_CMAKE_DIR}")
    install(FILES "${package_dir}/${CRD_PACKAGE_NAME}Config.cmake" "${package_dir}/${CRD_PACKAGE_NAME}ConfigVersion.cmake"
            DESTINATION "${CRD_PACKAGE_CMAKE_DIR}")
endfunction()
