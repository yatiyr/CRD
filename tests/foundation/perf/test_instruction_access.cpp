// HW-RESILIENCE.2: the memory accesses of a faulting instruction (crd/perf/instruction_access.hpp) -- x86-64 lengths
// and accesses for every addressing form, the stack, string, bit-offset, absolute and vector forms, EVEX's scaled
// displacement, the refused forms, cuts, and the divide error's operands against plain wide arithmetic; AArch64's
// loads, stores, pairs, exclusives, atomics, structures, literal, register-offset and RCpc forms, cache maintenance,
// the top byte, the forms that make no access and the refused ones. Every encoding below was cross-checked once with
// LLVM 20's llvm-mc (2026-11-01 sessions).

#include <crd/core/seed_generators.hpp>
#include <crd/perf/fault_classification.hpp>
#include <crd/perf/instruction_access.hpp>

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first
#include <dbghelp.h>
#endif

namespace cont = crd::containers;
namespace cp = crd::perf;
using crd::i64;
using crd::u32;
using crd::u64;
using crd::u8;

namespace
{

constexpr u64 kPc = 0x00000001'40001000ULL;
constexpr u64 kRsp = 0x7FF000U;
constexpr u8 kRd = cp::kAccessRead;
constexpr u8 kWr = cp::kAccessWrite;
constexpr u8 kRw = cp::kAccessRead | cp::kAccessWrite;

cp::FaultRegisters registers()
{
    cp::FaultRegisters r;
    r.known = true;
    r.pc = kPc;
    const u64 values[16] = {0x1010U, 0x2000U,          0x3000U, 0x4000U, kRsp,    0x5000U, 0x6000U, 0x7000U,
                            0x8000U, 0x1'0000'9000ULL, ~u64{8}, 0xB000U, 0xC000U, 0xD000U, 0xE000U, 0xF000U};
    for (u32 i = 0U; i < 16U; ++i)
    {
        r.gpr[i] = values[i];
    }
    r.flags = 0x246U;
    return r;
}

// "8b 01" -> bytes.
u32 parse_hex(const char* text, u8 (&out)[crd::crash::kCodeWindowBytes])
{
    u32 n = 0U;
    for (const char* p = text; *p != '\0' && n < crd::crash::kCodeWindowBytes;)
    {
        if (*p == ' ')
        {
            ++p;
            continue;
        }
        const auto digit = [](char c)
        {
            return c <= '9' ? c - '0' : c - 'a' + 10;
        };
        out[n++] = static_cast<u8>((digit(p[0]) << 4) | digit(p[1]));
        p += 2;
    }
    return n;
}

cp::InstructionAccess decode(const char* hex, const cp::FaultRegisters& regs = registers())
{
    u8 code[crd::crash::kCodeWindowBytes] = {};
    const u32 n = parse_hex(hex, code);
    return cp::decode_instruction_access(cp::InstructionSet::X64, {code, n}, regs);
}

struct Expect
{
    u64 address;
    u64 size;
    u8 kinds;
};

struct Case
{
    const char* hex;
    u32 length;
    u32 count;
    Expect accesses[8];
};

constexpr u64 rip(u32 length, u64 disp)
{
    return kPc + length + disp;
}

#if defined(__SIZEOF_INT128__)
__extension__ using U128 = unsigned __int128; // __extension__ keeps it under -Werror=pedantic
__extension__ using I128 = __int128;
#endif

// Each access the instruction makes, in the decoder's order.
const Case kCases[] = {
    {"8b 01", 2U, 1U, {{0x2000U, 4U, kRd}}},                                        // mov eax, [rcx]
    {"48 8b 44 24 08", 5U, 1U, {{kRsp + 8U, 8U, kRd}}},                             // mov rax, [rsp+8]
    {"89 0c 25 00 10 00 00", 7U, 1U, {{0x1000U, 4U, kWr}}},                         // mov [0x1000], ecx
    {"48 8b 05 10 00 00 00", 7U, 1U, {{rip(7U, 0x10U), 8U, kRd}}},                  // mov rax, [rip+16]
    {"c7 05 10 00 00 00 2a 00 00 00", 10U, 1U, {{rip(10U, 0x10U), 4U, kWr}}},       // mov [rip+16], 42
    {"66 c7 40 04 34 12", 6U, 1U, {{0x1014U, 2U, kWr}}},                            // mov word [rax+4], 0x1234
    {"48 c7 44 24 f8 ff ff ff ff", 9U, 1U, {{kRsp - 8U, 8U, kWr}}},                 // mov qword [rsp-8], -1
    {"42 8b 04 81", 4U, 1U, {{0x2000U + 0x8000U * 4U, 4U, kRd}}},                   // [rcx + 4*r8]
    {"42 8b 04 a1", 4U, 1U, {{0x2000U + 0xC000U * 4U, 4U, kRd}}},                   // [rcx + 4*r12]
    {"8b 04 a1", 3U, 1U, {{0x2000U, 4U, kRd}}},                                     // [rcx + 4*riz]
    {"41 8b 45 00", 4U, 1U, {{0xD000U, 4U, kRd}}},                                  // [r13]
    {"41 8b 04 24", 4U, 1U, {{0xC000U, 4U, kRd}}},                                  // [r12]
    {"41 8b 04 2d 00 01 00 00", 8U, 1U, {{0x5100U, 4U, kRd}}},                      // [1*rbp + 256]: no base
    {"67 41 8b 01", 4U, 1U, {{0x9000U, 4U, kRd}}},                                  // [r9d]
    {"f0 48 0f b1 0a", 5U, 1U, {{0x3000U, 8U, kRw}}},                               // lock cmpxchg [rdx], rcx
    {"48 0f c7 0e", 4U, 1U, {{0x6000U, 16U, kRw}}},                                 // cmpxchg16b [rsi]
    {"0f b6 03", 3U, 1U, {{0x4000U, 1U, kRd}}},                                     // movzx eax, byte [rbx]
    {"0f 18 0e", 3U, 0U, {}},                                                       // prefetcht0 [rsi]
    {"0f 1f 44 00 00", 5U, 0U, {}},                                                 // nop dword [rax+rax]
    {"66 0f 1f 84 00 00 00 00 00", 9U, 0U, {}},                                     // nop word [rax+rax+0]
    {"8d 04 08", 3U, 0U, {}},                                                       // lea eax, [rax+rcx]
    {"64 8b c0", 3U, 0U, {}},                                                       // mov eax, eax (FS unused)
    {"50", 1U, 1U, {{kRsp - 8U, 8U, kWr}}},                                         // push rax
    {"41 5f", 2U, 1U, {{kRsp, 8U, kRd}}},                                           // pop r15
    {"ff 34 24", 3U, 2U, {{kRsp, 8U, kRd}, {kRsp - 8U, 8U, kWr}}},                  // push qword [rsp]
    {"8f 44 24 08", 4U, 2U, {{kRsp, 8U, kRd}, {kRsp + 16U, 8U, kWr}}},              // pop qword [rsp+8], after
    {"c9", 1U, 1U, {{0x5000U, 8U, kRd}}},                                           // leave
    {"f3 a4", 2U, 2U, {{0x6000U, 0x2000U, kRd}, {0x7000U, 0x2000U, kWr}}},          // rep movsb
    {"48 ab", 2U, 1U, {{0x7000U, 8U, kWr}}},                                        // stosq
    {"d7", 1U, 1U, {{0x4010U, 1U, kRd}}},                                           // xlatb
    {"a1 88 77 66 55 44 33 22 11", 9U, 1U, {{0x1122334455667788ULL, 4U, kRd}}},     // mov eax, [moffs64]
    {"48 a3 88 77 66 55 44 33 22 11", 10U, 1U, {{0x1122334455667788ULL, 8U, kWr}}}, // mov [moffs64], rax
    {"0f a3 08", 3U, 1U, {{0x1410U - 3U, 7U, kRd}}},                                // bt [rax], ecx: +0x400 bytes
    {"4c 0f a3 10", 4U, 1U, {{0x100EU - 7U, 15U, kRd}}},                            // bt [rax], r10 (-9): -2 bytes
    {"0f ba 20 05", 4U, 1U, {{0x1010U, 4U, kRd}}},
    {"f6 00 01", 3U, 1U, {{0x1010U, 1U, kRd}}},             // test byte [rax], 1
    {"48 f7 00 ff 00 00 00", 7U, 1U, {{0x1010U, 8U, kRd}}}, // test qword [rax], 255
    {"66 f7 18", 3U, 1U, {{0x1010U, 2U, kRw}}},  // neg word [rax]                                  // bt dword [rax], 5
    {"0f 28 01", 3U, 1U, {{0x2000U, 16U, kRw}}}, // movaps xmm0, [rcx]
    {"66 0f 3a 0f 01 08", 6U, 1U, {{0x2000U, 16U, kRw}}},                 // palignr xmm0, [rcx], 8
    {"f2 0f 38 f1 c8", 5U, 0U, {}},                                       // crc32 ecx, eax
    {"0f 38 f0 01", 4U, 1U, {{0x2000U, 4U, kRd}}},                        // movbe eax, [rcx]
    {"dd 31", 2U, 1U, {{0x2000U, 108U, kWr}}},                            // fnsave [rcx]
    {"d9 7d 00", 3U, 1U, {{0x5000U, 2U, kWr}}},                           // fnstcw [rbp]
    {"0f ae 01", 3U, 1U, {{0x2000U, 512U, kWr}}},                         // fxsave [rcx]
    {"66 0f ae 3f", 4U, 1U, {{0x7000U, 64U, kRw}}},                       // clflushopt [rdi]
    {"c5 fc 28 01", 4U, 1U, {{0x2000U, 32U, kRw}}},                       // vmovaps ymm0, [rcx]
    {"c4 e2 7d 18 05 10 00 00 00", 9U, 1U, {{rip(9U, 0x10U), 32U, kRw}}}, // vbroadcastss ymm0, [rip+16]
    {"c4 e3 7d 18 01 01", 6U, 1U, {{0x2000U, 32U, kRw}}},                 // vinsertf128 ymm0, ymm0, [rcx], 1
    {"c5 f8 77", 3U, 0U, {}},                                             // vzeroupper
    {"62 f1 7c 48 28 01", 6U, 1U, {{0x2000U, 64U, kRw}}},                 // vmovaps zmm0, [rcx]
    {"62 f1 7c 48 28 41 01",
     7U,
     7U, // vmovaps zmm0, [rcx+1*N]
     {{0x2001U, 64U, kRw},
      {0x2002U, 64U, kRw},
      {0x2004U, 64U, kRw},
      {0x2008U, 64U, kRw},
      {0x2010U, 64U, kRw},
      {0x2020U, 64U, kRw},
      {0x2040U, 64U, kRw}}},
};

} // namespace

