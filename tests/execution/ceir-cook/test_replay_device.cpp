// DIAG.9a -- device records: backend-specific numeric replay of compute dispatches within a declared envelope,
// device-free. The two-dispatch program of device_replay_fixture.hpp runs on the CPU reference executor (crd-kir's
// eval_cpu_kernel) and on test executors that wrap it as "another adapter" whose f32 or integer outputs differ by a
// chosen amount. An exact record reproduces bit for bit on its own adapter and is refused on any other adapter or
// build before anything runs; an ulp record replays anywhere and holds exactly up to its declared bound; the first
// element outside the envelope is named with both values and the authored dispatch that writes it; an edited kernel or
// program is reported as such. The GPU legs are in the ceir-gpu Vulkan and DX12 device tests. ASCII test names.

#include "device_replay_fixture.hpp"

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>

#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <utility>

namespace
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
using ck::DeviceDivergenceKind;
using ck::DeviceElement;
using ck::DeviceEnvelope;
using ck::DeviceEnvelopeKind;
using ck::DeviceReplayStatus;
using ck::ReplayRecord;

constexpr const char* kRecordFile = "diag9a_device_record.crpl";

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

// Another adapter: the CPU reference, then every written f32 element moved `ulps` representable values up (positive
// values grow) and every written integer element moved by `ints`; with `fail`, it reports that error code blamed on
// the program's first dispatch. Counts its runs.
struct Shifted
{
    crd::memory::IAllocator* scratch = nullptr;
    u32                      ulps    = 0U;
    u32                      ints    = 0U;
    u32                      runs    = 0U;
    u8                       fail    = 0U;
};

bool shifted_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, ConstSpan<ck::DeviceKernelSource> kernels,
                 Span<ck::DeviceBufferView> buffers, ck::DeviceRunOutcome& out, String& reason, void* user)
{
    auto* const              s         = static_cast<Shifted*>(user);
    const ck::DeviceExecutor reference = ck::reference_device_executor(s->scratch);
    ++s->runs;
    if (!reference.run(ctx, module, kernels, buffers, out, reason, reference.user))
    {
        return false;
    }
    for (ck::DeviceBufferView& b : buffers)
    {
        if (!b.written)
        {
            continue;
        }
        for (u32& w : b.words)
        {
            w += b.element == DeviceElement::F32 ? s->ulps : s->ints;
        }
    }
    if (s->fail != 0U)
    {
        for (const crd::ceir::Operation* op = module.body()->first_block()->first_op(); op != nullptr;
             op = op->next_in_block())
        {
            if (ctx.op_name(op->kind()) == StringView{"compute.dispatch"})
            {
                out.error    = s->fail;
                out.fault_op = op->stable_id().value;
                break;
            }
        }
    }
    return true;
}

ck::DeviceExecutor shifted_executor(Shifted& s)
{
    ck::DeviceExecutor e;
    e.run             = &shifted_run;
    e.user            = &s;
    e.adapter.backend = StringView{"cpu-shifted"};
    e.adapter.name    = StringView{"the CPU reference, shifted"};
    e.adapter.vendor  = 7U;
    return e;
}

// A reference executor that counts its runs.
struct Counted
{
    ck::DeviceExecutor inner;
    u32                runs = 0U;
};

bool counted_run(crd::ceir::Context& ctx, const crd::ceir::Module& module, ConstSpan<ck::DeviceKernelSource> kernels,
                 Span<ck::DeviceBufferView> buffers, ck::DeviceRunOutcome& out, String& reason, void* user)
{
    auto* const c = static_cast<Counted*>(user);
    ++c->runs;
    return c->inner.run(ctx, module, kernels, buffers, out, reason, c->inner.user);
}

ck::DeviceExecutor counted_executor(Counted& c)
{
    ck::DeviceExecutor e = c.inner;
    e.run                = &counted_run;
    e.user               = &c;
    return e;
}

Array<u8> encode(const ReplayRecord& r, crd::memory::IAllocator* a)
{
    Array<u8> bytes(a);
    ck::encode_record(r, bytes);
    return bytes;
}

ck::RecordError decode(const Array<u8>& bytes, ReplayRecord& out)
{
    return ck::decode_record({bytes.data(), bytes.size()}, out);
}

