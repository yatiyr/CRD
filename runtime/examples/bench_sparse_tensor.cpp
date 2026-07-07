// v14-i sparse-tensor bench: OUR SparseCoo/SparseCsf kernels on the shared
// binary corpora written by scripts/v14i_sparse_oracle.py --bench (bit-exact
// same inputs as the scipy/torch peer timings in that script). Protocol:
// pinned (taskset -c 4), matched 1 thread, best-of-5, checksums printed for
// cross-validation against the python side.
#include <crd/hesap/tensor/sparse.hpp>
#include <crd/hesap/tensor/sparse_mttkrp.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

using crd::hesap::tensor::contract_mode;
using crd::hesap::tensor::coo_to_csf;
using crd::hesap::tensor::mttkrp;
using crd::hesap::tensor::reduce_max_mode;
using crd::hesap::tensor::reduce_max_root;
using crd::hesap::tensor::reduce_sum;
using crd::hesap::tensor::reduce_sum_mode;
using crd::hesap::tensor::reduce_sum_root;
using crd::hesap::tensor::sparse_add;
using crd::hesap::tensor::sparse_mul;
using crd::hesap::tensor::SparseCoo;
using crd::hesap::tensor::SparseCsf;
using crd::hesap::tensor::Tensor;
using crd::hesap::tensor::TensorStatus;
using crd::hesap::tensor::TensorView;

namespace
{

double now_ms()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) * 1e-6;
}

template <typename F> double best_of(F&& fn, int reps = 5)
{
    double best = 1e300;
    for (int r = 0; r < reps; ++r)
    {
        const double t0 = now_ms();
        fn();
        const double dt = now_ms() - t0;
        best = dt < best ? dt : best;
    }
    return best;
}

double checksum(const crd::f64* v, crd::u64 n)
{
    double s = 0.0;
    for (crd::u64 i = 0; i < n; ++i)
    {
        s += v[i];
    }
    return s;
}

bool load_coo(const char* path, crd::memory::IAllocator* alloc, SparseCoo<crd::f64>& out)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        std::fprintf(stderr, "cannot open %s (run scripts/v14i_sparse_oracle.py --bench first)\n", path);
        return false;
    }
    crd::u64 rank = 0;
    if (std::fread(&rank, sizeof(crd::u64), 1, f) != 1U || rank == 0U || rank > 8U)
    {
        std::fclose(f);
        return false;
    }
    crd::u64 shape[8];
    if (std::fread(shape, sizeof(crd::u64), rank, f) != rank)
    {
        std::fclose(f);
        return false;
    }
    crd::u64 nnz = 0;
    if (std::fread(&nnz, sizeof(crd::u64), 1, f) != 1U)
    {
        std::fclose(f);
        return false;
    }
    if (out.init({shape, static_cast<crd::usize>(rank)}, nnz) != TensorStatus::Ok)
    {
        std::fclose(f);
        return false;
    }
    (void)alloc;
    bool ok = true;
    for (crd::u32 m = 0; m < rank; ++m)
    {
        ok = ok && std::fread(out.idx_mut(m), sizeof(crd::u32), nnz, f) == nnz;
    }
    ok = ok && std::fread(out.val_mut(), sizeof(crd::f64), nnz, f) == nnz;
    std::fclose(f);
    return ok;
}

crd::f64* load_mat(const char* path, crd::memory::IAllocator* alloc, crd::u64& rows, crd::u64& cols)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr)
    {
        return nullptr;
    }
    if (std::fread(&rows, sizeof(crd::u64), 1, f) != 1U || std::fread(&cols, sizeof(crd::u64), 1, f) != 1U)
    {
        std::fclose(f);
        return nullptr;
    }
    const crd::u64 n = rows * cols;
    crd::f64* p = static_cast<crd::f64*>(alloc->allocate(n * sizeof(crd::f64), 64U));
    if (p == nullptr || std::fread(p, sizeof(crd::f64), n, f) != n)
    {
        std::fclose(f);
        return nullptr;
    }
    std::fclose(f);
    return p;
}

} // namespace

