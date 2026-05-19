#include <crd/hesap/cli/command_registry.hpp>

#include <crd/core/assert.hpp>
#include <crd/memory/memory.hpp>

#include <utility>

namespace crd::hesap::cli
{

namespace
{
[[nodiscard]] ArgValue& upsert(
    crd::containers::HashMap<crd::containers::String, ArgValue>& values,
    crd::memory::IAllocator* alloc,
    crd::containers::StringView name)
{
    crd::containers::String key{name, alloc};
    ArgValue* existing = values.find(key);
    if (existing != nullptr)
    {
        return *existing;
    }
    values.insert(key, ArgValue{alloc});
    return *values.find(key);
}
} // namespace

void CommandArgs::set_bool(crd::containers::StringView name, bool v)
{
    upsert(values, alloc, name).set_bool(v);
}

void CommandArgs::set_i64(crd::containers::StringView name, crd::i64 v)
{
    upsert(values, alloc, name).set_i64(v);
}

void CommandArgs::set_u64(crd::containers::StringView name, crd::u64 v)
{
    upsert(values, alloc, name).set_u64(v);
}

void CommandArgs::set_f64(crd::containers::StringView name, crd::f64 v)
{
    upsert(values, alloc, name).set_f64(v);
}

void CommandArgs::set_complex64(crd::containers::StringView name, const crd::hesap::Complex64& v)
{
    upsert(values, alloc, name).set_complex64(v);
}

void CommandArgs::set_string(crd::containers::StringView name, crd::containers::StringView v)
{
    upsert(values, alloc, name).set_string(v);
}

void CommandArgs::set_f64_array(crd::containers::StringView name, crd::containers::ConstSpan<crd::f64> v)
{
    upsert(values, alloc, name).set_f64_array(v);
}

void CommandArgs::set_i64_array(crd::containers::StringView name, crd::containers::ConstSpan<crd::i64> v)
{
    upsert(values, alloc, name).set_i64_array(v);
}

void CommandArgs::set_matrix_id(crd::containers::StringView name, crd::hesap::MatrixId v)
{
    upsert(values, alloc, name).set_matrix_id(v);
}

void CommandArgs::set_vector_id(crd::containers::StringView name, crd::hesap::VectorId v)
{
    upsert(values, alloc, name).set_vector_id(v);
}

const ArgValue* CommandArgs::find(crd::containers::StringView name) const noexcept
{
    crd::containers::String key{name, alloc};
    return values.find(key);
}

std::optional<crd::f64> CommandArgs::get_f64(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_f64() : std::nullopt;
}

std::optional<crd::i64> CommandArgs::get_i64(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_i64() : std::nullopt;
}

std::optional<crd::u64> CommandArgs::get_u64(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_u64() : std::nullopt;
}

std::optional<bool> CommandArgs::get_bool(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_bool() : std::nullopt;
}

std::optional<crd::hesap::Complex64> CommandArgs::get_complex64(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_complex64() : std::nullopt;
}

crd::containers::StringView CommandArgs::get_string(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_string() : crd::containers::StringView{};
}

crd::containers::ConstSpan<crd::f64> CommandArgs::get_f64_array(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_f64_array() : crd::containers::ConstSpan<crd::f64>{};
}

crd::containers::ConstSpan<crd::i64> CommandArgs::get_i64_array(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_i64_array() : crd::containers::ConstSpan<crd::i64>{};
}

std::optional<crd::hesap::MatrixId> CommandArgs::get_matrix_id(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_matrix_id() : std::nullopt;
}

std::optional<crd::hesap::VectorId> CommandArgs::get_vector_id(crd::containers::StringView name) const
{
    const ArgValue* v = find(name);
    return v != nullptr ? v->as_vector_id() : std::nullopt;
}


CommandRegistry::CommandRegistry()
    : m_records(crd::memory::default_allocator()),
      m_index(crd::memory::default_allocator()),
      m_pointer_cache(crd::memory::default_allocator())
{
}

CommandRegistry& CommandRegistry::global() noexcept
{
    // Meyers singleton. Lazy on first call; thread-safe construction per
    // [stmt.dcl]/4. Defeats static-init-order fiasco — per advisor 2026-05-19
    // and ADR-0081 §Consequences.
    static CommandRegistry instance;
    return instance;
}

bool CommandRegistry::register_command(CommandSchema schema, CommandImpl impl) noexcept
{
    CRD_ASSERT_MSG(!schema.name.empty(), "CommandRegistry::register_command requires a non-empty name");

    if (m_index.contains(schema.name))
    {
        // Duplicate names are a programmer error — same command registered
        // twice from different TUs. The static-init macro per ADR-0081 §7
        // means duplicate detection at registration is the right gate.
        CRD_ASSERT_MSG(false, "CommandRegistry: duplicate command name registered");
        return false;
    }

    const crd::usize new_idx = m_records.size();
    crd::containers::String name_copy{schema.name.c_str(), m_index.allocator()};
    CommandRecord rec{m_records.allocator()};
    rec.schema = std::move(schema);
    rec.impl = impl;
    m_records.push_back(std::move(rec));
    m_index.insert(name_copy, new_idx);
    m_pointer_cache_stale = true;
    return true;
}

const CommandRecord* CommandRegistry::find(crd::containers::StringView name) const noexcept
{
    crd::containers::String key{name, m_index.allocator()};
    const auto* idx = m_index.find(key);
    if (idx == nullptr)
    {
        return nullptr;
    }
    return &m_records[*idx];
}

crd::containers::ConstSpan<const CommandRecord*> CommandRegistry::all() const noexcept
{
    if (m_pointer_cache_stale)
    {
        m_pointer_cache.clear();
        m_pointer_cache.reserve(m_records.size());
        for (const CommandRecord& r : m_records)
        {
            m_pointer_cache.push_back(&r);
        }
        m_pointer_cache_stale = false;
    }
    return crd::containers::ConstSpan<const CommandRecord*>{m_pointer_cache.data(), m_pointer_cache.size()};
}

crd::usize CommandRegistry::size() const noexcept
{
    return m_records.size();
}

void CommandRegistry::clear_for_tests() noexcept
{
    m_records.clear();
    m_index.clear();
    m_pointer_cache.clear();
    m_pointer_cache_stale = true;
}

} // namespace crd::hesap::cli