ReplayRecord copy_of(const ReplayRecord& r, crd::memory::IAllocator* a)
{
    ReplayRecord out(a);
    REQUIRE(decode(encode(r, a), out) == ck::RecordError::Ok);
    return out;
}

Array<u8> slurp_bytes(const char* path, crd::memory::IAllocator* a)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    Array<u8> out(a);
    out.resize(static_cast<usize>(sz));
    f.read(reinterpret_cast<char*>(out.data()), sz);
    return out;
}

ReplayRecord record_on(const fx::Authored& a, const ck::DeviceExecutor& executor, DeviceEnvelope envelope,
                       crd::memory::IAllocator* alloc)
{
    ReplayRecord rec(alloc);
    String       reason(alloc);
    const DeviceReplayStatus s =
        ck::record_device_run(a.request(envelope), executor, &fx::registrar, nullptr, rec, reason);
    INFO(reason.c_str());
    REQUIRE(s == DeviceReplayStatus::Ok);
    return rec;
}

// Programs outside a device program: an f64 buffer, a zero grid dimension, no dispatch.
constexpr const char* kF64Buffer  = "module {\n  ^bb0:\n    %0 = arith.const() {value = 2} : !index\n"
                                    "    %1 = resource.declare() : !buffer<plain,!f64>\n"
                                    "    compute.dispatch(%0, %0, %0, %1) {access = \"w\", kernel = @scale}\n}\n";
constexpr const char* kZeroGrid   = "module {\n  ^bb0:\n    %0 = arith.const() {value = 2} : !index\n"
                                    "    %1 = arith.const() {value = 0} : !index\n"
                                    "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                    "    compute.dispatch(%0, %1, %0, %2) {access = \"w\", kernel = @scale}\n}\n";
constexpr const char* kNoDispatch = "module {\n  ^bb0:\n    %0 = arith.const() {value = 2} : !index\n"
                                    "    %1 = resource.declare() : !buffer<plain,!f32>\n}\n";

constexpr DeviceEnvelope kExact{DeviceEnvelopeKind::Exact, 0U};

DeviceEnvelope ulp(u32 n)
{
    return DeviceEnvelope{DeviceEnvelopeKind::Ulp, n};
}
} // namespace

