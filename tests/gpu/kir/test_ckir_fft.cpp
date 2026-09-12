// test_ckir_fft.cpp -- B-cmp Phase 1: the CKIR radix-2 Stockham FFT (ckir_fft.hpp) on the CPU oracle. Proves the authored
// FFT KGraph computes a correct DFT (vs a direct f64 DFT within f32 tolerance) and that the ping-pong / Stockham index map
// is right at several sizes. GPU bit-exactness (Vulkan/DX12/CUDA, `precise` temps) + radix-8 + batched are the next slices.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_fft.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>

#include <crd/math/cmath.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
constexpr crd::f64 kTwoPi = 6.28318530717958647693;

crd::f64 fabs64(crd::f64 x) { return x < 0.0 ? -x : x; }

// direct DFT reference in f64: X[k] = sum_n x[n] * (cos(2pi kn/N), -sin(2pi kn/N)).
void dft_ref(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* refr, crd::f64* refi)
{
    for (int k = 0; k < n; ++k)
    {
        crd::f64 ar = 0.0;
        crd::f64 ai = 0.0;
        for (int m = 0; m < n; ++m)
        {
            const crd::f64 ang = kTwoPi * static_cast<crd::f64>(k) * static_cast<crd::f64>(m) / static_cast<crd::f64>(n);
            const crd::f64 c   = crd::math::cos(ang);
            const crd::f64 s   = crd::math::sin(ang);
            ar += xr[m] * c + xi[m] * s;
            ai += xi[m] * c - xr[m] * s;
        }
        refr[k] = ar;
        refi[k] = ai;
    }
}

// Run the CKIR radix-2 FFT of size n over the oracle. xr/xi in, outr/outi out (length n).
void run_fft(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix2(g, n, false);

    // twiddles W_N^k = (cos(2pi k/N), -sin(2pi k/N)), k = 0..N/2-1, f32-rounded (uploaded as f32).
    const int half = n / 2;
    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(half));
    twi.resize(static_cast<crd::usize>(half));
    for (int k = 0; k < half; ++k)
    {
        const crd::f64 ang = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    crd::containers::Array<crd::f64> ii(&alloc);
    ir.resize(static_cast<crd::usize>(n));
    ii.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xr[i]));
        ii[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xi[i]));
        outr[i] = -99.0;
        outi[i] = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir.data(), n, 0, 0},  {ii.data(), n, 0, 1},  {twr.data(), half, 0, 2},
                                 {twi.data(), half, 0, 3}, {outr, n, 0, 4}, {outi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(half), &alloc);
}

// Run the CKIR radix-4 FFT of size n (power of 4) over the oracle. Uses the FULL W_N[N] twiddle table.
void run_fft4(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix4(g, n, false);

    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(n));
    twi.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    crd::containers::Array<crd::f64> ii(&alloc);
    ir.resize(static_cast<crd::usize>(n));
    ii.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xr[i]));
        ii[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xi[i]));
        outr[i]                        = -99.0;
        outi[i]                        = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir.data(), n, 0, 0},  {ii.data(), n, 0, 1},  {twr.data(), n, 0, 2},
                                 {twi.data(), n, 0, 3}, {outr, n, 0, 4}, {outi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(n / 4), &alloc);
}

// Run the FUSED FFT-convolution of x with filter h (h's spectrum = DFT(h)) over the oracle → circular conv(x,h).
void run_conv(int n, const crd::f64* xr, const crd::f64* xi, const crd::f64* hr, const crd::f64* hi, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_convolution(g, n);

    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    crd::containers::Array<crd::f64> twr(&alloc);  crd::containers::Array<crd::f64> twi(&alloc);
    crd::containers::Array<crd::f64> ir(&alloc);   crd::containers::Array<crd::f64> ii(&alloc);
    crd::containers::Array<crd::f64> fr(&alloc);   crd::containers::Array<crd::f64> fi(&alloc);
    const crd::usize un = static_cast<crd::usize>(n);
    twr.resize(un); twi.resize(un); ir.resize(un); ii.resize(un); fr.resize(un); fi.resize(un);
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = f32(crd::math::cos(ang));
        twi[static_cast<crd::usize>(k)] = f32(-crd::math::sin(ang));
        ir[static_cast<crd::usize>(k)]  = f32(xr[k]);
        ii[static_cast<crd::usize>(k)]  = f32(xi[k]);
    }
    // filter spectrum = DFT(h) (f32-rounded, as uploaded).
    crd::f64 hf_r[256];
    crd::f64 hf_i[256];
    dft_ref(n, hr, hi, hf_r, hf_i);
    for (int k = 0; k < n; ++k) { fr[static_cast<crd::usize>(k)] = f32(hf_r[k]); fi[static_cast<crd::usize>(k)] = f32(hf_i[k]); }

    kir::KernelBuffer bufs[8] = {{ir.data(), n, 0, 0},  {ii.data(), n, 0, 1},  {twr.data(), n, 0, 2}, {twi.data(), n, 0, 3},
                                 {fr.data(), n, 0, 4}, {fi.data(), n, 0, 5}, {outr, n, 0, 6}, {outi, n, 0, 7}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 8, static_cast<crd::u32>(n / 4), &alloc);
}

// Run the CKIR radix-8 FFT of size n (power of 8) over the oracle. Full W_N[N] twiddle table.
void run_fft8(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix8(g, n, false);

    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(n));
    twi.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    crd::containers::Array<crd::f64> ii(&alloc);
    ir.resize(static_cast<crd::usize>(n));
    ii.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xr[i]));
        ii[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xi[i]));
        outr[i]                        = -99.0;
        outi[i]                        = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir.data(), n, 0, 0},  {ii.data(), n, 0, 1},  {twr.data(), n, 0, 2},
                                 {twi.data(), n, 0, 3}, {outr, n, 0, 4}, {outi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(n / 8), &alloc);
}

// Run the REGISTER-BLOCKED radix-16 FFT (n = 4^p) over the oracle. Full W_N[N] table; n/16 threads.
void run_fft16(int n, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix16(g, n, false);

    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(n));
    twi.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    crd::containers::Array<crd::f64> ii(&alloc);
    ir.resize(static_cast<crd::usize>(n));
    ii.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i)
    {
        ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xr[i]));
        ii[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xi[i]));
        outr[i]                        = -99.0;
        outi[i]                        = -99.0;
    }
    kir::KernelBuffer bufs[6] = {{ir.data(), n, 0, 0},  {ii.data(), n, 0, 1},  {twr.data(), n, 0, 2},
                                 {twi.data(), n, 0, 3}, {outr, n, 0, 4}, {outi, n, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, plan.entry.local_size[0], &alloc);
}

// R2C: real input `xr` (n) → half spectrum `outr`/`outi` (n/2+1, padded to `hs`). Grid = 1 (single row).
void run_r2c(int n, int hs, const crd::f64* xr, crd::f64* outr, crd::f64* outi)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_r2c(g, n, hs);
    crd::containers::Array<crd::f64> twr(&alloc); crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(n)); twi.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    ir.resize(static_cast<crd::usize>(n));
    for (int i = 0; i < n; ++i) { ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(xr[i])); }
    for (int i = 0; i < hs; ++i) { outr[i] = -99.0; outi[i] = -99.0; }
    kir::KernelBuffer bufs[5] = {{ir.data(), n, 0, 0}, {twr.data(), n, 0, 1}, {twi.data(), n, 0, 2}, {outr, hs, 0, 3}, {outi, hs, 0, 4}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 5, plan.entry.local_size[0], &alloc);
}

