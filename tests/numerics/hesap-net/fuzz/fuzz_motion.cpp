// crd-fuzz-hesap-motion: hesap-net's interpolation, extrapolation, rewind history, dead reckoning and projective
// velocity blending on any snapshot, time and limit a peer's words can reach.
//
// Input. Op bytes, each followed by its arguments; a short input reads zeros.
//
// Oracles, none of them the kernels' own verdict:
//   * a component batch: each result the expression of crd/hesap/net/motion.hpp restated here, at every level the CPU
//     runs, NaN only where the expression gives NaN, in place too;
//   * nlerp: the restated expression at every level, finite and unit length for any input, the identity when nothing
//     is left to normalise;
//   * slerp: the restated series at every level, and for unit inputs within reach of the true arc;
//   * a rewind history: every stamp, slot, sample and refusal against a linear search over a plain list of stamps.
//   * dead reckoning: any columns give portable's bits at every level, in place too, with nothing written past a batch
//   or
//     into a column the algorithm does not move; columns a real entity could hold follow the motion they mean, by
//     integrating the true rotation and by libm's sine and cosine, with no R1 or R2 in the oracle;
//   * projective blending: both outputs the restated expressions at every level, nothing past a batch.
#include <crd/fuzz/harness.hpp>
#include <crd/hesap/net/dead_reckoning.hpp>
#include <crd/hesap/net/motion.hpp>
#include <crd/hesap/net/rewind.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>

namespace
{
using crd::f32;
using crd::f64;
using crd::i64;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using namespace crd::hesap::net;

constexpr usize kMaxBatch = 37U;
constexpr f32 kInf = std::numeric_limits<f32>::infinity();
constexpr f32 kEdges[] = {0.0F,
                          -0.0F,
                          1.0F,
                          -1.0F,
                          0.5F,
                          0.25F,
                          1024.0F,
                          -1024.0F,
                          1e-40F,
                          kInf,
                          -kInf,
                          std::numeric_limits<f32>::quiet_NaN(),
                          std::numeric_limits<f32>::max(),
                          std::numeric_limits<f32>::lowest(),
                          1.1754944e-38F,
                          0.70710678F};

struct Reader
{
    const u8* data;
    usize size;

    [[nodiscard]] u8 next() noexcept
    {
        if (size == 0U)
        {
            return 0U;
        }
        --size;
        return *data++;
    }

    [[nodiscard]] u32 next_u32() noexcept
    {
        u32 value = 0U;
        for (u32 i = 0U; i < 4U; ++i)
        {
            value |= u32{next()} << (8U * i);
        }
        return value;
    }

    [[nodiscard]] u64 next_u64() noexcept
    {
        const u64 low = next_u32();
        return low | (u64{next_u32()} << 32U);
    }

    // A byte names a value at an edge; any other reads a float's bits.
    [[nodiscard]] f32 next_float() noexcept
    {
        const u8 pick = next();
        if (pick < std::size(kEdges))
        {
            return kEdges[pick];
        }
        return std::bit_cast<f32>(next_u32());
    }