TEST_CASE("diag 9a: a device run on the CPU reference is recorded and reproduces exactly from its file",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ck::DeviceExecutor reference = ck::reference_device_executor(&alloc);

    const ReplayRecord rec = record_on(a, reference, kExact, &alloc);
    CHECK(rec.schema == ck::kReplayRecordSchema);
    CHECK(rec.executor == ck::ReplayExecutorKind::Device);
    CHECK(view(rec.program_path) == StringView{fx::kFile});
    CHECK(rec.entry.empty());
    CHECK(rec.events.empty());
    CHECK(view(rec.device_adapter.backend) == StringView{"cpu-reference"});
    CHECK(rec.device_adapter.driver == ck::kReferenceDeviceVersion);
    CHECK(rec.device_envelope.kind == DeviceEnvelopeKind::Exact);
    CHECK(rec.device_error == 0U);
    CHECK(rec.fault_op == 0U);
    REQUIRE(rec.device_kernels.size() == 2U);
    CHECK(view(rec.device_kernels[0].symbol) == StringView{"scale"});
    CHECK(view(rec.device_kernels[1].ckir) == view(a.wave));
    CHECK(rec.device_kernels[1].hash == crd::containers::fnv1a_64(a.wave.data(), a.wave.size()));

    // Inputs: the program, build, initial contents and the declared envelope are in the record; nothing else is needed.
    for (u32 i = 0U; i < ck::kReplayInputs; ++i)
    {
        INFO(ck::replay_input_name(i).data());
        const bool held = i <= 2U || i == 8U;
        CHECK(rec.inputs[i].state == (held ? ck::ReplayInputState::Recorded : ck::ReplayInputState::NotNeeded));
    }

    // Buffers in declaration order: a is read only; b, c and d are written.
    REQUIRE(rec.device_buffers.size() == fx::kBuffers);
    CHECK_FALSE(rec.device_buffers[0].written);
    CHECK(rec.device_buffers[0].output.empty());
    CHECK(rec.device_buffers[3].element == DeviceElement::U32);
    for (u32 b = 1U; b < fx::kBuffers; ++b)
    {
        CHECK(rec.device_buffers[b].written);
        CHECK(rec.device_buffers[b].output.size() == fx::kN);
    }
    // An independent evaluation of the kernels: b exact, c the f32 rounding of exp then of the product, d the index.
    for (u32 i = 0U; i < fx::kN; ++i)
    {
        const float av = fx::float_of(a.a[i]);
        const float bv = av * 3.0F + 0.25F;
        const auto  e  = static_cast<float>(crd::math::exp(static_cast<double>(bv)));
        CHECK(rec.device_buffers[0].initial[i] == a.a[i]);
        CHECK(rec.device_buffers[1].output[i] == fx::bits_of(bv));
        CHECK(rec.device_buffers[2].output[i] == fx::bits_of(e * av));
        CHECK(rec.device_buffers[3].output[i] == i);
    }

    // The encoding is deterministic, and the file decodes to the same bytes.
    const Array<u8> bytes = encode(rec, &alloc);
    REQUIRE(ck::write_record_file(StringView{kRecordFile}, rec) == ck::RecordWrite::Ok);
    const Array<u8> file = slurp_bytes(kRecordFile, &alloc);
    (void)std::remove(kRecordFile);
    REQUIRE(file.size() == bytes.size());
    CHECK(StringView{reinterpret_cast<const char*>(file.data()), file.size()} ==
          StringView{reinterpret_cast<const char*>(bytes.data()), bytes.size()});
    ReplayRecord back(&alloc);
    REQUIRE(decode(file, back) == ck::RecordError::Ok);
    const Array<u8> again = encode(back, &alloc);
    CHECK(StringView{reinterpret_cast<const char*>(again.data()), again.size()} ==
          StringView{reinterpret_cast<const char*>(bytes.data()), bytes.size()});

    // Replayed from the file in fresh Contexts, on the same adapter and build: every element bit-identical.
    ck::DeviceReplay r(&alloc);
    REQUIRE(ck::replay_device_record(back, reference, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::Ok);
    CHECK(r.divergence.kind == DeviceDivergenceKind::None);
    CHECK(r.compared == 3U * fx::kN);
    CHECK(r.max_distance == 0U);
    CHECK_FALSE(r.adapter_differs);
    CHECK_FALSE(r.build_differs);
    CHECK_FALSE(r.program_differs);
    CHECK_FALSE(r.kernels_differ);
    CHECK(r.replayed_hash == rec.content_hash);
}

TEST_CASE("diag 9a: an exact device record is refused on another adapter or build before anything runs",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ReplayRecord rec = record_on(a, ck::reference_device_executor(&alloc), kExact, &alloc);

    Shifted          other{&alloc, 0U, 0U, 0U, 0U};
    ck::DeviceReplay r(&alloc);
    CHECK(ck::replay_device_record(rec, shifted_executor(other), &fx::registrar, nullptr, {}, r) ==
          DeviceReplayStatus::OtherAdapter);
    CHECK(other.runs == 0U);
    CHECK(r.adapter_differs);
    INFO(r.reason.c_str());
    CHECK(has(view(r.reason), "an exact envelope is claimed only on the recording adapter"));
    CHECK(has(view(r.reason), "backend,name,vendor"));

    // Each adapter field alone is enough.
    Counted count{ck::reference_device_executor(&alloc), 0U};
    for (u32 field = 0U; field < 4U; ++field)
    {
        ck::DeviceExecutor e = counted_executor(count);
        e.adapter.device     = field == 0U ? 1U : 0U;
        e.adapter.driver     = field == 1U ? 2U : ck::kReferenceDeviceVersion;
        e.adapter.api        = field == 2U ? 3U : 0U;
        e.adapter.name       = field == 3U ? StringView{"another name"} : e.adapter.name;
        CHECK(ck::replay_device_record(rec, e, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::OtherAdapter);
    }
    CHECK(count.runs == 0U);

    // Another build: an exact record is refused even when another build is allowed; an ulp record only without that.
    ReplayRecord built = copy_of(rec, &alloc);
    built.build.version.append("-other");
    ck::DeviceReplayOptions any;
    any.any_build = true;
    CHECK(ck::replay_device_record(built, counted_executor(count), &fx::registrar, nullptr, any, r) ==
          DeviceReplayStatus::OtherBuild);
    CHECK(has(view(r.reason), "an exact envelope is claimed only on the recording build"));
    CHECK(has(view(r.reason), "version"));
    CHECK(count.runs == 0U);

    ReplayRecord loose = record_on(a, ck::reference_device_executor(&alloc), ulp(4U), &alloc);
    loose.build.version.append("-other");
    CHECK(ck::replay_device_record(loose, counted_executor(count), &fx::registrar, nullptr, {}, r) ==
          DeviceReplayStatus::OtherBuild);
    CHECK(count.runs == 0U);
    REQUIRE(ck::replay_device_record(loose, counted_executor(count), &fx::registrar, nullptr, any, r) ==
            DeviceReplayStatus::Ok);
    CHECK(count.runs == 1U);
    CHECK(r.build_differs);
    CHECK(r.divergence.kind == DeviceDivergenceKind::None);

    // Not a device record.
    ReplayRecord plan = copy_of(rec, &alloc);
    plan.executor     = ck::ReplayExecutorKind::Plan;
    CHECK(ck::replay_device_record(plan, counted_executor(count), &fx::registrar, nullptr, {}, r) ==
          DeviceReplayStatus::WrongExecutor);
    CHECK(count.runs == 1U);
}

TEST_CASE("diag 9a: an ulp envelope holds another adapter's outputs exactly up to its declared bound",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ck::DeviceExecutor reference = ck::reference_device_executor(&alloc);
    const StringView         text{fx::kProgram};

    // Recorded on an adapter whose f32 outputs are 3 ULP above the reference's.
    Shifted three{&alloc, 3U, 0U, 0U, 0U};
    {
        const ReplayRecord rec = record_on(a, shifted_executor(three), ulp(3U), &alloc);
        CHECK(view(rec.device_adapter.backend) == StringView{"cpu-shifted"});
        CHECK(rec.device_adapter.vendor == 7U);
        ck::DeviceReplay r(&alloc);
        REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::Ok);
        CHECK(r.adapter_differs);
        CHECK(r.divergence.kind == DeviceDivergenceKind::None);
        CHECK(r.max_distance == 3U);
        CHECK(r.compared == 3U * fx::kN);
    }

    // The same run declared at 2 ULP: the first written element (b[0]) is outside it, named at @scale's dispatch.
    {
        const ReplayRecord rec = record_on(a, shifted_executor(three), ulp(2U), &alloc);
        ck::DeviceReplay   r(&alloc);
        REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::Ok);
        const ck::DeviceDivergence& d = r.divergence;
        REQUIRE(d.kind == DeviceDivergenceKind::Element);
        CHECK(d.buffer == 1U);
        CHECK(d.element == 0U);
        CHECK(d.type == DeviceElement::F32);
        CHECK(d.distance == 3U);
        CHECK(d.bound == 2U);
        CHECK(d.recorded == rec.device_buffers[1].output[0]);
        CHECK(d.replayed + 3U == d.recorded);
        CHECK(r.max_distance == 3U); // every element is still measured
        const fx::TextPos at = fx::locate(text, fx::kScaleDispatch);
        CHECK(view(r.dispatch.file) == StringView{fx::kFile});
        CHECK(r.dispatch.line == at.line);
        CHECK(r.dispatch.col == at.col);
        const fx::TextPos decl = fx::locate(text, "%3 = resource.declare()");
        CHECK(r.declaration.line == decl.line);
        CHECK(r.declaration.col == decl.col);
    }

    // Integers are equal under any ulp envelope: an index one off is the divergence, at @wave's dispatch.
    {
        Shifted            ints{&alloc, 0U, 1U, 0U, 0U};
        const ReplayRecord rec = record_on(a, shifted_executor(ints), ulp(1000U), &alloc);
        ck::DeviceReplay   r(&alloc);
        REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::Ok);
        REQUIRE(r.divergence.kind == DeviceDivergenceKind::Element);
        CHECK(r.divergence.buffer == 3U);
        CHECK(r.divergence.element == 0U);
        CHECK(r.divergence.type == DeviceElement::U32);
        CHECK(r.divergence.distance == 1U);
        CHECK(r.divergence.bound == 0U);
        CHECK(r.max_distance == 0U);
        CHECK(r.dispatch.line == fx::locate(text, fx::kWaveDispatch).line);
    }

    // An executor that fails where the recording one did not: the outcome is the first divergence, its blamed
    // dispatch named at its line, before any element is compared against it.
    {
        const ReplayRecord rec = record_on(a, reference, ulp(4U), &alloc);
        Shifted            failing{&alloc, 0U, 0U, 0U, 9U};
        ck::DeviceReplay   r(&alloc);
        REQUIRE(ck::replay_device_record(rec, shifted_executor(failing), &fx::registrar, nullptr, {}, r) ==
                DeviceReplayStatus::Ok);
        REQUIRE(r.divergence.kind == DeviceDivergenceKind::Outcome);
        CHECK(r.divergence.recorded == 0U);
        CHECK(r.divergence.replayed == 9U);
        CHECK(r.divergence.recorded_fault == 0U);
        CHECK(r.divergence.replayed_fault == r.fault.op);
        CHECK(r.fault.line == fx::locate(text, fx::kScaleDispatch).line);
        CHECK(r.recorded_fault.op == 0U);

        // Recorded failing, the same failure reproduces.
        const ReplayRecord failed = record_on(a, shifted_executor(failing), ulp(4U), &alloc);
        CHECK(failed.device_error == 9U);
        CHECK(failed.fault_op != 0U);
        REQUIRE(ck::replay_device_record(failed, shifted_executor(failing), &fx::registrar, nullptr, {}, r) ==
                DeviceReplayStatus::Ok);
        CHECK(r.divergence.kind == DeviceDivergenceKind::None);
        CHECK(r.recorded_fault.line == fx::locate(text, fx::kScaleDispatch).line);
    }
}

