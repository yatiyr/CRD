// The hesap-stats kernels of crd/hesap/stats/stats_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the block
// functions and the log core included, so no function of this unit is shared with general code or with another level.
// The portable bodies are the scalar loops (Threefry three blocks at a time); the SSE2 bodies run 16 Philox blocks or
// seven Threefry blocks (four in vector registers beside three scalar ones) or four Student-t lanes in two registers
// at a time, and the AVX2 bodies 24 Philox blocks, 12 Threefry blocks or four Student-t lanes. Every body gives the
// same bits: the generators are integer arithmetic, and every Student-t body keeps four lane sums in the same order,
// with single-rounded FMAs and the log core that crd_log4 and crd_log_x2 reproduce lane for lane.

#include <crd/core/platform.hpp>
#include <crd/hesap/stats/counter_rng_core.hpp>
#include <crd/hesap/stats/philox_blocks.hpp>
#include <crd/hesap/stats/stats_kernels.hpp>
#include <crd/math/exp_log_core.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/transcendental.hpp>

#include <cstring>

namespace crd::hesap::stats::inline CRD_MATH_LEVEL_NS
{
namespace
{

#if CRD_SIMD_HAS_AVX2
struct alignas(32) ByteShuffle
{
    unsigned char index[32];
};

// The vpshufb indices that rotate each 64-bit lane left by S bytes: result byte i is byte (i - S) mod 8.
template <int S> constexpr ByteShuffle byte_rotation() noexcept
{
    ByteShuffle shuffle{};
    for (int k = 0; k < 32; ++k)
    {
        const int in_lane = k & 15; // vpshufb indexes within each 128-bit half
        shuffle.index[k] = static_cast<unsigned char>((in_lane & 8) + (((in_lane & 7) - S) & 7));
    }
    return shuffle;
}

template <int S> constexpr ByteShuffle kByteRotation = byte_rotation<S>();

// Four Threefry blocks per group: lane j of word register w holds word w of block j.
struct ThreefryAvx2
{
    using V = __m256i;
    static constexpr int kLanes = 4;

    static V add(V a, V b) noexcept { return _mm256_add_epi64(a, b); }
    static V bxor(V a, V b) noexcept { return _mm256_xor_si256(a, b); }
    static V set1(crd::u64 w) noexcept { return _mm256_set1_epi64x(static_cast<long long>(w)); }
    static V iota() noexcept { return _mm256_set_epi64x(3, 2, 1, 0); }
    static V load(const crd::u64* w) noexcept { return _mm256_loadu_si256(reinterpret_cast<const __m256i*>(w)); }

    // Each 64-bit lane rotated left by R bits: one dword or byte shuffle where R is a multiple of 8.
    template <int R> static V rotl(V x) noexcept
    {
        if constexpr (R == 32)
        {
            return _mm256_shuffle_epi32(x, 0xB1);
        }
        else if constexpr (R % 8 == 0)
        {
            return _mm256_shuffle_epi8(x,
                                       _mm256_load_si256(reinterpret_cast<const __m256i*>(kByteRotation<R / 8>.index)));
        }
        else
        {
            return _mm256_or_si256(_mm256_slli_epi64(x, R), _mm256_srli_epi64(x, 64 - R));
        }
    }

    // Transpose 4x4 u64 (lanes to blocks) and store the 16 words in block order.
    static void store(const V (&x)[4], crd::u64* out) noexcept
    {
        const __m256i t0 = _mm256_unpacklo_epi64(x[0], x[1]);
        const __m256i t1 = _mm256_unpackhi_epi64(x[0], x[1]);
        const __m256i t2 = _mm256_unpacklo_epi64(x[2], x[3]);
        const __m256i t3 = _mm256_unpackhi_epi64(x[2], x[3]);
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 0), _mm256_permute2x128_si256(t0, t2, 0x20));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 4), _mm256_permute2x128_si256(t1, t3, 0x20));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), _mm256_permute2x128_si256(t0, t2, 0x31));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 12), _mm256_permute2x128_si256(t1, t3, 0x31));
    }
};
#elif CRD_SIMD_HAS_SSE2
// Two Threefry blocks per group: lane j of word register w holds word w of block j.
struct ThreefrySse2
{
    using V = __m128i;
    static constexpr int kLanes = 2;

    static V add(V a, V b) noexcept { return _mm_add_epi64(a, b); }
    static V bxor(V a, V b) noexcept { return _mm_xor_si128(a, b); }
    static V set1(crd::u64 w) noexcept { return _mm_set1_epi64x(static_cast<long long>(w)); }
    static V iota() noexcept { return _mm_set_epi64x(1, 0); }
    static V load(const crd::u64* w) noexcept { return _mm_loadu_si128(reinterpret_cast<const __m128i*>(w)); }

