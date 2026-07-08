// backend_hip.cpp — Phase 3.1.6 v17-d: KirBackendHip over the HIP runtime + hiprtc. Mirrors the CUDA backend
// (cu*→hip*), reusing the CUDA emitter (HIP C == CUDA C). Per kernel: emit HIP C → hiprtc compile (code object,
// -ffp-contract=off for determinism, targeting the device's gcnArchName) → hipModuleLoadData → hipMalloc/HtoD →
// hipModuleLaunchKernel → DtoH. Bit-exact vs the CPU reference on AMD. Validated on real AMD at Part C. ADR-0098.

#include <crd/kir/hip/backend_hip.hpp>

#include <crd/kir/ckir_cuda.hpp> // HIP C == CUDA C ⇒ reuse the CUDA emitter

#include <crd/containers/array.hpp>

#include <cstdio>
#include <cstring>

#include <hip/hip_runtime.h>
#include <hip/hiprtc.h>

namespace crd::kir
{

struct KirBackendHip::Impl
{
    crd::memory::IAllocator* alloc = nullptr;
    char                     arch[64] = {};
    bool                     ok = false;
};

namespace
{
constexpr int kMaxIn = 32;

// hiprtc: compile HIP C (null-terminated `src`) → a code object for `arch`, into `code`. false on failure.
bool compile_code(const char* src, const char* arch, crd::containers::Array<char>& code)
{
    hiprtcProgram prog = nullptr;
    if (hiprtcCreateProgram(&prog, src, "ckir.cu", 0, nullptr, nullptr) != HIPRTC_SUCCESS) { return false; }
    char archopt[80];
    std::snprintf(archopt, sizeof(archopt), "--gpu-architecture=%s", arch);
    const char*        opts[] = {"-ffp-contract=off", archopt};
    const hiprtcResult r      = hiprtcCompileProgram(prog, 2, opts);
    if (r != HIPRTC_SUCCESS)
    {
        size_t logsz = 0;
        hiprtcGetProgramLogSize(prog, &logsz);
        if (logsz > 1)
        {
            crd::containers::Array<char> log(code.allocator());
            log.resize(logsz, '\0');
            hiprtcGetProgramLog(prog, log.data());
            std::fprintf(stderr, "[ckir-hip hiprtc] %s\nsrc:\n%s\n", log.data(), src);
        }
        hiprtcDestroyProgram(&prog);
        return false;
    }
    size_t sz = 0;
    if (hiprtcGetCodeSize(prog, &sz) != HIPRTC_SUCCESS || sz == 0) { hiprtcDestroyProgram(&prog); return false; }
    code.resize(sz, '\0');
    const hiprtcResult gr = hiprtcGetCode(prog, code.data());
    hiprtcDestroyProgram(&prog);
    return gr == HIPRTC_SUCCESS;
}

bool launch_and_readback(hipFunction_t fn, crd::u32 groups, void** params, hipDeviceptr_t* d_in, const crd::u64* in_bytes,
                         int n_inputs, const int* input_iidx, const float* const* inputs, hipDeviceptr_t d_out,
                         crd::u64 out_bytes, float* out)
{
    for (int i = 0; i < n_inputs; ++i)
    {
        if (hipMemcpyHtoD(d_in[i], const_cast<float*>(inputs[input_iidx[i]]), in_bytes[i]) != hipSuccess) { return false; }
    }
    if (hipModuleLaunchKernel(fn, groups > 0U ? groups : 1U, 1U, 1U, 256U, 1U, 1U, 0U, nullptr, params, nullptr) != hipSuccess) { return false; }
    if (hipDeviceSynchronize() != hipSuccess) { return false; }
    return hipMemcpyDtoH(out, d_out, out_bytes) == hipSuccess;
}
} // namespace

KirBackendHip::KirBackendHip(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    if (hipInit(0) != hipSuccess) { return; }
    int count = 0;
    if (hipGetDeviceCount(&count) != hipSuccess || count == 0) { return; }
    if (hipSetDevice(0) != hipSuccess) { return; }
    hipDeviceProp_t prop{};
    if (hipGetDeviceProperties(&prop, 0) != hipSuccess) { return; }
    std::snprintf(impl.arch, sizeof(impl.arch), "%s", prop.gcnArchName);
    impl.ok = true;
}

KirBackendHip::~KirBackendHip() = default;

bool KirBackendHip::valid() const noexcept { return m_impl->ok; }

bool KirBackendHip::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxIn) { return false; }
    const KNode& outn = g.node(output);

    GlslKernel kern(impl.alloc);
    crd::u64   in_bytes[kMaxIn] = {};
    crd::u64   out_bytes        = 0;
    crd::u32   groups           = 0;
    crd::u32   d0 = 0, d1 = 0, d2 = 0, d3 = 0;

    if (outn.op == KOp::Contract)
    {
        if (!emit_contract_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode& an = g.node(outn.a);
        const KNode& bn = g.node(outn.b);
        const int    r  = an.shape.rank;
        d0              = static_cast<crd::u32>(an.shape.dims[r - 2]);
        d1              = static_cast<crd::u32>(an.shape.dims[r - 1]);
        d2              = static_cast<crd::u32>(bn.shape.dims[bn.shape.rank - 1]);
        d3              = 1U;
        for (int k = 0; k < r - 2; ++k) { d3 *= static_cast<crd::u32>(an.shape.dims[k]); }
        in_bytes[0] = static_cast<crd::u64>(d0) * d1 * d3 * sizeof(float);
        in_bytes[1] = static_cast<crd::u64>(d1) * d2 * d3 * sizeof(float);
        out_bytes   = static_cast<crd::u64>(d0) * d2 * d3 * sizeof(float);
        groups      = (d0 * d2 * d3 + 255U) / 256U;
    }
    else if (is_reduce(outn.op))
    {
        const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel block tree-reduce
        if (fast) { if (!emit_reduce_fast_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_reduce_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
        const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
        d0                       = static_cast<crd::u32>(out_numel);
        d1                       = static_cast<crd::u32>(in_numel / out_numel);
        in_bytes[0]              = in_numel * sizeof(float);
        out_bytes                = out_numel * sizeof(float);
        groups                   = fast ? static_cast<crd::u32>(out_numel) : (static_cast<crd::u32>(out_numel) + 255U) / 256U; // T2: 1 block/output
    }
    else if (outn.op == KOp::Gather)
    {
        if (!emit_gather_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   dn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 data_numel = static_cast<crd::u64>(dn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        d0                        = static_cast<crd::u32>(out_numel);                                     // nout
        d1                        = static_cast<crd::u32>(data_numel / static_cast<crd::u64>(dn.shape.dims[0])); // rowsize
        in_bytes[0]               = data_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::Scatter)
    {
        if (!emit_scatter_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const KNode&   bn         = g.node(outn.a);
        const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
        const crd::u64 base_numel = static_cast<crd::u64>(bn.shape.numel());
        const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
        const crd::u64 upd_numel  = static_cast<crd::u64>(g.node(outn.c).shape.numel());
        d0                        = static_cast<crd::u32>(out_numel);                                     // nout
        d1                        = static_cast<crd::u32>(base_numel / static_cast<crd::u64>(bn.shape.dims[0])); // rowsize
        d2                        = static_cast<crd::u32>(idx_numel);                                     // M
        in_bytes[0]               = base_numel * sizeof(float);
        in_bytes[1]               = idx_numel * sizeof(float);
        in_bytes[2]               = upd_numel * sizeof(float);
        out_bytes                 = out_numel * sizeof(float);
        groups                    = (static_cast<crd::u32>(out_numel) + 255U) / 256U;
    }
    else if (outn.op == KOp::ScanSum)
    {
        const bool fast = (outn.tier == DetTier::Fast); // T2 parallel block prefix-sum
        if (fast) { if (!emit_scan_fast_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
        else if (!emit_scan_cuda(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
        const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
        d0                     = static_cast<crd::u32>(numel / scanlen); // nrows
        d1                     = scanlen;
        in_bytes[0]            = numel * sizeof(float);
        out_bytes              = numel * sizeof(float);        // scan KEEPS the shape
        groups                 = fast ? d0 : (d0 + 255U) / 256U; // T2: 1 block/row
    }
    else
    {
        if (!emit_elementwise_cuda(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
        const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
        d0                = static_cast<crd::u32>(on);
        for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
        out_bytes = on * sizeof(float);
        groups    = (static_cast<crd::u32>(on) + 255U) / 256U;
    }

    crd::containers::Array<char> code(impl.alloc);
    if (!compile_code(kern.source.c_str(), impl.arch, code)) { return false; }
    hipModule_t mod = nullptr;
    if (hipModuleLoadData(&mod, code.data()) != hipSuccess) { return false; }
    hipFunction_t fn = nullptr;
    if (hipModuleGetFunction(&fn, mod, "ckir") != hipSuccess) { hipModuleUnload(mod); return false; }

    hipDeviceptr_t d_in[kMaxIn] = {};
    hipDeviceptr_t d_out        = nullptr;
    bool           alloc_ok     = true;
    for (int i = 0; i < n_inputs; ++i) { if (hipMalloc(&d_in[i], in_bytes[i]) != hipSuccess) { alloc_ok = false; } }
    if (alloc_ok && hipMalloc(&d_out, out_bytes) != hipSuccess) { alloc_ok = false; }

    bool result = false;
    if (alloc_ok)
    {
        void* params[kMaxIn + 6];
        int   np = 0;
        for (int i = 0; i < n_inputs; ++i) { params[np++] = &d_in[i]; }
        params[np++] = &d_out;
        params[np++] = &d0;
        if (outn.op == KOp::Contract) { params[np++] = &d1; params[np++] = &d2; params[np++] = &d3; }
        else if (is_reduce(outn.op)) { params[np++] = &d1; }
        else if (outn.op == KOp::Gather) { params[np++] = &d1; }
        else if (outn.op == KOp::Scatter) { params[np++] = &d1; params[np++] = &d2; }
        else if (outn.op == KOp::ScanSum) { params[np++] = &d1; }
        result = launch_and_readback(fn, groups, params, d_in, in_bytes, n_inputs, kern.input_iidx, inputs, d_out, out_bytes, out);
    }

    for (int i = 0; i < n_inputs; ++i) { if (d_in[i] != nullptr) { hipFree(d_in[i]); } }
    if (d_out != nullptr) { hipFree(d_out); }
    hipModuleUnload(mod);
    return result;
}

} // namespace crd::kir