TEST_CASE("diag 9a: a device replay against an edited kernel or program names the dispatch that writes the change",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ck::DeviceExecutor reference = ck::reference_device_executor(&alloc);
    const ReplayRecord       rec       = record_on(a, reference, kExact, &alloc);

    // The checkout's @wave now scales its product by 1.25: c[0] is the first element that differs.
    const String                   edited = fx::wave_kernel(&alloc, 1.25);
    const ck::DeviceKernelSource   kernels[2] = {a.kernels[0], {StringView{"wave"}, view(edited)}};
    ck::DeviceReplayOptions        options;
    options.kernels_against = {kernels, 2U};
    ck::DeviceReplay r(&alloc);
    REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, options, r) == DeviceReplayStatus::Ok);
    CHECK(r.kernels_differ);
    CHECK_FALSE(r.program_differs);
    REQUIRE(r.divergence.kind == DeviceDivergenceKind::Element);
    CHECK(r.divergence.buffer == 2U);
    CHECK(r.divergence.element == 0U);
    CHECK(fx::float_of(r.divergence.replayed) == fx::float_of(r.divergence.recorded) * 1.25F);
    const fx::TextPos wave = fx::locate(StringView{fx::kProgram}, fx::kWaveDispatch);
    CHECK(r.dispatch.line == wave.line);
    CHECK(r.dispatch.col == wave.col);

    // The same kernels against the recorded texts reproduce, and are not reported as edited.
    options.kernels_against = {a.kernels, 2U};
    REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, options, r) == DeviceReplayStatus::Ok);
    CHECK_FALSE(r.kernels_differ);
    CHECK(r.divergence.kind == DeviceDivergenceKind::None);

    // An edited program (one more constant) with the edited kernel: another content hash, the dispatch a line lower.
    const Array<u8> moved = fx::cook(fx::kProgramMoved, &alloc);
    options.against         = {moved.data(), moved.size()};
    options.kernels_against = {kernels, 2U};
    REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, options, r) == DeviceReplayStatus::Ok);
    CHECK(r.program_differs);
    CHECK(r.replayed_hash != rec.content_hash);
    REQUIRE(r.divergence.kind == DeviceDivergenceKind::Element);
    CHECK(r.dispatch.line == fx::locate(StringView{fx::kProgramMoved}, fx::kWaveDispatch).line);
    CHECK(r.dispatch.line == wave.line + 1U);

    // The moved program with the recorded kernels reproduces: the change is only where the ops are.
    options.kernels_against = {};
    REQUIRE(ck::replay_device_record(rec, reference, &fx::registrar, nullptr, options, r) == DeviceReplayStatus::Ok);
    CHECK(r.program_differs);
    CHECK(r.divergence.kind == DeviceDivergenceKind::None);
}

