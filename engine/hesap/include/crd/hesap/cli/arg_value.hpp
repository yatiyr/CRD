#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/handles.hpp>
#include <crd/memory/allocator.hpp>

#include <optional>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// ArgValue — discriminated-union typed param value.
//
// Per ADR-0081 §3 + the v0b elite scope (2026-05-19): the CLI substrate
// needs typed param storage so command impls can read their arguments
// without hand-rolled JSON parsing. ArgValue holds one of:
//   - Empty (default)
//   - Bool / I64 / U64 / F64 / Complex64 scalars
//   - String (allocator-owned)
//   - F64Array / I64Array (allocator-owned)
//   - MatrixId / VectorId handles
//
// Setters mutate kind to the new variant; previous variable-size storage
// is cleared. Getters return std::optional<T> for scalars (empty if kind
// mismatch) and an empty ConstSpan/StringView for arrays/strings.
//
// Scope note: this is the v0b minimum surface for BLAS L1. Future kinds
// (Path / Enum / typed Tensor handles) append later — additive only,
// matches ADR-0081 §2 schema-versioning policy.
// -----------------------------------------------------------------------

class ArgValue
{
public:
    enum class Kind : crd::u8
    {
        Empty = 0,
        Bool = 1,
        I64 = 2,
        U64 = 3,
        F64 = 4,
        Complex64 = 5,
        String = 6,
        F64Array = 7,
        I64Array = 8,
        MatrixId = 9,
        VectorId = 10,
    };

    explicit ArgValue(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) noexcept;

    [[nodiscard]] Kind kind() const noexcept { return m_kind; }
    [[nodiscard]] crd::memory::IAllocator* allocator() const noexcept { return m_alloc; }

    // ---- Setters --------------------------------------------------

    void set_bool(bool v) noexcept;
    void set_i64(crd::i64 v) noexcept;
    void set_u64(crd::u64 v) noexcept;
    void set_f64(crd::f64 v) noexcept;
    void set_complex64(const crd::hesap::Complex64& v) noexcept;
    void set_string(crd::containers::StringView v);
    void set_f64_array(crd::containers::ConstSpan<crd::f64> v);
    void set_i64_array(crd::containers::ConstSpan<crd::i64> v);
    void set_matrix_id(crd::hesap::MatrixId v) noexcept;
    void set_vector_id(crd::hesap::VectorId v) noexcept;

    // ---- Typed getters --------------------------------------------

    [[nodiscard]] std::optional<bool> as_bool() const noexcept;
    [[nodiscard]] std::optional<crd::i64> as_i64() const noexcept;
    [[nodiscard]] std::optional<crd::u64> as_u64() const noexcept;
    [[nodiscard]] std::optional<crd::f64> as_f64() const noexcept;
    [[nodiscard]] std::optional<crd::hesap::Complex64> as_complex64() const noexcept;
    [[nodiscard]] crd::containers::StringView as_string() const noexcept;
    [[nodiscard]] crd::containers::ConstSpan<crd::f64> as_f64_array() const noexcept;
    [[nodiscard]] crd::containers::ConstSpan<crd::i64> as_i64_array() const noexcept;
    [[nodiscard]] std::optional<crd::hesap::MatrixId> as_matrix_id() const noexcept;
    [[nodiscard]] std::optional<crd::hesap::VectorId> as_vector_id() const noexcept;

private:
    Kind m_kind = Kind::Empty;
    crd::memory::IAllocator* m_alloc = crd::memory::default_allocator();

    // Scalar union — kept as POD bytes; the active member is selected by m_kind.
    union ScalarStorage
    {
        bool b;
        crd::i64 i;
        crd::u64 u;
        crd::f64 f;
        crd::hesap::Complex64 c;
        crd::hesap::MatrixId mid;
        crd::hesap::VectorId vid;

        ScalarStorage() noexcept : i(0) {}
    };
    ScalarStorage m_scalar{};

    // Variable-size storage. Only one is non-empty at any time (matches m_kind).
    crd::containers::String m_string;
    crd::containers::Array<crd::f64> m_f64_array;
    crd::containers::Array<crd::i64> m_i64_array;
};

} // namespace crd::hesap::cli
