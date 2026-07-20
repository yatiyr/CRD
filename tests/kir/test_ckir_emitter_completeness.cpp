// test_ckir_emitter_completeness.cpp — the COMPUTE-EMITTER COVERAGE GATE.
//
// ⛔ THE SCAR THIS EXISTS TO KILL. Three times now a compute kernel has failed to emit because the value emitter did not
//    handle an op the CPU oracle evaluates perfectly well:
//      2026-07-14 (B14-c SVGF)  — Exp, Pow missing
//      2026-07-15 (B16-a ocean) — Log, Tanh, Atan2 missing
//      2026-07-20 (B18-d LOD)   — Exp2 missing, while Log2 was present
//    Each time the fix was one line, and each time the note said the set was now "at parity". Prose cannot establish
//    parity. This test does: it builds a one-op kernel for EVERY unary/binary/ternary KOp and asserts the emitter lowers
//    it. A newly-added KOp that nobody wired fails HERE, at the moment it is added, instead of inside whichever slice
//    first happens to need it.
//
//    Note the pattern in the three occurrences: ops arrive in PAIRS (Exp/Exp2, Log/Log2, Sqrt/Rsqrt) and it is always the
//    SECOND of the pair that is forgotten — because the first is the one the slice being written happened to need.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_glsl.hpp>
#include <crd/kir/ckir_hlsl.hpp>

#include <crd/containers/array.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;

namespace
{
struct OpSpec
{
    kir::KOp    op;
    int         arity;   // 1, 2 or 3
    const char* name;
    bool        integer; // operates on the bit-pattern / index domain rather than floats
};

// Every arithmetic KOp a COMPUTE kernel may legitimately contain. Screen-space derivatives (DFdx/DFdy/Fwidth) are
// deliberately absent: they do not exist outside a fragment stage and must keep failing to emit.
constexpr OpSpec kOps[] = {
    {kir::KOp::Neg, 1, "Neg", false},          {kir::KOp::Abs, 1, "Abs", false},
    {kir::KOp::Sqrt, 1, "Sqrt", false},        {kir::KOp::Rsqrt, 1, "Rsqrt", false},
    {kir::KOp::Recip, 1, "Recip", false},      {kir::KOp::Exp, 1, "Exp", false},
    {kir::KOp::Exp2, 1, "Exp2", false},        {kir::KOp::Log, 1, "Log", false},
    {kir::KOp::Log2, 1, "Log2", false},        {kir::KOp::Sin, 1, "Sin", false},
    {kir::KOp::Cos, 1, "Cos", false},          {kir::KOp::Tan, 1, "Tan", false},
    {kir::KOp::Asin, 1, "Asin", false},        {kir::KOp::Acos, 1, "Acos", false},
    {kir::KOp::Atan, 1, "Atan", false},        {kir::KOp::Sinh, 1, "Sinh", false},
    {kir::KOp::Cosh, 1, "Cosh", false},        {kir::KOp::Tanh, 1, "Tanh", false},
    {kir::KOp::Floor, 1, "Floor", false},      {kir::KOp::Ceil, 1, "Ceil", false},
    {kir::KOp::Round, 1, "Round", false},      {kir::KOp::Trunc, 1, "Trunc", false},
    {kir::KOp::Fract, 1, "Fract", false},      {kir::KOp::Sign, 1, "Sign", false},
    {kir::KOp::Cbrt, 1, "Cbrt", false},        {kir::KOp::Radians, 1, "Radians", false},
    {kir::KOp::Degrees, 1, "Degrees", false},
    {kir::KOp::Add, 2, "Add", false},          {kir::KOp::Sub, 2, "Sub", false},
    {kir::KOp::Mul, 2, "Mul", false},          {kir::KOp::Div, 2, "Div", false},
    {kir::KOp::Mod, 2, "Mod", false},          {kir::KOp::Pow, 2, "Pow", false},
    {kir::KOp::Min, 2, "Min", false},          {kir::KOp::Max, 2, "Max", false},
    {kir::KOp::Atan2, 2, "Atan2", false},      {kir::KOp::Step, 2, "Step", false},
    {kir::KOp::Ldexp, 2, "Ldexp", false},
    {kir::KOp::Clamp, 3, "Clamp", false},      {kir::KOp::Mix, 3, "Mix", false},
    {kir::KOp::Fma, 3, "Fma", false},          {kir::KOp::Smoothstep, 3, "Smoothstep", false},
};

// Build a minimal kernel: out[tid] = op(in[tid], ...). One statement, so a failure is unambiguously the op.
[[nodiscard]] bool emits(crd::memory::IAllocator& alloc, const OpSpec& spec, bool glsl)
{
    kir::KGraph      g(&alloc);
    const kir::Shape shu = kir::make_shape({1});
    const auto       cu  = [&](crd::u32 v) { return g.constant(static_cast<double>(v), shu, kir::DType::U32); };

    const int in_b  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int out_b = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int tid   = g.binary(kir::KOp::Add, g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64)),
                             g.builtin(kir::KBuiltin::LocalInvocationIndex));

    const int mark = g.kernel_stmt_mark();
    const int a    = g.buffer_load(in_b, tid);
    g.stmt_materialize(a);
    const int b = g.constant(0.5, shu, kir::DType::F32);
    const int c = g.constant(0.25, shu, kir::DType::F32);

    int r = -1;
    if (spec.arity == 1) { r = g.unary(spec.op, a); }
    else if (spec.arity == 2) { r = g.binary(spec.op, a, b); }
    else { r = g.ternary(spec.op, a, b, c); }
    g.stmt_buffer_store(out_b, tid, r);

    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;

    kir::GlslKernel kern(&alloc);
    return glsl ? kir::emit_compute_kernel_glsl(g, e, &alloc, kern)
                : kir::emit_compute_kernel_hlsl(g, e, &alloc, kern);
}
} // namespace

TEST_CASE("ckir GLSL compute emitter lowers every arithmetic KOp", "[ckir][emitter][completeness]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U, nullptr, "emit-cover-glsl");
    for (const OpSpec& sp : kOps)
    {
        INFO("GLSL compute emitter cannot lower KOp::" << sp.name
                                                       << " — the CPU oracle evaluates it, so a kernel using it would "
                                                          "pass every CPU gate and then fail to emit for the GPU.");
        CHECK(emits(alloc, sp, true));
    }
}

TEST_CASE("ckir HLSL compute emitter lowers every arithmetic KOp", "[ckir][emitter][completeness]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U, nullptr, "emit-cover-hlsl");
    for (const OpSpec& sp : kOps)
    {
        INFO("HLSL compute emitter cannot lower KOp::" << sp.name
                                                       << " — the two backends must agree, or a kernel silently works "
                                                          "on Vulkan and dies on DX12.");
        CHECK(emits(alloc, sp, false));
    }
}