// C2R: half spectrum `inr`/`ini` (hs) → real output `outr` (n). Grid = 1.
void run_c2r(int n, int hs, const crd::f64* inr, const crd::f64* ini, crd::f64* outr)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_c2r(g, n, hs);
    crd::containers::Array<crd::f64> twr(&alloc); crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(n)); twi.resize(static_cast<crd::usize>(n));
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc); crd::containers::Array<crd::f64> ii(&alloc);
    ir.resize(static_cast<crd::usize>(hs)); ii.resize(static_cast<crd::usize>(hs));
    for (int i = 0; i < hs; ++i) { ir[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(inr[i])); ii[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>(ini[i])); }
    for (int i = 0; i < n; ++i) { outr[i] = -99.0; }
    kir::KernelBuffer bufs[5] = {{ir.data(), hs, 0, 0}, {ii.data(), hs, 0, 1}, {twr.data(), n, 0, 2}, {twi.data(), n, 0, 3}, {outr, n, 0, 4}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 5, plan.entry.local_size[0], &alloc);
}

// Separable 2-D DFT reference (exact): DFT every row (length cols), then DFT every column (length rows). Row-major.
void dft2_ref(int rows, int cols, const crd::f64* xr, const crd::f64* xi, crd::f64* refr, crd::f64* refi,
              crd::memory::IAllocator* alloc)
{
    const crd::usize                 rc = static_cast<crd::usize>(rows) * static_cast<crd::usize>(cols);
    crd::containers::Array<crd::f64> tr(alloc);
    crd::containers::Array<crd::f64> ti(alloc);
    tr.resize(rc);
    ti.resize(rc);
    for (int r = 0; r < rows; ++r) // row DFTs
    {
        dft_ref(cols, xr + r * cols, xi + r * cols, tr.data() + r * cols, ti.data() + r * cols);
    }
    crd::f64 cr[2048];
    crd::f64 ci[2048];
    crd::f64 orr[2048];
    crd::f64 oi[2048];
    for (int c = 0; c < cols; ++c) // column DFTs (gather stride cols → dft → scatter)
    {
        for (int r = 0; r < rows; ++r) { cr[r] = tr.data()[r * cols + c]; ci[r] = ti.data()[r * cols + c]; }
        dft_ref(rows, cr, ci, orr, oi);
        for (int r = 0; r < rows; ++r) { refr[r * cols + c] = orr[r]; refi[r * cols + c] = oi[r]; }
    }
}

// Run the CKIR tiled TRANSPOSE (rows×cols → cols×rows) over the oracle. in length rows*cols, out length cols*rows.
void run_transpose2d(int rows, int cols, int tile, const crd::f64* in, crd::f64* out)
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::KEntry          e = kir::build_transpose2d(g, rows, cols, tile);

    const crd::usize rc = static_cast<crd::usize>(rows) * static_cast<crd::usize>(cols);
    crd::containers::Array<crd::f64> ib(&alloc);
    ib.resize(rc);
    for (crd::usize i = 0; i < rc; ++i) { ib[i] = static_cast<crd::f64>(static_cast<float>(in[i])); out[i] = -1.0; }
    kir::KernelBuffer bufs[2] = {{ib.data(), rows * cols, 0, 0}, {out, cols * rows, 0, 1}};
    const crd::u32    grid    = static_cast<crd::u32>((rows / tile) * (cols / tile));
    kir::eval_cpu_kernel(g, e, bufs, 2, e.local_size[0], &alloc, grid);
}

// Drive the full 2-D FFT plan (6 dispatches, ping-ponged host buffers) over the oracle. imgr/imgi in (rows*cols, row-major),
// outr/outi out. `tile` must divide rows and cols. Mirrors exactly what the GPU harness does (dispatch each pass in order).
void run_fft2d(int rows, int cols, const crd::f64* imgr, const crd::f64* imgi, crd::f64* outr, crd::f64* outi, int tile)
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::KGraph                g0(&alloc); // one graph per unique entry: row FFT, R×C transpose, col FFT, C×R transpose
    kir::KGraph                g1(&alloc);
    kir::KGraph                g2(&alloc);
    kir::KGraph                g3(&alloc);
    kir::KGraph*               graphs[4] = {&g0, &g1, &g2, &g3};
    const kir::Fft2dPlan       plan      = kir::build_fft2d_c2c(graphs, rows, cols, false, tile);

    int off[16];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rows * cols; ++i) { buf(plan.in_re)[i] = f32(imgr[i]); buf(plan.in_im)[i] = f32(imgi[i]); }
    for (int k = 0; k < cols; ++k) // cols-point twiddles (row FFT)
    {
        const crd::f64 a = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cols);
        buf(plan.tw_col_re)[k] = f32(crd::math::cos(a));
        buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a));
    }
    for (int k = 0; k < rows; ++k) // rows-point twiddles (column FFT)
    {
        const crd::f64 a = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rows);
        buf(plan.tw_row_re)[k] = f32(crd::math::cos(a));
        buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a));
    }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k)
        {
            kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)};
        }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }
    for (int i = 0; i < rows * cols; ++i) { outr[i] = buf(plan.res_re)[i]; outi[i] = buf(plan.res_im)[i]; }
}

// Direct 2-D CIRCULAR convolution reference: conv[a][b] = sum_{p,q} x[p][q] * h[(a-p) mod R][(b-q) mod C] (complex).
void conv2d_ref(int rows, int cols, const crd::f64* xr, const crd::f64* xi, const crd::f64* hr, const crd::f64* hi,
                crd::f64* cr, crd::f64* ci)
{
    for (int a = 0; a < rows; ++a)
    {
        for (int b = 0; b < cols; ++b)
        {
            crd::f64 sr = 0.0;
            crd::f64 si = 0.0;
            for (int p = 0; p < rows; ++p)
            {
                for (int q = 0; q < cols; ++q)
                {
                    const int      ra  = (((a - p) % rows) + rows) % rows;
                    const int      rb  = (((b - q) % cols) + cols) % cols;
                    const crd::f64 xrv = xr[p * cols + q];
                    const crd::f64 xiv = xi[p * cols + q];
                    const crd::f64 hrv = hr[ra * cols + rb];
                    const crd::f64 hiv = hi[ra * cols + rb];
                    sr += xrv * hrv - xiv * hiv;
                    si += xrv * hiv + xiv * hrv;
                }
            }
            cr[a * cols + b] = sr;
            ci[a * cols + b] = si;
        }
    }
}

// Direct UNNORMALISED inverse 2-D DFT: out[a][b] = sum_{kr,kc} X[kr][kc] * e^{+i 2pi (a*kr/rows + b*kc/cols)}.
void idft2_ref(int rows, int cols, const crd::f64* xr, const crd::f64* xi, crd::f64* outr, crd::f64* outi)
{
    for (int a = 0; a < rows; ++a)
    {
        for (int b = 0; b < cols; ++b)
        {
            crd::f64 sr = 0.0;
            crd::f64 si = 0.0;
            for (int kr = 0; kr < rows; ++kr)
            {
                for (int kc = 0; kc < cols; ++kc)
                {
                    const crd::f64 ang = kTwoPi * (static_cast<crd::f64>(a) * static_cast<crd::f64>(kr) / static_cast<crd::f64>(rows)
                                                   + static_cast<crd::f64>(b) * static_cast<crd::f64>(kc) / static_cast<crd::f64>(cols));
                    const crd::f64 c   = crd::math::cos(ang);
                    const crd::f64 s   = crd::math::sin(ang);
                    const crd::f64 vr  = xr[kr * cols + kc];
                    const crd::f64 vi  = xi[kr * cols + kc];
                    sr += vr * c - vi * s;
                    si += vr * s + vi * c;
                }
            }
            outr[a * cols + b] = sr;
            outi[a * cols + b] = si;
        }
    }
}

