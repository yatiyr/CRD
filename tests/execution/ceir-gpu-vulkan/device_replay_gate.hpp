#pragma once

// DIAG.9a -- the shared GPU leg of the device record tests (the Vulkan test and its DX12 twin call
// device_replay_gate; it lives beside the Vulkan test, whose target links every module it names). A GPU device
// executor for crd-ceir-cook's device records: it lowers the program's block (lower_region), reads each dispatched
// kernel's CKIR text, compiles it with the backend's hook, runs the list on host data with execute_lowered_host and
// reports the compute context's own adapter. The gate records the fixture program of
// tests/execution/ceir-cook/device_replay_fixture.hpp on the device and checks the declared envelopes against the
// device itself and against the CPU reference executor. Soft-skips are the callers'.

#include "../ceir-cook/device_replay_fixture.hpp"

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/gpu/execute.hpp>
#include <crd/ceir/gpu/lower.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <memory>

namespace crd::ceir_gpu_test::device_replay
{
namespace ck = crd::ceir::cook;
namespace fx = crd::ceir_test::device_replay;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::String;
using crd::containers::StringView;

// Compile one CKIR compute kernel with `bindings` storage bindings for the backend (nullptr: it does not build).
using CompileFn = std::unique_ptr<crd::gpu::ComputePipeline> (*)(const crd::kir::KGraph& graph,
                                                                 const crd::kir::KEntry& entry, int bindings,
                                                                 void* user, crd::memory::IAllocator* alloc);

inline constexpr u32 kMaxKernels = 16U;

struct GpuRig
{
    crd::gpu::IComputeContext* device       = nullptr;
    CompileFn                  compile      = nullptr;
    void*                      compile_user = nullptr;
    crd::memory::IAllocator*   alloc        = nullptr;
    crd::gpu::ComputeAdapter   adapter{};
    u32                        runs = 0U;
};

// The dispatch ops and their pipelines, resolved by op identity.
struct PipelineTable
{
    const crd::ceir::Operation*                ops[kMaxKernels]{};
    std::unique_ptr<crd::gpu::ComputePipeline> pipes[kMaxKernels];
    u32                                        n = 0U;
};

inline crd::gpu::ComputePipeline* resolve_by_op(const crd::ceir::Operation* op, void* user)
{
    const auto* t = static_cast<const PipelineTable*>(user);
    for (u32 i = 0U; i < t->n; ++i)
    {
        if (t->ops[i] == op)
        {
            return t->pipes[i].get();
        }
    }
    return nullptr;
}

inline bool gpu_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, ConstSpan<ck::DeviceKernelSource> kernels,
                    Span<ck::DeviceBufferView> buffers, ck::DeviceRunOutcome& out, String& reason, void* user)
{
    namespace ceg = crd::ceir::gpu;
    auto* const rig = static_cast<GpuRig*>(user);
    ++rig->runs;
    const crd::ceir::Block* const block = module.body()->first_block();
    Array<ceg::LoweredCommand>    cmds(rig->alloc);
    ceg::lower_region(ctx, *block, cmds);

    PipelineTable table;
    for (const ceg::LoweredCommand& cmd : cmds)
    {
        if (cmd.kind != ceg::LoweredKind::Dispatch || table.n >= kMaxKernels)
        {
            continue;
        }
        const crd::ceir::AttrValue kv = ctx.attr_value(cmd.op->attr(StringView{"kernel"}));
        StringView                 text;
        for (const ck::DeviceKernelSource& k : kernels)
        {
            if (k.symbol == kv.s)
            {
                text = k.ckir;
            }
        }
        crd::kir::KGraph g(rig->alloc);
        crd::kir::KEntry e;
        if (!crd::kir::ckir_read(text, g, e).ok)
        {
            reason.append("a kernel's CKIR text does not read");
            return false;
        }
        table.ops[table.n]   = cmd.op;
        table.pipes[table.n] =
            rig->compile(g, e, static_cast<int>(cmd.op->num_operands()) - 3, rig->compile_user, rig->alloc);
        if (table.pipes[table.n] == nullptr)
        {
            reason.append("a kernel did not compile for the device");
            return false;
        }
        ++table.n;
    }

    ceg::HostBufferBinding host[ceg::kMaxHostBufferBindings];
    for (usize i = 0U; i < buffers.size() && i < ceg::kMaxHostBufferBindings; ++i)
    {
        host[i] = ceg::HostBufferBinding{buffers[i].resource, buffers[i].words.data(),
                                         static_cast<u64>(buffers[i].words.size()) * sizeof(u32), buffers[i].written};
    }
    ceg::DispatchSites sites(rig->alloc);
    const ceg::ExecuteError err = ceg::execute_lowered_host(
        ctx, ConstSpan<ceg::LoweredCommand>(cmds.data(), cmds.size()), *rig->device, &resolve_by_op, &table,
        ConstSpan<ceg::HostBufferBinding>(host, buffers.size()), &sites);
    out.error    = static_cast<u8>(err);
    out.fault_op = sites.fault() != nullptr ? sites.fault()->stable_id().value : 0U;
    return true;
}

