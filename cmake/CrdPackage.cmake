# Relocatable package export (REPO.DEV.8). Contract: docs/design/public-consumption.md.
# crd_package_library() registers the install rules of one module library under the Cerid export set and
# crd_package_config() writes the package files (CeridConfig.cmake, its version file, the exported targets).
# Install rules add no build statement, so the build graph is unchanged; `cmake --install` produces the prefix.
# One installed prefix is one profile: the generated <crd/core/build_config.hpp> belongs to the configuration that
# installed it, and CeridConfig.cmake records that profile and refuses a consumer built for another one.
include_guard(GLOBAL)

set(CRD_PACKAGE_NAME Cerid)
set(CRD_PACKAGE_EXPORT CeridTargets)
set(CRD_PACKAGE_CMAKE_DIR lib/cmake/Cerid)

# crd_package_library(<target> NAME <exported name> INCLUDE <public include dir> [GENERATED <file> DESTINATION <dir>])
# Exports <target> as Cerid::<name>, installs the public headers (templates *.in stay out) and, when given, the
# generated header under include/<dir>. The target's own usage requirements must be export-clean: source and build
# paths wrapped in $<BUILD_INTERFACE:...>, engine-only interface targets such as crd-warnings likewise.
function(crd_package_library target)
    cmake_parse_arguments(ARG "" "NAME;INCLUDE;GENERATED;DESTINATION" "" ${ARGN})
    if(NOT ARG_NAME OR NOT ARG_INCLUDE)
        message(FATAL_ERROR "crd_package_library(${target}) needs NAME <exported name> and INCLUDE <directory>")
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
    install(DIRECTORY "${ARG_INCLUDE}/" DESTINATION include PATTERN "*.in" EXCLUDE)
    if(ARG_GENERATED)
        install(FILES "${ARG_GENERATED}" DESTINATION "include/${ARG_DESTINATION}")
    endif()
    set_property(GLOBAL APPEND PROPERTY CRD_PACKAGE_LIBRARIES "${target}")
endfunction()

# Writes CeridConfig.cmake (profile record, mismatch refusal, the exported targets) and the version file, and
# installs them with the export set. Call once from the root after the modules are added.
function(crd_package_config)
    get_property(libraries GLOBAL PROPERTY CRD_PACKAGE_LIBRARIES)
    if(NOT libraries)
        return()
    endif()
    include(CMakePackageConfigHelpers)
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
    list(JOIN libraries " " CRD_PACKAGE_LIBRARY_LIST)
    set(package_dir "${CMAKE_BINARY_DIR}/package")
    configure_package_config_file("${CMAKE_SOURCE_DIR}/cmake/CeridConfig.cmake.in"
                                  "${package_dir}/${CRD_PACKAGE_NAME}Config.cmake"
                                  INSTALL_DESTINATION "${CRD_PACKAGE_CMAKE_DIR}")
    write_basic_package_version_file("${package_dir}/${CRD_PACKAGE_NAME}ConfigVersion.cmake"
                                     VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
    install(EXPORT ${CRD_PACKAGE_EXPORT} NAMESPACE "${CRD_PACKAGE_NAME}::" DESTINATION "${CRD_PACKAGE_CMAKE_DIR}")
    install(FILES "${package_dir}/${CRD_PACKAGE_NAME}Config.cmake" "${package_dir}/${CRD_PACKAGE_NAME}ConfigVersion.cmake"
            DESTINATION "${CRD_PACKAGE_CMAKE_DIR}")
endfunction()
