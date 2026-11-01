// The hardware check's compute arm (crd/perf/hardware_check_kernels.hpp), compiled by crd_add_cpu_kernels
// (../CMakeLists.txt) at portable, sse2, avx2, avx512 and neon; crd/core/kernel_level.hpp names the unit's level. One
// generic loop over the level's lanes: each group of lanes runs every step in registers, then the next group.

#include <crd/core/kernel_level.hpp>
#include <crd/core/types.hpp>
#include <crd/perf/hardware_check_kernels.hpp>

#include <cstring>

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2 || CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2 ||                          \
    CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512
#include <immintrin.h>
#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON
#include <arm_neon.h>
#endif

namespace crd::perf::inline CRD_MATH_LEVEL_NS
{
namespace
{

constexpr crd::u32 kMultiplier = 0x9E3779B1U;

#if CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_SSE2

struct Lanes
{
    using I = __m128i;
    using D = __m128d;
    static constexpr crd::u32 kWidth = 2U;

    static I load(const crd::u64* p) noexcept { return _mm_load_si128(reinterpret_cast<const __m128i*>(p)); }
    static D load(const double* p) noexcept { return _mm_load_pd(p); }
    static void store(crd::u64* p, I v) noexcept { _mm_store_si128(reinterpret_cast<__m128i*>(p), v); }
    static void store(double* p, D v) noexcept { _mm_store_pd(p, v); }
    static I splat_multiplier() noexcept { return _mm_set1_epi64x(kMultiplier); }
    static D splat(double v) noexcept { return _mm_set1_pd(v); }
    static I mul_lo32(I x, I m) noexcept { return _mm_mul_epu32(x, m); }
    template <int N> static I shr(I x) noexcept { return _mm_srli_epi64(x, N); }
    template <int N> static I shl(I x) noexcept { return _mm_slli_epi64(x, N); }
    static I add(I a, I b) noexcept { return _mm_add_epi64(a, b); }
    static I bxor(I a, I b) noexcept { return _mm_xor_si128(a, b); }
    static I bor(I a, I b) noexcept { return _mm_or_si128(a, b); }
    static D add(D a, D b) noexcept { return _mm_add_pd(a, b); }
    static D sub(D a, D b) noexcept { return _mm_sub_pd(a, b); }
    static D mul(D a, D b) noexcept { return _mm_mul_pd(a, b); }
    static D div(D a, D b) noexcept { return _mm_div_pd(a, b); }
    static I bits(D v) noexcept { return _mm_castpd_si128(v); }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX2

struct Lanes
{
    using I = __m256i;
    using D = __m256d;
    static constexpr crd::u32 kWidth = 4U;

    static I load(const crd::u64* p) noexcept { return _mm256_load_si256(reinterpret_cast<const __m256i*>(p)); }
    static D load(const double* p) noexcept { return _mm256_load_pd(p); }
    static void store(crd::u64* p, I v) noexcept { _mm256_store_si256(reinterpret_cast<__m256i*>(p), v); }
    static void store(double* p, D v) noexcept { _mm256_store_pd(p, v); }
    static I splat_multiplier() noexcept { return _mm256_set1_epi64x(kMultiplier); }
    static D splat(double v) noexcept { return _mm256_set1_pd(v); }
    static I mul_lo32(I x, I m) noexcept { return _mm256_mul_epu32(x, m); }
    template <int N> static I shr(I x) noexcept { return _mm256_srli_epi64(x, N); }
    template <int N> static I shl(I x) noexcept { return _mm256_slli_epi64(x, N); }
    static I add(I a, I b) noexcept { return _mm256_add_epi64(a, b); }
    static I bxor(I a, I b) noexcept { return _mm256_xor_si256(a, b); }
    static I bor(I a, I b) noexcept { return _mm256_or_si256(a, b); }
    static D add(D a, D b) noexcept { return _mm256_add_pd(a, b); }
    static D sub(D a, D b) noexcept { return _mm256_sub_pd(a, b); }
    static D mul(D a, D b) noexcept { return _mm256_mul_pd(a, b); }
    static D div(D a, D b) noexcept { return _mm256_div_pd(a, b); }
    static I bits(D v) noexcept { return _mm256_castpd_si256(v); }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_AVX512

struct Lanes
{
    using I = __m512i;
    using D = __m512d;
    static constexpr crd::u32 kWidth = 8U;

    static I load(const crd::u64* p) noexcept { return _mm512_load_si512(p); }
    static D load(const double* p) noexcept { return _mm512_load_pd(p); }
    static void store(crd::u64* p, I v) noexcept { _mm512_store_si512(p, v); }
    static void store(double* p, D v) noexcept { _mm512_store_pd(p, v); }
    static I splat_multiplier() noexcept { return _mm512_set1_epi64(kMultiplier); }
    static D splat(double v) noexcept { return _mm512_set1_pd(v); }
    static I mul_lo32(I x, I m) noexcept { return _mm512_mul_epu32(x, m); }
    template <int N> static I shr(I x) noexcept { return _mm512_srli_epi64(x, N); }
    template <int N> static I shl(I x) noexcept { return _mm512_slli_epi64(x, N); }
    static I add(I a, I b) noexcept { return _mm512_add_epi64(a, b); }
    static I bxor(I a, I b) noexcept { return _mm512_xor_si512(a, b); }
    static I bor(I a, I b) noexcept { return _mm512_or_si512(a, b); }
    static D add(D a, D b) noexcept { return _mm512_add_pd(a, b); }
    static D sub(D a, D b) noexcept { return _mm512_sub_pd(a, b); }
    static D mul(D a, D b) noexcept { return _mm512_mul_pd(a, b); }
    static D div(D a, D b) noexcept { return _mm512_div_pd(a, b); }
    static I bits(D v) noexcept { return _mm512_castpd_si512(v); }
};

#elif CRD_KERNEL_LEVEL == CRD_KERNEL_LEVEL_NEON

struct Lanes
{
    using I = uint64x2_t;
    using D = float64x2_t;
    static constexpr crd::u32 kWidth = 2U;

