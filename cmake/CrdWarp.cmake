# CrdWarp.cmake — stage a pinned app-local Microsoft WARP (scripts/install-warp.py) beside every test executable.
#
# Opt-in provider experiment harness; hosted lanes do not set it (docs/recipes/2026-09-13-dx12-pinned-warp.md
# records why the 1.0.20 package was withdrawn from CI). CRD_WARP_DLL (FILEPATH, default empty) names the verified
# d3d10warp.dll; empty keeps the OS WARP and adds nothing. The D3D12 runtime loads d3d10warp.dll from the application
# directory ahead of System32, so a copy beside each executable selects the pinned build for that process only.
# Nothing is installed system-wide, copies are content checked, and the staged provider identifies itself through
# the device census driver field. Windows only; other platforms ignore the variable.
set(CRD_WARP_DLL "" CACHE FILEPATH "Verified app-local Microsoft WARP DLL staged beside test executables (Windows)")

function(_crd_collect_executables directory out_var)
    set(found "${${out_var}}")
    # Executables kept out of ALL (target or directory EXCLUDE_FROM_ALL) are not staged: depending on them from an
    # ALL target would silently force them into every hosted build.
    get_property(directory_excluded DIRECTORY "${directory}" PROPERTY EXCLUDE_FROM_ALL)
    if(directory_excluded)
        set("${out_var}" "${found}" PARENT_SCOPE)
        return()
    endif()
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(type "${target}" TYPE)
        get_target_property(excluded "${target}" EXCLUDE_FROM_ALL)
        if(type STREQUAL "EXECUTABLE" AND NOT excluded)
            list(APPEND found "${target}")
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        _crd_collect_executables("${child}" found)
    endforeach()
    set("${out_var}" "${found}" PARENT_SCOPE)
endfunction()

# Call once, after every test subdirectory of `root` has been added. Creates the ALL target `crd-warp-stage`, which
# runs after every ALL executable below `root` and copies the DLL into each executable's output directory.
function(crd_stage_warp_dll root)
    if(NOT WIN32 OR CRD_WARP_DLL STREQUAL "")
        return()
    endif()
    if(NOT EXISTS "${CRD_WARP_DLL}")
        message(FATAL_ERROR "CRD_WARP_DLL does not exist: ${CRD_WARP_DLL}")
    endif()
    set(executables "")
    _crd_collect_executables("${root}" executables)
    if(executables STREQUAL "")
        message(FATAL_ERROR "crd_stage_warp_dll found no executables below ${root}")
    endif()
    set(commands "")
    foreach(target IN LISTS executables)
        list(APPEND commands COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CRD_WARP_DLL}"
             "$<TARGET_FILE_DIR:${target}>/d3d10warp.dll")
    endforeach()
    add_custom_target(crd-warp-stage ALL ${commands}
                      COMMENT "Staging pinned WARP ${CRD_WARP_DLL} beside test executables" VERBATIM)
    add_dependencies(crd-warp-stage ${executables})
    set_property(TARGET crd-warp-stage PROPERTY FOLDER "tooling")
endfunction()