    // Each 64-bit lane rotated left by R bits: a dword shuffle for 32, word shuffles for 16 (no byte shuffle before
    // SSSE3).
    template <int R> static V rotl(V x) noexcept
    {
        if constexpr (R == 32)
        {
            return _mm_shuffle_epi32(x, 0xB1);
        }
        else if constexpr (R == 16)
        {
            return _mm_shufflehi_epi16(_mm_shufflelo_epi16(x, 0x93), 0x93);
        }
        else
        {
            return _mm_or_si128(_mm_slli_epi64(x, R), _mm_srli_epi64(x, 64 - R));
        }
    }

    // The two blocks' words in block order: 8 words at out.
    static void store(const V (&x)[4], crd::u64* out) noexcept
    {
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 0), _mm_unpacklo_epi64(x[0], x[1]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 2), _mm_unpacklo_epi64(x[2], x[3]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 4), _mm_unpackhi_epi64(x[0], x[1]));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(out + 6), _mm_unpackhi_epi64(x[2], x[3]));
    }
};
#endif

// Block ctr + n's counter (256-bit) plus the key: the first key injection. Word 0 alone moves unless it carries.
CRD_FORCEINLINE void threefry_block_start(const crd::u64 (&ctr)[4], crd::u64 n, const crd::u64 (&ks)[5],
                                          crd::u64 (&out)[4]) noexcept
{
    if (ctr[0] <= ~crd::u64{0} - n)
    {
        out[0] = ctr[0] + n + ks[0];
        for (int w = 1; w < 4; ++w)
        {
            out[w] = ctr[w] + ks[w];
        }
        return;
    }
    crd::u64 c[4] = {ctr[0], ctr[1], ctr[2], ctr[3]};
    threefry_counter_add(c, n);
    for (int w = 0; w < 4; ++w)
    {
        out[w] = c[w] + ks[w];
    }
}

#if CRD_SIMD_HAS_SSE2
// A group's word registers for blocks ctr + first .. ctr + first + kLanes - 1 with the first key injection added;
// the lanes take their own 256-bit counters where word 0 carries among them.
template <class Isa>
CRD_FORCEINLINE void threefry_load(typename Isa::V (&x)[4], const crd::u64 (&ctr)[4], crd::u64 first,
                                   const crd::u64 (&ks)[5]) noexcept
{
    if (ctr[0] <= ~crd::u64{0} - (first + static_cast<crd::u64>(Isa::kLanes - 1)))
    {
        x[0] = Isa::add(Isa::set1(ctr[0] + first + ks[0]), Isa::iota());
        for (int w = 1; w < 4; ++w)
        {
            x[w] = Isa::set1(ctr[w] + ks[w]);
        }
        return;
    }
    crd::u64 words[4][Isa::kLanes];
    for (int j = 0; j < Isa::kLanes; ++j)
    {
        crd::u64 lane[4];
        threefry_block_start(ctr, first + static_cast<crd::u64>(j), ks, lane);
        for (int w = 0; w < 4; ++w)
        {
            words[w][j] = lane[w];
        }
    }
    for (int w = 0; w < 4; ++w)
    {
        x[w] = Isa::load(words[w]);
    }
}

// One round on G vector groups, as threefry_round does on scalar blocks.
template <class Isa, int M, int G> void threefry_vround(typename Isa::V (&x)[G][4]) noexcept
{
    for (int g = 0; g < G; ++g)
    {
        x[g][0] = Isa::add(x[g][0], x[g][1]);
        const typename Isa::V x1 = Isa::bxor(Isa::template rotl<kThreefryRot[M][0]>(x[g][1]), x[g][0]);
        x[g][2] = Isa::add(x[g][2], x[g][3]);
        x[g][1] = Isa::bxor(Isa::template rotl<kThreefryRot[M][1]>(x[g][3]), x[g][2]);
        x[g][3] = x1;
    }
}

// The scalar blocks beside a step's vector groups (one unused block where there are none).
template <int GS> using ThreefryScalarBlocks = crd::u64[GS > 0 ? GS : 1][4];

// One round on G vector groups and GS scalar blocks side by side, so the vector and the scalar ports both work.
template <class Isa, int M, int G, int GS>
void threefry_round_both(typename Isa::V (&v)[G][4], ThreefryScalarBlocks<GS>& x) noexcept
{
    threefry_vround<Isa, M, G>(v);
    if constexpr (GS > 0)
    {
        threefry_round<M, GS>(x);
    }
}

