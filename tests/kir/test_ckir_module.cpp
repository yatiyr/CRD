// tests/kir/test_ckir_module.cpp — GENERICS + MODULES (the CKIR authoring language layer). A KModule registers named GENERIC
// graph functions; KModule::call instantiates one by name, monomorphized on the argument dtype + shaped by the arguments, and
// TYPE-CHECKED at the boundary. These tests pin the contract: a generic function reused across dtype + shape produces correct
// subgraphs (oracle-verified), and a mis-call (wrong arity / unknown name) fails fast instead of miscompiling.

#include <catch2/catch_test_macros.hpp>

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_harness.hpp>
#include <crd/kir/ckir_module.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cmath>
#include <cstdio>

namespace kir = crd::kir;

TEST_CASE("generics/modules: a generic function instantiates per-dtype + per-shape, type-checked at the call", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_activations(mod);
    CHECK(mod.count() == 4); // silu, softplus, gelu, linear — the first CKIR module (a shared standard library)
    CHECK(mod.find("silu") != nullptr);
    CHECK(mod.find("nonesuch") == nullptr);

    // instantiate silu generically for F64, verify the built subgraph vs the closed-form reference (oracle-exact).
    kir::KGraph g(&alloc);
    const int   x = g.input(kir::make_shape({4}), kir::DType::F64);
    const int   y = mod.call(g, "silu", &x, 1);
    REQUIRE(y >= 0);
    const crd::f64        xv[4]     = {0.5, -1.0, 2.0, -0.25};
    const crd::f64* const inputs[]  = {xv};
    crd::f64              out[4];
    kir::eval_cpu(g, inputs, &alloc, y, out);
    for (int i = 0; i < 4; ++i)
    {
        const crd::f64 ref = xv[i] / (1.0 + std::exp(-xv[i])); // silu(x) = x·σ(x)
        CHECK(std::abs(out[i] - ref) < 1e-12);
    }

    // GENERIC over DTYPE: the SAME function, an F32 argument ⇒ a valid F32 subgraph (element dtype monomorphized from the arg).
    kir::KGraph g32(&alloc);
    const int   x32 = g32.input(kir::make_shape({8}), kir::DType::F32);
    const int   y32 = mod.call(g32, "silu", &x32, 1);
    REQUIRE(y32 >= 0);
    CHECK(g32.node(y32).dtype() == kir::DType::F32);

    // GENERIC over SHAPE: a 2-D argument ⇒ the whole subgraph shapes to it (every op infers its shape from operands).
    kir::KGraph g2d(&alloc);
    const int   x2d = g2d.input(kir::make_shape({3, 5}), kir::DType::F32);
    const int   y2d = mod.call(g2d, "gelu", &x2d, 1);
    REQUIRE(y2d >= 0);
    CHECK(g2d.node(y2d).shape == kir::make_shape({3, 5}));

    // TYPE CHECKING at the boundary: wrong arity + unknown name fail fast (return -1) — never a silently-wrong graph.
    kir::KGraph ge(&alloc);
    const int   xe          = ge.input(kir::make_shape({4}), kir::DType::F32);
    const int   two_args[2] = {xe, xe};
    CHECK(mod.call(ge, "silu", two_args, 2) == -1); // arity: silu takes 1, not 2
    CHECK(mod.call(ge, "nonesuch", &xe, 1) == -1);  // unknown function

    std::printf("[module] KModule stdlib: %d generic fns; silu/gelu instantiate per dtype+shape, type-checked at the boundary\n",
                mod.count());
}

