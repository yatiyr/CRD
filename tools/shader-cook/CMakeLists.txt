# shader_cook — D-007 D2 (ADR-0104): the offline shader-cook CLI (.kgph -> .crdr). Links the crd-shader-cook cook library.

add_executable(shader_cook
    src/main.cpp
)

target_link_libraries(shader_cook PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    crd-resources
    crd-platform
    crd-shader-cook
)

target_compile_features(shader_cook PRIVATE cxx_std_20)
crd_enable_default_pch(shader_cook)
