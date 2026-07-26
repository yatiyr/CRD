// test_ckir_variant.cpp — REN-37.7 GATE (D-007 row 140): the VARIANT MATRIX + the übershader/variant DUAL MODE.
//
// Four claims, each one of the reasons the design says our IR dissolves the übershader-vs-variants trade:
//   1. ⭐ THE FREE COLLAPSE — at `PassType::Shadow` the surface is never consumed, so lowering DCEs all of it and
//      EVERY material cooks to the SAME program. What other engines hand-engineer as "depth-only permutations"
//      falls out of lowering + a content hash, because we compose in an IR rather than in text. MEASURED
//      (`dedup_ratio`), never assumed — a collapse that silently stopped would look like a cook-time regression
//      with nothing pointing at it.
//   2. The declared matrix ENUMERATES exactly what was declared: passes x the cartesian product of the axes.
//   3. A Filament-style NEGATIVE declaration (`variantFilter`) removes combinations BEFORE they cost anything.
//   4. EDITOR mode cooks ONE program from the SAME graph, so the übershader and the variant are one artifact at
//      two lowering levels rather than two authoring paths.

#include <crd/kir/ckir_variant.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace crd::kir;
namespace tq = crd::kir::technique;
namespace ck = crd::kir::cook;

namespace
{
// Two DIFFERENT materials: one flat, one that samples a texture and does noticeably more work. They must produce
// different Forward programs and the SAME Shadow one — that difference is the whole point of claim 1.
int surface_flat(KGraph& g, int struct_id, const ck::SurfaceInputs& in, void* /*user*/)
{
    const auto sh = make_shape({1});
    const auto k  = [&](double v) { return g.constant(v, sh, DType::F32); };
    return material::build_surface(g, struct_id, g.vec3(k(0.8), k(0.2), k(0.2)), k(0.0), k(0.6), in.world_normal,
                                   g.vec3(k(0.0), k(0.0), k(0.0)), k(1.0), k(1.0));
}
int surface_textured(KGraph& g, int struct_id, const ck::SurfaceInputs& in, void* /*user*/)
{
    const auto sh   = make_shape({1});
    const auto k    = [&](double v) { return g.constant(v, sh, DType::F32); };
    const int  tex  = g.texture(0, 1, DType::F32, TexDim::Tex2D, false, false, false);
    const int  samp = g.sampler(0, 2, false);
    const int  uv   = g.vec2(k(0.5), k(0.5));
    const int  t    = g.tex_sample(tex, samp, uv);
    return material::build_surface(g, struct_id, g.vec3(g.swizzle(t, 0), g.swizzle(t, 1), g.swizzle(t, 2)), k(1.0),
                                   k(0.3), in.world_normal, g.vec3(k(0.0), k(0.0), k(0.0)), k(1.0), k(1.0));
}

// The per-fragment inputs a renderer would supply. Built into the SAME graph the variant is cooked into, so each
// cook gets its own; the helper takes the graph.
ck::SurfaceInputs make_inputs(KGraph& g)
{
    ck::SurfaceInputs in;
    in.world_normal = g.stage_in(KType::vec(DType::F32, 3), 0, Interp::Smooth);
    const int wp    = g.stage_in(KType::vec(DType::F32, 4), 2, Interp::Smooth);
    in.world_pos    = g.vec3(g.swizzle(wp, 0), g.swizzle(wp, 1), g.swizzle(wp, 2));
    const auto sh   = make_shape({1});
    in.view_dir     = g.normalize(g.vec3(g.constant(0.0, sh, DType::F32), g.constant(0.0, sh, DType::F32),
                                         g.constant(1.0, sh, DType::F32)));
    return in;
}

// The graph-local environment builder the matrix calls once per cooked variant. ⛔ It MUST build into the graph
// it is handed: a CKIR node id indexes ONE graph, and every variant gets a fresh one.
void env_plain(KGraph& g, tq::VariantEnv& out, void* /*user*/)
{
    out.in          = make_inputs(g);
    const auto sh   = make_shape({1});
    const auto kf   = [&](double v) { return g.constant(v, sh, DType::F32); };
    out.light_dir   = g.vec3(kf(0.0), kf(-1.0), kf(0.0));
    out.light_color = g.vec3(kf(1.0), kf(1.0), kf(1.0));
}
[[nodiscard]] crd::u64 cook_one(crd::memory::IAllocator* a, const ck::MaterialTemplate& mt, const tq::Technique& t,
                                ck::PassType pass, const int* opts, int n_opts)
{
    KGraph g(a);
    KEntry e;
    const ck::SurfaceInputs in = make_inputs(g);
    const auto              sh = make_shape({1});
    const int  ldir = g.vec3(g.constant(0.0, sh, DType::F32), g.constant(-1.0, sh, DType::F32),
                             g.constant(0.0, sh, DType::F32));
    const int  lcol = g.vec3(g.constant(1.0, sh, DType::F32), g.constant(1.0, sh, DType::F32),
                             g.constant(1.0, sh, DType::F32));
    crd::f64   vals[tq::kMaxVariantOptions] = {};
    for (int i = 0; i < n_opts; ++i) { vals[i] = static_cast<crd::f64>(opts[i]); }
    const ck::VariantOptions vo{material::AlphaMode::Opaque, 0.5};
    const bool ok = tq::build_fs_for_pass(mt, t, pass, vo, in, g, e, ldir, lcol, nullptr, 0,
                                          static_cast<const crd::f64*>(vals), n_opts);
    REQUIRE(ok);
    return tq::graph_content_hash(g, e, a);
}
} // namespace