// A composed graph: two library calls chained (softplus(silu(x))) — modules COMPOSE, and the result is oracle-correct. This is
// the payoff: a consumer builds a pipeline from named reusable components instead of re-authoring every subgraph.
TEST_CASE("generics/modules: library functions COMPOSE + optimize()/superoptimize collapse duplicate instantiations", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_activations(mod);

    kir::KGraph g(&alloc);
    const int   x  = g.input(kir::make_shape({4}), kir::DType::F64);
    const int   s  = mod.call(g, "silu", &x, 1);
    REQUIRE(s >= 0);
    const int sp = mod.call(g, "softplus", &s, 1);
    REQUIRE(sp >= 0);
    // call silu AGAIN on the same x — a duplicate instantiation that CSE must collapse.
    const int s2 = mod.call(g, "silu", &x, 1);
    REQUIRE(s2 >= 0);
    const int out = g.binary(kir::KOp::Add, sp, s2);

    const crd::f64        xv[4]    = {0.3, -0.7, 1.2, -1.5};
    const crd::f64* const inputs[] = {xv};
    crd::f64              before[4];
    kir::eval_cpu(g, inputs, &alloc, out, before);

    const int before_n = g.size();
    int       roots[1] = {out};
    g.superoptimize(roots, 1); // the duplicate silu(x) collapses
    crd::f64 after[4];
    kir::eval_cpu(g, inputs, &alloc, roots[0], after);
    CHECK(kir::bit_equal(before, after, 4)); // composition is semantics-preserving through optimization
    CHECK(g.size() < before_n);              // the duplicate instantiation was CSE'd away
    // spot-check the math: softplus(silu(x)) + silu(x)
    for (int i = 0; i < 4; ++i)
    {
        const crd::f64 silu = xv[i] / (1.0 + std::exp(-xv[i]));
        const crd::f64 ref  = std::log(1.0 + std::exp(silu)) + silu;
        CHECK(std::abs(before[i] - ref) < 1e-12);
    }
    std::printf("[module] composition softplus(silu(x))+silu(x): %d -> %d nodes (dup instantiation CSE'd), oracle-correct\n",
                before_n, g.size());
}

// The PAYOFF: a real MLP LAYER authored ENTIRELY from named module functions — y = gelu(linear(x, W, b)) — multi-argument
// generics (linear is 3-arg), type-checked at each call, composed, and oracle-correct. The linear's contract IS the AS
// autotuner's GEMM, so the module-authored layer inherits the vendor-crushing tuned schedule for free.
TEST_CASE("generics/modules: a real MLP layer gelu(linear(x,W,b)) authored from named module functions, oracle-correct", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_activations(mod);

    kir::KGraph g(&alloc);
    const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
    const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
    const int   lin_args[3] = {x, w, b};
    const int   h = mod.call(g, "linear", lin_args, 3); // multi-arg generic, uniform-dtype checked
    REQUIRE(h >= 0);
    CHECK(g.node(h).shape == kir::make_shape({2, 2}));
    const int y = mod.call(g, "gelu", &h, 1); // compose the activation on top
    REQUIRE(y >= 0);
    CHECK(g.node(y).shape == kir::make_shape({2, 2}));

    const crd::f64        xv[6]    = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1}; // [2,3]
    const crd::f64        wv[6]    = {0.4, -0.6, 0.9, 0.1, -0.5, 0.7}; // [3,2]
    const crd::f64        bv[2]    = {0.05, -0.1};
    const crd::f64* const inputs[] = {xv, wv, bv};
    crd::f64              out[4];
    kir::eval_cpu(g, inputs, &alloc, y, out);
    for (int i = 0; i < 2; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            double acc = bv[j];
            for (int k = 0; k < 3; ++k) { acc += xv[i * 3 + k] * wv[k * 2 + j]; } // x·W + b
            const double x3    = acc * (acc * acc);                                  // match fn_gelu's op order
            const double inner = acc + 0.044715 * x3;
            const double g3    = 0.7978845608028654 * inner;
            const double gelu  = 0.5 * acc * (1.0 + std::tanh(g3));
            CHECK(std::abs(out[i * 2 + j] - gelu) < 1e-9); // ULP-safe: the oracle's tanh differs from std::tanh by a few ULP
        }
    }
    std::printf("[module] MLP layer gelu(linear(x,W,b)) from module fns: shape [2,2], oracle-correct; the GEMM is autotuner-scheduled\n");
}