TEST_CASE("diag 9a: device records refuse malformed files, foreign fields and requests outside a device program",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ck::DeviceExecutor reference = ck::reference_device_executor(&alloc);
    const ReplayRecord       rec       = record_on(a, reference, ulp(2U), &alloc);
    ReplayRecord             back(&alloc);

    SECTION("the decoder")
    {
        ReplayRecord r = copy_of(rec, &alloc);
        r.schema       = 4U;
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::UnsupportedSchema);

        r = copy_of(rec, &alloc);
        r.entry.append("main"); // a device run has no entry
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r                = copy_of(rec, &alloc);
        r.events_total   = 1U;
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r = copy_of(rec, &alloc);
        r.device_buffers[1].output.push_back(0U); // outputs longer than the buffer
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r = copy_of(rec, &alloc);
        r.device_buffers[0].output.push_back(0U); // outputs of a buffer nothing writes
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r                      = copy_of(rec, &alloc);
        r.device_envelope.kind = DeviceEnvelopeKind::Exact; // an exact envelope carries no bound
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r                      = copy_of(rec, &alloc);
        r.device_envelope.ulps = ck::kReplayMaxUlps + 1U;
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r = copy_of(rec, &alloc);
        r.device_kernels.clear(); // a device record names its kernels
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        r = copy_of(rec, &alloc);
        r.device_adapter.backend.clear();
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        // Device fields in a plan record.
        r          = copy_of(rec, &alloc);
        r.executor = ck::ReplayExecutorKind::Plan;
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);

        // A third executor is the last one.
        r          = copy_of(rec, &alloc);
        r.executor = static_cast<ck::ReplayExecutorKind>(3U);
        CHECK(decode(encode(r, &alloc), back) == ck::RecordError::Malformed);
        CHECK(ck::replay_executor_name(ck::ReplayExecutorKind::Device) == StringView{"device"});
    }

    SECTION("content that is not what was recorded")
    {
        ck::DeviceReplay r(&alloc);
        ReplayRecord     t = copy_of(rec, &alloc);
        t.device_kernels[1].ckir.append(" "); // the text no longer hashes to its recorded hash
        REQUIRE(decode(encode(t, &alloc), back) == ck::RecordError::Ok);
        CHECK(ck::replay_device_record(back, reference, &fx::registrar, nullptr, {}, r) ==
              DeviceReplayStatus::ContentMismatch);
        CHECK(has(view(r.reason), "kernel @wave"));

        t = copy_of(rec, &alloc);
        t.content_hash ^= 1U;
        CHECK(ck::replay_device_record(t, reference, &fx::registrar, nullptr, {}, r) ==
              DeviceReplayStatus::ContentMismatch);

        t = copy_of(rec, &alloc);
        t.program.resize(t.program.size() / 2U);
        CHECK(ck::replay_device_record(t, reference, &fx::registrar, nullptr, {}, r) == DeviceReplayStatus::NotLoaded);

        t = copy_of(rec, &alloc);
        t.inputs[8].state = ck::ReplayInputState::Missing;
        CHECK(ck::replay_device_record(t, reference, &fx::registrar, nullptr, {}, r) ==
              DeviceReplayStatus::MissingInputs);
        CHECK(has(view(r.reason), "device-tolerance"));
    }

    SECTION("requests outside a device program")
    {
        ReplayRecord out(&alloc);
        String       reason(&alloc);

        ck::DeviceRecordRequest q = a.request(kExact);
        q.buffers                 = {a.buffers, 3U};
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::BadRequest);
        CHECK(has(view(reason), "declares 4 buffers and the request gives 3"));

        q                      = a.request(kExact);
        ConstSpan<u32> bufs[4] = {a.buffers[0], a.buffers[1], {}, a.buffers[3]};
        q.buffers              = {bufs, 4U};
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::BadRequest);

        q         = a.request(kExact);
        q.kernels = {a.kernels, 1U}; // no @wave
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::BadRequest);
        CHECK(has(view(reason), "no kernel is given for @wave"));

        const ck::DeviceKernelSource three[3] = {a.kernels[0], a.kernels[1], {StringView{"spare"}, a.kernels[0].ckir}};
        q         = a.request(kExact);
        q.kernels = {three, 3U};
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::BadRequest);
        CHECK(has(view(reason), "kernel @spare is given twice or not dispatched"));

        q          = a.request(DeviceEnvelope{DeviceEnvelopeKind::Exact, 1U});
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::BadRequest);

        // A program that is not one block of declarations and constant-grid dispatches.
        const char* const unsupported[] = {kF64Buffer, kZeroGrid, kNoDispatch};
        const char* const why[] = {"a device buffer is a plain f32, i32 or u32 buffer",
                                   "a dispatch grid is a positive constant", "dispatches at least once"};
        for (usize i = 0U; i < 3U; ++i)
        {
            const Array<u8> blob = fx::cook(unsupported[i], &alloc);
            q                    = a.request(kExact);
            q.blob               = {blob.data(), blob.size()};
            q.buffers            = {a.buffers, 1U};
            q.kernels            = {a.kernels, 1U};
            INFO(unsupported[i]);
            CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
                  DeviceReplayStatus::Unsupported);
            CHECK(has(view(reason), why[i]));
        }
        CHECK(out.program.empty()); // nothing above made a record
    }

    SECTION("a kernel the CPU reference cannot evaluate faithfully")
    {
        // @scale indexed by GlobalInvocationId, which the evaluator would read as 0.
        crd::kir::KGraph      g(&alloc);
        const int             in   = g.buffer_decl(crd::kir::DType::F32, 0, 0, false);
        const int             outb = g.buffer_decl(crd::kir::DType::F32, 0, 1, true);
        const int             mark = g.kernel_stmt_mark();
        const int             idx  = g.vec_comp(g.builtin(crd::kir::KBuiltin::GlobalInvocationId), 0);
        g.stmt_buffer_store(outb, idx, g.buffer_load(in, idx));
        const String           text = crd::kir::ckir_write(g, fx::kernel_entry(g, mark), &alloc);
        ck::DeviceKernelSource k[2] = {{StringView{"scale"}, view(text)}, a.kernels[1]};
        ck::DeviceRecordRequest q   = a.request(kExact);
        q.kernels                   = {k, 2U};
        ReplayRecord out(&alloc);
        String       reason(&alloc);
        CHECK(ck::record_device_run(q, reference, &fx::registrar, nullptr, out, reason) ==
              DeviceReplayStatus::DeviceFailed);
        INFO(reason.c_str());
        CHECK(has(view(reason), "the CPU reference cannot run @scale: it has a vector"));
        CHECK(out.program.empty());
    }
}