    template <class T>
    [[nodiscard]] T next_value() noexcept
    {
        if constexpr (sizeof(T) == sizeof(f32))
        {
            return next_float();
        }
        else
        {
            const u8 pick = next();
            if (pick < std::size(kEdges))
            {
                return static_cast<f64>(kEdges[pick]);
            }
            return std::bit_cast<f64>(next_u64());
        }
    }
};

template <typename Check> void for_each_level(const Check& check)
{
    const crd::CpuSelection& selection = crd::cpu_selection();
    const crd::CpuLevel cap = crd::kernel_level_cap();
    for (const KernelLevel<MotionKernels>& level : motion_kernel_levels())
    {
        if (crd::kernel_level_eligible(level, selection.selected, selection.kernel_features, cap))
        {
            check(*level.table);
        }
    }
}

template <class T>
[[nodiscard]] bool same_value(T a, T b) noexcept
{
    return std::memcmp(&a, &b, sizeof(T)) == 0 || (std::isnan(a) && std::isnan(b));
}

template <class T>
void component_batches(Reader& in)
{
    const usize count = in.next() % (kMaxBatch + 1U);
    T a[kMaxBatch] = {};
    T b[kMaxBatch] = {};
    T c[kMaxBatch] = {};
    T d[kMaxBatch] = {};
    for (usize i = 0U; i < count; ++i)
    {
        a[i] = in.next_value<T>();
        b[i] = in.next_value<T>();
        c[i] = in.next_value<T>();
        d[i] = in.next_value<T>();
    }
    const T t = in.next_value<T>();
    const T width = in.next_value<T>();

    const T t2 = t * t;
    const T t3 = t2 * t;
    const T h1 = (static_cast<T>(2) * t3) - (static_cast<T>(3) * t2) + static_cast<T>(1);
    const T h2 = (static_cast<T>(-2) * t3) + (static_cast<T>(3) * t2);
    const T h3 = t3 - (static_cast<T>(2) * t2) + t;
    const T h4 = t3 - t2;
    const T half = static_cast<T>(0.5) * t * t;

    for_each_level(
        [&](const MotionKernels& table)
        {
            const auto lerp_of = [&](const T* x, const T* y, usize n, T* out) noexcept
            {
                if constexpr (sizeof(T) == sizeof(f32))
                {
                    table.lerp_f32(x, y, t, n, out);
                }
                else
                {
                    table.lerp_f64(x, y, t, n, out);
                }
            };
            const auto hermite_of = [&](usize n, T* out) noexcept
            {
                if constexpr (sizeof(T) == sizeof(f32))
                {
                    table.hermite_f32({a, b, c, d, t, width}, n, out);
                }
                else
                {
                    table.hermite_f64({a, b, c, d, t, width}, n, out);
                }
            };
            const auto extrapolate_of = [&](const T* acceleration, usize n, T* out) noexcept
            {
                if constexpr (sizeof(T) == sizeof(f32))
                {
                    table.extrapolate_f32(a, b, acceleration, t, n, out);
                }
                else
                {
                    table.extrapolate_f64(a, b, acceleration, t, n, out);
                }
            };

            // A guard past the batch catches a write beyond its count at every level.
            T out[kMaxBatch + 1U];
            const T guard = static_cast<T>(-77.25);
            out[count] = guard;
            lerp_of(a, b, count, out);
            for (usize i = 0U; i < count; ++i)
            {
                CRD_FUZZ_REQUIRE(same_value<T>(out[i], a[i] + ((b[i] - a[i]) * t)));
            }
            CRD_FUZZ_REQUIRE(out[count] == guard);
            T in_place[kMaxBatch];
            std::memcpy(in_place, a, sizeof(a));
            lerp_of(in_place, b, count, in_place);
            for (usize i = 0U; i < count; ++i)
            {
                CRD_FUZZ_REQUIRE(same_value<T>(in_place[i], out[i]));
            }

            hermite_of(count, out);
            for (usize i = 0U; i < count; ++i)
            {
                const T want = (h1 * a[i]) + (h2 * c[i]) + (h3 * (b[i] * width)) + (h4 * (d[i] * width));
                CRD_FUZZ_REQUIRE(same_value<T>(out[i], want));
            }
            CRD_FUZZ_REQUIRE(out[count] == guard);

            extrapolate_of(nullptr, count, out);
            for (usize i = 0U; i < count; ++i)
            {
                CRD_FUZZ_REQUIRE(same_value<T>(out[i], a[i] + (b[i] * t)));
            }
            extrapolate_of(c, count, out);
            for (usize i = 0U; i < count; ++i)
            {
                CRD_FUZZ_REQUIRE(same_value<T>(out[i], a[i] + (b[i] * t) + (c[i] * half)));
            }
            CRD_FUZZ_REQUIRE(out[count] == guard);
        });
}

[[nodiscard]] f32 dot_of(const f32* a, const f32* b) noexcept
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

// The restated nlerp; `length2` is the chord's square.
void nlerp_of(const f32* a, const f32* b, f32 t, f32* out, f32& length2) noexcept
{
    const bool same_side = dot_of(a, b) >= 0.0F;
    f32 chord[4];
    for (usize i = 0U; i < 4U; ++i)
    {
        chord[i] = a[i] + (((same_side ? b[i] : -b[i]) - a[i]) * t);
    }
    length2 = dot_of(chord, chord);
    if (!(length2 > 0.0F && length2 < kInf))
    {
        out[0] = 0.0F;
        out[1] = 0.0F;
        out[2] = 0.0F;
        out[3] = 1.0F;
        return;
    }
    const f32 inverse = 1.0F / std::sqrt(length2);
    for (usize i = 0U; i < 4U; ++i)
    {
        out[i] = chord[i] * inverse;
    }
}

[[nodiscard]] f32 ratio_of(f32 t, f32 x) noexcept
{
    const f32 square = t * t;
    f32 product = 1.0F;
    for (u32 i = 16U; i > 0U; --i)
    {
        const auto index = static_cast<f32>(i);
        const f32 odd = (2.0F * index) + 1.0F;
        const f32 scale = (i == 16U) ? 1.902F : 1.0F;
        product = 1.0F + ((((((scale / (index * odd)) * square) - ((scale * index) / odd))) * x) * product);
    }
    return t * product;
}

[[nodiscard]] f32 cut_to_unit(f32 raw) noexcept
{
    if (!(raw >= 0.0F))
    {
        return 0.0F;
    }
    return (raw <= 1.0F) ? raw : 1.0F;
}

void slerp_of(const f32* a, const f32* b, f32 raw, f32* out) noexcept
{
    const f32 t = cut_to_unit(raw);
    const f32 cosine = dot_of(a, b);
    const bool same_side = cosine >= 0.0F;
    const f32 magnitude = same_side ? cosine : -cosine;
    const f32 x = ((magnitude <= 1.0F) ? magnitude : 1.0F) - 1.0F;
    const f32 from_a = ratio_of(1.0F - t, x);
    const f32 ratio = ratio_of(t, x);
    const f32 from_b = same_side ? ratio : -ratio;
    for (usize i = 0U; i < 4U; ++i)
    {
        out[i] = (from_a * a[i]) + (from_b * b[i]);
    }
}

void quaternion_batches(Reader& in, bool normalise_inputs)
{
    const usize count = in.next() % (kMaxBatch + 1U);
    f32 a[4U * kMaxBatch] = {};
    f32 b[4U * kMaxBatch] = {};
    for (usize i = 0U; i < 4U * count; ++i)
    {
        a[i] = in.next_float();
        b[i] = in.next_float();
    }
    if (normalise_inputs)
    {
        for (usize i = 0U; i < count; ++i)
        {
            const f32 q[4] = {0.0F, 0.0F, 0.0F, 1.0F};
            f32 unused = 0.0F;
            nlerp_of(a + (4U * i), q, 0.0F, a + (4U * i), unused);
            nlerp_of(b + (4U * i), q, 0.0F, b + (4U * i), unused);
        }
    }
    const f32 t = in.next_float();
    for_each_level(
        [&](const MotionKernels& table)
        {
            f32 out[(4U * kMaxBatch) + 1U];
            constexpr f32 guard = -77.25F;
            out[4U * count] = guard;
            table.nlerp(a, b, t, count, out);
            CRD_FUZZ_REQUIRE(out[4U * count] == guard);
            for (usize i = 0U; i < count; ++i)
            {
                f32 want[4];
                f32 length2 = 0.0F;
                nlerp_of(a + (4U * i), b + (4U * i), t, want, length2);
                for (usize field = 0U; field < 4U; ++field)
                {
                    CRD_FUZZ_REQUIRE(same_value<f32>(out[(4U * i) + field], want[field]));
                    CRD_FUZZ_REQUIRE(std::isfinite(want[field]));
                }
                // Unit once the chord's square is a normal float, whose rounding keeps the length; the identity
                // when nothing is left to normalise.
                const f32 length = dot_of(want, want);
                CRD_FUZZ_REQUIRE(length2 < 1.1754944e-38F || !(length2 < kInf) || std::fabs(length - 1.0F) < 1e-5F);
                CRD_FUZZ_REQUIRE(length2 > 0.0F || (want[3] == 1.0F && want[0] == 0.0F));
                CRD_FUZZ_REQUIRE(length2 < kInf || (want[3] == 1.0F && want[0] == 0.0F));
            }
            table.slerp(a, b, t, count, out);
            CRD_FUZZ_REQUIRE(out[4U * count] == guard);
            for (usize i = 0U; i < count; ++i)
            {
                f32 want[4];
                slerp_of(a + (4U * i), b + (4U * i), t, want);
                for (usize field = 0U; field < 4U; ++field)
                {
                    CRD_FUZZ_REQUIRE(same_value<f32>(out[(4U * i) + field], want[field]));
                }
                if (!normalise_inputs)
                {
                    continue;
                }

                // Unit inputs: the true arc, by the libm in double, is near.
                const f64 cosine = std::fmin(std::fabs(static_cast<f64>(dot_of(a + (4U * i), b + (4U * i)))), 1.0);
                const f64 sign = (dot_of(a + (4U * i), b + (4U * i)) >= 0.0F) ? 1.0 : -1.0;
                const f64 angle = std::acos(cosine);
                const auto at = static_cast<f64>(cut_to_unit(t));
                const bool tiny = angle < 1e-3;
                const f64 sine = tiny ? 1.0 : std::sin(angle);
                const f64 from_a = tiny ? 1.0 - at : std::sin((1.0 - at) * angle) / sine;
                const f64 from_b = sign * (tiny ? at : std::sin(at * angle) / sine);
                for (usize field = 0U; field < 4U; ++field)
                {
                    const f64 arc = (from_a * static_cast<f64>(a[(4U * i) + field])) +
                                    (from_b * static_cast<f64>(b[(4U * i) + field]));
                    CRD_FUZZ_REQUIRE(std::fabs(static_cast<f64>(want[field]) - arc) < 1e-5);
                }
            }
        });
}

// The rewind history next to the plain list of what it must hold.
void rewind_history(Reader& in)
{
    static constexpr u32 kCapacity = 5U;
    RewindHistory<kCapacity> history;
    i64 stamps[kCapacity];
    u32 slots[kCapacity];
    u32 held = 0U;
    u32 next_slot = 0U;
    const u32 ops = (in.next() % 40U) + 1U;
    for (u32 op = 0U; op < ops; ++op)
    {
        const u8 kind = in.next();
        const u64 word = in.next_u64();
        const i64 value = ((kind & 2U) != 0U) ? std::bit_cast<i64>(word) : static_cast<i64>(word & 0xFFFFU) - 0x8000;
        if ((kind & 1U) == 0U)
        {
            u32 slot = 0xFFFFFFFFU;
            const bool accepted = history.push(value, slot);
            CRD_FUZZ_REQUIRE(accepted == (held == 0U || value > stamps[held - 1U]));
            if (!accepted)
            {
                CRD_FUZZ_REQUIRE(slot == 0xFFFFFFFFU);
                continue;
            }
            CRD_FUZZ_REQUIRE(slot == next_slot);
            next_slot = (next_slot + 1U == kCapacity) ? 0U : next_slot + 1U;
            if (held == kCapacity)
            {
                std::memmove(stamps, stamps + 1U, (kCapacity - 1U) * sizeof(i64));
                std::memmove(slots, slots + 1U, (kCapacity - 1U) * sizeof(u32));
                --held;
            }
            stamps[held] = value;
            slots[held] = slot;
            ++held;
            continue;
        }
        const RewindLimits limits{((kind & 4U) != 0U) ? std::bit_cast<i64>(in.next_u64())
                                                       : static_cast<i64>(in.next_u32() & 0xFFFFU),
                                  ((kind & 8U) != 0U) ? std::bit_cast<i64>(in.next_u64())
                                                      : static_cast<i64>(in.next_u32() & 0xFFFFU)};
        const RewindSample sample = history.sample(value, limits);
        CRD_FUZZ_REQUIRE(history.size() == held);
        if (held == 0U)
        {
            CRD_FUZZ_REQUIRE(sample.place == RewindPlace::Empty);
            continue;
        }
        CRD_FUZZ_REQUIRE(sample.place != RewindPlace::Empty);
        const i64 newest = stamps[held - 1U];
        const i64 oldest = stamps[0];
        // The floor in unsigned arithmetic, restated: the later of the oldest stamp and the window's start.
        i64 floor = oldest;
        if (limits.max_rewind <= 0)
        {
            floor = newest;
        }
        else if (std::bit_cast<u64>(newest) - std::bit_cast<u64>(oldest) > static_cast<u64>(limits.max_rewind))
        {
            floor = std::bit_cast<i64>(std::bit_cast<u64>(newest) - static_cast<u64>(limits.max_rewind));
        }
        i64 want_time = value;
        RewindMoved want_moved = RewindMoved::No;
        if (value > newest)
        {
            want_time = newest;
            want_moved = RewindMoved::Future;
        }
        else if (value < floor)
        {
            want_time = floor;
            want_moved = RewindMoved::TooOld;
        }
        CRD_FUZZ_REQUIRE(sample.time == want_time);
        CRD_FUZZ_REQUIRE(sample.moved == want_moved);
        u32 below = 0U;
        for (u32 i = 0U; i < held; ++i)
        {
            if (stamps[i] <= want_time)
            {
                below = i;
            }
        }
        CRD_FUZZ_REQUIRE(sample.older == slots[below]);
        CRD_FUZZ_REQUIRE(sample.t >= 0.0 && sample.t <= 1.0);
        if (stamps[below] == want_time)
        {
            CRD_FUZZ_REQUIRE(sample.place == RewindPlace::Exact && sample.newer == slots[below] && sample.t == 0.0);
            continue;
        }
        CRD_FUZZ_REQUIRE(below + 1U < held && sample.newer == slots[below + 1U]);
        const u64 width = std::bit_cast<u64>(stamps[below + 1U]) - std::bit_cast<u64>(stamps[below]);
        if (limits.max_gap < 0 || width > static_cast<u64>(limits.max_gap))
        {
            CRD_FUZZ_REQUIRE(sample.place == RewindPlace::Gap && sample.t == 0.0);
        }
        else
        {
            CRD_FUZZ_REQUIRE(sample.place == RewindPlace::Between);
        }
    }
}

// ---- Dead reckoning: hostile columns at every level, and bounded ones against the motion they mean ----

struct Fleet
{
    f64 columns[17][kMaxBatch];
    f64 out[10][kMaxBatch + 1U];
};

[[nodiscard]] DeadReckoningInput input_of(const Fleet& fleet) noexcept
{
    const auto& c = fleet.columns;
    return {{c[0], c[1], c[2]},   {c[3], c[4], c[5]},           {c[6], c[7], c[8]},
            {c[9], c[10], c[11]}, {c[12], c[13], c[14], c[15]}, c[16]};
}

[[nodiscard]] DeadReckoningOutput output_of(f64 (&o)[10][kMaxBatch + 1U]) noexcept
{
    return {{o[0], o[1], o[2]}, {o[3], o[4], o[5]}, {o[6], o[7], o[8], o[9]}};
}

[[nodiscard]] bool algorithm_rotates(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rpw || a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Rpb ||
           a == DeadReckoningAlgorithm::Rvb;
}

[[nodiscard]] bool algorithm_accelerates(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Rvw || a == DeadReckoningAlgorithm::Fvw || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

[[nodiscard]] bool algorithm_on_body(DeadReckoningAlgorithm a) noexcept
{
    return a == DeadReckoningAlgorithm::Fpb || a == DeadReckoningAlgorithm::Rpb || a == DeadReckoningAlgorithm::Rvb ||
           a == DeadReckoningAlgorithm::Fvb;
}

// x y z of a rotation of `v` by q, body to world.
void rotate_into(const f64* q, const f64* v, f64* out) noexcept
{
    const f64 tx = 2.0 * ((q[1] * v[2]) - (q[2] * v[1]));
    const f64 ty = 2.0 * ((q[2] * v[0]) - (q[0] * v[2]));
    const f64 tz = 2.0 * ((q[0] * v[1]) - (q[1] * v[0]));
    out[0] = v[0] + (q[3] * tx) + ((q[1] * tz) - (q[2] * ty));
    out[1] = v[1] + (q[3] * ty) + ((q[2] * tx) - (q[0] * tz));
    out[2] = v[2] + (q[3] * tz) + ((q[0] * ty) - (q[1] * tx));
}

// The displacement in the body's first axes, by integrating the true rotation: no R1 or R2 in it.
void integrated(const f64* w, const f64* v, const f64* a, f64 dt, f64* out) noexcept
{
    const f64 speed = std::sqrt((w[0] * w[0]) + (w[1] * w[1]) + (w[2] * w[2]));
    const auto panels = static_cast<u32>(8.0 + (4.0 * speed * std::fabs(dt)));
    const f64 h = dt / static_cast<f64>(panels);
    constexpr f64 node = 0.7745966692414834;
    constexpr f64 outer_weight = 5.0 / 18.0;
    constexpr f64 middle_weight = 8.0 / 18.0;
    out[0] = 0.0;
    out[1] = 0.0;
    out[2] = 0.0;
    const auto add = [&](f64 tau, f64 weight) noexcept
    {
        const f64 half = speed * tau * 0.5;
        const f64 scale = (speed > 0.0) ? std::sin(half) / speed : 0.0;
        const f64 q[4] = {w[0] * scale, w[1] * scale, w[2] * scale, std::cos(half)};
        const f64 at[3] = {v[0] + (a[0] * tau), v[1] + (a[1] * tau), v[2] + (a[2] * tau)};
        f64 turned[3];
        rotate_into(q, at, turned);
        for (usize k = 0U; k < 3U; ++k)
        {
            out[k] += turned[k] * weight * h;
        }
    };
    for (u32 i = 0U; i < panels; ++i)
    {
        const f64 middle = (static_cast<f64>(i) + 0.5) * h;
        add(middle - (node * h * 0.5), outer_weight);
        add(middle, middle_weight);
        add(middle + (node * h * 0.5), outer_weight);
    }
}

void hold_to_motion(const Fleet& fleet, DeadReckoningAlgorithm algorithm, usize count,
                    const f64 (&o)[10][kMaxBatch + 1U])
{
    const auto& c = fleet.columns;
    const bool body = algorithm_on_body(algorithm);
    const bool turns = algorithm_rotates(algorithm);
    const bool speeds = algorithm_accelerates(algorithm);
    for (usize i = 0U; i < count; ++i)
    {
        const f64 dt = c[16][i];
        const f64 v[3] = {c[3][i], c[4][i], c[5][i]};
        const f64 a[3] = {speeds ? c[6][i] : 0.0, speeds ? c[7][i] : 0.0, speeds ? c[8][i] : 0.0};
        const f64 none[3] = {0.0, 0.0, 0.0};
        const f64 w[3] = {c[9][i], c[10][i], c[11][i]};
        const f64 q[4] = {c[12][i], c[13][i], c[14][i], c[15][i]};
        f64 moved[3];
        if (body)
        {
            f64 local[3];
            integrated(turns ? w : none, v, a, dt, local);
            rotate_into(q, local, moved);
        }
        else
        {
            for (usize k = 0U; k < 3U; ++k)
            {
                moved[k] = (v[k] * dt) + (a[k] * (0.5 * dt * dt));
            }
        }
        for (usize k = 0U; k < 3U; ++k)
        {
            CRD_FUZZ_REQUIRE(std::fabs(o[k][i] - (c[k][i] + moved[k])) < 1e-7);
        }
        if (turns)
        {
            const f64 unit = (o[6][i] * o[6][i]) + (o[7][i] * o[7][i]) + (o[8][i] * o[8][i]) + (o[9][i] * o[9][i]);
            CRD_FUZZ_REQUIRE(std::fabs(unit - 1.0) < 1e-13);
            // The same turn by libm's sine and cosine.
            const f64 speed = std::sqrt((w[0] * w[0]) + (w[1] * w[1]) + (w[2] * w[2]));
            const f64 half = speed * dt * 0.5;
            const f64 scale = (speed > 0.0) ? std::sin(half) / speed : dt * 0.5;
            const f64 turn[4] = {w[0] * scale, w[1] * scale, w[2] * scale, std::cos(half)};
            const f64 expected[4] = {(q[3] * turn[0]) + (q[0] * turn[3]) + (q[1] * turn[2]) - (q[2] * turn[1]),
                                     (q[3] * turn[1]) - (q[0] * turn[2]) + (q[1] * turn[3]) + (q[2] * turn[0]),
                                     (q[3] * turn[2]) + (q[0] * turn[1]) - (q[1] * turn[0]) + (q[2] * turn[3]),
                                     (q[3] * turn[3]) - (q[0] * turn[0]) - (q[1] * turn[1]) - (q[2] * turn[2])};
            for (usize k = 0U; k < 4U; ++k)
            {
                CRD_FUZZ_REQUIRE(std::fabs(o[6U + k][i] - expected[k]) < 1e-10);
            }
        }
        if (speeds)
        {
            for (usize k = 0U; k < 3U; ++k)
            {
                CRD_FUZZ_REQUIRE(same_value(o[3U + k][i], c[3U + k][i] + (c[6U + k][i] * dt)));
            }
        }
    }
}

void dead_reckoning(Reader& in, bool bounded)
{
    const u8 number = in.next();
    const usize count = in.next() % (kMaxBatch + 1U);
    Fleet fleet{};
    for (usize i = 0U; i < kMaxBatch; ++i)
    {
        for (usize k = 0U; k < 17U; ++k)
        {
            fleet.columns[k][i] = in.size != 0U ? in.next_value<f64>() : 0.0;
        }
        if (bounded)
        {
            // Any word becomes a value a real entity holds: a unit quaternion, and speeds and times in range.
            const auto cut = [](f64 value, f64 limit) noexcept
            {
                if (!std::isfinite(value))
                {
                    return 0.0;
                }
                return std::fmod(value, limit);
            };
            for (usize k = 0U; k < 9U; ++k)
            {
                fleet.columns[k][i] = cut(fleet.columns[k][i], 1000.0);
            }
            for (usize k = 9U; k < 12U; ++k)
            {
                fleet.columns[k][i] = cut(fleet.columns[k][i], 16.0);
            }
            fleet.columns[16][i] = cut(fleet.columns[16][i], 4.0);
            f64 length2 = 0.0;
            for (usize k = 12U; k < 16U; ++k)
            {
                fleet.columns[k][i] = cut(fleet.columns[k][i], 1.0);
                length2 += fleet.columns[k][i] * fleet.columns[k][i];
            }
            if (length2 < 1e-6)
            {
                length2 += 1.0 - (fleet.columns[15][i] * fleet.columns[15][i]);
                fleet.columns[15][i] = 1.0;
            }
            const f64 scale = 1.0 / std::sqrt(length2);
            for (usize k = 12U; k < 16U; ++k)
            {
                fleet.columns[k][i] *= scale;
            }
        }
    }
    if (!dead_reckoning_algorithm_known(number))
    {
        return;
    }
    const auto algorithm = static_cast<DeadReckoningAlgorithm>(number);
    const DeadReckoningInput columns = input_of(fleet);
    static f64 reference[10][kMaxBatch + 1U];
    for (auto& column : reference)
    {
        std::fill_n(column, kMaxBatch + 1U, -1.5);
    }
    kMotionKernelsPortable.dead_reckon(algorithm, columns, count, output_of(reference));
    if (bounded && algorithm != DeadReckoningAlgorithm::Static)
    {
        hold_to_motion(fleet, algorithm, count, reference);
    }
    for (usize k = 0U; k < 10U; ++k)
    {
        const bool written =
            k < 3U || (k < 6U && algorithm_accelerates(algorithm)) || (k >= 6U && algorithm_rotates(algorithm));
        for (usize i = (written ? count : 0U); i <= kMaxBatch; ++i)
        {
            CRD_FUZZ_REQUIRE(reference[k][i] == -1.5);
        }
    }
    for_each_level(
        [&](const MotionKernels& table)
        {
            static f64 got[10][kMaxBatch + 1U];
            for (auto& column : got)
            {
                std::fill_n(column, kMaxBatch + 1U, -1.5);
            }
            table.dead_reckon(algorithm, columns, count, output_of(got));
            for (usize k = 0U; k < 10U; ++k)
            {
                for (usize i = 0U; i <= kMaxBatch; ++i)
                {
                    CRD_FUZZ_REQUIRE(same_value(got[k][i], reference[k][i]));
                }
            }
            // In place, the same answers.
            Fleet copy = fleet;
            const DeadReckoningOutput same{{copy.columns[0], copy.columns[1], copy.columns[2]},
                                           {copy.columns[3], copy.columns[4], copy.columns[5]},
                                           {copy.columns[12], copy.columns[13], copy.columns[14], copy.columns[15]}};
            table.dead_reckon(algorithm, input_of(copy), count, same);
            for (usize i = 0U; i < count; ++i)
            {
                for (usize k = 0U; k < 3U; ++k)
                {
                    CRD_FUZZ_REQUIRE(same_value(copy.columns[k][i], reference[k][i]));
                }
            }
        });
}

template <class T> void blends(Reader& in)
{
    const usize count = in.next() % (kMaxBatch + 1U);
    const bool accelerated = (in.next() & 1U) != 0U;
    T c[7][kMaxBatch];
    for (auto& column : c)
    {
        for (usize i = 0U; i < kMaxBatch; ++i)
        {
            column[i] = in.size != 0U ? in.next_value<T>() : static_cast<T>(0);
        }
    }
    const ProjectiveBlendSpan<T> span{c[0], c[1], c[2], c[3], accelerated ? c[4] : nullptr, c[5], c[6]};
    for_each_level(
        [&](const MotionKernels& table)
        {
            T position[kMaxBatch + 1U];
            T velocity[kMaxBatch + 1U];
            std::fill_n(position, kMaxBatch + 1U, static_cast<T>(-1.5));
            std::fill_n(velocity, kMaxBatch + 1U, static_cast<T>(-1.5));
            if constexpr (sizeof(T) == sizeof(f32))
            {
                table.projective_blend_f32(span, count, position, velocity);
            }
            else
            {
                table.projective_blend_f64(span, count, position, velocity);
            }
            for (usize i = 0U; i < count; ++i)
            {
                const T rate = accelerated ? c[4][i] : static_cast<T>(0);
                const T elapsed = c[5][i];
                const T width = c[6][i];
                T fraction = elapsed / width;
                fraction = (fraction >= static_cast<T>(0)) ? fraction : static_cast<T>(0);
                fraction = (fraction <= static_cast<T>(1)) ? fraction : static_cast<T>(1);
                fraction = (width > static_cast<T>(0)) ? fraction : static_cast<T>(1);
                const T change = c[3][i] - c[1][i];
                const T blended = c[1][i] + (change * fraction);
                const T half = static_cast<T>(0.5) * rate * elapsed * elapsed;
                const T projected = c[0][i] + (blended * elapsed) + half;
                const T difference = (c[2][i] + (c[3][i] * elapsed) + half) - projected;
                CRD_FUZZ_REQUIRE(same_value(position[i], projected + (difference * fraction)));
                const T gained = rate * elapsed;
                const T target_rate = c[3][i] + gained;
                const T projected_rate = blended + (change * fraction) + gained;
                const T expected =
                    (fraction < static_cast<T>(1))
                        ? projected_rate + ((target_rate - projected_rate) * fraction) + (difference / width)
                        : target_rate;
                CRD_FUZZ_REQUIRE(same_value(velocity[i], expected));
            }
            for (usize i = count; i <= kMaxBatch; ++i)
            {
                CRD_FUZZ_REQUIRE(position[i] == static_cast<T>(-1.5) && velocity[i] == static_cast<T>(-1.5));
            }
        });
}

void add(crd::fuzz::SeedSink& sink, const char* name, std::initializer_list<u8> bytes)
{
    sink.add(name, bytes.begin(), bytes.size());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    Reader in{data, size};
    for (u32 ops = 0U; ops < 8U && in.size != 0U; ++ops)
    {
        switch (in.next() % 9U)
        {
            case 0U:
                component_batches<f32>(in);
                break;
            case 1U:
                component_batches<f64>(in);
                break;
            case 2U:
                quaternion_batches(in, false);
                break;
            case 3U:
                quaternion_batches(in, true);
                break;
            case 4U:
                rewind_history(in);
                break;
            case 5U:
                dead_reckoning(in, false);
                break;
            case 6U:
                dead_reckoning(in, true);
                break;
            case 7U:
                blends<f32>(in);
                break;
            default:
                blends<f64>(in);
                break;
        }
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    // One op of each kind at small values and at the extremes.
    add(sink, "lerp-small", {0U, 5U, 2U, 3U, 4U, 5U, 3U, 2U, 4U, 5U, 4U, 2U, 3U, 5U, 2U, 4U, 5U, 3U, 2U, 2U, 3U, 4U,
                             4U, 3U, 2U, 5U, 3U, 4U});
    add(sink, "lerp-extremes", {0U, 3U, 9U, 10U, 11U, 12U, 13U, 12U, 8U, 14U, 11U, 0U, 1U, 9U, 12U, 11U});
    add(sink, "lerp-long", {0U, 37U});
    add(sink, "twin-small", {1U, 5U, 2U, 3U, 4U, 5U, 3U, 2U, 4U, 5U, 4U, 2U, 3U, 5U, 2U, 4U, 5U, 3U, 2U, 2U, 3U, 4U,
                             4U, 3U, 2U, 5U, 3U, 4U});
    add(sink, "twin-extremes", {1U, 3U, 9U, 10U, 11U, 12U, 13U, 12U, 8U, 14U, 11U, 0U, 1U, 9U, 12U, 11U});
    add(sink, "quaternions-raw", {2U, 2U, 2U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 3U, 15U, 15U, 15U, 0U, 0U, 15U, 0U, 4U});
    add(sink, "quaternions-zero", {2U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 4U});
    add(sink, "quaternions-huge", {2U, 1U, 12U, 12U, 12U, 12U, 13U, 13U, 13U, 13U, 4U});
    add(sink, "quaternions-nan", {2U, 1U, 11U, 2U, 2U, 2U, 2U, 2U, 2U, 2U, 11U});
    add(sink, "arc-orthogonal", {3U, 2U, 2U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U, 0U, 2U, 0U, 2U, 0U, 0U, 2U, 5U});
    add(sink, "arc-antipodal", {3U, 1U, 2U, 0U, 0U, 0U, 3U, 0U, 0U, 0U, 4U});
    add(sink, "arc-past-both-ends", {3U, 1U, 2U, 0U, 0U, 0U, 15U, 15U, 0U, 0U, 9U, 3U, 1U, 2U, 0U, 0U, 0U, 15U, 15U, 0U,
                                     0U, 10U});
    add(sink, "history-pushes", {4U, 12U, 0U, 1U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U,
                                 4U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 0U, 16U, 0U, 16U});
    add(sink, "history-range-ends", {4U, 6U, 2U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 128U, 2U, 255U, 255U, 255U, 255U, 255U,
                                     255U, 255U, 127U, 3U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 255U, 0U, 255U, 0U});
    // Dead reckoning: each algorithm once at every kind of value, the first word a count of a few entities.
    for (u8 algorithm = 0U; algorithm <= 10U; ++algorithm)
    {
        const char* const names[] = {"reckon-other", "reckon-static", "reckon-fpw",    "reckon-rpw",
                                     "reckon-rvw",   "reckon-fvw",    "reckon-fpb",    "reckon-rpb",
                                     "reckon-rvb",   "reckon-fvb",    "reckon-unknown"};
        add(sink, names[algorithm],
            {6U, algorithm, 5U, 3U, 2U, 4U, 5U, 3U, 2U, 4U, 5U, 4U, 2U, 3U, 5U, 2U, 4U, 5U, 3U, 2U, 7U,
             3U, 4U,        5U, 2U, 3U, 2U, 5U, 3U, 4U, 2U, 5U, 3U, 4U, 2U, 2U, 2U, 3U, 5U, 4U, 3U});
    }
    add(sink, "reckon-hostile",
        {5U, 8U, 9U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 9U, 10U, 11U, 12U, 13U, 14U, 15U, 9U, 10U, 11U});
    add(sink, "reckon-huge-rate", {5U, 7U, 3U, 2U, 2U, 2U, 2U, 2U, 2U, 2U, 2U, 2U, 12U, 12U, 12U, 0U, 0U, 0U, 2U, 12U});
    add(sink, "blend-small",
        {7U, 7U, 1U, 2U, 3U, 4U, 5U, 6U, 2U, 7U, 3U, 2U, 4U, 5U, 3U, 7U, 2U, 5U, 3U, 2U, 2U, 4U, 5U});
    add(sink, "blend-no-span", {8U, 4U, 0U, 2U, 3U, 4U, 5U, 2U, 3U, 0U, 0U, 1U, 11U, 9U});
    add(sink, "blend-extremes", {8U, 3U, 1U, 9U, 10U, 11U, 12U, 13U, 14U, 11U, 12U, 13U, 14U, 11U, 12U, 13U, 14U, 15U});
    add(sink, "history-negative-limits", {4U, 3U, 0U, 9U, 128U, 0U, 0U, 0U, 0U, 0U, 0U, 15U, 9U, 128U, 0U, 0U, 0U, 0U,
                                          0U, 0U, 255U, 255U, 255U, 255U, 255U, 255U, 255U, 255U, 0U, 0U, 0U, 0U, 0U,
                                          0U, 0U, 128U});
}