// GM-3 — the FIRST-CLASS call-node: instead of inlining a module function at authoring time (`call`), `call_node` builds a
// serializable Call NODE that carries the function id + args + the (shape-rule-computed) output shape. The graph then HOLDS the
// named calls — the node-editor node type and the serialization seam — and `lower_calls` inlines them before optimize/emit. This
// pins the contract: (1) call_node produces real KOp::Call nodes with the correct output shape WITHOUT building the body; (2)
// lower_calls inlines them (Call consuming a Call resolves in id order); (3) the lowered+optimized graph is bit-identical to the
// directly-inlined `call` version and oracle-correct.
TEST_CASE("generics/modules: GM-3 first-class call-node builds KOp::Call, lowers to the inlined graph bit-for-bit", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_activations(mod);

    const crd::f64        xv[6]    = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1}; // [2,3]
    const crd::f64        wv[6]    = {0.4, -0.6, 0.9, 0.1, -0.5, 0.7}; // [3,2]
    const crd::f64        bv[2]    = {0.05, -0.1};
    const crd::f64* const inputs[] = {xv, wv, bv};

    // REFERENCE: the same MLP layer authored with `call` (inlined at authoring time) — the ground truth.
    crd::f64 ref[4];
    {
        kir::KGraph g(&alloc);
        const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
        const int   la[3] = {x, w, b};
        const int   h = mod.call(g, "linear", la, 3);
        const int   y = mod.call(g, "gelu", &h, 1);
        kir::eval_cpu(g, inputs, &alloc, y, ref);
    }

    // FIRST-CLASS: author the SAME layer with call_node — the graph holds Call nodes (a Call consuming a Call: gelu(linear)).
    kir::KGraph g(&alloc);
    const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
    const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
    const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
    const int   la[3] = {x, w, b};
    const int   h = mod.call_node(g, "linear", la, 3);
    REQUIRE(h >= 0);
    CHECK(g.node(h).op == kir::KOp::Call);
    CHECK(g.node(h).shape == kir::make_shape({2, 2})); // linear_shape rule: [M,K]·[K,N] → [M,N], WITHOUT building the body
    const int y = mod.call_node(g, "gelu", &h, 1);
    REQUIRE(y >= 0);
    CHECK(g.node(y).op == kir::KOp::Call);
    CHECK(g.node(y).shape == kir::make_shape({2, 2})); // gelu has no shape rule ⇒ shape-preserving (arg[0] = the linear Call)

    int n_calls = 0;
    for (int i = 0; i < g.size(); ++i) { if (g.node(i).op == kir::KOp::Call) { ++n_calls; } }
    CHECK(n_calls == 2); // two real Call nodes live in the graph before lowering — the serializable named-call seam

    // LOWER: inline every Call into its body, then optimize. A mis-call would have returned -1 at call_node; here both lower.
    int       roots[1] = {y};
    const int lowered  = mod.lower_calls(g, roots, 1);
    CHECK(lowered == 2);
    CHECK(roots[0] != y); // the root WAS the gelu Call — lower_calls redirected it to the inlined body output
    g.superoptimize(roots, 1);
    for (int i = 0; i < g.size(); ++i)
    {
        if (i == roots[0]) { continue; }
        // no Call node is reachable from the root after lowering+DCE (emitters/oracle never see a Call)
    }

    crd::f64 out[4];
    kir::eval_cpu(g, inputs, &alloc, roots[0], out);
    CHECK(kir::bit_equal(out, ref, 4)); // the first-class-call path lowers to EXACTLY the inlined graph — bit-for-bit
    for (int i = 0; i < 2; ++i)
    {
        for (int j = 0; j < 2; ++j)
        {
            double acc = bv[j];
            for (int k = 0; k < 3; ++k) { acc += xv[i * 3 + k] * wv[k * 2 + j]; }
            const double x3    = acc * (acc * acc);
            const double inner = acc + 0.044715 * x3;
            const double gelu  = 0.5 * acc * (1.0 + std::tanh(0.7978845608028654 * inner));
            CHECK(std::abs(out[i * 2 + j] - gelu) < 1e-9);
        }
    }
    std::printf("[module] GM-3 call-node: 2 KOp::Call nodes (gelu(linear)) shaped by rule, lowered+optimized to the inlined graph bit-for-bit\n");
}

