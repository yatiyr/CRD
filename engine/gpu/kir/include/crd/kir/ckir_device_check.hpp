#pragma once

// ckir_device_check.hpp -- HW-RESILIENCE.3: the known-answer kernel of the GPU device check.
//
// A GPU backend runs this kernel on an adapter before the process uses a device on it (crd/gpu/device_check.hpp), so
// a GPU or driver that misexecutes plain compute is passed over for the next adapter instead of silently producing
// wrong results. It is one CKIR compute kernel, so every backend lowers the same source with its own emitter and
// compiler (ADR-0103), and the CPU oracle (eval_cpu_kernel) runs it too. Two workgroups of 64 invocations each:
//   - arithmetic: lane = ((x * x + 3x) - 5) * (x - 2) in f32 on x = device_check_input(gid);
//   - shared-exchange: each invocation writes its lane to workgroup memory, and after a barrier reads its neighbour's;
//   - workgroup-sum: a barrier-separated tree sum of the workgroup's lanes, written by invocation 0;
//   - integer-chain: h = gid * 0x9E3779B1 + 0x7F4A7C15, then four rounds of h ^= h >> 15, h *= 0x2C1B3C6D in u32.
// Every float is a small exact integer and every sum stays below 2^24, so the answer has the same bits whatever
// order a device reduces in or whichever products it contracts into fused multiply-adds; the u32 chain wraps modulo
// 2^32 exactly. The answers are evaluated at compile time below.
//
// Bindings (set 0): input = 0 (f32, read), floats = 1 (f32: arithmetic, shared-exchange, workgroup-sum), words = 2
// (u32: integer-chain).

#include <crd/kir/ckir.hpp>

#include <bit>

