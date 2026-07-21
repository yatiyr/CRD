// test_ckir_sort.cpp -- B-cmp: the CKIR STABLE LSD RADIX SORT (ckir_sort.hpp) on the CPU oracle. First increment: the HISTOGRAM
// kernel (shared atomic-add ⇒ order-independent bit-exact counts). Validates the digit extraction + per-block histogram vs a
// direct count. Scan-offset + scatter kernels + the multi-pass driver + GPU dispatch + CUB bench are the next increments.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/kir/ckir_sort.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

TEST_CASE("B-cmp: CKIR radix-sort HISTOGRAM == direct per-block digit count", "[kir][kernel][sort]")
{
    constexpr int n          = 16384;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 1 << radix_bits;
    constexpr int epb        = 1024;         // elems per block
    constexpr int nblocks    = n / epb;      // 16
    crd::memory::TlsfAllocator alloc(64U << 20U);

    crd::containers::Array<crd::f64> keys(&alloc);
    keys.resize(n);
    for (int i = 0; i < n; ++i) { keys[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<crd::u32>(i) * 2654435761U); } // Knuth hash ⇒ spread digits

    for (int shift = 0; shift < 32; shift += 8) // all four 8-bit digits
    {
        kir::KGraph       g(&alloc);
        const kir::KEntry e = kir::build_sort_histogram(g, epb, threads, radix_bits, shift, nblocks);

        crd::containers::Array<crd::f64> bhist(&alloc);
        bhist.resize(static_cast<crd::usize>(nblocks) * static_cast<crd::usize>(nbins), -1.0);
        kir::KernelBuffer kb[2] = {{keys.data(), n, 0, 0}, {bhist.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(g, e, kb, 2, e.local_size[0], &alloc, static_cast<crd::u32>(nblocks));

        // reference: block_hist[block*nbins + bin] = count of block's keys with digit == bin
        int bad = 0;
        for (int blk = 0; blk < nblocks; ++blk)
        {
            int ref[nbins];
            for (int b = 0; b < nbins; ++b) { ref[b] = 0; }
            for (int j = 0; j < epb; ++j)
            {
                const crd::usize kidx = static_cast<crd::usize>(blk) * static_cast<crd::usize>(epb) + static_cast<crd::usize>(j);
                const crd::u32   key  = static_cast<crd::u32>(keys[kidx]);
                const int        d    = static_cast<int>((key >> static_cast<crd::u32>(shift)) & static_cast<crd::u32>(nbins - 1));
                ++ref[d];
            }
            for (int b = 0; b < nbins; ++b) // BIN-major layout: bhist[bin*nblocks + blk]
            {
                const crd::usize hidx = static_cast<crd::usize>(b) * static_cast<crd::usize>(nblocks) + static_cast<crd::usize>(blk);
                if (static_cast<int>(bhist[hidx]) != ref[b]) { ++bad; }
            }
        }
        CHECK(bad == 0);
    }
}

TEST_CASE("B-cmp: CKIR full 4-pass LSD radix sort == sorted (oracle)", "[kir][kernel][sort]")
{
    constexpr int n          = 16384;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 1 << radix_bits;
    constexpr int epb        = 1024;
    constexpr int nblocks    = n / epb;
    crd::memory::TlsfAllocator alloc(128U << 20U);

    crd::containers::Array<crd::f64> ka(&alloc); crd::containers::Array<crd::f64> kb(&alloc);
    crd::containers::Array<crd::f64> bh(&alloc); crd::containers::Array<crd::f64> go(&alloc);
    crd::containers::Array<crd::f64> tot(&alloc); crd::containers::Array<crd::f64> gb(&alloc);
    ka.resize(n); kb.resize(n, 0.0);
    bh.resize(static_cast<crd::usize>(nblocks) * static_cast<crd::usize>(nbins), 0.0);
    go.resize(static_cast<crd::usize>(nblocks) * static_cast<crd::usize>(nbins), 0.0);
    tot.resize(static_cast<crd::usize>(nbins), 0.0);
    gb.resize(static_cast<crd::usize>(nbins), 0.0);
    constexpr int scan_threads = nblocks < threads ? nblocks : threads; // divides nblocks (16 here)
    for (int i = 0; i < n; ++i) { ka[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<crd::u32>((i * 1103515245U + 12345U) ^ (static_cast<crd::u32>(i) << 13U))); }

    crd::f64* cur = ka.data();
    crd::f64* oth = kb.data();
    for (int pass = 0; pass < 4; ++pass)
    {
        const int shift = pass * 8;
        kir::KGraph gh(&alloc); kir::KGraph gof1(&alloc); kir::KGraph gof2(&alloc); kir::KGraph gs(&alloc);
        const kir::KEntry eh  = kir::build_sort_histogram(gh, epb, threads, radix_bits, shift, nblocks);
        const kir::KEntry eo1 = kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads);
        const kir::KEntry eo2 = kir::build_sort_gbase(gof2, radix_bits);
        const kir::KEntry es  = kir::build_sort_scatter(gs, epb, threads, radix_bits, shift, nblocks);

        kir::KernelBuffer h[2] = {{cur, n, 0, 0}, {bh.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(gh, eh, h, 2, eh.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        // parallel offset: local (grid=nbins) → within-bin block prefix + totals; gbase (1 WG) → per-bin global base gb.
        kir::KernelBuffer o1[3] = {{bh.data(), nblocks * nbins, 0, 0}, {go.data(), nblocks * nbins, 0, 1}, {tot.data(), nbins, 0, 2}};
        kir::eval_cpu_kernel(gof1, eo1, o1, 3, eo1.local_size[0], &alloc, static_cast<crd::u32>(nbins));
        kir::KernelBuffer o2[2] = {{tot.data(), nbins, 0, 0}, {gb.data(), nbins, 0, 1}};
        kir::eval_cpu_kernel(gof2, eo2, o2, 2, eo2.local_size[0], &alloc, 1U);
        if (pass == 0) // verify gb[b] + go[blk][b] = the exact bin-major-then-block global prefix
        {
            int offbad = 0; crd::u32 running = 0U;
            for (int b = 0; b < nbins; ++b)
            {
                for (int blk = 0; blk < nblocks; ++blk)
                {
                    const crd::u32 got = static_cast<crd::u32>(gb[static_cast<crd::usize>(b)])
                                       + static_cast<crd::u32>(go[static_cast<crd::usize>(b) * static_cast<crd::usize>(nblocks) + static_cast<crd::usize>(blk)]); // BIN-major
                    if (got != running) { ++offbad; }
                    running += static_cast<crd::u32>(bh[static_cast<crd::usize>(b) * static_cast<crd::usize>(nblocks) + static_cast<crd::usize>(blk)]); // BIN-major
                }
            }
            CHECK(offbad == 0); // gb + within-bin prefix == the exact global prefix
            CHECK(running == static_cast<crd::u32>(n));
        }
        kir::KernelBuffer s[4] = {{cur, n, 0, 0}, {oth, n, 0, 1}, {go.data(), nblocks * nbins, 0, 2}, {gb.data(), nbins, 0, 3}};
        kir::eval_cpu_kernel(gs, es, s, 4, es.local_size[0], &alloc, static_cast<crd::u32>(nblocks));

        crd::f64* tmp = cur; cur = oth; oth = tmp; // ping-pong
    }

    // `cur` holds the fully-sorted keys — verify ascending.
    int bad = 0;
    for (int i = 1; i < n; ++i) { if (static_cast<crd::u32>(cur[static_cast<crd::usize>(i - 1)]) > static_cast<crd::u32>(cur[static_cast<crd::usize>(i)])) { ++bad; } }
    CHECK(bad == 0);
    // it is a PERMUTATION of the input (same multiset) — XOR + sum checksum is invariant under reordering.
    crd::u32 ix = 0U; crd::u32 sx = 0U; crd::u32 is = 0U; crd::u32 ss = 0U;
    for (int i = 0; i < n; ++i)
    {
        const crd::u32 in  = static_cast<crd::u32>(ka[static_cast<crd::usize>(i)]);
        const crd::u32 out = static_cast<crd::u32>(cur[static_cast<crd::usize>(i)]);
        ix ^= in; sx ^= out; is += in; ss += out;
    }
    CHECK(ix == sx);
    CHECK(is == ss);
}

// B19-a3: the KEY-VALUE radix sort — a per-key payload rides the same permutation as its key. The keystone that 3DGS
// needs (sort splats by depth, carry the gaussian index) and any indexed sort. Same 4-pass driver, `carry_val` on.
TEST_CASE("B-cmp: CKIR key-VALUE radix sort carries the payload bit-exactly", "[kir][kernel][sort]")
{
    constexpr int n          = 16384;
    constexpr int threads    = 256;
    constexpr int radix_bits = 8;
    constexpr int nbins      = 1 << radix_bits;
    constexpr int epb        = 1024;
    constexpr int nblocks    = n / epb;
    crd::memory::TlsfAllocator alloc(160U << 20U);

    crd::containers::Array<crd::f64> ka(&alloc); crd::containers::Array<crd::f64> kb(&alloc);
    crd::containers::Array<crd::f64> va(&alloc); crd::containers::Array<crd::f64> vb(&alloc);
    crd::containers::Array<crd::f64> bh(&alloc); crd::containers::Array<crd::f64> go(&alloc);
    crd::containers::Array<crd::f64> tot(&alloc); crd::containers::Array<crd::f64> gb(&alloc);
    ka.resize(n); kb.resize(n, 0.0); va.resize(n); vb.resize(n, 0.0);
    bh.resize(static_cast<crd::usize>(nblocks) * static_cast<crd::usize>(nbins), 0.0);
    go.resize(static_cast<crd::usize>(nblocks) * static_cast<crd::usize>(nbins), 0.0);
    tot.resize(static_cast<crd::usize>(nbins), 0.0);
    gb.resize(static_cast<crd::usize>(nbins), 0.0);
    constexpr int scan_threads = nblocks < threads ? nblocks : threads;
    crd::containers::Array<crd::u32> key0(&alloc);
    key0.resize(n);
    for (int i = 0; i < n; ++i)
    {
        const crd::u32 k = static_cast<crd::u32>((i * 2246822519U) ^ (static_cast<crd::u32>(i) << 11U));
        ka[static_cast<crd::usize>(i)] = static_cast<crd::f64>(k);
        key0[static_cast<crd::usize>(i)] = k;
        va[static_cast<crd::usize>(i)] = static_cast<crd::f64>(static_cast<crd::u32>(i)); // payload = original index
    }

    crd::f64* ck = ka.data(); crd::f64* ok = kb.data();
    crd::f64* cv = va.data(); crd::f64* ov = vb.data();
    for (int pass = 0; pass < 4; ++pass)
    {
        const int shift = pass * 8;
        kir::KGraph gh(&alloc); kir::KGraph gof1(&alloc); kir::KGraph gof2(&alloc); kir::KGraph gs(&alloc);
        const kir::KEntry eh  = kir::build_sort_histogram(gh, epb, threads, radix_bits, shift, nblocks);
        const kir::KEntry eo1 = kir::build_sort_offset_local(gof1, nblocks, radix_bits, scan_threads);
        const kir::KEntry eo2 = kir::build_sort_gbase(gof2, radix_bits);
        const kir::KEntry es  = kir::build_sort_scatter(gs, epb, threads, radix_bits, shift, nblocks, /*carry_val=*/true);

        kir::KernelBuffer h[2] = {{ck, n, 0, 0}, {bh.data(), nblocks * nbins, 0, 1}};
        kir::eval_cpu_kernel(gh, eh, h, 2, eh.local_size[0], &alloc, static_cast<crd::u32>(nblocks));
        kir::KernelBuffer o1[3] = {{bh.data(), nblocks * nbins, 0, 0}, {go.data(), nblocks * nbins, 0, 1}, {tot.data(), nbins, 0, 2}};
        kir::eval_cpu_kernel(gof1, eo1, o1, 3, eo1.local_size[0], &alloc, static_cast<crd::u32>(nbins));
        kir::KernelBuffer o2[2] = {{tot.data(), nbins, 0, 0}, {gb.data(), nbins, 0, 1}};
        kir::eval_cpu_kernel(gof2, eo2, o2, 2, eo2.local_size[0], &alloc, 1U);
        kir::KernelBuffer s[6] = {{ck, n, 0, 0}, {ok, n, 0, 1}, {go.data(), nblocks * nbins, 0, 2}, {gb.data(), nbins, 0, 3},
                                  {cv, n, 0, 4}, {ov, n, 0, 5}};
        kir::eval_cpu_kernel(gs, es, s, 6, es.local_size[0], &alloc, static_cast<crd::u32>(nblocks));

        crd::f64* tk = ck; ck = ok; ok = tk;
        crd::f64* tv = cv; cv = ov; ov = tv;
    }

    // keys ascending, AND the payload followed: the value at position i is the ORIGINAL index of the key now there.
    int badk = 0; int badv = 0;
    for (int i = 0; i < n; ++i)
    {
        if (i > 0 && static_cast<crd::u32>(ck[static_cast<crd::usize>(i - 1)]) > static_cast<crd::u32>(ck[static_cast<crd::usize>(i)])) { ++badk; }
        const crd::u32 idx = static_cast<crd::u32>(cv[static_cast<crd::usize>(i)]);
        if (idx >= static_cast<crd::u32>(n) || key0[static_cast<crd::usize>(idx)] != static_cast<crd::u32>(ck[static_cast<crd::usize>(i)])) { ++badv; }
    }
    CHECK(badk == 0); // keys sorted
    CHECK(badv == 0); // every payload points back to the original key now at its position
    // the payload set is a permutation of 0..n-1 (XOR + sum checksum invariant).
    crd::u32 vx = 0U; crd::u32 vs = 0U;
    for (int i = 0; i < n; ++i) { vx ^= static_cast<crd::u32>(cv[static_cast<crd::usize>(i)]); vs += static_cast<crd::u32>(cv[static_cast<crd::usize>(i)]); }
    crd::u32 rx = 0U; crd::u32 rs = 0U;
    for (int i = 0; i < n; ++i) { rx ^= static_cast<crd::u32>(i); rs += static_cast<crd::u32>(i); }
    CHECK(vx == rx);
    CHECK(vs == rs);
}