TEST_CASE("instruction access: x86-64 lengths and accesses for every addressing, stack, string and vector form",
          "[perf][diag][hw-resilience]")
{
    for (const Case& c : kCases)
    {
        const cp::InstructionAccess a = decode(c.hex);
        INFO(c.hex << " -> " << cont::String{a.reason}.c_str());
        REQUIRE(a.decoded);
        CHECK(a.length == c.length);
        REQUIRE(a.count == c.count);
        for (u32 i = 0U; i < c.count; ++i)
        {
            INFO("access " << i);
            CHECK(a.accesses[i].address == c.accesses[i].address);
            CHECK(a.accesses[i].size == c.accesses[i].size);
            CHECK(a.accesses[i].kinds == c.accesses[i].kinds);
        }
        CHECK_FALSE(a.divides);
    }
}

// ---- AArch64 ----------------------------------------------------------------------------------------------------

namespace
{

constexpr u64 kA64Pc = 0x400000U;
constexpr u64 kA64Sp = 0x7FF000U;
constexpr u64 kA64Tagged = 0x3400'0000'0000'8000ULL; // x7: tag 0x34, address 0x8000

cp::FaultRegisters a64_registers()
{
    cp::FaultRegisters r;
    r.known = true;
    r.pc = kA64Pc;
    for (u32 i = 0U; i < 31U; ++i)
    {
        r.gpr[i] = 0x10000U + 0x1000U * i; // x1 = 0x11000, x2 = 0x12000, x4 = 0x14000
    }
    r.gpr[5] = ~u64{0xF};        // UXTW reads 0xFFFFFFF0
    r.gpr[6] = 0x1'8000'0000ULL; // SXTW reads -2^31
    r.gpr[7] = kA64Tagged;
    r.gpr[9] = 0x12345U;
    r.gpr[31] = kA64Sp;
    return r;
}

cp::InstructionAccess decode_a64(u32 word, u32 bytes = 4U)
{
    const u8 code[4] = {static_cast<u8>(word), static_cast<u8>(word >> 8U), static_cast<u8>(word >> 16U),
                        static_cast<u8>(word >> 24U)};
    return cp::decode_instruction_access(cp::InstructionSet::AArch64, {code, bytes}, a64_registers());
}

struct A64Case
{
    u32 word;
    u64 address;
    u64 size;
    u8 kinds;
};

constexpr u64 kSxtw = (0x11000U - 0x2'0000'0000ULL) & cp::kAArch64AddressBits; // x1 + 4 * -2^31, top byte cleared

const A64Case kA64Cases[] = {
    {0xB9400020U, 0x11000U, 4U, kRd},                 // ldr w0, [x1]
    {0xF9000BE0U, kA64Sp + 16U, 8U, kWr},             // str x0, [sp, #16]
    {0x397FFC20U, 0x11FFFU, 1U, kRd},                 // ldrb w0, [x1, #4095]
    {0xF85F8020U, 0x10FF8U, 8U, kRd},                 // ldur x0, [x1, #-8]
    {0xF8410420U, 0x11000U, 8U, kRd},                 // ldr x0, [x1], #16
    {0x3C9E0C20U, 0x10FE0U, 16U, kWr},                // str q0, [x1, #-32]!
    {0xF8627820U, 0x11000U + 0x12000U * 8U, 8U, kRd}, // ldr x0, [x1, x2, lsl #3]
    {0xB866D820U, kSxtw, 4U, kRd},                    // ldr w0, [x1, w6, sxtw #2]
    {0x78654820U, 0x11000U + 0xFFFFFFF0ULL, 2U, kRd}, // ldrh w0, [x1, w5, uxtw]
    {0xF87F6820U, 0x11000U, 8U, kRd},                 // ldr x0, [x1, xzr]
    {0x58000800U, kA64Pc + 256U, 8U, kRd},            // ldr x0, .+256
    {0x98FFFFE0U, kA64Pc - 4U, 4U, kRd},              // ldrsw x0, .-4
    {0x9C000080U, kA64Pc + 16U, 16U, kRd},            // ldr q0, .+16
    {0xA9BF7BFDU, kA64Sp - 16U, 16U, kWr},            // stp x29, x30, [sp, #-16]!
    {0xACC10420U, 0x11000U, 32U, kRd},                // ldp q0, q1, [x1], #32
    {0x69410420U, 0x11008U, 8U, kRd},                 // ldpsw x0, x1, [x1, #8]
    {0x6C3F8420U, 0x10FF8U, 16U, kWr},                // stnp d0, d1, [x1, #-8]
    {0x69010420U, 0x11020U, 16U, kWr},                // stgp x0, x1, [x1, #32]
    {0x885F7C20U, 0x11000U, 4U, kRd},                 // ldxr w0, [x1]
    {0xC802FC20U, 0x11000U, 8U, kWr},                 // stlxr w2, x0, [x1]
    {0xC87F8440U, 0x12000U, 16U, kRd},                // ldaxp x0, x1, [x2]
    {0x089FFC20U, 0x11000U, 1U, kWr},                 // stlrb w0, [x1]
    {0xC8E0FC41U, 0x12000U, 8U, kRw},                 // casal x0, x1, [x2]
    {0x4860FC82U, 0x14000U, 16U, kRw},                // caspal x0, x1, x2, x3, [x4]
    {0x08A07FE1U, kA64Sp, 1U, kRw},                   // casb w0, w1, [sp]
    {0xB8E00041U, 0x12000U, 4U, kRw},                 // ldaddal w0, w1, [x2]
    {0xF820003FU, 0x11000U, 8U, kRw},                 // stadd x0, [x1]
    {0x38208041U, 0x12000U, 1U, kRw},                 // swpb w0, w1, [x2]
    {0xF8BFC020U, 0x11000U, 8U, kRd},                 // ldapr x0, [x1]
    {0x19211040U, 0x12000U, 16U, kRw},                // ldclrp x0, x1, [x2]
    {0x19200841U, 0x12000U, 8U, kRw},                 // rcwcas x0, x1, [x2]
    {0x19219040U, 0x12000U, 16U, kRw},                // rcwclrp x0, x1, [x2]
    {0x38209041U, 0x12000U, 8U, kRw},                 // rcwclr x0, x1, [x2]
    {0x19200441U, 0x12000U, 4U, kRw},                 // ldtadd w0, w1, [x2]
    {0xC9807C41U, 0x12000U, 8U, kRw},                 // cast x0, x1, [x2]
    {0x49807C82U, 0x14000U, 16U, kRw},                // caspt x0, x1, x2, x3, [x4]
    {0x895F7C20U, 0x11000U, 4U, kRd},                 // ldtxr w0, [x1]
    {0xEDC10440U, 0x12020U, 32U, kRd},                // ldtp q0, q1, [x2, #32]!
    {0xF83FD020U, 0x11000U, 64U, kRd},                // ld64b x0, [x1]
    {0xF822B020U, 0x11000U, 64U, kWr},                // st64bv x2, x0, [x1]
    {0x7C200041U, 0x12000U, 2U, kRw},                 // ldfadd h0, h1, [x2]
    {0x3C200041U, 0x12000U, 2U, kRw},                 // ldbfadd h0, h1, [x2]
    {0xFC206041U, 0x12000U, 8U, kRw},                 // ldfmaxnm d0, d1, [x2]
    {0xD9411840U, 0x12000U, 16U, kRd},                // ldiapp x0, x1, [x2]
    {0xD9010840U, 0x11FF0U, 16U, kWr},                // stilp x0, x1, [x2, #-16]!
    {0xD9800820U, 0x10FF8U, 8U, kWr},                 // stlr x0, [x1, #-8]!
    {0x99C00820U, 0x11000U, 4U, kRd},                 // ldapr w0, [x1], #4
    {0x1DDFD820U, 0x10FFDU, 16U, kRd},                // ldapur q0, [x1, #-3]
    {0x19801020U, 0x11001U, 1U, kRd},                 // ldapursb x0, [x1, #1]
    {0x591FE020U, 0x10FFEU, 2U, kWr},                 // stlurh w0, [x1, #-2]
    {0x4C400020U, 0x11000U, 64U, kRd},                // ld4 {v0.16b - v3.16b}, [x1]
    {0x0C9FA020U, 0x11000U, 16U, kWr},                // st1 {v0.8b, v1.8b}, [x1], #16
    {0x0D40B020U, 0x11000U, 12U, kRd},                // ld3 {v0.s - v2.s}[1], [x1]
    {0x4D60EC20U, 0x11000U, 32U, kRd},                // ld4r {v0.2d - v3.2d}, [x1]
    {0x4D418420U, 0x11000U, 8U, kRd},                 // ldap1 {v0.d}[1], [x1]
    {0x0DC25820U, 0x11000U, 2U, kRd},                 // ld1 {v0.h}[3], [x1], x2
    {0xD93FFC20U, 0x10FF0U, 16U, kWr},                // stg x0, [x1, #-16]!
    {0xD9A02820U, 0x11020U, 32U, kWr},                // st2g x0, [x1, #32]
    {0xD9602020U, 0x11020U, 16U, kRd},                // ldg x0, [x1, #32]
    {0xD91F0C20U, 0x11000U, 8U, kWr},                 // gcsstr x0, [x1]
    {0xD50B7429U, 0x12000U, 2048U, kWr},              // dc zva, x9: the 2 KiB bound
    {0xD50B7E29U, 0x12000U, 2048U, kRw},              // dc civac, x9
    {0xD50B7529U, 0x12000U, 2048U, kRw},              // ic ivau, x9
    {0xF94000E0U, 0x8000U, 8U, kRd},                  // ldr x0, [x7]: the top byte ignored
    {0xB90008E0U, 0x8008U, 4U, kWr},                  // str w0, [x7, #8]
};

} // namespace

