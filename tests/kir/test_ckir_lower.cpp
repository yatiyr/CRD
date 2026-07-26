// test_ckir_lower.cpp — D-007 B7-a: the material LOWERING pass (crd::kir::lower). Two guarantees: (1) `classify` assigns
// each node its cheapest-correct-stage FREQUENCY (Constant/Uniform/Vertex/Fragment) — a uniform-only chain stays Uniform (the
// hoist target), anything touching FragCoord/an interpolated varying/a derivative is Fragment; (2) `lower` (const-fold → DCE
// → CSE) is ROUND-TRIP BIT-STABLE + idempotent — the optimized graph evaluates bit-identically on the CPU oracle.

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_eval.hpp>
#include <crd/kir/ckir_lower.hpp>
#include <crd/kir/ckir_nodes.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

namespace kir = crd::kir;
namespace lo  = crd::kir::lower;
namespace nd  = crd::kir::nodes;

TEST_CASE("B7-a: lower::classify assigns the cheapest-correct-stage frequency", "[kir][lower][classify]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});

    // a per-material uniform block { color3 base, float amount }, read via field_get.
    kir::KType uf[2] = {kir::KType::vec(kir::DType::F32, 3), kir::KType::make_scalar(kir::DType::F32)};
    const int  usid  = g.define_struct(uf, 2);
    const int  ub    = g.uniform_block(usid, /*set=*/2, /*binding=*/0); // set 2 = per-material
    const int  ucol  = g.field_get(ub, 0);
    const int  uamt  = g.field_get(ub, 1);
    const int  pivot = g.constant(0.5, sh, kir::DType::F32);
    const int  adj   = nd::contrast(g, ucol, uamt, pivot); // uniform+uniform+const → Uniform (the hoistable boundary)

    const int fc  = g.builtin(kir::KBuiltin::FragCoord); // Fragment
    const int fcx = g.swizzle(fc, 0);                    // Fragment
    const int mixv = g.binary(kir::KOp::Mul, fcx, uamt); // uniform × fragment → Fragment
    const int dvx  = g.dfdx(fcx);                        // fragment-forcing

    kir::KEntry fe;
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(mixv, mixv, mixv, pivot), 0};

    crd::containers::Array<lo::Frequency> freq(&alloc);
    freq.resize(static_cast<crd::usize>(g.size()), lo::Frequency::Constant);
    lo::classify(g, fe, freq.data());

    CHECK(freq[static_cast<crd::usize>(ub)] == lo::Frequency::Uniform);
    CHECK(freq[static_cast<crd::usize>(ucol)] == lo::Frequency::Uniform);
    CHECK(freq[static_cast<crd::usize>(uamt)] == lo::Frequency::Uniform);
    CHECK(freq[static_cast<crd::usize>(pivot)] == lo::Frequency::Constant);
    CHECK(freq[static_cast<crd::usize>(adj)] == lo::Frequency::Uniform); // ⭐ a uniform-only chain is Uniform → hoistable
    CHECK(freq[static_cast<crd::usize>(fc)] == lo::Frequency::Fragment);
    CHECK(freq[static_cast<crd::usize>(fcx)] == lo::Frequency::Fragment);
    CHECK(freq[static_cast<crd::usize>(mixv)] == lo::Frequency::Fragment); // uniform × fragment → Fragment
    CHECK(freq[static_cast<crd::usize>(dvx)] == lo::Frequency::Fragment);  // dFdx forces Fragment
    // the entry's finest frequency is Fragment (it writes a FragCoord-derived colour).
    CHECK(lo::entry_frequency(fe, freq.data()) == lo::Frequency::Fragment);
}

TEST_CASE("B7-a: lower::classify -- a vertex entry's stage inputs are per-vertex", "[kir][lower][classify]")
{
    crd::memory::TlsfAllocator alloc(8U << 20U);
    kir::KGraph                g(&alloc);
    const int                  vi  = g.builtin(kir::KBuiltin::VertexIndex);           // vertex builtin → Vertex
    const int                  att = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0); // VS attribute → Vertex
    (void)vi;
    (void)att;

    kir::KEntry ve;
    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(g.swizzle(att, 0), g.swizzle(att, 1), g.constant(0.0, kir::make_shape({1}), kir::DType::F32), g.constant(1.0, kir::make_shape({1}), kir::DType::F32));

    crd::containers::Array<lo::Frequency> freq(&alloc);
    freq.resize(static_cast<crd::usize>(g.size()), lo::Frequency::Constant);
    lo::classify(g, ve, freq.data());
    CHECK(freq[static_cast<crd::usize>(vi)] == lo::Frequency::Vertex);
    CHECK(freq[static_cast<crd::usize>(att)] == lo::Frequency::Vertex);
    CHECK(lo::entry_frequency(ve, freq.data()) == lo::Frequency::Vertex);
}