TEST_CASE("diag 9a: distances between device elements", "[ceir][cook][diag][device]")
{
    const u32 one      = fx::bits_of(1.0F);
    const u32 nan      = 0x7FC00000U;
    const u32 nan2     = 0x7FC00001U;
    const u32 pzero    = 0x00000000U;
    const u32 nzero    = 0x80000000U;
    const u32 tiny     = 0x00000001U;  // the smallest positive subnormal
    const u32 ntiny    = 0x80000001U;
    const u32 infinity = 0x7F800000U;
    CHECK(ck::f32_ulp_distance(one, one) == 0U);
    CHECK(ck::f32_ulp_distance(one, one + 1U) == 1U);
    CHECK(ck::f32_ulp_distance(one + 5U, one) == 5U);
    CHECK(ck::f32_ulp_distance(pzero, nzero) == 0U);
    CHECK(ck::f32_ulp_distance(ntiny, tiny) == 2U); // across zero
    CHECK(ck::f32_ulp_distance(infinity, 0x7F7FFFFFU) == 1U);
    CHECK(ck::f32_ulp_distance(nan, nan2) == 0U);
    CHECK(ck::f32_ulp_distance(nan, one) == ck::kDeviceIncomparable);
    CHECK(ck::f32_ulp_distance(one, nan) == ck::kDeviceIncomparable);

    // Exact sees the bit pattern: a zero's sign and a NaN's payload differ by at least 1.
    CHECK(ck::device_distance(DeviceElement::F32, kExact, pzero, nzero) == 1U);
    CHECK(ck::device_distance(DeviceElement::F32, kExact, nan, nan2) == 1U);
    CHECK(ck::device_distance(DeviceElement::F32, kExact, one, one + 4U) == 4U);
    CHECK(ck::device_distance(DeviceElement::F32, ulp(4U), pzero, nzero) == 0U);
    CHECK(ck::device_distance(DeviceElement::I32, ulp(4U), fx::bits_of(0.0F), 0xFFFFFFFEU) == 2U); // 0 and -2
    CHECK(ck::device_distance(DeviceElement::U32, ulp(4U), 0xFFFFFFFEU, 0U) == 0xFFFFFFFEULL);

    CHECK(ck::within_envelope(DeviceElement::F32, ulp(4U), 4U));
    CHECK_FALSE(ck::within_envelope(DeviceElement::F32, ulp(4U), 5U));
    CHECK_FALSE(ck::within_envelope(DeviceElement::F32, ulp(4U), ck::kDeviceIncomparable));
    CHECK_FALSE(ck::within_envelope(DeviceElement::I32, ulp(4U), 1U));
    CHECK(ck::within_envelope(DeviceElement::U32, ulp(4U), 0U));
    CHECK_FALSE(ck::within_envelope(DeviceElement::F32, kExact, 1U));
    CHECK(ck::device_envelope_name(DeviceEnvelopeKind::Ulp) == StringView{"ulp"});
    CHECK(ck::device_element_name(DeviceElement::I32) == StringView{"i32"});
    CHECK(ck::device_replay_status_name(DeviceReplayStatus::OtherAdapter) == StringView{"other-adapter"});
    CHECK(ck::device_divergence_name(DeviceDivergenceKind::Element) == StringView{"element"});
}