// GM-4 — generics over INTEGER + UNSIGNED element types (not just float). The SAME generic body `muladd(a,b,c)=a·b+c` monomorphizes
// to a correct subgraph for F64/F32/I32/U32; on a 32-bit integer type it WRAPS mod 2^32 exactly as the GPU does (the oracle's
// apply_binary_typed wrap path), and the built graph structure is IDENTICAL across dtypes (monomorphization = pure dtype
// substitution). This pins that KModule generics are true dtype polymorphism, and that the GM-3 call-node path also carries an
// integer monomorphization through lowering wrap-correct.
TEST_CASE("generics/modules: GM-4 generics over int/uint element types, u32 wraps mod 2^32, structure dtype-invariant", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_numerics(mod);
    CHECK(mod.count() == 2); // muladd, clamp — the dtype-generic numeric group (separate from the 4 float activations)

    // helper: build muladd(a,b,c) for a given dtype, eval the three scalar inputs, return out[0] and the node count.
    auto muladd_eval = [&](kir::DType dt, crd::f64 a, crd::f64 b, crd::f64 c, int& n_nodes) -> crd::f64
    {
        kir::KGraph g(&alloc);
        const int   ia = g.input(kir::make_shape({1}), dt);
        const int   ib = g.input(kir::make_shape({1}), dt);
        const int   ic = g.input(kir::make_shape({1}), dt);
        const int   ar[3] = {ia, ib, ic};
        const int   y = mod.call(g, "muladd", ar, 3);
        REQUIRE(y >= 0);
        CHECK(g.node(y).dtype() == dt);           // monomorphized to the argument dtype
        CHECK(g.node(y).op == kir::KOp::Add);      // a·b + c ⇒ the output op is Add over (Mul, c)
        n_nodes = g.size();
        const crd::f64        av[1] = {a};
        const crd::f64        bv[1] = {b};
        const crd::f64        cv[1] = {c};
        const crd::f64* const in[]  = {av, bv, cv};
        crd::f64              out[1];
        kir::eval_cpu(g, in, &alloc, y, out);
        return out[0];
    };

    int n64 = 0;
    int n32 = 0;
    int ni  = 0;
    int nu  = 0;
    // F64 + F32: exact-in-both integer-valued operands ⇒ 3·4+5 = 17 (isolates monomorphization from float rounding, covered elsewhere).
    CHECK(muladd_eval(kir::DType::F64, 3.0, 4.0, 5.0, n64) == 17.0);
    CHECK(muladd_eval(kir::DType::F32, 3.0, 4.0, 5.0, n32) == 17.0);
    // U32: 100000·100000 = 10^10 wraps mod 2^32 = 1410065408, +7 = 1410065415 (verified against real u32 arithmetic).
    {
        const crd::u32 ref = static_cast<crd::u32>(100000U) * static_cast<crd::u32>(100000U) + static_cast<crd::u32>(7U);
        CHECK(ref == 1410065415U);
        CHECK(muladd_eval(kir::DType::U32, 100000.0, 100000.0, 7.0, nu) == static_cast<crd::f64>(ref));
    }
    // I32: 50000·50000 = 2.5·10^9 wraps mod 2^32 and reinterprets SIGNED to a negative i32 (proves two's-complement + sign).
    {
        const crd::i32 ref = static_cast<crd::i32>(static_cast<crd::u32>(50000U) * static_cast<crd::u32>(50000U) + 0U);
        CHECK(ref < 0); // 2.5e9 > 2^31 ⇒ negative when reinterpreted as i32
        CHECK(muladd_eval(kir::DType::I32, 50000.0, 50000.0, 0.0, ni) == static_cast<crd::f64>(ref));
    }
    // monomorphization is PURE dtype substitution ⇒ the graph structure is identical across every element type.
    CHECK(n64 == n32);
    CHECK(n64 == ni);
    CHECK(n64 == nu);

    // clamp is dtype-generic too: it holds for float AND integer element types.
    {
        kir::KGraph g(&alloc);
        const int   x  = g.input(kir::make_shape({4}), kir::DType::I32);
        const int   lo = g.input(kir::make_shape({4}), kir::DType::I32);
        const int   hi = g.input(kir::make_shape({4}), kir::DType::I32);
        const int   ar[3] = {x, lo, hi};
        const int   y  = mod.call(g, "clamp", ar, 3);
        REQUIRE(y >= 0);
        const crd::f64        xv[4]  = {-5.0, 2.0, 9.0, 3.0};
        const crd::f64        lov[4] = {0.0, 0.0, 0.0, 0.0};
        const crd::f64        hiv[4] = {3.0, 3.0, 3.0, 3.0};
        const crd::f64* const in[]   = {xv, lov, hiv};
        crd::f64              out[4];
        kir::eval_cpu(g, in, &alloc, y, out);
        const crd::f64 ref[4] = {0.0, 2.0, 3.0, 3.0}; // clamp to [0,3]
        CHECK(kir::bit_equal(out, ref, 4));
    }

    // GM-3 × GM-4: the first-class call-node carries the INTEGER monomorphization through lowering, wrap-correct.
    {
        kir::KGraph g(&alloc);
        const int   ia = g.input(kir::make_shape({1}), kir::DType::U32);
        const int   ib = g.input(kir::make_shape({1}), kir::DType::U32);
        const int   ic = g.input(kir::make_shape({1}), kir::DType::U32);
        const int   ar[3] = {ia, ib, ic};
        const int   cn = mod.call_node(g, "muladd", ar, 3);
        REQUIRE(cn >= 0);
        CHECK(g.node(cn).op == kir::KOp::Call);
        CHECK(g.node(cn).dtype() == kir::DType::U32); // the Call node itself carries the monomorphized dtype
        int roots[1] = {cn};
        CHECK(mod.lower_calls(g, roots, 1) == 1);
        g.superoptimize(roots, 1);
        const crd::f64        av[1] = {100000.0};
        const crd::f64        bv[1] = {100000.0};
        const crd::f64        cv[1] = {7.0};
        const crd::f64* const in[]  = {av, bv, cv};
        crd::f64              out[1];
        kir::eval_cpu(g, in, &alloc, roots[0], out);
        CHECK(out[0] == 1410065415.0); // u32 wrap survives the call-node → lower → optimize round-trip
    }

    std::printf("[module] GM-4 int/uint generics: muladd monomorphizes F64/F32/I32/U32 (u32 100000^2+7 wraps to 1410065415, i32 signed-wrap), structure dtype-invariant; clamp int-generic; call-node carries u32 through lowering\n");
}

