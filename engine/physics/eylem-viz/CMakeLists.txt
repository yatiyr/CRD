crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-eylem-viz STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-eylem-viz
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# v1b-d companion-module deps:
#   crd-eylem:        public component types (RigidBodyComponent, ColliderComponent)
#   crd-eylem-rigid3d: BodyPool + ColliderPool (read-only refs at register time)
#   crd-draw:         RenderBuffer + VisualizerRegistry + shape primitives
#   crd-scene:        World (cross-component lookup for Transform)
#   crd-math:         Mat4f / Vec3f / Quatf used in shape construction
target_link_libraries(crd-eylem-viz
    PUBLIC
        crd-core
        crd-eylem
        crd-eylem-rigid3d
        crd-draw
        crd-scene
        crd-math
)

target_compile_features(crd-eylem-viz PUBLIC cxx_std_20)
crd_enable_default_pch(crd-eylem-viz)
