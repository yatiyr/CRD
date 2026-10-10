// crd-ceir-cook -- minimizing the recorded inputs of a failing run (DIAG.9c). See crd/ceir/cook/replay_minimize.hpp.

#include <crd/ceir/cook/replay_minimize.hpp>

#include <crd/ceir/binary.hpp> // stable_hash
#include <crd/ceir/context.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>

#include <utility>

namespace crd::ceir::cook
{
namespace
{
namespace cont = crd::containers;
namespace mz   = crd::minimize;

// The candidate inputs of one record and one run of them.
class Reproducer
{
public:
    Reproducer(const ReplayProgram& program, const ReplayRecord& record, const std::atomic<bool>* cancel,
               memory::IAllocator* alloc)
        : args(record.args, alloc), reads(record.input_reads, alloc), m_program(&program), m_record(&record),
          m_cancel(cancel), m_trace(alloc)
    {
    }

    // One run of the current inputs: true when it failed the recorded way (same error, same op, no read the stream
    // could not answer). `trace()` holds the run.
    bool run()
    {
        if (m_cancel != nullptr && m_cancel->load(std::memory_order_acquire))
        {
            m_cancelled = true;
            return false;
        }
        InputFeed feed({reads.data(), reads.size()}, m_trace);
        run_traced(*m_program, cont::as_const_span(args), m_record->max_events, m_cancel, m_trace, feed.source());
        if (m_trace.error == plan::RunError::Cancelled && m_cancel != nullptr &&
            m_cancel->load(std::memory_order_acquire))
        {
            m_cancelled = true;
            return false;
        }
        return m_trace.error == m_record->error && m_trace.fault_op == m_record->fault_op &&
               !m_trace.input_refusal.refused;
    }

    // Keep only the reads the last run answered (it reproduced, so it is the current failing run).
    void cut_reads() { reads = m_trace.input_reads; }

    [[nodiscard]] ReplayTrace& trace() noexcept { return m_trace; }
    [[nodiscard]] bool         cancelled() const noexcept { return m_cancelled; }