// Four rounds from round M mod 8 on both, then key injection S.
template <class Isa, int M, int S, int G, int GS>
void threefry_rounds_both(typename Isa::V (&v)[G][4], ThreefryScalarBlocks<GS>& x, const crd::u64 (&ks)[5]) noexcept
{
    threefry_round_both<Isa, M, G, GS>(v, x);
    threefry_round_both<Isa, M + 1, G, GS>(v, x);
    threefry_round_both<Isa, M + 2, G, GS>(v, x);
    threefry_round_both<Isa, M + 3, G, GS>(v, x);
    const typename Isa::V k[4] = {Isa::set1(ks[S % 5]), Isa::set1(ks[(S + 1) % 5]), Isa::set1(ks[(S + 2) % 5]),
                                  Isa::set1(ks[(S + 3) % 5] + static_cast<crd::u64>(S))};
    for (int g = 0; g < G; ++g)
    {
        for (int w = 0; w < 4; ++w)
        {
            v[g][w] = Isa::add(v[g][w], k[w]);
        }
    }
    if constexpr (GS > 0)
    {
        threefry_inject<S, GS>(x, ks);
    }
}

// Blocks ctr .. ctr + G kLanes + GS - 1 in block order at out: G vector groups, then GS scalar blocks; ctr advances
// past them.
template <class Isa, int G, int GS>
CRD_FORCEINLINE void threefry_step(const crd::u64 (&ks)[5], crd::u64 (&ctr)[4], crd::u64* out) noexcept
{
    constexpr auto lanes = static_cast<crd::u64>(Isa::kLanes);
    constexpr crd::u64 vector_blocks = lanes * static_cast<crd::u64>(G);
    typename Isa::V v[G][4];
    for (int g = 0; g < G; ++g)
    {
        threefry_load<Isa>(v[g], ctr, lanes * static_cast<crd::u64>(g), ks);
    }
    ThreefryScalarBlocks<GS> x{};
    if constexpr (GS > 0)
    {
        for (int g = 0; g < GS; ++g)
        {
            threefry_block_start(ctr, vector_blocks + static_cast<crd::u64>(g), ks, x[g]);
        }
    }
    threefry_counter_add(ctr, vector_blocks + static_cast<crd::u64>(GS));
    threefry_rounds_both<Isa, 0, 1, G, GS>(v, x, ks);
    threefry_rounds_both<Isa, 4, 2, G, GS>(v, x, ks);
    threefry_rounds_both<Isa, 0, 3, G, GS>(v, x, ks);
    threefry_rounds_both<Isa, 4, 4, G, GS>(v, x, ks);
    threefry_rounds_both<Isa, 0, 5, G, GS>(v, x, ks);
    for (int g = 0; g < G; ++g)
    {
        Isa::store(v[g], out + 4 * Isa::kLanes * g);
    }
    if constexpr (GS > 0)
    {
        std::memcpy(out + 4U * vector_blocks, x, sizeof x);
    }
}
#endif

// Blocks ctr .. ctr + G - 1 by the scalar block function on G independent blocks; ctr advances past them.
template <int G>
CRD_FORCEINLINE void threefry_scalar_step(const crd::u64 (&ks)[5], crd::u64 (&ctr)[4], crd::u64* out) noexcept
{
    crd::u64 x[G][4];
    for (int g = 0; g < G; ++g)
    {
        threefry_block_start(ctr, static_cast<crd::u64>(g), ks, x[g]);
    }
    threefry_counter_add(ctr, static_cast<crd::u64>(G));
    threefry4x64_rounds_of<G>(x, ks);
    std::memcpy(out, x, sizeof x);
}

// The vector bodies take whole steps (AVX2: three groups of four blocks, then one group; SSE2: two groups of two
// beside three scalar blocks), then the scalar body three blocks at a time and one at a time.
void threefry4x64_blocks(const crd::u64 (&key)[4], const crd::u64 (&counter)[4], crd::u64* out,
                         crd::usize blocks) noexcept
{
    crd::u64 ks[5] = {};
    threefry_key_schedule(key, ks);
    crd::u64 ctr[4] = {counter[0], counter[1], counter[2], counter[3]};
    crd::usize b = 0;
#if CRD_SIMD_HAS_AVX2
    for (; b + 12U <= blocks; b += 12U)
    {
        threefry_step<ThreefryAvx2, 3, 0>(ks, ctr, out + 4U * b);
    }
    for (; b + 4U <= blocks; b += 4U)
    {
        threefry_step<ThreefryAvx2, 1, 0>(ks, ctr, out + 4U * b);
    }
#elif CRD_SIMD_HAS_SSE2
    for (; b + 7U <= blocks; b += 7U)
    {
        threefry_step<ThreefrySse2, 2, 3>(ks, ctr, out + 4U * b);
    }
#endif
    for (; b + 3U <= blocks; b += 3U)
    {
        threefry_scalar_step<3>(ks, ctr, out + 4U * b);
    }
    for (; b < blocks; ++b)
    {
        threefry_scalar_step<1>(ks, ctr, out + 4U * b);
    }
}

