#include <crd/ceir/cook/replay_record.hpp>

#include "bounded_file.hpp"

#include <crd/ceir/context.hpp>
#include <crd/ceir/provenance.hpp>
#include <crd/containers/hash.hpp> // fnv1a_64
#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>

#include <initializer_list>
#include <utility>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;

// 'C' 'R' 'P' 'L' as a little-endian u32.
constexpr crd::u32 kRecordMagic = 0x4C505243U;
// magic, schema, payload size, payload checksum.
constexpr crd::usize kHeaderBytes = 4U + 4U + 8U + 8U;

constexpr crd::u64 kNoPending = ~crd::u64{0U};

constexpr cont::StringView kInputNames[kReplayInputs] = {
    "program", "build", "entry-arguments", "random", "clock", "host-state", "external-results", "schedule",
    "device-tolerance",
};

// ---- encoding -------------------------------------------------------------------------------------------------------

void put_u8(cont::Array<crd::u8>& out, crd::u8 v)
{
    out.push_back(v);
}

void put_u32(cont::Array<crd::u8>& out, crd::u32 v)
{
    for (crd::u32 i = 0U; i < 4U; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put_u64(cont::Array<crd::u8>& out, crd::u64 v)
{
    for (crd::u32 i = 0U; i < 8U; ++i)
    {
        out.push_back(static_cast<crd::u8>((v >> (8U * i)) & 0xFFU));
    }
}

void put_i64(cont::Array<crd::u8>& out, crd::i64 v)
{
    put_u64(out, static_cast<crd::u64>(v));
}

void put_bytes(cont::Array<crd::u8>& out, const crd::u8* data, crd::usize n)
{
    put_u32(out, static_cast<crd::u32>(n));
    for (crd::usize i = 0U; i < n; ++i)
    {
        out.push_back(data[i]);
    }
}

void put_str(cont::Array<crd::u8>& out, const cont::String& s)
{
    put_bytes(out, reinterpret_cast<const crd::u8*>(s.data()), s.size());
}

void put_values(cont::Array<crd::u8>& out, const cont::Array<crd::i64>& values)
{
    put_u32(out, static_cast<crd::u32>(values.size()));
    for (const crd::i64 v : values)
    {
        put_i64(out, v);
    }
}

// A bounds-checked little-endian reader: a read past the end fails and every later read fails too.
class Reader
{
public:
    Reader(const crd::u8* data, crd::usize size) noexcept : m_data(data), m_size(size) {}

    [[nodiscard]] bool u8(crd::u8& v) noexcept
    {
        if (!take(1U))
        {
            return false;
        }
        v = m_data[m_pos - 1U];
        return true;
    }

    [[nodiscard]] bool u32(crd::u32& v) noexcept
    {
        if (!take(4U))
        {
            return false;
        }
        v = 0U;
        for (crd::u32 i = 0U; i < 4U; ++i)
        {
            v |= static_cast<crd::u32>(m_data[m_pos - 4U + i]) << (8U * i);
        }
        return true;
    }

    [[nodiscard]] bool u64(crd::u64& v) noexcept
    {
        if (!take(8U))
        {
            return false;
        }
        v = 0U;
        for (crd::u32 i = 0U; i < 8U; ++i)
        {
            v |= static_cast<crd::u64>(m_data[m_pos - 8U + i]) << (8U * i);
        }
        return true;
    }

    [[nodiscard]] bool i64(crd::i64& v) noexcept
    {
        crd::u64 u = 0U;
        if (!u64(u))
        {
            return false;
        }
        v = static_cast<crd::i64>(u);
        return true;
    }

    // A length-prefixed byte run of at most `max` bytes; `bounded` is false when the length exceeds `max`.
    [[nodiscard]] bool bytes(crd::u64 max, const crd::u8*& data, crd::usize& n, bool& bounded) noexcept
    {
        crd::u32 len = 0U;
        if (!u32(len))
        {
            return false;
        }
        bounded = len <= max;
        if (!bounded)
        {
            return true;
        }
        if (!take(len))
        {
            return false;
        }
        data = m_data + (m_pos - len);
        n    = len;
        return true;
    }

    [[nodiscard]] bool done() const noexcept { return m_pos == m_size; }
    [[nodiscard]] bool failed() const noexcept { return m_failed; }

private:
    [[nodiscard]] bool take(crd::usize n) noexcept
    {
        if (m_failed || n > m_size - m_pos)
        {
            m_failed = true;
            return false;
        }
        m_pos += n;
        return true;
    }

    const crd::u8* m_data;
    crd::usize     m_size;
    crd::usize     m_pos    = 0U;
    bool           m_failed = false;
};

// Read one bounded string; Malformed when longer than kReplayMaxStringBytes.
[[nodiscard]] RecordError read_str(Reader& r, cont::String& out)
{
    const crd::u8* data    = nullptr;
    crd::usize     n       = 0U;
    bool           bounded = true;
    if (!r.bytes(kReplayMaxStringBytes, data, n, bounded))
    {
        return RecordError::Truncated;
    }
    if (!bounded)
    {
        return RecordError::Malformed;
    }
    out.clear();
    out.append(cont::StringView{reinterpret_cast<const char*>(data), n});
    return RecordError::Ok;
}

[[nodiscard]] RecordError read_values(Reader& r, crd::u32 max, cont::Array<crd::i64>& out)
{
    crd::u32 n = 0U;
    if (!r.u32(n))
    {
        return RecordError::Truncated;
    }
    if (n > max)
    {
        return RecordError::Malformed;
    }
    out.clear();
    for (crd::u32 i = 0U; i < n; ++i)
    {
        crd::i64 v = 0;
        if (!r.i64(v))
        {
            return RecordError::Truncated;
        }
        out.push_back(v);
    }
    return RecordError::Ok;
}

[[nodiscard]] RecordError read_payload(Reader& r, ReplayRecord& out)
{
    for (cont::String* const field :
         {&out.build.version, &out.build.platform, &out.build.compiler, &out.build.arch, &out.build.config})
    {
        if (const RecordError e = read_str(r, *field); e != RecordError::Ok)
        {
            return e;
        }
    }
    crd::u8 asserts = 0U;
    crd::u8 kind    = 0U;
    if (!r.u8(asserts) || !r.u32(out.build.executor) || !r.u8(kind) || !r.u32(out.host_jobs) ||
        !r.u64(out.host_sub_fuel))
    {
        return RecordError::Truncated;
    }
    if (asserts > 1U || kind > static_cast<crd::u8>(ReplayExecutorKind::Host))
    {
        return RecordError::Malformed;
    }
    out.build.asserts = asserts != 0U;
    out.executor      = static_cast<ReplayExecutorKind>(kind);
    const bool host   = out.executor == ReplayExecutorKind::Host;
    if (host ? (out.host_jobs == 0U || out.host_jobs > kReplayMaxHostJobs || out.host_sub_fuel == 0U)
             : (out.host_jobs != 0U || out.host_sub_fuel != 0U))
    {
        return RecordError::Malformed; // a schedule belongs to the host executor, and it needs one
    }

    if (const RecordError e = read_str(r, out.program_path); e != RecordError::Ok)
    {
        return e;
    }
    if (!r.u64(out.content_hash) || !r.u64(out.asset) || !r.u64(out.generation))
    {
        return RecordError::Truncated;
    }
    if (out.generation != 0U && out.asset == 0U)
    {
        return RecordError::Malformed; // a generation belongs to an asset
    }
    const crd::u8* blob    = nullptr;
    crd::usize     blob_n  = 0U;
    bool           bounded = true;
    if (!r.bytes(kReplayMaxProgramBytes, blob, blob_n, bounded))
    {
        return RecordError::Truncated;
    }
    if (!bounded)
    {
        return RecordError::Malformed;
    }
    out.program.clear();
    out.program.reserve(blob_n);
    for (crd::usize i = 0U; i < blob_n; ++i)
    {
        out.program.push_back(blob[i]);
    }

    if (const RecordError e = read_str(r, out.entry); e != RecordError::Ok)
    {
        return e;
    }
    if (const RecordError e = read_values(r, kReplayMaxArgs, out.args); e != RecordError::Ok)
    {
        return e;
    }
    for (ReplayInput& in : out.inputs)
    {
        crd::u8 need  = 0U;
        crd::u8 state = 0U;
        if (!r.u8(need) || !r.u8(state))
        {
            return RecordError::Truncated;
        }
        if (need > static_cast<crd::u8>(ReplayNeed::Unknown) || state > static_cast<crd::u8>(ReplayInputState::Missing))
        {
            return RecordError::Malformed;
        }
        in.need  = static_cast<ReplayNeed>(need);
        in.state = static_cast<ReplayInputState>(state);
    }

    crd::u32 kept = 0U;
    if (!r.u32(out.max_events) || !r.u64(out.events_total) || !r.u32(kept))
    {
        return RecordError::Truncated;
    }
    const crd::u64 expected = out.events_total < out.max_events ? out.events_total : out.max_events;
    if (out.max_events == 0U || out.max_events > kReplayMaxEvents || kept != expected)
    {
        return RecordError::Malformed;
    }
    out.events.clear();
    out.events.reserve(kept);
    for (crd::u32 i = 0U; i < kept; ++i)
    {
        ReplayEvent ev;
        if (!r.u64(ev.op) || !r.u32(ev.depth) || !r.u32(ev.values))
        {
            return RecordError::Truncated;
        }
        if (ev.values > kReplayEventValues)
        {
            return RecordError::Malformed;
        }
        for (crd::u32 k = 0U; k < ev.values; ++k)
        {
            if (!r.i64(ev.value[k]))
            {
                return RecordError::Truncated;
            }
        }
        out.events.push_back(ev);
    }

    crd::u8 error      = 0U;
    crd::u8 host_error = 0U;
    if (!r.u8(error) || !r.u8(host_error) || !r.u64(out.fault_op))
    {
        return RecordError::Truncated;
    }
    if (error > static_cast<crd::u8>(plan::RunError::Cancelled) ||
        host_error > static_cast<crd::u8>(exec::ExecError::Cancelled))
    {
        return RecordError::Malformed;
    }
    out.error      = static_cast<plan::RunError>(error);
    out.host_error = static_cast<exec::ExecError>(host_error);
    if ((host && out.error != plan::RunError::None) || (!host && out.host_error != exec::ExecError::None))
    {
        return RecordError::Malformed; // each executor reports its own error
    }
    if (const RecordError e = read_values(r, kReplayMaxValues, out.results); e != RecordError::Ok)
    {
        return e;
    }
    if (const RecordError e = read_values(r, kReplayMaxValues, out.cells); e != RecordError::Ok)
    {
        return e;
    }
    return r.done() ? RecordError::Ok : RecordError::Malformed;
}

// ---- the traced run -------------------------------------------------------------------------------------------------

[[nodiscard]] StableId op_at(const plan::CompiledPlan& plan, plan::InstrRef at) noexcept
{
    return plan.sites[plan.seqs[at.seq].sites[at.instr]].op;
}

// One safe point of a traced run. `pending` holds, per call depth, the kept event whose results are read next
// (kNoPending: none).
void trace_safe_point(ReplayTrace& out, cont::Array<crd::u64>& pending, crd::u32 max_events,
                      const plan::CompiledPlan& plan, const plan::SafePoint& at)
{
    // The results of the previous kept event of this frame: its instr ran since then.
    if (at.depth < pending.size() && pending[at.depth] != kNoPending)
    {
        ReplayEvent& ev = out.events[static_cast<crd::usize>(pending[at.depth])];
        for (crd::u32 k = 0U; k < kReplayEventValues; ++k)
        {
            const plan::ValueRead v = plan::read_value(plan, at, StableId{ev.op}, k);
            if (v.state != plan::ValueState::Available)
            {
                break;
            }
            ev.value[k] = v.bits;
            ev.values   = k + 1U;
        }
    }
    // Frames deeper than this one have returned: their last events keep no values.
    pending.resize(static_cast<crd::usize>(at.depth) + 1U, kNoPending);

    const crd::u64 index = out.events_total++;
    if (index < max_events)
    {
        ReplayEvent ev;
        ev.op    = op_at(plan, at.at).value;
        ev.depth = at.depth;
        out.events.push_back(ev);
        out.sites.push_back(at.at);
        pending[at.depth] = index;
    }
    else
    {
        pending[at.depth] = kNoPending;
    }
}

[[nodiscard]] ReplaySite site_at(const Context& ctx, const plan::CompiledPlan& plan, plan::InstrRef at) noexcept
{
    ReplaySite site;
    if (!at.valid() || at.seq >= plan.seqs.size() || at.instr >= plan.seqs[at.seq].sites.size())
    {
        return site;
    }
    const Provenance p = plan::instr_provenance(plan, at);
    site.op            = p.op.value;
    if (const Origin* const o = p.primary(); o != nullptr)
    {
        site.file = ctx.file_path(o->loc.file_id);
        site.line = o->loc.line;
        site.col  = o->loc.col;
    }
    return site;
}

void diverge(Divergence& d, DivergenceKind kind, crd::u64 index, crd::i64 recorded, crd::i64 observed)
{
    d.kind     = kind;
    d.index    = index;
    d.recorded = recorded;
    d.observed = observed;
}

[[nodiscard]] bool compare_values(const cont::Array<crd::i64>& recorded, const cont::Array<crd::i64>& observed,
                                  DivergenceKind kind, Divergence& d)
{
    const crd::usize n = recorded.size() < observed.size() ? recorded.size() : observed.size();
    for (crd::usize i = 0U; i < n; ++i)
    {
        if (recorded[i] != observed[i])
        {
            diverge(d, kind, i, recorded[i], observed[i]);
            return false;
        }
    }
    if (recorded.size() != observed.size())
    {
        diverge(d, kind, n, static_cast<crd::i64>(recorded.size()), static_cast<crd::i64>(observed.size()));
        d.count = true;
        return false;
    }
    return true;
}
} // namespace

// ---- build identity -------------------------------------------------------------------------------------------------

ReplayBuild current_build(memory::IAllocator* alloc)
{
    ReplayBuild b(alloc);
    b.version.append(cont::StringView{CRD_VERSION_STRING});
    b.platform.append(cont::StringView{crd::platform_name()});
    b.compiler.append(cont::StringView{crd::compiler_name()});
    b.arch.append(cont::StringView{crd::arch_name()});
#if defined(CRD_DEBUG)
    b.config.append(cont::StringView{"debug"});
#else
    b.config.append(cont::StringView{"release"});
#endif
    b.asserts  = CRD_ENABLE_ASSERTS != 0;
    b.executor = kReplayExecutor;
    return b;
}

bool same_build(const ReplayBuild& a, const ReplayBuild& b, cont::String& differing)
{
    const auto note = [&differing](bool same, cont::StringView name)
    {
        if (same)
        {
            return;
        }
        if (!differing.empty())
        {
            differing.push_back(',');
        }
        differing.append(name);
    };
    const auto eq = [](const cont::String& x, const cont::String& y)
    {
        return cont::StringView{x.data(), x.size()} == cont::StringView{y.data(), y.size()};
    };
    const crd::usize before = differing.size();
    note(eq(a.version, b.version), "version");
    note(eq(a.platform, b.platform), "platform");
    note(eq(a.compiler, b.compiler), "compiler");
    note(eq(a.arch, b.arch), "arch");
    note(eq(a.config, b.config), "config");
    note(a.asserts == b.asserts, "asserts");
    note(a.executor == b.executor, "executor");
    return differing.size() == before;
}

// ---- names ----------------------------------------------------------------------------------------------------------

cont::StringView replay_input_state_name(ReplayInputState s) noexcept
{
    switch (s) // no default: every state is named
    {
    case ReplayInputState::NotNeeded: return cont::StringView{"not-needed"};
    case ReplayInputState::Recorded: return cont::StringView{"recorded"};
    case ReplayInputState::Missing: return cont::StringView{"missing"};
    }
    return cont::StringView{"?"};
}

cont::StringView replay_input_name(crd::u32 index) noexcept
{
    return index < kReplayInputs ? kInputNames[index] : cont::StringView{"?"};
}

cont::StringView replay_executor_name(ReplayExecutorKind k) noexcept
{
    switch (k) // no default (-Werror=switch)
    {
    case ReplayExecutorKind::Plan: return cont::StringView{"plan"};
    case ReplayExecutorKind::Host: return cont::StringView{"host"};
    }
    return cont::StringView{"?"};
}

cont::StringView record_error_name(RecordError e) noexcept
{
    switch (e) // no default: every error is named
    {
    case RecordError::Ok: return cont::StringView{"ok"};
    case RecordError::NotARecord: return cont::StringView{"not-a-record"};
    case RecordError::UnsupportedSchema: return cont::StringView{"unsupported-schema"};
    case RecordError::Truncated: return cont::StringView{"truncated"};
    case RecordError::BadChecksum: return cont::StringView{"bad-checksum"};
    case RecordError::Malformed: return cont::StringView{"malformed"};
    }
    return cont::StringView{"?"};
}

cont::StringView divergence_kind_name(DivergenceKind k) noexcept
{
    switch (k) // no default: every kind is named
    {
    case DivergenceKind::None: return cont::StringView{"none"};
    case DivergenceKind::Path: return cont::StringView{"path"};
    case DivergenceKind::Value: return cont::StringView{"value"};
    case DivergenceKind::Length: return cont::StringView{"length"};
    case DivergenceKind::Outcome: return cont::StringView{"outcome"};
    case DivergenceKind::Results: return cont::StringView{"results"};
    case DivergenceKind::Cells: return cont::StringView{"cells"};
    }
    return cont::StringView{"?"};
}

bool operator==(const ReplayEvent& a, const ReplayEvent& b) noexcept
{
    if (a.op != b.op || a.depth != b.depth || a.values != b.values)
    {
        return false;
    }
    for (crd::u32 k = 0U; k < a.values && k < kReplayEventValues; ++k)
    {
        if (a.value[k] != b.value[k])
        {
            return false;
        }
    }
    return true;
}

// ---- the record file ------------------------------------------------------------------------------------------------

bool is_replay_record(cont::ConstSpan<crd::u8> bytes) noexcept
{
    if (bytes.size() < 4U)
    {
        return false;
    }
    const crd::u32 magic = static_cast<crd::u32>(bytes[0]) | (static_cast<crd::u32>(bytes[1]) << 8U) |
                           (static_cast<crd::u32>(bytes[2]) << 16U) | (static_cast<crd::u32>(bytes[3]) << 24U);
    return magic == kRecordMagic;
}

void encode_record(const ReplayRecord& record, cont::Array<crd::u8>& out)
{
    cont::Array<crd::u8> payload(out.allocator());
    put_str(payload, record.build.version);
    put_str(payload, record.build.platform);
    put_str(payload, record.build.compiler);
    put_str(payload, record.build.arch);
    put_str(payload, record.build.config);
    put_u8(payload, record.build.asserts ? 1U : 0U);
    put_u32(payload, record.build.executor);
    put_u8(payload, static_cast<crd::u8>(record.executor));
    put_u32(payload, record.host_jobs);
    put_u64(payload, record.host_sub_fuel);
    put_str(payload, record.program_path);
    put_u64(payload, record.content_hash);
    put_u64(payload, record.asset);
    put_u64(payload, record.generation);
    put_bytes(payload, record.program.data(), record.program.size());
    put_str(payload, record.entry);
    put_values(payload, record.args);
    for (const ReplayInput& in : record.inputs)
    {
        put_u8(payload, static_cast<crd::u8>(in.need));
        put_u8(payload, static_cast<crd::u8>(in.state));
    }
    put_u32(payload, record.max_events);
    put_u64(payload, record.events_total);
    put_u32(payload, static_cast<crd::u32>(record.events.size()));
    for (const ReplayEvent& ev : record.events)
    {
        put_u64(payload, ev.op);
        put_u32(payload, ev.depth);
        put_u32(payload, ev.values);
        for (crd::u32 k = 0U; k < ev.values && k < kReplayEventValues; ++k)
        {
            put_i64(payload, ev.value[k]);
        }
    }
    put_u8(payload, static_cast<crd::u8>(record.error));
    put_u8(payload, static_cast<crd::u8>(record.host_error));
    put_u64(payload, record.fault_op);
    put_values(payload, record.results);
    put_values(payload, record.cells);

    out.clear();
    out.reserve(kHeaderBytes + payload.size());
    put_u32(out, kRecordMagic);
    put_u32(out, record.schema);
    put_u64(out, payload.size());
    put_u64(out, cont::fnv1a_64(payload.data(), payload.size()));
    for (const crd::u8 b : payload)
    {
        out.push_back(b);
    }
}

RecordError decode_record(cont::ConstSpan<crd::u8> bytes, ReplayRecord& out)
{
    if (!is_replay_record(bytes))
    {
        return RecordError::NotARecord;
    }
    Reader   header(bytes.data(), bytes.size());
    crd::u32 magic    = 0U;
    crd::u64 size     = 0U;
    crd::u64 checksum = 0U;
    if (!header.u32(magic) || !header.u32(out.schema) || !header.u64(size) || !header.u64(checksum))
    {
        return RecordError::Truncated;
    }
    if (out.schema != kReplayRecordSchema)
    {
        return RecordError::UnsupportedSchema;
    }
    const crd::usize available = bytes.size() - kHeaderBytes;
    if (size > available)
    {
        return RecordError::Truncated;
    }
    if (size < available)
    {
        return RecordError::Malformed;
    }
    const crd::u8* const payload = bytes.data() + kHeaderBytes;
    if (cont::fnv1a_64(payload, available) != checksum)
    {
        return RecordError::BadChecksum;
    }
    Reader r(payload, available);
    return read_payload(r, out);
}

cont::StringView record_write_name(RecordWrite w) noexcept
{
    switch (w) // no default (-Werror=switch)
    {
    case RecordWrite::Ok: return cont::StringView{"ok"};
    case RecordWrite::Exists: return cont::StringView{"exists"};
    case RecordWrite::Failed: return cont::StringView{"failed"};
    }
    return cont::StringView{"?"};
}

RecordWrite write_record_file(cont::StringView path, const ReplayRecord& record)
{
    if (detail::file_exists(path))
    {
        return RecordWrite::Exists;
    }
    memory::IAllocator* const alloc = record.program.allocator();
    cont::Array<crd::u8>      bytes(alloc);
    encode_record(record, bytes);
    cont::String reason(alloc);
    return detail::write_new_file(path, {bytes.data(), bytes.size()}, reason) == perf::DiagStatus::Ok
               ? RecordWrite::Ok
               : RecordWrite::Failed;
}

// ---- load, run and compare ------------------------------------------------------------------------------------------

void load_replay_program(Context& ctx, cont::ConstSpan<crd::u8> blob, cont::StringView entry, Registrar registrar,
                         void* user, ReplayProgram& out)
{
    if (registrar != nullptr)
    {
        registrar(ctx, user);
    }
    const ReadResult rr = read_program(ctx, blob, ctx.allocator());
    out.read            = rr.error;
    if (!rr.ok() || rr.module == nullptr)
    {
        return;
    }
    out.module       = rr.module;
    out.content_hash = rr.content_hash;
    ctx.assign_stable_ids(*out.module);
    out.compiled = plan::compile(ctx, *out.module, entry, ctx.allocator());
}

ReplayRecorder::ReplayRecorder(ReplayTrace& out, crd::u32 max_events)
    : m_out(&out), m_max(max_events < kReplayMaxEvents ? max_events : kReplayMaxEvents),
      m_pending(out.events.allocator())
{
    out.events.clear();
    out.sites.clear();
    out.results.clear();
    out.cells.clear();
    out.events_total = 0U;
    out.error        = plan::RunError::None;
    out.fault        = plan::InstrRef{};
    out.fault_op     = 0U;
    out.host_error   = exec::ExecError::None;
    out.events.reserve(m_max);
    out.sites.reserve(m_max);
    m_pending.reserve(16U);
}

plan::RunControl ReplayRecorder::control(const std::atomic<bool>* cancel) noexcept
{
    return plan::RunControl{&ReplayRecorder::on_safe_point, this, cancel};
}

plan::SafePointAction ReplayRecorder::on_safe_point(const plan::CompiledPlan& plan, const plan::SafePoint& at,
                                                    void* user)
{
    auto& self = *static_cast<ReplayRecorder*>(user);
    trace_safe_point(*self.m_out, self.m_pending, self.m_max, plan, at);
    return plan::SafePointAction::Continue;
}

void ReplayRecorder::finish(const plan::CompiledPlan& plan, const plan::RunResult& result)
{
    ReplayTrace& out = *m_out;
    out.error        = result.error;
    out.fault        = result.fault;
    out.fault_op     = result.fault.valid() ? op_at(plan, result.fault).value : 0U;
    out.results.clear();
    out.cells.clear();
    for (const crd::i64 v : result.values)
    {
        out.results.push_back(v);
    }
    for (const crd::i64 v : result.cells)
    {
        out.cells.push_back(v);
    }
}

void run_traced(const ReplayProgram& program, cont::ConstSpan<crd::i64> args, crd::u32 max_events,
                const std::atomic<bool>* cancel, ReplayTrace& out)
{
    ReplayRecorder            rec(out, max_events);
    const plan::RunControl    control  = rec.control(cancel);
    const plan::CompiledPlan& compiled = program.compiled.plan;
    const plan::RunResult r = plan::run(compiled, args, out.events.allocator(), plan::RunHooks{}, &control);
    rec.finish(compiled, r);
}

ReplaySite replay_site(const Context& ctx, const ReplayProgram& program, plan::InstrRef at) noexcept
{
    return site_at(ctx, program.compiled.plan, at);
}

ReplaySite replay_site_of_op(const Context& ctx, const ReplayProgram& program, crd::u64 op) noexcept
{
    const plan::CompiledPlan& p = program.compiled.plan;
    for (crd::u32 s = 0U; s < static_cast<crd::u32>(p.seqs.size()); ++s)
    {
        for (crd::u32 k = 0U; k < static_cast<crd::u32>(p.seqs[s].sites.size()); ++k)
        {
            if (op != 0U && p.sites[p.seqs[s].sites[k]].op.value == op)
            {
                return site_at(ctx, p, plan::InstrRef{s, k});
            }
        }
    }
    return ReplaySite{};
}

InterpreterRecorder::InterpreterRecorder(ReplayTrace& out, crd::u32 max_events)
    : m_out(&out), m_max(max_events < kReplayMaxEvents ? max_events : kReplayMaxEvents), m_open(out.events.allocator())
{
    out.events.clear();
    out.sites.clear();
    out.results.clear();
    out.cells.clear();
    out.events_total = 0U;
    out.error        = plan::RunError::None;
    out.fault        = plan::InstrRef{};
    out.fault_op     = 0U;
    out.host_error   = exec::ExecError::None;
    out.events.reserve(m_max);
    m_open.reserve(16U);
}

void InterpreterRecorder::attach(exec::Interpreter& in) noexcept
{
    m_in = &in;
    in.set_step_hooks(&InterpreterRecorder::on_pre, &InterpreterRecorder::on_post, this);
}

void InterpreterRecorder::detach(exec::Interpreter& in)
{
    in.set_step_hooks(nullptr, nullptr, nullptr);
    m_in = nullptr;
    m_open.clear();

    // The cells' current values (the value the next read returns), in stable id order.
    memory::IAllocator* const        alloc = m_out->cells.allocator();
    cont::Array<exec::StateSnapshot> cells(alloc);
    in.snapshot_state_by_id(cells, alloc);
    for (crd::usize i = 1U; i < cells.size(); ++i)
    {
        for (crd::usize k = i; k > 0U && cells[k - 1U].id > cells[k].id; --k)
        {
            exec::StateSnapshot t = std::move(cells[k - 1U]);
            cells[k - 1U]         = std::move(cells[k]);
            cells[k]              = std::move(t);
        }
    }
    m_out->cells.clear();
    for (const exec::StateSnapshot& c : cells)
    {
        m_out->cells.push_back(c.pos < c.ring.size() ? c.ring[c.pos] : 0);
    }
}

void InterpreterRecorder::on_pre(const Operation& op, void* user)
{
    auto&          self  = *static_cast<InterpreterRecorder*>(user);
    ReplayTrace&   out   = *self.m_out;
    const crd::u64 index = out.events_total++;
    Open           open;
    open.op    = &op;
    open.event = kNoPending;
    if (index < self.m_max)
    {
        ReplayEvent ev;
        ev.op    = op.stable_id().value;
        ev.depth = self.m_in != nullptr ? self.m_in->call_depth() : 0U;
        out.events.push_back(ev);
        open.event = index;
    }
    self.m_open.push_back(open);
}

void InterpreterRecorder::on_post(const Operation& op, void* user)
{
    auto& self = *static_cast<InterpreterRecorder*>(user);
    // Pre and post hooks nest, and a failed dispatch ends the run, so the innermost open op is this one.
    if (self.m_open.empty())
    {
        return;
    }
    const Open open = self.m_open.back();
    self.m_open.pop_back();
    if (open.event == kNoPending || open.op != &op || self.m_in == nullptr)
    {
        return;
    }
    ReplayEvent&   ev = self.m_out->events[static_cast<crd::usize>(open.event)];
    const crd::u32 n  = op.num_results() < kReplayEventValues ? op.num_results() : kReplayEventValues;
    for (crd::u32 k = 0U; k < n; ++k)
    {
        crd::i64 v = 0;
        if (!self.m_in->value_of(op.result(k), v))
        {
            break;
        }
        ev.value[k] = v;
        ev.values   = k + 1U;
    }
}

void InterpreterRecorder::finish(const exec::ExecResult& result)
{
    ReplayTrace& out = *m_out;
    out.host_error   = result.error;
    out.fault_op     = result.op != nullptr ? result.op->stable_id().value : 0U;
    out.results.clear();
    for (const crd::i64 v : result.values)
    {
        out.results.push_back(v);
    }
}

ReplaySite replay_site_in_module(const Context& ctx, const Module& module, crd::u64 op, memory::IAllocator* scratch)
{
    ReplaySite site;
    const Operation* const found = op != 0U ? ctx.find_by_stable_id(module, StableId{op}) : nullptr;
    if (found == nullptr)
    {
        return site;
    }
    cont::Array<Origin> storage(scratch);
    const Provenance    p = resolve_provenance(ctx, found, storage);
    site.op               = op;
    if (const Origin* const o = p.primary(); o != nullptr)
    {
        site.file = ctx.file_path(o->loc.file_id);
        site.line = o->loc.line;
        site.col  = o->loc.col;
    }
    return site;
}

bool record_missing_inputs(const ReplayRecord& record, cont::String& out)
{
    bool none = true;
    for (crd::u32 i = 0U; i < kReplayInputs; ++i)
    {
        if (record.inputs[i].state == ReplayInputState::Missing)
        {
            if (!out.empty())
            {
                out.push_back(',');
            }
            out.append(replay_input_name(i));
            none = false;
        }
    }
    return none;
}

Divergence first_divergence(const ReplayRecord& record, const ReplayTrace& trace) noexcept
{
    Divergence       d;
    const crd::usize kept = record.events.size() < trace.events.size() ? record.events.size() : trace.events.size();
    for (crd::usize i = 0U; i < kept; ++i)
    {
        const ReplayEvent& a = record.events[i];
        const ReplayEvent& b = trace.events[i];
        d.recorded_op        = a.op;
        d.observed_op        = b.op;
        d.site               = i < trace.sites.size() ? trace.sites[i] : plan::InstrRef{};
        if (a.op != b.op || a.depth != b.depth)
        {
            diverge(d, DivergenceKind::Path, i, static_cast<crd::i64>(a.depth), static_cast<crd::i64>(b.depth));
            return d;
        }
        if (a.values != b.values)
        {
            diverge(d, DivergenceKind::Value, i, a.values, b.values);
            d.count = true; // the number of results read differs
            return d;
        }
        for (crd::u32 k = 0U; k < a.values; ++k)
        {
            if (a.value[k] != b.value[k])
            {
                diverge(d, DivergenceKind::Value, i, a.value[k], b.value[k]);
                d.value_index = k;
                return d;
            }
        }
    }
    d = Divergence{};
    if (record.events_total != trace.events_total || record.events.size() != trace.events.size())
    {
        diverge(d, DivergenceKind::Length, kept, static_cast<crd::i64>(record.events_total),
                static_cast<crd::i64>(trace.events_total));
        d.count = true;
        if (kept < trace.sites.size())
        {
            d.site = trace.sites[kept];
        }
        return d;
    }
    if (record.error != trace.error || record.host_error != trace.host_error || record.fault_op != trace.fault_op)
    {
        const bool host = record.executor == ReplayExecutorKind::Host;
        diverge(d, DivergenceKind::Outcome, 0U,
                host ? static_cast<crd::i64>(record.host_error) : static_cast<crd::i64>(record.error),
                host ? static_cast<crd::i64>(trace.host_error) : static_cast<crd::i64>(trace.error));
        d.recorded_op = record.fault_op;
        d.observed_op = trace.fault_op;
        d.site        = trace.fault;
        return d;
    }
    if (!compare_values(record.results, trace.results, DivergenceKind::Results, d) ||
        !compare_values(record.cells, trace.cells, DivergenceKind::Cells, d))
    {
        return d;
    }
    return Divergence{};
}
} // namespace crd::ceir::cook
