#include <crd/hesap/cli/arg_value.hpp>

namespace crd::hesap::cli
{

ArgValue::ArgValue(crd::memory::IAllocator* alloc) noexcept
    : m_alloc(alloc), m_string(alloc), m_f64_array(alloc), m_i64_array(alloc)
{
}

void ArgValue::set_bool(bool v) noexcept
{
    m_kind = Kind::Bool;
    m_scalar.b = v;
}

void ArgValue::set_i64(crd::i64 v) noexcept
{
    m_kind = Kind::I64;
    m_scalar.i = v;
}

void ArgValue::set_u64(crd::u64 v) noexcept
{
    m_kind = Kind::U64;
    m_scalar.u = v;
}

void ArgValue::set_f64(crd::f64 v) noexcept
{
    m_kind = Kind::F64;
    m_scalar.f = v;
}

void ArgValue::set_complex64(const crd::hesap::Complex64& v) noexcept
{
    m_kind = Kind::Complex64;
    m_scalar.c = v;
}

void ArgValue::set_string(crd::containers::StringView v)
{
    m_kind = Kind::String;
    m_string = crd::containers::String{v, m_alloc};
}

void ArgValue::set_f64_array(crd::containers::ConstSpan<crd::f64> v)
{
    m_kind = Kind::F64Array;
    m_f64_array.clear();
    m_f64_array.reserve(v.size());
    for (crd::f64 x : v)
    {
        m_f64_array.push_back(x);
    }
}

void ArgValue::set_i64_array(crd::containers::ConstSpan<crd::i64> v)
{
    m_kind = Kind::I64Array;
    m_i64_array.clear();
    m_i64_array.reserve(v.size());
    for (crd::i64 x : v)
    {
        m_i64_array.push_back(x);
    }
}

void ArgValue::set_matrix_id(crd::hesap::MatrixId v) noexcept
{
    m_kind = Kind::MatrixId;
    m_scalar.mid = v;
}

void ArgValue::set_vector_id(crd::hesap::VectorId v) noexcept
{
    m_kind = Kind::VectorId;
    m_scalar.vid = v;
}

std::optional<bool> ArgValue::as_bool() const noexcept
{
    return m_kind == Kind::Bool ? std::optional<bool>{m_scalar.b} : std::nullopt;
}

std::optional<crd::i64> ArgValue::as_i64() const noexcept
{
    return m_kind == Kind::I64 ? std::optional<crd::i64>{m_scalar.i} : std::nullopt;
}

std::optional<crd::u64> ArgValue::as_u64() const noexcept
{
    return m_kind == Kind::U64 ? std::optional<crd::u64>{m_scalar.u} : std::nullopt;
}

std::optional<crd::f64> ArgValue::as_f64() const noexcept
{
    return m_kind == Kind::F64 ? std::optional<crd::f64>{m_scalar.f} : std::nullopt;
}

std::optional<crd::hesap::Complex64> ArgValue::as_complex64() const noexcept
{
    return m_kind == Kind::Complex64 ? std::optional<crd::hesap::Complex64>{m_scalar.c} : std::nullopt;
}

crd::containers::StringView ArgValue::as_string() const noexcept
{
    if (m_kind != Kind::String)
    {
        return crd::containers::StringView{};
    }
    return crd::containers::StringView{m_string.c_str(), m_string.size()};
}

crd::containers::ConstSpan<crd::f64> ArgValue::as_f64_array() const noexcept
{
    if (m_kind != Kind::F64Array)
    {
        return crd::containers::ConstSpan<crd::f64>{};
    }
    return crd::containers::ConstSpan<crd::f64>{m_f64_array.data(), m_f64_array.size()};
}

crd::containers::ConstSpan<crd::i64> ArgValue::as_i64_array() const noexcept
{
    if (m_kind != Kind::I64Array)
    {
        return crd::containers::ConstSpan<crd::i64>{};
    }
    return crd::containers::ConstSpan<crd::i64>{m_i64_array.data(), m_i64_array.size()};
}

std::optional<crd::hesap::MatrixId> ArgValue::as_matrix_id() const noexcept
{
    return m_kind == Kind::MatrixId ? std::optional<crd::hesap::MatrixId>{m_scalar.mid} : std::nullopt;
}

std::optional<crd::hesap::VectorId> ArgValue::as_vector_id() const noexcept
{
    return m_kind == Kind::VectorId ? std::optional<crd::hesap::VectorId>{m_scalar.vid} : std::nullopt;
}

} // namespace crd::hesap::cli
