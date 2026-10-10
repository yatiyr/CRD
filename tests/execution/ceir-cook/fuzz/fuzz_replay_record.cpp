// crd-fuzz-ceir-record: decode_record -- the reader of a run record (crd/ceir/cook/replay_record.hpp) -- under the
// bounded fuzz harness (DIAG.10a; docs/design/test-instruments.md). replay.run, replay.prepare, replay.minimize and the
// host and device replays all start from it, so a record it accepts is presented as the evidence of a run.
//
// Each input is decoded as given and, when it starts with a record's magic, again with its header's payload size and
// checksum recomputed, so a mutated payload reaches the payload reader instead of stopping at the checksum.
//
// Oracles:
//   * decoding the same bytes twice gives the same verdict;
//   * the decoder allocates no more than a fixed multiple of its input (8 times its size plus 64 KiB): a count is
//     bounded by the bytes that remain before anything is reserved for it;
//   * an accepted record is one a recorder can write: recorder_could_write() below restates, independently of the
//     decoder, what replay.record, record_host_run and the device recorder produce (each executor's own fields and
//     errors, never a cancelled run, a bounded trace and read stream, each input's state derived from its need the
//     way record_inputs derives it, every kept read of an input the program needs, each kernel's text with its hash);
//   * an accepted record re-encodes to exactly its own bytes, its missing inputs are the ones its states name (plus
//     external results unless they are not needed), and a trace that repeats the record shows no divergence.
// The program blob is carried, not read: the CRDR reader beneath it is crd-fuzz-ceir-binary's.
//
// Seeds are written by hand to the shapes the three recorders write (a fixed build and a small stand-in blob, so the
// committed corpus is the same bytes on every lane): plan records (a passing run with random reads, a truncated
// trace with clock and time-step reads, an unavailable read, a run under a reloading host), host records (a run with
// event reads, a failed run) and device records (an ulp envelope with a written and an unwritten buffer, an exact
// envelope with a device error). Every other seed contradicts a recorder in exactly one way, or damages the bytes
// (truncation, a trailing byte, a wrong checksum or size, an unknown schema, counts past the payload).
#include <crd/fuzz/harness.hpp>

#include <crd/ceir/cook/replay_record.hpp>
#include <crd/ceir/time.hpp> // kBuiltinDomainCount
#include <crd/containers/array.hpp>
#include <crd/containers/hash.hpp>
#include <crd/containers/string.hpp>

#include <cstring>
#include <initializer_list>
#include <utility>