    static I load(const crd::u64* p) noexcept { return vld1q_u64(p); }
    static D load(const double* p) noexcept { return vld1q_f64(p); }
    static void store(crd::u64* p, I v) noexcept { vst1q_u64(p, v); }
    static void store(double* p, D v) noexcept { vst1q_f64(p, v); }
    static I splat_multiplier() noexcept { return vdupq_n_u64(kMultiplier); }
    static D splat(double v) noexcept { return vdupq_n_f64(v); }
    static I mul_lo32(I x, I m) noexcept { return vmull_u32(vmovn_u64(x), vmovn_u64(m)); }
    template <int N> static I shr(I x) noexcept { return vshrq_n_u64(x, N); }
    template <int N> static I shl(I x) noexcept { return vshlq_n_u64(x, N); }
    static I add(I a, I b) noexcept { return vaddq_u64(a, b); }
    static I bxor(I a, I b) noexcept { return veorq_u64(a, b); }
    static I bor(I a, I b) noexcept { return vorrq_u64(a, b); }
    static D add(D a, D b) noexcept { return vaddq_f64(a, b); }
    static D sub(D a, D b) noexcept { return vsubq_f64(a, b); }
    static D mul(D a, D b) noexcept { return vmulq_f64(a, b); }
    static D div(D a, D b) noexcept { return vdivq_f64(a, b); }
    static I bits(D v) noexcept { return vreinterpretq_u64_f64(v); }
};

#else

struct Lanes
{
    using I = crd::u64;
    using D = double;
    static constexpr crd::u32 kWidth = 1U;

    static I load(const crd::u64* p) noexcept { return *p; }
    static D load(const double* p) noexcept { return *p; }
    static void store(crd::u64* p, I v) noexcept { *p = v; }
    static void store(double* p, D v) noexcept { *p = v; }
    static I splat_multiplier() noexcept { return kMultiplier; }
    static D splat(double v) noexcept { return v; }
    static I mul_lo32(I x, I m) noexcept { return (x & 0xFFFFFFFFU) * m; }
    template <int N> static I shr(I x) noexcept { return x >> N; }
    template <int N> static I shl(I x) noexcept { return x << N; }
    static I add(I a, I b) noexcept { return a + b; }
    static I bxor(I a, I b) noexcept { return a ^ b; }
    static I bor(I a, I b) noexcept { return a | b; }
    static D add(D a, D b) noexcept { return a + b; }
    static D sub(D a, D b) noexcept { return a - b; }
    static D mul(D a, D b) noexcept { return a * b; }
    static D div(D a, D b) noexcept { return a / b; }
    static I bits(D v) noexcept
    {
        I out = 0U;
        std::memcpy(&out, &v, sizeof(out));
        return out;
    }
};

#endif

static_assert(kHardwareComputeLanes % Lanes::kWidth == 0U, "the lanes split into whole vectors");

void compute(HardwareComputeState& state, crd::u32 steps) noexcept
{
    const Lanes::I multiplier = Lanes::splat_multiplier();
    const Lanes::D one = Lanes::splat(1.0);
    const Lanes::D three = Lanes::splat(3.0);
    const Lanes::D rate = Lanes::splat(3.99);
    for (crd::u32 lane = 0U; lane < kHardwareComputeLanes; lane += Lanes::kWidth)
    {
        Lanes::I x = Lanes::load(state.x + lane);
        Lanes::D y = Lanes::load(state.y + lane);
        Lanes::D z = Lanes::load(state.z + lane);
        Lanes::I c = Lanes::load(state.c + lane);
        for (crd::u32 step = 0U; step < steps; ++step)
        {
            x = Lanes::add(Lanes::mul_lo32(x, multiplier), Lanes::bxor(Lanes::shr<29>(x), Lanes::shl<7>(x)));
            y = Lanes::mul(rate, Lanes::mul(y, Lanes::sub(one, y)));
            z = Lanes::div(Lanes::add(y, three), Lanes::add(z, one));
            c = Lanes::bxor(Lanes::bor(Lanes::shl<1>(c), Lanes::shr<63>(c)),
                            Lanes::add(Lanes::bits(y), Lanes::bits(z)));
        }
        Lanes::store(state.x + lane, x);
        Lanes::store(state.y + lane, y);
        Lanes::store(state.z + lane, z);
        Lanes::store(state.c + lane, c);
    }
}

} // namespace
} // namespace crd::perf::inline CRD_MATH_LEVEL_NS

namespace crd::perf
{

const HardwareCheckKernels CRD_KERNEL_TABLE(kHardwareCheckKernels) = {&CRD_MATH_LEVEL_NS::compute};

} // namespace crd::perf
