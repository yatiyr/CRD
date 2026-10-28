crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-math STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-math
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(crd-math
    PUBLIC
        crd-core
        crd-simd-flags
        crd-units  # v0b-1: Vec3<Quantity<>> support via widened MathValue concept (ADR-0078 §2 D3)
)

target_compile_features(crd-math PUBLIC cxx_std_20)
crd_enable_default_pch(crd-math)

# The batch small-float conversions, one body per CPU level (ADR-0134); src/float_convert_dispatch.cpp lists them.
crd_add_cpu_kernels(crd-math NAME float_convert LEVELS portable sse2 sse42 avx2 avx512
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/float_convert_kernels.cpp")

# Cerid::math of the relocatable package, with the SIMD and floating-point contract flags its consumers must compile
# with (Cerid::simd-flags: the baseline ISA, CRD_SIMD_TARGET and the ADR-0063 determinism options).
crd_package_library(crd-simd-flags NAME simd-flags)
crd_package_library(crd-math NAME math INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/include")
