# crd-render-asset-core — RAF-1 (D-007 "RAF band"; mission §5 / §15).
#
# The canonical render-asset IDENTITY substrate: virtual paths + namespaces
# (engine:// app:// plugin:// test://, with crd:// as a deprecated alias),
# stable deterministic AssetIds + collision detection, structured Diagnostics,
# and a deterministic dependency graph. Shared by every render-asset family
# (shader · program · material · technique · render-phase · frame-graph) so
# identity/diagnostics/dependency ordering are defined once, not per cooker.
#
# See docs/design/raf-0-rendering-foundation-design.md §3 (type ownership).
# Leaf module — deps: crd-core / crd-containers / crd-memory only.

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-render-asset-core STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-render-asset-core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-render-asset-core
    PUBLIC
        crd-core         # u8/u16/u32/u64/usize, assert
        crd-containers   # String / StringView / Array / hash / sort
        crd-memory       # IAllocator + default_allocator()
)

target_compile_features(crd-render-asset-core PUBLIC cxx_std_20)
crd_enable_default_pch(crd-render-asset-core)
