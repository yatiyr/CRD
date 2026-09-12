#pragma once

// crd-ceir-gpu — CEIR-20b: the WORK-CHAIN BUILDER. `build_work_ceir` materializes an authored ceir.work program
// (queue_alloc + produce/consume/compact) from a payload-derived `WorkBuildDesc` into a fresh caller-owned Context, and
// verifies it with `find_work_misuse` (the verifier-first contract the executor assumes). This is the
// DEVICE-GENERATED-WORK sibling of `build_fullscreen_ceir`: the frame.pass `work.produce`/`work.consume`/`work.compact`
// mechanics cook to ceir.work ops through this ONE generic builder — NOTHING wavefront-specific lives here (the
// algorithm is the authored .frame.toml + .ckir kernels, the mandate-#1 line). ⛔ The lowering to a `LoweredCommand`
// plan (with `LoweredKind::DispatchIndirect`) is CEIR-20b STEP 3; step 2 EMITS + VERIFIES the ceir.work ops. Like
// build_fullscreen_ceir, the CALLER extracts a `WorkBuildDesc` from the frame's work passes + queue resources (keeping
// crd-ceir-gpu ⊥ crd-render-pass); this header names only CEIR types + core.
//
// ⛔ The queues are the CEIR IDENTITY of the frame's COUNTER/STRUCTURED buffers (work.queue_alloc, per 20a). Each queue
// + each binding carries a `source_param` — the resolver identity the record-time `WorkHooks` (step 4) map back to a
// device buffer (the build_fullscreen_ceir source-param precedent; the queue's resolver is `WorkHooks.resolve_queue`).

#include <crd/ceir/context.hpp>
#include <crd/ceir/gpu/lower.hpp> // LoweredCommand
#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::gpu
{
// One device-resident work queue the chain threads through (the CEIR identity of a frame COUNTER/STRUCTURED buffer).
struct WorkQueueDesc
{
    crd::u32 capacity = 1U;      // records (the frame resource's `count`; find_work_misuse: CapacityInvalid if < 1)
    crd::u32 record_stride = 1U; // bytes  (the frame resource's `stride`; find_work_misuse: RecordStrideInvalid if < 1)
    crd::u64 source_param =
        0U; // the resolver identity → the backing device buffer (the WorkHooks.resolve_queue hook, step 4)
};

// One bound resource of a work stage (a storage buffer the kernel reads/writes), in slot order. Its `access` becomes
// one comma-separated token of the op's `access` string ("r" | "w" | "rw"); find_work_misuse checks arity == binding
// count.
enum class WorkAccess : crd::u8
{
    Read = 0,
    Write,
    ReadWrite
};
struct WorkBinding
{
    crd::u64 source_param = 0U;           // the resolver identity → RecordContext.storage (record-time)
    WorkAccess access = WorkAccess::Read; // the per-binding access token
};

// The work op this stage materializes. produce = a grid dispatch APPENDING to `queue`; consume = an INDIRECT dispatch
// over `queue`'s DEVICE count (no host grid — the ceir.work distinction); compact = stream-compaction `src_queue` →
// `queue` (dst).
enum class WorkStageKind : crd::u8
{
    Produce = 0,
    Consume,
    Compact
};
struct WorkStageDesc
{
    WorkStageKind kind = WorkStageKind::Produce;
    crd::containers::StringView kernel; // the CKIR kernel symbol (@kernel) — must OUTLIVE the build call
    crd::u32 grid[3] = {1U, 1U, 1U};    // produce only (a const launch grid; consume is device-indirect)
    crd::u32 queue = 0U;                // index into WorkBuildDesc.queues — produce/consume queue / compact DST
    crd::u32 src_queue = 0U;            // compact only — the SRC queue index
    WorkBinding bindings[8] = {};
    crd::u32 num_bindings = 0U;
};

struct WorkBuildDesc
{
    WorkQueueDesc queues[8] = {};
    crd::u32 num_queues = 0U;
    WorkStageDesc stages[8] = {};
    crd::u32 num_stages = 0U;
};

// The DECLARED resources the built program created, correlated to their WorkBuildDesc `source_param`, so the caller
// maps each to a NAMED device buffer + builds the `WorkBinding{value, handle}` table `execute_work_lowered` resolves
// against (the `CeirDispatchAsset{block, binds}` precedent). One entry per `work.queue_alloc` result (the %queue Value,
// shared across the stages that flow through it) + one per binding `resource.declare` (fresh per stage). Key the
// caller's table by `value`.
struct WorkAssetResources
{
    struct Entry
    {
        crd::u64 source_param =
            0U; // the WorkBuildDesc queue/binding source_param (the caller's device-buffer identity)
        const Value* value =
            nullptr; // the CEIR Value execute_work_lowered looks up (a queue_alloc result / a declare result)
    };
    Entry entries[64] = {};
    crd::u32 count = 0U;
};

// Build the ceir.work program described by `desc` into `ctx` (⛔ a FRESH caller-owned Context — this registers the work
// / arith / resource dialects on it; the caller MUST keep `ctx` alive as long as the returned Module + `out_plan` are
// used, since the lowered commands hold `Operation*`s into `ctx`). Emits a `work.queue_alloc` per queue, then a
// produce/consume/compact op per stage, verifies with `find_work_misuse` (the builder's own correctness check), and on
// success LOWERS it into `out_plan` (produce → Dispatch, consume/compact → DispatchIndirect; queue_alloc emits nothing
// — the resolver provisions at execute) and fills `out_resources` (every queue + binding Value + its source_param — the
// caller's table). Both out-params CLEARED then filled. Returns the emitted Module* on success, or nullptr on a
// malformed `desc` (> 8 queues/stages/bindings, a stage naming a bad queue index or an empty kernel) or a
// `find_work_misuse` rejection (leaving both out-params empty).
[[nodiscard]] Module* build_work_ceir(Context& ctx, const WorkBuildDesc& desc,
                                      containers::Array<LoweredCommand>& out_plan, WorkAssetResources& out_resources);
} // namespace crd::ceir::gpu
