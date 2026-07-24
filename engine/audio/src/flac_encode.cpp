// flac_encode.cpp — GEO-10: the FLAC encoder — a COMPLIANT SUBSET writer (see flac.hpp): fixed 4096 blocking,
// per-frame stereo decorrelation by estimated bits, FIXED(0-4) + LPC(8, hesap-dsp aryule) predictors chosen by
// estimated rice cost, rice method 1 with partition order 0, STREAMINFO stamped with the PCM MD5. Correctness
// bar: decode(encode(x)) is BIT-EXACT with the MD5 verifying (gated); ratio grows at MED-6, not here.

#include <crd/audio/flac.hpp>

#include "flac_detail.hpp"

#include <crd/hesap/dsp/ar.hpp>


namespace crd::audio
{

namespace
{
    using detail::BitWriter;

    constexpr crd::u32 kBlockSize = 4096;
    constexpr crd::u32 kLpcOrder  = 8;
    constexpr crd::u32 kLpcPrecision = 12; // quantized coefficient bits (a common reference choice)

    // rice cost of a residual set at the best single parameter; returns the parameter too
    [[nodiscard]] crd::u64 rice_cost(const crd::i64* r, crd::usize n, crd::u32& best_param)
    {
        // param from the mean of the zigzag magnitudes (the standard estimator), then refine ±1
        crd::u64 sum = 0;
        for (crd::usize i = 0; i < n; ++i)
        {
            const crd::u64 u = (static_cast<crd::u64>(r[i]) << 1U) ^
                               static_cast<crd::u64>(r[i] >> 63); // zigzag
            sum += u;
        }
        crd::u32 base = 0;
        const crd::u64 mean = n > 0 ? sum / n : 0;
        while (base < 28 && (mean >> base) > 0) { ++base; }
        crd::u64 best = ~0ULL;
        best_param    = 0;
        for (crd::u32 p = base > 2 ? base - 2 : 0; p <= base + 1 && p <= 30; ++p)
        {
            crd::u64 bits = 0;
            for (crd::usize i = 0; i < n; ++i)
            {
                const crd::u64 u = (static_cast<crd::u64>(r[i]) << 1U) ^ static_cast<crd::u64>(r[i] >> 63);
                bits += (u >> p) + 1 + p;
                if (bits > (1ULL << 40U)) { break; } // hopeless parameter — stop counting
            }
            if (bits < best)
            {
                best       = bits;
                best_param = p;
            }
        }
        return best;
    }

    struct SubframePlan
    {
        crd::u32 type   = 8;      // 8+order = FIXED; 32+order-1 = LPC
        crd::u32 order  = 0;
        crd::u32 param  = 0;      // rice parameter (partition order 0)
        crd::u64 bits   = 0;      // estimated payload bits
        crd::i64 coef[32] = {};   // LPC only
        crd::i64 shift  = 0;      // LPC only
        bool     constant = false;
        bool     use_lpc  = false;
    };

    // residuals for a fixed order into `res` (size n - order)
    void fixed_residual(const crd::i64* x, crd::usize n, crd::u32 order, crd::i64* res)
    {
        for (crd::usize i = order; i < n; ++i)
        {
            crd::i64 p = 0;
            switch (order)
            {
            case 0: p = 0; break;
            case 1: p = x[i - 1]; break;
            case 2: p = 2 * x[i - 1] - x[i - 2]; break;
            case 3: p = 3 * x[i - 1] - 3 * x[i - 2] + x[i - 3]; break;
            case 4:
            default: p = 4 * x[i - 1] - 6 * x[i - 2] + 4 * x[i - 3] - x[i - 4]; break;
            }
            res[i - order] = x[i] - p;
        }
    }