TEST_CASE("REN-37.7: the FREE COLLAPSE -- every material cooks to the SAME shadow program", "[kir][ren37][variant]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U, nullptr, "variant-collapse");
    const tq::Technique        fwd = tq::standard_forward();
    const ck::MaterialTemplate flat{&surface_flat, nullptr};
    const ck::MaterialTemplate tex{&surface_textured, nullptr};

    // ⭐ Shadow: `n_out = 0`, the surface is never consumed, `lower_entry` DCEs the ENTIRE surface computation —
    // texture fetch, parameters and interpolant reads alike. Two materials that share NOTHING in their forward
    // programs produce a BIT-IDENTICAL shadow program.
    const crd::u64 shadow_flat = cook_one(&alloc, flat, fwd, ck::PassType::Shadow, nullptr, 0);
    const crd::u64 shadow_tex  = cook_one(&alloc, tex, fwd, ck::PassType::Shadow, nullptr, 0);
    CHECK(shadow_flat == shadow_tex);

    // ...and the collapse is REAL, not the hash being blind: the same two materials at Forward differ.
    const crd::u64 fwd_flat = cook_one(&alloc, flat, fwd, ck::PassType::Forward, nullptr, 0);
    const crd::u64 fwd_tex  = cook_one(&alloc, tex, fwd, ck::PassType::Forward, nullptr, 0);
    CHECK(fwd_flat != fwd_tex);
    // ⛔ and the shadow program is not merely "some shared program" — it is a DIFFERENT one from either forward
    // variant, which is what proves the depth-only routing happened rather than the hash collapsing everything.
    CHECK(shadow_flat != fwd_flat);
    CHECK(shadow_flat != fwd_tex);

    // The content hash is a pure function of the graph: cooking the same thing twice hashes the same.
    CHECK(cook_one(&alloc, flat, fwd, ck::PassType::Forward, nullptr, 0) == fwd_flat);

    // DepthPrepass takes the same depth-only route, so it collapses onto the shadow program too — one program for
    // every opaque material across BOTH depth-only passes.
    CHECK(cook_one(&alloc, tex, fwd, ck::PassType::DepthPrepass, nullptr, 0) == shadow_flat);
}