inline ck::DeviceExecutor gpu_executor(GpuRig& rig)
{
    ck::DeviceExecutor e;
    e.run             = &gpu_run;
    e.user            = &rig;
    e.adapter.backend = StringView{rig.adapter.backend};
    e.adapter.name    = StringView{rig.adapter.name};
    e.adapter.vendor  = rig.adapter.vendor;
    e.adapter.device  = rig.adapter.device;
    e.adapter.driver  = rig.adapter.driver;
    e.adapter.api     = rig.adapter.api;
    return e;
}

// A CPU reference executor that counts its runs.
struct CountedReference
{
    ck::DeviceExecutor inner;
    u32                runs = 0U;
};

inline bool counted_reference_run(crd::ceir::Context& ctx, const crd::ceir::Module& module,
                                  ConstSpan<ck::DeviceKernelSource> kernels, Span<ck::DeviceBufferView> buffers,
                                  ck::DeviceRunOutcome& out, String& reason, void* user)
{
    auto* const c = static_cast<CountedReference*>(user);
    ++c->runs;
    return c->inner.run(ctx, module, kernels, buffers, out, reason, c->inner.user);
}

inline ck::DeviceExecutor counted_reference(CountedReference& c)
{
    ck::DeviceExecutor e = c.inner;
    e.run                = &counted_reference_run;
    e.user               = &c;
    return e;
}

inline ck::ReplayRecord record_on_device(const fx::Authored& a, const ck::DeviceExecutor& e,
                                         ck::DeviceEnvelope envelope, crd::memory::IAllocator* alloc)
{
    ck::ReplayRecord rec(alloc);
    String           reason(alloc);
    const ck::DeviceReplayStatus s =
        ck::record_device_run(a.request(envelope), e, &fx::registrar, nullptr, rec, reason);
    INFO(reason.c_str());
    REQUIRE(s == ck::DeviceReplayStatus::Ok);
    return rec;
}

