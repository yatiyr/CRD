# crd-kir tests — Phase 3.1.6 v17. Also the LINK-ISOLATION SMOKE (ADR-0098): links ONLY crd-kir (+ base substrate) —
# a clean link proves the IR + CPU reference drag no GPU API (no vulkan.h symbol) into a consumer. The GLSL-emitter
# compile gate moved to tests/gpu-context-vulkan (ADR-0103: the shaderc compiler lives in the Vulkan backend now, which
# links Vulkan — keeping it here would break this very isolation smoke).
add_executable(crd-kir-tests
    test_ckir_graph.cpp
    test_ckir_grad.cpp
    test_ckir_opt.cpp
    test_ckir_backend.cpp
    test_ckir_tile.cpp
    test_ckir_msl.cpp
    test_ckir_bitops.cpp
    test_ckir_nodes.cpp
    test_ckir_noise.cpp
    test_ckir_lower.cpp
    test_ckir_lighting.cpp
    test_ckir_kernel.cpp
    test_ckir_kernel_emit.cpp
    test_ckir_fft.cpp
    test_ckir_reduce.cpp
    test_ckir_scan.cpp
    test_ckir_sort.cpp
    test_ckir_subgroup.cpp
    test_ckir_mlp.cpp
    test_ckir_svgf.cpp
    test_ckir_ddgi.cpp
    test_ckir_restir.cpp
    test_ckir_atmosphere.cpp
    test_ckir_nrc.cpp
    test_ckir_clouds.cpp
    test_ckir_ocean.cpp
    test_ckir_water.cpp
    test_ckir_hair.cpp
    test_ckir_hair_grazing.cpp
    test_ckir_hair_scatter.cpp
    test_ckir_hair_geom.cpp
    test_ckir_hair_filter.cpp
    test_ckir_hair_lod.cpp
    test_ckir_emitter_completeness.cpp
    test_ckir_hair_render.cpp
    test_ckir_lss.cpp
    test_ckir_curve_rt.cpp
    test_ckir_rt.cpp
)

target_link_libraries(crd-kir-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-kir
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-kir-tests)

catch_discover_tests(crd-kir-tests)