TEST_CASE("instruction access: REP counts, the direction flag and the 15-byte limit", "[perf][diag][hw-resilience]")
{
    cp::FaultRegisters down = registers();
    down.flags |= 1U << 10U; // DF: elements run downward from rsi and rdi
    const cp::InstructionAccess movsq = decode("f3 48 a5", down);
    REQUIRE(movsq.decoded);
    REQUIRE(movsq.count == 2U);
    CHECK(movsq.accesses[0].address == u64{0x6000U} - (0x2000U - 1U) * 8U);
    CHECK(movsq.accesses[0].size == 0x2000U * 8U);
    CHECK(movsq.accesses[1].address == u64{0x7000U} - (0x2000U - 1U) * 8U);

    cp::FaultRegisters none = registers();
    none.gpr[1] = 0U; // rcx zero: REP touches nothing
    const cp::InstructionAccess empty = decode("f3 a4", none);
    REQUIRE(empty.decoded);
    CHECK(empty.count == 0U);

    cp::FaultRegisters huge = registers();
    huge.gpr[1] = ~u64{0}; // a count whose span overflows covers everything
    const cp::InstructionAccess all = decode("f3 48 a5", huge);
    REQUIRE(all.decoded);
    CHECK(all.accesses[0].size == ~u64{0});

    const cp::InstructionAccess longest = decode("66 66 66 66 66 66 66 66 66 66 66 66 66 66 90");
    CHECK(longest.decoded);
    CHECK(longest.length == 15U);
    const cp::InstructionAccess too_long = decode("66 66 66 66 66 66 66 66 66 66 66 66 66 66 66 90");
    CHECK_FALSE(too_long.decoded);
    CHECK(too_long.reason == cont::StringView{"longer than 15 bytes, which is invalid"});
}

