# The MSVC /Od pin of crd-jobs' context-switch-adjacent TUs, worker_pool.cpp and fiber_init.cpp (why it is load-bearing:
# the pin's comment in ../CMakeLists.txt). Given as a per-source option it followed the configuration's own optimization
# level (/O2 in CMAKE_CXX_FLAGS_RELEASE), and cl.exe reported D9025 "overriding '/O2' with '/Od'" for both TUs on every
# optimized lane. The per-configuration flags are read per directory, so this directory drops the optimization level
# from its own copies and compiles the two TUs here, once for each archive that takes their objects. The rest of the
# command line is unchanged. /Ob<n> stays as well: it still selects inline expansion under /Od, so the objects are the
# ones the pin has always produced (cl.exe had already discarded the overridden /O2).
foreach(config IN LISTS CMAKE_CONFIGURATION_TYPES CMAKE_BUILD_TYPE)
    string(TOUPPER "${config}" upper)
    string(REGEX REPLACE "(^| )[/-]O[12x]( |$)" " " CMAKE_CXX_FLAGS_${upper} "${CMAKE_CXX_FLAGS_${upper}}")
endforeach()

cmake_path(GET CMAKE_CURRENT_SOURCE_DIR PARENT_PATH jobs_source_dir)
foreach(archive IN ITEMS crd-jobs crd-jobs-schedcheck)
    set(objects ${archive}-unoptimized)
    add_library(${objects} OBJECT ${CRD_JOBS_UNOPTIMIZED_SOURCES})
    target_include_directories(${objects} PRIVATE "${jobs_source_dir}/include")
    target_link_libraries(${objects} PRIVATE crd-core crd-containers)
    target_compile_options(${objects} PRIVATE /Od)
    # The archive's LTO-off pin holds for its objects too.
    set_target_properties(${objects} PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
    if(NOT CMAKE_CONFIGURATION_TYPES)
        # The debug types go to the archive's own compile PDB, as when the TUs were its sources: it is the PDB the
        # relocatable package installs beside the archive (cmake/CrdPackage.cmake).
        get_target_property(archive_binary_dir ${archive} BINARY_DIR)
        set_target_properties(${objects} PROPERTIES
            COMPILE_PDB_NAME ${archive}
            COMPILE_PDB_OUTPUT_DIRECTORY "${archive_binary_dir}/CMakeFiles/${archive}.dir"
        )
    endif()
endforeach()
target_compile_definitions(crd-jobs-unoptimized PRIVATE CRD_JOBS_SCHED_CHECK=$<BOOL:${CRD_JOBS_SCHED_CHECK}>)
target_compile_definitions(crd-jobs-schedcheck-unoptimized PRIVATE CRD_JOBS_SCHED_CHECK=1)
set_target_properties(crd-jobs-schedcheck-unoptimized PROPERTIES EXCLUDE_FROM_ALL ON)
