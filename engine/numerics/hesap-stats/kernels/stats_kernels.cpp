// The hesap-stats kernels of crd/hesap/stats/stats_kernels.hpp, compiled once per CPU level by crd_add_cpu_kernels
// (../CMakeLists.txt, ADR-0134 §5). Everything here lives in the level's namespace (CRD_MATH_LEVEL_NS), the block
// functions and the log core included, so no function of this unit is shared with general code or with another level.
// The portable bodies are the scalar loops; the SSE2 body runs 16 Philox blocks at a time, and the AVX2 bodies 24
// Philox blocks, four Threefry blocks or four Student-t lanes. Every body gives the same bits: the generators are
// integer arithmetic, and both Student-t bodies keep four lane sums in the same order, with single-rounded FMAs and the
// log core that crd_log4 reproduces lane for lane.

#include <crd/hesap/stats/counter_rng_core.hpp>
#include <crd/hesap/stats/philox_blocks.hpp>
#include <crd/hesap/stats/stats_kernels.hpp>
#include <crd/math/exp_log_core.hpp>
#include <crd/math/libm.hpp>
#include <crd/math/simd/transcendental.hpp>

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

// Each 64-bit lane of x rotated left by R bits: one dword or byte shuffle where R is a multiple of 8.
template <int R> __m256i threefry_rotl4(__m256i x) noexcept
{
    if constexpr (R == 32)
    {
        return _mm256_shuffle_epi32(x, 0xB1);
    }
    else if constexpr (R % 8 == 0)
    {
        return _mm256_shuffle_epi8(x, _mm256_load_si256(reinterpret_cast<const __m256i*>(kByteRotation<R / 8>.index)));
    }
    else
    {
        return _mm256_or_si256(_mm256_slli_epi64(x, R), _mm256_srli_epi64(x, 64 - R));
    }
}

struct Threefry4
{
    __m256i x0;
    __m256i x1;
    __m256i x2;
    __m256i x3;
};

// One Threefry round with the rotation pair of round M mod 8, then the word permutation (swap x1 and x3).
template <int M> void threefry_round4(Threefry4& x) noexcept
{
    x.x0 = _mm256_add_epi64(x.x0, x.x1);
    const __m256i x1 = _mm256_xor_si256(threefry_rotl4<kThreefryRot[M][0]>(x.x1), x.x0);
    x.x2 = _mm256_add_epi64(x.x2, x.x3);
    x.x1 = _mm256_xor_si256(threefry_rotl4<kThreefryRot[M][1]>(x.x3), x.x2);
    x.x3 = x1;
}

// Four rounds from round M mod 8, then key injection number S.
template <int M, int S> void threefry_rounds4(Threefry4& x, const crd::u64 (&ks)[5]) noexcept
{
    threefry_round4<M>(x);
    threefry_round4<M + 1>(x);
    threefry_round4<M + 2>(x);
    threefry_round4<M + 3>(x);
    x.x0 = _mm256_add_epi64(x.x0, _mm256_set1_epi64x(static_cast<long long>(ks[S % 5])));
    x.x1 = _mm256_add_epi64(x.x1, _mm256_set1_epi64x(static_cast<long long>(ks[(S + 1) % 5])));
    x.x2 = _mm256_add_epi64(x.x2, _mm256_set1_epi64x(static_cast<long long>(ks[(S + 2) % 5])));
    const crd::u64 k3 = ks[(S + 3) % 5] + static_cast<crd::u64>(S);
    x.x3 = _mm256_add_epi64(x.x3, _mm256_set1_epi64x(static_cast<long long>(k3)));
}

__m256i threefry_word4(const crd::u64 (&lanes)[4][4], int word, crd::u64 key_word) noexcept
{
    return _mm256_add_epi64(
        _mm256_set_epi64x(static_cast<long long>(lanes[3][word]), static_cast<long long>(lanes[2][word]),
                          static_cast<long long>(lanes[1][word]), static_cast<long long>(lanes[0][word])),
        _mm256_set1_epi64x(static_cast<long long>(key_word)));
}

// Threefry blocks counter .. counter + 3: lane j holds block counter + j, each with its own 256-bit counter.
void threefry4(const crd::u64 (&ks)[5], const crd::u64 (&counter)[4], crd::u64* out) noexcept
{
    Threefry4 x;
    if (counter[0] <= ~crd::u64{0} - 3U) // no carry out of word 0 among the four lanes
    {
        x.x0 = _mm256_add_epi64(_mm256_set1_epi64x(static_cast<long long>(counter[0] + ks[0])),
                                _mm256_set_epi64x(3, 2, 1, 0));
        x.x1 = _mm256_set1_epi64x(static_cast<long long>(counter[1] + ks[1]));
        x.x2 = _mm256_set1_epi64x(static_cast<long long>(counter[2] + ks[2]));
        x.x3 = _mm256_set1_epi64x(static_cast<long long>(counter[3] + ks[3]));
    }
    else
    {
        crd::u64 lanes[4][4];
        for (int j = 0; j < 4; ++j)
        {
            for (int w = 0; w < 4; ++w)
            {
                lanes[j][w] = counter[w];
            }
            threefry_counter_add(lanes[j], static_cast<crd::u64>(j));
        }
        x.x0 = threefry_word4(lanes, 0, ks[0]);
        x.x1 = threefry_word4(lanes, 1, ks[1]);
        x.x2 = threefry_word4(lanes, 2, ks[2]);
        x.x3 = threefry_word4(lanes, 3, ks[3]);
    }
    threefry_rounds4<0, 1>(x, ks);
    threefry_rounds4<4, 2>(x, ks);
    threefry_rounds4<0, 3>(x, ks);
    threefry_rounds4<4, 4>(x, ks);
    threefry_rounds4<0, 5>(x, ks);
    // Transpose 4x4 u64 (lanes to blocks) and store the 16 words in block order.
    const __m256i t0 = _mm256_unpacklo_epi64(x.x0, x.x1);
    const __m256i t1 = _mm256_unpackhi_epi64(x.x0, x.x1);
    const __m256i t2 = _mm256_unpacklo_epi64(x.x2, x.x3);
    const __m256i t3 = _mm256_unpackhi_epi64(x.x2, x.x3);
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 0), _mm256_permute2x128_si256(t0, t2, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 4), _mm256_permute2x128_si256(t1, t3, 0x20));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 8), _mm256_permute2x128_si256(t0, t2, 0x31));
    _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + 12), _mm256_permute2x128_si256(t1, t3, 0x31));
}
#endif

void threefry4x64_blocks(const crd::u64 (&key)[4], const crd::u64 (&counter)[4], crd::u64* out,
                         crd::usize blocks) noexcept
{
    crd::u64 ctr[4] = {counter[0], counter[1], counter[2], counter[3]};
    crd::usize b = 0;
#if CRD_SIMD_HAS_AVX2
    const crd::u64 ks[5] = {key[0], key[1], key[2], key[3], kThreefryParity ^ key[0] ^ key[1] ^ key[2] ^ key[3]};
    for (; b + 4U <= blocks; b += 4U)
    {
        threefry4(ks, ctr, out + 4U * b);
        threefry_counter_add(ctr, 4U);
    }
#endif
    for (; b < blocks; ++b)
    {
        const ThreefryBlock block = threefry4x64(ctr, key);
        for (int w = 0; w < 4; ++w)
        {
            out[4U * b + static_cast<crd::usize>(w)] = block.v[w];
        }
        threefry_counter_add(ctr, 1U);
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