TEST_CASE("diag 9a: replay.run refuses a device record on a host with no device executor", "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const ReplayRecord rec = record_on(a, ck::reference_device_executor(&alloc), kExact, &alloc);
    (void)std::remove(kRecordFile);
    REQUIRE(ck::write_record_file(StringView{kRecordFile}, rec) == ck::RecordWrite::Ok);

    namespace perf = crd::perf;
    perf::DiagServiceConfig config;
    config.root = StringView{"."};
    perf::DiagCommandService svc(perf::authority_bit(perf::DiagAuthority::Read) |
                                     perf::authority_bit(perf::DiagAuthority::Execute) |
                                     perf::authority_bit(perf::DiagAuthority::Record),
                                 config);
    ck::ReplayCommands cmd;
    cmd.registrar = &fx::registrar;
    REQUIRE(ck::register_replay_run(svc, cmd));
    perf::DiagRequest r;
    r.command    = ck::kReplayRunCommand;
    r.path       = StringView{kRecordFile};
    r.page_items = 64U;
    r.page_bytes = perf::kDiagMaxPageBytes;
    const perf::DiagResult result = svc.execute(r);
    INFO(result.json.c_str());
    CHECK(result.status == perf::DiagStatus::Unavailable);
    CHECK(has(view(result.json), "made by the device executor and this host binds no device executor"));
    CHECK(cmd.replay_runs.load() == 1U);

    // replay.prepare reads the same record: its device tolerance is in the record, and nothing is missing.
    ck::ReplayPrepareCommand prepare;
    prepare.registrar = &fx::registrar;
    REQUIRE(ck::register_replay_prepare(svc, prepare));
    r.command = ck::kReplayPrepareCommand;
    const perf::DiagResult prepared = svc.execute(r);
    (void)std::remove(kRecordFile);
    INFO(prepared.json.c_str());
    CHECK(prepared.status == perf::DiagStatus::Ok);
    CHECK(has(view(prepared.json), R"("input":"device-tolerance")"));
    CHECK(has(view(prepared.json), "a tolerance or oracle for device numerics is in the run record"));
    CHECK(has(view(prepared.json), R"("missing":0,)"));
}
