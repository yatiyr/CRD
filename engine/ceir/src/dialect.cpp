#include <crd/ceir/context.hpp>
#include <crd/ceir/dialect.hpp>

#include <crd/ceir/ir.hpp>
#include <crd/memory/construct.hpp>

namespace crd::ceir
{
OpId Dialect::register_op(containers::StringView op, u32 trait_bits, VerifyFn verify_fn)
{
    const OpId kind = m_ctx->intern_op(m_name, op);
    if (m_ctx->m_op_infos.contains(kind.value)) { return kind; } // idempotent — keep the first registration
    OpInfo* const info = memory::construct<OpInfo>(m_ctx->m_arena);
    info->kind    = kind;
    info->name    = m_ctx->op_name(kind); // the interned "dialect.op"
    info->dialect = this;
    info->traits  = trait_bits;
    info->verify  = verify_fn;
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