TEST_CASE("instruction access: what it cannot place is refused with its reason, and cuts are never guessed",
          "[perf][diag][hw-resilience]")
{
    struct Refusal
    {
        const char* hex;
        const char* reason;
    };
    const Refusal refusals[] = {
        {"c4 e2 75 92 04 81", "a gather or scatter (VSIB)"},
        {"e8 00 00 00 00", "a near call or return"},
        {"c3", "a near call or return"},
        {"ff 10", "a near call or return"},
        {"64 8b 04 25 28 00 00 00", "an FS or GS segment base"},
        {"65 48 8b 04 25 30 00 00 00", "an FS or GS segment base"},
        {"0f 05", "an opcode this decoder does not list"},
        {"66 e9 00 00", "a 66 prefix on a near branch"},
        {"0f 0f c1 b4", "an AMD 3DNow! instruction"},
        {"0f ae 21", "an XSAVE area"},
        {"62 f9 7c 48 28 01", "APX's extended registers"},
        {"66 c5 f8 77", "a VEX or EVEX prefix after"},
        {"c4 e2 78 49 01", "an AMX tile configuration or tile load"},
        {"67 f3 a4", "a string instruction with a 32-bit address size"},
        {"48 8b 05 10 00", "the capture holds fewer bytes than the instruction"},
        {"c7 05 10 00 00 00 2a 00", "the capture holds fewer bytes than the instruction"},
        {"a1 88 77 66", "the capture holds fewer bytes than the instruction"},
        {"62 f1 7c", "the capture holds fewer bytes than the instruction"},
    };
    for (const Refusal& r : refusals)
    {
        const cp::InstructionAccess a = decode(r.hex);
        const cont::String reason{a.reason};
        INFO(r.hex << " -> " << reason.c_str());
        CHECK_FALSE(a.decoded);
        CHECK(a.count == 0U);
        CHECK(std::strstr(reason.c_str(), r.reason) == reason.c_str());
    }
    cp::FaultRegisters unknown = registers();
    unknown.known = false;
    CHECK_FALSE(decode("8b 01", unknown).decoded);
    u8 code[] = {0x8BU, 0x01U};
    CHECK_FALSE(cp::decode_instruction_access(cp::InstructionSet::Unknown, {code, 2U}, registers()).decoded);
    // Every cut of every listed encoding is refused, never decoded short.
    for (const Case& c : kCases)
    {
        u8 bytes[crd::crash::kCodeWindowBytes] = {};
        const u32 n = parse_hex(c.hex, bytes);
        for (u32 cut = 0U; cut < n; ++cut)
        {
            INFO(c.hex << " cut at " << cut);
            CHECK_FALSE(cp::decode_instruction_access(cp::InstructionSet::X64, {bytes, cut}, registers()).decoded);
        }
    }
}