TEST_CASE("B7-a: lower() is round-trip BIT-STABLE + idempotent (const-fold + DCE + CSE)", "[kir][lower]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    constexpr int              nsamp = 64;
    const auto                 sh  = kir::make_shape({nsamp});

    // a graph with: a foldable constant subtree (2+3 → 5), CSE-able repeats (x*x twice), and a dead node.
    const int x   = g.input(sh, kir::DType::F64);
    const int c2  = g.constant(2.0, sh, kir::DType::F64);
    const int c3  = g.constant(3.0, sh, kir::DType::F64);
    const int cc  = g.binary(kir::KOp::Add, c2, c3); // → 5 (folds)
    const int xx1 = g.binary(kir::KOp::Mul, x, x);
    const int xx2 = g.binary(kir::KOp::Mul, x, x);   // CSE with xx1
    const int sum = g.binary(kir::KOp::Add, g.binary(kir::KOp::Add, xx1, xx2), cc);
    (void)g.unary(kir::KOp::Exp, x); // dead → DCE drops it

    crd::f64 xv[nsamp];
    for (int i = 0; i < nsamp; ++i) { xv[i] = (0.25 * i) - 8.0; }
    const crd::f64* inp[] = {xv};

    crd::f64 before[nsamp];
    kir::eval_cpu(g, inp, &alloc, sum, before);
    const int size_before = g.size();

    int root = sum;
    lo::lower(g, &root, 1);
    const int size_after = g.size();

    crd::f64 after[nsamp];
    kir::eval_cpu(g, inp, &alloc, root, after);

    int bad = 0;
    for (int i = 0; i < nsamp; ++i) { if (before[i] != after[i]) { ++bad; } } // BIT-identical
    CHECK(bad == 0);
    CHECK(size_after < size_before); // folded 2+3, merged x*x, dropped the dead exp

    // idempotent: a second lower changes nothing and preserves the value.
    int root2 = root;
    lo::lower(g, &root2, 1);
    CHECK(g.size() == size_after);
    crd::f64 after2[nsamp];
    kir::eval_cpu(g, inp, &alloc, root2, after2);
    for (int i = 0; i < nsamp; ++i) { CHECK(after2[i] == after[i]); }
}

TEST_CASE("B7-b: uniform_boundary finds the maximal uniform subexpression on the fragment path", "[kir][lower][hoist]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U);
    kir::KGraph                g(&alloc);
    const auto                 sh = kir::make_shape({1});

    kir::KType uf[2] = {kir::KType::make_scalar(kir::DType::F32), kir::KType::make_scalar(kir::DType::F32)};
    const int  usid  = g.define_struct(uf, 2);
    const int  ub    = g.uniform_block(usid, 2, 0);
    const int  ucol  = g.field_get(ub, 0);
    const int  uamt  = g.field_get(ub, 1);
    const int  pivot = g.constant(0.5, sh, kir::DType::F32);
    const int  adj   = nd::contrast(g, ucol, uamt, pivot); // a UNIFORM chain (uniform+uniform+const)

    const int fc   = g.builtin(kir::KBuiltin::FragCoord);
    const int fcx  = g.swizzle(fc, 0);
    const int frag = g.binary(kir::KOp::Mul, fcx, adj); // Fragment consumes the uniform chain `adj` → boundary

    kir::KEntry fe;
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(frag, frag, frag, g.constant(1.0, sh, kir::DType::F32)), 0};

    crd::containers::Array<lo::Frequency> freq(&alloc);
    freq.resize(static_cast<crd::usize>(g.size()), lo::Frequency::Constant);
    lo::classify(g, fe, freq.data());

    crd::containers::Array<crd::u8> seen(&alloc);
    seen.resize(static_cast<crd::usize>(g.size()), 0);
    crd::containers::Array<int> out(&alloc);
    out.resize(static_cast<crd::usize>(g.size()), -1);
    const int cnt = lo::uniform_boundary(g, fe, freq.data(), seen.data(), out.data(), g.size());

    // exactly the MAXIMAL uniform subexpression (`adj`) is the boundary — not its uniform sub-parts (ucol/uamt), whose
    // consumers are still Uniform, nor the fragment nodes.
    CHECK(cnt == 1);
    CHECK(out[0] == adj);
}

