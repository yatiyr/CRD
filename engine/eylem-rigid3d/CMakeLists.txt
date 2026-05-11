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
#   crd-math:                Vec/Quat AoS + simd::Soa + Vec8f/Vec4f columns
#   crd-eylem:               public RigidBody + BodyId + PhysicsConfig contract
# v1b-c will add crd-scene (RigidBodyComponent + EylemSystem hook into ECS).
target_link_libraries(crd-eylem-rigid3d
    PUBLIC
        crd-core
        crd-memory
        crd-containers
        crd-math
        crd-eylem
    PRIVATE
        crd-log
)

target_compile_features(crd-eylem-rigid3d PUBLIC cxx_std_20)
crd_enable_default_pch(crd-eylem-rigid3d)
