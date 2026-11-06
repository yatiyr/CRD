// crd-hesap-erasure-board -- hesap-net' Reed-Solomon code against Intel ISA-L (HESAP-NET; docs/bench,
// CRD_BUILD_ERASURE_VS_ISAL): both timed in one process on the same aligned buffers, at datagram and storage symbol
// sizes. `encode` makes r repair symbols from what an application keeps per shape (hesap's prepared rows, ISA-L's
// tables); `encode-cold` forms those in every call. `decode` recovers r lost sources from k received symbols with the
// whole per-pattern work on each side: hesap's barycentric weights, ISA-L's gf_invert_matrix, table build and encode.
//
// Usage: crd-hesap-erasure-board [--case TEXT] [--budget-ms N] [--rounds N] [--quick]
//   case       only the cases whose name contains TEXT
//   budget-ms  the time one batch of one arm takes (default 10): the batch's operation count is calibrated to it
//   rounds     batches per arm and case (default 9), the arms rotated each round; an arm's value is their median
//   quick      one operation per batch and one round (the smoke CTest: every case of every arm runs, nothing is timed)
//
// CRD_CPU_LEVEL selects hesap's kernel level and names its arm; ISA-L's arms are every ISA body this CPU runs and its
// own dispatch (isal_arms.h). After every batch the outputs must hold the bytes hesap's code and the sources define, or
// the run exits 1: a faster wrong answer is no measurement. A value line is "<arm> <case> <MB/s of source data>", which
// scripts/ab-board.py reads (--higher-is-better --arm isal=board:'^isal-avx2 (?P<key>\S+) (?P<value>\S+)$').

#include <crd/containers/array.hpp>
#include <crd/core/cpu_features.hpp>
#include <crd/hesap/net/erasure.hpp>
#include <crd/hesap/net/erasure_dispatch.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <erasure_code.h>

#include "isal_arms.h"

namespace
{

namespace hn = crd::hesap::net;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using Bytes = crd::containers::Array<u8>;

struct Shape
{
    u32 k;
    u32 r;
    usize bytes;
};

constexpr Shape kShapes[] = {{4U, 2U, 1200U},   {10U, 4U, 1200U},  {10U, 4U, 1500U},   {32U, 8U, 1200U},
                             {64U, 16U, 1200U}, {10U, 4U, 65536U}, {10U, 4U, 1048576U}};
constexpr usize kCacheLine = 64U;
constexpr u32 kMaxRounds = 64U;

// One shape's buffers on cache lines, its generator matrix and the loss of its first r sources.
class Block
{
public:
    explicit Block(const Shape& shape) : shape_(shape), code_(shape.k, shape.k + shape.r)
    {
        const usize stride = (shape.bytes + kCacheLine - 1U) / kCacheLine * kCacheLine;
        storage_.resize((usize{shape.k + (2U * shape.r)} * stride) + kCacheLine);
        const auto base = reinterpret_cast<std::uintptr_t>(storage_.data());
        u8* next = storage_.data() + (((base + kCacheLine - 1U) & ~std::uintptr_t{kCacheLine - 1U}) - base);
        u64 state = 0x9E3779B97F4A7C15U;
        for (u32 i = 0U; i < shape.k + (2U * shape.r); ++i, next += stride)
        {
            (i < shape.k ? sources_ : i < shape.k + shape.r ? repair_ : outputs_).push_back(next);
        }
        for (u8* const source : sources_)
        {
            for (usize b = 0U; b < shape.bytes; ++b)
            {
                state = (state * 6364136223846793005U) + 1442695040888963407U;
                source[b] = static_cast<u8>(state >> 56U);
            }
        }
        for (u32 q = 0U; q < shape.r; ++q)
        {
            repair_indices_.push_back(shape.k + q);
            lost_.push_back(q);
        }
        generator_.resize(usize{shape.k + shape.r} * shape.k);
        for (u32 j = 0U; j < shape.k + shape.r; ++j)
        {
            for (u32 i = 0U; i < shape.k; ++i)
            {
                generator_[(usize{j} * shape.k) + i] = code_.coefficient(i, j);
            }
        }
        const hn::ErasureStatus made = code_.encode(source_views(), repair_indices_, repair_, shape.bytes);
        if (made != hn::ErasureStatus::Ok)
        {
            std::fprintf(stderr, "crd-hesap-erasure-board: the reference repair symbols were refused\n");
            std::exit(1);
        }
        for (u32 t = 0U; t < shape.k; ++t)
        {
            const u32 index = t < shape.k - shape.r ? shape.r + t : shape.k + (t - (shape.k - shape.r));
            indices_.push_back(index);
            const u8* const data = index < shape.k ? sources_[index] : repair_[index - shape.k];
            received_.push_back(hn::ErasureSymbol{index, data});
            received_data_.push_back(const_cast<u8*>(data));
        }
    }

