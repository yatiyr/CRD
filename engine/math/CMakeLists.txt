crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-math STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-math
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-math
    PUBLIC
        crd-core
)

if(MSVC)
    # Guarded reciprocal code in geometry helpers triggers a conservative
    # C4723 path under LTCG in Release even though the runtime branch keeps
    # the divisor non-zero. Scope the suppression to crd-math only.
    target_compile_options(crd-math PUBLIC /wd4723)
endif()

target_compile_features(crd-math PUBLIC cxx_std_20)
crd_enable_default_pch(crd-math)
