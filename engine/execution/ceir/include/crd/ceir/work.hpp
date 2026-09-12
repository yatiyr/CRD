#pragma once

// crd-ceir — the work dialect's QUEUE/RECORD TYPE-CLASSES + the find_work_misuse verifier (CEIR-20a, §43). The four work
// ops (generated from work.ceirop.toml) orchestrate DEVICE-GENERATED work: queue_alloc → produce (append records) →
// consume (an INDIRECT dispatch over the queue's DEVICE count) → compact (stream-compaction src→dst). Following the RT /
// SCENE attachment precedent, the two device handles are 8a Extern TYPE-CLASSES (the ROLE rides the TYPE, per the 12a
// one-source-of-truth doctrine): work.queue / work.record. ⛔ DISTINCT TypeIds — a %queue is the produce/consume/compact
// flow value; a %record is the queue's element identity (the type-system companion, for a future produce_one/consume_one).
// The handles are OPAQUE (zero-member Externs): CEIR sees an identity, never the device buffer + atomic counter the host
// lowering backs a queue with (the currency rule; the 17b shared-handle precedent). Two classes with identical (zero) params
// are DIFFERENT TypeIds (the ADR-0111 landmine) — queue != record. The OPS are generated (register_work_ops); this header
// adds the type-classes + factories + register_dialect + the SEMANTIC type-chain walk (find_work_misuse — its OWN misuse
// enum, parallel to find_rt_misuse; the generated per-op verifier owns STRUCTURAL conformance, this owns the type/vocab chain).
//
// ⛔⛔ NOT ceir.async: async (sec-37, ADR-0109 HOST-ONLY) is host-side STRUCTURED CONCURRENCY (CPU fork/join tokens via
// crd-jobs); work is DEVICE-side work GENERATION (records in device memory, device-computed dispatch counts, lowered to
// indirect/DGC/Work-Graphs). Different tier; a work program may run inside an async region but they NEVER share ops.

#include <crd/ceir/context.hpp>
#include <crd/ceir/gen/work_ops.hpp> // register_work_ops (the generated ops)
#include <crd/ceir/id.hpp>
#include <crd/containers/string_view.hpp>

namespace crd::ceir::work
{
// Register the `work` dialect: its generated ops (register_work_ops) + the two opaque queue/record type-classes. Idempotent.
// ⛔ Callers use THIS (not the raw generated register_work_ops) so the handle types exist for the ops' result types +
// find_work_misuse.
Dialect* register_dialect(Context& ctx);

// The interned handle type-classes = intern_type_class("work", "<name>"). Each a DISTINCT opaque 8a Extern class
// (queue != record). Work for a not-yet-registered context (content-hash id); register_dialect gives them their hook.
[[nodiscard]] TypeClassId queue_class(Context& ctx);
[[nodiscard]] TypeClassId record_class(Context& ctx);

// Build an OPAQUE handle TYPE (a zero-member 8a Extern of the given class — host-opaque, no CEIR-visible payload). The
// class must be registered (type_extern asserts its hook).
[[nodiscard]] TypeId type_queue(Context& ctx);
[[nodiscard]] TypeId type_record(Context& ctx);

// ── the work misuse walk — the SEMANTIC type-chain check (find_rt_misuse's parallel). work's OWN enum (NOT a widen of
// RtMisuseKind — so no -Werror=switch audit ripples). Append at end. ──
enum class WorkMisuseKind : u8
{
    None = 0,
    QueueTypeMismatch,    // a %queue operand (produce op(3) / consume op(0) / compact op(0),op(1)) is not work.queue-typed
    CapacityInvalid,      // work.queue_alloc's `capacity` is < 1 (or non-Int)
    RecordStrideInvalid,  // work.queue_alloc's `record_stride` is < 1 (or non-Int)
    // ── the dispatch-shape checks (the find_dispatch_misuse mirror — produce/consume/compact are compute.dispatch's
    //    work-generation siblings: an optional launch grid + variadic bindings + a per-binding `access` string). ⛔ ORDER is
    //    contractual (negatives pin the exact kind): queue type-class → dims-index (produce only) → access(kind-fold → tokens
    //    → arity) → bindings-resource. ──
    DimNotIndex,          // a launch-dim operand (work.produce operands 0-2) is not Index-typed
    AccessTokenInvalid,   // `access` is non-String, or a token is not exactly one of {r, w, rw} (the 12b wrong-kind fold)
    AccessArityMismatch,  // the `access` token count != the number of variadic bindings
    BindingNotResource,   // a variadic binding operand is not a resource-kinded value
};
[[nodiscard]] containers::StringView work_misuse_kind_name(WorkMisuseKind k) noexcept;

// The pointing result of the work type-chain walk: the FIRST misuse (pre-order), the offending `op`, and the `value` it
// points at (the bad operand; null for an attribute misuse).
struct WorkMisuse
{
    const Value*     value = nullptr;
    const Operation* op    = nullptr;
    WorkMisuseKind   kind  = WorkMisuseKind::None;
};
// The FIRST work misuse in module `m` (pre-order), or {None}. This owns the TYPE/VOCAB CHAIN: an op's %queue operand is
// work.queue-typed (produce op(3) / consume op(0) / compact op(0)+op(1)); queue_alloc's capacity/record_stride >= 1; AND the
// dispatch-shape operand types (the find_dispatch_misuse mirror — produce's dims Index-typed, the `access` tokens {r|w|rw}
// and their arity == the binding count, bindings resource-kinded). STRUCTURAL conformance (operand/result counts, required-attr
// PRESENCE + KIND — `access` String, `kernel` SymbolRef, `capacity`/`record_stride` Int) is the GENERATED per-op verifier
// (verify_* in work_ops.cpp). (Takes Context& — NOT const — because the check interns the operand class-ids;
// intern_type_class is non-const. The interning is a benign caching side-effect done once at entry; the walk itself is const.)
[[nodiscard]] WorkMisuse find_work_misuse(Context& ctx, const Module& m);
} // namespace crd::ceir::work