    [[nodiscard]] const Shape& shape() const noexcept { return shape_; }
    [[nodiscard]] const hn::ReedSolomonCode& code() const noexcept { return code_; }
    [[nodiscard]] std::span<const u8* const> source_views() const noexcept
    {
        return {const_cast<const u8* const*>(sources_.data()), sources_.size()};
    }
    [[nodiscard]] std::span<const u32> repair_indices() const noexcept { return repair_indices_; }
    [[nodiscard]] std::span<const u32> lost() const noexcept { return lost_; }
    [[nodiscard]] std::span<const hn::ErasureSymbol> received() const noexcept { return received_; }
    [[nodiscard]] std::span<u8* const> outputs() const noexcept { return outputs_; }
    [[nodiscard]] unsigned char** source_data() noexcept { return sources_.data(); }
    [[nodiscard]] unsigned char** received_data() noexcept { return received_data_.data(); }
    [[nodiscard]] unsigned char** output_data() noexcept { return outputs_.data(); }
    [[nodiscard]] const u8* generator_row(u32 j) const noexcept { return generator_.data() + (usize{j} * shape_.k); }
    [[nodiscard]] u32 received_index(u32 t) const noexcept { return indices_[t]; }

    // Whether the outputs hold the repair symbols (`decoded` false) or the lost sources, and clears them.
    [[nodiscard]] bool take_outputs(bool decoded) noexcept
    {
        bool same = true;
        for (u32 q = 0U; q < shape_.r; ++q)
        {
            same = same && std::memcmp(outputs_[q], decoded ? sources_[q] : repair_[q], shape_.bytes) == 0;
            std::memset(outputs_[q], 0, shape_.bytes);
        }
        return same;
    }

private:
    Shape shape_;
    hn::ReedSolomonCode code_;
    Bytes storage_;
    Bytes generator_;
    crd::containers::Array<u8*> sources_;
    crd::containers::Array<u8*> repair_;
    crd::containers::Array<u8*> outputs_;
    crd::containers::Array<u8*> received_data_;
    crd::containers::Array<u32> repair_indices_;
    crd::containers::Array<u32> lost_;
    crd::containers::Array<u32> indices_;
    crd::containers::Array<hn::ErasureSymbol> received_;
};

// One side: hesap's code (`isal` null) or one ISA-L arm, with what it keeps per shape.
struct Arm
{
    Arm() = default;