TEST_CASE("REN-37.7: a DIFFERENT TECHNIQUE is a different program, and the key tracks it", "[kir][ren37][variant]")
{
    crd::memory::TlsfAllocator alloc(16U << 20U, nullptr, "variant-technique");
    const ck::MaterialTemplate flat{&surface_flat, nullptr};

    const crd::u64 lit    = cook_one(&alloc, flat, tq::standard_forward(), ck::PassType::Forward, nullptr, 0);
    const crd::u64 unlit  = cook_one(&alloc, flat, tq::unlit(), ck::PassType::Forward, nullptr, 0);
    // ⭐ THE LIGHTING AXIS IS REAL: one authored surface, two techniques, two genuinely different programs.
    CHECK(lit != unlit);

    // ...but the DEPTH-ONLY collapse is technique-independent: a technique is never invoked there, so swapping it
    // cannot change the shadow program. That is why a thousand materials AND every technique share ONE.
    CHECK(cook_one(&alloc, flat, tq::standard_forward(), ck::PassType::Shadow, nullptr, 0)
          == cook_one(&alloc, flat, tq::unlit(), ck::PassType::Shadow, nullptr, 0));

    // the variant KEY distinguishes them even though the CONTENT may not — two different hashes, deliberately.
    tq::VariantKey a;
    a.technique = 1;
    tq::VariantKey b;
    b.technique = 2;
    CHECK(tq::variant_key_hash(a) != tq::variant_key_hash(b));
    tq::VariantKey c;
    c.technique = 1;
    CHECK(tq::variant_key_hash(a) == tq::variant_key_hash(c));
}

namespace
{
// A Filament-style NEGATIVE declaration: this project never ships 16-tap PCF.
bool never_16_taps(const tq::VariantKey& k, void* /*user*/)
{
    for (int i = 0; i < k.n_options; ++i)
    {
        if (k.options[i] == 16) { return true; }
    }
    return false;
}
} // namespace

TEST_CASE("REN-37.7: the matrix enumerates what is DECLARED, and a negative filter removes combinations",
          "[kir][ren37][variant]")
{
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "variant-matrix");
    const ck::MaterialTemplate flat{&surface_flat, nullptr};
    const tq::Technique        fwd = tq::standard_forward();

    // Two declared axes. `standard_forward` reads neither — which is exactly what makes this a clean test of the
    // ENUMERATION and the DEDUP: 2 x 3 = 6 declared combinations that all lower to the same IR.
    const int   taps_v[3]   = {1, 4, 16};
    const int   shadows_v[2] = {0, 1};
    const tq::VariantAxis axes[2] = {
        {"pcf_taps", static_cast<const int*>(taps_v), 3, /*shape=*/true},
        {"shadows", static_cast<const int*>(shadows_v), 2, /*shape=*/false},
    };
    const ck::PassType passes[2] = {ck::PassType::Shadow, ck::PassType::Forward};

    tq::VariantRequest req;
    req.material_id = 7;
    req.material    = &flat;
    req.tech        = &fwd;
    req.passes      = static_cast<const ck::PassType*>(passes);
    req.n_passes    = 2;
    req.axes        = static_cast<const tq::VariantAxis*>(axes);
    req.n_axes      = 2;
    req.env         = &env_plain;

    // ── SHIP mode: every declared combination. 2 passes x 3 taps x 2 shadows = 12.
    {
        tq::VariantMatrix m(&alloc);
        REQUIRE(tq::cook_variant_matrix(req, &alloc, m));
        CHECK(m.variant_count() == 12U);
        CHECK(m.filtered == 0U);
        // ⭐ 12 declared combinations collapse to 2 PROGRAMS (one depth-only, one forward) because nothing this
        // technique computes actually depends on the declared options. MEASURED, not assumed.
        CHECK(m.program_count() == 2U);
        CHECK(m.dedup_ratio() == 6.0);
        // every entry maps to a program index inside the deduped set
        for (crd::usize i = 0; i < m.entries.size(); ++i) { CHECK(m.entries[i].program < m.program_count()); }
    }

    // ── the NEGATIVE declaration: "this project never ships 16-tap PCF". Filtered combinations cost NOTHING —
    // not cook time, not memory, not a hash — because the filter runs before the build.
    {
        req.filter = &never_16_taps;
        tq::VariantMatrix m(&alloc);
        REQUIRE(tq::cook_variant_matrix(req, &alloc, m));
        CHECK(m.filtered == 4U);        // 2 passes x 1 tap value x 2 shadows
        CHECK(m.variant_count() == 8U); // 12 declared - 4 filtered
        CHECK(m.program_count() == 2U); // the same two programs; the filter removes work, not capability
        req.filter = nullptr;
    }

    // ── EDITOR mode: ONE entry per pass, from the SAME graph. The übershader and the variant are one artifact at
    // two lowering levels — not two authoring paths, which is the mistake this design exists to avoid.
    {
        req.mode = tq::VariantMode::Editor;
        tq::VariantMatrix m(&alloc);
        REQUIRE(tq::cook_variant_matrix(req, &alloc, m));
        CHECK(m.variant_count() == 2U); // one per pass, not one per combination
        CHECK(m.program_count() == 2U);
        req.mode = tq::VariantMode::Ship;
    }

    // ⛔ An axis with NO declared values is a matrix with no variants — rejected rather than silently producing an
    // empty cook that would look like "nothing needed compiling".
    {
        const tq::VariantAxis bad[1] = {{"empty", nullptr, 0, false}};
        tq::VariantRequest    r      = req;
        r.axes                       = static_cast<const tq::VariantAxis*>(bad);
        r.n_axes                     = 1;
        tq::VariantMatrix m(&alloc);
        CHECK_FALSE(tq::cook_variant_matrix(r, &alloc, m));
    }
}

