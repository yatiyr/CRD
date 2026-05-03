add_executable(asset_cooker
    src/main.cpp
)

target_link_libraries(asset_cooker PRIVATE
    crd-core
    crd-memory
    crd-containers
    crd-log
    crd-platform
    crd-resources
)

target_compile_features(asset_cooker PRIVATE cxx_std_20)

crd_enable_default_pch(asset_cooker)