namespace
{
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;
using namespace crd::ceir::cook;
using crd::ceir::exec::ExecError;
using crd::ceir::input::InputKind;
using crd::ceir::plan::RunError;

// The record header: magic, schema, payload size, payload checksum.
constexpr usize kHeader         = 24U;
constexpr usize kSizeOffset     = 8U;
constexpr usize kChecksumOffset = 16U;

// The input slots, in record order.
constexpr u32 kProgram   = 0U;
constexpr u32 kBuild     = 1U;
constexpr u32 kArguments = 2U;
constexpr u32 kRandom    = 3U;
constexpr u32 kClock     = 4U;
constexpr u32 kHostState = 5U;
constexpr u32 kExternal  = 6U;
constexpr u32 kSchedule  = 7U;
constexpr u32 kTolerance = 8U;

constexpr usize kAllocationSlack = 64U * 1024U;
constexpr usize kAllocationRatio = 8U;

void put_le(u8* at, u64 value, usize bytes) noexcept
{
    for (usize i = 0U; i < bytes; ++i)
    {
        at[i] = static_cast<u8>((value >> (8U * i)) & 0xFFU);
    }
}

// Recompute the header's payload size and checksum over whatever follows the header.
void seal(Array<u8>& bytes) noexcept
{
    if (bytes.size() < kHeader)
    {
        return;
    }
    const usize payload = bytes.size() - kHeader;
    put_le(bytes.data() + kSizeOffset, payload, 8U);
    put_le(bytes.data() + kChecksumOffset, crd::containers::fnv1a_64(bytes.data() + kHeader, payload), 8U);
}

bool same_text(const String& a, const String& b) noexcept
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

// The input a host input read of `kind` is.
u32 input_of(InputKind kind) noexcept
{
    switch (kind)
    {
    case InputKind::Random: return kRandom;
    case InputKind::Clock:
    case InputKind::TimeStep: return kClock;
    case InputKind::Event: return kHostState;
    }
    return kHostState;
}

// The recorders' guarantees, restated without calling the decoder.
bool recorder_could_write(const ReplayRecord& r) noexcept
{
    const bool host   = r.executor == ReplayExecutorKind::Host;
    const bool device = r.executor == ReplayExecutorKind::Device;
    if (r.schema != kReplayRecordSchema || static_cast<u32>(r.executor) > static_cast<u32>(ReplayExecutorKind::Device))
    {
        return false;
    }
    // A host record carries the provider's schedule settings; no other record has any.
    if (host ? (r.host_jobs == 0U || r.host_jobs > kReplayMaxHostJobs || r.host_sub_fuel == 0U)
             : (r.host_jobs != 0U || r.host_sub_fuel != 0U))
    {
        return false;
    }
    const String* const strings[] = {&r.build.version, &r.build.platform, &r.build.compiler, &r.build.arch,
                                     &r.build.config,  &r.program_path,   &r.entry};
    for (const String* const s : strings)
    {
        if (s->size() > kReplayMaxStringBytes)
        {
            return false;
        }
    }
    if ((r.generation != 0U && r.asset == 0U) || r.program.size() > kReplayMaxProgramBytes ||
        r.args.size() > kReplayMaxArgs || r.results.size() > kReplayMaxValues || r.cells.size() > kReplayMaxValues)
    {
        return false;
    }

    // The bounded trace: the first max_events events kept, every one counted.
    const u64 kept = r.events_total < r.max_events ? r.events_total : r.max_events;
    if (r.max_events == 0U || r.max_events > kReplayMaxEvents || r.events.size() != kept)
    {
        return false;
    }
    for (const ReplayEvent& e : r.events)
    {
        if (e.values > kReplayEventValues)
        {
            return false;
        }
        for (u32 k = e.values; k < kReplayEventValues; ++k)
        {
            if (e.value[k] != 0)
            {
                return false;
            }
        }
    }

    // Each executor reports its own error, and a cancelled run is never written.
    if (static_cast<u32>(r.error) > static_cast<u32>(RunError::InputUnavailable) ||
        static_cast<u32>(r.host_error) > static_cast<u32>(ExecError::InputUnavailable) ||
        (host && r.error != RunError::None) || (!host && r.host_error != ExecError::None) ||
        r.error == RunError::Cancelled || r.host_error == ExecError::Cancelled)
    {
        return false;
    }

    // The read stream: the first kReplayMaxInputReads reads kept, a read the host did not answer holding no value, a
    // time read naming a built-in domain.
    const u64 kept_reads = r.input_reads_total < kReplayMaxInputReads ? r.input_reads_total : kReplayMaxInputReads;
    if (r.input_reads.size() != kept_reads)
    {
        return false;
    }
    for (const ReplayInputRead& read : r.input_reads)
    {
        const bool time_read = read.kind == InputKind::Clock || read.kind == InputKind::TimeStep;
        if (static_cast<u32>(read.kind) > static_cast<u32>(InputKind::Event) || (!read.delivered && read.value != 0) ||
            (time_read && read.channel >= crd::ceir::time::kBuiltinDomainCount))
        {
            return false;
        }
    }

    // record_inputs: a state is derived from the input's need; the identity inputs are always recorded, external
    // results never; the executor that holds the schedule or the tolerance records it, no other; a seam input is
    // recorded only when surely needed and every read is held.
    const bool held = r.input_reads_total <= kReplayMaxInputReads;
    for (u32 i = 0U; i < kReplayInputs; ++i)
    {
        const ReplayInput& in       = r.inputs[i];
        const bool         identity = i == kProgram || i == kBuild || i == kArguments;
        const bool         seam     = i == kRandom || i == kClock || i == kHostState;
        const bool         owner    = (i == kSchedule && host) || (i == kTolerance && device);
        if (static_cast<u32>(in.need) > static_cast<u32>(ReplayNeed::Unknown) ||
            static_cast<u32>(in.state) > static_cast<u32>(ReplayInputState::Missing))
        {
            return false;
        }
        bool expected_recorded = false;
        if (identity)
        {
            expected_recorded = true;
            if (in.need != ReplayNeed::Yes)
            {
                return false;
            }
        }
        else if (in.need == ReplayNeed::No)
        {
            if (in.state != ReplayInputState::NotNeeded)
            {
                return false;
            }
            continue;
        }
        else if (owner)
        {
            expected_recorded = true;
        }
        else if (seam && in.need == ReplayNeed::Yes && held)
        {
            // Recorded or missing: the recorder also stores it missing when an op reads it outside the seam.
            if (in.state == ReplayInputState::NotNeeded)
            {
                return false;
            }
            continue;
        }
        if (in.state != (expected_recorded ? ReplayInputState::Recorded : ReplayInputState::Missing))
        {
            return false;
        }
    }
    for (const ReplayInputRead& read : r.input_reads)
    {
        if (r.inputs[input_of(read.kind)].need == ReplayNeed::No)
        {
            return false;
        }
    }

    // The device section: a device record's adapter, kernels (text with its hash), buffers and envelope, and nothing
    // of a plan or host run; any other record has none of it.
    const DeviceEnvelope& env = r.device_envelope;
    if (static_cast<u32>(env.kind) > static_cast<u32>(DeviceEnvelopeKind::Ulp) || env.ulps > kReplayMaxUlps ||
        (env.kind == DeviceEnvelopeKind::Exact && env.ulps != 0U) ||
        r.device_kernels.size() > kReplayMaxDeviceKernels || r.device_buffers.size() > kReplayMaxDeviceBuffers ||
        r.device_adapter.backend.size() > kReplayMaxStringBytes || r.device_adapter.name.size() > kReplayMaxStringBytes)
    {
        return false;
    }
    if (!device)
    {
        return r.device_adapter.backend.empty() && r.device_adapter.name.empty() && r.device_adapter.vendor == 0U &&
               r.device_adapter.device == 0U && r.device_adapter.driver == 0U && r.device_adapter.api == 0U &&
               env.kind == DeviceEnvelopeKind::Exact && r.device_error == 0U && r.device_kernels.empty() &&
               r.device_buffers.empty();
    }
    if (!r.entry.empty() || !r.args.empty() || r.max_events != 1U || r.events_total != 0U || !r.results.empty() ||
        !r.cells.empty() || r.input_reads_total != 0U || r.device_adapter.backend.empty() ||
        r.device_kernels.empty() || r.device_buffers.empty())
    {
        return false;
    }
    for (const DeviceKernelRecord& k : r.device_kernels)
    {
        if (k.symbol.empty() || k.symbol.size() > kReplayMaxStringBytes || k.ckir.size() > kReplayMaxKernelBytes ||
            k.hash != crd::containers::fnv1a_64(k.ckir.data(), k.ckir.size()))
        {
            return false;
        }
    }
    u64 words = 0U;
    for (const DeviceBufferRecord& b : r.device_buffers)
    {
        if (static_cast<u32>(b.element) > static_cast<u32>(DeviceElement::U32) || b.initial.empty() ||
            b.output.size() != (b.written ? b.initial.size() : 0U))
        {
            return false;
        }
        words += b.initial.size();
    }
    return words <= kReplayMaxDeviceWords;
}

// What a replay reports missing for `r`: the inputs stored missing, and external results unless not needed.
void expected_missing(const ReplayRecord& r, String& out)
{
    for (u32 i = 0U; i < kReplayInputs; ++i)
    {
        const ReplayInput& in       = r.inputs[i];
        const bool         external = i == kExternal &&
                              (in.need != ReplayNeed::No || in.state != ReplayInputState::NotNeeded);
        if (in.state == ReplayInputState::Missing || external)
        {
            if (!out.empty())
            {
                out.push_back(',');
            }
            out.append(replay_input_name(i));
        }
    }
}

// A trace that repeats the record exactly.
void trace_of(const ReplayRecord& r, ReplayTrace& t)
{
    for (const ReplayEvent& e : r.events)
    {
        t.events.push_back(e);
    }
    t.events_total = r.events_total;
    t.error        = r.error;
    t.host_error   = r.host_error;
    t.fault_op     = r.fault_op;
    for (const i64 v : r.results)
    {
        t.results.push_back(v);
    }
    for (const i64 v : r.cells)
    {
        t.cells.push_back(v);
    }
    for (const ReplayInputRead& read : r.input_reads)
    {
        t.input_reads.push_back(read);
    }
    t.input_reads_total = r.input_reads_total;
}

void check(const u8* data, usize size)
{
    const ConstSpan<u8> input(data, size);
    const usize         allowed = kAllocationRatio * size + kAllocationSlack;

    crd::fuzz::BudgetAllocator other_root;
    ReplayRecord               other(&other_root);
    const RecordError          second = decode_record(input, other);

    crd::fuzz::BudgetAllocator root;
    ReplayRecord               record(&root);
    const RecordError          first = decode_record(input, record);
    CRD_FUZZ_REQUIRE(first == second);
    CRD_FUZZ_REQUIRE(root.peak_bytes() <= allowed && other_root.peak_bytes() <= allowed);
    if (first != RecordError::Ok)
    {
        CRD_FUZZ_REQUIRE(record_error_name(first) != StringView{"?"});
        return;
    }
    CRD_FUZZ_REQUIRE(recorder_could_write(record));

    Array<u8> again(&root);
    encode_record(record, again);
    CRD_FUZZ_REQUIRE(crd::fuzz::bytes_equal(again.data(), again.size(), data, size));

    String missing(&root);
    String expected(&root);
    expected_missing(record, expected);
    CRD_FUZZ_REQUIRE(record_missing_inputs(record, missing) == expected.empty());
    CRD_FUZZ_REQUIRE(same_text(missing, expected));

    ReplayTrace repeat(&root);
    trace_of(record, repeat);
    CRD_FUZZ_REQUIRE(first_divergence(record, repeat).kind == DivergenceKind::None);
}
} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > crd::fuzz::kMaxInputBytes)
    {
        return 0;
    }
    check(data, size);
    if (size >= kHeader && is_replay_record(ConstSpan<u8>(data, size)))
    {
        crd::fuzz::BudgetAllocator root;
        Array<u8>                  sealed(&root);
        sealed.reserve(size);
        for (usize i = 0U; i < size; ++i)
        {
            sealed.push_back(data[i]);
        }
        seal(sealed);
        check(sealed.data(), sealed.size());
    }
    return 0;
}

