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

private:
    memory::IAllocator* m_alloc;
    crd::u32            m_num_jobs;
    crd::u64            m_sub_fuel;
    std::atomic<bool>   m_cancel{false}; // §30 cooperative cancel flag (threaded to sub-interpreters)

    containers::HashMap<const Operation*, containers::Array<crd::i64>> m_map; // parallel_for op → its per-index outputs
};
} // namespace crd::ceir::host