namespace crd::kir
{

inline constexpr crd::u32 kDeviceCheckThreads = 64U;
inline constexpr crd::u32 kDeviceCheckGroups = 2U;
inline constexpr crd::u32 kDeviceCheckLanes = kDeviceCheckThreads * kDeviceCheckGroups;
inline constexpr crd::u32 kDeviceCheckFloats = 2U * kDeviceCheckLanes + kDeviceCheckGroups;
inline constexpr crd::u32 kDeviceCheckWords = kDeviceCheckLanes;
inline constexpr int kDeviceCheckBindings = 3;

// The input of invocation `gid`: an integer in [-11, 11].
[[nodiscard]] constexpr float device_check_input(crd::u32 gid) noexcept
{
    return static_cast<float>(static_cast<int>((gid * 37U + 11U) % 23U) - 11);
}

[[nodiscard]] constexpr float device_check_lane(float x) noexcept
{
    return ((x * x + 3.0F * x) - 5.0F) * (x - 2.0F);
}

[[nodiscard]] constexpr crd::u32 device_check_word(crd::u32 gid) noexcept
{
    crd::u32 h = gid * 0x9E3779B1U + 0x7F4A7C15U;
    for (int round = 0; round < 4; ++round)
    {
        h ^= h >> 15U;
        h *= 0x2C1B3C6DU;
    }
    return h;
}

// The expected value of float output `i` (arithmetic, then shared-exchange, then workgroup-sum).
[[nodiscard]] constexpr float device_check_float(crd::u32 i) noexcept
{
    if (i < kDeviceCheckLanes)
    {
        return device_check_lane(device_check_input(i));
    }
    if (i < 2U * kDeviceCheckLanes)
    {
        const crd::u32 gid = i - kDeviceCheckLanes;
        const crd::u32 group = gid / kDeviceCheckThreads;
        const crd::u32 next = (gid + 1U) & (kDeviceCheckThreads - 1U);
        return device_check_lane(device_check_input(group * kDeviceCheckThreads + next));
    }
    const crd::u32 group = i - 2U * kDeviceCheckLanes;
    float sum = 0.0F;
    for (crd::u32 t = 0U; t < kDeviceCheckThreads; ++t)
    {
        sum += device_check_lane(device_check_input(group * kDeviceCheckThreads + t));
    }
    return sum;
}

// The first part whose outputs differ from the known answers bit for bit ("arithmetic", "shared-exchange",
// "workgroup-sum", "integer-chain"), or nullptr when all match. `floats` holds kDeviceCheckFloats values and `words`
// kDeviceCheckWords.
[[nodiscard]] inline const char* device_check_mismatch(const float* floats, const crd::u32* words) noexcept
{
    for (crd::u32 i = 0U; i < kDeviceCheckFloats; ++i)
    {
        if (std::bit_cast<crd::u32>(floats[i]) != std::bit_cast<crd::u32>(device_check_float(i)))
        {
            if (i < kDeviceCheckLanes)
            {
                return "arithmetic";
            }
            return i < 2U * kDeviceCheckLanes ? "shared-exchange" : "workgroup-sum";
        }
    }
    for (crd::u32 i = 0U; i < kDeviceCheckWords; ++i)
    {
        if (words[i] != device_check_word(i))
        {
            return "integer-chain";
        }
    }
    return nullptr;
}

// Author the kernel into `g` (a graph of its own: an emitter emits every declaration of a graph) and return its entry,
// dispatched over kDeviceCheckGroups workgroups.
[[nodiscard]] inline KEntry build_device_check_kernel(KGraph& g)
{
    const Shape sh1 = make_shape({1});
    const auto ku = [&](crd::u32 v)
    {
        return g.constant(static_cast<crd::f64>(v), sh1, DType::U32);
    };
    const auto kf = [&](crd::f64 v)
    {
        return g.constant(v, sh1, DType::F32);
    };
    const auto bin = [&](KOp op, int a, int b)
    {
        return g.binary(op, a, b);
    };

    const int in_buf = g.buffer_decl(DType::F32, 0, 0, false);
    const int floats = g.buffer_decl(DType::F32, 0, 1, true);
    const int words = g.buffer_decl(DType::U32, 0, 2, true);
    const int sh = g.shared_decl(DType::F32, static_cast<int>(kDeviceCheckThreads));
    const int tid = g.builtin(KBuiltin::LocalInvocationIndex);
    const int wid = g.builtin(KBuiltin::WorkgroupIndex);
    const int gid = bin(KOp::Add, bin(KOp::Mul, wid, ku(kDeviceCheckThreads)), tid);

    const int mark = g.kernel_stmt_mark();

    const int x = g.buffer_load(in_buf, gid);
    const int quad = bin(KOp::Sub, bin(KOp::Add, bin(KOp::Mul, x, x), bin(KOp::Mul, kf(3.0), x)), kf(5.0));
    const int lane = bin(KOp::Mul, quad, bin(KOp::Sub, x, kf(2.0)));
    g.stmt_buffer_store(floats, gid, lane);

    int h = bin(KOp::Add, bin(KOp::Mul, gid, ku(0x9E3779B1U)), ku(0x7F4A7C15U));
    for (int round = 0; round < 4; ++round)
    {
        h = bin(KOp::Mul, bin(KOp::BitXor, h, bin(KOp::Shr, h, ku(15U))), ku(0x2C1B3C6DU));
    }
    g.stmt_buffer_store(words, gid, h);

    g.stmt_shared_store(sh, tid, lane);
    g.stmt_barrier();
    const int next = bin(KOp::BitAnd, bin(KOp::Add, tid, ku(1U)), ku(kDeviceCheckThreads - 1U));
    g.stmt_buffer_store(floats, bin(KOp::Add, gid, ku(kDeviceCheckLanes)), g.shared_load(sh, next));
    g.stmt_barrier();

    for (crd::u32 stride = kDeviceCheckThreads / 2U; stride >= 1U; stride /= 2U)
    {
        const int below = g.stmt_if_begin(bin(KOp::CmpLt, tid, ku(stride)));
        const int other = g.shared_load(sh, bin(KOp::Add, tid, ku(stride)));
        g.stmt_shared_store(sh, tid, bin(KOp::Add, g.shared_load(sh, tid), other));
        g.stmt_if_end(below);
        g.stmt_barrier();
    }
    const int first = g.stmt_if_begin(bin(KOp::CmpEq, tid, ku(0U)));
    g.stmt_buffer_store(floats, bin(KOp::Add, wid, ku(2U * kDeviceCheckLanes)), g.shared_load(sh, ku(0U)));
    g.stmt_if_end(first);

    KEntry e;
    e.stage = KStage::Compute;
    e.local_size[0] = kDeviceCheckThreads;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

} // namespace crd::kir