    char name[32] = {};
    const crd_isal_arm* isal = nullptr;
    crd::containers::Array<hn::GfCoefficient> rows;
    Bytes tables;
    Bytes scratch;
};

void prepare(Arm& arm, Block& block)
{
    const Shape& shape = block.shape();
    if (arm.isal == nullptr)
    {
        arm.rows.resize(usize{shape.k} * shape.r);
        if (block.code().prepare_encode(block.repair_indices(), arm.rows) != hn::ErasureStatus::Ok)
        {
            std::exit(1);
        }
        return;
    }
    arm.tables.resize(usize{32U} * shape.k * shape.r);
    arm.scratch.resize((usize{3U} * shape.k * shape.k) + (usize{shape.r} * shape.k));
    arm.isal->tables(static_cast<int>(shape.k), static_cast<int>(shape.r),
                     const_cast<u8*>(block.generator_row(shape.k)), arm.tables.data());
}

void encode(Arm& arm, Block& block, u64 ops)
{
    const Shape& shape = block.shape();
    for (u64 op = 0U; op < ops; ++op)
    {
        if (arm.isal == nullptr)
        {
            const hn::ErasureStatus status =
                block.code().encode_prepared(arm.rows, block.source_views(), block.outputs(), shape.bytes);
            if (status != hn::ErasureStatus::Ok)
            {
                std::exit(1);
            }
        }
        else
        {
            arm.isal->encode(static_cast<int>(shape.bytes), static_cast<int>(shape.k), static_cast<int>(shape.r),
                             arm.tables.data(), block.source_data(), block.output_data());
        }
    }
}

void encode_cold(Arm& arm, Block& block, u64 ops)
{
    const Shape& shape = block.shape();
    for (u64 op = 0U; op < ops; ++op)
    {
        if (arm.isal == nullptr)
        {
            const hn::ErasureStatus status =
                block.code().encode(block.source_views(), block.repair_indices(), block.outputs(), shape.bytes);
            if (status != hn::ErasureStatus::Ok)
            {
                std::exit(1);
            }
        }
        else
        {
            arm.isal->tables(static_cast<int>(shape.k), static_cast<int>(shape.r),
                             const_cast<u8*>(block.generator_row(shape.k)), arm.tables.data());
            arm.isal->encode(static_cast<int>(shape.bytes), static_cast<int>(shape.k), static_cast<int>(shape.r),
                             arm.tables.data(), block.source_data(), block.output_data());
        }
    }
}

void decode(Arm& arm, Block& block, u64 ops)
{
    const Shape& shape = block.shape();
    const usize k = shape.k;
    for (u64 op = 0U; op < ops; ++op)
    {
        if (arm.isal == nullptr)
        {
            const hn::ErasureStatus status =
                block.code().decode(block.received(), block.lost(), block.outputs(), shape.bytes);
            if (status != hn::ErasureStatus::Ok)
            {
                std::exit(1);
            }
            continue;
        }
        u8* const matrix = arm.scratch.data();
        u8* const inverse = matrix + (k * k);
        u8* const rows = inverse + (k * k);
        u8* const tables = arm.tables.data();
        for (u32 t = 0U; t < shape.k; ++t)
        {
            std::memcpy(matrix + (t * k), block.generator_row(block.received_index(t)), k);
        }
        if (gf_invert_matrix(matrix, inverse, static_cast<int>(k)) != 0)
        {
            std::exit(1);
        }
        for (u32 q = 0U; q < shape.r; ++q)
        {
            std::memcpy(rows + (q * k), inverse + (usize{block.lost()[q]} * k), k);
        }
        arm.isal->tables(static_cast<int>(k), static_cast<int>(shape.r), rows, tables);
        arm.isal->encode(static_cast<int>(shape.bytes), static_cast<int>(k), static_cast<int>(shape.r), tables,
                         block.received_data(), block.output_data());
    }
}

struct Options
{
    const char* filter = "";
    double budget_ns = 10e6;
    u32 rounds = 9U;
    bool quick = false;
};

Options parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--case") == 0 && has_value)
        {
            options.filter = argv[++i];
        }
        else if (std::strcmp(argv[i], "--budget-ms") == 0 && has_value)
        {
            options.budget_ns = std::atof(argv[++i]) * 1e6;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && has_value)
        {
            options.rounds = static_cast<u32>(std::atoi(argv[++i]));
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            options.quick = true;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-hesap-erasure-board [--case TEXT] [--budget-ms N] [--rounds N] [--quick]\n");
            std::exit(2);
        }
    }
    if (options.rounds < 1U || options.rounds > kMaxRounds || options.budget_ns <= 0.0)
    {
        std::fprintf(stderr, "crd-hesap-erasure-board: --rounds is 1 to %u and --budget-ms above 0\n", kMaxRounds);
        std::exit(2);
    }
    if (options.quick)
    {
        options.rounds = 1U;
    }
    return options;
}

