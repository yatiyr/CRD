// crd-hesap-quantize-board: nanoseconds per value of hesap-net's quantisation at each kernel level the host runs
// against its peers, mas-bandwidth/serialize's compressed_float and networked-physics' compressed_quaternion, on the
// same values. Each arm's bits are compared with the peer's, so a faster wrong answer fails. The serialize arms pay for
// its bit stream, so hesap's bounded arms run once alone and once followed by that same bit writer.
//
// Usage: crd-hesap-quantize-board [--rounds N] [--quick]
//   rounds  interleaved rounds of every arm; the best of them is reported (default 25)
//   quick   one round over few values (the smoke CTest)

#include "kernel_levels.hpp"
#include "quantize_peers.hpp"
#include "seeded_words.hpp"

#include <crd/containers/array.hpp>
#include <crd/hesap/net/quantize.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hn = crd::hesap::net;
using crd::f32;
using crd::KernelLevel;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;

namespace
{

template <typename T> using Array = crd::containers::Array<T>;
using Clock = std::chrono::steady_clock;

constexpr usize kMaxLevels = 8U;

volatile u64 g_sink = 0U;

template <typename Work> [[nodiscard]] double timed_ns(usize count, double best, const Work& work)
{
    const Clock::time_point start = Clock::now();
    g_sink = g_sink + work();
    const double elapsed = std::chrono::duration<double, std::nano>(Clock::now() - start).count();
    const double per_value = elapsed / static_cast<double>(count);
    return per_value < best ? per_value : best;
}

struct Arm
{
    char name[48];
    double encode_ns;
    double decode_ns;
};

void print_arm(const char* family, const Arm& arm, double peer_encode, double peer_decode)
{
    std::printf("| %s | %s | %.2f | %.2f | %.2fx | %.2fx |\n", family, arm.name, arm.encode_ns, arm.decode_ns,
                peer_encode / arm.encode_ns, peer_decode / arm.decode_ns);
}

} // namespace

