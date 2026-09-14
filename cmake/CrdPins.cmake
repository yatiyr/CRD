# CrdPins.cmake — the single registry of external inputs (cmake/pins.json, schema cerid-pins/1).
#
# Every third-party source, SDK, tool and workflow action the build or CI acquires is named there with its
# version, URL and SHA-256 (docs/design/pinned-inputs.md). `crd_add_pinned_package()` wraps CPMAddPackage with a
# commit-addressed archive and its hash, so CMake verifies the bytes before anything is extracted or compiled;
# `CRD_INPUT_ARCHIVES` names a directory of verified local archives for offline setup (the same hash applies).
# `crd_patched_copy()` applies a versioned replacement spec (cmake/patches/<name>.cmake) to a build-owned copy of
# a downloaded file; the shared source cache is never written, and an anchor that no longer matches upstream is a
# configure error rather than a silent no-op.
include_guard(GLOBAL)

set(CRD_PINS_FILE "${CMAKE_CURRENT_LIST_DIR}/pins.json")
set(CRD_PATCH_DIR "${CMAKE_CURRENT_LIST_DIR}/patches")
set(CRD_INPUT_ARCHIVES "" CACHE PATH
    "Directory of verified local archives; a file named like a pin's `file` entry is used instead of its URL")
file(READ "${CRD_PINS_FILE}" _crd_pins_json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CRD_PINS_FILE}")
string(JSON _crd_pins_schema ERROR_VARIABLE _crd_pins_error GET "${_crd_pins_json}" schema)
if(NOT _crd_pins_schema STREQUAL "cerid-pins/1")
    message(FATAL_ERROR "cmake/pins.json: expected schema cerid-pins/1, got '${_crd_pins_schema}' ${_crd_pins_error}")
endif()

# crd_pin(<out> <section> <name> <field>): one field of one pinned entry; a missing field is a configure error.
function(crd_pin out section name field)
    string(JSON value ERROR_VARIABLE error GET "${_crd_pins_json}" "${section}" "${name}" "${field}")
    if(error)
        message(FATAL_ERROR "cmake/pins.json: ${section}/${name}/${field}: ${error}")
    endif()
    set("${out}" "${value}" PARENT_SCOPE)
endfunction()

# crd_pin_optional(<out> <section> <name> <field> <default>)
function(crd_pin_optional out section name field default)
    string(JSON value ERROR_VARIABLE error GET "${_crd_pins_json}" "${section}" "${name}" "${field}")
    if(error)
        set(value "${default}")
    endif()
    set("${out}" "${value}" PARENT_SCOPE)
endfunction()

# crd_pinned_source(<out> <section> <name>): the pinned URL, or the verified local archive when CRD_INPUT_ARCHIVES
# holds a file with the pin's `file` name. The caller still passes the pinned hash, so both paths are verified.
function(crd_pinned_source out section name)
    crd_pin(url "${section}" "${name}" url)
    crd_pin_optional(file "${section}" "${name}" file "")
    if(file STREQUAL "")
        get_filename_component(file "${url}" NAME)
    endif()
    if(NOT CRD_INPUT_ARCHIVES STREQUAL "" AND EXISTS "${CRD_INPUT_ARCHIVES}/${file}")
        get_filename_component(local "${CRD_INPUT_ARCHIVES}/${file}" ABSOLUTE)
        message(STATUS "[crd] ${name}: using local archive ${local}")
        set("${out}" "${local}" PARENT_SCOPE)
    else()
        set("${out}" "${url}" PARENT_SCOPE)
    endif()
endfunction()

# crd_add_pinned_package(<name> [CPMAddPackage arguments...]): CPMAddPackage from the pinned archive and hash.
# A macro so the package variables (<name>_SOURCE_DIR, <name>_ADDED) land in the caller's scope as they always did.
macro(crd_add_pinned_package name)
    crd_pinned_source(_crd_pkg_source packages "${name}")
    crd_pin(_crd_pkg_hash packages "${name}" sha256)
    crd_pin(_crd_pkg_version packages "${name}" version)
    CPMAddPackage(NAME "${name}" VERSION "${_crd_pkg_version}" URL "${_crd_pkg_source}"
                  URL_HASH "SHA256=${_crd_pkg_hash}" ${ARGN})
    unset(_crd_pkg_source)
    unset(_crd_pkg_hash)
    unset(_crd_pkg_version)
endmacro()

# crd_patch_replace(<from> <to>): one replacement step of a patch spec; called by the included spec file.
function(crd_patch_replace from to)
    string(FIND "${_crd_patch_text}" "${from}" index)
    if(index EQUAL -1)
        message(FATAL_ERROR "Patch ${_crd_patch_name} no longer applies to ${_crd_patch_source}: anchor not found:\n${from}")
    endif()
    string(REPLACE "${from}" "${to}" text "${_crd_patch_text}")
    set(_crd_patch_text "${text}" PARENT_SCOPE)
    math(EXPR _crd_patch_steps "${_crd_patch_steps} + 1")
    set(_crd_patch_steps "${_crd_patch_steps}" PARENT_SCOPE)
endfunction()

# crd_patched_copy(<out_var> <package> <relative-file> <patch-name>): apply cmake/patches/<patch-name>.cmake to
# <package>_SOURCE_DIR/<relative-file> and write the result under ${CMAKE_BINARY_DIR}/patched/<package>/ (only
# when the content changes, so an unchanged patch does not rebuild its object). The downloaded tree is read only.
function(crd_patched_copy out_var package relative patch)
    set(source "${${package}_SOURCE_DIR}/${relative}")
    set(spec "${CRD_PATCH_DIR}/${patch}.cmake")
    if(NOT EXISTS "${source}")
        message(FATAL_ERROR "Patch ${patch}: ${source} does not exist")
    endif()
    if(NOT EXISTS "${spec}")
        message(FATAL_ERROR "Patch ${patch}: ${spec} does not exist")
    endif()
    file(READ "${source}" _crd_patch_text)
    set(_crd_patch_name "${patch}")
    set(_crd_patch_source "${source}")
    set(_crd_patch_steps 0)
    include("${spec}")
    if(_crd_patch_steps EQUAL 0)
        message(FATAL_ERROR "Patch ${patch}: ${spec} declares no replacement")
    endif()
    set(output "${CMAKE_BINARY_DIR}/patched/${package}/${relative}")
    set(current "")
    if(EXISTS "${output}")
        file(READ "${output}" current)
    endif()
    if(NOT current STREQUAL _crd_patch_text)
        get_filename_component(output_dir "${output}" DIRECTORY)
        file(MAKE_DIRECTORY "${output_dir}")
        file(WRITE "${output}" "${_crd_patch_text}")
        message(STATUS "[crd] Patched ${package}/${relative} with ${patch} (${_crd_patch_steps} replacements) into the build tree")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${spec}" "${source}")
    set("${out_var}" "${output}" PARENT_SCOPE)
endfunction()
