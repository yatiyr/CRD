#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <charconv>
#include <type_traits>

namespace crd::hesap::sparse
{
// -----------------------------------------------------------------------
// Matrix Market (.mtx) I/O -- engine-side, in-memory (the caller does file I/O;
// keeps crd-hesap-sparse free of a platform/filesystem dependency). v1g-1.
//
// Supported (v1): banner `%%MatrixMarket matrix coordinate <type> <qual>` with
//   type  in {real, complex, integer, pattern}
//   qual  in {general, symmetric, skew-symmetric, hermitian}
// symmetric/skew-symmetric/hermitian are expanded to the full matrix on read
// (skew negates, hermitian conjugates the mirrored off-diagonal entry).
// Rejected (explicit error, NOT silent): `array` (dense) -- out of sparse scope.
// Writer: emits `coordinate general` (always correct) real / complex / pattern.
//
// Determinism: entries are added in file order to the TripletBuilder, then
// compress() canonicalises (column-ascending, summed duplicates) -> the result
// is independent of file ordering.
// -----------------------------------------------------------------------

struct MatrixMarketError
{
    bool                    ok = true;
    crd::containers::String message;
    explicit MatrixMarketError(crd::memory::IAllocator* alloc) : message(alloc) {}
};

namespace detail
{
template <typename T>
struct mm_is_complex : std::false_type
{
};
template <typename U>
struct mm_is_complex<crd::hesap::Complex<U>> : std::true_type
{
};
template <typename T>
inline constexpr bool mm_is_complex_v = mm_is_complex<T>::value;

template <typename T>
struct mm_real_of
{
    using type = T;
};
template <typename U>
struct mm_real_of<crd::hesap::Complex<U>>
{
    using type = U;
};

inline bool mm_isws(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Advance `p` over whitespace; returns false at end.
inline bool mm_skip_ws(const char*& p, const char* end) noexcept
{
    while (p < end && mm_isws(*p))
    {
        ++p;
    }
    return p < end;
}

// Read one whitespace-delimited token [tok_begin, tok_end).
inline bool mm_token(const char*& p, const char* end, const char*& tb, const char*& te) noexcept
{
    if (!mm_skip_ws(p, end))
    {
        return false;
    }
    tb = p;
    while (p < end && !mm_isws(*p))
    {
        ++p;
    }
    te = p;
    return true;
}

inline bool mm_parse_i64(const char* b, const char* e, crd::i64& out) noexcept
{
    auto [ptr, ec] = std::from_chars(b, e, out);
    return ec == std::errc() && ptr == e;
}

inline bool mm_parse_f64(const char* b, const char* e, crd::f64& out) noexcept
{
    auto [ptr, ec] = std::from_chars(b, e, out);
    return ec == std::errc() && ptr == e;
}

inline bool mm_token_ci_eq(const char* b, const char* e, const char* lit) noexcept
{
    for (const char* p = b; p < e; ++p, ++lit)
    {
        if (*lit == '\0')
        {
            return false;
        }
        char c = *p;
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
        if (c != *lit)
        {
            return false;
        }
    }
    return *lit == '\0';
}

template <typename T>
void mm_set_value(TripletBuilder<T>& tb, crd::u32 r, crd::u32 c, crd::f64 re, crd::f64 im)
{
    if constexpr (mm_is_complex_v<T>)
    {
        using U = typename mm_real_of<T>::type;
        tb.add(r, c, T{static_cast<U>(re), static_cast<U>(im)});
    }
    else
    {
        (void)im;
        tb.add(r, c, static_cast<T>(re));
    }
}
} // namespace detail

// Parse Matrix Market coordinate text into a compressed CSR. On error, sets
// err.ok=false + a message and returns an empty matrix.
template <typename T>
[[nodiscard]] SparseMatrix<T, SparseFormat::Csr> read_matrix_market(crd::containers::StringView text,
                                                                    crd::memory::IAllocator* alloc,
                                                                    MatrixMarketError&       err)
{
    using namespace detail;
    err.ok          = true;
    const char* p   = text.data();
    const char* end = text.data() + text.size();

    // ---- banner line ----
    const char *tb = nullptr, *te = nullptr;
    auto        fail = [&](const char* msg) {
        err.ok      = false;
        err.message = crd::containers::String{msg, alloc};
        return SparseMatrix<T, SparseFormat::Csr>(SparsePattern{alloc}, SparseValues<T>{alloc});
    };

    if (!mm_token(p, end, tb, te) || !mm_token_ci_eq(tb, te, "%%matrixmarket"))
    {
        return fail("matrix_market: missing %%MatrixMarket banner");
    }
    if (!mm_token(p, end, tb, te) || !mm_token_ci_eq(tb, te, "matrix"))
    {
        return fail("matrix_market: banner object must be 'matrix'");
    }
    if (!mm_token(p, end, tb, te))
    {
        return fail("matrix_market: truncated banner");
    }
    if (mm_token_ci_eq(tb, te, "array"))
    {
        return fail("matrix_market: 'array' (dense) format unsupported in v1 (sparse coordinate only)");
    }
    if (!mm_token_ci_eq(tb, te, "coordinate"))
    {
        return fail("matrix_market: banner format must be 'coordinate'");
    }
    if (!mm_token(p, end, tb, te))
    {
        return fail("matrix_market: truncated banner (type)");
    }
    const bool is_pattern = mm_token_ci_eq(tb, te, "pattern");
    const bool is_complex = mm_token_ci_eq(tb, te, "complex");
    const bool is_real    = mm_token_ci_eq(tb, te, "real");
    const bool is_integer = mm_token_ci_eq(tb, te, "integer");
    if (!(is_pattern || is_complex || is_real || is_integer))
    {
        return fail("matrix_market: type must be real/complex/integer/pattern");
    }
    if (!mm_token(p, end, tb, te))
    {
        return fail("matrix_market: truncated banner (qualifier)");
    }
    const bool q_general = mm_token_ci_eq(tb, te, "general");
    const bool q_sym     = mm_token_ci_eq(tb, te, "symmetric");
    const bool q_skew    = mm_token_ci_eq(tb, te, "skew-symmetric");
    const bool q_herm    = mm_token_ci_eq(tb, te, "hermitian");
    if (!(q_general || q_sym || q_skew || q_herm))
    {
        return fail("matrix_market: qualifier must be general/symmetric/skew-symmetric/hermitian");
    }

    // ---- skip comment lines, read size line "rows cols nnz" ----
    // Move to next line start, then skip any %-comment lines.
    auto next_nonblank_noncomment = [&]() -> bool {
        for (;;)
        {
            if (!mm_skip_ws(p, end))
            {
                return false;
            }
            if (*p == '%')
            {
                while (p < end && *p != '\n')
                {
                    ++p;
                }
                continue;
            }
            return true;
        }
    };
    if (!next_nonblank_noncomment())
    {
        return fail("matrix_market: missing size line");
    }
    crd::i64 nrows = 0, ncols = 0, nnz = 0;
    if (!mm_token(p, end, tb, te) || !mm_parse_i64(tb, te, nrows) || !mm_token(p, end, tb, te) ||
        !mm_parse_i64(tb, te, ncols) || !mm_token(p, end, tb, te) || !mm_parse_i64(tb, te, nnz))
    {
        return fail("matrix_market: malformed size line");
    }
    if (nrows < 0 || ncols < 0 || nnz < 0)
    {
        return fail("matrix_market: negative dimension");
    }

    TripletBuilder<T> builder(alloc, static_cast<crd::u32>(nrows), static_cast<crd::u32>(ncols));
    builder.reserve(static_cast<crd::usize>(nnz) * ((q_general) ? 1U : 2U));

    for (crd::i64 e = 0; e < nnz; ++e)
    {
        crd::i64 ri = 0, ci = 0;
        if (!mm_token(p, end, tb, te) || !mm_parse_i64(tb, te, ri) || !mm_token(p, end, tb, te) ||
            !mm_parse_i64(tb, te, ci))
        {
            return fail("matrix_market: truncated/malformed entry");
        }
        crd::f64 re = 1.0, im = 0.0;
        if (!is_pattern)
        {
            if (!mm_token(p, end, tb, te) || !mm_parse_f64(tb, te, re))
            {
                return fail("matrix_market: malformed entry value");
            }
            if (is_complex)
            {
                if (!mm_token(p, end, tb, te) || !mm_parse_f64(tb, te, im))
                {
                    return fail("matrix_market: malformed complex imaginary part");
                }
            }
        }
        const crd::u32 r = static_cast<crd::u32>(ri - 1);  // MM is 1-based
        const crd::u32 c = static_cast<crd::u32>(ci - 1);
        mm_set_value<T>(builder, r, c, re, im);
        if (!q_general && r != c)  // expand the mirrored entry
        {
            crd::f64 mre = re, mim = im;
            if (q_skew)
            {
                mre = -re;
                mim = -im;
            }
            else if (q_herm)
            {
                mim = -im;  // conjugate
            }
            mm_set_value<T>(builder, c, r, mre, mim);
        }
    }
    return builder.compress();
}

// Emit `coordinate general` Matrix Market text for a compressed CSR.
template <typename T>
[[nodiscard]] crd::containers::String write_matrix_market(const SparseMatrix<T, SparseFormat::Csr>& a,
                                                          crd::memory::IAllocator* alloc)
{
    constexpr bool       cplx = detail::mm_is_complex_v<T>;
    crd::containers::String out(alloc);
    const SparsePattern& pat = a.pattern();
    char                 buf[96];

    auto append = [&](const char* s) {
        for (const char* q = s; *q; ++q)
        {
            out.push_back(*q);
        }
    };
    auto append_i64 = [&](crd::i64 v) {
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        (void)ec;
        for (char* q = buf; q < ptr; ++q)
        {
            out.push_back(*q);
        }
    };
    auto append_f64 = [&](crd::f64 v) {
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        (void)ec;
        for (char* q = buf; q < ptr; ++q)
        {
            out.push_back(*q);
        }
    };

    append(cplx ? "%%MatrixMarket matrix coordinate complex general\n"
                : "%%MatrixMarket matrix coordinate real general\n");
    append_i64(static_cast<crd::i64>(pat.rows));
    out.push_back(' ');
    append_i64(static_cast<crd::i64>(pat.cols));
    out.push_back(' ');
    append_i64(static_cast<crd::i64>(pat.nnz()));
    out.push_back('\n');

    const crd::u32* outer = pat.outer_ptr.data();
    const crd::u32* inner = pat.inner_idx.data();
    const T*        v      = a.values().values.data();
    for (crd::u32 i = 0; i < pat.rows; ++i)
    {
        for (crd::u32 k = outer[i]; k < outer[i + 1]; ++k)
        {
            append_i64(static_cast<crd::i64>(i) + 1);
            out.push_back(' ');
            append_i64(static_cast<crd::i64>(inner[k]) + 1);
            out.push_back(' ');
            if constexpr (cplx)
            {
                append_f64(static_cast<crd::f64>(v[k].re));
                out.push_back(' ');
                append_f64(static_cast<crd::f64>(v[k].im));
            }
            else
            {
                append_f64(static_cast<crd::f64>(v[k]));
            }
            out.push_back('\n');
        }
    }
    return out;
}

} // namespace crd::hesap::sparse
