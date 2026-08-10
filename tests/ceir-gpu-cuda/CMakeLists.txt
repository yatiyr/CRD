# CEIR-13z (CUDA): the THIRD backend — a CEIR compute asset executed on a real CUDA device via execute_lowered (ADR-0126),
# byte-identical to the direct CKIR dispatch. CUDA is a REQUIRED compute backend (mission: portable GPU compute, all
# backends). The `dispatch_ceir_{1wg,multi}` harness is backend-agnostic (an IComputeContext&); only the context factory +
# emitter (emit_compute_kernel_cuda → NVRTC via create_pipeline_from_cuda, entry "ckir") differ. ⛔ Guarded on the CUDA
# context target — the whole target vanishes on a machine without the CUDA toolkit (no false skips). NOT in gate6b.sh (the
# Linux sweep here has no NVIDIA device); it runs where CUDA is present.
if(NOT TARGET crd-gpu-context-cuda)
    return()
endif()

add_executable(crd-ceir-gpu-cuda-tests
    test_ceir_add_cuda.cpp
)

target_link_libraries(crd-ceir-gpu-cuda-tests PRIVATE
    crd-core
    crd-containers
    crd-memory
    crd-ceir             # the IR + the asset builder
    crd-ceir-gpu         # lower_region + execute_lowered (ADR-0125/0126)
    crd-kir              # the CKIR kernels + emit_compute_kernel_cuda + the CPU dispatch/oracle harness (header-only)
    crd-gpu-context      # IComputeContext
    crd-gpu-context-cuda # CudaComputeContext / create_cuda_compute_context / create_pipeline_from_cuda (NVRTC)
    Catch2::Catch2WithMain
)

crd_enable_default_pch(crd-ceir-gpu-cuda-tests)

# ⛔ Scar (2026-08-10): under a Linux ASan build on WSL, `cuInit` in the `libcuda.so` GPU-passthrough shim FAILS (the device is
# unavailable to ASan-instrumented processes) — so these tests correctly SKIP ("no CUDA device available"). But the driver's
# own cuInit allocations (~50 KB, some in `<unknown module>` JIT/anon regions LSan cannot name — so a `leak:libcuda`
# suppression only catches ~half) are never freed and trip LeakSanitizer at exit, turning a correct skip into exit != 0.
# Disable LEAK detection (only) for this binary on Linux — ASan's use-after-free / overflow checks stay fully active, and the
# tests never reach our code under ASan (they skip), so no leak of OURS is hidden. Real Linux CUDA coverage is the linux-gcc-
# DEBUG run on the physical device (5/5, real GPU); Windows debug+asan cover CUDA with the native driver (no suppression
# needed). ⛔ Do NOT broaden to skipping the target — the sentinel must stay so a CUDA-capable box still builds it.
if(UNIX)
    catch_discover_tests(crd-ceir-gpu-cuda-tests DISCOVERY_MODE PRE_TEST
        PROPERTIES RESOURCE_LOCK crd_gpu_device ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
else()
    catch_discover_tests(crd-ceir-gpu-cuda-tests DISCOVERY_MODE PRE_TEST PROPERTIES RESOURCE_LOCK crd_gpu_device)
endif()
