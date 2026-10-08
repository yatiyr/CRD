// DIAG.9a -- device records through the replay diagnostic commands, device-free. The two-dispatch program of
// device_replay_fixture.hpp is written as files under a diagnostic root (its text, its kernels' CKIR texts in a
// folder and one initial-contents file per declared buffer). `replay.record executor=device` runs it on the host's
// device executor (here the CPU reference, counted) and writes the library's device record byte for byte; a fresh
// service's `replay.run` reproduces it after the program and kernel files are edited, because the record holds its
// own program, kernels and buffers; replaying against the edited kernel folder names the first element outside the
// envelope at the authored dispatch that writes it, in the given program when one is given. An exact record is
// refused on another adapter, and every malformed request, unbound executor and missing or malformed input file is
// refused before the executor runs. The GPU legs (the same commands bound to a Vulkan or DX12 executor) are in the
// ceir-gpu device tests. Expected positions come from scanning the text, never from the parser. ASCII test names.

#include "device_replay_fixture.hpp"

#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/cook/replay_record.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>

namespace
{
namespace ck = crd::ceir::cook;
namespace fx = crd::ceir_test::device_replay;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::Span;
using crd::containers::String;
using crd::containers::StringView;
using crd::perf::DiagArg;
using crd::perf::DiagAuthority;
using crd::perf::DiagCommandService;
using crd::perf::DiagRequest;
using crd::perf::DiagResult;
using crd::perf::DiagServiceConfig;
using crd::perf::DiagStatus;

constexpr const char* kRecord = "device.crpl";

// A program the plan executor runs and that is not a device program (a function with a region).
constexpr const char* kPlanProgram = "module {\n"
                                     "  ^bb0:\n"
                                     "    func.func() {sym_name = \"main\"} {\n"
                                     "      ^bb0:\n"
                                     "        %0 = arith.const() {value = 7} : !i32\n"
                                     "        func.return(%0)\n"
                                     "    }\n"
                                     "}\n";

// The fixture's declarations with @scale dispatched twice (a to b, then a to c) and nothing else.
constexpr const char* kScaleTwice = "module {\n"
                                    "  ^bb0:\n"
                                    "    %0 = arith.const() {value = 4} : !index\n"
                                    "    %1 = arith.const() {value = 1} : !index\n"
                                    "    %2 = resource.declare() : !buffer<plain,!f32>\n"
                                    "    %3 = resource.declare() : !buffer<plain,!f32>\n"
                                    "    %4 = resource.declare() : !buffer<plain,!f32>\n"
                                    "    %5 = resource.declare() : !buffer<plain,!u32>\n"
                                    "    compute.dispatch(%0, %1, %1, %2, %3) {access = \"r,w\", kernel = @scale}\n"
                                    "    compute.dispatch(%0, %1, %1, %2, %4) {access = \"r,w\", kernel = @scale}\n"
                                    "}\n";

StringView view(const String& s)
{
    return StringView{s.data(), s.size()};
}

bool has(StringView hay, StringView needle)
{
    return hay.find(needle) != StringView::npos;
}

// The raw text of `"key":<value>` in `json` (the first occurrence at or after `from`), up to the next ',' or '}'
// (a string value keeps its quotes).
String field(StringView json, const char* key, crd::memory::IAllocator* a, usize from = 0U)
{
    String needle(a);
    needle.append("\"");
    needle.append(key);
    needle.append("\":");
    const usize at = json.find(view(needle), from);
    REQUIRE(at != StringView::npos);
    usize       end   = at + needle.size();
    const usize start = end;
    if (json[end] == '"')
    {
        end = json.find('"', end + 1U) + 1U;
    }
    else
    {
        while (end < json.size() && json[end] != ',' && json[end] != '}')
        {
            ++end;
        }
    }
    String out(a);
    out.append(json.substr(start, end - start));
    return out;
}

u64 number(StringView json, const char* key, crd::memory::IAllocator* a, usize from = 0U)
{
    const String v = field(json, key, a, from);
    u64          n = 0U;
    REQUIRE_FALSE(v.empty());
    for (usize i = 0U; i < v.size(); ++i)
    {
        REQUIRE(v.data()[i] >= '0');
        REQUIRE(v.data()[i] <= '9');
        n = n * 10U + static_cast<u64>(v.data()[i] - '0');
    }
    return n;
}

// The CPU reference executor under another adapter (or its own), counting its runs.
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

ck::DeviceExecutor counted(Counted& c)
{
    ck::DeviceExecutor e = c.inner;
    e.run                = &counted_run;
    e.user               = &c;
    return e;
}

constexpr crd::perf::DiagAuthoritySet kAll = crd::perf::authority_bit(DiagAuthority::Read) |
                                             crd::perf::authority_bit(DiagAuthority::Record) |
                                             crd::perf::authority_bit(DiagAuthority::Execute);

DiagServiceConfig at_root(StringView root)
{
    DiagServiceConfig c;
    c.root = root;
    return c;
}

// A diagnostic host whose replay commands bind `device` (null: no device executor).
struct Host
{
    Host(StringView root, const ck::DeviceExecutor* device) : svc(kAll, at_root(root))
    {
        cmd.registrar = &fx::registrar;
        cmd.device    = device;
        REQUIRE(ck::register_replay_record(svc, cmd));
        REQUIRE(ck::register_replay_run(svc, cmd));
    }

