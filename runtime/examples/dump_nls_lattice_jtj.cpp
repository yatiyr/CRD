// dump_nls_lattice_jtj -- Phase 3.1.6 v7-e-2: generate a representative, factorization-dominated
// nonlinear-least-squares normal matrix JᵀJ and write it as a Matrix-Market symmetric file, for the
// SupernodalCholesky-vs-CHOLMOD head-to-head (bench_hesap_cholesky_vs_cholmod) — CHOLMOD is exactly the
// factorization Ceres-sparse uses, so a per-factor win on the ACTUAL NLS JᵀJ is the honest v7-e-2 crush
// evidence (advisor-directed: measure on the real matrix, don't extrapolate from the v5a FEA corpus).
//
// THE PROBLEM (genuinely nonlinear-LS, 3D-elasticity structure — the cloth/deformation/FEA target):
// an s×s×s lattice of nodes, 3 position DOF each (n = 3·s³ variables). Residuals:
//   * per axis-aligned edge (i,j):  r = ‖p_i − p_j‖ − L0   (nonlinear length constraint; Jacobian row
//     has the unit edge vector ±û on the 6 DOF of i and j — this is what makes it an NLS, not a linear LS),
//   * per DOF: a soft anchor  r = w·(x − target)   (1 nonzero; guarantees full column rank ⇒ JᵀJ ≻ 0).
// JᵀJ then couples each node's 3×3 block to its ≤6 grid neighbors ⇒ a 3D vector-Laplacian / elasticity
// sparsity (the hood/ldoor family) with substantial AMD fill ⇒ factorization-dominated at scale. The
// Jacobian is evaluated at a deterministically-perturbed configuration so the edge unit vectors are
// well-defined (the moat ethos: reproducible, no RNG).
//
// JᵀJ is formed via sp::transpose(J) then sp::spgemm(Jᵀ,J) — the EXACT path minimize_levenberg_marquardt_sparse
// uses — so the dumped matrix is byte-for-byte what the sparse-LM would factor. Cerid's own SupernodalCholesky
// factors it first to confirm SPD (info==0) and report the fill + serial factor time (a sanity check that the
// matrix is the right regime; the CHOLMOD race is the bench's job).
//
// Usage:  dump_nls_lattice_jtj <out_dir> [s1 s2 ...]
//   writes <out_dir>/nls_lat<s>/nls_lat<s>.mtx for each size (default sizes: 24 32).
// Pure crd — no external deps; raw f64 (lower numeric layer, ADR-0078 §5).

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/direct/supernodal_cholesky.hpp>
#include <crd/hesap/sparse/convert.hpp>
#include <crd/hesap/sparse/spgemm.hpp>
#include <crd/hesap/sparse/sparse_matrix.hpp>
#include <crd/hesap/sparse/triplet_builder.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <chrono>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace sp = crd::hesap::sparse;
namespace dir = crd::hesap::direct;
namespace fs = crd::platform::fs;
using Csr = sp::SparseMatrix<crd::f64, sp::SparseFormat::Csr>;

crd::memory::GrowableTlsfAllocator g_alloc;

// Deterministic, reproducible perturbation in [-0.3, 0.3] (PCG-style integer hash — no RNG state).
crd::f64 pert(crd::u32 seed) noexcept
{
    seed = seed * 747796405U + 2891336453U;
    crd::u32 w = ((seed >> ((seed >> 28U) + 4U)) ^ seed) * 277803737U;
    w = (w >> 22U) ^ w;
    return (static_cast<crd::f64>(w) / 4294967296.0 - 0.5) * 0.6;
}