TEST_CASE("B7-b: specialize collapses a static switch to a variant, bit-identical to the runtime branch", "[kir][lower][variant]")
{
    constexpr int nsamp = 32;
    const auto    run   = [&](double optval, auto expected) {
        crd::memory::TlsfAllocator alloc(16U << 20U);
        kir::KGraph                g(&alloc);
        const auto                 sh = kir::make_shape({nsamp});
        const int                  x  = g.input(sh, kir::DType::F64);
        const int                  option = g.constant(0.5, sh, kir::DType::F64); // the ShaderOption selector (pinned below)
        const int                  cond   = g.binary(kir::KOp::CmpGt, option, g.constant(0.5, sh, kir::DType::F64));
        const int                  ba     = g.binary(kir::KOp::Mul, x, g.constant(2.0, sh, kir::DType::F64)); // 2x
        const int                  bb     = g.binary(kir::KOp::Add, x, g.constant(10.0, sh, kir::DType::F64)); // x+10
        const int                  sel    = g.select(cond, ba, bb);

        crd::f64 xv[nsamp];
        for (int i = 0; i < nsamp; ++i) { xv[i] = (0.5 * i) - 8.0; }
        const crd::f64* inp[] = {xv};

        const int size_before = g.size();
        int       root        = sel;
        lo::specialize(g, option, optval, &root, 1);
        const int size_after = g.size();

        crd::f64 o[nsamp];
        kir::eval_cpu(g, inp, &alloc, root, o);
        int bad = 0;
        for (int i = 0; i < nsamp; ++i) { if (o[i] != expected(xv[i])) { ++bad; } }
        CHECK(bad == 0);                 // the variant is BIT-identical to the chosen branch
        CHECK(size_after < size_before); // the dead branch was eliminated
    };
    run(1.0, [](crd::f64 x) { return x * 2.0; });   // option > 0.5 → branch A (2x)
    run(0.0, [](crd::f64 x) { return x + 10.0; });  // option ≤ 0.5 → branch B (x+10)
}

// ⛔⛔ B7 REGRESSION GATE: a MEMORY READ must never be const-folded, however constant its INDEX is.
// `StorageLoad`'s ONLY operand is the index, so `sbuf.data[22]` looked like an all-constant expression and the
// const-folder replaced the READ with a literal. Every LOWERED shader that reads a storage buffer at a fixed
// slot was silently miscompiled — the scene's cooked forward variant reads its light direction at word 22 and
// rendered BLACK, while the un-lowered hand-written shader using the same reads was fine. That asymmetry is
// what makes this class of bug so hard to see, and it also broke B7's documented "round-trip bit-stable"
// invariant. `BufferLoad`/`SharedLoad` escaped only because their first operand is a resource declaration.
TEST_CASE("B7 GATE: lowering never const-folds a memory read with a constant index", "[kir][lower][b7]")
{
    crd::memory::TlsfAllocator alloc(4U << 20U);
    crd::kir::KGraph           g(&alloc);

    const auto sh  = crd::kir::make_shape({1});
    const int  idx = g.constant(22.0, sh, crd::kir::DType::U32); // a LITERAL index — the trap
    const int  ld  = g.storage_load(idx);
    const int  val = g.int_bits_to_float(g.cast(ld, crd::kir::DType::I32));

    crd::kir::KEntry e;
    e.stage  = crd::kir::KStage::Fragment;
    e.n_out  = 1;
    e.out[0] = {g.vec4(val, val, val, g.constant(1.0, sh, crd::kir::DType::F32)), 0};

    crd::kir::lower::lower_entry(g, e);

    // the load must SURVIVE lowering: if the folder ate it, the shader reads a compile-time literal instead of
    // memory, and every uniform/header value the material depends on becomes whatever the folder invented.
    bool has_load = false;
    for (int i = 0; i < g.size(); ++i)
    {
        if (g.node(i).op == crd::kir::KOp::StorageLoad) { has_load = true; }
    }
    CHECK(has_load);
}