namespace
{
void set_text(String& s, const char* text)
{
    s.clear();
    s.append(StringView{text});
}

void set_input(ReplayRecord& r, u32 i, ReplayNeed need, ReplayInputState state)
{
    r.inputs[i].need  = need;
    r.inputs[i].state = state;
}

ReplayEvent event(u64 op, u32 depth, u32 values, i64 first)
{
    ReplayEvent e;
    e.op     = op;
    e.depth  = depth;
    e.values = values;
    for (u32 k = 0U; k < values; ++k)
    {
        e.value[k] = first + static_cast<i64>(k);
    }
    return e;
}

void add_read(ReplayRecord& r, InputKind kind, u32 channel, bool delivered, i64 value)
{
    ReplayInputRead read;
    read.kind      = kind;
    read.channel   = channel;
    read.delivered = delivered;
    read.value     = delivered ? value : 0;
    r.input_reads.push_back(read);
    r.input_reads_total = r.input_reads.size();
}

// What every recorder writes first: a fixed build, the program as a small stand-in blob, the identity inputs
// recorded and every other input not needed.
void base(ReplayRecord& r, ReplayExecutorKind executor)
{
    set_text(r.build.version, "0.0.0-seed");
    set_text(r.build.platform, "seed-platform");
    set_text(r.build.compiler, "seed-compiler");
    set_text(r.build.arch, "x64");
    set_text(r.build.config, "debug");
    r.build.asserts  = true;
    r.build.executor = kReplayExecutor;
    r.executor       = executor;
    set_text(r.program_path, "seed/demo.ceir");
    r.content_hash = 0x5EED'0000'C0DE'0001ULL;
    const char blob[] = "CRDR seed stand-in program blob";
    for (usize i = 0U; i + 1U < sizeof(blob); ++i)
    {
        r.program.push_back(static_cast<u8>(blob[i]));
    }
    for (u32 i = 0U; i < kReplayInputs; ++i)
    {
        set_input(r, i, ReplayNeed::No, ReplayInputState::NotNeeded);
    }
    for (const u32 i : {kProgram, kBuild, kArguments})
    {
        set_input(r, i, ReplayNeed::Yes, ReplayInputState::Recorded);
    }
}

// replay.record of a passing run that drew two random values.
ReplayRecord plan_passing()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Plan);
    set_text(r.entry, "main");
    r.args.push_back(1);
    r.args.push_back(-2);
    set_input(r, kRandom, ReplayNeed::Yes, ReplayInputState::Recorded);
    r.max_events   = kReplayDefaultMaxEvents;
    r.events_total = 3U;
    r.events.push_back(event(11U, 0U, 1U, 7));
    r.events.push_back(event(12U, 1U, 4U, -3));
    r.events.push_back(event(13U, 0U, 0U, 0));
    add_read(r, InputKind::Random, 0U, true, 0x1234);
    add_read(r, InputKind::Random, 2U, true, -5);
    r.results.push_back(3);
    r.cells.push_back(42);
    return r;
}