// Build the analytic Jacobian J (m×n CSR) of the s³ elastic lattice at a perturbed configuration.
Csr build_lattice_jacobian(crd::u32 s)
{
    const crd::u32 nn = s * s * s;            // node count
    const crd::u32 n = 3U * nn;               // variables (3 DOF / node)
    const crd::u32 edges = 3U * s * s * (s - 1U);
    const crd::u32 m = edges + n;             // residuals: edges + per-DOF anchors
    const crd::f64 w = 0.1;                   // anchor weight (soft data term ⇒ SPD)
    const crd::f64 l0 = 1.0;                  // rest edge length

    // Perturbed node positions p[node*3 + d].
    crd::containers::Array<crd::f64> p(&g_alloc);
    p.resize(static_cast<crd::usize>(n));
    for (crd::u32 node = 0; node < nn; ++node)
    {
        const crd::u32 x = node % s;
        const crd::u32 y = (node / s) % s;
        const crd::u32 z = node / (s * s);
        p[node * 3U + 0U] = static_cast<crd::f64>(x) + pert(node * 3U + 0U);
        p[node * 3U + 1U] = static_cast<crd::f64>(y) + pert(node * 3U + 1U);
        p[node * 3U + 2U] = static_cast<crd::f64>(z) + pert(node * 3U + 2U);
    }

    sp::TripletBuilder<crd::f64> jb(&g_alloc, m, n);
    jb.reserve(static_cast<crd::usize>(edges) * 6U + n);
    crd::u32 row = 0;
    auto add_edge = [&](crd::u32 i, crd::u32 j)
    {
        const crd::f64 dx = p[i * 3U + 0U] - p[j * 3U + 0U];
        const crd::f64 dy = p[i * 3U + 1U] - p[j * 3U + 1U];
        const crd::f64 dz = p[i * 3U + 2U] - p[j * 3U + 2U];
        const crd::f64 len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const crd::f64 inv = len > 0.0 ? 1.0 / len : 0.0;
        const crd::f64 u0 = dx * inv; // ∂‖p_i−p_j‖/∂p_i
        const crd::f64 u1 = dy * inv;
        const crd::f64 u2 = dz * inv;
        (void)l0;                                                  // residual value unused; J only
        jb.add(row, i * 3U + 0U, u0);
        jb.add(row, i * 3U + 1U, u1);
        jb.add(row, i * 3U + 2U, u2);
        jb.add(row, j * 3U + 0U, -u0);
        jb.add(row, j * 3U + 1U, -u1);
        jb.add(row, j * 3U + 2U, -u2);
        ++row;
    };
    for (crd::u32 z = 0; z < s; ++z)
    {
        for (crd::u32 y = 0; y < s; ++y)
        {
            for (crd::u32 x = 0; x < s; ++x)
            {
                const crd::u32 i = (z * s + y) * s + x;
                if (x + 1U < s)
                    add_edge(i, i + 1U);
                if (y + 1U < s)
                    add_edge(i, i + s);
                if (z + 1U < s)
                    add_edge(i, i + s * s);
            }
        }
    }
    // Per-DOF soft anchors (full column rank ⇒ JᵀJ SPD).
    for (crd::u32 c = 0; c < n; ++c)
    {
        jb.add(row, c, w);
        ++row;
    }
    return jb.compress();
}

// Write JᵀJ's lower triangle (incl. diagonal) as a Matrix-Market symmetric coordinate file.
bool write_symmetric_mtx(const char* path, const Csr& a)
{
    const sp::SparsePattern& pat = a.pattern();
    const crd::u32 n = pat.n_outer();
    const crd::f64* vals = a.values().values.data();

    // Count lower-triangle entries.
    crd::u64 lo_nnz = 0;
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 st = pat.outer_ptr[i];
        const crd::u32 cnt = pat.inner_count(i);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            if (pat.inner_idx[st + k] <= i)
            {
                ++lo_nnz;
            }
        }
    }

    crd::containers::String out(&g_alloc);
    out.reserve(static_cast<crd::usize>(lo_nnz) * 28U + 128U);
    char buf[64];
    auto append = [&](const char* sptr)
    {
        for (const char* q = sptr; *q; ++q)
        {
            out.push_back(*q);
        }
    };
    auto append_u64 = [&](crd::u64 v)
    {
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        (void)ec;
        for (char* q = buf; q < ptr; ++q)
        {
            out.push_back(*q);
        }
    };
    auto append_f64 = [&](crd::f64 v)
    {
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), v);
        (void)ec;
        for (char* q = buf; q < ptr; ++q)
        {
            out.push_back(*q);
        }
    };

    append("%%MatrixMarket matrix coordinate real symmetric\n");
    append_u64(n);
    out.push_back(' ');
    append_u64(n);
    out.push_back(' ');
    append_u64(lo_nnz);
    out.push_back('\n');
    for (crd::u32 i = 0; i < n; ++i)
    {
        const crd::u32 st = pat.outer_ptr[i];
        const crd::u32 cnt = pat.inner_count(i);
        for (crd::u32 k = 0; k < cnt; ++k)
        {
            const crd::u32 j = pat.inner_idx[st + k];
            if (j <= i)
            {
                append_u64(static_cast<crd::u64>(i) + 1U);
                out.push_back(' ');
                append_u64(static_cast<crd::u64>(j) + 1U);
                out.push_back(' ');
                append_f64(vals[st + k]);
                out.push_back('\n');
            }
        }
    }
    return fs::write_file_text(fs::Path{path}, crd::containers::StringView{out.c_str(), out.size()});
}

