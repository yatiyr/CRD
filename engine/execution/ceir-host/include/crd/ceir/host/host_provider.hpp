#pragma once

// crd-ceir-host — the crd-jobs execution provider (CEIR-6b; ADR-0109 §4.2 bridge). `HostProvider` implements the abstract
// `crd::ceir::IExecutionProvider` by executing a CEIR program on the host reference Interpreter, LOWERING `task.parallel_for`
// onto `crd::jobs::parallel_for` — each parallel index runs the body via a fresh interpreter (from a prototype) over its own
// scratch allocator (race-free; reads of the const Context are the only shared access). This header names NO jobs type
// (jobs is a PRIVATE impl dependency); the provider surface is pure `crd::ceir::*`.
//
// ⛔ PRECONDITION: the CALLER owns the crd::jobs pool lifecycle (jobs::init before / jobs::shutdown after) — the provider
// never inits/shuts it. ⛔ The parallel body must be STATE-FREE (no §20 StateEdge cell, transitively) and yield EXACTLY one
// value — the provider PRE-FLIGHTS this (else a typed ParallelBodyStateful / ParallelYieldArity error).

#include <crd/ceir/exec.hpp>       // exec::ExecResult
#include <crd/ceir/inspect.hpp>    // inspect::Session (DIAG.8b: an execution under a debugger session)
#include <crd/ceir/provider.hpp>   // crd::ceir::IExecutionProvider
#include <crd/ceir/semantics.hpp>  // RealtimeClass (§32 execution classes)
#include <crd/containers/array.hpp>
#include <crd/containers/hash_map.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/jobs/job_decl.hpp> // crd::jobs::Priority — a jobs type, legal in this BRIDGE header (never in crd-ceir; I4)
#include <crd/memory/allocator.hpp>

#include <atomic>

namespace crd::ceir::host
{
// Map a §32 execution class (a region's RealtimeClass tag) to a crd::jobs dispatch Priority. ⛔ Lives in the BRIDGE — it
// maps crd-ceir vocabulary to a jobs TYPE, so it CANNOT live in crd-ceir without breaking I4 (crd-ceir names no jobs type).
// A TOTAL no-default switch — a new RealtimeClass is a `-Werror=switch` compile error.
[[nodiscard]] crd::jobs::Priority priority_for(RealtimeClass rc) noexcept;

struct PooledToken; // CEIR-11a stage 3: a jobs-backed launch's {own scratch, result, counter, job ctx} — defined in the .cpp

// DIAG.9a: an observer of one execution's SUBMITTING interpreter (e.g. a run recorder). `attach` runs on the submitting
// thread after the interpreter is built and before the entry runs, so it may install step hooks; `detach` runs after
// the entry returned and every pooled launch was joined, while the interpreter is still alive. Step hooks are not
// copied to the provider's sub-interpreters, so the bodies they run (parallel ranges, fold steps, pooled launch
// bodies) are never observed. Either callback may be null.
struct HostObserver
{
    void (*attach)(exec::Interpreter& in, void* user) = nullptr;
    void (*detach)(exec::Interpreter& in, void* user) = nullptr;
    void* user                                        = nullptr;
};

class HostProvider final : public IExecutionProvider
{
public:
    // `alloc` backs the persistent map-output buffers (must outlive the provider). `num_jobs` = the parallel_for job
    // split (⭐ the RESULT is num_jobs-INDEPENDENT — the 6z determinism seed); `sub_fuel` = a per-index step budget.
    explicit HostProvider(memory::IAllocator* alloc, crd::u32 num_jobs = 8U, crd::u64 sub_fuel = crd::u64{1} << 20U);

    [[nodiscard]] containers::StringView name() const noexcept override;
    [[nodiscard]] bool                   advertises(const Context& ctx, OpId k) const override;
    [[nodiscard]] exec::ExecResult execute(Context& ctx, const Module& m, containers::StringView entry,
                                           containers::ConstSpan<crd::i64> args) override;
    // DIAG.8b: the same execution under an inspection session (bound to `m` with `Session::bind`). The submitting
    // thread's interpreter gets the session's safe points; every body this provider runs on its own sub-interpreter
    // (a parallel range on a pool worker, a map_reduce fold step, a pooled launch body) is DETACHED: it never pauses,
    // and a breakpoint hit there is counted and refused `DetachedBody`. This provider's cancel flag is the execution's
    // only flag, so `Session::cancel` also stops the pool work and `request_cancel` also ends a paused execution. Each
    // stop reports the pooled launches not yet joined; they keep running through a `Task` pause.
    [[nodiscard]] exec::ExecResult execute(Context& ctx, const Module& m, containers::StringView entry,
                                           containers::ConstSpan<crd::i64> args, inspect::Session& session);
    // The same execution with `observer` attached to the submitting interpreter.
    [[nodiscard]] exec::ExecResult execute(Context& ctx, const Module& m, containers::StringView entry,
                                           containers::ConstSpan<crd::i64> args, const HostObserver& observer);
    // Both. The session installs its own step hooks on the submitting interpreter, so the hooks `observer`
    // attached are handed to the session (`inspect::StepObserver`) and run inside its own: the observer sees every
    // op, the ones a stop holds included, whatever the controller does while the execution is paused.
    [[nodiscard]] exec::ExecResult execute(Context& ctx, const Module& m, containers::StringView entry,
                                           containers::ConstSpan<crd::i64> args, inspect::Session& session,
                                           const HostObserver& observer);

    // DIAG.9a: the host input seam (input.hpp) the next executions' `input` ops read through, on the submitting
    // interpreter only (sub-interpreters get none, and the shared pre-flight keeps input reads out of the bodies they
    // run: a launch body that reads one runs inline). Null (the default): every read fails `InputUnavailable`.
    // Borrowed; it must outlive each execute().
    void set_input_source(const input::InputSource* s) noexcept { m_inputs = s; }