// A trace bounded at two events of five, with clock and time-step reads, failing on its step budget; the schedule
// and an opaque op's possible external results stored missing.
ReplayRecord plan_truncated()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Plan);
    set_text(r.entry, "tick");
    set_input(r, kClock, ReplayNeed::Yes, ReplayInputState::Recorded);
    set_input(r, kSchedule, ReplayNeed::Yes, ReplayInputState::Missing);
    set_input(r, kExternal, ReplayNeed::Unknown, ReplayInputState::Missing);
    r.max_events   = 2U;
    r.events_total = 5U;
    r.events.push_back(event(21U, 0U, 1U, 100));
    r.events.push_back(event(22U, 0U, 2U, 200));
    add_read(r, InputKind::Clock, 0U, true, 1'000'000);
    add_read(r, InputKind::TimeStep, 3U, true, 16);
    r.error    = RunError::FuelExhausted;
    r.fault_op = 22U;
    return r;
}

// A random read the host could not answer: the run failed there.
ReplayRecord plan_input_unavailable()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Plan);
    set_text(r.entry, "main");
    set_input(r, kRandom, ReplayNeed::Yes, ReplayInputState::Recorded);
    set_input(r, kHostState, ReplayNeed::Unknown, ReplayInputState::Missing);
    r.events_total = 1U;
    r.events.push_back(event(31U, 0U, 0U, 0));
    add_read(r, InputKind::Random, 1U, false, 0);
    r.error    = RunError::InputUnavailable;
    r.fault_op = 31U;
    return r;
}