TEST_CASE("REN-37.7: a DECLARED option that the technique actually reads produces distinct programs",
          "[kir][ren37][variant]")
{
    // The previous case deliberately used a technique that ignores its options, to isolate enumeration + dedup.
    // This one closes the other half: when an option genuinely changes what is computed, the variants DIVERGE —
    // otherwise "dedup collapsed everything" would be indistinguishable from "the option does nothing".
    crd::memory::TlsfAllocator alloc(64U << 20U, nullptr, "variant-real-option");
    const ck::MaterialTemplate flat{&surface_flat, nullptr};
    const tq::Technique        csm = tq::forward_csm();

    // forward_csm needs its declared bindings resolved; build them into one graph and cook against it.
    const auto cook_taps = [&](int taps) {
        KGraph g(&alloc);
        KEntry e;
        const ck::SurfaceInputs in = make_inputs(g);
        const auto              sh = make_shape({1});
        const auto              kf = [&](double v) { return g.constant(v, sh, DType::F32); };
        crd::containers::Array<crd::i32> binds(&alloc);
        binds.push_back(g.texture(0, 1, DType::F32, TexDim::Tex2D, true, false, true));
        binds.push_back(g.sampler(0, 2, true));
        for (crd::u32 c = 0; c < 4U; ++c)
        {
            const int col = g.vec4(kf(1.0), kf(0.0), kf(0.0), kf(0.0));
            binds.push_back(g.mat4(col, col, col, col));
        }
        binds.push_back(kf(2048.0));
        const int ldir = g.vec3(kf(0.0), kf(-1.0), kf(0.0));
        const int lcol = g.vec3(kf(1.0), kf(1.0), kf(1.0));
        const crd::f64 vals[2] = {4.0, static_cast<crd::f64>(taps)};
        const ck::VariantOptions vo{material::AlphaMode::Opaque, 0.5};
        const bool ok = tq::build_fs_for_pass(flat, csm, ck::PassType::Forward, vo, in, g, e, ldir, lcol,
                                              binds.data(), static_cast<int>(binds.size()),
                                              static_cast<const crd::f64*>(vals), 2);
        REQUIRE(ok);
        return tq::graph_content_hash(g, e, &alloc);
    };

    const crd::u64 t1  = cook_taps(1);
    const crd::u64 t4  = cook_taps(4);
    const crd::u64 t16 = cook_taps(16);
    // ⭐ A SHAPE option unrolls a different number of taps, so each cooks to a genuinely different program.
    CHECK(t1 != t4);
    CHECK(t4 != t16);
    CHECK(t1 != t16);
    // ...and it is deterministic, so dedup is safe to rely on.
    CHECK(cook_taps(4) == t4);
}
