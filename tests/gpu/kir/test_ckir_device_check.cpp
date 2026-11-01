// HW-RESILIENCE.3 -- the GPU device check's known-answer kernel (crd/kir/ckir_device_check.hpp) on the CPU oracle.
//
// The kernel run by eval_cpu_kernel (two workgroups, barrier semantics) reproduces the answers evaluated at compile
// time bit for bit, so the answers a GPU is held to are the kernel's own; the verifier names the part a wrong output is
// in; and the GLSL and HLSL emitters lower the kernel (each backend compiles it in its own device tests).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_device_check.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hlsl.hpp>
#include <crd/kir/ckir_kernel_eval.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string_view>

namespace kir = crd::kir;

namespace
{
struct Expected
{
    float floats[kir::kDeviceCheckFloats] = {};
    crd::u32 words[kir::kDeviceCheckWords] = {};

    Expected()
    {
        for (crd::u32 i = 0U; i < kir::kDeviceCheckFloats; ++i)
        {
            floats[i] = kir::device_check_float(i);
        }
        for (crd::u32 i = 0U; i < kir::kDeviceCheckWords; ++i)
        {
            words[i] = kir::device_check_word(i);
        }
    }
};
} // namespace

TEST_CASE("hw-resilience 3: the device check kernel reproduces its known answers on the CPU oracle",
          "[kir][kernel][hw-resilience]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph g(&alloc);
    const kir::KEntry e = kir::build_device_check_kernel(g);
    REQUIRE(e.is_kernel());

    crd::f64 in[kir::kDeviceCheckLanes];
    crd::f64 floats[kir::kDeviceCheckFloats];
    crd::f64 words[kir::kDeviceCheckWords];
    for (crd::u32 i = 0U; i < kir::kDeviceCheckLanes; ++i)
    {
        in[i] = kir::device_check_input(i);
    }
    for (crd::f64& f : floats)
    {
        f = std::numeric_limits<crd::f64>::quiet_NaN();
    }
    for (crd::f64& w : words)
    {
        w = -1.0;
    }
    kir::KernelBuffer bufs[3] = {{in, static_cast<crd::i32>(kir::kDeviceCheckLanes), 0, 0},
                                 {floats, static_cast<crd::i32>(kir::kDeviceCheckFloats), 0, 1},
                                 {words, static_cast<crd::i32>(kir::kDeviceCheckWords), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, kir::kDeviceCheckThreads, &alloc, kir::kDeviceCheckGroups);

    float got_floats[kir::kDeviceCheckFloats];
    crd::u32 got_words[kir::kDeviceCheckWords];
    for (crd::u32 i = 0U; i < kir::kDeviceCheckFloats; ++i)
    {
        got_floats[i] = static_cast<float>(floats[i]);
        CHECK(static_cast<crd::f64>(got_floats[i]) == floats[i]); // every output is an exact f32
    }
    for (crd::u32 i = 0U; i < kir::kDeviceCheckWords; ++i)
    {
        REQUIRE(words[i] >= 0.0);
        REQUIRE(words[i] < 4294967296.0);
        got_words[i] = static_cast<crd::u32>(words[i]);
    }
    const char* const failed = kir::device_check_mismatch(got_floats, got_words);
    INFO((failed != nullptr ? failed : "none"));
    CHECK(failed == nullptr);

    // Spot answers worked by hand: input 0 is 11 mod 23 - 11 = 0 (lane = (0 + 0 - 5) * (0 - 2) = 10), input 1 is
    // 48 mod 23 - 11 = -9 (lane = (81 - 27 - 5) * -11 = -539), and the chain at gid 0 starts from 0x7F4A7C15.
    CHECK(kir::device_check_input(0U) == 0.0F);
    CHECK(got_floats[0] == 10.0F);
    CHECK(kir::device_check_input(1U) == -9.0F);
    CHECK(got_floats[1] == -539.0F);
    crd::u32 h = 0x7F4A7C15U;
    for (int round = 0; round < 4; ++round)
    {
        h ^= h >> 15U;
        h *= 0x2C1B3C6DU;
    }
    CHECK(got_words[0] == h);
}

TEST_CASE("hw-resilience 3: the device check names the part a wrong output is in", "[kir][hw-resilience]")
{
    Expected x;
    CHECK(kir::device_check_mismatch(x.floats, x.words) == nullptr);

    const auto flipped = [](float v)
    {
        return std::bit_cast<float>(std::bit_cast<crd::u32>(v) ^ 1U);
    };
    struct Case
    {
        crd::u32 index;
        const char* part;
    };
    const Case cases[] = {{5U, "arithmetic"},
                          {kir::kDeviceCheckLanes + 3U, "shared-exchange"},
                          {2U * kir::kDeviceCheckLanes + 1U, "workgroup-sum"}};
    for (const Case& c : cases)
    {
        Expected bad;
        bad.floats[c.index] = flipped(bad.floats[c.index]);
        const char* const failed = kir::device_check_mismatch(bad.floats, bad.words);
        REQUIRE(failed != nullptr);
        CHECK(std::string_view{failed} == c.part);
    }
    {
        Expected unwritten;
        unwritten.floats[kir::kDeviceCheckLanes - 1U] = std::bit_cast<float>(0xFFFFFFFFU);
        const char* const failed = kir::device_check_mismatch(unwritten.floats, unwritten.words);
        REQUIRE(failed != nullptr);
        CHECK(std::string_view{failed} == "arithmetic");
    }
    {
        Expected bad;
        bad.words[kir::kDeviceCheckWords - 1U] ^= 0x80000000U;
        const char* const failed = kir::device_check_mismatch(bad.floats, bad.words);
        REQUIRE(failed != nullptr);
        CHECK(std::string_view{failed} == "integer-chain");
    }
}

TEST_CASE("hw-resilience 3: the GLSL and HLSL emitters lower the device check kernel", "[kir][kernel][hw-resilience]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph g(&alloc);
    const kir::KEntry e = kir::build_device_check_kernel(g);
    kir::GlslKernel glsl(&alloc);
    CHECK(kir::emit_compute_kernel_glsl(g, e, &alloc, glsl));
    kir::GlslKernel hlsl(&alloc);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &alloc, hlsl));
}
