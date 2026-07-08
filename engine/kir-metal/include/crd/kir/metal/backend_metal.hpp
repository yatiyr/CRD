#pragma once

// backend_metal.hpp — Phase 3.1.6 v17-d: the CKIR **Metal backend** — `KirBackendMetal` implements the KirBackend seam
// over Apple Metal (Objective-C++ runtime in backend_metal.mm). A separate module (ADR-0096 lean-consumer), GUARDED on
// APPLE — absent off macOS ⇒ the module is skipped, the rest of the engine unaffected. The Metal library is compiled
// with math-mode SAFE (fast-math OFF) ⇒ no FMA fusion ⇒ bit-exact vs the CPU reference. **Built + validated on real
// Apple silicon at Part C (GitHub Actions macOS runners).** ADR-0098. The 6th backend from one CKIR IR.

#include <crd/kir/backend.hpp>

#include <memory>

namespace crd::kir
{

class KirBackendMetal final : public KirBackend
{
public:
    explicit KirBackendMetal(crd::memory::IAllocator* alloc);
    ~KirBackendMetal() override;
    KirBackendMetal(const KirBackendMetal&)            = delete;
    KirBackendMetal& operator=(const KirBackendMetal&) = delete;
    KirBackendMetal(KirBackendMetal&&)                 = delete;
    KirBackendMetal& operator=(KirBackendMetal&&)      = delete;

    [[nodiscard]] const char* name() const noexcept override { return "metal"; }
    [[nodiscard]] bool        valid() const noexcept;

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
