// Ordering CLI registration (ADR-0081). Type-agnostic: orderings operate on a
// matrix's STRUCTURE only, so each command is registered once (not ×4). Triplets
// travel as parallel I64Array (rows, cols); values are irrelevant to structure
// (a dummy 1.0 is used to build the pattern).

#include <crd/hesap/cli/arg_value.hpp>
#include <crd/hesap/cli/command_registry.hpp>
#include <crd/hesap/ordering/cli_anchor.hpp>
#include <crd/hesap/ordering/ordering.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>

#include <utility>

namespace crd::hesap::ordering
{
void register_ordering_cli_anchor() noexcept {}
} // namespace crd::hesap::ordering

namespace
{
using namespace crd::hesap;
using namespace crd::hesap::cli;
using namespace crd::hesap::ordering;

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

// Build a structure-only CSR matrix from triplet args (dummy 1.0 values). The
// matrix is kept alive by the caller so its `.pattern()` ref stays valid.
using OrderingCsr = crd::hesap::sparse::SparseMatrix<crd::f64, crd::hesap::sparse::SparseFormat::Csr>;

bool build_matrix(const CommandArgs& args, OrderingCsr& out, crd::memory::IAllocator* alloc, const char*& err)
{
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols)
    {
        err = "missing rows/cols";
        return false;
    }
    if (*rows != *cols)
    {
        err = "ordering requires a square matrix (rows == cols)";
        return false;
    }
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    if (tr.size() != tc.size())
    {
        err = "triplet_rows / triplet_cols length mismatch";
        return false;
    }
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(*rows), static_cast<crd::u32>(*cols));
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), 1.0);
    }
    out = tb.compress(); // move-assign
    return true;
}

CommandResult impl_bandwidth(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(bandwidth(mat.pattern())));
}

CommandResult impl_profile(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(profile(mat.pattern())));
}

CommandResult impl_nnz_l(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz_l(mat.pattern(), args.alloc)));
}

CommandResult impl_rcm_bandwidth(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = rcm_order(mat.pattern(), args.alloc);
    auto rp = apply_symmetric(mat.pattern(), p, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(bandwidth(rp)));
}

CommandResult impl_rcm_nnz_l(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = rcm_order(mat.pattern(), args.alloc);
    auto rp = apply_symmetric(mat.pattern(), p, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz_l(rp, args.alloc)));
}