TEST_CASE("instruction access: DIV and IDIV read their divisor register, and the 8-bit forms reach AH or SPL",
          "[perf][diag][hw-resilience]")
{
    cp::FaultRegisters r = registers();
    r.gpr[0] = 0x0000'0000'0000'1234ULL; // ax: ah 0x12, al 0x34
    r.gpr[2] = 0U;
    r.gpr[1] = 0x56U;                                       // rcx
    r.gpr[4] = 0x7FF0FFU;                                   // spl 0xFF
    const cp::InstructionAccess by_cl = decode("f6 f1", r); // div cl: 0x1234 / 0x56 = 0x36
    REQUIRE(by_cl.decoded);
    CHECK(by_cl.divides);
    CHECK(by_cl.divisor_known);
    CHECK_FALSE(by_cl.divide_faults);
    const cp::InstructionAccess by_ah = decode("f6 f4", r); // div ah: 0x1234 / 0x12 = 0x102, too wide
    REQUIRE(by_ah.decoded);
    CHECK(by_ah.divide_faults);
    const cp::InstructionAccess by_spl = decode("40 f6 f4", r); // div spl: 0x1234 / 0xFF = 0x12
    REQUIRE(by_spl.decoded);
    CHECK_FALSE(by_spl.divide_faults);
    r.gpr[1] = 0U;
    const cp::InstructionAccess by_zero = decode("48 f7 f9", r); // idiv rcx, rcx zero
    REQUIRE(by_zero.decoded);
    CHECK(by_zero.divide_faults);
    r.gpr[1] = 7U;
    r.gpr[0] = 100U;
    const cp::InstructionAccess fits = decode("48 f7 f9", r);
    CHECK_FALSE(fits.divide_faults);
    const cp::InstructionAccess from_memory = decode("48 f7 39", r); // idiv qword [rcx]
    REQUIRE(from_memory.decoded);
    CHECK(from_memory.divides);
    CHECK_FALSE(from_memory.divisor_known);
    REQUIRE(from_memory.count == 1U);
    CHECK(from_memory.accesses[0].size == 8U);
}

