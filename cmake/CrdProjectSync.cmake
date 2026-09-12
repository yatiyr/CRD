# Durable source-membership projection on every backend; native edit capture is opt-in.
include_guard(GLOBAL)
find_package(Python3 3.12 REQUIRED COMPONENTS Interpreter)
option(CRD_PROJECT_SYNC "Synchronize saved native Visual Studio structure edits" OFF)
set(_crd_sync_args)
if(CRD_PROJECT_SYNC AND CMAKE_GENERATOR MATCHES "^Visual Studio")
    list(APPEND _crd_sync_args --native)
    cmake_file_api(QUERY API_VERSION 1 CODEMODEL 2 CMAKEFILES 1)
endif()
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/scripts/project-sync.py"
            --root "${CMAKE_SOURCE_DIR}" preconfigure --build "${CMAKE_BINARY_DIR}" ${_crd_sync_args}
    RESULT_VARIABLE _crd_sync_result
    ERROR_VARIABLE _crd_sync_error
)
if(NOT _crd_sync_result EQUAL 0)
    message(FATAL_ERROR "Project structure synchronization: ${_crd_sync_error}")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/cmake/project-structure.json")
include("${CMAKE_BINARY_DIR}/cerid-project-sync/structure.cmake")