int main(int argc, char** argv)
{
    u32 rounds = 25U;
    usize count = 65536U;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--rounds") == 0 && i + 1 < argc)
        {
            rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            rounds = 1U;
            count = 1024U;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-hesap-quantize-board [--rounds N] [--quick]\n");
            return 2;
        }
    }
    if (rounds == 0U)
    {
        std::fprintf(stderr, "crd-hesap-quantize-board: --rounds is at least 1\n");
        return 2;
    }

    SeededWords rng(2026, 41);
    hn::BoundedFloat bounded{};
    if (!hn::bounded_float(-1024.0F, 1024.0F, 0.01F, bounded))
    {
        return 1;
    }
    Array<f32> values;
    Array<f32> quaternions;
    values.resize(count);
    quaternions.resize(4U * count);
    for (usize i = 0U; i < count; ++i)
    {
        values[i] = (static_cast<f32>(rng.next_u32() >> 8U) / 16777216.0F) * 2048.0F - 1024.0F;
        double sum = 0.0;
        double v[4];
        for (double& c : v)
        {
            c = (static_cast<double>(rng.next_u32()) / 2147483648.0) - 1.0;
            sum += c * c;
        }
        for (usize c = 0U; c < 4U; ++c)
        {
            quaternions[(4U * i) + c] = static_cast<f32>(v[c] / std::sqrt(sum));
        }
    }

    Array<u32> codes;
    Array<f32> decoded;
    Array<u8> stream;
    codes.resize(count);
    decoded.resize(4U * count);
    stream.resize((4U * count) + 64U);
    Array<u32> peer_codes;
    peer_codes.resize(count);
    Array<f32> peer_decoded;
    peer_decoded.resize(count);

    // ---- Bounded floats ------------------------------------------------------------------------------------------
    const auto wire_bits = static_cast<int>(bounded.bits);
    Arm peer_float{"serialize stream", 1e30, 1e30};
    const auto levels = hn::quantize_kernel_levels();
    Arm alone[kMaxLevels];
    Arm streamed[kMaxLevels];
    usize level_count = 0U;
    for (const KernelLevel<hn::QuantizeKernels>& level : levels)
    {
        if (!crd::test::kernel_level_runs_here(level) || level_count == kMaxLevels)
        {
            continue;
        }
        Arm arm{};
        std::snprintf(arm.name, sizeof(arm.name), "hesap %s", crd::cpu_level_name(level.level));
        arm.encode_ns = 1e30;
        arm.decode_ns = 1e30;
        alone[level_count] = arm;
        std::snprintf(arm.name, sizeof(arm.name), "hesap %s + its stream", crd::cpu_level_name(level.level));
        streamed[level_count] = arm;
        ++level_count;
    }
    for (u32 round = 0U; round < rounds; ++round)
    {
        peer_float.encode_ns = timed_ns(count, peer_float.encode_ns,
                                        [&]
                                        {
                                            return quantize_peers::float_write_batch(
                                                values.data(), count, bounded.max_code, wire_bits, bounded.delta,
                                                bounded.min, stream.data(), stream.size());
                                        });
        quantize_peers::bits_read_batch(stream.data(), stream.size(), wire_bits, peer_codes.data(), count);
        peer_float.decode_ns = timed_ns(count, peer_float.decode_ns,
                                        [&]
                                        {
                                            return quantize_peers::float_read_batch(
                                                stream.data(), stream.size(), bounded.max_code, wire_bits,
                                                bounded.delta, bounded.min, peer_decoded.data(), count);
                                        });
        usize index = 0U;
        for (const KernelLevel<hn::QuantizeKernels>& level : levels)
        {
            if (!crd::test::kernel_level_runs_here(level) || index == kMaxLevels)
            {
                continue;
            }
            const hn::QuantizeKernels& table = *level.table;
            alone[index].encode_ns = timed_ns(count, alone[index].encode_ns,
                                              [&]
                                              {
                                                  table.bounded_encode(bounded, values.data(), count, codes.data());
                                                  return static_cast<u64>(codes[count - 1U]);
                                              });
            alone[index].decode_ns = timed_ns(
                count, alone[index].decode_ns, [&]
                { return static_cast<u64>(table.bounded_decode(bounded, peer_codes.data(), count, decoded.data())); });
            streamed[index].encode_ns = timed_ns(count, streamed[index].encode_ns,
                                                 [&]
                                                 {
                                                     table.bounded_encode(bounded, values.data(), count, codes.data());
                                                     return quantize_peers::bits_write_batch(
                                                         codes.data(), count, wire_bits, stream.data(), stream.size());
                                                 });
            streamed[index].decode_ns = timed_ns(
                count, streamed[index].decode_ns,
                [&]
                {
                    quantize_peers::bits_read_batch(stream.data(), stream.size(), wire_bits, codes.data(), count);
                    return static_cast<u64>(table.bounded_decode(bounded, codes.data(), count, decoded.data()));
                });
            if (std::memcmp(codes.data(), peer_codes.data(), count * sizeof(u32)) != 0 ||
                std::memcmp(decoded.data(), peer_decoded.data(), count * sizeof(f32)) != 0)
            {
                std::fprintf(stderr, "bounded floats: %s differs from serialize\n", alone[index].name);
                return 1;
            }
            ++index;
        }
    }
    std::printf("| Family | Arm | encode ns | decode ns | encode vs peer | decode vs peer |\n");
    std::printf("|---|---|---|---|---|---|\n");
    print_arm("bounded float, 18 bits", peer_float, peer_float.encode_ns, peer_float.decode_ns);
    for (usize i = 0U; i < level_count; ++i)
    {
        print_arm("bounded float, 18 bits", alone[i], peer_float.encode_ns, peer_float.decode_ns);
        print_arm("bounded float, 18 bits", streamed[i], peer_float.encode_ns, peer_float.decode_ns);
    }

    // ---- Smallest three ------------------------------------------------------------------------------------------
    Array<u64> words;
    Array<u64> peer_words;
    Array<f32> peer_quaternions;
    words.resize(count);
    peer_words.resize(count);
    peer_quaternions.resize(4U * count);
    for (const u32 bits : {9U, 15U})
    {
        hn::SmallestThree format{};
        if (!hn::smallest_three(bits, format))
        {
            return 1;
        }
        Arm peer{"networked-physics", 1e30, 1e30};
        Arm arms[kMaxLevels];
        usize index = 0U;
        for (const KernelLevel<hn::QuantizeKernels>& level : levels)
        {
            if (!crd::test::kernel_level_runs_here(level) || index == kMaxLevels)
            {
                continue;
            }
            std::snprintf(arms[index].name, sizeof(arms[index].name), "hesap %s", crd::cpu_level_name(level.level));
            arms[index].encode_ns = 1e30;
            arms[index].decode_ns = 1e30;
            ++index;
        }
        for (u32 round = 0U; round < rounds; ++round)
        {
            peer.encode_ns = timed_ns(count, peer.encode_ns,
                                      [&]
                                      {
                                          quantize_peers::quaternion_load_batch(
                                              static_cast<int>(bits), quaternions.data(), count, peer_words.data());
                                          return peer_words[count - 1U];
                                      });
            peer.decode_ns =
                timed_ns(count, peer.decode_ns,
                         [&]
                         {
                             quantize_peers::quaternion_save_batch(static_cast<int>(bits), peer_words.data(), count,
                                                                   peer_quaternions.data());
                             return static_cast<u64>(peer_quaternions[0] != 0.0F);
                         });
            index = 0U;
            for (const KernelLevel<hn::QuantizeKernels>& level : levels)
            {
                if (!crd::test::kernel_level_runs_here(level) || index == kMaxLevels)
                {
                    continue;
                }
                const hn::QuantizeKernels& table = *level.table;
                arms[index].encode_ns =
                    timed_ns(count, arms[index].encode_ns,
                             [&]
                             {
                                 table.smallest_three_encode(format, quaternions.data(), count, words.data());
                                 return words[count - 1U];
                             });
                arms[index].decode_ns = timed_ns(count, arms[index].decode_ns,
                                                 [&]
                                                 {
                                                     return static_cast<u64>(table.smallest_three_decode(
                                                         format, words.data(), count, decoded.data()));
                                                 });
                if (std::memcmp(words.data(), peer_words.data(), count * sizeof(u64)) != 0 ||
                    std::memcmp(decoded.data(), peer_quaternions.data(), 4U * count * sizeof(f32)) != 0)
                {
                    std::fprintf(stderr, "smallest three: %s differs from networked-physics\n", arms[index].name);
                    return 1;
                }
                ++index;
            }
        }
        char family[32];
        std::snprintf(family, sizeof(family), "smallest three, %u bits", bits);
        print_arm(family, peer, peer.encode_ns, peer.decode_ns);
        for (usize i = 0U; i < level_count; ++i)
        {
            print_arm(family, arms[i], peer.encode_ns, peer.decode_ns);
        }
    }
    return 0;
}