    // plan the best subframe for one channel block
    [[nodiscard]] SubframePlan plan_subframe(crd::memory::IAllocator* alloc, const crd::i64* x, crd::usize n,
                                             crd::u32 bps)
    {
        SubframePlan plan;
        bool         all_equal = true;
        for (crd::usize i = 1; i < n; ++i)
        {
            if (x[i] != x[0])
            {
                all_equal = false;
                break;
            }
        }
        if (all_equal)
        {
            plan.constant = true;
            plan.bits     = bps;
            return plan;
        }

        crd::containers::Array<crd::i64> res(alloc);
        res.resize(n);

        // FIXED orders 0-4
        plan.bits = ~0ULL;
        for (crd::u32 o = 0; o <= 4 && o < n; ++o)
        {
            fixed_residual(x, n, o, res.data());
            crd::u32       param = 0;
            const crd::u64 bits  = rice_cost(res.data(), n - o, param) + static_cast<crd::u64>(o) * bps;
            if (bits < plan.bits)
            {
                plan.bits  = bits;
                plan.order = o;
                plan.param = param;
                plan.type  = 8 + o;
            }
        }

        // LPC order 8 via hesap-dsp aryule (f64 analysis, quantized to kLpcPrecision bits)
        if (n > kLpcOrder * 2)
        {
            crd::containers::Array<crd::f64> xf(alloc);
            xf.resize(n);
            for (crd::usize i = 0; i < n; ++i) { xf[i] = static_cast<crd::f64>(x[i]); }
            const crd::hesap::dsp::ArModel<crd::f64> model =
                crd::hesap::dsp::aryule<crd::f64>(alloc, crd::containers::ConstSpan<crd::f64>(xf.data(), n),
                                                  kLpcOrder);
            // FLAC predicts x̂[i] = Σ c[j]·x[i-1-j]; AR gives A(z) with prediction -Σ a[j+1]·x[i-1-j]
            crd::f64 cmax = 0.0;
            crd::f64 cf[kLpcOrder];
            for (crd::u32 j = 0; j < kLpcOrder; ++j)
            {
                cf[j] = -model.a[j + 1];
                const crd::f64 mag = cf[j] < 0.0 ? -cf[j] : cf[j];
                if (mag > cmax) { cmax = mag; }
            }
            if (cmax > 0.0)
            {
                // choose the shift so the largest coefficient fills the precision
                crd::i64 shift = static_cast<crd::i64>(kLpcPrecision) - 1;
                while (shift > 0 && cmax * static_cast<crd::f64>(1LL << shift) >=
                                        static_cast<crd::f64>(1LL << (kLpcPrecision - 1)))
                {
                    --shift;
                }
                if (shift > 0)
                {
                    SubframePlan lpc;
                    lpc.use_lpc = true;
                    lpc.order   = kLpcOrder;
                    lpc.type    = 32 + kLpcOrder - 1;
                    lpc.shift   = shift;
                    const crd::i64 lim = 1LL << (kLpcPrecision - 1);
                    for (crd::u32 j = 0; j < kLpcOrder; ++j)
                    {
                        crd::i64 q = static_cast<crd::i64>(
                            cf[j] * static_cast<crd::f64>(1LL << shift) + (cf[j] >= 0.0 ? 0.5 : -0.5));
                        if (q >= lim) { q = lim - 1; }
                        if (q < -lim) { q = -lim; }
                        lpc.coef[j] = q;
                    }
                    for (crd::usize i = kLpcOrder; i < n; ++i)
                    {
                        crd::i64 acc = 0;
                        for (crd::u32 j = 0; j < kLpcOrder; ++j) { acc += lpc.coef[j] * x[i - 1 - j]; }
                        res[i - kLpcOrder] = x[i] - (acc >> static_cast<crd::u32>(shift));
                    }
                    crd::u32       param = 0;
                    const crd::u64 bits  = rice_cost(res.data(), n - kLpcOrder, param) +
                                          static_cast<crd::u64>(kLpcOrder) * bps +
                                          static_cast<crd::u64>(kLpcOrder) * kLpcPrecision + 4 + 5;
                    if (bits < plan.bits)
                    {
                        lpc.param = param;
                        lpc.bits  = bits;
                        plan      = lpc;
                    }
                }
            }
        }
        return plan;
    }

