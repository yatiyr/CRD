include(CTest)
include(Catch)

add_subdirectory(core)
add_subdirectory(log)
add_subdirectory(memory)
add_subdirectory(math)
add_subdirectory(containers)

if(CRD_BUILD_BENCHMARKS)
    add_subdirectory(bench)
endif()