    cont::Array<crd::i64>        args;
    cont::Array<ReplayInputRead> reads;

private:
    const ReplayProgram*     m_program;
    const ReplayRecord*      m_record;
    const std::atomic<bool>* m_cancel;
    ReplayTrace              m_trace;
    bool                     m_cancelled = false;
};

// Judge the current inputs on K runs and, when they reproduce, cut the read stream to the failing run's reads.
mz::Verdict judge_current(Reproducer& rep, const mz::Budget& budget, mz::Stats& stats)
{
    const auto run_current = [&rep](int /*unused*/)
    {
        return rep.run();
    };
    const mz::Verdict v = mz::judge(0, run_current, budget, stats);
    if (v == mz::Verdict::Yes)
    {
        rep.cut_reads();
    }
    return v;
}

// Shrink `slot` (an argument or a read's value) toward 0; `moved` is set when it changed.
mz::Status shrink(Reproducer& rep, crd::i64& slot, const mz::Budget& budget, mz::Stats& stats, bool& moved)
{
    const crd::i64 original = slot;
    const auto     run_with = [&rep, &slot, original](crd::i64 v)
    {
        slot           = v;
        const bool hit = rep.run();
        slot           = original;
        return hit;
    };
    crd::i64         value = original;
    const mz::Status s     = mz::minimize_toward_zero(value, run_with, budget, stats);
    slot                   = value;
    moved                  = value != original;
    return s;
}

void refuse(RecordMinimizeResult& result, RecordMinimizeStatus status, cont::StringView why)
{
    result.status = status;
    result.reason.append(why);
}

} // namespace

cont::StringView record_minimize_status_name(RecordMinimizeStatus s) noexcept
{
    switch (s)
    {
    case RecordMinimizeStatus::Minimal:
        return "minimal";
    case RecordMinimizeStatus::Budget:
        return "budget";
    case RecordMinimizeStatus::NotReproduced:
        return "not-reproduced";
    case RecordMinimizeStatus::NotAFailure:
        return "not-a-failure";
    case RecordMinimizeStatus::Unsupported:
        return "unsupported";
    case RecordMinimizeStatus::Refused:
        return "refused";
    case RecordMinimizeStatus::Unrunnable:
        return "unrunnable";
    case RecordMinimizeStatus::Cancelled:
        return "cancelled";
    }
    return "unknown";
}

void minimize_record_inputs(const ReplayRecord& record, Registrar registrar, void* user,
                            const RecordMinimizeOptions& options, const std::atomic<bool>* cancel, ReplayRecord& out,
                            RecordMinimizeResult& result)
{
    memory::IAllocator* const alloc = out.args.allocator();
    result.stats                    = mz::Stats{};
    result.args_changed             = 0U;
    result.reads_changed            = 0U;
    result.reads_cut                = 0U;
    result.reason.clear();

    // Refused before anything runs.
    if (record.executor != ReplayExecutorKind::Plan)
    {
        refuse(result, RecordMinimizeStatus::Unsupported, "only a plan record's inputs are minimized; this is a ");
        result.reason.append(replay_executor_name(record.executor));
        result.reason.append(" record");
        return;
    }
    if (record.input_reads_total != record.input_reads.size())
    {
        refuse(result, RecordMinimizeStatus::Unsupported,
               "the record's host input stream is incomplete (the run read more than a record keeps)");
        return;
    }
    if (record.error == plan::RunError::None)
    {
        refuse(result, RecordMinimizeStatus::NotAFailure, "the recorded run did not fail: nothing to minimize");
        return;
    }
    cont::String differing(alloc);
    if (!same_build(record.build, current_build(alloc), differing) && !options.any_build)
    {
        refuse(result, RecordMinimizeStatus::Refused, "the record was made by another build (differs in ");
        result.reason.append(cont::StringView{differing.data(), differing.size()});
        result.reason.append(")");
        return;
    }
    cont::String missing(alloc);
    if (!record_missing_inputs(record, missing))
    {
        refuse(result, RecordMinimizeStatus::Refused, "the record is missing inputs its program needs (");
        result.reason.append(cont::StringView{missing.data(), missing.size()});
        result.reason.append(")");
        return;
    }
    Context       ctx(alloc);
    ReplayProgram program(alloc);
    load_replay_program(ctx, {record.program.data(), record.program.size()},
                        cont::StringView{record.entry.data(), record.entry.size()}, registrar, user, program);
    if (program.module == nullptr)
    {
        refuse(result, RecordMinimizeStatus::Unrunnable, "the record's program does not load");
        return;
    }
    if (program.content_hash != record.content_hash || stable_hash(ctx, *program.module, alloc) != record.content_hash)
    {
        refuse(result, RecordMinimizeStatus::Refused,
               "the record's program does not match the content hash it was recorded with");
        return;
    }
    cont::String     external(alloc);
    const ExternalReplay ext = check_external_replay(ctx, *program.module, "the record's", alloc, cancel, external);
    if (ext == ExternalReplay::Cancelled)
    {
        refuse(result, RecordMinimizeStatus::Cancelled, "cancelled before the program ran");
        return;
    }
    if (ext == ExternalReplay::Refused)
    {
        refuse(result, RecordMinimizeStatus::Refused, cont::StringView{external.data(), external.size()});
        return;
    }
    if (!program.compiled.ok())
    {
        refuse(result, RecordMinimizeStatus::Unrunnable, "the record's program does not compile");
        return;
    }

    Reproducer           rep(program, record, cancel, alloc);
    const mz::Budget&    budget = options.budget;
    mz::Stats&           stats  = result.stats;
    RecordMinimizeStatus status = RecordMinimizeStatus::Minimal;
    const auto           stop   = [&](mz::Status s) -> bool
    {
        if (rep.cancelled())
        {
            status = RecordMinimizeStatus::Cancelled;
            return true;
        }
        if (s == mz::Status::NotReproduced)
        {
            status = RecordMinimizeStatus::NotReproduced;
            return true;
        }
        if (s == mz::Status::Budget)
        {
            status = RecordMinimizeStatus::Budget;
            return true;
        }
        return false;
    };
    const auto judged = [](mz::Verdict v)
    {
        if (v == mz::Verdict::Yes)
        {
            return mz::Status::Minimal;
        }
        return v == mz::Verdict::Budget ? mz::Status::Budget : mz::Status::NotReproduced;
    };

    // Each value is shrunk again only when another value moved since it was last shrunk (`epoch` counts the moves, a
    // slot's stamp is the epoch it was last shrunk at): re-shrinking a value nothing else changed could only drift
    // along a local boundary, and the pass would never settle. Arguments come first, then the reads.
    const crd::u64        never_shrunk = ~crd::u64{0U};
    const crd::usize      slots        = rep.args.size() + rep.reads.size();
    cont::Array<crd::u64> stamp(alloc);
    crd::u64              epoch = 0U;
    stamp.resize(slots, never_shrunk);
    bool first_pass = true;
    bool changed    = true;
    while (changed)
    {
        changed = false;
        if (stop(judged(judge_current(rep, budget, stats))))
        {
            break;
        }
        if (first_pass)
        {
            result.reads_cut = record.input_reads.size() - rep.reads.size();
            first_pass       = false;
        }
        bool       halted = false;
        const auto visit  = [&](crd::usize slot, crd::i64& value)
        {
            if (stamp[slot] == epoch)
            {
                return;
            }
            bool moved = false;
            halted     = stop(shrink(rep, value, budget, stats, moved));
            if (moved)
            {
                ++epoch;
                changed = true;
            }
            stamp[slot] = epoch;
        };
        for (crd::usize i = 0U; i < rep.args.size() && !halted; ++i)
        {
            visit(i, rep.args[i]);
        }
        // The arguments may have made the failing run read less: cut the stream before shrinking the reads.
        if (halted || stop(judged(judge_current(rep, budget, stats))))
        {
            break;
        }
        for (crd::usize i = 0U; i < rep.reads.size() && !halted; ++i)
        {
            if (rep.reads[i].delivered)
            {
                visit(rep.args.size() + i, rep.reads[i].value);
            }
        }
        if (halted)
        {
            break;
        }
    }
    if (status == RecordMinimizeStatus::Cancelled)
    {
        result.status = status;
        result.reason.append("cancelled while the inputs were minimized");
        return;
    }
    if (status == RecordMinimizeStatus::NotReproduced)
    {
        result.status = status;
        result.reason.append(first_pass ? "the record's own inputs did not fail the recorded way in every run"
                                        : "the inputs stopped failing the recorded way while they were minimized");
        return;
    }

    // The new record: one more run of the final inputs, recorded as `replay.record` records a run.
    if (!rep.run())
    {
        result.status = rep.cancelled() ? RecordMinimizeStatus::Cancelled : RecordMinimizeStatus::NotReproduced;
        result.reason.append(rep.cancelled() ? "cancelled while the new record was made"
                                             : "the final inputs did not fail the recorded way when recorded");
        return;
    }
    for (crd::usize i = 0U; i < rep.args.size(); ++i)
    {
        if (rep.args[i] != record.args[i])
        {
            ++result.args_changed;
        }
    }
    for (crd::usize i = 0U; i < rep.reads.size(); ++i)
    {
        if (rep.reads[i].value != record.input_reads[i].value)
        {
            ++result.reads_changed;
        }
    }

    ReplayTrace& trace = rep.trace();
    out.schema         = kReplayRecordSchema;
    out.build          = current_build(alloc);
    out.executor       = ReplayExecutorKind::Plan;
    out.host_jobs      = 0U;
    out.host_sub_fuel  = 0U;
    out.program_path   = record.program_path;
    out.content_hash   = record.content_hash;
    out.asset          = record.asset;
    out.generation     = record.generation;
    out.program        = record.program;
    out.entry          = record.entry;
    out.args           = rep.args;
    for (crd::u32 i = 0U; i < kReplayInputs; ++i)
    {
        out.inputs[i] = record.inputs[i];
    }
    out.max_events        = record.max_events;
    out.events_total      = trace.events_total;
    out.events            = std::move(trace.events);
    out.error             = trace.error;
    out.host_error        = exec::ExecError::None;
    out.fault_op          = trace.fault_op;
    out.results           = std::move(trace.results);
    out.cells             = std::move(trace.cells);
    out.input_reads_total = trace.input_reads_total;
    out.input_reads       = std::move(trace.input_reads);
    out.device_adapter    = DeviceAdapterRecord(alloc);
    out.device_envelope   = DeviceEnvelope{};
    out.device_kernels.clear();
    out.device_buffers.clear();
    out.device_error = 0U;
    result.status    = status;
}

} // namespace crd::ceir::cook
