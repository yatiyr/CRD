#pragma once

// crd-ceir — the REWRITE / CONVERSION skeleton (CEIR-8g, ADR-0117, U-§67/U-§74). ⛔ DATA STRUCTURES + a caller-driven
// per-op apply ONLY — the greedy/worklist DRIVER (traversal order, fixpoint, iterator invalidation) IS the CEIR-26
// work and is deliberately RESERVED. A `RewritePattern` matches + rewrites; a `ConversionTarget` declares per-op-kind
// legality; ⛔ an UNLISTED kind is ILLEGAL (the EMPTY≠UNKNOWN default — an unknown op must not read as legal).

#include <crd/ceir/context.hpp>
#include <crd/ceir/id.hpp>
#include <crd/ceir/ir.hpp>
#include <crd/containers/array.hpp>
#include <crd/core/types.hpp>

namespace crd::ceir
{
// A rewrite pattern: `match` decides if it applies to an op; `rewrite` applies it (a skeleton hook — real transforms
// land at CEIR-26). Both are plain fn-ptrs (the verify-hook precedent), so the core never special-cases a dialect.
struct RewritePattern
{
    bool (*match)(const Context&, const Operation&)   = nullptr; // nullptr ⇒ never matches
    void (*rewrite)(Context&, Operation&)             = nullptr; // nullptr ⇒ match-only (a legality probe)
};

// ⛔ CALLER-DRIVEN, PER-OP: match then rewrite THIS op; returns true iff it applied. NOT a block walk — a walk that
// rewrites while iterating is the iterator-invalidation trap whose solution IS the reserved CEIR-26 driver.
[[nodiscard]] inline bool try_apply(const RewritePattern& p, Context& ctx, Operation& op)
{
    if (p.match == nullptr || !p.match(ctx, op)) { return false; }
    if (p.rewrite != nullptr) { p.rewrite(ctx, op); }
    return true;
}

// The legality of an op-kind under a conversion. ⛔ `Illegal = 0` so the DEFAULT (an unlisted kind) is Illegal.
// NOLINTNEXTLINE(performance-enum-size)
enum class Legality : u8
{
    Illegal = 0,
    Legal,
    Dynamic, // legal iff a per-instance predicate says so
};

// A ConversionTarget: per-op-kind legality for a lowering/legalization. `is_legal(op)` is the query a future driver
// loops on. ⛔ an UNLISTED kind ⇒ Illegal (EMPTY≠UNKNOWN).
class ConversionTarget
{
public:
    explicit ConversionTarget(memory::IAllocator* alloc) : m_rules(alloc) {}

    void set_legal(OpId kind) { set(kind, Legality::Legal, nullptr); }
    void set_illegal(OpId kind) { set(kind, Legality::Illegal, nullptr); }
    void set_dynamic(OpId kind, bool (*pred)(const Context&, const Operation&)) { set(kind, Legality::Dynamic, pred); }

    [[nodiscard]] bool is_legal(const Context& ctx, const Operation& op) const noexcept
    {
        for (usize i = 0; i < m_rules.size(); ++i)
        {
            if (m_rules[i].kind == op.kind())
            {
                if (m_rules[i].legality == Legality::Legal) { return true; }
                if (m_rules[i].legality == Legality::Illegal) { return false; }
                return m_rules[i].dynamic == nullptr || m_rules[i].dynamic(ctx, op); // Dynamic
            }
        }
        return false; // ⛔ UNLISTED ⇒ Illegal
    }

private:
    struct Rule
    {
        OpId     kind;
        Legality legality;
        bool     (*dynamic)(const Context&, const Operation&);
    };
    void set(OpId kind, Legality l, bool (*pred)(const Context&, const Operation&))
    {
        for (usize i = 0; i < m_rules.size(); ++i)
        {
            if (m_rules[i].kind == kind) // overwrite an existing rule (last set wins)
            {
                m_rules[i].legality = l;
                m_rules[i].dynamic  = pred;
                return;
            }
        }
        m_rules.push_back(Rule{kind, l, pred});
    }
    containers::Array<Rule> m_rules;
};
} // namespace crd::ceir
