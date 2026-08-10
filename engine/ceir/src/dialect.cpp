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
    // ⛔ CEIR-8e (ADR-0115): a dialect may only SET core-minted trait bits, never MINT a new one — a stray bit would
    // silently collide with whoever the core later assigns it to. Plugin BEHAVIOR belongs in an op-INTERFACE, not a
    // trait bit. The kLastEffectFamily parallel (the factory leg of the closed-vocabulary guard).
    CRD_ASSERT_MSG((spec.traits & ~kKnownTraitsMask) == 0U,
                   "register_op: traits carry an out-of-vocabulary bit (dialects set core traits, never mint; see kKnownTraitsMask)");
    for (usize i = 0; i < spec.effects.size(); ++i) // hand-path bound: a stray cast can't slip an out-of-vocabulary code in
    {
        const EffectRecord& e = spec.effects[i];
        CRD_ASSERT_MSG(e.family <= kLastEffectFamily && e.target <= kLastLocationKind,
                       "register_op: effect family/location-kind out of range");
        // CEIR-8c (ADR-0113) the junk-field guard (the 8a/8b canonicality discipline applied to EffectRecord):
        // location_class is set IFF the target is the Extern location door — else it prints/analyses inconsistently.
        CRD_ASSERT_MSG((e.target == EffectTarget::Extern) == e.location_class.valid(),
                       "register_op: EffectRecord.location_class must be set iff target == Extern");
        // the FACTORY leg of the verify triple: a REGISTERED Extern location class's verify hook validates the record
        // (an UNREGISTERED class is preserved — the hazard analysis treats it as Universe).
        CRD_ASSERT_MSG(m_ctx->effect_location_valid(e),
                       "register_op: the effect-location class verify hook rejected this record");
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
    // CEIR-8f (ADR-0116): intern the required-capability NAMES to CapabilityIds and arena-copy the set (the effects
    // precedent — the borrowed name span need not outlive the call). The program's required set is the module-wide union.
    if (const auto n = static_cast<u32>(spec.capabilities.size()); n > 0U)
    {
        CapabilityId* const owned = memory::construct_array<CapabilityId>(m_ctx->m_arena, n);
        for (u32 i = 0; i < n; ++i)
        {
            // ⛔ declared-words-validated (the intrinsic-must-name-provider parallel): an EMPTY capability name hashes
            // to the FNV offset basis — a valid-looking phantom id that would smuggle into the interface hash. Reject it.
            CRD_ASSERT_MSG(!spec.capabilities[i].empty(), "register_op: a required capability name must be non-empty");
            owned[i] = m_ctx->intern_capability(spec.capabilities[i]);
        }
        info->required_capabilities = owned;
        info->num_capabilities      = n;
    }
    m_ctx->m_op_infos.insert(kind.value, info);
    return kind;
}

containers::ConstSpan<CapabilityId> Context::op_capabilities(OpId kind) const noexcept
{
    const OpInfo* const info = op_info(kind); // ⛔ EMPTY≠UNKNOWN: unregistered ⇒ {} here; program_capabilities adds
    if (info == nullptr) { return {}; }       // external.process for an unknown kind (an empty span = registered, none)
    return containers::ConstSpan<CapabilityId>(info->required_capabilities, info->num_capabilities);
}

TypeClassId Dialect::register_type_class(containers::StringView cls, const TypeClassSpec& spec)
{
    const TypeClassId id = m_ctx->intern_type_class(m_name, cls);
    if (m_ctx->m_type_classes.contains(id.value)) { return id; } // idempotent — keep the first registration (late-bind ok)
    TypeClassInfo* const info = memory::construct<TypeClassInfo>(m_ctx->m_arena);
    info->id      = id;
    info->name    = m_ctx->type_class_name(id); // the interned "dialect.class"
    info->dialect = this;
    info->verify  = spec.verify;
    info->version = spec.version;
    m_ctx->m_type_classes.insert(id.value, info);
    return id;
}

AttrClassId Dialect::register_attr_class(containers::StringView cls, const AttrClassSpec& spec)
{
    const AttrClassId id = m_ctx->intern_attr_class(m_name, cls);
    if (m_ctx->m_attr_classes.contains(id.value)) { return id; } // idempotent — keep the first registration (late-bind ok)
    AttrClassInfo* const info = memory::construct<AttrClassInfo>(m_ctx->m_arena);
    info->id      = id;
    info->name    = m_ctx->attr_class_name(id); // the interned "dialect.attr"
    info->dialect = this;
    info->verify  = spec.verify;
    info->version = spec.version;
    m_ctx->m_attr_classes.insert(id.value, info);
    return id;
}

LocationClassId Dialect::register_location_class(containers::StringView cls, const LocationClassSpec& spec)
{
    const LocationClassId id = m_ctx->intern_location_class(m_name, cls);
    if (m_ctx->m_location_classes.contains(id.value)) { return id; } // idempotent — keep the first registration (late-bind)
    LocationClassInfo* const info = memory::construct<LocationClassInfo>(m_ctx->m_arena);
    info->id             = id;
    info->name           = m_ctx->location_class_name(id); // the interned "dialect.location"
    info->dialect        = this;
    info->verify         = spec.verify;
    info->resource_class = spec.resource_class; // the hazard analysis reads this to place the effect in a conflict class
    info->version        = spec.version;
    m_ctx->m_location_classes.insert(id.value, info);
    return id;
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
    // CEIR-8e (ADR-0115): the id is the FNV of the name (byte-identical to interface_hash_ct, so this returns the SAME
    // id a typed interface's compile-time `T::kId` holds). The name table is now a REVERSE-lookup index (diagnostics).
    const u64 h = containers::hash_string(name.data(), name.size());
    for (usize i = 0; i < m_interface_names.size(); ++i)
    {
        if (m_interface_names[i].hash == h) { return InterfaceId{h}; } // already registered (dedup by hash)
    }
    m_interface_names.push_back(OpName{h, intern_symbol(name)});
    return InterfaceId{h};
}

containers::StringView Context::interface_name(InterfaceId id) const noexcept
{
    for (usize i = 0; i < m_interface_names.size(); ++i)
    {
        if (m_interface_names[i].hash == id.value) { return m_interface_names[i].name; }
    }
    return containers::StringView{}; // an id whose name was never interned (a compile-time kId used without intern)
}

void Context::register_interface(OpId kind, InterfaceId iface, const void* impl)
{
    OpInfo** slot = m_op_infos.find(kind.value);
    // ⛔ CEIR-8e (ADR-0115): "op first" is the contract — an interface impl lives ON the op-kind's OpInfo, so there is
    // nowhere to park it before the op registers. On THE typed extension surface a silent drop is a plugin footgun
    // (a mysterious nullptr from get_op_interface far away), so this is LOUD (assert) + release-safe (no-op).
    CRD_ASSERT_MSG(slot != nullptr,
                   "register_interface: register the op-kind (register_op) BEFORE binding an interface to it");
    if (slot == nullptr) { return; }
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
