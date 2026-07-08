#pragma once

// backend_cuda.hpp — Phase 3.1.6 v17-c: the CKIR **CUDA backend** — `KirBackendCuda` implements the KirBackend seam
// over the CUDA DRIVER API + NVRTC (CUDA C → PTX at runtime). NOT through crd-rhi (CUDA bypasses Vulkan). A separate
// module (ADR-0096 lean-consumer): only a consumer that wants CUDA links CUDA. NVRTC compiles with `--fmad=false
// --prec-div=true --prec-sqrt=true` ⇒ bit-exact vs the CPU reference for correctly-rounded ops incl. division.
// ADR-0098. This is also the vendor-perf backend (the cuBLAS/cuFFT crush baseline, v17-g onward).

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendCuda final : public KirBackend
{
public:
    explicit KirBackendCuda(crd::memory::IAllocator* alloc);
    ~KirBackendCuda() override;
    KirBackendCuda(const KirBackendCuda&)            = delete;
    KirBackendCuda& operator=(const KirBackendCuda&) = delete;
    KirBackendCuda(KirBackendCuda&&)                 = delete;
    KirBackendCuda& operator=(KirBackendCuda&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "cuda"; }
    [[nodiscard]] bool        valid() const noexcept; // false if no CUDA device / driver

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
