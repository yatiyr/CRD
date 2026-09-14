# crd-technique-cook — REN-37.2 (D-007 row 140): the LIGHTING TECHNIQUE as an authored asset.
# Parses + validates `.crdt` and cooks it to a canonical `.crdk` blob, and (REN-37.3) verifies a technique's
# declared PASS-frequency bindings against the frame-graph pass that drives it.
#
# This module owns the `crd-kir` edge deliberately: `crd-frame-cook` depends on nothing but the API-NEUTRAL GPU
# enums, which is what makes "the frame asset can never name a backend concept" structural rather than a promise.
# A technique's types are IR types, so the edge lives here instead.
# One-way: technique-cook -> {kir, frame-cook, containers, memory}; nothing depends back on it.

add_library(crd-technique-cook
    src/technique_asset.cpp
)

target_include_directories(crd-technique-cook PUBLIC include)

target_link_libraries(crd-technique-cook
    PUBLIC
        crd-core
        crd-containers
        crd-memory
        crd-kir          # the technique CONTRACT vocabulary (BindType/BindFrequency) + the splice ABI
        crd-frame-cook   # REN-37.3: the binding contract is checked against a FramePassDesc
    PRIVATE
        crd-toml
        crd-warnings
)

# checkable parse_result and `ParseFailed` becomes unreachable-by-return.
target_compile_features(crd-technique-cook PUBLIC cxx_std_20)
crd_enable_default_pch(crd-technique-cook)
