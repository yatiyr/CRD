// Sparse CLI command registration. Phase 3.1.6 v1a-2.
//
// Registers 12 CommandSchemas (3 ops x 4 type variants) via the
// CRD_HESAP_CLI_REGISTER_MODULE static-init hook (ADR-0081 §7 + §10):
//   from_triplets : COO triplet arrays -> CSR; returns nnz (Scalar).
//   to_csr        : COO triplet arrays -> CSR; returns the compressed value
//                   array (BinaryBlob, f64; complex flattened {re,im,...}).
//   build         : like from_triplets, but a `uncompressed` flag routes
//                   assembly through the make_uncompressed + coeff_ref insert
//                   path (then make_compressed) -- a real CLI consumer of the
//                   uncompressed storage mode. Returns nnz (Scalar).
//
// Stateless wire shape (v0b precedent, D14): no SparseRegistry yet. Triplets
// travel as parallel I64Array (rows, cols) + F64Array (values; complex
// flattened length 2*nt). A handle-returning surface arrives with a
// SparseRegistry consumer (filed follow-on), bumping schema major version.

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/complex.hpp>
#include <crd/hesap/sparse/cli_anchor.hpp>
#include <crd/hesap/sparse/sparse.hpp>

#include <utility>

namespace crd::hesap::sparse
{
void register_sparse_cli_anchor() noexcept {}
} // namespace crd::hesap::sparse

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::sparse;

CommandResult error_result(crd::memory::IAllocator* alloc, const char* msg)
{
    CommandResult r{alloc};
    r.ok = false;
    ResultError e{alloc};
    e.error_kind = crd::containers::String{"InvalidArgument", alloc};
    e.error_message = crd::containers::String{msg, alloc};
    r.value = std::move(e);
    return r;
}

CommandResult scalar_result(crd::memory::IAllocator* alloc, crd::f64 value)
{
    CommandResult r{alloc};
    r.ok = true;
    r.value = ResultScalarF64{value};
    return r;
}

CommandResult text_result(crd::memory::IAllocator* alloc, crd::containers::String&& text)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultText t{alloc};
    t.text  = std::move(text);
    r.value = std::move(t);
    return r;
}

CommandResult blob_result(crd::memory::IAllocator* alloc, crd::containers::ConstSpan<crd::f64> values)
{
    CommandResult r{alloc};
    r.ok = true;
    ResultBinaryBlob blob{alloc};
    const auto* raw = reinterpret_cast<const crd::u8*>(values.data());
    const crd::usize n_bytes = values.size() * sizeof(crd::f64);
    blob.bytes.reserve(n_bytes);
    for (crd::usize i = 0; i < n_bytes; ++i)
    {
        blob.bytes.push_back(raw[i]);
    }
    r.value = std::move(blob);
    return r;
}

// Validate + extract the common triplet inputs. Returns false on shape error.
struct TripletInputs
{
    crd::u32                              rows = 0;
    crd::u32                              cols = 0;
    crd::containers::ConstSpan<crd::i64>  tr;
    crd::containers::ConstSpan<crd::i64>  tc;
    crd::containers::ConstSpan<crd::f64>  vals;
};

bool read_inputs(const CommandArgs& args, bool complex, TripletInputs& out, const char*& err)
{
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols)
    {
        err = "rows and cols are required";
        return false;
    }
    out.rows = static_cast<crd::u32>(*rows);
    out.cols = static_cast<crd::u32>(*cols);
    out.tr = args.get_i64_array("triplet_rows");
    out.tc = args.get_i64_array("triplet_cols");
    out.vals = args.get_f64_array("values");
    const crd::usize nt = out.tr.size();
    if (out.tc.size() != nt)
    {
        err = "triplet_rows and triplet_cols must have equal length";
        return false;
    }
    const crd::usize expect_vals = complex ? nt * 2 : nt;
    if (out.vals.size() != expect_vals)
    {
        err = complex ? "complex values must be flattened length 2*nt" : "values length must equal triplet count";
        return false;
    }
    return true;
}

template <typename T>
void fill_builder_real(TripletBuilder<T>& b, const TripletInputs& in)
{
    for (crd::usize k = 0; k < in.tr.size(); ++k)
    {
        b.add(static_cast<crd::u32>(in.tr[k]), static_cast<crd::u32>(in.tc[k]), static_cast<T>(in.vals[k]));
    }
}

template <typename U>
void fill_builder_complex(TripletBuilder<Complex<U>>& b, const TripletInputs& in)
{
    for (crd::usize k = 0; k < in.tr.size(); ++k)
    {
        const Complex<U> v{static_cast<U>(in.vals[2 * k]), static_cast<U>(in.vals[2 * k + 1])};
        b.add(static_cast<crd::u32>(in.tr[k]), static_cast<crd::u32>(in.tc[k]), v);
    }
}

// ---- from_triplets : returns nnz ---------------------------------------

template <typename T>
CommandResult impl_from_triplets_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto m = b.compress();
    return scalar_result(args.alloc, static_cast<crd::f64>(m.nnz()));
}

template <typename U>
CommandResult impl_from_triplets_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto m = b.compress();
    return scalar_result(args.alloc, static_cast<crd::f64>(m.nnz()));
}

// ---- to_csr : returns the compressed value array (f64 blob) ------------

