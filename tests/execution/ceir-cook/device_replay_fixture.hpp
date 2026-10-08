#pragma once

// DIAG.9a -- the shared fixture of the device record tests (device-free in crd-ceir-cook-tests, and the Vulkan and
// DX12 legs in the ceir-gpu device tests): a two-dispatch device program authored as text under a named file and
// cooked, the CKIR text of its two kernels (built here and written with ckir_write), and its initial buffers.
//
//   @scale  b[i] = a[i] * 3 + 0.25             (exact in f32 for the chosen inputs, so every device agrees)
//   @wave   c[i] = exp(b[i]) * a[i];  d[i] = i  (exp is a device approximation: a GPU may differ from the reference)
//
// Four workgroups of 64 threads cover the 256 elements of each buffer. The inputs a[i] = 0.25 + i / 256 are positive,
// so the product never cancels, and b = 1 + 3i / 256 stays in [1, 4.75]. The declared envelope is a budget from the
// APIs' precision rules, not a measurement: Vulkan bounds exp(x) at 3 + 2|x| ULP (at most 12.5 here), D3D at about
// 2^-21 relative (4 ULP); the product and the reference's own rounding add about one more. kDeclaredUlps (32) is that
// budget with headroom.
// Expected positions come from scanning the text, never from the parser.

#include <crd/ceir/context.hpp>
#include <crd/ceir/cook/device_replay.hpp>
#include <crd/ceir/cook/program_cook.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/compute_ops.hpp>
#include <crd/ceir/gen/resource_ops.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp> // ckir_write

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace crd::ceir_test::device_replay
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::Array;
using crd::containers::ConstSpan;
using crd::containers::String;
using crd::containers::StringView;

inline constexpr const char* kFile         = "programs/diag/device_replay.ceir";
inline constexpr u32         kLocal        = 64U;
inline constexpr u32         kGroups       = 4U;
inline constexpr u32         kN            = kLocal * kGroups;
inline constexpr u32         kBuffers      = 4U;
inline constexpr u32         kDeclaredUlps = 32U;

inline constexpr const char* kProgram =
    "module {\n"
    "  ^bb0:\n"
    "    %0 = arith.const() {value = 4} : !index\n"
    "    %1 = arith.const() {value = 1} : !index\n"
    "    %2 = resource.declare() : !buffer<plain,!f32>\n"
    "    %3 = resource.declare() : !buffer<plain,!f32>\n"
    "    %4 = resource.declare() : !buffer<plain,!f32>\n"
    "    %5 = resource.declare() : !buffer<plain,!u32>\n"
    "    compute.dispatch(%0, %1, %1, %2, %3) {access = \"r,w\", kernel = @scale}\n"
    "    compute.dispatch(%0, %1, %1, %3, %2, %4, %5) {access = \"r,r,w,w\", kernel = @wave}\n"
    "}\n";

// The same program with one more constant before the dispatches: another content hash, every dispatch a line lower.
inline constexpr const char* kProgramMoved =
    "module {\n"
    "  ^bb0:\n"
    "    %0 = arith.const() {value = 4} : !index\n"
    "    %1 = arith.const() {value = 1} : !index\n"
    "    %2 = resource.declare() : !buffer<plain,!f32>\n"
    "    %3 = resource.declare() : !buffer<plain,!f32>\n"
    "    %4 = resource.declare() : !buffer<plain,!f32>\n"
    "    %5 = resource.declare() : !buffer<plain,!u32>\n"
    "    %6 = arith.const() {value = 7} : !index\n"
    "    compute.dispatch(%0, %1, %1, %2, %3) {access = \"r,w\", kernel = @scale}\n"
    "    compute.dispatch(%0, %1, %1, %3, %2, %4, %5) {access = \"r,r,w,w\", kernel = @wave}\n"
    "}\n";

inline constexpr const char* kScaleDispatch = "compute.dispatch(%0, %1, %1, %2, %3)";
inline constexpr const char* kWaveDispatch  = "compute.dispatch(%0, %1, %1, %3, %2, %4, %5)";

inline void register_dialects(ceir::Context& ctx)
{
    (void)ceir::arith::register_arith_ops(ctx);
    (void)ceir::func::register_dialect(ctx);
    (void)ceir::resource::register_resource_ops(ctx);
    (void)ceir::compute::register_compute_ops(ctx);
}

inline void registrar(ceir::Context& ctx, void* /*user*/)
{
    register_dialects(ctx);
}

struct TextPos
{
    u32 line = 0U;
    u32 col  = 0U;
};
// The 1-based line holding the only occurrence of `needle`, and the column of that line's first non-blank character.
inline TextPos locate(StringView text, StringView needle)
{
    const usize at = text.find(needle);
    REQUIRE(at != StringView::npos);
    REQUIRE(text.find(needle, at + 1U) == StringView::npos);
    TextPos p;
    p.line           = 1U;
    usize line_start = 0U;
    for (usize i = 0U; i < at; ++i)
    {
        if (text[i] == '\n')
        {
            ++p.line;
            line_start = i + 1U;
        }
    }
    usize first = line_start;
    while (text[first] == ' ')
    {
        ++first;
    }
    p.col = static_cast<u32>(first - line_start + 1U);
    return p;
}