TEST_CASE("instruction access: the divide error's operands match wide arithmetic at every width",
          "[perf][diag][hw-resilience]")
{
    // 8 bits, exhaustively: AX by an 8-bit divisor.
    bool same = true;
    u32 first_bad = 0U;
    for (u32 dividend = 0U; dividend < 0x10000U && same; ++dividend)
    {
        for (u32 divisor = 0U; divisor < 0x100U && same; ++divisor)
        {
            const bool unsigned_faults = divisor == 0U || dividend / divisor > 0xFFU;
            const int sd = static_cast<int>(static_cast<crd::i16>(static_cast<crd::u16>(dividend)));
            const int sv = divisor >= 0x80U ? static_cast<int>(divisor) - 0x100 : static_cast<int>(divisor);
            const bool signed_faults = sv == 0 || sd / sv > 127 || sd / sv < -128;
            same = cp::x86_divide_faults(false, 8U, dividend >> 8U, dividend, divisor) == unsigned_faults &&
                   cp::x86_divide_faults(true, 8U, dividend >> 8U, dividend, divisor) == signed_faults;
            first_bad = (dividend << 8U) | divisor;
        }
    }
    INFO("first difference at dividend:divisor " << first_bad);
    CHECK(same);
    // 16 and 32 bits, by sweep against 64-bit arithmetic.
    u64 state = 0x243F6A8885A308D3ULL;
    for (u32 bits = 16U; bits <= 32U; bits += 16U)
    {
        const u64 mask = (u64{1} << bits) - 1U;
        bool agree = true;
        for (u32 i = 0U; i < 200000U && agree; ++i)
        {
            const u64 high = crd::splitmix64_next(state) & mask;
            const u64 low = crd::splitmix64_next(state) & mask;
            const u64 shift = crd::splitmix64_next(state) % bits; // small divisors as often as large ones
            u64 divisor = (crd::splitmix64_next(state) & mask) >> shift;
            divisor = (i % 7U) == 0U ? (high + (i % 3U)) & mask : divisor; // the unsigned boundary
            const u64 dividend = (high << bits) | low;
            const bool unsigned_faults = divisor == 0U || dividend / divisor > mask;
            const i64 sd = bits == 32U ? static_cast<i64>(dividend)
                                       : static_cast<i64>(static_cast<crd::i32>(static_cast<u32>(dividend)));
            const i64 sv = bits == 32U ? static_cast<i64>(static_cast<crd::i32>(static_cast<u32>(divisor)))
                                       : static_cast<i64>(static_cast<crd::i16>(static_cast<crd::u16>(divisor)));
            const i64 limit = i64{1} << (bits - 1U);
            bool signed_faults = sv == 0;
            if (!signed_faults)
            {
                // |-2^63| / |divisor| is at least 2^32 for any 32-bit divisor (and -2^63 / -1 overflows).
                signed_faults = sd == INT64_MIN ? true : (sd / sv >= limit || sd / sv < -limit);
            }
            agree = cp::x86_divide_faults(false, bits, high, low, divisor) == unsigned_faults &&
                    cp::x86_divide_faults(true, bits, high, low, divisor) == signed_faults;
            INFO("bits " << bits << " high " << high << " low " << low << " divisor " << divisor);
            CHECK(agree);
        }
    }
    // 64 bits at the boundaries: RDX:RAX by a 64-bit divisor.
    constexpr u64 min64 = u64{1} << 63U;
    const u64 all = ~u64{0};
    CHECK_FALSE(cp::x86_divide_faults(false, 64U, 6U, 0U, 7U)); // high below the divisor
    CHECK(cp::x86_divide_faults(false, 64U, 7U, 0U, 7U));       // high at the divisor
    CHECK(cp::x86_divide_faults(false, 64U, 0U, 1U, 0U));
    CHECK(cp::x86_divide_faults(true, 64U, all, min64, all));            // -2^63 / -1 = 2^63
    CHECK_FALSE(cp::x86_divide_faults(true, 64U, all, min64, 1U));       // -2^63 / 1
    CHECK_FALSE(cp::x86_divide_faults(true, 64U, 0U, min64 - 1U, 1U));   // 2^63 - 1
    CHECK(cp::x86_divide_faults(true, 64U, 0U, min64, 1U));              // 2^63
    CHECK_FALSE(cp::x86_divide_faults(true, 64U, 0U, min64, all));       // 2^63 / -1 = -2^63
    CHECK(cp::x86_divide_faults(true, 64U, all, min64 - 1U, 1U));        // -(2^63 + 1)
    CHECK_FALSE(cp::x86_divide_faults(true, 64U, 1U, min64 - 1U, 3U));   // (3 * 2^63 - 1) / 3 = 2^63 - 1
    CHECK(cp::x86_divide_faults(true, 64U, 1U, min64, 3U));              // 3 * 2^63 / 3 = 2^63
    CHECK_FALSE(cp::x86_divide_faults(true, 64U, all, min64, all - 2U)); // -2^63 / -3
#if defined(__SIZEOF_INT128__)
    // And by sweep against 128-bit arithmetic where the compiler has it.
    bool agree = true;
    for (u32 i = 0U; i < 200000U && agree; ++i)
    {
        const u64 divisor_bits = crd::splitmix64_next(state);
        const u64 divisor = (i % 5U) == 0U ? divisor_bits & 0xFFU : divisor_bits >> (i % 64U);
        // A dividend near the divisor times a random 64-bit quotient, so both sides of each limit are reached.
        const U128 q = crd::splitmix64_next(state) >> (i % 3U);
        const U128 product = q * static_cast<U128>(divisor) + (i % 11U);
        const u64 high = static_cast<u64>(product >> 64U) ^ ((i % 13U) == 0U ? all : 0U);
        const u64 low = static_cast<u64>(product);
        const U128 dividend = (static_cast<U128>(high) << 64U) | low;
        const bool unsigned_faults = divisor == 0U || dividend / divisor > all;
        bool signed_faults = divisor == 0U;
        if (!signed_faults)
        {
            const I128 sd = static_cast<I128>(dividend);
            const I128 sv = static_cast<I128>(static_cast<i64>(divisor));
            const bool overflow = sd == (static_cast<I128>(1) << 127U) && sv == -1;
            const I128 quotient = overflow ? 0 : sd / sv;
            signed_faults = overflow || quotient > INT64_MAX || quotient < INT64_MIN;
        }
        agree = cp::x86_divide_faults(false, 64U, high, low, divisor) == unsigned_faults &&
                cp::x86_divide_faults(true, 64U, high, low, divisor) == signed_faults;
        INFO("high " << high << " low " << low << " divisor " << divisor);
        CHECK(agree);
    }
#endif
}