template <typename T>
CommandResult impl_to_csr_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto m = b.compress();
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(m.nnz());
    for (crd::usize i = 0; i < m.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(m.values().values[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_to_csr_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto m = b.compress();
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(m.nnz() * 2);
    for (crd::usize i = 0; i < m.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(m.values().values[i].re));
        out.push_back(static_cast<crd::f64>(m.values().values[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- build : optional uncompressed path; returns nnz -------------------

template <typename T>
CommandResult impl_build_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const bool uncompressed = args.get_bool("uncompressed").value_or(false);
    crd::usize nnz = 0;
    if (uncompressed)
    {
        auto m = SparseMatrix<T, SparseFormat::Csr>::make_uncompressed(args.alloc, in.rows, in.cols);
        for (crd::usize k = 0; k < in.tr.size(); ++k)
        {
            T& slot = m.coeff_ref(static_cast<crd::u32>(in.tr[k]), static_cast<crd::u32>(in.tc[k]));
            slot = slot + static_cast<T>(in.vals[k]);  // accumulate duplicates
        }
        m.make_compressed();
        nnz = m.nnz();
    }
    else
    {
        TripletBuilder<T> b(args.alloc, in.rows, in.cols);
        fill_builder_real<T>(b, in);
        nnz = b.compress().nnz();
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

template <typename U>
CommandResult impl_build_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const bool uncompressed = args.get_bool("uncompressed").value_or(false);
    crd::usize nnz = 0;
    if (uncompressed)
    {
        auto m = SparseMatrix<Complex<U>, SparseFormat::Csr>::make_uncompressed(args.alloc, in.rows, in.cols);
        for (crd::usize k = 0; k < in.tr.size(); ++k)
        {
            const Complex<U> v{static_cast<U>(in.vals[2 * k]), static_cast<U>(in.vals[2 * k + 1])};
            Complex<U>&      slot = m.coeff_ref(static_cast<crd::u32>(in.tr[k]), static_cast<crd::u32>(in.tc[k]));
            slot = slot + v;  // accumulate duplicates
        }
        m.make_compressed();
        nnz = m.nnz();
    }
    else
    {
        TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
        fill_builder_complex<U>(b, in);
        nnz = b.compress().nnz();
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

// ---- to_csc : returns the compressed CSC value array (f64 blob) --------

template <typename T>
CommandResult impl_to_csc_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto m = b.compress_csc();
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(m.nnz());
    for (crd::usize i = 0; i < m.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(m.values().values[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_to_csc_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto m = b.compress_csc();
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(m.nnz() * 2);
    for (crd::usize i = 0; i < m.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(m.values().values[i].re));
        out.push_back(static_cast<crd::f64>(m.values().values[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- structural queries (type-agnostic; build an f64 CSR then inspect) -

// Builds the CSR structure from the triplet (rows/cols/coordinates); values
// only affect duplicate sums, never structure. Returns the matrix's nnz/etc.
crd::hesap::sparse::SparseMatrix<crd::f64, crd::hesap::sparse::SparseFormat::Csr>
build_structure(const CommandArgs& args, const TripletInputs& in)
{
    TripletBuilder<crd::f64> b(args.alloc, in.rows, in.cols);
    for (crd::usize k = 0; k < in.tr.size(); ++k)
    {
        b.add(static_cast<crd::u32>(in.tr[k]), static_cast<crd::u32>(in.tc[k]), 1.0);
    }
    return b.compress();
}

CommandResult impl_nnz(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(build_structure(args, in).nnz()));
}

CommandResult impl_density(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, build_structure(args, in).density());
}

// Returns a f64[8] blob: {rows, cols, nnz, density, n_outer, min_inner_nnz,
// max_inner_nnz, is_compressed?1:0}.
CommandResult impl_structural_query(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto m = build_structure(args, in);
    const auto s = crd::hesap::sparse::structural_stats(m);
    const crd::f64 packed[8] = {static_cast<crd::f64>(s.rows),
                                static_cast<crd::f64>(s.cols),
                                static_cast<crd::f64>(s.nnz),
                                s.density,
                                static_cast<crd::f64>(s.n_outer),
                                static_cast<crd::f64>(s.min_inner_nnz),
                                static_cast<crd::f64>(s.max_inner_nnz),
                                s.is_compressed ? 1.0 : 0.0};
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{packed, 8});
}

// ---- spmv : y = alpha*op(A)*x + beta*y -> returns y (f64 blob) ----------

template <typename T, Trans Tr>
CommandResult impl_spmv_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto m = b.compress();

    const crd::u32 xlen = (Tr == Trans::None) ? in.cols : in.rows;
    const crd::u32 ylen = (Tr == Trans::None) ? in.rows : in.cols;
    const auto     xs   = args.get_f64_array("x");
    if (xs.size() != xlen)
    {
        return error_result(args.alloc, "spmv: x has wrong length for this orientation");
    }
    crd::containers::Array<T> xv(args.alloc);
    xv.reserve(xlen);
    for (crd::u32 i = 0; i < xlen; ++i)
    {
        xv.push_back(static_cast<T>(xs[i]));
    }
    crd::containers::Array<T> yv(args.alloc);
    yv.resize(ylen);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    spmv<T>(alpha, m, Tr, crd::containers::ConstSpan<T>{xv.data(), xv.size()}, beta,
            crd::containers::Span<T>{yv.data(), yv.size()});

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(ylen);
    for (crd::u32 i = 0; i < ylen; ++i)
    {
        out.push_back(static_cast<crd::f64>(yv[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U, Trans Tr>
CommandResult impl_spmv_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto m = b.compress();

    const crd::u32 xlen = (Tr == Trans::None) ? in.cols : in.rows;
    const crd::u32 ylen = (Tr == Trans::None) ? in.rows : in.cols;
    const auto     xs   = args.get_f64_array("x");  // flattened {re,im,...}
    if (xs.size() != static_cast<crd::usize>(xlen) * 2)
    {
        return error_result(args.alloc, "spmv: complex x must be flattened length 2*xlen");
    }
    crd::containers::Array<Complex<U>> xv(args.alloc);
    xv.reserve(xlen);
    for (crd::u32 i = 0; i < xlen; ++i)
    {
        xv.push_back(Complex<U>{static_cast<U>(xs[2 * i]), static_cast<U>(xs[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> yv(args.alloc);
    yv.resize(ylen);
    const auto af = args.get_f64_array("alpha");
    const auto bf = args.get_f64_array("beta");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    const Complex<U> beta  = bf.size() >= 2 ? Complex<U>{static_cast<U>(bf[0]), static_cast<U>(bf[1])}
                                            : Complex<U>{U(0), U(0)};
    spmv<Complex<U>>(alpha, m, Tr, crd::containers::ConstSpan<Complex<U>>{xv.data(), xv.size()}, beta,
                     crd::containers::Span<Complex<U>>{yv.data(), yv.size()});

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(ylen) * 2);
    for (crd::u32 i = 0; i < ylen; ++i)
    {
        out.push_back(static_cast<crd::f64>(yv[i].re));
        out.push_back(static_cast<crd::f64>(yv[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- spmv_sell : y = alpha*A*x + beta*y via SELL-C-σ (None only) --------

template <typename T>
CommandResult impl_spmv_sell_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto sell = to_sell(b.compress(), args.alloc);

    const auto xs = args.get_f64_array("x");
    if (xs.size() != in.cols)
    {
        return error_result(args.alloc, "spmv_sell: x must have cols() entries");
    }
    crd::containers::Array<T> xv(args.alloc);
    xv.reserve(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        xv.push_back(static_cast<T>(xs[i]));
    }
    crd::containers::Array<T> yv(args.alloc);
    yv.resize(in.rows);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    spmv_sell<T>(alpha, sell, crd::containers::ConstSpan<T>{xv.data(), xv.size()}, beta,
                 crd::containers::Span<T>{yv.data(), yv.size()});

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(yv[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_spmv_sell_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto sell = to_sell(b.compress(), args.alloc);

    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols) * 2)
    {
        return error_result(args.alloc, "spmv_sell: complex x must be flattened length 2*cols");
    }
    crd::containers::Array<Complex<U>> xv(args.alloc);
    xv.reserve(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        xv.push_back(Complex<U>{static_cast<U>(xs[2 * i]), static_cast<U>(xs[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> yv(args.alloc);
    yv.resize(in.rows);
    spmv_sell<Complex<U>>(Complex<U>{U(1), U(0)}, sell, crd::containers::ConstSpan<Complex<U>>{xv.data(), xv.size()},
                          Complex<U>{U(0), U(0)}, crd::containers::Span<Complex<U>>{yv.data(), yv.size()});

    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(in.rows) * 2);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(yv[i].re));
        out.push_back(static_cast<crd::f64>(yv[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- transpose : returns A^T values (row-major of A^T) ------------------

template <typename T>
CommandResult impl_transpose_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto at = transpose(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(at.nnz());
    for (crd::usize i = 0; i < at.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(at.values().values[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_transpose_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto at = transpose(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(at.nnz() * 2);
    for (crd::usize i = 0; i < at.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(at.values().values[i].re));
        out.push_back(static_cast<crd::f64>(at.values().values[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- to_coo : returns interleaved [row, col, value(s)] -------------------

template <typename T>
CommandResult impl_to_coo_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto coo = to_coo(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(coo.row_idx.size() * 3);
    for (crd::usize k = 0; k < coo.row_idx.size(); ++k)
    {
        out.push_back(static_cast<crd::f64>(coo.row_idx[k]));
        out.push_back(static_cast<crd::f64>(coo.col_idx[k]));
        out.push_back(static_cast<crd::f64>(coo.values[k]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_to_coo_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto coo = to_coo(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(coo.row_idx.size() * 4);
    for (crd::usize k = 0; k < coo.row_idx.size(); ++k)
    {
        out.push_back(static_cast<crd::f64>(coo.row_idx[k]));
        out.push_back(static_cast<crd::f64>(coo.col_idx[k]));
        out.push_back(static_cast<crd::f64>(coo.values[k].re));
        out.push_back(static_cast<crd::f64>(coo.values[k].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- element-wise: scale (1 matrix) + add/sub/hadamard (2 matrices) -----

template <typename T>
CommandResult impl_scale_mat_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    auto    c     = scale<T>(alpha, b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.nnz());
    for (crd::usize i = 0; i < c.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c.values().values[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// Read a second triplet set (b_triplet_rows/b_triplet_cols/b_values) into B.
template <typename T>
bool read_b_real(const CommandArgs& args, crd::u32 rows, crd::u32 cols, TripletBuilder<T>& b, const char*& err)
{
    const auto br = args.get_i64_array("b_triplet_rows");
    const auto bc = args.get_i64_array("b_triplet_cols");
    const auto bv = args.get_f64_array("b_values");
    if (bc.size() != br.size() || bv.size() != br.size())
    {
        err = "b_triplet_rows/cols/values must have equal length";
        return false;
    }
    (void)rows;
    (void)cols;
    for (crd::usize k = 0; k < br.size(); ++k)
    {
        b.add(static_cast<crd::u32>(br[k]), static_cast<crd::u32>(bc[k]), static_cast<T>(bv[k]));
    }
    return true;
}

// op: 0=add, 1=sub, 2=hadamard. Returns C nnz (the structural union/intersection size).
template <typename T, int Op>
CommandResult impl_binary_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> ab(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(ab, in);
    TripletBuilder<T> bb(args.alloc, in.rows, in.cols);
    if (!read_b_real<T>(args, in.rows, in.cols, bb, err))
    {
        return error_result(args.alloc, err);
    }
    auto a = ab.compress();
    auto b = bb.compress();
    crd::usize nnz = 0;
    if constexpr (Op == 0)
    {
        nnz = add<T>(a, b, args.alloc).nnz();
    }
    else if constexpr (Op == 1)
    {
        nnz = subtract<T>(a, b, args.alloc).nnz();
    }
    else
    {
        nnz = hadamard<T>(a, b, args.alloc).nnz();
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

template <typename U>
CommandResult impl_scale_mat_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    const auto       af = args.get_f64_array("alpha");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    auto                             c = scale<Complex<U>>(alpha, b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.nnz() * 2);
    for (crd::usize i = 0; i < c.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c.values().values[i].re));
        out.push_back(static_cast<crd::f64>(c.values().values[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U, int Op>
CommandResult impl_binary_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> ab(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(ab, in);
    const auto br = args.get_i64_array("b_triplet_rows");
    const auto bc = args.get_i64_array("b_triplet_cols");
    const auto bv = args.get_f64_array("b_values");
    if (bc.size() != br.size() || bv.size() != br.size() * 2)
    {
        return error_result(args.alloc, "b_triplet_rows/cols + flattened b_values length mismatch");
    }
    TripletBuilder<Complex<U>> bb(args.alloc, in.rows, in.cols);
    for (crd::usize k = 0; k < br.size(); ++k)
    {
        bb.add(static_cast<crd::u32>(br[k]), static_cast<crd::u32>(bc[k]),
               Complex<U>{static_cast<U>(bv[2 * k]), static_cast<U>(bv[2 * k + 1])});
    }
    auto       a = ab.compress();
    auto       b = bb.compress();
    crd::usize nnz = 0;
    if constexpr (Op == 0)
    {
        nnz = add<Complex<U>>(a, b, args.alloc).nnz();
    }
    else if constexpr (Op == 1)
    {
        nnz = subtract<Complex<U>>(a, b, args.alloc).nnz();
    }
    else
    {
        nnz = hadamard<Complex<U>>(a, b, args.alloc).nnz();
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

// ---- diag (returns diagonal blob) + triu/tril (return nnz) --------------

template <typename T>
CommandResult impl_diag_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    auto d = extract_diagonal(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(d.size());
    for (crd::usize i = 0; i < d.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(d[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_diag_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    auto d = extract_diagonal(b.compress(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(d.size() * 2);
    for (crd::usize i = 0; i < d.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(d[i].re));
        out.push_back(static_cast<crd::f64>(d[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T, bool Upper>
CommandResult impl_tri_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> b(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(b, in);
    const crd::i32 k = static_cast<crd::i32>(args.get_i64("k").value_or(0));
    auto           a = b.compress();
    const crd::usize nnz = Upper ? triu<T>(a, k, args.alloc).nnz() : tril<T>(a, k, args.alloc).nnz();
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

template <typename U, bool Upper>
CommandResult impl_tri_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> b(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(b, in);
    const crd::i32 k = static_cast<crd::i32>(args.get_i64("k").value_or(0));
    auto           a = b.compress();
    const crd::usize nnz = Upper ? triu<Complex<U>>(a, k, args.alloc).nnz() : tril<Complex<U>>(a, k, args.alloc).nnz();
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz));
}

// ---- spgemm (C=A*B, 2 matrices) + spgemm_ata (C=A*A^T) -> C nnz ----------

template <typename U>
bool read_b_complex(const CommandArgs& args, TripletBuilder<Complex<U>>& b, const char*& err)
{
    const auto br = args.get_i64_array("b_triplet_rows");
    const auto bc = args.get_i64_array("b_triplet_cols");
    const auto bv = args.get_f64_array("b_values");
    if (bc.size() != br.size() || bv.size() != br.size() * 2)
    {
        err = "b_triplet_rows/cols + flattened b_values length mismatch";
        return false;
    }
    for (crd::usize k = 0; k < br.size(); ++k)
    {
        b.add(static_cast<crd::u32>(br[k]), static_cast<crd::u32>(bc[k]),
              Complex<U>{static_cast<U>(bv[2 * k]), static_cast<U>(bv[2 * k + 1])});
    }
    return true;
}

template <typename T>
CommandResult impl_spgemm_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> ab(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(ab, in);
    const crd::u32    bcols = static_cast<crd::u32>(args.get_u64("b_cols").value_or(in.cols));
    TripletBuilder<T> bb(args.alloc, in.cols, bcols);  // B is (A.cols x b_cols)
    if (!read_b_real<T>(args, in.cols, bcols, bb, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(spgemm<T>(ab.compress(), bb.compress(), args.alloc).nnz()));
}

template <typename U>
CommandResult impl_spgemm_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> ab(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(ab, in);
    const crd::u32             bcols = static_cast<crd::u32>(args.get_u64("b_cols").value_or(in.cols));
    TripletBuilder<Complex<U>> bb(args.alloc, in.cols, bcols);
    if (!read_b_complex<U>(args, bb, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc,
                         static_cast<crd::f64>(spgemm<Complex<U>>(ab.compress(), bb.compress(), args.alloc).nnz()));
}

template <typename T>
CommandResult impl_spgemm_ata_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> ab(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(ab, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(spgemm_ata<T>(ab.compress(), args.alloc).nnz()));
}

template <typename U>
CommandResult impl_spgemm_ata_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> ab(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(ab, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(spgemm_ata<Complex<U>>(ab.compress(), args.alloc).nnz()));
}

// ---- spmm (C = alpha*A*B + beta*C, B dense row-major k x r) -> C dense ---

template <typename T>
CommandResult impl_spmm_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> ab(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(ab, in);
    const crd::u32 r  = static_cast<crd::u32>(args.get_u64("r").value_or(1));
    const auto     bd = args.get_f64_array("b_dense");  // (cols x r) row-major
    if (bd.size() != static_cast<crd::usize>(in.cols) * r)
    {
        return error_result(args.alloc, "spmm: b_dense length must be cols*r");
    }
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    auto    a     = ab.compress();
    crd::containers::Array<T> b(args.alloc);
    b.resize(bd.size());
    for (crd::usize i = 0; i < bd.size(); ++i)
    {
        b[i] = static_cast<T>(bd[i]);
    }
    crd::containers::Array<T> c(args.alloc);
    c.resize(static_cast<crd::usize>(in.rows) * r);  // beta default 0 -> overwritten
    spmm<T>(alpha, a, b.data(), r, r, beta, c.data(), r);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.size());
    for (crd::usize i = 0; i < c.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_spmm_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> ab(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(ab, in);
    const crd::u32 r  = static_cast<crd::u32>(args.get_u64("r").value_or(1));
    const auto     bd = args.get_f64_array("b_dense");  // flattened {re,im,...}, (cols x r)
    if (bd.size() != static_cast<crd::usize>(in.cols) * r * 2)
    {
        return error_result(args.alloc, "spmm: complex b_dense must be flattened length 2*cols*r");
    }
    const auto       af    = args.get_f64_array("alpha");
    const auto       bf    = args.get_f64_array("beta");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    const Complex<U> beta  = bf.size() >= 2 ? Complex<U>{static_cast<U>(bf[0]), static_cast<U>(bf[1])}
                                            : Complex<U>{U(0), U(0)};
    auto a = ab.compress();
    crd::containers::Array<Complex<U>> b(args.alloc);
    b.reserve(static_cast<crd::usize>(in.cols) * r);
    for (crd::usize i = 0; i < static_cast<crd::usize>(in.cols) * r; ++i)
    {
        b.push_back(Complex<U>{static_cast<U>(bd[2 * i]), static_cast<U>(bd[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> c(args.alloc);
    c.resize(static_cast<crd::usize>(in.rows) * r);
    spmm<Complex<U>>(alpha, a, b.data(), r, r, beta, c.data(), r);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.size() * 2);
    for (crd::usize i = 0; i < c.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c[i].re));
        out.push_back(static_cast<crd::f64>(c[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- sddmm (C = alpha*sample(X*Y^T, mask)) -> C values (nnz blob) --------

template <typename T>
CommandResult impl_sddmm_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> mb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(mb, in);
    const crd::u32 r  = static_cast<crd::u32>(args.get_u64("r").value_or(1));
    const auto     xd = args.get_f64_array("x_dense");  // (rows x r)
    const auto     yd = args.get_f64_array("y_dense");  // (cols x r)
    if (xd.size() != static_cast<crd::usize>(in.rows) * r || yd.size() != static_cast<crd::usize>(in.cols) * r)
    {
        return error_result(args.alloc, "sddmm: x_dense must be rows*r and y_dense cols*r");
    }
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    crd::containers::Array<T> x(args.alloc);
    x.resize(xd.size());
    for (crd::usize i = 0; i < xd.size(); ++i)
    {
        x[i] = static_cast<T>(xd[i]);
    }
    crd::containers::Array<T> y(args.alloc);
    y.resize(yd.size());
    for (crd::usize i = 0; i < yd.size(); ++i)
    {
        y[i] = static_cast<T>(yd[i]);
    }
    auto c = sddmm<T>(mb.compress(), x.data(), r, y.data(), r, r, alpha, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.nnz());
    for (crd::usize i = 0; i < c.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c.values().values[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_sddmm_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> mb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(mb, in);
    const crd::u32 r  = static_cast<crd::u32>(args.get_u64("r").value_or(1));
    const auto     xd = args.get_f64_array("x_dense");  // flattened {re,im,...}, (rows x r)
    const auto     yd = args.get_f64_array("y_dense");  // flattened {re,im,...}, (cols x r)
    if (xd.size() != static_cast<crd::usize>(in.rows) * r * 2 || yd.size() != static_cast<crd::usize>(in.cols) * r * 2)
    {
        return error_result(args.alloc, "sddmm: complex x_dense must be 2*rows*r and y_dense 2*cols*r");
    }
    const auto       af    = args.get_f64_array("alpha");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    crd::containers::Array<Complex<U>> x(args.alloc);
    x.reserve(static_cast<crd::usize>(in.rows) * r);
    for (crd::usize i = 0; i < static_cast<crd::usize>(in.rows) * r; ++i)
    {
        x.push_back(Complex<U>{static_cast<U>(xd[2 * i]), static_cast<U>(xd[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> y(args.alloc);
    y.reserve(static_cast<crd::usize>(in.cols) * r);
    for (crd::usize i = 0; i < static_cast<crd::usize>(in.cols) * r; ++i)
    {
        y.push_back(Complex<U>{static_cast<U>(yd[2 * i]), static_cast<U>(yd[2 * i + 1])});
    }
    auto c = sddmm<Complex<U>>(mb.compress(), x.data(), r, y.data(), r, r, alpha, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(c.nnz() * 2);
    for (crd::usize i = 0; i < c.nnz(); ++i)
    {
        out.push_back(static_cast<crd::f64>(c.values().values[i].re));
        out.push_back(static_cast<crd::f64>(c.values().values[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- BSR: to_bsr (nnz_blocks) / from_bsr (round-trip CSR nnz) / bsr_spmv ----

template <typename T>
CommandResult impl_to_bsr_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "to_bsr: rows/cols must be nonzero multiples of b");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto bsr = to_bsr<T>(tb.compress(), b, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(bsr.nnz_blocks()));
}

template <typename U>
CommandResult impl_to_bsr_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "to_bsr: rows/cols must be nonzero multiples of b");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto bsr = to_bsr<Complex<U>>(tb.compress(), b, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(bsr.nnz_blocks()));
}

template <typename T>
CommandResult impl_from_bsr_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "from_bsr: rows/cols must be nonzero multiples of b");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    // Round-trip: CSR -> BSR -> CSR; returns the block-dense CSR nnz.
    auto bsr = to_bsr<T>(tb.compress(), b, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_bsr<T>(bsr, args.alloc).nnz()));
}

template <typename U>
CommandResult impl_from_bsr_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "from_bsr: rows/cols must be nonzero multiples of b");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto bsr = to_bsr<Complex<U>>(tb.compress(), b, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_bsr<Complex<U>>(bsr, args.alloc).nnz()));
}

template <typename T>
CommandResult impl_bsr_spmv_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "bsr_spmv: rows/cols must be nonzero multiples of b");
    }
    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols))
    {
        return error_result(args.alloc, "bsr_spmv: x length must equal cols");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto    bsr   = to_bsr<T>(tb.compress(), b, args.alloc);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    crd::containers::Array<T> x(args.alloc);
    x.resize(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x[i] = static_cast<T>(xs[i]);
    }
    crd::containers::Array<T> y(args.alloc);
    y.resize(in.rows);
    spmv_bsr<T>(alpha, bsr, crd::containers::ConstSpan<T>{x.data(), x.size()}, beta,
                crd::containers::Span<T>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_bsr_spmv_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 b = static_cast<crd::u32>(args.get_u64("b").value_or(1));
    if (b == 0 || in.rows % b != 0 || in.cols % b != 0)
    {
        return error_result(args.alloc, "bsr_spmv: rows/cols must be nonzero multiples of b");
    }
    const auto xs = args.get_f64_array("x");  // flattened {re,im,...}
    if (xs.size() != static_cast<crd::usize>(in.cols) * 2)
    {
        return error_result(args.alloc, "bsr_spmv: complex x must be flattened length 2*cols");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto             bsr = to_bsr<Complex<U>>(tb.compress(), b, args.alloc);
    const auto       af  = args.get_f64_array("alpha");
    const auto       bf  = args.get_f64_array("beta");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    const Complex<U> beta  = bf.size() >= 2 ? Complex<U>{static_cast<U>(bf[0]), static_cast<U>(bf[1])}
                                            : Complex<U>{U(0), U(0)};
    crd::containers::Array<Complex<U>> x(args.alloc);
    x.reserve(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x.push_back(Complex<U>{static_cast<U>(xs[2 * i]), static_cast<U>(xs[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> y(args.alloc);
    y.resize(in.rows);
    spmv_bsr<Complex<U>>(alpha, bsr, crd::containers::ConstSpan<Complex<U>>{x.data(), x.size()}, beta,
                         crd::containers::Span<Complex<U>>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(in.rows) * 2);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i].re));
        out.push_back(static_cast<crd::f64>(y[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- ELL / DIA: to_X (param) / from_X (round-trip nnz) / X_spmv (y) ---------

template <typename T>
CommandResult impl_to_ell_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(to_ell<T>(tb.compress(), args.alloc).max_row_len));
}

template <typename U>
CommandResult impl_to_ell_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(to_ell<Complex<U>>(tb.compress(), args.alloc).max_row_len));
}

template <typename T>
CommandResult impl_from_ell_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto ell = to_ell<T>(tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_ell<T>(ell, args.alloc).nnz()));
}

template <typename U>
CommandResult impl_from_ell_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto ell = to_ell<Complex<U>>(tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_ell<Complex<U>>(ell, args.alloc).nnz()));
}

template <typename T>
CommandResult impl_ell_spmv_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols))
    {
        return error_result(args.alloc, "ell_spmv: x length must equal cols");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto    ell   = to_ell<T>(tb.compress(), args.alloc);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    crd::containers::Array<T> x(args.alloc);
    x.resize(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x[i] = static_cast<T>(xs[i]);
    }
    crd::containers::Array<T> y(args.alloc);
    y.resize(in.rows);
    spmv_ell<T>(alpha, ell, crd::containers::ConstSpan<T>{x.data(), x.size()}, beta,
                crd::containers::Span<T>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename T>
CommandResult impl_to_dia_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(to_dia<T>(tb.compress(), args.alloc).ndiag));
}

template <typename U>
CommandResult impl_to_dia_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(to_dia<Complex<U>>(tb.compress(), args.alloc).ndiag));
}

template <typename T>
CommandResult impl_from_dia_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto dia = to_dia<T>(tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_dia<T>(dia, args.alloc).nnz()));
}

template <typename U>
CommandResult impl_from_dia_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto dia = to_dia<Complex<U>>(tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_dia<Complex<U>>(dia, args.alloc).nnz()));
}

template <typename T>
CommandResult impl_dia_spmv_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols))
    {
        return error_result(args.alloc, "dia_spmv: x length must equal cols");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    auto    dia   = to_dia<T>(tb.compress(), args.alloc);
    const T alpha = static_cast<T>(args.get_f64("alpha").value_or(1.0));
    const T beta  = static_cast<T>(args.get_f64("beta").value_or(0.0));
    crd::containers::Array<T> x(args.alloc);
    x.resize(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x[i] = static_cast<T>(xs[i]);
    }
    crd::containers::Array<T> y(args.alloc);
    y.resize(in.rows);
    spmv_dia<T>(alpha, dia, crd::containers::ConstSpan<T>{x.data(), x.size()}, beta,
                crd::containers::Span<T>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_ell_spmv_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols) * 2)
    {
        return error_result(args.alloc, "ell_spmv: complex x must be flattened length 2*cols");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto             ell = to_ell<Complex<U>>(tb.compress(), args.alloc);
    const auto       af  = args.get_f64_array("alpha");
    const auto       bf  = args.get_f64_array("beta");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    const Complex<U> beta  = bf.size() >= 2 ? Complex<U>{static_cast<U>(bf[0]), static_cast<U>(bf[1])}
                                            : Complex<U>{U(0), U(0)};
    crd::containers::Array<Complex<U>> x(args.alloc);
    x.reserve(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x.push_back(Complex<U>{static_cast<U>(xs[2 * i]), static_cast<U>(xs[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> y(args.alloc);
    y.resize(in.rows);
    spmv_ell<Complex<U>>(alpha, ell, crd::containers::ConstSpan<Complex<U>>{x.data(), x.size()}, beta,
                         crd::containers::Span<Complex<U>>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(in.rows) * 2);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i].re));
        out.push_back(static_cast<crd::f64>(y[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

template <typename U>
CommandResult impl_dia_spmv_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto xs = args.get_f64_array("x");
    if (xs.size() != static_cast<crd::usize>(in.cols) * 2)
    {
        return error_result(args.alloc, "dia_spmv: complex x must be flattened length 2*cols");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    auto             dia = to_dia<Complex<U>>(tb.compress(), args.alloc);
    const auto       af  = args.get_f64_array("alpha");
    const auto       bf  = args.get_f64_array("beta");
    const Complex<U> alpha = af.size() >= 2 ? Complex<U>{static_cast<U>(af[0]), static_cast<U>(af[1])}
                                            : Complex<U>{U(1), U(0)};
    const Complex<U> beta  = bf.size() >= 2 ? Complex<U>{static_cast<U>(bf[0]), static_cast<U>(bf[1])}
                                            : Complex<U>{U(0), U(0)};
    crd::containers::Array<Complex<U>> x(args.alloc);
    x.reserve(in.cols);
    for (crd::u32 i = 0; i < in.cols; ++i)
    {
        x.push_back(Complex<U>{static_cast<U>(xs[2 * i]), static_cast<U>(xs[2 * i + 1])});
    }
    crd::containers::Array<Complex<U>> y(args.alloc);
    y.resize(in.rows);
    spmv_dia<Complex<U>>(alpha, dia, crd::containers::ConstSpan<Complex<U>>{x.data(), x.size()}, beta,
                         crd::containers::Span<Complex<U>>{y.data(), y.size()});
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(in.rows) * 2);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        out.push_back(static_cast<crd::f64>(y[i].re));
        out.push_back(static_cast<crd::f64>(y[i].im));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- Matrix Market: mtx_read (text -> nnz) / mtx_write (triplets -> text) ----

template <typename T>
CommandResult impl_mtx_read_real(const CommandArgs& args)
{
    MatrixMarketError err(args.alloc);
    auto              m = read_matrix_market<T>(args.get_string("text"), args.alloc, err);
    if (!err.ok)
    {
        return error_result(args.alloc, err.message.c_str());
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(m.nnz()));
}

template <typename U>
CommandResult impl_mtx_read_complex(const CommandArgs& args)
{
    MatrixMarketError err(args.alloc);
    auto              m = read_matrix_market<Complex<U>>(args.get_string("text"), args.alloc, err);
    if (!err.ok)
    {
        return error_result(args.alloc, err.message.c_str());
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(m.nnz()));
}

template <typename T>
CommandResult impl_mtx_write_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return text_result(args.alloc, write_matrix_market<T>(tb.compress(), args.alloc));
}

template <typename U>
CommandResult impl_mtx_write_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return text_result(args.alloc, write_matrix_market<Complex<U>>(tb.compress(), args.alloc));
}

// ---- v1g-3 CLI-audit gap fills: from_csc / scale_rows / submatrix / to_sell /
//      inner_indices (type-agnostic) -------------------------------------------

template <typename T>
CommandResult impl_from_csc_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_csc<T>(tb.compress_csc(), args.alloc).nnz()));
}

template <typename U>
CommandResult impl_from_csc_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(from_csc<Complex<U>>(tb.compress_csc(), args.alloc).nnz()));
}

template <typename T>
CommandResult impl_scale_rows_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto sc = args.get_f64_array("scale");
    if (sc.size() != static_cast<crd::usize>(in.rows))
    {
        return error_result(args.alloc, "scale_rows: scale length must equal rows");
    }
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    crd::containers::Array<T> s(args.alloc);
    s.resize(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        s[i] = static_cast<T>(sc[i]);
    }
    auto out = scale_rows<T>(crd::containers::ConstSpan<T>{s.data(), s.size()}, tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(out.nnz()));
}

template <typename U>
CommandResult impl_scale_rows_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const auto sc = args.get_f64_array("scale");  // flattened {re,im,...}
    if (sc.size() != static_cast<crd::usize>(in.rows) * 2)
    {
        return error_result(args.alloc, "scale_rows: complex scale must be flattened length 2*rows");
    }
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    crd::containers::Array<Complex<U>> s(args.alloc);
    s.reserve(in.rows);
    for (crd::u32 i = 0; i < in.rows; ++i)
    {
        s.push_back(Complex<U>{static_cast<U>(sc[2 * i]), static_cast<U>(sc[2 * i + 1])});
    }
    auto out =
        scale_rows<Complex<U>>(crd::containers::ConstSpan<Complex<U>>{s.data(), s.size()}, tb.compress(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(out.nnz()));
}

template <typename T>
CommandResult impl_submatrix_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 r0 = static_cast<crd::u32>(args.get_u64("r0").value_or(0));
    const crd::u32 r1 = static_cast<crd::u32>(args.get_u64("r1").value_or(in.rows));
    const crd::u32 c0 = static_cast<crd::u32>(args.get_u64("c0").value_or(0));
    const crd::u32 c1 = static_cast<crd::u32>(args.get_u64("c1").value_or(in.cols));
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(submatrix<T>(tb.compress(), r0, r1, c0, c1, args.alloc).nnz()));
}

template <typename U>
CommandResult impl_submatrix_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 r0 = static_cast<crd::u32>(args.get_u64("r0").value_or(0));
    const crd::u32 r1 = static_cast<crd::u32>(args.get_u64("r1").value_or(in.rows));
    const crd::u32 c0 = static_cast<crd::u32>(args.get_u64("c0").value_or(0));
    const crd::u32 c1 = static_cast<crd::u32>(args.get_u64("c1").value_or(in.cols));
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return scalar_result(args.alloc,
                         static_cast<crd::f64>(submatrix<Complex<U>>(tb.compress(), r0, r1, c0, c1, args.alloc).nnz()));
}

template <typename T>
CommandResult impl_to_sell_real(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 sigma = static_cast<crd::u32>(args.get_u64("sigma").value_or(0));
    TripletBuilder<T> tb(args.alloc, in.rows, in.cols);
    fill_builder_real<T>(tb, in);
    return scalar_result(args.alloc, static_cast<crd::f64>(to_sell<T>(tb.compress(), args.alloc, sigma).num_slices));
}

template <typename U>
CommandResult impl_to_sell_complex(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/true, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 sigma = static_cast<crd::u32>(args.get_u64("sigma").value_or(0));
    TripletBuilder<Complex<U>> tb(args.alloc, in.rows, in.cols);
    fill_builder_complex<U>(tb, in);
    return scalar_result(args.alloc,
                         static_cast<crd::f64>(to_sell<Complex<U>>(tb.compress(), args.alloc, sigma).num_slices));
}

// Type-agnostic: row k's stored column indices (structure only).
CommandResult impl_inner_indices(const CommandArgs& args)
{
    TripletInputs in;
    const char* err = nullptr;
    if (!read_inputs(args, /*complex=*/false, in, err))
    {
        return error_result(args.alloc, err);
    }
    const crd::u32 k = static_cast<crd::u32>(args.get_u64("k").value_or(0));
    const auto     m = build_structure(args, in);
    if (k >= m.rows())
    {
        return error_result(args.alloc, "inner_indices: row k out of range");
    }
    const auto cols = crd::hesap::sparse::inner_indices(m, k);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(cols.size());
    for (crd::usize i = 0; i < cols.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(cols[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

// ---- schema helpers ----------------------------------------------------

void add_param(CommandSchema& s, crd::memory::IAllocator* alloc, const char* name, const char* desc, ParamKind kind,
               bool required)
{
    ParamSchema p{alloc};
    p.name = crd::containers::String{name, alloc};
    p.description = crd::containers::String{desc, alloc};
    p.kind = kind;
    p.required = required;
    s.params.push_back(std::move(p));
}

CommandSchema make_triplet_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc,
                                  OutputKind out_kind, bool with_uncompressed_flag)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = out_kind;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Number of matrix rows", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Number of matrix columns", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "values", "COO values (F64Array; complex flattened {re,im,...})", ParamKind::F64, true);
    if (with_uncompressed_flag)
    {
        add_param(s, alloc, "uncompressed", "Assemble via uncompressed insert path (default false)", ParamKind::Bool,
                  false);
    }
    return s;
}

CommandSchema make_mtx_read_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s{alloc};
    s.name               = crd::containers::String{name, alloc};
    s.description        = crd::containers::String{desc, alloc};
    s.output.kind        = OutputKind::Scalar;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent         = true;
    add_param(s, alloc, "text", "Matrix Market (.mtx) coordinate text", ParamKind::String, true);
    return s;
}

CommandSchema make_spmv_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::BinaryBlob, false);
    add_param(s, alloc, "x", "Dense input vector (F64Array; complex flattened {re,im,...})", ParamKind::F64, true);
    add_param(s, alloc, "alpha", "Scalar multiplier on A*x (default 1)", ParamKind::F64, false);
    add_param(s, alloc, "beta", "Scalar multiplier on prior y (default 0)", ParamKind::F64, false);
    return s;
}

CommandSchema make_scale_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::BinaryBlob, false);
    add_param(s, alloc, "alpha", "Scalar multiplier (complex flattened [re,im])", ParamKind::F64, true);
    return s;
}

CommandSchema make_binary_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
    add_param(s, alloc, "b_triplet_rows", "B COO row indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "b_triplet_cols", "B COO column indices (I64Array)", ParamKind::I64, true);
    add_param(s, alloc, "b_values", "B COO values (F64Array; complex flattened)", ParamKind::F64, true);
    return s;
}

} // namespace

CRD_HESAP_CLI_REGISTER_MODULE([](CommandRegistry& reg) {
    auto* alloc = crd::memory::default_allocator();

    // from_triplets.<T> -> nnz
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.from_triplets.f32", "Assemble COO triplets into CSR (f32); returns nnz.",
                            OutputKind::Scalar, false),
        &impl_from_triplets_real<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.from_triplets.f64", "Assemble COO triplets into CSR (f64); returns nnz.",
                            OutputKind::Scalar, false),
        &impl_from_triplets_real<crd::f64>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.from_triplets.c32",
                            "Assemble COO triplets into CSR (Complex<f32>); returns nnz.", OutputKind::Scalar, false),
        &impl_from_triplets_complex<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.from_triplets.c64",
                            "Assemble COO triplets into CSR (Complex<f64>); returns nnz.", OutputKind::Scalar, false),
        &impl_from_triplets_complex<crd::f64>);

    // to_csr.<T> -> compressed value array (f64 blob)
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csr.f32", "Assemble triplets, return CSR values (f32 blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csr_real<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csr.f64", "Assemble triplets, return CSR values (f64 blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csr_real<crd::f64>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csr.c32",
                            "Assemble triplets, return CSR values (Complex<f32> flattened blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csr_complex<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csr.c64",
                            "Assemble triplets, return CSR values (Complex<f64> flattened blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csr_complex<crd::f64>);

    // build.<T> -> nnz (optional uncompressed path)
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.build.f32",
                            "Build CSR from triplets (f32); `uncompressed` flag routes through insert path.",
                            OutputKind::Scalar, true),
        &impl_build_real<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.build.f64",
                            "Build CSR from triplets (f64); `uncompressed` flag routes through insert path.",
                            OutputKind::Scalar, true),
        &impl_build_real<crd::f64>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.build.c32", "Build CSR from triplets (Complex<f32>); returns nnz.",
                            OutputKind::Scalar, true),
        &impl_build_complex<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.build.c64", "Build CSR from triplets (Complex<f64>); returns nnz.",
                            OutputKind::Scalar, true),
        &impl_build_complex<crd::f64>);

    // to_csc.<T> -> compressed CSC value array (f64 blob)
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csc.f32", "Assemble triplets, return CSC values (f32 blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csc_real<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csc.f64", "Assemble triplets, return CSC values (f64 blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csc_real<crd::f64>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csc.c32",
                            "Assemble triplets, return CSC values (Complex<f32> flattened blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csc_complex<crd::f32>);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.to_csc.c64",
                            "Assemble triplets, return CSC values (Complex<f64> flattened blob).",
                            OutputKind::BinaryBlob, false),
        &impl_to_csc_complex<crd::f64>);

    // Structural queries -- type-agnostic (structure-only), one command each.
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.nnz", "Number of stored entries after dedup; returns nnz.",
                            OutputKind::Scalar, false),
        &impl_nnz);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.density", "Fraction of dense cells stored; returns density.",
                            OutputKind::Scalar, false),
        &impl_density);
    reg.register_command(
        make_triplet_schema(alloc, "hesap.sparse.structural_query",
                            "Structural stats f64[8]: rows,cols,nnz,density,n_outer,min_inner,max_inner,is_compressed.",
                            OutputKind::BinaryBlob, false),
        &impl_structural_query);

    // spmv.<T> : y = alpha*A*x + beta*y  ;  spmv_transpose.<T> : y = alpha*A^T*x + beta*y
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv.f32", "y = alpha*A*x + beta*y (CSR, f32)."),
                         &impl_spmv_real<crd::f32, Trans::None>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv.f64", "y = alpha*A*x + beta*y (CSR, f64)."),
                         &impl_spmv_real<crd::f64, Trans::None>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv.c32", "y = alpha*A*x + beta*y (CSR, Complex<f32>)."),
                         &impl_spmv_complex<crd::f32, Trans::None>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv.c64", "y = alpha*A*x + beta*y (CSR, Complex<f64>)."),
                         &impl_spmv_complex<crd::f64, Trans::None>);

    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_transpose.f32", "y = alpha*A^T*x + beta*y (CSR, f32)."),
        &impl_spmv_real<crd::f32, Trans::Transpose>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_transpose.f64", "y = alpha*A^T*x + beta*y (CSR, f64)."),
        &impl_spmv_real<crd::f64, Trans::Transpose>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_transpose.c32", "y = alpha*A^T*x + beta*y (CSR, Complex<f32>)."),
        &impl_spmv_complex<crd::f32, Trans::Transpose>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_transpose.c64", "y = alpha*A^T*x + beta*y (CSR, Complex<f64>)."),
        &impl_spmv_complex<crd::f64, Trans::Transpose>);

    // spmv_sell.<T> : y = alpha*A*x + beta*y via the SELL-C-σ SIMD primary.
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv_sell.f32", "y = alpha*A*x + beta*y via SELL (f32)."),
                         &impl_spmv_sell_real<crd::f32>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.spmv_sell.f64", "y = alpha*A*x + beta*y via SELL (f64)."),
                         &impl_spmv_sell_real<crd::f64>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_sell.c32", "y = alpha*A*x + beta*y via SELL (Complex<f32>)."),
        &impl_spmv_sell_complex<crd::f32>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_sell.c64", "y = alpha*A*x + beta*y via SELL (Complex<f64>)."),
        &impl_spmv_sell_complex<crd::f64>);

    // transpose.<T> : returns A^T values (row-major of A^T).
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.transpose.f32", "Transpose A; returns A^T values (f32).",
                                             OutputKind::BinaryBlob, false),
                         &impl_transpose_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.transpose.f64", "Transpose A; returns A^T values (f64).",
                                             OutputKind::BinaryBlob, false),
                         &impl_transpose_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.transpose.c32",
                                             "Transpose A; returns A^T values (Complex<f32>, non-conjugating).",
                                             OutputKind::BinaryBlob, false),
                         &impl_transpose_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.transpose.c64",
                                             "Transpose A; returns A^T values (Complex<f64>, non-conjugating).",
                                             OutputKind::BinaryBlob, false),
                         &impl_transpose_complex<crd::f64>);

    // to_coo.<T> : returns interleaved [row,col,value(s)] COO triples.
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_coo.f32",
                                             "CSR->COO; returns [row,col,val] triples (f32).", OutputKind::BinaryBlob,
                                             false),
                         &impl_to_coo_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_coo.f64",
                                             "CSR->COO; returns [row,col,val] triples (f64).", OutputKind::BinaryBlob,
                                             false),
                         &impl_to_coo_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_coo.c32",
                                             "CSR->COO; returns [row,col,re,im] (Complex<f32>).", OutputKind::BinaryBlob,
                                             false),
                         &impl_to_coo_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_coo.c64",
                                             "CSR->COO; returns [row,col,re,im] (Complex<f64>).", OutputKind::BinaryBlob,
                                             false),
                         &impl_to_coo_complex<crd::f64>);

    // scale.<T> : alpha*A -> scaled values blob.
    reg.register_command(make_scale_schema(alloc, "hesap.sparse.scale.f32", "alpha*A; returns scaled values (f32)."),
                         &impl_scale_mat_real<crd::f32>);
    reg.register_command(make_scale_schema(alloc, "hesap.sparse.scale.f64", "alpha*A; returns scaled values (f64)."),
                         &impl_scale_mat_real<crd::f64>);
    reg.register_command(make_scale_schema(alloc, "hesap.sparse.scale.c32", "alpha*A; returns scaled values (Complex<f32>)."),
                         &impl_scale_mat_complex<crd::f32>);
    reg.register_command(make_scale_schema(alloc, "hesap.sparse.scale.c64", "alpha*A; returns scaled values (Complex<f64>)."),
                         &impl_scale_mat_complex<crd::f64>);

    // add/sub/hadamard.<T> : two-matrix element-wise -> C nnz (Scalar).
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.add.f32", "C = A + B; returns nnz (f32)."),
                         &impl_binary_real<crd::f32, 0>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.add.f64", "C = A + B; returns nnz (f64)."),
                         &impl_binary_real<crd::f64, 0>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.add.c32", "C = A + B; returns nnz (Complex<f32>)."),
                         &impl_binary_complex<crd::f32, 0>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.add.c64", "C = A + B; returns nnz (Complex<f64>)."),
                         &impl_binary_complex<crd::f64, 0>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.sub.f32", "C = A - B; returns nnz (f32)."),
                         &impl_binary_real<crd::f32, 1>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.sub.f64", "C = A - B; returns nnz (f64)."),
                         &impl_binary_real<crd::f64, 1>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.sub.c32", "C = A - B; returns nnz (Complex<f32>)."),
                         &impl_binary_complex<crd::f32, 1>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.sub.c64", "C = A - B; returns nnz (Complex<f64>)."),
                         &impl_binary_complex<crd::f64, 1>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.hadamard.f32", "C = A .* B; returns nnz (f32)."),
                         &impl_binary_real<crd::f32, 2>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.hadamard.f64", "C = A .* B; returns nnz (f64)."),
                         &impl_binary_real<crd::f64, 2>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.hadamard.c32", "C = A .* B; returns nnz (Complex<f32>)."),
                         &impl_binary_complex<crd::f32, 2>);
    reg.register_command(make_binary_schema(alloc, "hesap.sparse.hadamard.c64", "C = A .* B; returns nnz (Complex<f64>)."),
                         &impl_binary_complex<crd::f64, 2>);

    // diag.<T> : main diagonal as a dense blob.
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.diag.f32", "Main diagonal A[i,i] (f32).",
                                             OutputKind::BinaryBlob, false),
                         &impl_diag_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.diag.f64", "Main diagonal A[i,i] (f64).",
                                             OutputKind::BinaryBlob, false),
                         &impl_diag_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.diag.c32", "Main diagonal A[i,i] (Complex<f32>).",
                                             OutputKind::BinaryBlob, false),
                         &impl_diag_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.diag.c64", "Main diagonal A[i,i] (Complex<f64>).",
                                             OutputKind::BinaryBlob, false),
                         &impl_diag_complex<crd::f64>);

    // triu/tril.<T> : triangular part (k = diagonal offset, default 0); returns nnz.
    auto tri_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
        add_param(s, alloc, "k", "Diagonal offset (default 0)", ParamKind::I64, false);
        return s;
    };
    reg.register_command(tri_schema("hesap.sparse.triu.f32", "Upper triangular part nnz (f32)."),
                         &impl_tri_real<crd::f32, true>);
    reg.register_command(tri_schema("hesap.sparse.triu.f64", "Upper triangular part nnz (f64)."),
                         &impl_tri_real<crd::f64, true>);
    reg.register_command(tri_schema("hesap.sparse.triu.c32", "Upper triangular part nnz (Complex<f32>)."),
                         &impl_tri_complex<crd::f32, true>);
    reg.register_command(tri_schema("hesap.sparse.triu.c64", "Upper triangular part nnz (Complex<f64>)."),
                         &impl_tri_complex<crd::f64, true>);
    reg.register_command(tri_schema("hesap.sparse.tril.f32", "Lower triangular part nnz (f32)."),
                         &impl_tri_real<crd::f32, false>);
    reg.register_command(tri_schema("hesap.sparse.tril.f64", "Lower triangular part nnz (f64)."),
                         &impl_tri_real<crd::f64, false>);
    reg.register_command(tri_schema("hesap.sparse.tril.c32", "Lower triangular part nnz (Complex<f32>)."),
                         &impl_tri_complex<crd::f32, false>);
    reg.register_command(tri_schema("hesap.sparse.tril.c64", "Lower triangular part nnz (Complex<f64>)."),
                         &impl_tri_complex<crd::f64, false>);

    // spgemm.<T> : C = A*B (A + B triplet sets, b_cols) -> C nnz.
    auto spgemm_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_binary_schema(alloc, name, desc);  // adds b_triplet_*; output Scalar
        add_param(s, alloc, "b_cols", "Number of columns of B (B is A.cols x b_cols)", ParamKind::U64, true);
        return s;
    };
    reg.register_command(spgemm_schema("hesap.sparse.spgemm.f32", "C = A*B; returns nnz (f32)."),
                         &impl_spgemm_real<crd::f32>);
    reg.register_command(spgemm_schema("hesap.sparse.spgemm.f64", "C = A*B; returns nnz (f64)."),
                         &impl_spgemm_real<crd::f64>);
    reg.register_command(spgemm_schema("hesap.sparse.spgemm.c32", "C = A*B; returns nnz (Complex<f32>)."),
                         &impl_spgemm_complex<crd::f32>);
    reg.register_command(spgemm_schema("hesap.sparse.spgemm.c64", "C = A*B; returns nnz (Complex<f64>)."),
                         &impl_spgemm_complex<crd::f64>);

    // spgemm_ata.<T> : C = A*A^T (1 matrix) -> C nnz.
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.spgemm_ata.f32", "C = A*A^T; returns nnz (f32).",
                                             OutputKind::Scalar, false),
                         &impl_spgemm_ata_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.spgemm_ata.f64", "C = A*A^T; returns nnz (f64).",
                                             OutputKind::Scalar, false),
                         &impl_spgemm_ata_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.spgemm_ata.c32",
                                             "C = A*A^T; returns nnz (Complex<f32>).", OutputKind::Scalar, false),
                         &impl_spgemm_ata_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.spgemm_ata.c64",
                                             "C = A*A^T; returns nnz (Complex<f64>).", OutputKind::Scalar, false),
                         &impl_spgemm_ata_complex<crd::f64>);

    // spmm.<T> : C = alpha*A*B + beta*C, B dense row-major (cols x r) -> C dense (rows x r) blob.
    auto spmm_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::BinaryBlob, false);
        add_param(s, alloc, "b_dense", "Dense B row-major cols*r (F64Array; complex flattened {re,im,...})",
                  ParamKind::F64, true);
        add_param(s, alloc, "r", "Number of dense columns (RHS count)", ParamKind::U64, true);
        add_param(s, alloc, "alpha", "Scalar on A*B (complex flattened [re,im]; default 1)", ParamKind::F64, false);
        add_param(s, alloc, "beta", "Scalar on prior C (complex flattened [re,im]; default 0)", ParamKind::F64, false);
        return s;
    };
    reg.register_command(spmm_schema("hesap.sparse.spmm.f32", "C = alpha*A*B + beta*C (f32)."),
                         &impl_spmm_real<crd::f32>);
    reg.register_command(spmm_schema("hesap.sparse.spmm.f64", "C = alpha*A*B + beta*C (f64)."),
                         &impl_spmm_real<crd::f64>);
    reg.register_command(spmm_schema("hesap.sparse.spmm.c32", "C = alpha*A*B + beta*C (Complex<f32>)."),
                         &impl_spmm_complex<crd::f32>);
    reg.register_command(spmm_schema("hesap.sparse.spmm.c64", "C = alpha*A*B + beta*C (Complex<f64>)."),
                         &impl_spmm_complex<crd::f64>);

    // sddmm.<T> : C = alpha*sample(X*Y^T, mask). Returns C values (nnz) as a blob.
    auto sddmm_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::BinaryBlob, false);
        add_param(s, alloc, "x_dense", "Dense X row-major rows*r (F64Array; complex flattened {re,im,...})",
                  ParamKind::F64, true);
        add_param(s, alloc, "y_dense", "Dense Y row-major cols*r (F64Array; complex flattened {re,im,...})",
                  ParamKind::F64, true);
        add_param(s, alloc, "r", "Inner (feature) dimension", ParamKind::U64, true);
        add_param(s, alloc, "alpha", "Scalar on each sampled dot (complex flattened [re,im]; default 1)", ParamKind::F64,
                  false);
        return s;
    };
    reg.register_command(sddmm_schema("hesap.sparse.sddmm.f32", "C = alpha*sample(X*Y^T, mask) (f32)."),
                         &impl_sddmm_real<crd::f32>);
    reg.register_command(sddmm_schema("hesap.sparse.sddmm.f64", "C = alpha*sample(X*Y^T, mask) (f64)."),
                         &impl_sddmm_real<crd::f64>);
    reg.register_command(sddmm_schema("hesap.sparse.sddmm.c32", "C = alpha*sample(X*Y^T, mask) (Complex<f32>)."),
                         &impl_sddmm_complex<crd::f32>);
    reg.register_command(sddmm_schema("hesap.sparse.sddmm.c64", "C = alpha*sample(X*Y^T, mask) (Complex<f64>)."),
                         &impl_sddmm_complex<crd::f64>);

    // to_bsr.<T> : CSR -> BSR; returns nnz_blocks. from_bsr.<T> : CSR->BSR->CSR
    // round-trip; returns the block-dense CSR nnz (b = block size).
    auto bsr_conv_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
        add_param(s, alloc, "b", "Block size (square b x b blocks; rows/cols must be multiples of b)", ParamKind::U64,
                  true);
        return s;
    };
    reg.register_command(bsr_conv_schema("hesap.sparse.to_bsr.f32", "CSR -> BSR; returns nnz_blocks (f32)."),
                         &impl_to_bsr_real<crd::f32>);
    reg.register_command(bsr_conv_schema("hesap.sparse.to_bsr.f64", "CSR -> BSR; returns nnz_blocks (f64)."),
                         &impl_to_bsr_real<crd::f64>);
    reg.register_command(bsr_conv_schema("hesap.sparse.to_bsr.c32", "CSR -> BSR; returns nnz_blocks (Complex<f32>)."),
                         &impl_to_bsr_complex<crd::f32>);
    reg.register_command(bsr_conv_schema("hesap.sparse.to_bsr.c64", "CSR -> BSR; returns nnz_blocks (Complex<f64>)."),
                         &impl_to_bsr_complex<crd::f64>);
    reg.register_command(bsr_conv_schema("hesap.sparse.from_bsr.f32", "CSR->BSR->CSR round-trip nnz (f32)."),
                         &impl_from_bsr_real<crd::f32>);
    reg.register_command(bsr_conv_schema("hesap.sparse.from_bsr.f64", "CSR->BSR->CSR round-trip nnz (f64)."),
                         &impl_from_bsr_real<crd::f64>);
    reg.register_command(bsr_conv_schema("hesap.sparse.from_bsr.c32", "CSR->BSR->CSR round-trip nnz (Complex<f32>)."),
                         &impl_from_bsr_complex<crd::f32>);
    reg.register_command(bsr_conv_schema("hesap.sparse.from_bsr.c64", "CSR->BSR->CSR round-trip nnz (Complex<f64>)."),
                         &impl_from_bsr_complex<crd::f64>);

    // bsr_spmv.<T> : y = alpha*A*x + beta*y via BSR block-spmv (b = block size).
    auto bsr_spmv_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_spmv_schema(alloc, name, desc);  // adds x/alpha/beta
        add_param(s, alloc, "b", "Block size (square b x b blocks; rows/cols must be multiples of b)", ParamKind::U64,
                  true);
        return s;
    };
    reg.register_command(bsr_spmv_schema("hesap.sparse.bsr_spmv.f32", "y = alpha*A*x + beta*y via BSR (f32)."),
                         &impl_bsr_spmv_real<crd::f32>);
    reg.register_command(bsr_spmv_schema("hesap.sparse.bsr_spmv.f64", "y = alpha*A*x + beta*y via BSR (f64)."),
                         &impl_bsr_spmv_real<crd::f64>);
    reg.register_command(bsr_spmv_schema("hesap.sparse.bsr_spmv.c32", "y = alpha*A*x + beta*y via BSR (Complex<f32>)."),
                         &impl_bsr_spmv_complex<crd::f32>);
    reg.register_command(bsr_spmv_schema("hesap.sparse.bsr_spmv.c64", "y = alpha*A*x + beta*y via BSR (Complex<f64>)."),
                         &impl_bsr_spmv_complex<crd::f64>);

    // to_ell.<T> (-> max_row_len) / from_ell.<T> (-> round-trip CSR nnz).
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_ell.f32", "CSR -> ELL; returns max_row_len (f32).",
                                             OutputKind::Scalar, false),
                         &impl_to_ell_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_ell.f64", "CSR -> ELL; returns max_row_len (f64).",
                                             OutputKind::Scalar, false),
                         &impl_to_ell_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_ell.c32",
                                             "CSR -> ELL; returns max_row_len (Complex<f32>).", OutputKind::Scalar,
                                             false),
                         &impl_to_ell_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_ell.c64",
                                             "CSR -> ELL; returns max_row_len (Complex<f64>).", OutputKind::Scalar,
                                             false),
                         &impl_to_ell_complex<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_ell.f32", "ELL->CSR round-trip nnz (f32).",
                                             OutputKind::Scalar, false),
                         &impl_from_ell_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_ell.f64", "ELL->CSR round-trip nnz (f64).",
                                             OutputKind::Scalar, false),
                         &impl_from_ell_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_ell.c32",
                                             "ELL->CSR round-trip nnz (Complex<f32>).", OutputKind::Scalar, false),
                         &impl_from_ell_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_ell.c64",
                                             "ELL->CSR round-trip nnz (Complex<f64>).", OutputKind::Scalar, false),
                         &impl_from_ell_complex<crd::f64>);

    // ell_spmv.<T> : y = alpha*A*x + beta*y via ELL.
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.ell_spmv.f32", "y = alpha*A*x + beta*y via ELL (f32)."),
                         &impl_ell_spmv_real<crd::f32>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.ell_spmv.f64", "y = alpha*A*x + beta*y via ELL (f64)."),
                         &impl_ell_spmv_real<crd::f64>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.ell_spmv.c32", "y = alpha*A*x + beta*y via ELL (Complex<f32>)."),
        &impl_ell_spmv_complex<crd::f32>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.ell_spmv.c64", "y = alpha*A*x + beta*y via ELL (Complex<f64>)."),
        &impl_ell_spmv_complex<crd::f64>);

    // to_dia.<T> (-> ndiag) / from_dia.<T> (-> round-trip CSR nnz).
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_dia.f32", "CSR -> DIA; returns ndiag (f32).",
                                             OutputKind::Scalar, false),
                         &impl_to_dia_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_dia.f64", "CSR -> DIA; returns ndiag (f64).",
                                             OutputKind::Scalar, false),
                         &impl_to_dia_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_dia.c32",
                                             "CSR -> DIA; returns ndiag (Complex<f32>).", OutputKind::Scalar, false),
                         &impl_to_dia_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.to_dia.c64",
                                             "CSR -> DIA; returns ndiag (Complex<f64>).", OutputKind::Scalar, false),
                         &impl_to_dia_complex<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_dia.f32", "DIA->CSR round-trip nnz (f32).",
                                             OutputKind::Scalar, false),
                         &impl_from_dia_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_dia.f64", "DIA->CSR round-trip nnz (f64).",
                                             OutputKind::Scalar, false),
                         &impl_from_dia_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_dia.c32",
                                             "DIA->CSR round-trip nnz (Complex<f32>).", OutputKind::Scalar, false),
                         &impl_from_dia_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_dia.c64",
                                             "DIA->CSR round-trip nnz (Complex<f64>).", OutputKind::Scalar, false),
                         &impl_from_dia_complex<crd::f64>);

    // dia_spmv.<T> : y = alpha*A*x + beta*y via DIA.
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.dia_spmv.f32", "y = alpha*A*x + beta*y via DIA (f32)."),
                         &impl_dia_spmv_real<crd::f32>);
    reg.register_command(make_spmv_schema(alloc, "hesap.sparse.dia_spmv.f64", "y = alpha*A*x + beta*y via DIA (f64)."),
                         &impl_dia_spmv_real<crd::f64>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.dia_spmv.c32", "y = alpha*A*x + beta*y via DIA (Complex<f32>)."),
        &impl_dia_spmv_complex<crd::f32>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.dia_spmv.c64", "y = alpha*A*x + beta*y via DIA (Complex<f64>)."),
        &impl_dia_spmv_complex<crd::f64>);

    // mtx_read.<T> : Matrix Market text -> CSR; returns nnz (Scalar).
    reg.register_command(make_mtx_read_schema(alloc, "hesap.sparse.mtx_read.f32", "Read .mtx; returns nnz (f32)."),
                         &impl_mtx_read_real<crd::f32>);
    reg.register_command(make_mtx_read_schema(alloc, "hesap.sparse.mtx_read.f64", "Read .mtx; returns nnz (f64)."),
                         &impl_mtx_read_real<crd::f64>);
    reg.register_command(
        make_mtx_read_schema(alloc, "hesap.sparse.mtx_read.c32", "Read .mtx; returns nnz (Complex<f32>)."),
        &impl_mtx_read_complex<crd::f32>);
    reg.register_command(
        make_mtx_read_schema(alloc, "hesap.sparse.mtx_read.c64", "Read .mtx; returns nnz (Complex<f64>)."),
        &impl_mtx_read_complex<crd::f64>);

    // mtx_write.<T> : CSR (triplets) -> Matrix Market `coordinate general` text.
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.mtx_write.f32",
                                             "Write CSR as .mtx coordinate general text (f32).", OutputKind::Text,
                                             false),
                         &impl_mtx_write_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.mtx_write.f64",
                                             "Write CSR as .mtx coordinate general text (f64).", OutputKind::Text,
                                             false),
                         &impl_mtx_write_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.mtx_write.c32",
                                             "Write CSR as .mtx coordinate general text (Complex<f32>).",
                                             OutputKind::Text, false),
                         &impl_mtx_write_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.mtx_write.c64",
                                             "Write CSR as .mtx coordinate general text (Complex<f64>).",
                                             OutputKind::Text, false),
                         &impl_mtx_write_complex<crd::f64>);

    // ---- v1g-3 CLI-audit gap fills ----
    // from_csc.<T> : CSC (triplets) -> CSR; returns nnz.
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_csc.f32", "CSC -> CSR; returns nnz (f32).",
                                             OutputKind::Scalar, false),
                         &impl_from_csc_real<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_csc.f64", "CSC -> CSR; returns nnz (f64).",
                                             OutputKind::Scalar, false),
                         &impl_from_csc_real<crd::f64>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_csc.c32",
                                             "CSC -> CSR; returns nnz (Complex<f32>).", OutputKind::Scalar, false),
                         &impl_from_csc_complex<crd::f32>);
    reg.register_command(make_triplet_schema(alloc, "hesap.sparse.from_csc.c64",
                                             "CSC -> CSR; returns nnz (Complex<f64>).", OutputKind::Scalar, false),
                         &impl_from_csc_complex<crd::f64>);

    // scale_rows.<T> : C = diag(scale)*A; returns nnz.
    auto scale_rows_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
        add_param(s, alloc, "scale", "Per-row scale vector length rows (F64Array; complex flattened {re,im,...})",
                  ParamKind::F64, true);
        return s;
    };
    reg.register_command(scale_rows_schema("hesap.sparse.scale_rows.f32", "C = diag(scale)*A; returns nnz (f32)."),
                         &impl_scale_rows_real<crd::f32>);
    reg.register_command(scale_rows_schema("hesap.sparse.scale_rows.f64", "C = diag(scale)*A; returns nnz (f64)."),
                         &impl_scale_rows_real<crd::f64>);
    reg.register_command(
        scale_rows_schema("hesap.sparse.scale_rows.c32", "C = diag(scale)*A; returns nnz (Complex<f32>)."),
        &impl_scale_rows_complex<crd::f32>);
    reg.register_command(
        scale_rows_schema("hesap.sparse.scale_rows.c64", "C = diag(scale)*A; returns nnz (Complex<f64>)."),
        &impl_scale_rows_complex<crd::f64>);

    // submatrix.<T> : A[r0:r1, c0:c1]; returns nnz.
    auto submatrix_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
        add_param(s, alloc, "r0", "Row start (inclusive)", ParamKind::U64, true);
        add_param(s, alloc, "r1", "Row end (exclusive)", ParamKind::U64, true);
        add_param(s, alloc, "c0", "Col start (inclusive)", ParamKind::U64, true);
        add_param(s, alloc, "c1", "Col end (exclusive)", ParamKind::U64, true);
        return s;
    };
    reg.register_command(submatrix_schema("hesap.sparse.submatrix.f32", "A[r0:r1, c0:c1]; returns nnz (f32)."),
                         &impl_submatrix_real<crd::f32>);
    reg.register_command(submatrix_schema("hesap.sparse.submatrix.f64", "A[r0:r1, c0:c1]; returns nnz (f64)."),
                         &impl_submatrix_real<crd::f64>);
    reg.register_command(submatrix_schema("hesap.sparse.submatrix.c32", "A[r0:r1, c0:c1]; returns nnz (Complex<f32>)."),
                         &impl_submatrix_complex<crd::f32>);
    reg.register_command(submatrix_schema("hesap.sparse.submatrix.c64", "A[r0:r1, c0:c1]; returns nnz (Complex<f64>)."),
                         &impl_submatrix_complex<crd::f64>);

    // to_sell.<T> : CSR -> SELL-C-sigma; returns num_slices.
    auto to_sell_schema = [&](const char* name, const char* desc) {
        CommandSchema s = make_triplet_schema(alloc, name, desc, OutputKind::Scalar, false);
        add_param(s, alloc, "sigma", "Row-length sort window (0 = global sort; default 0)", ParamKind::U64, false);
        return s;
    };
    reg.register_command(to_sell_schema("hesap.sparse.to_sell.f32", "CSR -> SELL; returns num_slices (f32)."),
                         &impl_to_sell_real<crd::f32>);
    reg.register_command(to_sell_schema("hesap.sparse.to_sell.f64", "CSR -> SELL; returns num_slices (f64)."),
                         &impl_to_sell_real<crd::f64>);
    reg.register_command(to_sell_schema("hesap.sparse.to_sell.c32", "CSR -> SELL; returns num_slices (Complex<f32>)."),
                         &impl_to_sell_complex<crd::f32>);
    reg.register_command(to_sell_schema("hesap.sparse.to_sell.c64", "CSR -> SELL; returns num_slices (Complex<f64>)."),
                         &impl_to_sell_complex<crd::f64>);

    // spmv_adjoint.<T> : y = alpha*A^H*x + beta*y (conjugate transpose). Complex
    // only -- for real T the adjoint == transpose (see spmv_transpose).
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_adjoint.c32", "y = alpha*A^H*x + beta*y (CSR, Complex<f32>)."),
        &impl_spmv_complex<crd::f32, Trans::ConjTranspose>);
    reg.register_command(
        make_spmv_schema(alloc, "hesap.sparse.spmv_adjoint.c64", "y = alpha*A^H*x + beta*y (CSR, Complex<f64>)."),
        &impl_spmv_complex<crd::f64, Trans::ConjTranspose>);

    // inner_indices : row k's stored column indices (type-agnostic; structure only).
    {
        CommandSchema s = make_triplet_schema(alloc, "hesap.sparse.inner_indices",
                                              "Row k's stored column indices (BinaryBlob f64; structure only).",
                                              OutputKind::BinaryBlob, false);
        add_param(s, alloc, "k", "Row index", ParamKind::U64, true);
        reg.register_command(std::move(s), &impl_inner_indices);
    }
})