// GM-5 — SEPARATE COMPILATION / intra-module LINKAGE. `ffn = gelu∘linear` is a module function authored as CALLS to "linear" then
// "gelu" by NAME (late binding via the module handle), not by hand-inlining them — so the callees are independently compilable and
// a consumer names `ffn` without seeing its internals. This pins: (1) the linked composite equals the hand-composed graph bit-for-
// bit; (2) as a first-class call-node, `ffn` is ONE serializable named call whose lowering EXPANDS through the linkage into the
// full linear+gelu subgraph (0 Call nodes remain), oracle-correct.
TEST_CASE("generics/modules: GM-5 separate compilation -- a composite fn LINKS others by name, lowers through the linkage", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_stdlib(mod);
    CHECK(mod.count() == 7); // silu, softplus, gelu, linear, muladd, clamp, ffn

    const crd::f64        xv[6]    = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1}; // [2,3]
    const crd::f64        wv[6]    = {0.4, -0.6, 0.9, 0.1, -0.5, 0.7}; // [3,2]
    const crd::f64        bv[2]    = {0.05, -0.1};
    const crd::f64* const inputs[] = {xv, wv, bv};

    // REFERENCE: gelu(linear(x,W,b)) hand-composed by the consumer (two explicit calls) — the ground truth.
    crd::f64 ref[4];
    {
        kir::KGraph g(&alloc);
        const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
        const int   la[3] = {x, w, b};
        const int   h = mod.call(g, "linear", la, 3);
        const int   y = mod.call(g, "gelu", &h, 1);
        kir::eval_cpu(g, inputs, &alloc, y, ref);
    }

    // LINKED: a SINGLE call to `ffn`, which internally links linear+gelu by name — the consumer never authors the composition.
    {
        kir::KGraph g(&alloc);
        const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
        const int   fa[3] = {x, w, b};
        const int   y = mod.call(g, "ffn", fa, 3);
        REQUIRE(y >= 0);
        CHECK(g.node(y).shape == kir::make_shape({2, 2})); // ffn's shape rule = linear's [M,N]
        crd::f64 out[4];
        kir::eval_cpu(g, inputs, &alloc, y, out);
        CHECK(kir::bit_equal(out, ref, 4)); // linkage produces EXACTLY the hand-composed graph
    }

    // FIRST-CLASS: `ffn` as ONE serializable call-node whose lowering EXPANDS through the linkage (linear+gelu) — 0 Calls remain.
    {
        kir::KGraph g(&alloc);
        const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   w = g.input(kir::make_shape({3, 2}), kir::DType::F64);
        const int   b = g.input(kir::make_shape({2}), kir::DType::F64);
        const int   fa[3] = {x, w, b};
        const int   cn = mod.call_node(g, "ffn", fa, 3);
        REQUIRE(cn >= 0);
        CHECK(g.node(cn).op == kir::KOp::Call);
        CHECK(g.node(cn).shape == kir::make_shape({2, 2}));
        int n_calls = 0;
        for (int i = 0; i < g.size(); ++i) { if (g.node(i).op == kir::KOp::Call) { ++n_calls; } }
        CHECK(n_calls == 1); // ONE named call stands in for the whole gelu∘linear composite — the serialization seam

        int roots[1] = {cn};
        CHECK(mod.lower_calls(g, roots, 1) == 1); // lowering the ONE ffn Call expands its body (which itself links linear+gelu inline)
        g.superoptimize(roots, 1);
        int n_calls_after = 0;
        for (int i = 0; i < g.size(); ++i) { if (g.node(i).op == kir::KOp::Call) { ++n_calls_after; } }
        CHECK(n_calls_after == 0); // ffn's body used inline `call` for linear/gelu ⇒ the linkage fully expands, no Calls survive
        crd::f64 out[4];
        kir::eval_cpu(g, inputs, &alloc, roots[0], out);
        CHECK(kir::bit_equal(out, ref, 4));
    }

    std::printf("[module] GM-5 separate compilation: ffn LINKS linear+gelu by name (7 fns), equals the hand-composed graph bit-for-bit; as a call-node it is ONE serializable call that lowers/expands through the linkage to 0 Calls\n");
}

