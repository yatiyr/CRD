// bench_hesap_spgemm_vs_reference -- Phase 3.1.6 v1d-2.
//
// C = A*A on REAL SuiteSparse matrices: Cerid serial + parallel spgemm vs
// Eigen (single-threaded -- Eigen does NOT multi-thread sparse*sparse). The
// v1d gate: median Cerid-parallel / Eigen >= 1.3x. Matrices are fetched at
// configure time (CMake file(DOWNLOAD), gated by CRD_BUILD_HESAP_VS_REFERENCE)
// into CRD_SUITESPARSE_DIR; we read the .mtx with a crd-native Matrix-Market reader.
//
// Built only when CRD_BUILD_HESAP_VS_REFERENCE=ON.
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

struct Mtx
{
    crd::i32 n = 0; // square assumed (A*A); rows==cols
    crd::containers::Array<Trip> trips{&g_alloc};
    bool ok = false;
};

// crd-native Matrix-Market reader (read_file_text + strtol/strtod; pattern/symmetric/hermitian).
Mtx read_mtx(const char* path)
{
    Mtx out;
    crd::containers::String text(&g_alloc);
    if (!fs::read_file_text(fs::Path{path}, text))
    {
        return out;
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
            out.n = rows > cols ? rows : cols; // square-pad by max dim
            out.trips.reserve(static_cast<crd::usize>(nnz) * (symmetric ? 2 : 1));
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
        out.trips.push_back(Trip{r, c, v});
        if (symmetric && r != c)
        {
            out.trips.push_back(Trip{c, r, v});
        }
        if (++seen >= nnz)
        {
            break;
        }
    }
    out.ok = dims_read;
    return out;
}

template <typename Fn> crd::f64 best_ms(Fn&& fn)
{
    fn(); // warmup
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

void run(const char* name, const char* group)
{
    crd::containers::String path(&g_alloc);
    path.append(CRD_SUITESPARSE_DIR);
    path.append("/");
    path.append(name);
    path.append("/");
    path.append(name);
    path.append(".mtx");
    Mtx mtx = read_mtx(path.c_str());
    if (!mtx.ok)
    {
        std::printf("  %-12s (%s)  SKIP (not found: %s)\n", name, group, path.c_str());
        return;
    }
    const crd::i32 n = mtx.n;

    crd::memory::IAllocator* alloc = &g_alloc;
    crd::hesap::sparse::TripletBuilder<crd::f64> tb(alloc, static_cast<crd::u32>(n), static_cast<crd::u32>(n));
    tb.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        tb.add(static_cast<crd::u32>(t.r), static_cast<crd::u32>(t.c), t.v);
    }
    auto a = tb.compress();

    const crd::f64 t_ser = best_ms(
        [&]()
        {
            auto c = crd::hesap::sparse::spgemm(a, a, alloc);
            volatile crd::usize s = c.nnz();
            (void)s;
        });
    const crd::f64 t_par = best_ms(
        [&]()
        {
            auto c = crd::hesap::sparse::spgemm_parallel(a, a, alloc);
            crd::jobs::frame_reset();
            volatile crd::usize s = c.nnz();
            (void)s;
        });

    crd::containers::Array<Eigen::Triplet<double>> et(alloc);
    et.reserve(mtx.trips.size());
    for (const Trip& t : mtx.trips)
    {
        et.push_back(Eigen::Triplet<double>(t.r, t.c, t.v));
    }
    Eigen::SparseMatrix<double, Eigen::RowMajor> ea(n, n);
    ea.setFromTriplets(et.data(), et.data() + et.size());
    Eigen::setNbThreads(1);
    Eigen::SparseMatrix<double, Eigen::RowMajor> ec;
    const crd::f64 t_eig = best_ms(
        [&]()
        {
            ec = ea * ea; // natural Eigen idiom (no .pruned() — matches our no-prune behavior)
            volatile int s = static_cast<int>(ec.nonZeros());
            (void)s;
        });

    const crd::f64 ratio = t_eig / t_par;
    std::printf("  %-12s nnz(A)=%-8lld nnz(C)=%-9lld | Cerid ser=%8.2f par=%8.2f  Eigen=%8.2f ms  par/Eigen=%.2fx %s\n",
                name, static_cast<long long>(a.nnz()), static_cast<long long>(ec.nonZeros()), t_ser, t_par, t_eig,
                ratio, (ratio >= 1.0 ? "WIN" : "loss"));
}
} // namespace

int main()
{
    crd::jobs::init();
    Eigen::initParallel();
    std::printf("[bench_hesap_spgemm] C=A*A on SuiteSparse: Cerid serial/parallel vs Eigen-ST (best-of-7). "
                "Gate: par/Eigen>=1.3x median\n");
    run("bcsstk13", "HB");
    run("bcsstk24", "HB");
    run("bcsstk25", "HB");
    run("sherman3", "HB");
    run("gemat11", "HB");
    run("west2021", "HB");
    crd::jobs::shutdown();
    return 0;
}
