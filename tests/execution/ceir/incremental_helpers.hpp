#pragma once

// CEIR-9x shared TEST helpers — the content-addressed incremental-proof primitives, hoisted at the THIRD consumer
// (9d CAD; previously duplicated in 9a notebook + 9c DCC) per the absorb-with-a-real-consumer discipline: unify at the
// third consumer, not before. ⛔ A cell/modifier/feature's CONTENT hash = its FORMULA (op kind + operand DEP stable-ids
// — reorder/id-INDEPENDENT — + its attrs); its VALUE hash = its computed output. BOTH forced NONZERO (`| 1`): 0 is the
// `IncrementalDag`'s unset-revision default, so a node that computes 0 must stay distinguishable from "never computed".
// Self-contained (compiles as its own tidy TU). Host-only.

#include <crd/ceir/ceir.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir::test
{
[[nodiscard]] inline u64 fnv_mix(u64 h, u64 x) noexcept
{
    h ^= x;
    h *= 0x00000100000001B3ULL;
    return h;
}
// FORMULA hash: op kind + each operand's defining-op STABLE ID (dep identity, not creation order) + each attribute.
[[nodiscard]] inline u64 content_hash(const Operation& op, const Context& ctx) noexcept
{
    u64 h = 0xcbf29ce484222325ULL;
    h     = fnv_mix(h, op.kind().value);
    for (u32 i = 0; i < op.num_operands(); ++i)
    {
        const Operation* const d = op.operand(i)->defining_op();
        h                        = fnv_mix(h, d != nullptr ? d->stable_id().value : 0U);
    }
    for (u32 i = 0; i < op.num_attrs(); ++i)
    {
        const containers::StringView nm = op.attr_name(i);
        for (usize k = 0; k < nm.size(); ++k) { h = fnv_mix(h, static_cast<u64>(static_cast<unsigned char>(nm[k]))); }
        h = fnv_mix(h, static_cast<u64>(ctx.attr_value(op.attr_id_at(i)).i));
    }
    return h | 1ULL;
}
// VALUE (interface) hash of a computed scalar output — nonzero even for a value of 0.
[[nodiscard]] inline u64 value_hash(i64 v) noexcept
{
    u64       h    = 0xcbf29ce484222325ULL;
    const u64 bits = static_cast<u64>(v);
    for (int b = 0; b < 8; ++b) { h = fnv_mix(h, (bits >> (b * 8)) & 0xFFULL); }
    return h | 1ULL;
}
// Exact-set equality (size AND membership) — a SUPERSET must NOT pass silently (the tag-set contract).
[[nodiscard]] inline bool set_eq(const containers::Array<u64>& a, const u64* expected, usize n) noexcept
{
    if (a.size() != n) { return false; }
    for (usize i = 0; i < n; ++i)
    {
        bool found = false;
        for (usize j = 0; j < a.size(); ++j)
        {
            if (a[j] == expected[i]) { found = true; }
        }
        if (!found) { return false; }
    }
    return true;
}
} // namespace crd::ceir::test
