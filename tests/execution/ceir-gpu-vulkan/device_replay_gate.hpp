#pragma once

// DIAG.9a -- the shared GPU leg of the device record tests (the Vulkan test and its DX12 twin call
// device_replay_gate; it lives beside the Vulkan test, whose target links every module it names). The device executor
// wraps crd-ceir-gpu's run_device_block (crd/ceir/gpu/device_run.hpp) over the backend's compile hook with the compute
// context's own adapter, the executor a GPU host binds to the replay commands. The gate records the fixture
// program of tests/execution/ceir-cook/device_replay_fixture.hpp on the device and checks the declared envelopes
// against the device itself and against the CPU reference executor, through the library and through the replay
// commands bound to that executor. Soft-skips are the callers'.

#include "../ceir-cook/device_replay_fixture.hpp"

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/gpu/device_run.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/gpu/compute.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <initializer_list>
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

using CompileFn = crd::ceir::gpu::KernelCompileFn;

// The GPU device executor: run_device_block's rig and the context's adapter (the executor's views point into it).
struct GpuRig
{
    crd::ceir::gpu::DeviceRunRig run;
    crd::gpu::ComputeAdapter     adapter{};
};

inline bool gpu_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, ConstSpan<ck::DeviceKernelSource> kernels,
                    Span<ck::DeviceBufferView> buffers, ck::DeviceRunOutcome& out, String& reason, void* user)
{
    namespace ceg   = crd::ceir::gpu;
    auto* const rig = static_cast<GpuRig*>(user);
    Array<ceg::DeviceKernelText> texts(rig->run.alloc);
    for (const ck::DeviceKernelSource& k : kernels)
    {
        texts.push_back(ceg::DeviceKernelText{k.symbol, k.ckir});
    }
    ceg::HostBufferBinding host[ceg::kMaxHostBufferBindings];
    for (usize i = 0U; i < buffers.size() && i < ceg::kMaxHostBufferBindings; ++i)
    {
        host[i] = ceg::HostBufferBinding{buffers[i].resource, buffers[i].words.data(),
                                         static_cast<u64>(buffers[i].words.size()) * sizeof(u32), buffers[i].written};
    }
    ceg::DeviceRunResult result;
    if (!ceg::run_device_block(ctx, *module.body()->first_block(),
                               ConstSpan<ceg::DeviceKernelText>(texts.data(), texts.size()),
                               ConstSpan<ceg::HostBufferBinding>(host, buffers.size()), rig->run, result, reason))
    {
        return false;
    }
    out.error    = static_cast<u8>(result.error);
    out.fault_op = result.fault_op;
    return true;
}