TEST_CASE("instruction access: AArch64 loads, stores, pairs, exclusives, atomics, structures and cache maintenance",
          "[perf][diag][hw-resilience]")
{
    for (const A64Case& c : kA64Cases)
    {
        const cp::InstructionAccess a = decode_a64(c.word);
        INFO(std::hex << c.word << " -> " << cont::String{a.reason}.c_str());
        REQUIRE(a.decoded);
        CHECK(a.length == 4U);
        REQUIRE(a.count == 1U);
        CHECK(a.accesses[0].address == c.address);
        CHECK(a.accesses[0].size == c.size);
        CHECK(a.accesses[0].kinds == c.kinds);
        CHECK_FALSE(a.divides);
        CHECK_FALSE(decode_a64(c.word, 3U).decoded); // a cut word is never decoded
    }
    // No data access: data processing, branches, hints, system register moves, PRFM, UDF, SVC and BRK.
    const u32 none[] = {0x8B020020U, 0x14000010U, 0xB4000041U, 0x54000040U, 0xD503201FU, 0xD53BD040U, 0xD61F0020U,
                        0xD4000001U, 0xF9800020U, 0x00000000U, 0xD4200000U, 0x1E204020U, 0x4E208400U};
    for (const u32 word : none)
    {
        const cp::InstructionAccess a = decode_a64(word);
        INFO(std::hex << word << " -> " << cont::String{a.reason}.c_str());
        CHECK(a.decoded);
        CHECK(a.length == 4U);
        CHECK(a.count == 0U);
    }
}

