crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-eylem-rigid3d STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-eylem-rigid3d
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# v1b-a deps:
#   crd-core / crd-memory:   IAllocator, types, asserts
#   crd-containers:          Array (free-list side-table)
#   crd-math:                Vec/Quat AoS + simd::Soa (the chunks hold plain f32 columns)
#   crd-eylem:               public RigidBody + BodyId + PhysicsConfig contract
# v1b-c added crd-scene PUBLIC: EylemSystem inherits ISystem, queries
# (Transform, RigidBodyComponent), calls World setters. Component types
# themselves live in crd-eylem so other modules can register / serialise
# them without depending on the concrete pool family.
target_link_libraries(crd-eylem-rigid3d
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-eylem
        crd-scene
    PRIVATE
        crd-log
)

target_compile_features(crd-eylem-rigid3d PUBLIC cxx_std_20)
crd_enable_default_pch(crd-eylem-rigid3d)

# The fixed-substep body step on eight lanes a step, one body per CPU level (ADR-0134); src/rigid3d_kernel_dispatch.cpp
# lists them.
crd_add_cpu_kernels(crd-eylem-rigid3d NAME rigid3d LEVELS portable sse2 avx2 avx512 INCLUDES crd-math
                    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/kernels/rigid3d_kernels.cpp")