inline ck::DeviceExecutor gpu_executor(GpuRig& rig)
{
    rig.adapter = rig.run.device->adapter();
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

// A diagnostic host whose replay commands bind `bound`, rooted at `root`.
struct CommandHost
{
    static crd::perf::DiagServiceConfig rooted(StringView root)
    {
        crd::perf::DiagServiceConfig c;
        c.root = root;
        return c;
    }

    CommandHost(StringView root, const ck::DeviceExecutor* bound)
        : svc(crd::perf::authority_bit(crd::perf::DiagAuthority::Execute) |
                  crd::perf::authority_bit(crd::perf::DiagAuthority::Record),
              rooted(root))
    {
        cmd.registrar = &fx::registrar;
        cmd.device    = bound;
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
    }

    ck::ReplayCommands            cmd;
    crd::perf::DiagCommandService svc;
};

inline crd::perf::DiagResult command(CommandHost& h, StringView name, const char* path,
                                     std::initializer_list<crd::perf::DiagArg> args)
{
    crd::perf::DiagRequest r;
    r.command    = name;
    r.path       = StringView{path};
    r.args       = {args.begin(), args.size()};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

inline bool contains(const crd::perf::DiagResult& r, StringView needle)
{
    return StringView{r.json.data(), r.json.size()}.find(needle) != StringView::npos;
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
    rig.run.device       = &device;
    rig.run.compile      = compile;
    rig.run.compile_user = compile_user;
    rig.run.alloc        = alloc;

    const ck::DeviceExecutor gpu = gpu_executor(rig);
    REQUIRE(rig.adapter.known);
    CHECK(StringView{rig.adapter.backend} == StringView{backend});
    CHECK_FALSE(StringView{rig.adapter.name}.empty());
    CHECK(rig.adapter.vendor != 0U);
    UNSCOPED_INFO("adapter " << rig.adapter.name << " vendor " << rig.adapter.vendor << " device " << rig.adapter.device
                             << " driver " << rig.adapter.driver << " api " << rig.adapter.api);
    CountedReference reference{ck::reference_device_executor(alloc), 0U};

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
    CHECK(rig.run.runs == 2U);

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

    // 6. Through the replay commands bound to this executor, as a GPU host binds them, from the fixture's files:
    //    replay.record executor=device on the device writes the library's record of step 4 (same inputs and envelope,
    //    and the device is bit-stable, step 2), a fresh service reproduces it on the device, a service bound to the
    //    CPU reference replays it within the declared envelope at the measured distance, and against the edited
    //    kernel folder the device names @wave's dispatch.
    String tag(alloc);
    tag.append("crd_diag9a_device_cmd_");
    tag.append(backend);
    const fx::DeviceRoot root(tag.c_str(), a, alloc);
    {
        CommandHost                 h(root.root(), &gpu);
        const crd::perf::DiagResult made = command(h, ck::kReplayRecordCommand, fx::kFile,
                                                   {{"out", "device.crpl"},
                                                    {"executor", "device"},
                                                    {"envelope", "ulp:32"},
                                                    {"kernel_dir", fx::kKernelDir},
                                                    {"buffers", fx::kBufferFiles}});
        INFO(made.json.c_str());
        REQUIRE(made.status == crd::perf::DiagStatus::Ok);
        String backend_field(alloc);
        backend_field.append(R"("backend":")");
        backend_field.append(backend);
        backend_field.append(R"(")");
        CHECK(contains(made, StringView{backend_field.data(), backend_field.size()}));
        Array<u8> library(alloc);
        ck::encode_record(loose, library);
        const bool same_bytes = root.read(StringView{"device.crpl"}) == library; // a bool: no byte dump on failure
        CHECK(same_bytes);
    }
    {
        CommandHost                 h(root.root(), &gpu);
        const crd::perf::DiagResult again = command(h, ck::kReplayRunCommand, "device.crpl", {});
        INFO(again.json.c_str());
        REQUIRE(again.status == crd::perf::DiagStatus::Ok);
        CHECK(contains(again, R"("adapter_differs":false)"));
        CHECK(contains(again, R"("compared":768,"max_distance":0,"result":"reproduced")"));
    }
    {
        const ck::DeviceExecutor    cpu = counted_reference(reference);
        CommandHost                 h(root.root(), &cpu);
        const crd::perf::DiagResult there = command(h, ck::kReplayRunCommand, "device.crpl", {});
        INFO(there.json.c_str());
        REQUIRE(there.status == crd::perf::DiagStatus::Ok);
        CHECK(contains(there, R"("adapter_differs":true)"));
        CHECK(contains(there, R"("result":"reproduced")"));
        char distance[64];
        (void)std::snprintf(distance, sizeof(distance), R"("max_distance":%llu,)",
                            static_cast<unsigned long long>(measured));
        CHECK(contains(there, StringView{distance}));
    }
    root.text(StringView{"kernels/wave.ckir"}, StringView{edited.data(), edited.size()});
    {
        CommandHost                 h(root.root(), &gpu);
        const crd::perf::DiagResult diff =
            command(h, ck::kReplayRunCommand, "device.crpl", {{"kernel_dir", fx::kKernelDir}});
        INFO(diff.json.c_str());
        REQUIRE(diff.status == crd::perf::DiagStatus::Ok);
        CHECK(contains(diff, R"("divergence":"element","buffer":2,"element":0,"type":"f32",)"));
        char at_line[160];
        (void)std::snprintf(at_line, sizeof(at_line), R"("dispatch_file":"%s","dispatch_line":%u,)", fx::kFile,
                            fx::locate(text, fx::kWaveDispatch).line);
        CHECK(contains(diff, StringView{at_line}));
        CHECK(contains(diff, R"("kernels_match":false)"));
    }
}
} // namespace crd::ceir_gpu_test::device_replay