// Drive the FUSED 2-D FFT-convolution plan (7 dispatches) over the oracle: y = IFFT2(FFT2(x) ⊙ FFT2(h)). The filter's 2-D
// spectrum H = FFT2(h) (via dft2_ref) is uploaded in TRANSPOSED layout (filt[v*rows+u] = H[u][v]) as the plan requires.
void run_fft2d_conv(int rows, int cols, const crd::f64* xr, const crd::f64* xi, const crd::f64* hr, const crd::f64* hi,
                    crd::f64* outr, crd::f64* outi, int tile)
{
    crd::memory::TlsfAllocator alloc(256U << 20U);
    kir::KGraph                g0(&alloc); // row FFT, R×C transpose, fused column conv, C×R transpose, inverse row FFT
    kir::KGraph                g1(&alloc);
    kir::KGraph                g2(&alloc);
    kir::KGraph                g3(&alloc);
    kir::KGraph                g4(&alloc);
    kir::KGraph*               graphs[5] = {&g0, &g1, &g2, &g3, &g4};
    const kir::Fft2dPlan       plan      = kir::build_fft2d_convolution(graphs, rows, cols, tile);

    int off[20];
    int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rows * cols; ++i) { buf(plan.in_re)[i] = f32(xr[i]); buf(plan.in_im)[i] = f32(xi[i]); }
    for (int k = 0; k < cols; ++k)
    {
        const crd::f64 a       = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(cols);
        buf(plan.tw_col_re)[k] = f32(crd::math::cos(a));
        buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a));
    }
    for (int k = 0; k < rows; ++k)
    {
        const crd::f64 a       = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(rows);
        buf(plan.tw_row_re)[k] = f32(crd::math::cos(a));
        buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a));
    }
    // filter spectrum H = FFT2(h), uploaded TRANSPOSED: filt[v*rows + u] = H[u][v].
    crd::containers::Array<crd::f64> hspecr(&alloc);
    crd::containers::Array<crd::f64> hspeci(&alloc);
    hspecr.resize(static_cast<crd::usize>(rows) * static_cast<crd::usize>(cols));
    hspeci.resize(static_cast<crd::usize>(rows) * static_cast<crd::usize>(cols));
    dft2_ref(rows, cols, hr, hi, hspecr.data(), hspeci.data(), &alloc);
    for (int u = 0; u < rows; ++u)
    {
        for (int v = 0; v < cols; ++v)
        {
            buf(plan.filt_re)[v * rows + u] = f32(hspecr.data()[u * cols + v]);
            buf(plan.filt_im)[v * rows + u] = f32(hspeci.data()[u * cols + v]);
        }
    }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k)
        {
            kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)};
        }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }
    for (int i = 0; i < rows * cols; ++i) { outr[i] = buf(plan.res_re)[i]; outi[i] = buf(plan.res_im)[i]; }
}
} // namespace