// A run under a reloading host: its asset and generation named.
ReplayRecord plan_reloaded()
{
    ReplayRecord r = plan_passing();
    r.asset        = 9U;
    r.generation   = 2U;
    return r;
}

// record_host_run with input.event reads: the schedule recorded by the host executor.
ReplayRecord host_passing()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Host);
    r.host_jobs     = 4U;
    r.host_sub_fuel = 1024U;
    set_text(r.entry, "main");
    r.args.push_back(5);
    set_input(r, kHostState, ReplayNeed::Yes, ReplayInputState::Recorded);
    set_input(r, kSchedule, ReplayNeed::Yes, ReplayInputState::Recorded);
    r.max_events   = 16U;
    r.events_total = 2U;
    r.events.push_back(event(41U, 0U, 1U, 9));
    r.events.push_back(event(42U, 1U, 0U, 0));
    add_read(r, InputKind::Event, 0U, true, 0x0001'0002'0300'0401LL);
    r.results.push_back(9);
    return r;
}

// A failed host run: its own error and the op it blamed; the schedule not needed.
ReplayRecord host_failed()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Host);
    r.host_jobs     = 1U;
    r.host_sub_fuel = 1U;
    set_text(r.entry, "main");
    r.events_total = 1U;
    r.events.push_back(event(51U, 0U, 0U, 0));
    r.host_error = ExecError::BadForStep;
    r.fault_op   = 51U;
    return r;
}

DeviceKernelRecord kernel(const char* symbol, const char* text)
{
    DeviceKernelRecord k(crd::memory::default_allocator());
    set_text(k.symbol, symbol);
    set_text(k.ckir, text);
    k.hash = crd::containers::fnv1a_64(k.ckir.data(), k.ckir.size());
    return k;
}

