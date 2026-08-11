# CEIR-13z-1b (DX12): the DirectX 12 mirror of ceir-gpu-vulkan — a CEIR compute asset executed on a real D3D12 device via
# execute_lowered (ADR-0126), byte-identical to the direct CKIR dispatch. ⛔ Windows-only: crd-kir-dx12 is created only where
# the D3D12 backend is available, so guard on it (the kir-dx12 idiom) — else the link fails at configure on Linux. NOT added
# to gate6b.sh (that gate is the Linux sweep).
if(NOT TARGET crd-kir-dx12)
    return()
endif()

add_executable(crd-ceir-gpu-dx12-tests
    test_ceir_add_dx12.cpp
    test_ceir_render_dx12.cpp   # CEIR-14z-3: the DX12 mirror of the render DEVICE pixel proof
)

target_link_libraries(crd-ceir-gpu-dx12-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-ceir          # the IR + the asset builder
    crd-ceir-gpu      # lower_region + execute_lowered (ADR-0125/0126)
    crd-kir           # the CKIR add kernel + the CPU dispatch harness
    crd-kir-dx12      # the D3D12 CKIR backend (emit_compute_kernel_hlsl support)
    crd-gpu-context-dx12  # Dx12ComputeContext / create_pipeline_from_hlsl (its include dir isn't re-exported by crd-kir-dx12)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-gpu-dx12-tests)
catch_discover_tests(crd-ceir-gpu-dx12-tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK crd_gpu_device)