// A kernel's global index: WorkgroupIndex * kLocal + LocalInvocationIndex (u32).
inline int global_index(kir::KGraph& g)
{
    const kir::Shape one   = kir::make_shape({1});
    const int        local = g.constant(static_cast<double>(kLocal), one, kir::DType::U32);
    return g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), local),
                    g.builtin(kir::KBuiltin::LocalInvocationIndex));
}

inline kir::KEntry kernel_entry(const kir::KGraph& g, int mark)
{
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = kLocal;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// @scale: b[i] = a[i] * mul + add. Bindings: a (0, read), b (1, write).
inline String scale_kernel(memory::IAllocator* alloc, double mul = 3.0, double add = 0.25)
{
    kir::KGraph      g(alloc);
    const kir::Shape one  = kir::make_shape({1});
    const int        a    = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int        b    = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int        mark = g.kernel_stmt_mark();
    const int        i    = global_index(g);
    const int        ax   = g.binary(kir::KOp::Mul, g.buffer_load(a, i), g.constant(mul, one, kir::DType::F32));
    g.stmt_buffer_store(b, i, g.binary(kir::KOp::Add, ax, g.constant(add, one, kir::DType::F32)));
    return kir::ckir_write(g, kernel_entry(g, mark), alloc);
}

// @wave: c[i] = exp(b[i]) * a[i] * gain; d[i] = i. Bindings: b (0, read), a (1, read), c (2, write), d (3, write).
inline String wave_kernel(memory::IAllocator* alloc, double gain = 1.0)
{
    kir::KGraph      g(alloc);
    const kir::Shape one  = kir::make_shape({1});
    const int        b    = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int        a    = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int        c    = g.buffer_decl(kir::DType::F32, 0, 2, true);
    const int        d    = g.buffer_decl(kir::DType::U32, 0, 3, true);
    const int        mark = g.kernel_stmt_mark();
    const int        i    = global_index(g);
    int              v    = g.binary(kir::KOp::Mul, g.unary(kir::KOp::Exp, g.buffer_load(b, i)), g.buffer_load(a, i));
    if (gain != 1.0)
    {
        v = g.binary(kir::KOp::Mul, v, g.constant(gain, one, kir::DType::F32));
    }
    g.stmt_buffer_store(c, i, v);
    g.stmt_buffer_store(d, i, i);
    return kir::ckir_write(g, kernel_entry(g, mark), alloc);
}

inline u32 bits_of(float f)
{
    u32 w = 0U;
    std::memcpy(&w, &f, sizeof(w));
    return w;
}

inline float float_of(u32 w)
{
    float f = 0.0F;
    std::memcpy(&f, &w, sizeof(f));
    return f;
}

// The authored program, cooked, with its kernels and initial buffers.
struct Authored
{
    explicit Authored(memory::IAllocator* alloc) : blob(alloc), scale(alloc), wave(alloc) {}

    Array<u8> blob;
    String    scale;
    String    wave;
    u32       a[kN]{};
    u32       b[kN]{};
    u32       c[kN]{};
    u32       d[kN]{};

    ConstSpan<u32>                 buffers[kBuffers];
    ceir::cook::DeviceKernelSource kernels[2];

    [[nodiscard]] ceir::cook::DeviceRecordRequest request(ceir::cook::DeviceEnvelope envelope) const
    {
        ceir::cook::DeviceRecordRequest r;
        r.blob     = {blob.data(), blob.size()};
        r.path     = StringView{kFile};
        r.kernels  = {kernels, 2U};
        r.buffers  = {buffers, kBuffers};
        r.envelope = envelope;
        return r;
    }
};

// `text` cooked under kFile in a fresh Context.
inline Array<u8> cook(const char* text, memory::IAllocator* alloc)
{
    ceir::Context ctx(alloc);
    register_dialects(ctx);
    ceir::cook::CookResult cr =
        ceir::cook::cook_program_text(ctx, StringView{text}, StringView{kFile}, 1U, alloc, alloc);
    REQUIRE(cr.ok());
    return std::move(cr.blob);
}

inline void author(Authored& out, memory::IAllocator* alloc)
{
    out.blob  = cook(kProgram, alloc);
    out.scale = scale_kernel(alloc);
    out.wave  = wave_kernel(alloc);
    for (u32 i = 0U; i < kN; ++i)
    {
        out.a[i] = bits_of(0.25F + static_cast<float>(i) / 256.0F);
        out.b[i] = bits_of(-1.0F); // overwritten by @scale
        out.c[i] = bits_of(-1.0F); // overwritten by @wave
        out.d[i] = 0xFFFFFFFFU;    // overwritten by @wave
    }
    out.buffers[0] = {out.a, kN};
    out.buffers[1] = {out.b, kN};
    out.buffers[2] = {out.c, kN};
    out.buffers[3] = {out.d, kN};
    out.kernels[0] = {StringView{"scale"}, StringView{out.scale.data(), out.scale.size()}};
    out.kernels[1] = {StringView{"wave"}, StringView{out.wave.data(), out.wave.size()}};
}
} // namespace crd::ceir_test::device_replay