    void write_subframe(BitWriter& bw, crd::memory::IAllocator* alloc, const crd::i64* x, crd::usize n,
                        crd::u32 bps, const SubframePlan& plan)
    {
        bw.write_bits(0, 1); // pad
        if (plan.constant)
        {
            bw.write_bits(0, 6);
            bw.write_bits(0, 1); // no wasted bits
            bw.write_bits(static_cast<crd::u64>(x[0]) & ((1ULL << bps) - 1ULL), bps);
            return;
        }
        bw.write_bits(plan.type, 6);
        bw.write_bits(0, 1); // no wasted bits

        for (crd::u32 i = 0; i < plan.order; ++i)
        {
            bw.write_bits(static_cast<crd::u64>(x[i]) & ((1ULL << bps) - 1ULL), bps);
        }
        if (plan.use_lpc)
        {
            bw.write_bits(kLpcPrecision - 1, 4);
            bw.write_bits(static_cast<crd::u64>(plan.shift) & 0x1FULL, 5);
            for (crd::u32 j = 0; j < plan.order; ++j)
            {
                bw.write_bits(static_cast<crd::u64>(plan.coef[j]) & ((1ULL << kLpcPrecision) - 1ULL),
                              kLpcPrecision);
            }
        }

        // residual: method 1 (5-bit params), partition order 0
        crd::containers::Array<crd::i64> res(alloc);
        res.resize(n);
        if (plan.use_lpc)
        {
            for (crd::usize i = plan.order; i < n; ++i)
            {
                crd::i64 acc = 0;
                for (crd::u32 j = 0; j < plan.order; ++j) { acc += plan.coef[j] * x[i - 1 - j]; }
                res[i - plan.order] = x[i] - (acc >> static_cast<crd::u32>(plan.shift));
            }
        }
        else
        {
            fixed_residual(x, n, plan.order, res.data());
        }
        bw.write_bits(1, 2); // method 1
        bw.write_bits(0, 4); // partition order 0
        bw.write_bits(plan.param, 5);
        for (crd::usize i = 0; i < n - plan.order; ++i)
        {
            const crd::u64 u = (static_cast<crd::u64>(res[i]) << 1U) ^ static_cast<crd::u64>(res[i] >> 63);
            bw.write_unary(static_cast<crd::u32>(u >> plan.param));
            bw.write_bits(u & ((1ULL << plan.param) - 1ULL), plan.param);
        }
    }