using Operation = void (*)(Arm&, Block&, u64);

// The nanoseconds of `ops` operations, whose bytes must be right.
double timed(const Operation operation, Arm& arm, Block& block, bool decoded, u64 ops, const char* name)
{
    const auto start = std::chrono::steady_clock::now();
    operation(arm, block, ops);
    const auto stop = std::chrono::steady_clock::now();
    if (!block.take_outputs(decoded))
    {
        std::fprintf(stderr, "crd-hesap-erasure-board: %s gave the wrong bytes in %s\n", arm.name, name);
        std::exit(1);
    }
    return std::chrono::duration<double, std::nano>(stop - start).count();
}

void run_case(crd::containers::Array<Arm>& arms, Block& block, const Options& options, const char* operation_name,
              const Operation operation, bool decoded)
{
    const Shape& shape = block.shape();
    char name[64];
    std::snprintf(name, sizeof(name), "%s-k%u-r%u-%zu", operation_name, shape.k, shape.r, shape.bytes);
    if (std::strstr(name, options.filter) == nullptr)
    {
        return;
    }
    const usize count = arms.size();
    crd::containers::Array<u64> ops;
    crd::containers::Array<double> rates;
    rates.resize(count * options.rounds);
    for (Arm& arm : arms)
    {
        prepare(arm, block);
        u64 batch = 1U;
        while (!options.quick)
        {
            const double elapsed = timed(operation, arm, block, decoded, batch, name);
            if (elapsed * 4.0 >= options.budget_ns)
            {
                batch = std::max<u64>(1U, static_cast<u64>(static_cast<double>(batch) * options.budget_ns / elapsed));
                break;
            }
            batch *= 2U;
        }
        ops.push_back(batch);
    }
    const double source_bytes = static_cast<double>(shape.k) * static_cast<double>(shape.bytes);
    for (u32 round = 0U; round < options.rounds; ++round)
    {
        for (usize step = 0U; step < count; ++step)
        {
            const usize a = (step + round) % count;
            const double elapsed = timed(operation, arms[a], block, decoded, ops[a], name);
            rates[(a * options.rounds) + round] = source_bytes * static_cast<double>(ops[a]) * 1e3 / elapsed;
        }
    }
    for (usize a = 0U; a < count; ++a)
    {
        double* const first = rates.data() + (a * options.rounds);
        std::sort(first, first + options.rounds);
        std::printf("%s %s %.1f\n", arms[a].name, name, first[options.rounds / 2U]);
    }
}

} // namespace

int main(int argc, char** argv)
{
    const Options options = parse(argc, argv);
    crd::containers::Array<Arm> arms;
    arms.push_back(Arm());
    const crd::KernelLevel<hn::ErasureKernels>& level = hn::erasure_kernels();
    std::snprintf(arms.back().name, sizeof(arms.back().name), "cerid-%s%s", crd::cpu_level_name(level.level),
                  level.features.has(crd::CpuFeature::Gfni) ? "-gfni" : "");
    for (unsigned a = 0U; a < crd_isal_arm_count; ++a)
    {
        if (crd_isal_arm_runs(crd_isal_arms[a]))
        {
            arms.push_back(Arm());
            std::snprintf(arms.back().name, sizeof(arms.back().name), "isal-%s", crd_isal_arms[a].name);
            arms.back().isal = &crd_isal_arms[a];
        }
    }
    for (const Shape& shape : kShapes)
    {
        Block block(shape);
        run_case(arms, block, options, "encode", encode, false);
        run_case(arms, block, options, "encode-cold", encode_cold, false);
        run_case(arms, block, options, "decode", decode, true);
    }
    return 0;
}
