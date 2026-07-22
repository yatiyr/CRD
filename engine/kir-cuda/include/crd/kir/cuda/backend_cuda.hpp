#pragma once

// backend_cuda.hpp — Phase 3.1.6 v17-c: the CKIR **CUDA backend** — `KirBackendCuda` implements the KirBackend seam
// over the CUDA DRIVER API + NVRTC (CUDA C → PTX at runtime). NOT through crd-rhi (CUDA bypasses Vulkan). A separate
// module (ADR-0096 lean-consumer): only a consumer that wants CUDA links CUDA. NVRTC compiles with `--fmad=false
// --prec-div=true --prec-sqrt=true` ⇒ bit-exact vs the CPU reference for correctly-rounded ops incl. division.
// ADR-0098. This is also the vendor-perf backend (the cuBLAS/cuFFT crush baseline, v17-g onward).

#include <crd/kir/backend.hpp>
#include <crd/kir/ckir_tile.hpp> // TileSchedule — the AS-1 autotuner injects a specific schedule to measure

#include <memory>

namespace crd::kir
{

// AS-1b: a single measured schedule — GPU-event-timed launch (excludes upload/readback), min over the timed iterations.
struct ContractTiming
{
    bool   ok     = false;
    double min_ms = 0.0; // best of `iters` launches, GPU timestamps only
};

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

    // AS-1b (ADR-0098 §4 autotuner inner loop): compile the Contract at `output` with the EXPLICIT `sched` (bypassing
    // select_schedule), upload inputs once, launch `warmup`+`iters` times GPU-event-timed, write the result to `out` for the
    // oracle to certify. Returns the min-of-iters kernel time. `ok=false` if the schedule doesn't emit/compile/launch.
    [[nodiscard]] ContractTiming time_contract_schedule(const KGraph& g, int output, const TileSchedule& sched,
                                                        const float* const* inputs, int n_inputs, float* out, int warmup,
                                                        int iters);

    // AS-4 (the FUSED crush): time the FUSED GEMM+epilogue kernel at `output` (an elementwise cone over a WarpTiled Contract —
    // GEMM+bias+activation in ONE kernel, the structural win cuBLAS can't fuse: it pays a separate epilogue pass). Uses the
    // auto-tuned schedule (select_schedule). GPU-event-timed min-of-iters; writes `out`. `ok=false` if `output` isn't fusable.
    [[nodiscard]] ContractTiming time_fused_contract(const KGraph& g, int output, const float* const* inputs, int n_inputs,
                                                     float* out, int warmup, int iters);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace crd::kir