    // estimated cost of one channel layout (sum of planned subframe bits)
    [[nodiscard]] crd::u64 layout_cost(crd::memory::IAllocator* alloc, const crd::i64* a, const crd::i64* b,
                                       crd::usize n, crd::u32 bps_a, crd::u32 bps_b, SubframePlan& pa,
                                       SubframePlan& pb)
    {
        pa = plan_subframe(alloc, a, n, bps_a);
        pb = plan_subframe(alloc, b, n, bps_b);
        return pa.bits + pb.bits;
    }
} // namespace

crd::containers::Array<crd::u8> flac_encode(const AudioPcm& pcm, crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> out(alloc);
    if (!pcm.valid() || pcm.is_float()) { return out; }
    if (pcm.bits_per_sample != 16 && pcm.bits_per_sample != 24) { return out; }
    if (pcm.channels != 1 && pcm.channels != 2) { return out; }
    if (pcm.sample_rate >= (1U << 20U)) { return out; } // STREAMINFO's 20-bit rate field is the format's cap

    const crd::u32 bps       = pcm.bits_per_sample;
    const crd::u32 nch       = pcm.channels;
    const crd::u64 total     = pcm.frame_count();
    const crd::u32 bytes_per = bps / 8U;

    // MD5 of the unencoded PCM (little-endian bytes, interleaved)
    detail::Md5 md5;
    for (crd::i32 v : pcm.isamples)
    {
        crd::u8 le[4];
        for (crd::u32 b = 0; b < bytes_per; ++b)
        {
            le[b] = static_cast<crd::u8>((static_cast<crd::u32>(v) >> (8U * b)) & 0xFFU);
        }
        md5.update(le, bytes_per);
    }
    crd::u8 digest[16];
    md5.final(digest);

    // "fLaC" + STREAMINFO (the only metadata block, is-last set)
    out.push_back('f');
    out.push_back('L');
    out.push_back('a');
    out.push_back('C');
    out.push_back(0x80); // last block, type 0
    out.push_back(0);
    out.push_back(0);
    out.push_back(34);
    {
        BitWriter bw{&out, 0, 0};
        bw.write_bits(kBlockSize, 16); // min block
        bw.write_bits(kBlockSize, 16); // max block
        bw.write_bits(0, 24);          // min frame size unknown
        bw.write_bits(0, 24);          // max frame size unknown
        bw.write_bits(pcm.sample_rate, 20);
        bw.write_bits(nch - 1, 3);
        bw.write_bits(bps - 1, 5);
        bw.write_bits(total, 36);
    }
    for (crd::u8 b : digest) { out.push_back(b); }

    // frames
    crd::containers::Array<crd::i64> cha(alloc);
    crd::containers::Array<crd::i64> chb(alloc);
    crd::containers::Array<crd::i64> mid(alloc);
    crd::containers::Array<crd::i64> side(alloc);
    cha.resize(kBlockSize);
    chb.resize(kBlockSize);
    mid.resize(kBlockSize);
    side.resize(kBlockSize);

    crd::u64 frame_index = 0;
    for (crd::u64 start = 0; start < total; start += kBlockSize, ++frame_index)
    {
        const crd::u32 block = static_cast<crd::u32>(total - start < kBlockSize ? total - start : kBlockSize);
        for (crd::u32 i = 0; i < block; ++i)
        {
            cha[i] = pcm.isamples[(start + i) * nch];
            if (nch == 2)
            {
                chb[i]  = pcm.isamples[(start + i) * nch + 1];
                mid[i]  = (cha[i] + chb[i]) >> 1U;
                side[i] = cha[i] - chb[i];
            }
        }

        // pick the channel layout by estimated bits
        crd::u32     assignment = nch == 1 ? 0U : 1U; // mono / independent stereo
        SubframePlan plan_a;
        SubframePlan plan_b;
        const crd::i64* sub_a = cha.data();
        const crd::i64* sub_b = chb.data();
        crd::u32        bps_a = bps;
        crd::u32        bps_b = bps;
        if (nch == 1) { plan_a = plan_subframe(alloc, cha.data(), block, bps); }
        else
        {
            SubframePlan   ia;
            SubframePlan   ib;
            const crd::u64 cost_ind = layout_cost(alloc, cha.data(), chb.data(), block, bps, bps, ia, ib);
            SubframePlan   la;
            SubframePlan   ls;
            const crd::u64 cost_ls = layout_cost(alloc, cha.data(), side.data(), block, bps, bps + 1, la, ls);
            SubframePlan   rs;
            SubframePlan   rb;
            const crd::u64 cost_rs = layout_cost(alloc, side.data(), chb.data(), block, bps + 1, bps, rs, rb);
            SubframePlan   ma;
            SubframePlan   ms;
            const crd::u64 cost_ms = layout_cost(alloc, mid.data(), side.data(), block, bps, bps + 1, ma, ms);

            // pick the winner, then set the COMPLETE layout — a branch chain that mutates the four
            // (sub, bps) fields cumulatively leaves stale halves when two branches fire (the desync bug
            // this comment replaces: L/S then R/S left subframe B pointing at side @25)
            crd::u64 best = cost_ind;
            assignment    = 1;
            if (cost_ls < best)
            {
                best       = cost_ls;
                assignment = 8;
            }
            if (cost_rs < best)
            {
                best       = cost_rs;
                assignment = 9;
            }
            if (cost_ms < best) { assignment = 10; }
            switch (assignment)
            {
            case 8: // L/S
                plan_a = la;
                plan_b = ls;
                sub_a  = cha.data();
                sub_b  = side.data();
                bps_a  = bps;
                bps_b  = bps + 1;
                break;
            case 9: // R/S (side first)
                plan_a = rs;
                plan_b = rb;
                sub_a  = side.data();
                sub_b  = chb.data();
                bps_a  = bps + 1;
                bps_b  = bps;
                break;
            case 10: // M/S
                plan_a = ma;
                plan_b = ms;
                sub_a  = mid.data();
                sub_b  = side.data();
                bps_a  = bps;
                bps_b  = bps + 1;
                break;
            default: // independent
                plan_a = ia;
                plan_b = ib;
                sub_a  = cha.data();
                sub_b  = chb.data();
                bps_a  = bps;
                bps_b  = bps;
                break;
            }
        }

        // frame bytes into a scratch (CRC-16 runs over them)
        crd::containers::Array<crd::u8> frame(alloc);
        {
            BitWriter bw{&frame, 0, 0};
            bw.write_bits(0x3FFE, 14);
            bw.write_bits(0, 1); // reserved
            bw.write_bits(0, 1); // fixed blocking
            bw.write_bits(7, 4); // block size: 16-bit-1 at end
            bw.write_bits(0, 4); // rate: from STREAMINFO
            bw.write_bits(nch == 1 ? 0U : assignment, 4);
            bw.write_bits(bps == 16 ? 4U : 6U, 3);
            bw.write_bits(0, 1); // reserved
            bw.write_utf8(frame_index);
            bw.write_bits(block - 1, 16);
            // header CRC-8 (the writer is byte-aligned here by construction)
            crd::u8 crc = 0;
            for (crd::u8 b : frame) { crc = detail::crc8_update(crc, b); }
            bw.write_bits(crc, 8);

            write_subframe(bw, alloc, sub_a, block, bps_a, plan_a);
            if (nch == 2) { write_subframe(bw, alloc, sub_b, block, bps_b, plan_b); }
            bw.align_zero();
            crd::u16 crc16 = 0;
            for (crd::u8 b : frame) { crc16 = detail::crc16_update(crc16, b); }
            bw.write_bits(crc16, 16);
        }
        for (crd::u8 b : frame) { out.push_back(b); }
    }
    return out;
}

} // namespace crd::audio
