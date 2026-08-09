#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>

#include <crd/ceir/ir.hpp>
#include <crd/memory/construct.hpp>

namespace crd::ceir
{
OpId Dialect::register_op(containers::StringView op, const OpSpec& spec)
{
    // ⛔ Pure ⇒ zero effects (§26): a Pure op is CSE/DCE-safe, which ANY declared effect (even a read) disqualifies. The
    // hand-path live arm (the generator asserts the same at cook time); one direction only (effectless needn't be Pure).
    CRD_ASSERT_MSG((spec.traits & static_cast<u32>(OpTrait::Pure)) == 0U || spec.effects.size() == 0U,
                   "register_op: an OpTrait::Pure op must declare zero effects");
    for (usize i = 0; i < spec.effects.size(); ++i) // hand-path bound: a stray cast can't slip an out-of-vocabulary code in
    {
        CRD_ASSERT_MSG(spec.effects[i].family <= kLastEffectFamily && spec.effects[i].target <= EffectTarget::Result,
                       "register_op: effect family/target out of range");
    }
    CRD_ASSERT_MSG(spec.domain <= kLastEvalDomain, "register_op: eval domain out of range");
    const OpId kind = m_ctx->intern_op(m_name, op);
    if (m_ctx->m_op_infos.contains(kind.value)) { return kind; } // idempotent — keep the first registration
    OpInfo* const info = memory::construct<OpInfo>(m_ctx->m_arena);
    info->kind    = kind;
    info->name    = m_ctx->op_name(kind); // the interned "dialect.op"
    info->dialect = this;
    info->traits  = spec.traits;
    info->verify  = spec.verify;
    // COPY the effect records into the arena — the caller's span (a generated constexpr array OR a scope-local one) need
    // not outlive this call (the alloc-outlives-borrowers rule; records are trivially-copyable PODs).
    if (const auto n = static_cast<u32>(spec.effects.size()); n > 0U)
    {
        EffectRecord* const owned = memory::construct_array<EffectRecord>(m_ctx->m_arena, n);
        for (u32 i = 0; i < n; ++i) { owned[i] = spec.effects[i]; }
        info->effects     = owned;
        info->num_effects = n;
    }
    info->effects_fn  = spec.effects_fn; // §34 callee-derived-effects hook (CEIR-5c); nullptr for the common case
    info->determinism = spec.determinism;
    info->domain      = spec.domain;
    // ADR-0110 §2.1 native binding (CEIR-7a): promote to the runtime OpInfo (the §106 dep collector reads it). ⛔ Declared-
    // words-validated: an intrinsic MUST name its provider (else it is a native binding to nowhere). The generated static
    // provider string outlives the Context, so it is stored by reference (like OpInfo::name), not arena-copied.
    CRD_ASSERT_MSG(!spec.intrinsic || !spec.native_provider.empty(),
                   "register_op: an intrinsic op ([op.native]) must name a native_provider");
    info->intrinsic       = spec.intrinsic;
    info->native_provider = spec.native_provider;
    m_ctx->m_op_infos.insert(kind.value, info);
    return kind;
}

Dialect* Context::register_dialect(containers::StringView name)
{
    if (Dialect** existing = m_dialects.find(name)) { return *existing; } // idempotent
    const containers::StringView interned = intern_symbol(name);          // arena-stable map key
    Dialect* const d = memory::construct<Dialect>(m_arena, this, interned);
    m_dialects.insert(interned, d);
    return d;
}

Dialect* Context::dialect(containers::StringView name) noexcept
{
    Dialect** slot = m_dialects.find(name);
    return slot != nullptr ? *slot : nullptr;
}

const OpInfo* Context::op_info(OpId kind) const noexcept
{
    OpInfo* const* slot = m_op_infos.find(kind.value);
    return slot != nullptr ? *slot : nullptr;
}

const Dialect* Context::dialect_of(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind);
    return info != nullptr ? info->dialect : nullptr;
}

bool Context::has_trait(OpId kind, OpTrait t) const noexcept
{
    const OpInfo* const info = op_info(kind);
    return info != nullptr && (info->traits & static_cast<u32>(t)) != 0U;
}

bool Context::op_has_trait(const Operation& op, OpTrait t) const noexcept { return has_trait(op.kind(), t); }

containers::ConstSpan<EffectRecord> Context::op_effects(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind); // ⛔ callers: op_info==nullptr (unregistered) is MAXIMALLY effectful, not
    if (info == nullptr) { return {}; }       // effect-free — an empty span here means "registered + declared no effects"
    return containers::ConstSpan<EffectRecord>(info->effects, info->num_effects);
}

DeterminismClass Context::op_determinism(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind); // ⛔ same EMPTY≠UNKNOWN discipline: an UNREGISTERED kind returns Unspecified
    return info != nullptr ? info->determinism : DeterminismClass::Unspecified; // here too — callers must check op_info first
}

EvalDomain Context::op_domain(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind); // ⛔ EMPTY≠UNKNOWN again — an UNREGISTERED kind is Unspecified but UNKNOWN
    return info != nullptr ? info->domain : EvalDomain::Unspecified;
}

bool Context::verify(const Operation& op) const
{
    const OpInfo* const info = op_info(op.kind());
    if (info == nullptr || info->verify == nullptr) { return true; } // opaque / unknown-dialect ⇒ valid
    return info->verify(*this, op);
}

InterfaceId Context::intern_interface(containers::StringView name)
{
    for (usize i = 0; i < m_interface_names.size(); ++i)
    {
        if (m_interface_names[i] == name) { return InterfaceId{static_cast<u32>(i + 1U)}; }
    }
    m_interface_names.push_back(intern_symbol(name));
    return InterfaceId{static_cast<u32>(m_interface_names.size())};
}

void Context::register_interface(OpId kind, InterfaceId iface, const void* impl)
{
    OpInfo** slot = m_op_infos.find(kind.value);
    if (slot == nullptr) { return; } // only a registered op-kind can carry interfaces
    OpInfo* const info = *slot;
    for (u32 k = 0; k < info->num_ifaces; ++k)
    {
        if (info->ifaces[k].id == iface) // overwrite an existing implementation
        {
            info->ifaces[k].impl = impl;
            return;
        }
    }
    const u32          n     = info->num_ifaces; // grow by rebuild (old slice leaks into the arena)
    OpInterface* const grown = memory::construct_array<OpInterface>(m_arena, n + 1U);
    for (u32 k = 0; k < n; ++k) { grown[k] = info->ifaces[k]; }
    grown[n]         = OpInterface{iface, impl};
    info->ifaces     = grown;
    info->num_ifaces = n + 1U;
}

const void* Context::get_interface(OpId kind, InterfaceId iface) const noexcept
{
    const OpInfo* const info = op_info(kind);
    if (info == nullptr) { return nullptr; }
    for (u32 k = 0; k < info->num_ifaces; ++k)
    {
        if (info->ifaces[k].id == iface) { return info->ifaces[k].impl; }
    }
    return nullptr;
}
} // namespace crd::ceir
