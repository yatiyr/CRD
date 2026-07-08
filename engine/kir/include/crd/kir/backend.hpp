#pragma once

// backend.hpp — Phase 3.1.6 v17-b: the CKIR EXECUTION-BACKEND SEAM. `crd-kir` core stays GPU-free; a thin `KirBackend`
// interface is the API-agnostic runtime boundary (ADR-0098). The CPU reference is one backend (the oracle); Vulkan
// (`crd-kir-vulkan`, over crd-rhi-compute + crd-shader), CUDA (`crd-kir-cuda`, over the CUDA driver), etc. are others
// — each does its OWN codegen (GLSL/PTX/…) + dispatch behind this interface. Conformance = run a graph on backend X
// and on the CPU backend, compare bit/ulp. v17-b interface = one fused-elementwise kernel end-to-end (host f32 →
// host f32); persistent buffers + multi-kernel graphs + reduce/contract grow the interface later.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>

#include <crd/containers/array.hpp>

namespace crd::kir
{

class KirBackend
{
public:
    KirBackend()          = default;
    virtual ~KirBackend() = default;
    KirBackend(const KirBackend&) = delete;
    KirBackend& operator=(const KirBackend&) = delete;
    KirBackend(KirBackend&&)                 = delete;
    KirBackend& operator=(KirBackend&&)      = delete;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    // Run the single-kernel graph rooted at `output` (a fused-elementwise cone, or a Contract/Reduce of Input leaves).
    // inputs[k] = host f32 for Input iidx k (sized to that input's numel); out = host f32 sized to output numel. The
    // backend derives all sizes + the kernel type from the graph. Returns false if it can't run the graph.
    [[nodiscard]] virtual bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) = 0;
};

// The CPU reference as a backend — the oracle. Wraps eval_cpu (computes each op in f64 then rounds to the node dtype,
// so an F32 graph is f32-faithful ⇒ IEEE-arithmetic kernels bit-match a GPU f32 kernel). Always available, no GPU.
class KirBackendCpu final : public KirBackend
{
public:
    explicit KirBackendCpu(crd::memory::IAllocator* a) noexcept : m_alloc(a) {}

    [[nodiscard]] const char* name() const noexcept override { return "cpu"; }

    [[nodiscard]] bool run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out) override
    {
        const crd::i64                   on = g.node(output).shape.numel();
        crd::containers::Array<crd::i64> inum(m_alloc); // numel of each Input by iidx
        inum.resize(static_cast<crd::usize>(n_inputs), 0);
        for (int i = 0; i < g.size(); ++i)
        {
            const KNode& nd = g.node(i);
            if (nd.op == KOp::Input && nd.iidx < n_inputs) { inum[static_cast<crd::usize>(nd.iidx)] = nd.shape.numel(); }
        }
        crd::i64 total = 0;
        for (int k = 0; k < n_inputs; ++k) { total += inum[static_cast<crd::usize>(k)]; }
        crd::containers::Array<crd::f64>  buf(m_alloc);
        crd::containers::Array<crd::f64*> ptrs(m_alloc);
        crd::containers::Array<crd::f64>  fout(m_alloc);
        buf.resize(static_cast<crd::usize>(total), 0.0);
        fout.resize(static_cast<crd::usize>(on), 0.0);
        ptrs.reserve(static_cast<crd::usize>(n_inputs));
        crd::i64 off = 0;
        for (int k = 0; k < n_inputs; ++k)
        {
            crd::f64* dst = buf.data() + off;
            for (crd::i64 i = 0; i < inum[static_cast<crd::usize>(k)]; ++i) { dst[i] = static_cast<crd::f64>(inputs[k][i]); }
            ptrs.push_back(dst);
            off += inum[static_cast<crd::usize>(k)];
        }
        eval_cpu(g, ptrs.data(), m_alloc, output, fout.data());
        for (crd::i64 i = 0; i < on; ++i) { out[i] = static_cast<float>(fout[i]); }
        return true;
    }

private:
    crd::memory::IAllocator* m_alloc;
};

} // namespace crd::kir
