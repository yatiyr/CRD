// OWN.1's before/after board for the biquad section step moved onto hesap-dsp and the Clough-Tocher walk's orientation
// moved onto crd-math: each case times the moved code (`owner`: the audio module's apply_biquad and hesap-dsp's
// sosfilt_stream, both now on biquad_step, and crd-math's 2D cross) and the copy it replaced (`reference`, written out
// here as it was) over the same inputs in one process, alternating which runs first, so scripts/ab-board.py reads both
// arms from one run. A line is `<case> <arm> <ns per item>`.

#include <crd/audio/audio_kernels.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/hesap/dsp/filtering.hpp>
#include <crd/hesap/dsp/rbj.hpp>
#include <crd/math/vec.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <chrono>
#include <cstdio>

using crd::f32;
using crd::f64;
using crd::i64;
using crd::u64;
using crd::usize;
using crd::hesap::dsp::Biquad;
using crd::hesap::dsp::BiquadState;
using crd::math::Vec2;

namespace
{
volatile f64 g_sink = 0.0;

constexpr usize kItems = 4096;
constexpr usize kSections = 4;
constexpr auto kLowpass = static_cast<crd::u8>(crd::audio::BiquadType::Lowpass);

// The audio module's loop before the move (apply_biquad with its own DF2T recurrence and z1/z2 arrays).
void reference_apply_biquad(f32* bus, i64 len, const Biquad<f64>& bq, f64 z1[2], f64 z2[2])
{
    for (i64 i = 0; i < len; ++i)
    {
        for (int c = 0; c < 2; ++c)
        {
            const f64 x = static_cast<f64>(bus[i * 2 + c]);
            const f64 y = bq.b0 * x + z1[c];
            z1[c] = bq.b1 * x - bq.a1 * y + z2[c];
            z2[c] = bq.b2 * x - bq.a2 * y;
            bus[i * 2 + c] = static_cast<f32>(y);
        }
    }
}

// sosfilt_stream before the move, as filtering.hpp wrote it (the recurrence inline per section, the same spans and the
// section count read at run time), so the two arms differ only by the section step.
void reference_sosfilt(const crd::hesap::dsp::SecondOrderSections<f64>& sos, crd::containers::ConstSpan<f64> x,
                       crd::containers::Span<f64> out, crd::containers::Span<BiquadState<f64>> state) noexcept
{
    const usize ns = sos.sections.size();
    for (usize i = 0; i < x.size(); ++i)
    {
        f64 xn = x[i];
        for (usize s = 0; s < ns; ++s)
        {
            const Biquad<f64>& bq = sos.sections[s];
            BiquadState<f64>& st = state[s];
            const f64 xnew = bq.b0 * xn + st.z1;
            st.z1 = bq.b1 * xn - bq.a1 * xnew + st.z2;
            st.z2 = bq.b2 * xn - bq.a2 * xnew;
            xn = xnew;
        }
        out[i] = xn;
    }
}

// The Clough-Tocher walk's orientation before the move (fast_orient).
f64 reference_fast_orient(const Vec2<f64>& a, const Vec2<f64>& b, const Vec2<f64>& c)
{
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Nanoseconds per item of `body()` over kItems inputs, the median of five timed runs after a warm one, each long
// enough (~20 ms) that the clock's resolution does not show.
template <typename Body> double ns_per_item(Body body)
{
    body();
    usize reps = 1U;
    for (;;)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        const double elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count();
        if (elapsed >= 2.0e7)
        {
            break;
        }
        reps *= 2U;
    }
    double runs[5] = {};
    for (double& run : runs)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (usize r = 0; r < reps; ++r)
        {
            body();
        }
        run = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
              (static_cast<double>(reps) * static_cast<double>(kItems));
    }
    for (usize i = 1; i < 5U; ++i) // insertion sort of five
    {
        for (usize j = i; j > 0U && runs[j - 1U] > runs[j]; --j)
        {
            const double t = runs[j];
            runs[j] = runs[j - 1U];
            runs[j - 1U] = t;
        }
    }
    return runs[2];
}

// The arms run in alternating order from case to case, so in-process drift lands on both.
bool g_owner_first = true;

template <typename Owner, typename Reference> void row(const char* name, Owner owner, Reference reference)
{
    double owner_ns = 0.0;
    double reference_ns = 0.0;
    if (g_owner_first)
    {
        owner_ns = ns_per_item(owner);
        reference_ns = ns_per_item(reference);
    }
    else
    {
        reference_ns = ns_per_item(reference);
        owner_ns = ns_per_item(owner);
    }
    g_owner_first = !g_owner_first;
    std::printf("%s owner %.4f\n%s reference %.4f\n", name, owner_ns, name, reference_ns);
}

