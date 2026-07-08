# crd-kir-hip — Phase 3.1.6 v17-d: the CKIR HIP/ROCm backend. `KirBackendHip` over the HIP runtime + hiprtc (reuses the
# CUDA emitter — HIP C == CUDA C). A SEPARATE module (ADR-0096 lean-consumer). GUARDED on the HIP SDK — ABSENT on this
# NVIDIA box (no AMD GPU; HIP-on-NVIDIA-Windows is awkward) ⇒ the module is skipped and the rest of the engine builds
# unaffected. **Built + validated on real AMD silicon at Part C (RunPod).** ADR-0098.

if(NOT DEFINED HIP_PATH AND NOT DEFINED ENV{HIP_PATH})
    file(GLOB _crd_rocm_dirs "C:/Program Files/AMD/ROCm/*")
    if(_crd_rocm_dirs)
        list(SORT _crd_rocm_dirs)
        list(GET _crd_rocm_dirs -1 HIP_PATH)
    endif()
endif()

find_package(hip QUIET)
if(NOT hip_FOUND)
    message(STATUS "crd-kir-hip: HIP SDK not found -- skipping the HIP backend (built + validated on real AMD at Part C).")
    return()
endif()

crd_collect_sources(SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src")
crd_collect_sources(HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/include")

add_library(crd-kir-hip STATIC ${SOURCES} ${HEADERS})

target_include_directories(crd-kir-hip
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_link_libraries(crd-kir-hip
    PUBLIC
        crd-kir
        crd-core
        crd-containers
        crd-memory
        hip::host   # HIP runtime; hiprtc ships with it
    PRIVATE
        crd-warnings
)

# hiprtc lib (name varies by platform); link it explicitly if present.
find_library(CRD_HIPRTC_LIB NAMES hiprtc HINTS "${HIP_PATH}/lib")
if(CRD_HIPRTC_LIB)
    target_link_libraries(crd-kir-hip PUBLIC "${CRD_HIPRTC_LIB}")
endif()

target_compile_features(crd-kir-hip PUBLIC cxx_std_20)
crd_enable_default_pch(crd-kir-hip)
