// backend_metal.mm — Phase 3.1.6 v17-d: KirBackendMetal over Apple Metal (Objective-C++). Per kernel: emit MSL →
// newLibraryWithSource (math-mode SAFE = no FMA fusion ⇒ bit-exact) → compute pipeline → shared buffers → compute
// encoder dispatchThreads → waitUntilCompleted → read contents. Built + validated on real Apple silicon at Part C.
// ADR-0098.

#include <crd/kir/metal/backend_metal.hpp>

#include <crd/kir/ckir_msl.hpp>

#include <cstring>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

namespace crd::kir
{

namespace
{
constexpr int kMaxIn = 32;
}

struct KirBackendMetal::Impl
{
    crd::memory::IAllocator* alloc = nullptr;
    id<MTLDevice>            device = nil;
    id<MTLCommandQueue>      queue = nil;
    bool                     ok = false;
};

KirBackendMetal::KirBackendMetal(crd::memory::IAllocator* alloc) : m_impl(std::make_unique<Impl>())
{
    auto& impl = *m_impl;
    impl.alloc = alloc;
    impl.device = MTLCreateSystemDefaultDevice();
    if (impl.device == nil) { return; }
    impl.queue = [impl.device newCommandQueue];
    impl.ok    = impl.queue != nil;
}

KirBackendMetal::~KirBackendMetal() = default;

bool KirBackendMetal::valid() const noexcept { return m_impl->ok; }

bool KirBackendMetal::run(const KGraph& g, int output, const float* const* inputs, int n_inputs, float* out)
{
    auto& impl = *m_impl;
    if (!impl.ok || n_inputs > kMaxIn) { return false; }
    @autoreleasepool
    {
        const KNode& outn = g.node(output);
        GlslKernel   kern(impl.alloc);
        crd::u64     in_bytes[kMaxIn] = {};
        crd::u64     out_bytes        = 0;
        crd::u64     total            = 0;
        unsigned     pc[4]            = {0, 0, 0, 0};
        int          pc_index         = 0;

        if (outn.op == KOp::Contract)
        {
            if (!emit_contract_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            const KNode& an = g.node(outn.a);
            const KNode& bn = g.node(outn.b);
            const int    r  = an.shape.rank;
            pc[0]           = static_cast<unsigned>(an.shape.dims[r - 2]);              // M
            pc[1]           = static_cast<unsigned>(an.shape.dims[r - 1]);              // K
            pc[2]           = static_cast<unsigned>(bn.shape.dims[bn.shape.rank - 1]);  // N
            pc[3]           = 1U;
            for (int k = 0; k < r - 2; ++k) { pc[3] *= static_cast<unsigned>(an.shape.dims[k]); }
            in_bytes[0] = static_cast<crd::u64>(pc[0]) * pc[1] * pc[3] * sizeof(float);
            in_bytes[1] = static_cast<crd::u64>(pc[1]) * pc[2] * pc[3] * sizeof(float);
            out_bytes   = static_cast<crd::u64>(pc[0]) * pc[2] * pc[3] * sizeof(float);
            total       = static_cast<crd::u64>(pc[0]) * pc[2] * pc[3];
            pc_index    = 3;
        }
        else if (is_reduce(outn.op))
        {
            const bool fast = (outn.tier == DetTier::Fast && is_fast_reduceable(outn.op)); // T2 parallel threadgroup tree-reduce
            if (fast) { if (!emit_reduce_fast_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
            else if (!emit_reduce_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            const crd::u64 in_numel  = static_cast<crd::u64>(g.node(outn.a).shape.numel());
            const crd::u64 out_numel = static_cast<crd::u64>(outn.shape.numel());
            pc[0]                    = static_cast<unsigned>(out_numel);
            pc[1]                    = static_cast<unsigned>(in_numel / out_numel);
            in_bytes[0]              = in_numel * sizeof(float);
            out_bytes                = out_numel * sizeof(float);
            total                    = fast ? out_numel * 256 : out_numel; // T2: nout threadgroups x 256 threads
            pc_index                 = 2;
        }
        else if (outn.op == KOp::Gather)
        {
            if (!emit_gather_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            const KNode&   dn         = g.node(outn.a);
            const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
            const crd::u64 data_numel = static_cast<crd::u64>(dn.shape.numel());
            const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
            pc[0]                     = static_cast<unsigned>(out_numel);                                     // nout
            pc[1]                     = static_cast<unsigned>(data_numel / static_cast<crd::u64>(dn.shape.dims[0])); // rowsize
            in_bytes[0]               = data_numel * sizeof(float);
            in_bytes[1]               = idx_numel * sizeof(float);
            out_bytes                 = out_numel * sizeof(float);
            total                     = out_numel;
            pc_index                  = 3;
        }
        else if (outn.op == KOp::Scatter)
        {
            if (!emit_scatter_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            const KNode&   bn         = g.node(outn.a);
            const crd::u64 out_numel  = static_cast<crd::u64>(outn.shape.numel());
            const crd::u64 base_numel = static_cast<crd::u64>(bn.shape.numel());
            const crd::u64 idx_numel  = static_cast<crd::u64>(g.node(outn.b).shape.numel());
            const crd::u64 upd_numel  = static_cast<crd::u64>(g.node(outn.c).shape.numel());
            pc[0]                     = static_cast<unsigned>(out_numel);                                     // nout
            pc[1]                     = static_cast<unsigned>(base_numel / static_cast<crd::u64>(bn.shape.dims[0])); // rowsize
            pc[2]                     = static_cast<unsigned>(idx_numel);                                     // M
            in_bytes[0]               = base_numel * sizeof(float);
            in_bytes[1]               = idx_numel * sizeof(float);
            in_bytes[2]               = upd_numel * sizeof(float);
            out_bytes                 = out_numel * sizeof(float);
            total                     = out_numel;
            pc_index                  = 4;
        }
        else if (outn.op == KOp::ScanSum)
        {
            const bool fast = (outn.tier == DetTier::Fast); // T2 parallel threadgroup prefix-sum
            if (fast) { if (!emit_scan_fast_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; } }
            else if (!emit_scan_msl(g, output, kern) || kern.n_inputs != n_inputs) { return false; }
            const crd::u64 numel   = static_cast<crd::u64>(outn.shape.numel());
            const crd::u32 scanlen = static_cast<crd::u32>(outn.shape.dims[outn.shape.rank - 1]);
            pc[0]                  = static_cast<unsigned>(numel / scanlen); // nrows
            pc[1]                  = scanlen;
            in_bytes[0]            = numel * sizeof(float);
            out_bytes              = numel * sizeof(float);       // scan KEEPS the shape
            total                  = fast ? (numel / scanlen) * 256 : numel / scanlen; // T2: nrows threadgroups x 256
            pc_index               = 2;
        }
        else
        {
            if (!emit_elementwise_msl(g, output, impl.alloc, kern) || kern.n_inputs != n_inputs) { return false; }
            const crd::u64 on = static_cast<crd::u64>(outn.shape.numel());
            pc[0]             = static_cast<unsigned>(on);
            for (int i = 0; i < n_inputs; ++i) { in_bytes[i] = on * sizeof(float); }
            out_bytes = on * sizeof(float);
            total     = on;
            pc_index  = n_inputs + 1;
        }

        NSString*          src  = [NSString stringWithUTF8String:kern.source.c_str()];
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode           = MTLMathModeSafe; // fast-math OFF ⇒ no FMA fusion ⇒ bit-exact vs the CPU reference
        NSError*       err = nil;
        id<MTLLibrary> lib = [impl.device newLibraryWithSource:src options:opts error:&err];
        if (lib == nil) { return false; }
        id<MTLFunction>             fn  = [lib newFunctionWithName:@"ckir"];
        id<MTLComputePipelineState> pso = [impl.device newComputePipelineStateWithFunction:fn error:&err];
        if (pso == nil) { return false; }

        id<MTLBuffer> inbuf[kMaxIn] = {nil};
        for (int i = 0; i < n_inputs; ++i)
        {
            inbuf[i] = [impl.device newBufferWithBytes:inputs[kern.input_iidx[i]] length:(NSUInteger)in_bytes[i] options:MTLResourceStorageModeShared];
        }
        id<MTLBuffer> outbuf = [impl.device newBufferWithLength:(NSUInteger)out_bytes options:MTLResourceStorageModeShared];
        id<MTLBuffer> pcbuf  = [impl.device newBufferWithBytes:pc length:sizeof(pc) options:MTLResourceStorageModeShared];

        id<MTLCommandBuffer>        cb  = [impl.queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pso];
        for (int i = 0; i < n_inputs; ++i) { [enc setBuffer:inbuf[i] offset:0 atIndex:(NSUInteger)i]; }
        [enc setBuffer:outbuf offset:0 atIndex:(NSUInteger)n_inputs];
        [enc setBuffer:pcbuf offset:0 atIndex:(NSUInteger)pc_index];
        const NSUInteger tg = 256;
        [enc dispatchThreads:MTLSizeMake((NSUInteger)total, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { return false; }
        std::memcpy(out, outbuf.contents, static_cast<size_t>(out_bytes));
        return true;
    }
}

} // namespace crd::kir