TEST_CASE("instruction access: AArch64 refuses SVE, SME, MOPS, authenticated loads, GCS users and system instructions",
          "[perf][diag][hw-resilience]")
{
    struct Refusal
    {
        u32 word;
        u32 bytes;
        const char* reason;
    };
    const Refusal refusals[] = {
        {0xA5404020U, 4U, "an SVE instruction"},                                 // ld1w {z0.s}, p0/z, [x1, x0, lsl #2]
        {0xE1000020U, 4U, "an SME instruction"},                                 // ldr za[w12, 0], [x1]
        {0x19010440U, 4U, "a memory copy or set (FEAT_MOPS)"},                   // cpyfp [x0]!, [x1]!, x2!
        {0xF8200420U, 4U, "a pointer-authenticated load"},                       // ldraa x0, [x1]
        {0x94000010U, 4U, "a call, a return or a Guarded Control Stack"},        // bl
        {0xD63F0020U, 4U, "a call, a return or a Guarded Control Stack"},        // blr x1
        {0xD65F03C0U, 4U, "a call, a return or a Guarded Control Stack"},        // ret
        {0xD50B7700U, 4U, "a call, a return or a Guarded Control Stack"},        // gcspushm x0
        {0xD69F03E0U, 4U, "an exception return or a system instruction"},        // eret
        {0xD508871FU, 4U, "an exception return or a system instruction"},        // tlbi vmalle1
        {0xB9C00020U, 4U, "an encoding this decoder does not list"},             // unallocated
        {0xB9400020U, 3U, "the capture holds fewer bytes than the instruction"}, // ldr w0, [x1], cut
    };
    for (const Refusal& r : refusals)
    {
        const cp::InstructionAccess a = decode_a64(r.word, r.bytes);
        const cont::String reason{a.reason};
        INFO(std::hex << r.word << " -> " << reason.c_str());
        CHECK_FALSE(a.decoded);
        CHECK(a.count == 0U);
        CHECK(std::strstr(reason.c_str(), r.reason) == reason.c_str());
    }
}

#if defined(_WIN32)
// The CONTEXT and exception stream offsets fault classification reads on any platform, against the SDK's structures.
#if defined(_M_X64)
static_assert(offsetof(CONTEXT, ContextFlags) == 0x30U && offsetof(CONTEXT, EFlags) == 0x44U);
static_assert(offsetof(CONTEXT, Rax) == 0x78U && offsetof(CONTEXT, Rcx) == 0x80U && offsetof(CONTEXT, Rdx) == 0x88U);
static_assert(offsetof(CONTEXT, Rbx) == 0x90U && offsetof(CONTEXT, Rsp) == 0x98U && offsetof(CONTEXT, Rbp) == 0xA0U);
static_assert(offsetof(CONTEXT, Rsi) == 0xA8U && offsetof(CONTEXT, Rdi) == 0xB0U && offsetof(CONTEXT, R8) == 0xB8U);
static_assert(offsetof(CONTEXT, R15) == 0xF0U && offsetof(CONTEXT, Rip) == 0xF8U);
static_assert(CONTEXT_CONTROL == 0x00100001U && CONTEXT_INTEGER == 0x00100002U);
#elif defined(_M_ARM64)
static_assert(offsetof(CONTEXT, ContextFlags) == 0U && offsetof(CONTEXT, Cpsr) == 4U && offsetof(CONTEXT, X0) == 8U);
static_assert(offsetof(CONTEXT, Fp) == 0xF0U && offsetof(CONTEXT, Lr) == 0xF8U && offsetof(CONTEXT, Sp) == 0x100U);
static_assert(offsetof(CONTEXT, Pc) == 0x108U && CONTEXT_CONTROL == 0x00400001U && CONTEXT_INTEGER == 0x00400002U);
#endif
static_assert(offsetof(MINIDUMP_EXCEPTION_STREAM, ThreadContext) == 160U);
constexpr WORD kSdkAmd64 = PROCESSOR_ARCHITECTURE_AMD64;
constexpr WORD kSdkArm64 = PROCESSOR_ARCHITECTURE_ARM64;
static_assert(kSdkAmd64 == 9U && kSdkArm64 == 12U);
#endif