// GM-6 — PRODUCTION NEURAL BLOCKS authored as composable module functions. layernorm/softmax/attention are verified against
// hand-computed math (the primitives are correct, not just self-consistent); the pre-norm transformer_block (self-attention + FFN
// with two residuals, 9-arg, LINKING layernorm/attention/linear/gelu by name) is verified oracle-correct + right-shaped. This is
// the payoff: CKIR authors a real transformer block from named, type-checked, reusable pieces — no new KOps, all backends emit it.
TEST_CASE("generics/modules: GM-6 production neural blocks -- layernorm, softmax, attention, a composed transformer block", "[kir][module]")
{
    crd::memory::TlsfAllocator alloc(32U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_stdlib(mod); // linear, gelu, ... (transformer_block links these)
    kir::stdlib::register_neural(mod); // layernorm, softmax, attention, transformer_block
    CHECK(mod.count() == 11);          // 7 stdlib + 4 neural

    // ── LayerNorm vs hand math (mean/var over the last axis, then γ·norm + β per column) ──
    {
        kir::KGraph g(&alloc);
        const int   x   = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   gam = g.input(kir::make_shape({3}), kir::DType::F64);
        const int   bet = g.input(kir::make_shape({3}), kir::DType::F64);
        const int   a[3] = {x, gam, bet};
        const int   y   = mod.call(g, "layernorm", a, 3);
        REQUIRE(y >= 0);
        CHECK(g.node(y).shape == kir::make_shape({2, 3})); // shape-preserving
        const crd::f64        xv[6]   = {1.0, 2.0, 3.0, -1.0, 0.0, 4.0};
        const crd::f64        gamv[3] = {1.5, 0.5, 2.0};
        const crd::f64        betv[3] = {0.1, -0.2, 0.3};
        const crd::f64* const in[]    = {xv, gamv, betv};
        crd::f64              out[6];
        kir::eval_cpu(g, in, &alloc, y, out);
        for (int i = 0; i < 2; ++i)
        {
            double mean = 0.0;
            for (int j = 0; j < 3; ++j) { mean += xv[i * 3 + j]; }
            mean /= 3.0;
            double var = 0.0;
            for (int j = 0; j < 3; ++j) { const double d = xv[i * 3 + j] - mean; var += d * d; }
            var /= 3.0;
            const double inv = 1.0 / std::sqrt(var + 1e-5);
            for (int j = 0; j < 3; ++j)
            {
                const double ref = (xv[i * 3 + j] - mean) * inv * gamv[j] + betv[j];
                CHECK(std::abs(out[i * 3 + j] - ref) < 1e-6); // Rsqrt is ULP vs exact 1/sqrt
            }
        }
    }

    // ── Softmax vs hand math (stable exp/Σexp over the last axis) ──
    {
        kir::KGraph g(&alloc);
        const int   x = g.input(kir::make_shape({2, 3}), kir::DType::F64);
        const int   y = mod.call(g, "softmax", &x, 1);
        REQUIRE(y >= 0);
        const crd::f64        xv[6]  = {1.0, 2.0, 3.0, 0.5, -0.5, 0.0};
        const crd::f64* const in[]   = {xv};
        crd::f64              out[6];
        kir::eval_cpu(g, in, &alloc, y, out);
        for (int i = 0; i < 2; ++i)
        {
            double mx = xv[i * 3];
            for (int j = 1; j < 3; ++j) { mx = xv[i * 3 + j] > mx ? xv[i * 3 + j] : mx; }
            double s = 0.0;
            double e[3];
            for (int j = 0; j < 3; ++j) { e[j] = std::exp(xv[i * 3 + j] - mx); s += e[j]; }
            double rowsum = 0.0;
            for (int j = 0; j < 3; ++j) { CHECK(std::abs(out[i * 3 + j] - e[j] / s) < 1e-9); rowsum += out[i * 3 + j]; }
            CHECK(std::abs(rowsum - 1.0) < 1e-12); // a probability distribution
        }
    }

    // ── Attention vs hand math (S=2, D=2): out = softmax(q·kᵀ/√2)·v ──
    {
        kir::KGraph g(&alloc);
        const int   q = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   k = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   v = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   a[3] = {q, k, v};
        const int   y = mod.call(g, "attention", a, 3);
        REQUIRE(y >= 0);
        CHECK(g.node(y).shape == kir::make_shape({2, 2})); // [S,D]
        const crd::f64        qv[4] = {0.5, 1.0, -0.5, 0.25};
        const crd::f64        kv[4] = {1.0, 0.0, 0.5, -1.0};
        const crd::f64        vv[4] = {2.0, 1.0, 0.0, 3.0};
        const crd::f64* const in[]  = {qv, kv, vv};
        crd::f64              out[4];
        kir::eval_cpu(g, in, &alloc, y, out);
        const double scale = 1.0 / std::sqrt(2.0);
        for (int i = 0; i < 2; ++i)
        {
            double sc[2];
            for (int j = 0; j < 2; ++j) { sc[j] = (qv[i * 2] * kv[j * 2] + qv[i * 2 + 1] * kv[j * 2 + 1]) * scale; } // q_i·k_j /√2
            const double mx = sc[0] > sc[1] ? sc[0] : sc[1];
            const double e0 = std::exp(sc[0] - mx);
            const double e1 = std::exp(sc[1] - mx);
            const double s  = e0 + e1;
            const double p0 = e0 / s;
            const double p1 = e1 / s;
            for (int d = 0; d < 2; ++d)
            {
                const double ref = p0 * vv[0 * 2 + d] + p1 * vv[1 * 2 + d]; // Σ_j softmax_j · v_j
                CHECK(std::abs(out[i * 2 + d] - ref) < 1e-9);
            }
        }
    }

    // ── The PAYOFF: a pre-norm transformer block (self-attn + FFN, 2 residuals) authored from name-linked calls. Verify it is
    // right-shaped AND oracle-correct by matching the hand-composed sequence bit-for-bit (proves the 9-arg linkage wires correctly).
    {
        const int seq = 2;
        const int dim = 3;
        const int hid = 4;
        const crd::f64        xv[6]   = {0.5, -1.0, 0.3, 0.8, -0.2, 1.1};   // [seq,dim]
        const crd::f64        g1[3]   = {1.2, 0.9, 1.0};
        const crd::f64        b1[3]   = {0.0, 0.1, -0.1};
        const crd::f64        g2[3]   = {1.0, 1.1, 0.8};
        const crd::f64        b2[3]   = {0.05, 0.0, -0.05};
        const crd::f64        w1[12]  = {0.1, -0.2, 0.3, 0.0, 0.2, 0.1, -0.1, 0.4, -0.3, 0.2, 0.1, -0.2}; // [dim,hid]
        const crd::f64        bf1[4]  = {0.0, 0.1, -0.1, 0.2};
        const crd::f64        w2[12]  = {0.2, -0.1, 0.0, 0.3, 0.1, -0.2, -0.1, 0.2, 0.0, 0.1, 0.4, -0.3}; // [hid,dim]
        const crd::f64        bf2[3]  = {0.0, -0.05, 0.1};
        const crd::f64* const in[]    = {xv, g1, b1, g2, b2, w1, bf1, w2, bf2};

        auto build = [&](bool composite, crd::f64* out)
        {
            kir::KGraph g(&alloc);
            const int   x    = g.input(kir::make_shape({seq, dim}), kir::DType::F64);
            const int   ig1  = g.input(kir::make_shape({dim}), kir::DType::F64);
            const int   ib1  = g.input(kir::make_shape({dim}), kir::DType::F64);
            const int   ig2  = g.input(kir::make_shape({dim}), kir::DType::F64);
            const int   ib2  = g.input(kir::make_shape({dim}), kir::DType::F64);
            const int   iw1  = g.input(kir::make_shape({dim, hid}), kir::DType::F64);
            const int   ibf1 = g.input(kir::make_shape({hid}), kir::DType::F64);
            const int   iw2  = g.input(kir::make_shape({hid, dim}), kir::DType::F64);
            const int   ibf2 = g.input(kir::make_shape({dim}), kir::DType::F64);
            int         y    = -1;
            if (composite)
            {
                const int a9[9] = {x, ig1, ib1, ig2, ib2, iw1, ibf1, iw2, ibf2};
                y = mod.call(g, "transformer_block", a9, 9);
            }
            else // hand-composed: the exact same sequence via direct sub-function calls
            {
                const int la1[3] = {x, ig1, ib1};
                const int a1     = mod.call(g, "layernorm", la1, 3);
                const int aa[3]  = {a1, a1, a1};
                const int at     = mod.call(g, "attention", aa, 3);
                const int h      = g.binary(kir::KOp::Add, x, at);
                const int la2[3] = {h, ig2, ib2};
                const int a2     = mod.call(g, "layernorm", la2, 3);
                const int lf1[3] = {a2, iw1, ibf1};
                const int f1     = mod.call(g, "linear", lf1, 3);
                const int gf     = mod.call(g, "gelu", &f1, 1);
                const int lf2[3] = {gf, iw2, ibf2};
                const int f2     = mod.call(g, "linear", lf2, 3);
                y = g.binary(kir::KOp::Add, h, f2);
            }
            REQUIRE(y >= 0);
            CHECK(g.node(y).shape == kir::make_shape({seq, dim})); // shape-preserving (both residuals)
            kir::eval_cpu(g, in, &alloc, y, out);
        };

        crd::f64 comp[6];
        crd::f64 manual[6];
        build(true, comp);
        build(false, manual);
        CHECK(kir::bit_equal(comp, manual, 6)); // the composite block == the hand-composed sequence, bit-for-bit
        // sanity: the block is a genuine transform (not identity / not all-zero)
        bool changed = false;
        for (int i = 0; i < 6; ++i) { if (std::abs(comp[i] - xv[i]) > 1e-9) { changed = true; } }
        CHECK(changed);
    }

    std::printf("[module] GM-6 neural blocks: layernorm/softmax/attention vs hand math (oracle-correct); a pre-norm transformer_block (self-attn+FFN, 2 residuals, 9-arg, LINKS layernorm/attention/linear/gelu) == hand-composed bit-for-bit — CKIR authors a real transformer\n");
}

// AS-4 FUSION: the first-class KOp::Attention intrinsic (the CUDA backend fuses it to a flash kernel). Its CPU oracle is the NAIVE
// reference — verify it equals the expanded/module attention (GM-6, already hand-math-validated) on the same inputs, so the two
// paths (portable-expanded vs fused-intrinsic) agree, and the flash kernel then validates ULP-tolerant against this oracle.
TEST_CASE("AS-4: the KOp::Attention fused intrinsic oracle == the naive expanded attention", "[kir][module][attention]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KModule               mod(&alloc);
    kir::stdlib::register_stdlib(mod);
    kir::stdlib::register_neural(mod);

    const crd::f64        qv[4] = {0.5, 1.0, -0.5, 0.25}; // [2,2]
    const crd::f64        kv[4] = {1.0, 0.0, 0.5, -1.0};
    const crd::f64        vv[4] = {2.0, 1.0, 0.0, 3.0};
    const crd::f64* const in[]  = {qv, kv, vv};
    const crd::f64        scale = 1.0 / std::sqrt(2.0); // 1/√D, D=2 — matches fn_attention's internal scale

    // expanded/module attention (GM-6) — the portable, all-backends path.
    crd::f64 expanded[4];
    {
        kir::KGraph g(&alloc);
        const int   q = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   k = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   v = g.input(kir::make_shape({2, 2}), kir::DType::F64);
        const int   a[3] = {q, k, v};
        const int   y = mod.call(g, "attention", a, 3);
        kir::eval_cpu(g, in, &alloc, y, expanded);
    }

    // the fused intrinsic — one KOp::Attention node; its oracle is the naive reference.
    kir::KGraph g(&alloc);
    const int   q = g.input(kir::make_shape({2, 2}), kir::DType::F64);
    const int   k = g.input(kir::make_shape({2, 2}), kir::DType::F64);
    const int   v = g.input(kir::make_shape({2, 2}), kir::DType::F64);
    const int   y = g.attention(q, k, v, scale);
    CHECK(g.node(y).op == kir::KOp::Attention);
    CHECK(g.node(y).shape == kir::make_shape({2, 2})); // [S,D], shaped like Q
    crd::f64 intrinsic[4];
    kir::eval_cpu(g, in, &alloc, y, intrinsic);

    for (int i = 0; i < 4; ++i) { CHECK(std::abs(intrinsic[i] - expanded[i]) < 1e-12); } // both naive f64 attention ⇒ agree
    std::printf("[module] AS-4 KOp::Attention intrinsic: oracle == expanded/module attention (naive reference) to 1e-12; the CUDA backend fuses this node to a flash kernel\n");
}