CommandResult impl_rcm(const CommandArgs& args) // returns the permutation as an f64 blob
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = rcm_order(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(p.size());
    for (crd::u32 i = 0; i < p.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(p.perm[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_amd_nnz_l(const CommandArgs& args)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = amd_order(mat.pattern(), args.alloc);
    auto rp = apply_symmetric(mat.pattern(), p, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz_l(rp, args.alloc)));
}

CommandResult impl_amd(const CommandArgs& args) // returns the AMD permutation as an f64 blob
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = amd_order(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(p.size());
    for (crd::u32 i = 0; i < p.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(p.perm[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_etree(const CommandArgs& args) // returns parent[] as an f64 blob (kNoParent → -1)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto et = elimination_tree(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(et.size());
    for (crd::usize i = 0; i < et.size(); ++i)
    {
        out.push_back(et[i] == kNoParent ? -1.0 : static_cast<crd::f64>(et[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_postorder(const CommandArgs& args) // postorder of the etree (BinaryBlob f64)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto et = elimination_tree(mat.pattern(), args.alloc);
    auto post = postorder({et.data(), et.size()}, args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(post.size());
    for (crd::usize i = 0; i < post.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(post[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_symbolic_nnz_l(const CommandArgs& args) // nnz(L) via the full symbolic factorisation
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    const auto sf = symbolic_factorize(mat.pattern(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(sf.nnz()));
}

CommandResult impl_supernode_count(const CommandArgs& args) // number of fundamental supernodes
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    const auto sf = symbolic_factorize(mat.pattern(), args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(sf.nsuper));
}

CommandResult impl_supernodes(const CommandArgs& args) // supernode column boundaries (BinaryBlob f64, len nsuper+1)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    const auto sf = symbolic_factorize(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(sf.super.size());
    for (crd::usize i = 0; i < sf.super.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(sf.super[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_nd_bipartition(const CommandArgs& args) // multilevel-ND 2-way partition part[v] (BinaryBlob f64)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto part = nd_bipartition(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(part.size());
    for (crd::u32 i = 0; i < part.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(part[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_nd_order(const CommandArgs& args) // nested-dissection permutation (BinaryBlob f64)
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = nd_order(mat.pattern(), args.alloc);
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(p.size());
    for (crd::u32 i = 0; i < p.size(); ++i)
    {
        out.push_back(static_cast<crd::f64>(p.perm[i]));
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

CommandResult impl_nd_nnz_l(const CommandArgs& args) // nnz(L) after nested-dissection reordering
{
    OrderingCsr mat(args.alloc);
    const char* err = nullptr;
    if (!build_matrix(args, mat, args.alloc, err))
    {
        return error_result(args.alloc, err);
    }
    auto p = nd_order(mat.pattern(), args.alloc);
    auto rp = apply_symmetric(mat.pattern(), p, args.alloc);
    return scalar_result(args.alloc, static_cast<crd::f64>(nnz_l(rp, args.alloc)));
}

// MC64 max-weight matching + scaling. Unlike the structure-only orderings this is VALUE-aware
// (matches by |a_ij|), so it reads a real `values` F64Array. Returns [colperm(n), D_r(n), D_c(n)]
// concatenated as a BinaryBlob f64 (the C++ API covers all 4 type variants).
CommandResult impl_mc64(const CommandArgs& args)
{
    const auto rows = args.get_u64("rows");
    const auto cols = args.get_u64("cols");
    if (!rows || !cols || *rows != *cols)
    {
        return error_result(args.alloc, "mc64 requires a square matrix");
    }
    const auto tr = args.get_i64_array("triplet_rows");
    const auto tc = args.get_i64_array("triplet_cols");
    const auto vv = args.get_f64_array("values");
    if (tr.size() != tc.size() || tr.size() != vv.size())
    {
        return error_result(args.alloc, "mc64: triplet_rows/cols/values length mismatch");
    }
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(args.alloc, static_cast<crd::u32>(*rows),
                                                    static_cast<crd::u32>(*cols));
    tb.reserve(tr.size());
    for (crd::usize k = 0; k < tr.size(); ++k)
    {
        tb.add(static_cast<crd::u32>(tr[k]), static_cast<crd::u32>(tc[k]), vv[k]);
    }
    auto a = tb.compress();
    auto s = mc64_match_and_scale<crd::f64>(a, args.alloc);
    const crd::u32 n = a.rows();
    crd::containers::Array<crd::f64> out(args.alloc);
    out.reserve(static_cast<crd::usize>(n) * 3);
    for (crd::u32 i = 0; i < n; ++i)
    {
        out.push_back(static_cast<crd::f64>(s.colperm[i]));
    }
    for (crd::u32 i = 0; i < n; ++i)
    {
        out.push_back(s.dr[i]);
    }
    for (crd::u32 j = 0; j < n; ++j)
    {
        out.push_back(s.dc[j]);
    }
    return blob_result(args.alloc, crd::containers::ConstSpan<crd::f64>{out.data(), out.size()});
}

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

CommandSchema make_ordering_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc,
                                   OutputKind out_kind)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{name, alloc};
    s.description = crd::containers::String{desc, alloc};
    s.output.kind = out_kind;
    s.required_caps.bits = Capability::kHesapCompute;
    s.idempotent = true;
    add_param(s, alloc, "rows", "Number of matrix rows (== cols; square)", ParamKind::U64, true);
    add_param(s, alloc, "cols", "Number of matrix columns (== rows; square)", ParamKind::U64, true);
    add_param(s, alloc, "triplet_rows", "COO row indices (I64Array; structure only)", ParamKind::I64, true);
    add_param(s, alloc, "triplet_cols", "COO column indices (I64Array; structure only)", ParamKind::I64, true);
    return s;
}

CommandSchema make_mc64_schema(crd::memory::IAllocator* alloc, const char* name, const char* desc)
{
    CommandSchema s = make_ordering_schema(alloc, name, desc, OutputKind::BinaryBlob);
    add_param(s, alloc, "values", "COO values (F64Array; matched by |value|)", ParamKind::F64, true);
    return s;
}
} // namespace

// Registration uses crd allocators (abort on OOM, never throw); the std bad_alloc path the check
// traces is unreachable, and the registrar ctor is noexcept (would terminate, not escape) regardless.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
CRD_HESAP_CLI_REGISTER_MODULE(
    [](CommandRegistry& reg)
    {
        auto* alloc = crd::memory::default_allocator();
        reg.register_command(
            make_ordering_schema(alloc, "hesap.ordering.bandwidth", "Matrix bandwidth max|i-j|.", OutputKind::Scalar),
            &impl_bandwidth);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.profile", "Matrix profile (envelope size).",
                                                  OutputKind::Scalar),
                             &impl_profile);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.nnz_l", "nnz(L) of chol(A), natural ordering.",
                                                  OutputKind::Scalar),
                             &impl_nnz_l);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.rcm_bandwidth",
                                                  "Bandwidth after RCM reordering.", OutputKind::Scalar),
                             &impl_rcm_bandwidth);
        reg.register_command(
            make_ordering_schema(alloc, "hesap.ordering.rcm_nnz_l", "nnz(L) after RCM reordering.", OutputKind::Scalar),
            &impl_rcm_nnz_l);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.rcm", "RCM permutation (BinaryBlob f64).",
                                                  OutputKind::BinaryBlob),
                             &impl_rcm);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.amd_nnz_l",
                                                  "nnz(L) after AMD (approximate minimum degree) reordering.",
                                                  OutputKind::Scalar),
                             &impl_amd_nnz_l);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.amd", "AMD permutation (BinaryBlob f64).",
                                                  OutputKind::BinaryBlob),
                             &impl_amd);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.etree",
                                                  "Elimination tree parent[] (BinaryBlob f64; root = -1).",
                                                  OutputKind::BinaryBlob),
                             &impl_etree);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.postorder",
                                                  "Postorder of the elimination tree (BinaryBlob f64).",
                                                  OutputKind::BinaryBlob),
                             &impl_postorder);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.symbolic_nnz_l",
                                                  "nnz(L) via the full symbolic factorisation (natural ordering).",
                                                  OutputKind::Scalar),
                             &impl_symbolic_nnz_l);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.supernode_count",
                                                  "Number of fundamental supernodes of chol(A).", OutputKind::Scalar),
                             &impl_supernode_count);
        reg.register_command(
            make_ordering_schema(alloc, "hesap.ordering.supernodes",
                                 "Fundamental supernode column boundaries (BinaryBlob f64, len nsuper+1).",
                                 OutputKind::BinaryBlob),
            &impl_supernodes);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.nd_bipartition",
                                                  "Multilevel-ND 2-way partition part[v] in {0,1} (BinaryBlob f64).",
                                                  OutputKind::BinaryBlob),
                             &impl_nd_bipartition);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.nd_order",
                                                  "Nested-dissection fill-reducing permutation (BinaryBlob f64).",
                                                  OutputKind::BinaryBlob),
                             &impl_nd_order);
        reg.register_command(make_ordering_schema(alloc, "hesap.ordering.nd_nnz_l",
                                                  "nnz(L) after nested-dissection reordering.", OutputKind::Scalar),
                             &impl_nd_nnz_l);
        reg.register_command(
            make_mc64_schema(alloc, "hesap.ordering.mc64",
                             "MC64 max-weight matching + scaling: [colperm(n), D_r(n), D_c(n)] (BinaryBlob f64)."),
            &impl_mc64);
    })