void gen(const char* out_dir, crd::u32 s)
{
    const auto t0 = Clock::now();
    Csr jmat = build_lattice_jacobian(s);
    Csr jt = sp::transpose(jmat, &g_alloc);
    Csr jtj = sp::spgemm(jt, jmat, &g_alloc); // JᵀJ — the exact sparse-LM path
    const auto t1 = Clock::now();

    const crd::u32 n = jtj.pattern().n_outer();
    const crd::u64 jtj_nnz = jtj.pattern().nnz();
    const crd::f64 form_ms = std::chrono::duration<crd::f64, std::milli>(t1 - t0).count();

    // SPD sanity check (the soft anchors make JᵀJ provably full-rank ⇒ SPD; this only re-confirms the
    // construction). The factor's m_lx is the 3D-Cholesky fill — multi-GB at large s — so only run it for
    // small n; the CHOLMOD bench factors the matrix at full scale anyway.
    bool spd = true;
    crd::u64 fill = 0;
    crd::f64 fac_ms = 0.0;
    if (n <= 30000U)
    {
        const auto tf0 = Clock::now();
        auto f = dir::factor_supernodal_cholesky<crd::f64>(jtj.pattern(),
                                                           {jtj.values().values.data(), jtj.values().values.size()},
                                                           &g_alloc, dir::kSupernodeRelax, 1);
        const auto tf1 = Clock::now();
        spd = f.info() == 0;
        fill = f.factor_nnz();
        fac_ms = std::chrono::duration<crd::f64, std::milli>(tf1 - tf0).count();
    }

    crd::containers::String dir_path(&g_alloc);
    dir_path.append(out_dir);
    dir_path.append("/nls_lat");
    char sb[16];
    auto [sp_ptr, sp_ec] = std::to_chars(sb, sb + sizeof(sb), s);
    (void)sp_ec;
    *sp_ptr = '\0';
    dir_path.append(sb);
    (void)fs::create_directories(fs::Path{dir_path.c_str()});

    crd::containers::String file_path(&g_alloc);
    file_path.append(dir_path.c_str());
    file_path.append("/nls_lat");
    file_path.append(sb);
    file_path.append(".mtx");

    const bool ok = spd ? write_symmetric_mtx(file_path.c_str(), jtj) : false;

    const char* spd_status = "skip";
    if (n <= 30000U)
    {
        spd_status = spd ? "SPD" : "NOT-SPD!";
    }
    std::printf("  s=%-3u n=%-8u JtJ_nnz=%-9llu | form(J->JtJ)=%8.1f ms | Cerid factor: %s fill=%-10llu %8.1f ms | "
                "wrote=%s %s\n",
                s, n, static_cast<unsigned long long>(jtj_nnz), form_ms, spd_status,
                static_cast<unsigned long long>(fill), fac_ms, ok ? "OK" : "FAIL", file_path.c_str());
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: dump_nls_lattice_jtj <out_dir> [s1 s2 ...]\n");
        return 2;
    }
    const char* out_dir = argv[1];
    crd::jobs::init();
    std::printf("[dump_nls_lattice_jtj] 3D elastic-lattice NLS JᵀJ -> %s (symmetric Matrix-Market)\n", out_dir);
    if (argc >= 3)
    {
        for (int i = 2; i < argc; ++i)
        {
            gen(out_dir, static_cast<crd::u32>(std::strtoul(argv[i], nullptr, 10)));
        }
    }
    else
    {
        gen(out_dir, 24);
        gen(out_dir, 32);
    }
    crd::jobs::shutdown();
    return 0;
}