// The device recorder: an ulp envelope, a written f32 buffer and an unwritten u32 buffer (the last field of the
// record, so its word count sits at a known distance from the end).
ReplayRecord device_ulp()
{
    ReplayRecord r(crd::memory::default_allocator());
    base(r, ReplayExecutorKind::Device);
    set_input(r, kTolerance, ReplayNeed::Yes, ReplayInputState::Recorded);
    r.max_events = 1U;
    set_text(r.device_adapter.backend, "cpu-reference");
    set_text(r.device_adapter.name, "seed adapter");
    r.device_adapter.vendor = 0x10DEU;
    r.device_adapter.device = 0x2204U;
    r.device_adapter.driver = 0x0002'0001ULL;
    r.device_adapter.api    = 0x0040'3000U;
    r.device_envelope.kind  = DeviceEnvelopeKind::Ulp;
    r.device_envelope.ulps  = 2U;
    r.device_kernels.push_back(kernel("@wave", "kernel @wave(%x: f32) { seed }"));
    DeviceBufferRecord written(crd::memory::default_allocator());
    written.element = DeviceElement::F32;
    written.written = true;
    for (u32 i = 0U; i < 4U; ++i)
    {
        written.initial.push_back(0x3F80'0000U + i);
        written.output.push_back(0x4000'0000U + i);
    }
    r.device_buffers.push_back(std::move(written));
    DeviceBufferRecord constants(crd::memory::default_allocator());
    constants.element = DeviceElement::U32;
    constants.initial.push_back(7U);
    constants.initial.push_back(8U);
    r.device_buffers.push_back(std::move(constants));
    return r;
}

// An exact envelope and an error the device executor reported at a dispatch.
ReplayRecord device_exact_error()
{
    ReplayRecord r         = device_ulp();
    r.device_envelope.kind = DeviceEnvelopeKind::Exact;
    r.device_envelope.ulps = 0U;
    r.device_error         = 3U;
    r.fault_op             = 61U;
    return r;
}

ReplayRecord copy_of(const ReplayRecord& r)
{
    Array<u8> bytes(crd::memory::default_allocator());
    encode_record(r, bytes);
    ReplayRecord copy(crd::memory::default_allocator());
    (void)decode_record({bytes.data(), bytes.size()}, copy);
    return copy;
}

Array<u8> bytes_of(const ReplayRecord& r)
{
    Array<u8> bytes(crd::memory::default_allocator());
    encode_record(r, bytes);
    return bytes;
}

void add_record(crd::fuzz::SeedSink& sink, const char* name, const ReplayRecord& r)
{
    const Array<u8> bytes = bytes_of(r);
    sink.add(name, bytes.data(), bytes.size());
}

// The bytes of `r` with the u32 at `offset` set to `value` and the header sealed again.
void add_count(crd::fuzz::SeedSink& sink, const char* name, const ReplayRecord& r, usize offset, u32 value)
{
    Array<u8> bytes = bytes_of(r);
    if (offset + 4U <= bytes.size())
    {
        put_le(bytes.data() + offset, value, 4U);
        seal(bytes);
        sink.add(name, bytes.data(), bytes.size());
    }
}

usize string_bytes(const String& s)
{
    return 4U + s.size();
}

// Where the kept-event count of `r` sits: after the build, the executor and schedule, the program, the entry and its
// arguments, the input states, max_events and events_total (replay_record.cpp: encode_record).
usize events_count_offset(const ReplayRecord& r)
{
    usize at = kHeader;
    for (const String* const s : {&r.build.version, &r.build.platform, &r.build.compiler, &r.build.arch,
                                  &r.build.config})
    {
        at += string_bytes(*s);
    }
    at += 1U + 4U + 1U + 4U + 8U + string_bytes(r.program_path) + 3U * 8U + 4U + r.program.size();
    at += string_bytes(r.entry) + 4U + 8U * r.args.size() + 2U * kReplayInputs + 4U + 8U;
    return at;
}

// Where the kept-read count of `r` sits: after the events, the outcome, the results and cells, and the read total.
usize reads_count_offset(const ReplayRecord& r)
{
    usize at = events_count_offset(r) + 4U;
    for (const ReplayEvent& e : r.events)
    {
        at += 16U + 8U * e.values;
    }
    return at + 1U + 1U + 8U + 4U + 8U * r.results.size() + 4U + 8U * r.cells.size() + 8U;
}

bool record_accepted(const u8* data, usize size)
{
    ReplayRecord r(crd::memory::default_allocator());
    return decode_record(ConstSpan<u8>(data, size), r) == RecordError::Ok;
}

// The records a recorder writes; every other seed must be refused.
constexpr const char* kAcceptedSeeds[] = {"plan-passing",     "plan-truncated-trace", "plan-input-unavailable",
                                          "plan-reloaded",    "host-passing",         "host-failed",
                                          "device-ulp",       "device-exact-error"};
} // namespace

void crd_fuzz_seeds(crd::fuzz::SeedSink& out)
{
    crd::fuzz::VerdictSink sink(out, &record_accepted, kAcceptedSeeds,
                                sizeof(kAcceptedSeeds) / sizeof(kAcceptedSeeds[0]));
    const ReplayRecord plan   = plan_passing();
    const ReplayRecord bound  = plan_truncated();
    const ReplayRecord host   = host_passing();
    const ReplayRecord device = device_ulp();

    add_record(sink, "plan-passing", plan);
    add_record(sink, "plan-truncated-trace", bound);
    add_record(sink, "plan-input-unavailable", plan_input_unavailable());
    add_record(sink, "plan-reloaded", plan_reloaded());
    add_record(sink, "host-passing", host);
    add_record(sink, "host-failed", host_failed());
    add_record(sink, "device-ulp", device);
    add_record(sink, "device-exact-error", device_exact_error());

    // Input states no recorder writes; each must be refused.
    struct StateForgery
    {
        const char*         name;
        const ReplayRecord* from;
        u32                 input;
        ReplayNeed          need;
        ReplayInputState    state;
    };
    const StateForgery states[] = {
        {"forged-unneeded-input-recorded", &plan, kHostState, ReplayNeed::No, ReplayInputState::Recorded},
        {"forged-unneeded-input-missing", &plan, kHostState, ReplayNeed::No, ReplayInputState::Missing},
        {"forged-needed-input-not-needed", &plan, kClock, ReplayNeed::Yes, ReplayInputState::NotNeeded},
        {"forged-program-missing", &plan, kProgram, ReplayNeed::Yes, ReplayInputState::Missing},
        {"forged-build-not-needed", &plan, kBuild, ReplayNeed::No, ReplayInputState::NotNeeded},
        {"forged-arguments-unknown", &plan, kArguments, ReplayNeed::Unknown, ReplayInputState::Recorded},
        {"forged-external-results-recorded", &plan, kExternal, ReplayNeed::Yes, ReplayInputState::Recorded},
        {"forged-plan-schedule-recorded", &plan, kSchedule, ReplayNeed::Yes, ReplayInputState::Recorded},
        {"forged-host-tolerance-recorded", &host, kTolerance, ReplayNeed::Yes, ReplayInputState::Recorded},
        {"forged-host-schedule-missing", &host, kSchedule, ReplayNeed::Yes, ReplayInputState::Missing},
        {"forged-device-tolerance-missing", &device, kTolerance, ReplayNeed::Unknown, ReplayInputState::Missing},
        {"forged-seam-unknown-recorded", &plan, kRandom, ReplayNeed::Unknown, ReplayInputState::Recorded},
        {"forged-read-of-unneeded-input", &plan, kRandom, ReplayNeed::No, ReplayInputState::NotNeeded},
    };
    for (const StateForgery& f : states)
    {
        ReplayRecord r = copy_of(*f.from);
        set_input(r, f.input, f.need, f.state);
        add_record(sink, f.name, r);
    }

    // Other fields that contradict a recorder in exactly one way.
    {
        ReplayRecord r = copy_of(plan);
        r.error        = RunError::Cancelled;
        add_record(sink, "forged-plan-cancelled", r);
    }
    {
        ReplayRecord r = copy_of(host);
        r.host_error   = ExecError::Cancelled;
        add_record(sink, "forged-host-cancelled", r);
    }
    {
        ReplayRecord r = copy_of(host);
        r.error        = RunError::FuelExhausted;
        add_record(sink, "forged-host-plan-error", r);
    }
    {
        ReplayRecord r = copy_of(plan);
        r.host_error   = ExecError::FuelExhausted;
        add_record(sink, "forged-plan-host-error", r);
    }
    {
        ReplayRecord r = copy_of(device);
        r.device_kernels[0].hash ^= 1U;
        add_record(sink, "forged-kernel-hash", r);
    }
    {
        ReplayRecord r = copy_of(plan);
        r.device_kernels.push_back(kernel("@wave", "kernel @wave(%x: f32) { seed }"));
        add_record(sink, "forged-plan-with-kernel", r);
    }
    {
        ReplayRecord       r = copy_of(plan);
        DeviceBufferRecord b(crd::memory::default_allocator());
        b.element = DeviceElement::U32;
        b.initial.push_back(1U);
        r.device_buffers.push_back(std::move(b));
        add_record(sink, "forged-plan-with-buffer", r);
    }
    {
        ReplayRecord r = copy_of(plan);
        set_text(r.device_adapter.backend, "vulkan");
        add_record(sink, "forged-plan-with-adapter", r);
    }
    {
        ReplayRecord r = copy_of(plan);
        r.asset        = 0U;
        r.generation   = 3U;
        add_record(sink, "forged-generation-without-asset", r);
    }
    {
        ReplayRecord r = copy_of(host);
        r.host_jobs    = 0U;
        add_record(sink, "forged-host-without-jobs", r);
    }
    {
        ReplayRecord r  = copy_of(plan);
        r.host_jobs     = 4U;
        r.host_sub_fuel = 64U;
        add_record(sink, "forged-plan-with-schedule-settings", r);
    }
    {
        ReplayRecord r = copy_of(device);
        set_text(r.entry, "main");
        add_record(sink, "forged-device-with-entry", r);
    }
    {
        ReplayRecord r         = copy_of(device);
        r.device_envelope.kind = DeviceEnvelopeKind::Exact;
        add_record(sink, "forged-exact-envelope-with-ulps", r);
    }
    {
        ReplayRecord r = copy_of(device);
        r.device_buffers[1].written = true;
        add_record(sink, "forged-unwritten-outputs", r);
    }
    {
        ReplayRecord r             = copy_of(plan);
        r.input_reads[0].delivered = false;
        add_record(sink, "forged-undelivered-value", r);
    }
    {
        ReplayRecord r = copy_of(bound);
        r.input_reads[1].channel = crd::ceir::time::kBuiltinDomainCount;
        add_record(sink, "forged-unknown-time-domain", r);
    }
    {
        ReplayRecord r   = copy_of(bound);
        r.events_total   = 1U;
        add_record(sink, "forged-trace-longer-than-total", r);
    }

    // Structural damage on the plan record's bytes.
    const Array<u8> bytes = bytes_of(plan);
    {
        Array<u8> b = bytes;
        b.push_back(0U);
        sink.add("trailing-byte-unsealed", b.data(), b.size());
        seal(b);
        sink.add("trailing-byte-sealed", b.data(), b.size());
    }
    {
        Array<u8> b = bytes;
        b[bytes.size() - 1U] ^= 0x40U;
        sink.add("bad-checksum", b.data(), b.size());
    }
    {
        Array<u8> b = bytes;
        put_le(b.data() + 4U, kReplayRecordSchema - 1U, 4U);
        sink.add("older-schema", b.data(), b.size());
    }
    // Cut short at several points: as given, and with the header sealed over what is left (a cut after the header
    // then reaches the payload reader).
    const usize cuts[]   = {3U, 12U, kHeader, kHeader + 2U, bytes.size() / 2U, bytes.size() - 1U};
    const char* names[]  = {"cut-in-magic",    "cut-in-header",   "cut-after-header",
                            "cut-in-first-string", "cut-mid-payload", "cut-last-byte"};
    const char* sealed[] = {nullptr,                    nullptr,                  "cut-after-header-sealed",
                            "cut-in-first-string-sealed", "cut-mid-payload-sealed", "cut-last-byte-sealed"};
    for (usize i = 0U; i < sizeof(cuts) / sizeof(cuts[0]); ++i)
    {
        sink.add(names[i], bytes.data(), cuts[i]);
        if (sealed[i] != nullptr)
        {
            Array<u8> b(crd::memory::default_allocator());
            for (usize k = 0U; k < cuts[i]; ++k)
            {
                b.push_back(bytes[k]);
            }
            seal(b);
            sink.add(sealed[i], b.data(), b.size());
        }
    }

    // Counts the payload cannot hold, sealed so they reach the payload reader: each must be refused without an
    // allocation sized by the count.
    {
        ReplayRecord r = copy_of(plan);
        r.max_events   = kReplayMaxEvents;
        r.events_total = kReplayMaxEvents;
        add_count(sink, "events-count-past-payload", r, events_count_offset(r), kReplayMaxEvents);
    }
    {
        ReplayRecord r      = copy_of(plan);
        r.input_reads_total = kReplayMaxInputReads;
        add_count(sink, "reads-count-past-payload", r, reads_count_offset(r), kReplayMaxInputReads);
    }
    {
        // The unwritten buffer is the last field: its output count (0), its two words, then its initial count.
        const Array<u8> d = bytes_of(device);
        add_count(sink, "words-count-past-payload", device, d.size() - 4U - 2U * 4U - 4U,
                  static_cast<u32>(kReplayMaxDeviceWords - 4U));
    }
    sink.add_text("empty", "");
    sink.add_text("garbage", "this is not a run record");
}