// The gate. `backend` is the name the context must report ("vulkan", "dx12"); `record_path` a scratch file.
inline void device_replay_gate(crd::gpu::IComputeContext& device, CompileFn compile, void* compile_user,
                               const char* backend, const char* record_path, crd::memory::IAllocator* alloc)
{
    GpuRig rig;
    rig.device       = &device;
    rig.compile      = compile;
    rig.compile_user = compile_user;
    rig.alloc        = alloc;
    rig.adapter      = device.adapter();
    REQUIRE(rig.adapter.known);
    CHECK(StringView{rig.adapter.backend} == StringView{backend});
    CHECK_FALSE(StringView{rig.adapter.name}.empty());
    CHECK(rig.adapter.vendor != 0U);
    UNSCOPED_INFO("adapter " << rig.adapter.name << " vendor " << rig.adapter.vendor << " device " << rig.adapter.device
                             << " driver " << rig.adapter.driver << " api " << rig.adapter.api);
    const ck::DeviceExecutor gpu = gpu_executor(rig);
    CountedReference         reference{ck::reference_device_executor(alloc), 0U};

    fx::Authored a(alloc);
    fx::author(a, alloc);
    const StringView text{fx::kProgram};
    const ck::DeviceEnvelope exact{ck::DeviceEnvelopeKind::Exact, 0U};

    // 1. Recorded on the device: the exact parts agree with an independent evaluation, exp within the declared bound.
    const ck::ReplayRecord rec = record_on_device(a, gpu, exact, alloc);
    CHECK(rec.device_error == 0U);
    CHECK(StringView{rec.device_adapter.backend.data(), rec.device_adapter.backend.size()} == StringView{backend});
    CHECK(rec.device_adapter.driver == rig.adapter.driver);
    REQUIRE(rec.device_buffers.size() == fx::kBuffers);
    for (u32 i = 0U; i < fx::kN; ++i)
    {
        const float av = fx::float_of(a.a[i]);
        const float bv = av * 3.0F + 0.25F;
        const float cv = static_cast<float>(crd::math::exp(static_cast<double>(bv))) * av;
        CHECK(rec.device_buffers[1].output[i] == fx::bits_of(bv));
        CHECK(ck::f32_ulp_distance(rec.device_buffers[2].output[i], fx::bits_of(cv)) <= fx::kDeclaredUlps);
        CHECK(rec.device_buffers[3].output[i] == i);
    }

    // 2. Through a file, replayed in fresh Contexts on the same adapter and build: bit-identical.
    (void)std::remove(record_path);
    REQUIRE(ck::write_record_file(StringView{record_path}, rec) == ck::RecordWrite::Ok);
    Array<u8> bytes(alloc);
    {
        std::ifstream f(record_path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        bytes.resize(static_cast<usize>(sz));
        f.read(reinterpret_cast<char*>(bytes.data()), sz);
    }
    (void)std::remove(record_path);
    ck::ReplayRecord back(alloc);
    REQUIRE(ck::decode_record({bytes.data(), bytes.size()}, back) == ck::RecordError::Ok);
    ck::DeviceReplay r(alloc);
    REQUIRE(ck::replay_device_record(back, gpu, &fx::registrar, nullptr, {}, r) == ck::DeviceReplayStatus::Ok);
    CHECK(r.divergence.kind == ck::DeviceDivergenceKind::None);
    CHECK(r.max_distance == 0U);
    CHECK(r.compared == 3U * fx::kN);
    CHECK_FALSE(r.adapter_differs);
    CHECK(rig.runs == 2U);

    // 3. The exact record is never replayed on another adapter: the CPU reference is refused before it runs.
    CHECK(ck::replay_device_record(back, counted_reference(reference), &fx::registrar, nullptr, {}, r) ==
          ck::DeviceReplayStatus::OtherAdapter);
    CHECK(reference.runs == 0U);

    // 4. Declared at kDeclaredUlps, the device's record replays on the CPU reference within it. The measured distance
    //    is the device's own; one ULP less than it must fail at the first such element, in @wave's output.
    const ck::ReplayRecord loose = record_on_device(a, gpu, ck::DeviceEnvelope{ck::DeviceEnvelopeKind::Ulp,
                                                                                fx::kDeclaredUlps},
                                                    alloc);
    REQUIRE(ck::replay_device_record(loose, counted_reference(reference), &fx::registrar, nullptr, {}, r) ==
            ck::DeviceReplayStatus::Ok);
    CHECK(reference.runs == 1U);
    CHECK(r.adapter_differs);
    CHECK(r.divergence.kind == ck::DeviceDivergenceKind::None);
    const u64 measured = r.max_distance;
    UNSCOPED_INFO("the device's largest distance from the CPU reference: " << measured << " ULP");
    CHECK(measured <= fx::kDeclaredUlps);
    if (measured > 0U)
    {
        const ck::ReplayRecord tight =
            record_on_device(a, gpu, ck::DeviceEnvelope{ck::DeviceEnvelopeKind::Ulp, static_cast<u32>(measured - 1U)},
                             alloc);
        REQUIRE(ck::replay_device_record(tight, counted_reference(reference), &fx::registrar, nullptr, {}, r) ==
                ck::DeviceReplayStatus::Ok);
        REQUIRE(r.divergence.kind == ck::DeviceDivergenceKind::Element);
        CHECK(r.divergence.buffer == 2U); // b is exact everywhere; the first difference is exp's
        CHECK(r.divergence.distance == measured);
        CHECK(r.divergence.bound == measured - 1U);
        const fx::TextPos wave = fx::locate(text, fx::kWaveDispatch);
        CHECK(r.dispatch.line == wave.line);
        CHECK(r.dispatch.col == wave.col);
    }
    else
    {
        WARN("this adapter's exp matched the CPU reference bit for bit: the one-ULP-less leg has nothing to bound");
    }

    // 5. On the device again, against an edited @wave (its product scaled by 1.25): named at @wave's dispatch.
    const String                 edited     = fx::wave_kernel(alloc, 1.25);
    const ck::DeviceKernelSource kernels[2] = {a.kernels[0],
                                               {StringView{"wave"}, StringView{edited.data(), edited.size()}}};
    ck::DeviceReplayOptions      options;
    options.kernels_against = {kernels, 2U};
    REQUIRE(ck::replay_device_record(back, gpu, &fx::registrar, nullptr, options, r) == ck::DeviceReplayStatus::Ok);
    CHECK(r.kernels_differ);
    REQUIRE(r.divergence.kind == ck::DeviceDivergenceKind::Element);
    CHECK(r.divergence.buffer == 2U);
    CHECK(r.divergence.element == 0U);
    CHECK(r.dispatch.line == fx::locate(text, fx::kWaveDispatch).line);
}
} // namespace crd::ceir_gpu_test::device_replay