TEST_CASE("B-cmp: CKIR radix-2 Stockham FFT matches a direct DFT (CPU oracle, f32 tol)", "[kir][kernel][fft]")
{
    for (int n : {2, 4, 8, 16, 32, 64})
    {
        crd::f64 xr[64];
        crd::f64 xi[64];
        crd::f64 outr[64];
        crd::f64 outi[64];
        crd::f64 refr[64];
        crd::f64 refi[64];
        for (int i = 0; i < n; ++i) { xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0; }
        dft_ref(n, xr, xi, refr, refi);
        run_fft(n, xr, xi, outr, outi);

        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < n; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
        int bad = 0;
        for (int k = 0; k < n; ++k)
        {
            if (fabs64(outr[k] - refr[k]) > 3e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - refi[k]) > 3e-3 * maxmag) { ++bad; }
        }
        INFO("n = " << n);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR radix-4 Stockham FFT matches a direct DFT (CPU oracle, f32 tol)", "[kir][kernel][fft]")
{
    for (int n : {16, 64, 256, 1024})
    {
        crd::f64 xr[1024];
        crd::f64 xi[1024];
        crd::f64 outr[1024];
        crd::f64 outi[1024];
        crd::f64 refr[1024];
        crd::f64 refi[1024];
        for (int i = 0; i < n; ++i) { xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0; }
        dft_ref(n, xr, xi, refr, refi);
        run_fft4(n, xr, xi, outr, outi);

        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < n; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
        int bad = 0;
        for (int k = 0; k < n; ++k)
        {
            if (fabs64(outr[k] - refr[k]) > 3e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - refi[k]) > 3e-3 * maxmag) { ++bad; }
        }
        INFO("radix-4 n = " << n);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR FUSED FFT-convolution == circular convolution (fwd->x-spectrum->inv, one kernel)", "[kir][kernel][fft][conv]")
{
    for (int n : {16, 64, 256})
    {
        crd::f64 xr[256]; crd::f64 xi[256]; crd::f64 hr[256]; crd::f64 hi[256]; crd::f64 outr[256]; crd::f64 outi[256];
        for (int i = 0; i < n; ++i)
        {
            xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = 0.0;
            hr[i] = (i < 5) ? (1.0 / static_cast<crd::f64>(i + 1)) : 0.0; hi[i] = 0.0; // a small smoothing kernel
        }
        run_conv(n, xr, xi, hr, hi, outr, outi);

        // reference: circular convolution y[k] = sum_m x[m] h[(k-m) mod n].
        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < n; ++k)
        {
            crd::f64 cr = 0.0;
            for (int m = 0; m < n; ++m) { cr += xr[m] * hr[((k - m) % n + n) % n]; }
            maxmag = maxmag > fabs64(cr) ? maxmag : fabs64(cr);
        }
        int bad = 0;
        for (int k = 0; k < n; ++k)
        {
            crd::f64 cr = 0.0;
            crd::f64 ci = 0.0;
            for (int m = 0; m < n; ++m)
            {
                const int mm = ((k - m) % n + n) % n;
                cr += xr[m] * hr[mm] - xi[m] * hi[mm];
                ci += xr[m] * hi[mm] + xi[m] * hr[mm];
            }
            if (fabs64(outr[k] - cr) > 5e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - ci) > 5e-3 * maxmag) { ++bad; }
        }
        INFO("conv n = " << n);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR radix-8 Stockham FFT matches a direct DFT (CPU oracle, f32 tol)", "[kir][kernel][fft]")
{
    for (int n : {8, 64, 512})
    {
        crd::f64 xr[512];
        crd::f64 xi[512];
        crd::f64 outr[512];
        crd::f64 outi[512];
        crd::f64 refr[512];
        crd::f64 refi[512];
        for (int i = 0; i < n; ++i) { xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0; }
        dft_ref(n, xr, xi, refr, refi);
        run_fft8(n, xr, xi, outr, outi);

        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < n; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
        int bad = 0;
        for (int k = 0; k < n; ++k)
        {
            if (fabs64(outr[k] - refr[k]) > 3e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - refi[k]) > 3e-3 * maxmag) { ++bad; }
        }
        INFO("radix-8 n = " << n);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR radix-4 BATCHED FFT -- each workgroup transforms its own slice (WorkgroupIndex offset)", "[kir][kernel][fft]")
{
    constexpr int              nn = 64;
    constexpr int              nb = 5; // 5 independent FFTs, one per workgroup
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_radix4(g, nn, false, true); // batched

    crd::containers::Array<crd::f64> twr(&alloc);
    crd::containers::Array<crd::f64> twi(&alloc);
    twr.resize(static_cast<crd::usize>(nn));
    twi.resize(static_cast<crd::usize>(nn));
    for (int k = 0; k < nn; ++k)
    {
        const crd::f64 ang              = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(nn);
        twr[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(crd::math::cos(ang)));
        twi[static_cast<crd::usize>(k)] = static_cast<crd::f64>(static_cast<float>(-crd::math::sin(ang)));
    }
    crd::containers::Array<crd::f64> ir(&alloc);
    crd::containers::Array<crd::f64> ii(&alloc);
    crd::containers::Array<crd::f64> orr(&alloc);
    crd::containers::Array<crd::f64> oi(&alloc);
    const crd::usize total = static_cast<crd::usize>(nb) * static_cast<crd::usize>(nn);
    ir.resize(total);
    ii.resize(total);
    orr.resize(total);
    oi.resize(total);
    for (int b = 0; b < nb; ++b)
    {
        const crd::usize row = static_cast<crd::usize>(b) * static_cast<crd::usize>(nn);
        for (int i = 0; i < nn; ++i)
        {
            ir[row + static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>((i * 7 + b * 3) % 11 - 5));
            ii[row + static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<float>((i * 5 + b) % 7 - 3));
            orr[row + static_cast<crd::usize>(i)] = -99.0;
            oi[row + static_cast<crd::usize>(i)]  = -99.0;
        }
    }
    kir::KernelBuffer bufs[6] = {{ir.data(), nb * nn, 0, 0},  {ii.data(), nb * nn, 0, 1}, {twr.data(), nn, 0, 2},
                                 {twi.data(), nn, 0, 3}, {orr.data(), nb * nn, 0, 4}, {oi.data(), nb * nn, 0, 5}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 6, static_cast<crd::u32>(nn / 4), &alloc, static_cast<crd::u32>(nb));

    // each slice must BIT-EXACTLY equal the single-workgroup FFT of that slice (same oracle, same data).
    int bad = 0;
    for (int b = 0; b < nb; ++b)
    {
        const crd::usize row = static_cast<crd::usize>(b) * static_cast<crd::usize>(nn);
        crd::f64         refr[nn];
        crd::f64         refi[nn];
        run_fft4(nn, &ir[row], &ii[row], refr, refi);
        for (int i = 0; i < nn; ++i)
        {
            if (orr[row + static_cast<crd::usize>(i)] != refr[i] || oi[row + static_cast<crd::usize>(i)] != refi[i]) { ++bad; }
        }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR radix-2 FFT of a unit impulse is all-ones (bit-exact: twiddles hit only zeros)", "[kir][kernel][fft]")
{
    constexpr int n = 16;
    crd::f64      xr[n];
    crd::f64      xi[n];
    crd::f64      outr[n];
    crd::f64      outi[n];
    for (int i = 0; i < n; ++i) { xr[i] = 0.0; xi[i] = 0.0; }
    xr[0] = 1.0; // delta at 0  ->  X[k] = 1 for all k
    run_fft(n, xr, xi, outr, outi);

    int bad = 0;
    for (int k = 0; k < n; ++k) { if (outr[k] != 1.0 || outi[k] != 0.0) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR radix-2 FFT of a constant is a scaled impulse (DC = N)", "[kir][kernel][fft]")
{
    constexpr int n = 8;
    crd::f64      xr[n];
    crd::f64      xi[n];
    crd::f64      outr[n];
    crd::f64      outi[n];
    for (int i = 0; i < n; ++i) { xr[i] = 1.0; xi[i] = 0.0; }
    run_fft(n, xr, xi, outr, outi); // X[0] = N, X[k>0] ~ 0

    CHECK(outr[0] == static_cast<crd::f64>(n)); // DC bin is an exact integer sum
    CHECK(outi[0] == 0.0);
}

TEST_CASE("B-cmp Phase 2: CKIR tiled 2-D transpose -- out[c,r] = in[r,c] (CPU oracle, bit-exact)", "[kir][kernel][fft]")
{
    // square 8x8 and rectangular 8x16 (tile 4): the 1-D-workgroup -> 2-D-tile Div/Mod map + the barrier-gated cross-thread read.
    constexpr int cases[2][2] = {{8, 8}, {8, 16}};
    for (int ci = 0; ci < 2; ++ci)
    {
        const int rr = cases[ci][0];
        const int cc = cases[ci][1];
        crd::f64  in[8 * 16];
        crd::f64  out[8 * 16];
        for (int i = 0; i < rr * cc; ++i) { in[i] = static_cast<crd::f64>((i * 7 + 1) % 251 - 120); } // integer -> f32-exact
        run_transpose2d(rr, cc, 4, in, out);
        int bad = 0;
        for (int r = 0; r < rr; ++r) { for (int c = 0; c < cc; ++c) { if (out[c * rr + r] != in[r * cc + c]) { ++bad; } } }
        INFO("transpose " << rr << "x" << cc);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp Phase 2: CKIR 2-D FFT (row->transpose->col->transpose) matches a separable 2-D DFT (f32 tol)", "[kir][kernel][fft]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U);
    constexpr int              cases[2][2] = {{16, 16}, {16, 64}}; // square + rectangular (both power-of-4 dims -> radix-4)
    for (int ci = 0; ci < 2; ++ci)
    {
        const int rr = cases[ci][0];
        const int cc = cases[ci][1];
        const int rc = rr * cc;
        crd::f64  xr[1024]; crd::f64 xi[1024]; crd::f64 outr[1024]; crd::f64 outi[1024]; crd::f64 refr[1024]; crd::f64 refi[1024];
        for (int i = 0; i < rc; ++i) { xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0; }
        dft2_ref(rr, cc, xr, xi, refr, refi, &alloc);
        run_fft2d(rr, cc, xr, xi, outr, outi, 4);

        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
        int bad = 0;
        for (int k = 0; k < rc; ++k)
        {
            if (fabs64(outr[k] - refr[k]) > 5e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - refi[k]) > 5e-3 * maxmag) { ++bad; }
        }
        INFO("2-D FFT " << rr << "x" << cc);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp Phase 2: CKIR 2-D FFT of a unit impulse is all-ones (bit-exact: twiddles hit only zeros)", "[kir][kernel][fft]")
{
    constexpr int rr = 16;
    constexpr int cc = 16;
    crd::f64      xr[rr * cc]; crd::f64 xi[rr * cc]; crd::f64 outr[rr * cc]; crd::f64 outi[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { xr[i] = 0.0; xi[i] = 0.0; }
    xr[0] = 1.0; // impulse at (0,0)
    run_fft2d(rr, cc, xr, xi, outr, outi, 4);
    int bad = 0;
    for (int i = 0; i < rr * cc; ++i) { if (outr[i] != 1.0 || outi[i] != 0.0) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp Phase 2: CKIR 2-D FFT of a constant has DC = rows*cols (bit-exact integer add-tree)", "[kir][kernel][fft]")
{
    constexpr int rr = 16;
    constexpr int cc = 16;
    crd::f64      xr[rr * cc]; crd::f64 xi[rr * cc]; crd::f64 outr[rr * cc]; crd::f64 outi[rr * cc];
    for (int i = 0; i < rr * cc; ++i) { xr[i] = 1.0; xi[i] = 0.0; }
    run_fft2d(rr, cc, xr, xi, outr, outi, 4);
    CHECK(outr[0] == static_cast<crd::f64>(rr * cc)); // DC = exact integer sum (jidx=0 path is W^0 twiddle-free)
    CHECK(outi[0] == 0.0);
}

TEST_CASE("B-cmp crush: REGISTER-BLOCKED radix-16 Stockham FFT matches a direct DFT (CPU oracle, f32 tol)", "[kir][kernel][fft]")
{
    for (int n : {64, 256, 1024}) // [16,4] / [16,16] / [16,16,4] stage mixes
    {
        crd::f64 xr[1024];
        crd::f64 xi[1024];
        crd::f64 outr[1024];
        crd::f64 outi[1024];
        crd::f64 refr[1024];
        crd::f64 refi[1024];
        for (int i = 0; i < n; ++i) { xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0; }
        dft_ref(n, xr, xi, refr, refi);
        run_fft16(n, xr, xi, outr, outi);

        crd::f64 maxmag = 1e-6;
        for (int k = 0; k < n; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
        int bad = 0;
        for (int k = 0; k < n; ++k)
        {
            if (fabs64(outr[k] - refr[k]) > 3e-3 * maxmag) { ++bad; }
            if (fabs64(outi[k] - refi[k]) > 3e-3 * maxmag) { ++bad; }
        }
        INFO("radix-16 n = " << n);
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp crush: radix-16 FFT of a unit impulse is all-ones (bit-exact: twiddles hit only zeros)", "[kir][kernel][fft]")
{
    constexpr int n = 256;
    crd::f64      xr[n]; crd::f64 xi[n]; crd::f64 outr[n]; crd::f64 outi[n];
    for (int i = 0; i < n; ++i) { xr[i] = 0.0; xi[i] = 0.0; }
    xr[0] = 1.0;
    run_fft16(n, xr, xi, outr, outi);
    int bad = 0;
    for (int i = 0; i < n; ++i) { if (outr[i] != 1.0 || outi[i] != 0.0) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp crush: radix-16 FUSED FFT-convolution == circular convolution (CPU oracle, f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int n = 256;
    crd::f64      xr[n]; crd::f64 xi[n]; crd::f64 hr[n]; crd::f64 hi[n]; crd::f64 outr[n]; crd::f64 outi[n];
    for (int i = 0; i < n; ++i)
    {
        xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0;
        xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0;
        hr[i] = (i < 4) ? 1.0 : 0.0; // a small box filter
        hi[i] = 0.0;
    }
    // reference: direct circular convolution in f64.
    crd::f64 refr[n]; crd::f64 refi[n];
    for (int a = 0; a < n; ++a)
    {
        crd::f64 sr = 0.0; crd::f64 si = 0.0;
        for (int p = 0; p < n; ++p)
        {
            const int q = ((a - p) % n + n) % n;
            sr += xr[p] * hr[q] - xi[p] * hi[q];
            si += xr[p] * hi[q] + xi[p] * hr[q];
        }
        refr[a] = sr; refi[a] = si;
    }
    // drive the radix-16 fused conv over the oracle (filter spectrum = DFT(h), f32-rounded).
    crd::memory::TlsfAllocator alloc(64U << 20U);
    kir::KGraph                g(&alloc);
    const kir::Fft1dPlan       plan = kir::build_fft1d_convolution16(g, n);
    const auto                 f32  = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };
    crd::f64                   twr[n]; crd::f64 twi[n]; crd::f64 ir[n]; crd::f64 ii[n]; crd::f64 fr[n]; crd::f64 fi[n];
    crd::f64                   hfr[n]; crd::f64 hfi[n];
    dft_ref(n, hr, hi, hfr, hfi);
    for (int k = 0; k < n; ++k)
    {
        const crd::f64 ang = kTwoPi * static_cast<crd::f64>(k) / static_cast<crd::f64>(n);
        twr[k] = f32(crd::math::cos(ang));
        twi[k] = f32(-crd::math::sin(ang));
        ir[k]  = f32(xr[k]);
        ii[k]  = f32(xi[k]);
        fr[k]  = f32(hfr[k]);
        fi[k]  = f32(hfi[k]);
        outr[k] = -99.0; outi[k] = -99.0;
    }
    kir::KernelBuffer bufs[8] = {{ir, n, 0, 0}, {ii, n, 0, 1}, {twr, n, 0, 2}, {twi, n, 0, 3},
                                 {fr, n, 0, 4}, {fi, n, 0, 5}, {outr, n, 0, 6}, {outi, n, 0, 7}};
    kir::eval_cpu_kernel(g, plan.entry, bufs, 8, plan.entry.local_size[0], &alloc);

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < n; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
    int bad = 0;
    for (int k = 0; k < n; ++k)
    {
        if (fabs64(outr[k] - refr[k]) > 5e-3 * maxmag) { ++bad; }
        if (fabs64(outi[k] - refi[k]) > 5e-3 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp Phase 3: CKIR FUSED 2-D FFT-convolution == direct 2-D circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr = 16;
    constexpr int cc = 16;
    constexpr int rc = rr * cc;
    crd::f64      xr[rc]; crd::f64 xi[rc]; crd::f64 hr[rc]; crd::f64 hi[rc];
    crd::f64      outr[rc]; crd::f64 outi[rc]; crd::f64 refr[rc]; crd::f64 refi[rc];
    for (int i = 0; i < rc; ++i)
    {
        xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0;
        xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0;
        hr[i] = static_cast<crd::f64>((i * 3 + 2) % 5) - 2.0; // a small compact-ish filter
        hi[i] = static_cast<crd::f64>((i * 2 + 1) % 3) - 1.0;
    }
    conv2d_ref(rr, cc, xr, xi, hr, hi, refr, refi);
    run_fft2d_conv(rr, cc, xr, xi, hr, hi, outr, outi, 4);

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
    int bad = 0;
    for (int k = 0; k < rc; ++k)
    {
        if (fabs64(outr[k] - refr[k]) > 1e-2 * maxmag) { ++bad; }
        if (fabs64(outi[k] - refi[k]) > 1e-2 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp Phase 3: CKIR FUSED 2-D FFT-convolution with an impulse filter recovers the input (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr = 16;
    constexpr int cc = 16;
    constexpr int rc = rr * cc;
    crd::f64      xr[rc]; crd::f64 xi[rc]; crd::f64 hr[rc]; crd::f64 hi[rc]; crd::f64 outr[rc]; crd::f64 outi[rc];
    for (int i = 0; i < rc; ++i)
    {
        xr[i] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0;
        xi[i] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0;
        hr[i] = 0.0;
        hi[i] = 0.0;
    }
    hr[0] = 1.0; // h = delta ⇒ FFT2(h) = all ones ⇒ conv(x, delta) = x (validates the fwd→inv round-trip + 1/(R·C) scale)
    run_fft2d_conv(rr, cc, xr, xi, hr, hi, outr, outi, 4);

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(xr[k]) ? maxmag : fabs64(xr[k]); maxmag = maxmag > fabs64(xi[k]) ? maxmag : fabs64(xi[k]); }
    int bad = 0;
    for (int k = 0; k < rc; ++k)
    {
        if (fabs64(outr[k] - xr[k]) > 1e-3 * maxmag) { ++bad; }
        if (fabs64(outi[k] - xi[k]) > 1e-3 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR TRANSPOSE-ON-WRITE 3-dispatch conv == direct 2-D circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr = 64; // rows a power of 4 (radix-16 conv), cols power of 2
    constexpr int cc = 64;
    constexpr int rc = rr * cc;
    crd::memory::TlsfAllocator      alloc(256U << 20U);
    crd::containers::Array<crd::f64> x_re(&alloc); crd::containers::Array<crd::f64> x_im(&alloc);
    crd::containers::Array<crd::f64> h_re(&alloc); crd::containers::Array<crd::f64> h_im(&alloc);
    crd::containers::Array<crd::f64> refr(&alloc); crd::containers::Array<crd::f64> refi(&alloc);
    x_re.resize(rc); x_im.resize(rc); h_re.resize(rc); h_im.resize(rc); refr.resize(rc); refi.resize(rc);
    for (int i = 0; i < rc; ++i)
    {
        x_re[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0;
        x_im[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0;
        h_re[static_cast<crd::usize>(i)] = (i < 3) ? 1.0 : 0.0; // tiny box filter
        h_im[static_cast<crd::usize>(i)] = 0.0;
    }
    conv2d_ref(rr, cc, x_re.data(), x_im.data(), h_re.data(), h_im.data(), refr.data(), refi.data());

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc);
    kir::KGraph* graphs[3] = {&g0, &g1, &g2};
    const kir::Fft2dPlan plan = kir::build_fft2d_convolution_strided(graphs, rr, cc);

    int off[16]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rc; ++i) { buf(plan.in_re)[i] = f32(x_re[static_cast<crd::usize>(i)]); buf(plan.in_im)[i] = f32(x_im[static_cast<crd::usize>(i)]); }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = kTwoPi * k / cc; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = kTwoPi * k / rr; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }
    // filter spectrum H = FFT2(h), ROW-MAJOR (filt[u*cols + c]).
    crd::containers::Array<crd::f64> hr(&alloc); crd::containers::Array<crd::f64> hi(&alloc);
    hr.resize(rc); hi.resize(rc);
    dft2_ref(rr, cc, h_re.data(), h_im.data(), hr.data(), hi.data(), &alloc);
    for (int i = 0; i < rc; ++i) { buf(plan.filt_re)[i] = f32(hr[static_cast<crd::usize>(i)]); buf(plan.filt_im)[i] = f32(hi[static_cast<crd::usize>(i)]); }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(refr[static_cast<crd::usize>(k)]) ? maxmag : fabs64(refr[static_cast<crd::usize>(k)]); }
    int bad = 0;
    for (int k = 0; k < rc; ++k)
    {
        if (fabs64(buf(plan.res_re)[k] - refr[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
        if (fabs64(buf(plan.res_im)[k] - refi[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR TILED (tile_c=4) 3-dispatch conv == direct 2-D circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr = 64; // cols divisible by tile_c=4 → coalesced multi-column blocks
    constexpr int cc = 64;
    constexpr int rc = rr * cc;
    crd::memory::TlsfAllocator      alloc(256U << 20U);
    crd::containers::Array<crd::f64> x_re(&alloc); crd::containers::Array<crd::f64> x_im(&alloc);
    crd::containers::Array<crd::f64> h_re(&alloc); crd::containers::Array<crd::f64> h_im(&alloc);
    crd::containers::Array<crd::f64> refr(&alloc); crd::containers::Array<crd::f64> refi(&alloc);
    x_re.resize(rc); x_im.resize(rc); h_re.resize(rc); h_im.resize(rc); refr.resize(rc); refi.resize(rc);
    for (int i = 0; i < rc; ++i)
    {
        x_re[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0;
        x_im[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 5 + 1) % 7) - 3.0;
        h_re[static_cast<crd::usize>(i)] = (i < 3) ? 1.0 : 0.0; // tiny box filter
        h_im[static_cast<crd::usize>(i)] = 0.0;
    }
    conv2d_ref(rr, cc, x_re.data(), x_im.data(), h_re.data(), h_im.data(), refr.data(), refi.data());

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc);
    kir::KGraph* graphs[3] = {&g0, &g1, &g2};
    const kir::Fft2dPlan plan = kir::build_fft2d_convolution_strided(graphs, rr, cc, 4);

    int off[16]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rc; ++i) { buf(plan.in_re)[i] = f32(x_re[static_cast<crd::usize>(i)]); buf(plan.in_im)[i] = f32(x_im[static_cast<crd::usize>(i)]); }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = kTwoPi * k / cc; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = kTwoPi * k / rr; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }
    crd::containers::Array<crd::f64> hr(&alloc); crd::containers::Array<crd::f64> hi(&alloc);
    hr.resize(rc); hi.resize(rc);
    dft2_ref(rr, cc, h_re.data(), h_im.data(), hr.data(), hi.data(), &alloc);
    for (int i = 0; i < rc; ++i) { buf(plan.filt_re)[i] = f32(hr[static_cast<crd::usize>(i)]); buf(plan.filt_im)[i] = f32(hi[static_cast<crd::usize>(i)]); }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(refr[static_cast<crd::usize>(k)]) ? maxmag : fabs64(refr[static_cast<crd::usize>(k)]); }
    int bad = 0;
    for (int k = 0; k < rc; ++k)
    {
        if (fabs64(buf(plan.res_re)[k] - refr[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
        if (fabs64(buf(plan.res_im)[k] - refi[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR BATCHED (B=3, tile_c=4) 3-dispatch conv == per-image 2-D circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr    = 64; // B contiguous images, ONE shared PSF spectrum -- the DRAM-bound crush regime
    constexpr int cc    = 64;
    constexpr int batch = 3;
    constexpr int rc    = rr * cc;
    constexpr int rcb   = rc * batch;
    crd::memory::TlsfAllocator      alloc(512U << 20U);
    crd::containers::Array<crd::f64> x_re(&alloc); crd::containers::Array<crd::f64> x_im(&alloc);
    crd::containers::Array<crd::f64> h_re(&alloc); crd::containers::Array<crd::f64> h_im(&alloc);
    crd::containers::Array<crd::f64> refr(&alloc); crd::containers::Array<crd::f64> refi(&alloc);
    x_re.resize(rcb); x_im.resize(rcb); h_re.resize(rc); h_im.resize(rc); refr.resize(rcb); refi.resize(rcb);
    for (int b = 0; b < batch; ++b)
    {
        for (int i = 0; i < rc; ++i) // each image gets DIFFERENT data so a cross-image index bug is caught
        {
            const crd::usize idx = static_cast<crd::usize>(b) * static_cast<crd::usize>(rc) + static_cast<crd::usize>(i);
            x_re[idx]            = static_cast<crd::f64>((i * 7 + 3 + b * 13) % 11) - 5.0;
            x_im[idx]            = static_cast<crd::f64>((i * 5 + 1 + b * 2) % 7) - 3.0;
        }
    }
    for (int i = 0; i < rc; ++i) { h_re[static_cast<crd::usize>(i)] = (i < 3) ? 1.0 : 0.0; h_im[static_cast<crd::usize>(i)] = 0.0; }
    for (int b = 0; b < batch; ++b) // reference: the SAME filter convolves EACH image independently
    {
        conv2d_ref(rr, cc, x_re.data() + b * rc, x_im.data() + b * rc, h_re.data(), h_im.data(), refr.data() + b * rc, refi.data() + b * rc);
    }

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc);
    kir::KGraph* graphs[3] = {&g0, &g1, &g2};
    const kir::Fft2dPlan plan = kir::build_fft2d_convolution_strided(graphs, rr, cc, 4, batch);

    int off[16]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rcb; ++i) { buf(plan.in_re)[i] = f32(x_re[static_cast<crd::usize>(i)]); buf(plan.in_im)[i] = f32(x_im[static_cast<crd::usize>(i)]); }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = kTwoPi * k / cc; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = kTwoPi * k / rr; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }
    crd::containers::Array<crd::f64> hr(&alloc); crd::containers::Array<crd::f64> hi(&alloc);
    hr.resize(rc); hi.resize(rc);
    dft2_ref(rr, cc, h_re.data(), h_im.data(), hr.data(), hi.data(), &alloc);
    for (int i = 0; i < rc; ++i) { buf(plan.filt_re)[i] = f32(hr[static_cast<crd::usize>(i)]); buf(plan.filt_im)[i] = f32(hi[static_cast<crd::usize>(i)]); }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rcb; ++k) { maxmag = maxmag > fabs64(refr[static_cast<crd::usize>(k)]) ? maxmag : fabs64(refr[static_cast<crd::usize>(k)]); }
    int bad = 0;
    for (int k = 0; k < rcb; ++k)
    {
        if (fabs64(buf(plan.res_re)[k] - refr[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
        if (fabs64(buf(plan.res_im)[k] - refi[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR R2C real FFT half-spectrum == direct DFT; C2R round-trip == N.x (f32 tol)", "[kir][kernel][fft]")
{
    constexpr int n  = 256; // power of 4 (radix-16 core)
    constexpr int hs = n / 2 + 1;
    crd::f64 x[n];
    for (int i = 0; i < n; ++i) { x[i] = static_cast<crd::f64>((i * 5 + 3) % 13) - 6.0 + 0.25 * static_cast<crd::f64>(i % 4); } // REAL

    // R2C == the direct full DFT of the real signal, columns 0..N/2
    crd::f64 hr[hs]; crd::f64 hi[hs];
    run_r2c(n, hs, x, hr, hi);
    crd::f64 xi0[n]; crd::f64 refr[n]; crd::f64 refi[n];
    for (int i = 0; i < n; ++i) { xi0[i] = 0.0; }
    dft_ref(n, x, xi0, refr, refi);
    crd::f64 maxmag = 1e-6;
    for (int k = 0; k <= n / 2; ++k) { maxmag = maxmag > fabs64(refr[k]) ? maxmag : fabs64(refr[k]); maxmag = maxmag > fabs64(refi[k]) ? maxmag : fabs64(refi[k]); }
    int bad = 0;
    for (int k = 0; k <= n / 2; ++k)
    {
        if (fabs64(hr[k] - refr[k]) > 5e-3 * maxmag) { ++bad; }
        if (fabs64(hi[k] - refi[k]) > 5e-3 * maxmag) { ++bad; }
    }
    CHECK(bad == 0);

    // C2R(R2C(x)) == N.x (unnormalized forward+inverse)
    crd::f64 rt[n];
    run_c2r(n, hs, hr, hi, rt);
    crd::f64 xmax = 1e-6;
    for (int i = 0; i < n; ++i) { xmax = xmax > fabs64(x[i]) ? xmax : fabs64(x[i]); }
    int badr = 0;
    for (int i = 0; i < n; ++i) { if (fabs64(rt[i] - static_cast<crd::f64>(n) * x[i]) > 5e-3 * static_cast<crd::f64>(n) * xmax) { ++badr; } }
    CHECK(badr == 0);
}

TEST_CASE("B-cmp: CKIR R2C 3-dispatch REAL 2-D conv == direct 2-D circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr = 64; // real image + real PSF ⇒ half-width column conv
    constexpr int cc = 64;
    constexpr int tc = 4;
    constexpr int rc = rr * cc;
    crd::memory::TlsfAllocator      alloc(256U << 20U);
    crd::containers::Array<crd::f64> x_re(&alloc); crd::containers::Array<crd::f64> x_im(&alloc);
    crd::containers::Array<crd::f64> h_re(&alloc); crd::containers::Array<crd::f64> h_im(&alloc);
    crd::containers::Array<crd::f64> refr(&alloc); crd::containers::Array<crd::f64> refi(&alloc);
    x_re.resize(rc); x_im.resize(rc); h_re.resize(rc); h_im.resize(rc); refr.resize(rc); refi.resize(rc);
    for (int i = 0; i < rc; ++i)
    {
        x_re[static_cast<crd::usize>(i)] = static_cast<crd::f64>((i * 7 + 3) % 11) - 5.0; // REAL image
        x_im[static_cast<crd::usize>(i)] = 0.0;
        h_re[static_cast<crd::usize>(i)] = (i < 3) ? 1.0 : 0.0; // REAL box filter
        h_im[static_cast<crd::usize>(i)] = 0.0;
    }
    conv2d_ref(rr, cc, x_re.data(), x_im.data(), h_re.data(), h_im.data(), refr.data(), refi.data());

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc);
    kir::KGraph* graphs[3] = {&g0, &g1, &g2};
    const kir::Fft2dPlan plan = kir::build_fft2d_convolution_r2c(graphs, rr, cc, tc, 1);
    const int hw = plan.buffers[static_cast<crd::usize>(plan.filt_re)].size / rr; // half-spectrum row width

    int off[20]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rc; ++i) { buf(plan.in_re)[i] = f32(x_re[static_cast<crd::usize>(i)]); }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = kTwoPi * k / cc; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = kTwoPi * k / rr; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }
    // HALF PSF: H = FFT2(h), columns 0..cols/2 stored row-major at [u*hw + c].
    crd::containers::Array<crd::f64> hr(&alloc); crd::containers::Array<crd::f64> hi(&alloc);
    hr.resize(rc); hi.resize(rc);
    dft2_ref(rr, cc, h_re.data(), h_im.data(), hr.data(), hi.data(), &alloc);
    for (int u = 0; u < rr; ++u)
    {
        for (int c = 0; c <= cc / 2; ++c)
        {
            const crd::usize hsrc          = static_cast<crd::usize>(u) * static_cast<crd::usize>(cc) + static_cast<crd::usize>(c);
            buf(plan.filt_re)[u * hw + c] = f32(hr[hsrc]);
            buf(plan.filt_im)[u * hw + c] = f32(hi[hsrc]);
        }
    }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rc; ++k) { maxmag = maxmag > fabs64(refr[static_cast<crd::usize>(k)]) ? maxmag : fabs64(refr[static_cast<crd::usize>(k)]); }
    int bad = 0;
    for (int k = 0; k < rc; ++k) { if (fabs64(buf(plan.res_re)[k] - refr[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: CKIR R2C BATCHED (B=3) REAL 2-D conv == per-image circular convolution (f32 tol)", "[kir][kernel][fft][conv]")
{
    constexpr int rr    = 64;
    constexpr int cc    = 64;
    constexpr int tc    = 4;
    constexpr int batch = 3;
    constexpr int rc    = rr * cc;
    constexpr int rcb   = rc * batch;
    crd::memory::TlsfAllocator      alloc(512U << 20U);
    crd::containers::Array<crd::f64> x_re(&alloc); crd::containers::Array<crd::f64> h_re(&alloc); crd::containers::Array<crd::f64> h_im(&alloc);
    crd::containers::Array<crd::f64> refr(&alloc); crd::containers::Array<crd::f64> refi(&alloc); crd::containers::Array<crd::f64> zeros(&alloc);
    x_re.resize(rcb); h_re.resize(rc); h_im.resize(rc); refr.resize(rcb); refi.resize(rcb); zeros.resize(rc, 0.0);
    for (int b = 0; b < batch; ++b)
    {
        for (int i = 0; i < rc; ++i)
        {
            const crd::usize idx = static_cast<crd::usize>(b) * static_cast<crd::usize>(rc) + static_cast<crd::usize>(i);
            x_re[idx]            = static_cast<crd::f64>((i * 7 + 3 + b * 13) % 11) - 5.0; // DIFFERENT per image
        }
    }
    for (int i = 0; i < rc; ++i) { h_re[static_cast<crd::usize>(i)] = (i < 3) ? 1.0 : 0.0; h_im[static_cast<crd::usize>(i)] = 0.0; }
    for (int b = 0; b < batch; ++b) { conv2d_ref(rr, cc, x_re.data() + b * rc, zeros.data(), h_re.data(), h_im.data(), refr.data() + b * rc, refi.data() + b * rc); }

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc); kir::KGraph g2(&alloc);
    kir::KGraph* graphs[3] = {&g0, &g1, &g2};
    const kir::Fft2dPlan plan = kir::build_fft2d_convolution_r2c(graphs, rr, cc, tc, batch);
    const int hw = plan.buffers[static_cast<crd::usize>(plan.filt_re)].size / rr;

    int off[20]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(static_cast<crd::usize>(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rcb; ++i) { buf(plan.in_re)[i] = f32(x_re[static_cast<crd::usize>(i)]); }
    for (int k = 0; k < cc; ++k) { const crd::f64 a = kTwoPi * k / cc; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rr; ++k) { const crd::f64 a = kTwoPi * k / rr; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }
    crd::containers::Array<crd::f64> hr(&alloc); crd::containers::Array<crd::f64> hi(&alloc);
    hr.resize(rc); hi.resize(rc);
    dft2_ref(rr, cc, h_re.data(), h_im.data(), hr.data(), hi.data(), &alloc);
    for (int u = 0; u < rr; ++u)
    {
        for (int c = 0; c <= cc / 2; ++c)
        {
            const crd::usize hsrc          = static_cast<crd::usize>(u) * static_cast<crd::usize>(cc) + static_cast<crd::usize>(c);
            buf(plan.filt_re)[u * hw + c] = f32(hr[hsrc]);
            buf(plan.filt_im)[u * hw + c] = f32(hi[hsrc]);
        }
    }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::f64 maxmag = 1e-6;
    for (int k = 0; k < rcb; ++k) { maxmag = maxmag > fabs64(refr[static_cast<crd::usize>(k)]) ? maxmag : fabs64(refr[static_cast<crd::usize>(k)]); }
    int bad = 0;
    for (int k = 0; k < rcb; ++k) { if (fabs64(buf(plan.res_re)[k] - refr[static_cast<crd::usize>(k)]) > 1e-2 * maxmag) { ++bad; } }
    CHECK(bad == 0);
}

TEST_CASE("B-cmp: batched STRIDED inverse 2-D FFT (build_fft2d_c2c_batched) == direct inverse DFT, every image", "[kir][kernel][fft][fft2d][batched]")
{
    constexpr int rows  = 16; // power of FOUR (radix-16/4 tiled column)
    constexpr int cols  = 16;
    constexpr int batch = 3;  // three DIFFERENT complex images share one dispatch set
    constexpr int tilec = 8;  // columns per column-FFT block; must divide cols
    constexpr int rc    = rows * cols;
    const auto    uz    = [](int v) { return static_cast<crd::usize>(v); };

    crd::memory::TlsfAllocator      alloc(128U << 20U);
    crd::containers::Array<crd::f64> inr(&alloc); crd::containers::Array<crd::f64> ini(&alloc);
    inr.resize(uz(rc * batch)); ini.resize(uz(rc * batch));
    for (int i = 0; i < rc * batch; ++i)
    {
        inr[uz(i)] = crd::math::cos(0.11 * i) + 0.4 * crd::math::sin(0.03 * i + 1.0);
        ini[uz(i)] = 0.3 * crd::math::sin(0.07 * i) - 0.2 * crd::math::cos(0.05 * i + 0.5);
    }

    kir::KGraph  g0(&alloc); kir::KGraph g1(&alloc);
    kir::KGraph* graphs[2] = {&g0, &g1};
    const kir::Fft2dPlan plan = kir::build_fft2d_c2c_batched(graphs, rows, cols, batch, /*inverse=*/true, tilec);

    int off[16]; int total = 0;
    for (int b = 0; b < plan.nbuffers; ++b) { off[b] = total; total += plan.buffers[b].size; }
    crd::containers::Array<crd::f64> arena(&alloc);
    arena.resize(uz(total), 0.0);
    const auto buf = [&](int id) -> crd::f64* { return arena.data() + off[id]; };
    const auto f32 = [](crd::f64 v) { return static_cast<crd::f64>(static_cast<float>(v)); };

    for (int i = 0; i < rc * batch; ++i) { buf(plan.in_re)[i] = f32(inr[uz(i)]); buf(plan.in_im)[i] = f32(ini[uz(i)]); }
    for (int k = 0; k < cols; ++k) { const crd::f64 a = kTwoPi * k / cols; buf(plan.tw_col_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_col_im)[k] = f32(-crd::math::sin(a)); }
    for (int k = 0; k < rows; ++k) { const crd::f64 a = kTwoPi * k / rows; buf(plan.tw_row_re)[k] = f32(crd::math::cos(a)); buf(plan.tw_row_im)[k] = f32(-crd::math::sin(a)); }

    for (int pi = 0; pi < plan.npasses; ++pi)
    {
        const kir::Fft2dPass& p = plan.passes[pi];
        kir::KernelBuffer      kb[8];
        for (int k = 0; k < p.nbind; ++k) { kb[k] = kir::KernelBuffer{buf(p.bind[k]), plan.buffers[p.bind[k]].size, 0, static_cast<crd::u8>(k)}; }
        kir::eval_cpu_kernel(*p.graph, p.entry, kb, p.nbind, p.entry.local_size[0], &alloc, p.num_workgroups);
    }

    crd::containers::Array<crd::f64> xr(&alloc); crd::containers::Array<crd::f64> xi(&alloc);
    crd::containers::Array<crd::f64> rr(&alloc); crd::containers::Array<crd::f64> ri(&alloc);
    xr.resize(uz(rc)); xi.resize(uz(rc)); rr.resize(uz(rc)); ri.resize(uz(rc));
    crd::f64 maxerr = 0.0;
    for (int im = 0; im < batch; ++im)
    {
        for (int i = 0; i < rc; ++i) { xr[uz(i)] = f32(inr[uz(im * rc + i)]); xi[uz(i)] = f32(ini[uz(im * rc + i)]); }
        idft2_ref(rows, cols, xr.data(), xi.data(), rr.data(), ri.data());
        for (int i = 0; i < rc; ++i)
        {
            const crd::f64 gr    = buf(plan.res_re)[im * rc + i];
            const crd::f64 gi    = buf(plan.res_im)[im * rc + i];
            const crd::f64 scale = 1.0 > (fabs64(rr[uz(i)]) + fabs64(ri[uz(i)])) ? 1.0 : (fabs64(rr[uz(i)]) + fabs64(ri[uz(i)]));
            const crd::f64 er    = fabs64(gr - rr[uz(i)]) / scale;
            const crd::f64 ei    = fabs64(gi - ri[uz(i)]) / scale;
            maxerr = maxerr > er ? maxerr : er;
            maxerr = maxerr > ei ? maxerr : ei;
        }
    }
    CHECK(maxerr < 1e-3);
}