int main()
{
    static crd::memory::TlsfAllocator alloc(2ULL * 1024ULL * 1024ULL * 1024ULL);
    const char* home = std::getenv("HOME");
    if (home == nullptr)
    {
        std::fprintf(stderr, "no HOME\n");
        return 1;
    }
    char path[512];
    SparseCoo<crd::f64> t1(&alloc);
    SparseCoo<crd::f64> t2(&alloc);
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_t1.bin", home);
    if (!load_coo(path, &alloc, t1))
    {
        return 1;
    }
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_t2.bin", home);
    if (!load_coo(path, &alloc, t2))
    {
        return 1;
    }
    crd::u64 jr = 0;
    crd::u64 rr = 0;
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_fb.bin", home);
    crd::f64* fbp = load_mat(path, &alloc, jr, rr);
    crd::u64 kr = 0;
    crd::u64 rr2 = 0;
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_fc.bin", home);
    crd::f64* fcp = load_mat(path, &alloc, kr, rr2);
    crd::u64 ju = 0;
    crd::u64 fu = 0;
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_u1.bin", home);
    crd::f64* u1p = load_mat(path, &alloc, ju, fu);
    crd::u64 ku = 0;
    crd::u64 fu2 = 0;
    std::snprintf(path, sizeof(path), "%s/cerid-bench-data/v14i_u2.bin", home);
    crd::f64* u2p = load_mat(path, &alloc, ku, fu2);
    if (fbp == nullptr || fcp == nullptr || u1p == nullptr || u2p == nullptr)
    {
        std::fprintf(stderr, "matrix load failed\n");
        return 1;
    }
    const crd::u64 di = t1.shape(0);
    const crd::u64 dj = t1.shape(1);
    const crd::u64 dk = t1.shape(2);
    std::printf("== ours: shape (%llu,%llu,%llu) nnz %llu, R=%llu, F=%llu, 1 thread ==\n",
                static_cast<unsigned long long>(di), static_cast<unsigned long long>(dj),
                static_cast<unsigned long long>(dk), static_cast<unsigned long long>(t1.nnz()),
                static_cast<unsigned long long>(rr), static_cast<unsigned long long>(fu));

    // ---- MTTKRP mode-0 (CSF prebuilt, like the peers' prebuilt csr) ----
    const crd::u32 order[3] = {0U, 1U, 2U};
    SparseCsf<crd::f64> csf(&alloc);
    {
        const double t0 = now_ms();
        if (coo_to_csf(t1, {order, 3U}, csf) != TensorStatus::Ok)
        {
            return 1;
        }
        std::printf("(prep)  coo->csf         : %9.2f ms\n", now_ms() - t0);
    }
    const crd::u64 fbshape[2] = {dj, rr};
    const crd::u64 fcshape[2] = {dk, rr};
    crd::containers::Array<TensorView<const crd::f64>> facs(&alloc);
    facs.resize(3U);
    facs[1] = TensorView<const crd::f64>::contiguous(fbp, {fbshape, 2});
    facs[2] = TensorView<const crd::f64>::contiguous(fcp, {fcshape, 2});
    const crd::u64 mshape[2] = {di, rr};
    Tensor<crd::f64> mout(&alloc, {mshape, 2});
    {
        const double t = best_of(
            [&]
            {
                if (mttkrp<crd::f64>(csf, {facs.data(), 3U}, mout.view(), &alloc, 1U) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   mttkrp-mode0      : %9.2f ms   checksum %.17g\n", t, checksum(mout.data(), di * rr));
    }

    // ---- MTTKRP modes 1 and 2 (the SPLATT all-modes board; factor data
    // reuse is fine — dims are uniform and SPLATT/TACO use their own random
    // factors too, only the sparsity structure must match) ----
    for (crd::u32 target = 1; target <= 2U; ++target)
    {
        const crd::u32 order2[3] = {target, target == 1U ? 0U : 0U, target == 1U ? 2U : 1U};
        SparseCsf<crd::f64> csft(&alloc);
        if (coo_to_csf(t1, {order2, 3U}, csft) != TensorStatus::Ok)
        {
            return 1;
        }
        crd::containers::Array<TensorView<const crd::f64>> ft(&alloc);
        ft.resize(3U);
        const crd::u64 fshape[2] = {di, rr}; // all dims 1024
        for (crd::u32 m = 0; m < 3U; ++m)
        {
            ft[m] = TensorView<const crd::f64>::contiguous(m == 2U ? fcp : fbp, {fshape, 2});
        }
        const crd::u64 mshape2[2] = {t1.shape(target), rr};
        Tensor<crd::f64> mo(&alloc, {mshape2, 2});
        const double t = best_of(
            [&]
            {
                if (mttkrp<crd::f64>(csft, {ft.data(), 3U}, mo.view(), &alloc, 1U) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   mttkrp-mode%u      : %9.2f ms   checksum %.17g\n", target, t,
                    checksum(mo.data(), t1.shape(target) * rr));
    }

    // ---- TTM mode-1 and mode-2 ----
    const crd::u64 u1shape[2] = {dj, fu};
    const TensorView<const crd::f64> u1 = TensorView<const crd::f64>::contiguous(u1p, {u1shape, 2});
    const crd::u64 y1shape[3] = {di, fu, dk};
    Tensor<crd::f64> y1(&alloc, {y1shape, 3});
    {
        const double t = best_of(
            [&]
            {
                if (contract_mode<crd::f64>(t1, 1U, u1, y1.view(), &alloc, 1U) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   ttm-mode1         : %9.2f ms   checksum %.17g\n", t, checksum(y1.data(), di * fu * dk));
    }
    const crd::u64 u2shape[2] = {dk, fu2};
    const TensorView<const crd::f64> u2 = TensorView<const crd::f64>::contiguous(u2p, {u2shape, 2});
    const crd::u64 y2shape[3] = {di, dj, fu2};
    Tensor<crd::f64> y2(&alloc, {y2shape, 3});
    {
        const double t = best_of(
            [&]
            {
                if (contract_mode<crd::f64>(t1, 2U, u2, y2.view(), &alloc, 1U) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   ttm-mode2         : %9.2f ms   checksum %.17g\n", t, checksum(y2.data(), di * dj * fu2));
    }

    // ---- elementwise ----
    {
        double cks = 0.0;
        const double t = best_of(
            [&]
            {
                SparseCoo<crd::f64> s(&alloc);
                if (sparse_add(t1, t2, s) != TensorStatus::Ok)
                {
                    std::abort();
                }
                cks = checksum(s.val(), s.nnz());
            });
        std::printf("ours   sparse add        : %9.2f ms   checksum %.17g\n", t, cks);
    }
    {
        double cks = 0.0;
        const double t = best_of(
            [&]
            {
                SparseCoo<crd::f64> p(&alloc);
                if (sparse_mul(t1, t2, p) != TensorStatus::Ok)
                {
                    std::abort();
                }
                cks = checksum(p.val(), p.nnz());
            });
        std::printf("ours   sparse mul        : %9.2f ms   checksum %.17g\n", t, cks);
    }

    // ---- reductions ----
    {
        crd::f64 s = 0.0;
        const double t = best_of(
            [&]
            {
                if (reduce_sum(t1, s) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   reduce sum-total  : %9.2f ms   checksum %.17g\n", t, s);
    }
    {
        crd::containers::Array<crd::f64> row(&alloc);
        row.resize(static_cast<crd::usize>(di));
        // board row: CSF root reduction (the format-matched comparison —
        // scipy's csr row pointers are precomputed exactly like the CSF fptr)
        const double t = best_of(
            [&]
            {
                if (reduce_sum_root(csf, {row.data(), row.size()}) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   reduce sum-mode0  : %9.2f ms   checksum %.17g   (csf)\n", t,
                    checksum(row.data(), di));
        const double tc = best_of(
            [&]
            {
                if (reduce_sum_mode(t1, 0U, {row.data(), row.size()}) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("(info) reduce sum-mode0  : %9.2f ms   checksum %.17g   (coo, run-detect)\n", tc,
                    checksum(row.data(), di));
    }
    {
        crd::containers::Array<crd::f64> row(&alloc);
        row.resize(static_cast<crd::usize>(di));
        const double t = best_of(
            [&]
            {
                if (reduce_max_root(csf, {row.data(), row.size()}) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("ours   reduce max-mode0  : %9.2f ms   checksum %.17g   (csf)\n", t,
                    checksum(row.data(), di));
        const double tc = best_of(
            [&]
            {
                if (reduce_max_mode(t1, 0U, {row.data(), row.size()}, &alloc) != TensorStatus::Ok)
                {
                    std::abort();
                }
            });
        std::printf("(info) reduce max-mode0  : %9.2f ms   checksum %.17g   (coo, run-detect)\n", tc,
                    checksum(row.data(), di));
    }
    return 0;
}