// One element's term: -log(fma(x^2, 1/nu, 1))/2 + (k x^2) / (nu + x^2), k = (nu + 1) / (2 nu).
crd::f64 student_t_term(crd::f64 x, crd::f64 nu, crd::f64 inv_nu, crd::f64 k) noexcept
{
    const crd::f64 x2 = x * x;
    const crd::f64 log_u = crd::math::crd_log1(crd::math::libm::fma(x2, inv_nu, 1.0));
    return crd::math::libm::fma(-0.5, log_u, (k * x2) / (nu + x2));
}

crd::f64 student_t_nu_score_sum(const crd::f64* xs, crd::usize n, crd::f64 nu) noexcept
{
    const crd::f64 inv_nu = 1.0 / nu; // x^2/nu by a multiply: no division per element
    const crd::f64 k = (nu + 1.0) / (2.0 * nu);
    crd::usize i = 0;
    crd::f64 lanes[4] = {0.0, 0.0, 0.0, 0.0};
#if CRD_SIMD_HAS_AVX2
    const __m256d vnu = _mm256_set1_pd(nu);
    const __m256d vone = _mm256_set1_pd(1.0);
    const __m256d vinv_nu = _mm256_set1_pd(inv_nu);
    const __m256d vneg_half = _mm256_set1_pd(-0.5);
    const __m256d vk = _mm256_set1_pd(k);
    __m256d acc = _mm256_setzero_pd();
    for (; i + 4U <= n; i += 4U)
    {
        const __m256d vx = _mm256_loadu_pd(xs + i);
        const __m256d vx2 = _mm256_mul_pd(vx, vx);
        const __m256d vlog = crd::math::crd_log4(_mm256_fmadd_pd(vx2, vinv_nu, vone));
        const __m256d vrat = _mm256_div_pd(_mm256_mul_pd(vk, vx2), _mm256_add_pd(vnu, vx2));
        acc = _mm256_add_pd(acc, _mm256_fmadd_pd(vneg_half, vlog, vrat));
    }
    _mm256_storeu_pd(lanes, acc);
#elif CRD_SIMD_HAS_SSE2
    // Lanes 0 and 1 in one register, 2 and 3 in the other: element i still adds to lane i % 4.
    const __m128d vnu = _mm_set1_pd(nu);
    const __m128d vone = _mm_set1_pd(1.0);
    const __m128d vinv_nu = _mm_set1_pd(inv_nu);
    const __m128d vneg_half = _mm_set1_pd(-0.5);
    const __m128d vk = _mm_set1_pd(k);
    const auto term = [&](__m128d vx) noexcept
    {
        const __m128d vx2 = _mm_mul_pd(vx, vx);
        const __m128d vlog = crd::math::crd_log_x2(crd::math::simd::detail::fma_pd(vx2, vinv_nu, vone));
        const __m128d vrat = _mm_div_pd(_mm_mul_pd(vk, vx2), _mm_add_pd(vnu, vx2));
        return crd::math::simd::detail::fma_pd(vneg_half, vlog, vrat);
    };
    __m128d acc_lo = _mm_setzero_pd();
    __m128d acc_hi = _mm_setzero_pd();
    for (; i + 4U <= n; i += 4U)
    {
        acc_lo = _mm_add_pd(acc_lo, term(_mm_loadu_pd(xs + i)));
        acc_hi = _mm_add_pd(acc_hi, term(_mm_loadu_pd(xs + i + 2U)));
    }
    _mm_storeu_pd(lanes, acc_lo);
    _mm_storeu_pd(lanes + 2, acc_hi);
#else
    for (; i + 4U <= n; i += 4U)
    {
        for (crd::usize l = 0; l < 4U; ++l)
        {
            lanes[l] += student_t_term(xs[i + l], nu, inv_nu, k);
        }
    }
#endif
    crd::f64 s = (lanes[0] + lanes[1]) + (lanes[2] + lanes[3]);
    for (; i < n; ++i)
    {
        s += student_t_term(xs[i], nu, inv_nu, k);
    }
    return s;
}

} // namespace

} // namespace crd::hesap::stats::inline CRD_MATH_LEVEL_NS

namespace crd::hesap::stats
{

const StatsKernels CRD_KERNEL_TABLE(kStatsKernels) = {
    &CRD_MATH_LEVEL_NS::philox4x32_fill_blocks,
    &CRD_MATH_LEVEL_NS::threefry4x64_blocks,
    &CRD_MATH_LEVEL_NS::student_t_nu_score_sum,
};

} // namespace crd::hesap::stats
