# A thin external application of an installed Cerid package (DIAG.11a; see main.cpp and the example_*.cpp files). It
# is configured only against an installed prefix (CMAKE_PREFIX_PATH), never added to the engine's build:
# scripts/test-package-consumer.py copies it next to the moved prefix, builds it and judges what it prints.
cmake_minimum_required(VERSION 3.25)
project(cerid_diag_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Cerid CONFIG REQUIRED)
add_executable(diag-consumer main.cpp consumer_support.cpp example_allocator.cpp example_jobs.cpp example_reload.cpp
    example_capture.cpp)
# The diagnostic host (perf), and the modules the examples name directly: allocators (memory), the pool (jobs) and the
# application's own authored data (toml).
target_link_libraries(diag-consumer PRIVATE Cerid::perf Cerid::memory Cerid::jobs Cerid::toml)
# Symbols in every configuration, as a shipped application keeps them for its crash reports: the link reads the
# engine archives' debug information, so a package whose symbols do not travel is caught here (MSVC: LNK4099).
if(MSVC)
    set_property(TARGET diag-consumer PROPERTY MSVC_DEBUG_INFORMATION_FORMAT ProgramDatabase)
    target_link_options(diag-consumer PRIVATE /DEBUG:FULL)
else()
    target_compile_options(diag-consumer PRIVATE -g)
endif()
message(STATUS "Cerid ${Cerid_VERSION} profile: ${Cerid_PROFILE}; libraries: ${Cerid_LIBRARIES}")
message(STATUS "Cerid debug information: ${Cerid_DEBUG_INFORMATION}")
message(STATUS "Cerid asset directory: ${Cerid_ASSET_DIR}")
