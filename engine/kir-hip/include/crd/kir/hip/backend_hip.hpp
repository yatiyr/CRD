#pragma once

// backend_hip.hpp — Phase 3.1.6 v17-d: the CKIR **HIP/ROCm backend** — `KirBackendHip` over the HIP runtime + hiprtc
// (HIP C → code object at runtime). A separate module (ADR-0096 lean-consumer), GUARDED on the HIP SDK. HIP kernel C
// is ~identical to CUDA C (`__global__`/`blockIdx`/…), so this backend REUSES the CUDA emitter (`ckir_cuda.hpp`) — only
// the runtime API differs (`hip*` + hiprtc). `-ffp-contract=off` ⇒ bit-exact vs the CPU reference on AMD.
// **Validated on real AMD silicon at Part C (RunPod)** — HIP-on-NVIDIA-Windows is awkward, so this is authored + wired
// now and gated where the SDK+device exist. ADR-0098.

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendHip final : public KirBackend
{
public:
    explicit KirBackendHip(crd::memory::IAllocator* alloc);
    ~KirBackendHip() override;
    KirBackendHip(const KirBackendHip&)            = delete;
    KirBackendHip& operator=(const KirBackendHip&) = delete;
    KirBackendHip(KirBackendHip&&)                 = delete;
    KirBackendHip& operator=(KirBackendHip&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "hip"; }
    [[nodiscard]] bool        valid() const noexcept;

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