    ck::ReplayCommands cmd;
    DiagCommandService svc;
};

DiagResult send(Host& h, StringView command, const char* path, std::initializer_list<DiagArg> args)
{
    DiagRequest r;
    r.command    = command;
    r.path       = StringView{path};
    r.args       = {args.begin(), args.size()};
    r.page_items = 64U;
    r.page_bytes = crd::perf::kDiagMaxPageBytes;
    return h.svc.execute(r);
}

DiagResult record(Host& h, const char* out, const char* envelope)
{
    return send(h, ck::kReplayRecordCommand, fx::kFile,
                {{"out", out},
                 {"executor", "device"},
                 {"envelope", envelope},
                 {"kernel_dir", fx::kKernelDir},
                 {"buffers", fx::kBufferFiles}});
}

void refused(const DiagResult& r, DiagStatus want, const char* reason)
{
    INFO(r.json.c_str());
    CHECK(r.status == want);
    CHECK(has(StringView{r.reason.data(), r.reason.size()}, StringView{reason}));
}
} // namespace

TEST_CASE("diag 9a: replay.record executor=device writes the library's device record and replay.run reproduces it",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const fx::DeviceRoot root("crd_diag9a_device_cmd_record", a, &alloc);
    Counted              reference{ck::reference_device_executor(&alloc), 0U};
    const ck::DeviceExecutor device = counted(reference);
    const ck::DeviceEnvelope ulp4{ck::DeviceEnvelopeKind::Ulp, 4U};

    {
        Host             h(root.root(), &device);
        const DiagResult r = record(h, kRecord, "ulp:4");
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(field(j, "executor", &alloc) == "\"device\"");
        CHECK(field(j, "backend", &alloc) == "\"cpu-reference\"");
        CHECK(number(j, "driver", &alloc) == ck::kReferenceDeviceVersion);
        CHECK(field(j, "envelope", &alloc) == "\"ulp\"");
        CHECK(number(j, "ulps", &alloc) == 4U);
        CHECK(number(j, "kernels", &alloc) == 2U);
        CHECK(number(j, "buffers", &alloc) == fx::kBuffers);
        CHECK(number(j, "elements", &alloc) == u64{fx::kBuffers} * fx::kN);
        CHECK(number(j, "written_buffers", &alloc) == 3U);
        CHECK(number(j, "error", &alloc) == 0U);
        CHECK(field(j, "replay", &alloc) == "\"replayable\"");
        CHECK(has(j, R"("input":"device-tolerance","guarantee":"numeric","needed":"yes","state":"recorded")"));
        CHECK(has(j, R"("kind":"kernel","symbol":"wave","file":"kernels/wave.ckir")"));
        CHECK(has(j, R"("kind":"buffer","index":3,"element":"u32","elements":256,"written":true)"));
        CHECK(has(j, R"("kind":"buffer","index":0,"element":"f32","elements":256,"written":false)"));
        CHECK(reference.runs == 1U);
        CHECK(h.cmd.executions.load() == 1U);
        CHECK(h.cmd.records_written.load() == 1U);

        // The command's record is the library's record of the same artifact, inputs and envelope, byte for byte.
        ck::ReplayRecord lib(&alloc);
        String           reason(&alloc);
        REQUIRE(ck::record_device_run(a.request(ulp4), ck::reference_device_executor(&alloc), &fx::registrar, nullptr,
                                      lib, reason) == ck::DeviceReplayStatus::Ok);
        Array<u8> lib_bytes(&alloc);
        ck::encode_record(lib, lib_bytes);
        const bool same_bytes = root.read(StringView{kRecord}) == lib_bytes; // a bool: no byte dump on failure
        CHECK(same_bytes);

        // Never overwritten: a second record to the same file is refused before anything runs.
        refused(record(h, kRecord, "ulp:4"), DiagStatus::Failed, "refusing to overwrite");
        CHECK(reference.runs == 1U);
    }

    // A fresh service reproduces it from the record's own program, kernels and buffers.
    Host       h(root.root(), &device);
    DiagResult r = send(h, ck::kReplayRunCommand, kRecord, {});
    {
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j = view(r.json);
        CHECK(field(j, "result", &alloc) == "\"reproduced\"");
        CHECK(number(j, "compared", &alloc) == 3U * u64{fx::kN});
        CHECK(number(j, "max_distance", &alloc) == 0U);
        CHECK(field(j, "adapter_differs", &alloc) == "false");
        CHECK(field(j, "program_matches", &alloc) == "true");
        CHECK(field(j, "kernels_match", &alloc) == "true");
        CHECK(field(j, "build", &alloc) == "\"same\"");
        CHECK(reference.runs == 2U);
    }

    // Edit the checkout: @wave's product scaled by 1.25, the program moved a line down. The record still reproduces.
    const String wave = fx::wave_kernel(&alloc, 1.25);
    root.text(StringView{"kernels/wave.ckir"}, view(wave));
    root.text(StringView{fx::kFile}, StringView{fx::kProgramMoved});
    r = send(h, ck::kReplayRunCommand, kRecord, {});
    CHECK(field(view(r.json), "result", &alloc) == "\"reproduced\"");

    // Against the edited kernel folder: the first element outside the envelope, at the record's @wave dispatch.
    const fx::TextPos wave_at = fx::locate(StringView{fx::kProgram}, StringView{fx::kWaveDispatch});
    const fx::TextPos c_decl  = fx::locate(StringView{fx::kProgram}, StringView{"%4 = resource.declare()"});
    r                         = send(h, ck::kReplayRunCommand, kRecord, {{"kernel_dir", fx::kKernelDir}});
    {
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j  = view(r.json);
        const usize      at = j.find(StringView{R"("kind":"divergence")"});
        REQUIRE(at != StringView::npos);
        CHECK(field(j, "divergence", &alloc, at) == "\"element\"");
        CHECK(number(j, "buffer", &alloc, at) == 2U);
        CHECK(number(j, "element", &alloc, at) == 0U);
        CHECK(field(j, "type", &alloc, at) == "\"f32\"");
        CHECK(number(j, "distance", &alloc, at) > 4U);
        CHECK(number(j, "bound", &alloc, at) == 4U);
        CHECK(field(j, "dispatch_file", &alloc, at) == "\"programs/diag/device_replay.ceir\"");
        CHECK(number(j, "dispatch_line", &alloc, at) == wave_at.line);
        CHECK(number(j, "dispatch_col", &alloc, at) == wave_at.col);
        CHECK(number(j, "declaration_line", &alloc, at) == c_decl.line);
        CHECK(field(j, "result", &alloc) == "\"diverged\"");
        CHECK(field(j, "kernels_source", &alloc) == "\"argument\"");
        CHECK(field(j, "kernels_match", &alloc) == "false");
        CHECK(field(j, "program_matches", &alloc) == "true");
    }

    // The edited program as well: the same element, named at the given program's (moved) dispatch.
    const fx::TextPos moved = fx::locate(StringView{fx::kProgramMoved}, StringView{fx::kWaveDispatch});
    REQUIRE(moved.line != wave_at.line);
    r = send(h, ck::kReplayRunCommand, kRecord, {{"kernel_dir", fx::kKernelDir}, {"program", fx::kFile}});
    {
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        const StringView j  = view(r.json);
        const usize      at = j.find(StringView{R"("kind":"divergence")"});
        REQUIRE(at != StringView::npos);
        CHECK(number(j, "buffer", &alloc, at) == 2U);
        CHECK(number(j, "dispatch_line", &alloc, at) == moved.line);
        CHECK(field(j, "program_source", &alloc) == "\"argument\"");
        CHECK(field(j, "program_matches", &alloc) == "false");
    }

    // The edited program alone replays the recorded kernels: the same numbers, so it reproduces.
    r = send(h, ck::kReplayRunCommand, kRecord, {{"program", fx::kFile}});
    CHECK(field(view(r.json), "result", &alloc) == "\"reproduced\"");
    CHECK(field(view(r.json), "program_matches", &alloc) == "false");
    CHECK(reference.runs == 6U);

    // A kernel dispatched twice is read once: @scale writes b, then c.
    root.text(StringView{"twice.ceir"}, StringView{kScaleTwice});
    r = send(h, ck::kReplayRecordCommand, "twice.ceir",
             {{"out", "twice.crpl"},
              {"executor", "device"},
              {"envelope", "exact"},
              {"kernel_dir", fx::kKernelDir},
              {"buffers", fx::kBufferFiles}});
    {
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(number(view(r.json), "kernels", &alloc) == 1U);
        CHECK(number(view(r.json), "written_buffers", &alloc) == 2U);
        CHECK(reference.runs == 7U);
    }
}