f64 unit_draw(u64& state)
{
    return static_cast<f64>(crd::splitmix64_next(state) >> 11U) * 0x1.0p-52 - 1.0;
}

// The inputs, drawn once; the arrays are static so the stack stays small. Each biquad arm filters a fresh copy of the
// same block so the state stays bounded and both arms see the same samples.
f32 g_stereo[kItems * 2];
f32 g_bus[kItems * 2];
f64 g_mono[kItems];
f64 g_out[kItems];
Vec2<f64> g_points[3][kItems];
} // namespace

int main()
{
    u64 state = 0xB1'0AD5'0E1EULL;
    for (f32& v : g_stereo)
    {
        v = static_cast<f32>(unit_draw(state) * 0.5);
    }
    for (f64& v : g_mono)
    {
        v = unit_draw(state) * 0.5;
    }
    for (auto& points : g_points)
    {
        for (Vec2<f64>& p : points)
        {
            p = Vec2<f64>{unit_draw(state), unit_draw(state)};
        }
    }

    crd::memory::TlsfAllocator alloc(1U << 20U);
    crd::hesap::dsp::SecondOrderSections<f64> sos(&alloc);
    const f64 cutoffs[kSections] = {0.05, 0.1, 0.2, 0.3};
    for (const f64 cutoff : cutoffs)
    {
        sos.sections.push_back(crd::hesap::dsp::rbj_lowpass<f64>(cutoff, 0.7071));
    }

    std::printf("# OWN.1 biquad section step (hesap-dsp) and walk orientation (crd-math): owner vs the moved copies "
                "(reference), ns per item\n");
    row(
        "audio-biquad-stereo",
        []
        {
            for (usize i = 0; i < kItems * 2U; ++i)
            {
                g_bus[i] = g_stereo[i];
            }
            BiquadState<f64> st[2] = {};
            crd::audio::apply_biquad(g_bus, static_cast<i64>(kItems), kLowpass, 0.125F, 0.7071F, st);
            g_sink = static_cast<f64>(g_bus[kItems]);
        },
        []
        {
            for (usize i = 0; i < kItems * 2U; ++i)
            {
                g_bus[i] = g_stereo[i];
            }
            // the copy designed its coefficients on every call too
            const Biquad<f64> lowpass =
                crd::hesap::dsp::rbj_lowpass<f64>(static_cast<f64>(0.125F), static_cast<f64>(0.7071F));
            f64 z1[2] = {};
            f64 z2[2] = {};
            reference_apply_biquad(g_bus, static_cast<i64>(kItems), lowpass, z1, z2);
            g_sink = static_cast<f64>(g_bus[kItems]);
        });
    row(
        "sosfilt-4-sections",
        [&sos]
        {
            BiquadState<f64> st[kSections] = {};
            crd::hesap::dsp::sosfilt_stream<f64>(sos, crd::containers::ConstSpan<f64>(g_mono, kItems),
                                                 crd::containers::Span<f64>(g_out, kItems),
                                                 crd::containers::Span<BiquadState<f64>>(st, kSections));
            g_sink = g_out[kItems - 1U];
        },
        [&sos]
        {
            BiquadState<f64> st[kSections] = {};
            reference_sosfilt(sos, crd::containers::ConstSpan<f64>(g_mono, kItems),
                              crd::containers::Span<f64>(g_out, kItems),
                              crd::containers::Span<BiquadState<f64>>(st, kSections));
            g_sink = g_out[kItems - 1U];
        });
    row(
        "walk-orient-f64",
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec2<f64>& a = g_points[0][i];
                const Vec2<f64>& b = g_points[1][i];
                const Vec2<f64>& q = g_points[2][i];
                acc += crd::math::cross(b - a, q - a) + crd::math::cross(q - b, a - b);
            }
            g_sink = acc;
        },
        []
        {
            f64 acc = 0.0;
            for (usize i = 0; i < kItems; ++i)
            {
                const Vec2<f64>& a = g_points[0][i];
                const Vec2<f64>& b = g_points[1][i];
                const Vec2<f64>& q = g_points[2][i];
                acc += reference_fast_orient(a, b, q) + reference_fast_orient(b, q, a);
            }
            g_sink = acc;
        });
    return 0;
}
