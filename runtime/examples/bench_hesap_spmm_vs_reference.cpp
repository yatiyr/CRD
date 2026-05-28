// bench_hesap_spmm_vs_reference -- Phase 3.1.6 v1e-1.
//
// C = A*B (sparse A x dense B, multiple RHS r) on REAL SuiteSparse matrices:
// Cerid serial + parallel vs Eigen sparse*dense (ST + MT). Honest framing:
// Eigen's sparse*dense is ALSO one-pass-over-A; the win is row-PARALLELISM, so
// the gate (>=2x) is meaningful vs Eigen-MT. Eigen + Cerid both row-major dense.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON. Matrices fetched at configure
// (CRD_SUITESPARSE_DIR), read with a crd-native Matrix-Market reader.
//
// crd conventions throughout: crd::containers::Array (never std::vector),
// crd::platform::fs::read_file_text + manual parse (never std::ifstream/string),
// a named GrowableTlsfAllocator (never malloc / default_allocator); crd::f64/usize/
// i32/u32 (never raw). Raw double only at the Eigen C++ API boundary.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/sparse/sparse.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <Eigen/Sparse>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib> // strtol / strtod
#include <cstring> // strncmp

#ifndef CRD_SUITESPARSE_DIR
#define CRD_SUITESPARSE_DIR "."
#endif

namespace
{
using Clock = std::chrono::high_resolution_clock;
namespace fs = crd::platform::fs;

crd::memory::GrowableTlsfAllocator g_alloc;

struct Trip
{
    crd::i32 r, c;
    crd::f64 v;
};

// crd-native Matrix-Market reader (read_file_text + strtol/strtod; handles
// pattern/symmetric/hermitian banners). DATA → crd::Array<Trip>.
bool read_mtx(const char* path, crd::i32& n, crd::containers::Array<Trip>& trips)
{
    crd::containers::String text(&g_alloc);
    if (!fs::read_file_text(fs::Path{path}, text))
    {
        return false;
    }
    const char* p = text.c_str();
    const char* end = p + text.size();
    bool is_pattern = false;
    bool symmetric = false;
    bool dims_read = false;
    crd::i32 nnz = 0;
    crd::i32 seen = 0;
    while (p < end)
    {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        {
            ++p;
        }
        if (p >= end)
        {
            break;
        }
        if (*p == '%')
        {
            const char* eol = p;
            while (eol < end && *eol != '\n')
            {
                ++eol;
            }
            if (!dims_read)
            {
                for (const char* q = p; q + 9 <= eol; ++q)
                {
                    if (std::strncmp(q, "pattern", 7) == 0)
                    {
                        is_pattern = true;
                    }
                    if (std::strncmp(q, "symmetric", 9) == 0 || std::strncmp(q, "hermitian", 9) == 0)
                    {
                        symmetric = true;
                    }
                }
            }
            p = eol;
            continue;
        }
        char* np = nullptr;
        if (!dims_read)
        {
            const crd::i32 rows = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            const crd::i32 cols = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            nnz = static_cast<crd::i32>(std::strtol(p, &np, 10));
            p = np;
            n = rows > cols ? rows : cols;
            trips.reserve(static_cast<crd::usize>(nnz) * (symmetric ? 2 : 1));
            dims_read = true;
            continue;
        }
        const crd::i32 r = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        const crd::i32 c = static_cast<crd::i32>(std::strtol(p, &np, 10)) - 1;
        p = np;
        crd::f64 v = 1.0;
        if (!is_pattern)
        {
            v = std::strtod(p, &np);
            p = np;
        }
        trips.push_back(Trip{r, c, v});
        if (symmetric && r != c)
        {
            trips.push_back(Trip{c, r, v});
        }
        if (++seen >= nnz)
        {
            break;
        }
    }
    return dims_read;
}

template <typename Fn> crd::f64 best_ms(Fn&& fn)
{
    fn();
    crd::f64 best = 1e30;
    for (crd::i32 rep = 0; rep < 7; ++rep)
    {
        const auto t0 = Clock::now();
        fn();
        const auto t1 = Clock::now();
        best = std::min(best, std::chrono::duration<crd::f64, std::milli>(t1 - t0).count());
    }
    return best;
}

void run(const char* name, crd::i32 r)
{
    crd::containers::String path(&g_alloc);
    path.append(CRD_SUITESPARSE_DIR);
    path.append("/");
    path.append(name);
    path.append("/");
    path.append(name);
    path.append(".mtx");
    crd::i32 n = 0;
    crd::containers::Array<Trip> trips(&g_alloc);
    if (!read_mtx(path.c_str(), n, trips))
    {
        std::printf("  %-10s r=%-4d SKIP (not found)\n", name, r);
        return;
    }

    crd::memory::IAllocator* alloc = &g_alloc;
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(trips.size());
    for (const Trip& t : trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();

    crd::containers::Array<crd::f64> b(alloc);
    crd::containers::Array<crd::f64> c(alloc);
    b.resize(static_cast<crd::usize>(n) * r);
    c.resize(static_cast<crd::usize>(n) * r);
    for (crd::usize i = 0; i < b.size(); ++i)
    {
        b[i] = 1.0 + 0.01 * static_cast<crd::f64>(i % 31);
    }
    const auto ru = static_cast<crd::u32>(r);

    const crd::f64 t_ser =
        best_ms([&]() { crd::hesap::sparse::spmm<crd::f64>(1.0, a, b.data(), ru, ru, 0.0, c.data(), ru); });
    const crd::f64 t_par = best_ms(
        [&]()
        {
            crd::hesap::sparse::spmm_parallel<crd::f64>(1.0, a, b.data(), ru, ru, 0.0, c.data(), ru);
            crd::jobs::frame_reset();
        });

    crd::containers::Array<Eigen::Triplet<double>> et(alloc);
    et.reserve(trips.size());
    for (const Trip& t : trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    using RowMat = Eigen::Matrix<double, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>;
    RowMat eb = Eigen::Map<RowMat>(b.data(), n, r);
    RowMat ec(n, r);

    Eigen::setNbThreads(1);
    const crd::f64 t_eig_st = best_ms([&]() { ec.noalias() = ea * eb; });
    Eigen::setNbThreads(0);
    const crd::f64 t_eig_mt = best_ms([&]() { ec.noalias() = ea * eb; });

    const crd::f64 mt = t_eig_mt / t_par;
    std::printf("  %-10s r=%-4d | Cerid ser=%8.3f par=%8.3f  Eigen ST=%8.3f MT=%8.3f ms  par/Eigen-MT=%.2fx %s\n", name,
                r, t_ser, t_par, t_eig_st, t_eig_mt, mt, (mt >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spmm] C=A*B (sparse x dense, r RHS) on SuiteSparse: Cerid vs Eigen (best-of-7). "
                "Gate: par/Eigen-MT>=2x on r>=4\n");
    for (crd::i32 r : {1, 4, 32, 128})
    {
        run("bcsstk25", r);
        run("gemat11", r);
    }
    crd::jobs::shutdown();
    return 0;
}