TEST_CASE("diag 9a: device records through the replay commands are refused before the executor runs",
          "[ceir][cook][diag][device]")
{
    crd::memory::GrowableTlsfAllocator alloc;
    fx::Authored                       a(&alloc);
    fx::author(a, &alloc);
    const fx::DeviceRoot root("crd_diag9a_device_cmd_refuse", a, &alloc);
    Counted              reference{ck::reference_device_executor(&alloc), 0U};
    const ck::DeviceExecutor device = counted(reference);

    SECTION("no device executor bound: nothing is read or run")
    {
        Host h(root.root(), nullptr);
        refused(record(h, "unbound.crpl", "exact"), DiagStatus::Unavailable, "binds no device executor");
        CHECK(h.cmd.bytes_read.load() == 0U);
        CHECK_FALSE(root.has(StringView{"unbound.crpl"}));

        Host bound(root.root(), &device);
        REQUIRE(record(bound, kRecord, "exact").status == DiagStatus::Ok);
        CHECK(reference.runs == 1U);
        refused(send(h, ck::kReplayRunCommand, kRecord, {}), DiagStatus::Unavailable,
                "the record was made by the device executor and this host binds no device executor");
        CHECK(reference.runs == 1U);
    }

    SECTION("malformed arguments are refused before a file is read")
    {
        Host h(root.root(), &device);
        const char* const out = "bad.crpl";
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out}, {"executor", "device"}, {"kernel_dir", "kernels"}, {"buffers", fx::kBufferFiles}}),
                DiagStatus::BadArgument, "needs 'envelope'");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out}, {"executor", "device"}, {"envelope", "exact"}, {"buffers", fx::kBufferFiles}}),
                DiagStatus::BadArgument, "needs 'envelope'");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out}, {"executor", "device"}, {"envelope", "exact"}, {"kernel_dir", "kernels"}}),
                DiagStatus::BadArgument, "needs 'envelope'");
        for (const char* const bad : {"fuzzy", "ulp:", "ulp:x", "ulp:-1", "ulp:16777217", "exact:0", "ULP:4"})
        {
            INFO(bad);
            refused(record(h, out, bad), DiagStatus::BadArgument, "must be 'exact' or 'ulp:N'");
        }
        refused(send(h, ck::kReplayRecordCommand, fx::kFile, {{"out", out}, {"envelope", "exact"}}),
                DiagStatus::BadArgument, "need executor=device");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out}, {"executor", "host"}, {"kernel_dir", "kernels"}}),
                DiagStatus::BadArgument, "need executor=device");
        for (const char* const run_only : {"entry", "args", "max_events", "seed", "events", "sim_step"})
        {
            INFO(run_only);
            const StringView name{run_only};
            const char*      value = "1";
            if (name == StringView{"entry"})
            {
                value = "main";
            }
            else if (name == StringView{"events"})
            {
                value = "resize:1:1";
            }
            refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                         {{"out", out},
                          {"executor", "device"},
                          {"envelope", "exact"},
                          {"kernel_dir", "kernels"},
                          {"buffers", fx::kBufferFiles},
                          {run_only, value}}),
                    DiagStatus::BadArgument, "is refused with executor=device");
        }
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "../kernels"},
                      {"buffers", fx::kBufferFiles}}),
                DiagStatus::BadArgument, "relative path");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q"}}),
                DiagStatus::BadArgument, "1 to 16 comma-separated");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "buffers/a.bin,,buffers/c.bin"}}),
                DiagStatus::BadArgument, "1 to 16 comma-separated");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile, {{"out", out}, {"executor", "gpu"}}),
                DiagStatus::BadArgument, "'plan', 'host' or 'device'");
        CHECK(h.cmd.bytes_read.load() == 0U);
        CHECK(reference.runs == 0U);
        CHECK_FALSE(root.has(StringView{out}));
    }

    SECTION("missing or malformed input files and a program that is not a device program")
    {
        Host              h(root.root(), &device);
        const char* const out = "bad.crpl";
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "buffers/a.bin,buffers/b.bin,buffers/c.bin"}}),
                DiagStatus::BadArgument, "declares 4 buffers and 'buffers' names 3 files");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "nokernels"},
                      {"buffers", fx::kBufferFiles}}),
                DiagStatus::Failed, "cannot open nokernels/scale.ckir");
        const u8 six[6] = {1U, 2U, 3U, 4U, 5U, 6U};
        root.bytes(StringView{"buffers/six.bin"}, ConstSpan<u8>{six, 6U});
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "buffers/a.bin,buffers/six.bin,buffers/c.bin,buffers/d.bin"}}),
                DiagStatus::BadArgument, "buffers/six.bin holds 6 bytes");
        root.bytes(StringView{"buffers/none.bin"}, ConstSpan<u8>{six, 0U});
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "buffers/a.bin,buffers/none.bin,buffers/c.bin,buffers/d.bin"}}),
                DiagStatus::BadArgument, "buffers/none.bin holds 0 bytes");
        refused(send(h, ck::kReplayRecordCommand, fx::kFile,
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", "buffers/a.bin,buffers/b.bin,buffers/c.bin,buffers/gone.bin"}}),
                DiagStatus::Failed, "cannot open buffers/gone.bin");
        root.text(StringView{"plan.ceir"}, StringView{kPlanProgram});
        refused(send(h, ck::kReplayRecordCommand, "plan.ceir",
                     {{"out", out},
                      {"executor", "device"},
                      {"envelope", "exact"},
                      {"kernel_dir", "kernels"},
                      {"buffers", fx::kBufferFiles}}),
                DiagStatus::Failed, "unsupported: a device program op has no regions: func.func");
        CHECK(reference.runs == 0U);
        CHECK(h.cmd.executions.load() == 0U);
        CHECK_FALSE(root.has(StringView{out}));

        // replay.run: kernel_dir only replays a device record, jobs never does.
        REQUIRE(send(h, ck::kReplayRecordCommand, "plan.ceir", {{"out", "plan.crpl"}}).status == DiagStatus::Ok);
        refused(send(h, ck::kReplayRunCommand, "plan.crpl", {{"kernel_dir", "kernels"}}), DiagStatus::BadArgument,
                "'kernel_dir' replays a device record's kernels; this is a plan record");
        REQUIRE(record(h, kRecord, "exact").status == DiagStatus::Ok);
        CHECK(reference.runs == 1U);
        refused(send(h, ck::kReplayRunCommand, kRecord, {{"jobs", "2"}}), DiagStatus::BadArgument,
                "this is a device record");
        refused(send(h, ck::kReplayRunCommand, kRecord, {{"kernel_dir", "nokernels"}}), DiagStatus::Failed,
                "cannot open nokernels/scale.ckir");
        CHECK(reference.runs == 1U);
    }

    SECTION("an exact record is refused on another adapter, an ulp record replays there")
    {
        Host host(root.root(), &device);
        REQUIRE(record(host, "exact.crpl", "exact").status == DiagStatus::Ok);
        REQUIRE(record(host, "loose.crpl", "ulp:0").status == DiagStatus::Ok);
        CHECK(reference.runs == 2U);

        Counted other{ck::reference_device_executor(&alloc), 0U};
        other.inner.adapter.backend = StringView{"cpu-other"};
        const ck::DeviceExecutor elsewhere = counted(other);
        Host                     h(root.root(), &elsewhere);
        refused(send(h, ck::kReplayRunCommand, "exact.crpl", {}), DiagStatus::Unavailable,
                "incompatible replay: other-adapter: an exact envelope is claimed only on the recording adapter");
        refused(send(h, ck::kReplayRunCommand, "exact.crpl", {{"build", "any"}}), DiagStatus::Unavailable,
                "other-adapter");
        CHECK(other.runs == 0U);

        const DiagResult r = send(h, ck::kReplayRunCommand, "loose.crpl", {});
        INFO(r.json.c_str());
        REQUIRE(r.status == DiagStatus::Ok);
        CHECK(field(view(r.json), "adapter_differs", &alloc) == "true");
        CHECK(field(view(r.json), "recorded_backend", &alloc) == "\"cpu-reference\"");
        CHECK(field(view(r.json), "backend", &alloc) == "\"cpu-other\"");
        CHECK(field(view(r.json), "result", &alloc) == "\"reproduced\"");
        CHECK(other.runs == 1U);
    }
}