    // DIAG.9a: the schedule settings a run record stores (the constructor's, after its zero-job clamp).
    [[nodiscard]] crd::u32 num_jobs() const noexcept { return m_num_jobs; }
    [[nodiscard]] crd::u64 sub_fuel() const noexcept { return m_sub_fuel; }

    // Inspection: the map output of a `task.parallel_for` op (its per-index yields). Instance-keyed (builder-form; pointers
    // don't survive a round-trip). Empty if `pf_op` was not executed by this provider.
    [[nodiscard]] containers::ConstSpan<crd::i64> map_output(const Operation* pf_op) const noexcept;

    // §30 cooperative cancellation: request that the running (or next) execution stop. The flag is threaded into every
    // parallel sub-interpreter; ranges observe it in their step loop and return `ExecError::Cancelled` (⛔ crd-jobs has NO
    // cancel primitive — this is cooperative, checked at op/loop granularity). Sticky until a fresh provider (single-use).
    void                             request_cancel() noexcept { m_cancel.store(true, std::memory_order_relaxed); }
    [[nodiscard]] const std::atomic<bool>* cancel_flag() const noexcept { return &m_cancel; }

    // Internal: stash a parallel_for's map output (called by the task EvalFn). Public so the free EvalFn can reach it.
    void store_map(const Operation* pf_op, containers::Array<crd::i64>&& out);
    [[nodiscard]] memory::IAllocator* map_allocator() const noexcept { return m_alloc; }

    // ── CEIR-11a stage 3: jobs-backed launch/await ON-POOL (the pooled-token machinery; called by the async EvalFns) ──
    // ⭐ POOLED handles live in a DISJOINT space (`kPoolBase + index`, all > u32-max) so a pooled token can NEVER truncate
    // in the sequential `u32(tok)` cast and alias a valid sequential handle (a silent wrong read). `resolve_token` routes
    // by full i64: >= kPoolBase ⇒ pooled (wait the counter, read the result), else the sequential yield-store.
    static constexpr crd::i64 kPoolBase = crd::i64{1} << 40; // far above any sequential yield-store index
    // Create a pooled token for a launch body + SUBMIT it to the pool (the body runs on a fresh sub over the token's own
    // scratch, filling its result before the counter decrements). Returns the token's i64 handle (kPoolBase + index).
    [[nodiscard]] crd::i64 pool_launch(const exec::Interpreter& proto, const Module& module, const Region* body,
                                       const std::atomic<bool>* cancel, crd::u64 sub_fuel, crd::jobs::Priority prio,
                                       crd::i32 pin_thread);
    // Resolve a token's yields: pooled ⇒ wait the counter (once) + return its result (or its typed error); returns false
    // ⇒ a bad/forged handle. `out` is valid only when the return is true AND `*out_err == None`. DIAG.8a: `out_op`
    // is the launch body's op that raised the error (null when ok or when the body named none), for the await to blame.
    [[nodiscard]] bool resolve_pooled(crd::i64 tok, containers::ConstSpan<crd::i64>& out, exec::ExecError& out_err,
                                      const Operation*& out_op) noexcept;
    [[nodiscard]] bool is_pooled(crd::i64 tok) const noexcept { return tok >= kPoolBase; }
    [[nodiscard]] bool pooled_index_valid(crd::i64 tok) const noexcept // a pooled handle in range (race/cancel: no wait)
    {
        return tok >= kPoolBase && (tok - kPoolBase) < static_cast<crd::i64>(m_pooled.size());
    }
    // ⭐ the witness — the CUMULATIVE count of launches that actually POOLED this execution (asserted ==N pooled / ==0
    // fallback). ⛔ Cumulative, NOT the live table size: `drain_pooled` frees the table at execute() exit, so a live count
    // would read 0 afterward and a never-pools impl would pass every parity test.
    [[nodiscard]] crd::usize pooled_count() const noexcept { return m_pooled_total; }
    // DIAG.8b: pooled launches of the running execution not yet joined by an await/join (they may still be running).
    // Executing thread only (the table is that thread's state); 0 after execute() returns (the drain joins them all).
    [[nodiscard]] crd::u32 pooled_unjoined() const noexcept;
    void drain_pooled() noexcept; // wait every outstanding counter + free every entry (execute() calls it at exit)

private:
    [[nodiscard]] exec::ExecResult run(Context& ctx, const Module& m, containers::StringView entry,
                                       containers::ConstSpan<crd::i64> args, inspect::Session* session,
                                       const HostObserver* observer);
    [[nodiscard]] static crd::u32 unjoined_of(void* self) noexcept; // the session's HostLink::pending probe

    memory::IAllocator* m_alloc;
    crd::u32            m_num_jobs;
    crd::u64            m_sub_fuel;
    std::atomic<bool>   m_cancel{false}; // §30 cooperative cancel flag (threaded to sub-interpreters)

    containers::HashMap<const Operation*, containers::Array<crd::i64>> m_map; // parallel_for op → its per-index outputs
    containers::Array<PooledToken*> m_pooled; // stage 3: heap-owned pooled tokens (⛔ heap so the growable table never
                                              // moves an entry the JobDecl captured by pointer — the push-back-UAF scar)
    crd::usize m_pooled_total = 0U;           // cumulative pooled-this-execution (the witness; NOT reset by drain)
    inspect::Session* m_session = nullptr;    // DIAG.8b: the attached session while execute() runs (else null)
    const input::InputSource* m_inputs = nullptr; // DIAG.9a: the host input seam of the submitting interpreter
};
} // namespace crd::ceir::host
