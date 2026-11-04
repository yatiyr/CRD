# crd-platform -- the process's boundary with its OS (ADR-0006): files and durable saves, owner-only directories and
# file locks, file watching, dynamic libraries, threads, the program's arguments and the input snapshot types. It names
# no window backend: the GLFW context and window are crd-platform-window (engine/foundation/platform-window), so this
# module joins the relocatable package (OWN.1: crd-perf's bundle publish goes through its durable files). It links no
# job system either: the asynchronous reads are crd-platform-async (engine/foundation/platform-async, 2026-11-04).

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-platform STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-platform
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-platform
    PUBLIC
        crd-core
        crd-log
        crd-containers
        crd-memory
        crd-time   # crd::platform::Timer / FrameClock are aliases into crd::time::* (Detour D-006)
    PRIVATE
        ${CMAKE_DL_LIBS} # dlopen (DynamicLibrary) before glibc 2.34; GLFW used to bring it
)

if(WIN32)
    # advapi32: the owner-only DACL of create_private_directory (the process token's user, SetNamedSecurityInfoW).
    target_link_libraries(crd-platform PRIVATE advapi32)
endif()

target_compile_features(crd-platform PUBLIC cxx_std_20)
crd_enable_default_pch(crd-platform)

crd_package_library(crd-platform NAME platform INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
