// test_ckir_asset.cpp — CEIR-18q (D-007 §BAND-18): the `.ckir` SERIALIZED-ASSET round-trip gate.
//
// `ckir_write`/`ckir_read` (ckir_asset.hpp) are the human-authorable serialization of a CKIR program: they make a
// program editable as a `.ckir` file, so a hand-built C++ `ensure_*`/`build_*` builder can be replaced by a committed
// `.ckir` asset and DELETED. The identity contract is HASH-EXACT: `serialize_graph(ckir_read(ckir_write(g))) ==
// serialize_graph(g)` byte-for-byte (serialize_graph is the cook's content-hash source, so byte-identity == the graph
// is preserved exactly).
//
//   (1) NAME-TABLE bijection — a DUPLICATE op name would make two ops write the same token ⇒ read ambiguity; the
//       static_assert in ckir_asset.hpp already catches a count drift, this catches a collision.
//   (2) ROUND-TRIP byte-exact on a COMPUTE kernel (the Forward+ light cull — statements, ifs, buffer stores), a scale
//       kernel, and a RASTER fragment entry (out[]/StageIn/texture/sampler — the padding-heavy KEntry corners).
//   (3) MALFORMED input is REPORTED (ok=false + a byte offset), never thrown (the crd::ceir read discipline).

#include <crd/kir/ckir.hpp>
#include <crd/kir/ckir_asset.hpp>
#include <crd/kir/ckir_glsl.hpp>       // CEIR-18a-2 Stage 2b: GLSL emit check for the light-cull kernel
#include <crd/kir/ckir_hlsl.hpp>       // CEIR-19b: HLSL emit check for the worldpos kernel (both-backend compute texture sample)
#include <crd/kir/ckir_light_cull.hpp> // CEIR-18a-2: LightCullParams word indices for the .ckir device-parity setup (⛔ builder deletion pending — light_cull)
#include <crd/kir/ckir_serialize.hpp>
#include <crd/kir/ckir_technique.hpp> // CEIR-18p: body_moment_convert/blur — the library builders the moment bootstrap emits from

#include <crd/core/platform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <cstdio>  // FILE/fopen/fwrite — the [.emitckir] regen writer (the test_ckir_kernel_emit idiom)
#include <cstring>
#include <fstream> // bootstrap file IO for the .ckir emitter/identity tests (streams, not std containers)

#include <catch2/catch_test_macros.hpp>

#ifndef CRD_REPO_DIR
#define CRD_REPO_DIR "." // CMake defines the real repo root; this keeps the TU standalone-parseable
#endif

namespace kir = crd::kir;
namespace ad  = crd::kir::asset_detail;
namespace ad_tech = crd::kir::technique; // CEIR-18p: the library moment/hzb builders the bootstrap emits from

namespace
{
// out[lid] = in[lid] * scale (the D1 scale kernel).
kir::KEntry build_scale(kir::KGraph& g, double scale)
{
    const int  inbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false);
    const int  outbuf = g.buffer_decl(kir::DType::F32, 0, 1, true);
    const int  lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const auto sh1    = kir::make_shape({1});
    const int  mark   = g.kernel_stmt_mark();
    g.stmt_buffer_store(outbuf, lid,
                        g.binary(kir::KOp::Mul, g.buffer_load(inbuf, lid), g.constant(scale, sh1, kir::DType::F32)));
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 32;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

kir::KEntry build_raster_fs(kir::KGraph& g)
{
    const auto sh1 = kir::make_shape({1});
    const int  uv  = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 0);
    const int  tex = g.texture(2, 0);
    const int  smp = g.sampler(2, 1);
    const int  col = g.tex_sample(tex, smp, uv);
    const int  lit = g.binary(kir::KOp::Mul, col, g.constant(0.75, sh1, kir::DType::F32));
    kir::KEntry e;
    e.stage  = kir::KStage::Fragment;
    e.n_out  = 1;
    e.out[0] = kir::KStageOutput{lit, 0, kir::Interp::Smooth};
    return e;
}

int first_diff(const crd::containers::Array<crd::u8>& x, const crd::containers::Array<crd::u8>& y)
{
    const crd::usize n = x.size() < y.size() ? x.size() : y.size();
    for (crd::usize i = 0; i < n; ++i)
    {
        if (x[i] != y[i]) { return static_cast<int>(i); }
    }
    return x.size() == y.size() ? -1 : static_cast<int>(n);
}

// write(g,e) -> read -> re-serialize; return the FIRST differing byte vs the original blob (-1 = byte-identical).
int ckir_roundtrip_diff(kir::KGraph& g, const kir::KEntry& e, crd::memory::IAllocator* a)
{
    const auto              blob = kir::serialize_graph(g, e, a);
    crd::containers::String text = kir::ckir_write(g, e, a);
    kir::KGraph             g2(a);
    kir::KEntry             e2;
    const auto              rr = kir::ckir_read(crd::containers::StringView(text.c_str(), text.size()), g2, e2);
    if (!rr.ok) { return -2; } // read failed outright
    const auto blob2 = kir::serialize_graph(g2, e2, a);
    if (blob.size() != blob2.size()) { return static_cast<int>(blob.size() < blob2.size() ? blob.size() : blob2.size()); }
    return first_diff(blob, blob2);
}
} // namespace

TEST_CASE("CEIR-18q: the CKIR op/stmt NAME TABLES are a bijection (no duplicate token collides two ops)",
          "[kir][asset][ckir18q]")
{
    // first_index(name[v]) == v iff name[v] is UNIQUE; a duplicated name returns the earlier index ⇒ fails here (and WOULD
    // make write/read lossy — two ops writing the same token). Count/gap drift is caught by the static_asserts in the header.
    const auto first_index = [](const char* const* names, int count, const char* name) {
        for (int k = 0; k < count; ++k) { if (std::strcmp(names[k], name) == 0) { return k; } }
        return -1;
    };
    for (int v = 0; v < ad::kKOpCount; ++v) { CHECK(first_index(ad::kKOpNames, ad::kKOpCount, ad::kKOpNames[v]) == v); }
    for (int v = 0; v < ad::kKStmtCount; ++v) { CHECK(first_index(ad::kKStmtNames, ad::kKStmtCount, ad::kKStmtNames[v]) == v); }
}

TEST_CASE("CEIR-18q: ckir_write/read round-trips a scale kernel AND a raster fragment entry byte-identically",
          "[kir][asset][ckir18q]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    {
        kir::KGraph       g(&a);
        const kir::KEntry e = build_scale(g, 2.0);
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    }
    {
        kir::KGraph       g(&a);
        const kir::KEntry e = build_raster_fs(g); // out[]/StageIn/texture/sampler — the KEntry padding corners
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    }
}

TEST_CASE("CEIR-18q: malformed .ckir input is REPORTED (ok=false), never thrown", "[kir][asset][ckir18q]")
{
    crd::memory::TlsfAllocator a(1U << 20U);
    const auto                 read = [&](const char* s) {
        kir::KGraph g(&a);
        kir::KEntry e;
        return kir::ckir_read(crd::containers::StringView(s), g, e);
    };
    CHECK_FALSE(read("").ok);                       // empty
    CHECK_FALSE(read("garbage not a program").ok);  // bad magic
    CHECK_FALSE(read("KIR1 inputs 0 nodes 1 NotAnOp F32 Scalar 0 0 0 0 0").ok); // bad op name (truncated too)
}

// ── CEIR-18a-1: the committed scene_light_cull.ckir LOADS + is SELF-CONSISTENT + has the cull shape. ──────────────────
// The C++ builder `build_cluster_light_cull` is DELETED (git history is the regen escape hatch); the `.ckir` is the source
// of truth. This gate proves the COMMITTED asset parses, round-trips byte-exact (serialize(read) == serialize(read→write→
// read) — no builder RHS, no A==A), and has the cull's structural shape, so a corrupt / hand-broken file fails LOUD.
// Device correctness (list == oracle == analytic) is carried by the gpu-context cull gates on BOTH backends.
TEST_CASE("CEIR-18a-1: the committed scene_light_cull.ckir parses, round-trips + has the cull shape",
          "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/scene_light_cull.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);

    // structural pins — the cull's shape. CEIR-18a-2 Stage 2b RE-EMITTED this asset from build_cluster_light_cull in
    // the palette_snapshot header-indirection form (1 group buffer + header-word section bases, COMPONENT-WISE scalar
    // sphere-vs-AABB), re-parameterized to the SCENE's counts (16 clusters / 4 lights = kMaxScenePointLights / cap 8 ⇒
    // 250 nodes / 30 stmts; compute @64; the 30 = 25 + 5 stmt_materialize that keep the list-base + write cursor at the
    // guard-body scope, not inside a sibling store-if — the if-block shared-temp scar). Was 16/6/8 = 332 (4-flat-buffer).
    CHECK(g.size() == 250);
    CHECK(g.stmt_count() == 30);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);

    // self-consistency: the parsed graph round-trips byte-exact through ckir_write/ckir_read.
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18b: the committed scene_light_cull_3d.ckir (64 = 4×4×4 clusters) LOADS + round-trips + is DISTINCT from the 2D. ──
// The 3D clustered renderer loads a SECOND parameterization of the same builder — num_clusters 16→64 (threads stay 64 so one
// workgroup still covers every cluster). ⛔ The graph SHAPE is identical to the 2D (num_clusters is one constant's VALUE, not
// a node), so the shape pins alone cannot catch a stale COPY of the 2D asset. This gate also asserts the 3D bytes DIFFER from
// the 2D — the only guard against `cp scene_light_cull.ckir scene_light_cull_3d.ckir` (which would leave clusters 16..63
// unculled = garbage light lists for the far/near froxels). Device correctness (list == oracle) rides the gpu-context 3D kit.
TEST_CASE("CEIR-18b: the committed scene_light_cull_3d.ckir parses, round-trips + is DISTINCT from the 2D",
          "[kir][asset][ceir18b]")
{
    crd::memory::TlsfAllocator a(8U << 20U);

    const auto slurp = [&](const char* path, crd::containers::Array<char>& out) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        out.resize(static_cast<crd::usize>(sz), '\0');
        f.read(out.data(), sz);
    };

    crd::containers::Array<char> src3d(&a);
    slurp(CRD_REPO_DIR "/assets/ckir/scene_light_cull_3d.ckir", src3d);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src3d.data(), src3d.size()), g, e).ok);

    // same cull SHAPE as the 2D (num_clusters is a constant value, not extra nodes): 250 nodes / 30 stmts, compute @64.
    CHECK(g.size() == 250);
    CHECK(g.stmt_count() == 30);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);

    // ⛔ DISTINCT from the 2D asset — the num_clusters=64 guard constant must be baked, or the far/near froxels never cull.
    crd::containers::Array<char> src2d(&a);
    slurp(CRD_REPO_DIR "/assets/ckir/scene_light_cull.ckir", src2d);
    bool differs = src2d.size() != src3d.size();
    for (crd::usize i = 0; !differs && i < src2d.size(); ++i) { differs = src2d[i] != src3d[i]; }
    CHECK(differs);
}

// ── CEIR-18a-2 Stage 2b: the RE-AUTHORED light-cull builder (build_cluster_light_cull) is well-formed + regen-stable. ──
// ⛔ The 18a-1 C++ builder was lost (deleted inside an uncommitted batch — no git history). This one is re-authored from
// the ckir_render.hpp froxel helpers, KEPT IN-TREE as the `[.emitckir]` regen source, and re-parameterized to the SCENE's
// counts (16 clusters / 4 lights = kMaxScenePointLights / cap 8) + the palette_snapshot HEADER-INDIRECTION form (section
// bases read from header words, not flat buffers). This gate compiles it, pins its shape, and proves ckir_write/read
// round-trips it byte-exact (so the committed .ckir the scene loads is a faithful serialization of THIS builder).
TEST_CASE("CEIR-18a-2 Stage 2b: build_cluster_light_cull is a Compute kernel + round-trips byte-exact",
          "[kir][asset][ceir18a2]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph               g(&a);
    kir::KEntry               e;
    kir::LightCullParams      p; // scene defaults: 16 clusters, 4 lights, cap 8, null 4, words 110/113/114
    REQUIRE(kir::build_cluster_light_cull(g, e, p));

    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);
    CHECK(g.size() > 0);
    CHECK(g.stmt_count() > 0);
    // ⛔ non-vacuous: a bad param is REFUSED (num_lights > cap, or null_index < num_lights).
    {
        kir::KGraph          gb(&a);
        kir::KEntry          eb;
        kir::LightCullParams bad;
        bad.num_lights = 9U; // > cap 8
        CHECK_FALSE(kir::build_cluster_light_cull(gb, eb, bad));
    }
    // regen-stable: the builder's graph survives ckir_write → ckir_read byte-exact (the .ckir is a faithful mirror).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    UNSCOPED_INFO("[2b] light-cull nodes=" << g.size() << " stmts=" << g.stmt_count());
    // ⛔ it must EMIT to GLSL (the device path) — a compute kernel the GLSL emitter refuses would null the pipeline.
    kir::GlslKernel kern(&a);
    const bool      glsl_ok = kir::emit_compute_kernel_glsl(g, e, &a, kern);
    UNSCOPED_INFO("[2b] glsl_ok=" << glsl_ok << " src_len=" << kern.source.size());
    CHECK(glsl_ok);
}

// HIDDEN GENERATOR ([.emitckir]): regenerate the committed scene_light_cull.ckir FROM build_cluster_light_cull. Run
// explicitly: `crd-kir-tests.exe "[.emitckir]"`. This is the `[.emitckir]` regen path the tracker mandates — the builder
// stays in-tree (uncommitted-delete-loses-source scar) and the asset is a faithful `ckir_write` of it. ⛔ Regenerate
// whenever ckir_light_cull.hpp changes, then re-run the device cull gates (they load the committed asset).
TEST_CASE("REGEN: emit scene_light_cull.ckir from build_cluster_light_cull", "[.emitckir]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    // ⛔ CEIR-18b: the ONE builder emits BOTH the 2D tiled (16 clusters) and 3D clustered (64 = 4×4×4) assets — two
    // PARAMETERIZATIONS of one source (num_clusters differs; threads stay 64 = the workgroup so cluster=LocalInvocationIndex
    // still covers every cluster with one dispatch), NOT an inert copy. Re-run whenever ckir_light_cull.hpp changes, then
    // re-run the device cull gates (they load the committed assets).
    const auto emit = [&](const kir::LightCullParams& p, const char* path) {
        kir::KGraph g(&a);
        kir::KEntry e;
        REQUIRE(kir::build_cluster_light_cull(g, e, p));
        const crd::containers::String text = kir::ckir_write(g, e, &a);
        FILE*                         f    = nullptr;
#ifdef _MSC_VER
        if (fopen_s(&f, path, "wb") != 0) { f = nullptr; }
#else
        f = std::fopen(path, "wb");
#endif
        REQUIRE(f != nullptr);
        fwrite(text.c_str(), 1, text.size(), f);
        fclose(f);
    };
    kir::LightCullParams p2d; // 16 clusters / 4 lights / cap 8 / null 4 / words 110,113,114
    emit(p2d, CRD_REPO_DIR "/assets/ckir/scene_light_cull.ckir");
    kir::LightCullParams p3d = p2d;
    p3d.num_clusters         = 64U; // 4×4×4 — the ONLY change; threads stay 64 (one workgroup covers all 64 clusters)
    emit(p3d, CRD_REPO_DIR "/assets/ckir/scene_light_cull_3d.ckir");
}

// ── CEIR-18p: the committed deferred_lighting.ckir parses, round-trips + is a fragment program. ──────────────────────
// ensure_deferred_lighting_program (scene_renderer.cpp) is now a THIN asset-load — the hand-built FS is DELETED. This gate
// proves the COMMITTED asset parses + round-trips byte-exact + has the deferred FS shape. Device correctness (the lit
// verdict on both backends) is the CEIR-18c gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed deferred_lighting.ckir parses, round-trips + is a fragment program",
          "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/deferred_lighting.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18z: build the visibility-buffer FS graph (the id → graded-grey mapping the REN-38-F6 gate reads on the ordinary
// colour output: (primId+1)·0.25). ⛔ This is the LAST hand-built KGraph in scene_renderer (the band-close ensure_* audit
// found it — a builder, not a fixed pass-contract: it makes 3 authorable choices +1/·0.25/grey). The build body lives HERE
// (the [.emitckir] regen source), IN-TREE until the user commits (the uncommitted-delete-loses-source scar); `ensure_visbuffer_fs`
// is now a THIN resolve_program_text loader. create_program-native (NO lower — cast/add/mul/vec4), so ckir_read → create_program.
inline void build_visbuffer_fs(crd::kir::KGraph& g, crd::kir::KEntry& e)
{
    const auto sh   = kir::make_shape({1});
    const int  prim = g.cast(g.builtin(kir::KBuiltin::PrimitiveId), kir::DType::F32);
    const int  grey = g.binary(kir::KOp::Mul,
                               g.binary(kir::KOp::Add, prim, g.constant(1.0, sh, kir::DType::F32)),
                               g.constant(0.25, sh, kir::DType::F32));
    e        = kir::KEntry{};
    e.stage  = kir::KStage::Fragment;
    e.n_out  = 1;
    e.out[0] = {g.vec4(grey, grey, grey, g.constant(1.0, sh, kir::DType::F32)), 0};
}

TEST_CASE("REGEN: emit visbuffer_fs.ckir from build_visbuffer_fs", "[.emitckir]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph               g(&a);
    kir::KEntry               e;
    build_visbuffer_fs(g, e);
    const crd::containers::String text = kir::ckir_write(g, e, &a);
    FILE*                         f    = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, CRD_REPO_DIR "/assets/ckir/visbuffer_fs.ckir", "wb") != 0) { f = nullptr; }
#else
    f = std::fopen(CRD_REPO_DIR "/assets/ckir/visbuffer_fs.ckir", "wb");
#endif
    REQUIRE(f != nullptr);
    fwrite(text.c_str(), 1, text.size(), f);
    fclose(f);
}

// ── CEIR-18z: the committed visbuffer_fs.ckir parses, round-trips + is a fragment program. ────────────────────────────
// ensure_visbuffer_fs (scene_renderer.cpp) is now a THIN asset-load — the last hand-built FS is an authored asset. This gate
// proves the COMMITTED asset parses + round-trips byte-exact + has the visbuffer FS shape; the BEHAVIORAL verdict (the
// 0.25/0.5 graded-grey per-primitive pixels) is the REN-38-F6 device gate in tests/scene-render, which now renders THROUGH
// the loaded asset. No distinctness check — no sibling.
TEST_CASE("CEIR-18z: the committed visbuffer_fs.ckir parses, round-trips + is a fragment program",
          "[kir][asset][ceir18z]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/visbuffer_fs.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-19b: the WORLD-POSITION reconstruction kernel is an AUTHORED `.ckir` asset (assets/ckir/rt_worldpos.ckir) — the
// FIRST compute-stage texture sampler. ⛔⛔ NO C++ BUILDER (the user's #1 mandate 2026-08-16: the `.ckir` is the SOLE
// source, authored/edited DIRECTLY; the [.emitckir] build→emit pattern is RETIRED). This gate LOADS the committed asset +
// proves: it is a Compute program, round-trips byte-exact, AND EMITS on BOTH device paths (GLSL + HLSL) — a compute kernel
// with a texture sample the emitter refuses would null the pipeline (the compute-emitter-lag scar). Device correctness
// (world-pos vs a CPU-analytic probe, then the RT shadow term) is the CEIR-19b gate in tests/scene-render.
TEST_CASE("CEIR-19b: the authored rt_worldpos.ckir is a Compute program that round-trips + emits GLSL+HLSL",
          "[kir][asset][ceir19b]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/rt_worldpos.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    // ⛔ it must EMIT on BOTH device paths — a compute kernel with a texture sample the emitter refuses nulls the pipeline.
    kir::GlslKernel gk(&a);
    CHECK(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19b: the rt_composite fullscreen FS is an AUTHORED `.ckir` asset (assets/ckir/rt_composite.ckir) — samples
// scene_hdr → @output (STEP 1 passthrough; the shadow-multiply STEP 2 is a follow-up authored by EDITING the `.ckir`
// directly — add the shadow_mask_buf read + multiply nodes). ⛔⛔ NO C++ BUILDER (the user's #1 mandate: the `.ckir` is the
// sole source). 1-read fullscreen ⇒ the single texture at binding 1 + sampler at binding 2 (the one-read-binds-single-
// texture scar), NOT the bindless heap. This gate loads the committed asset + proves it is a Fragment program.
TEST_CASE("CEIR-19b: the authored rt_composite.ckir parses, round-trips + is a Fragment program", "[kir][asset][ceir19b]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/rt_composite.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);
    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
}

// ── CEIR-18p: the committed taa_resolve.ckir parses, round-trips + is a fragment program. ────────────────────────────
// ensure_taa_program (scene_renderer.cpp) is now a THIN asset-load — the hand-built FS (a large reproject + variance-clamp
// + Catmull-Rom history resolve) is DELETED. This gate proves the COMMITTED asset parses + round-trips byte-exact + has the
// TAA FS shape, and exercises the ckir form's harder corners the small fixtures never hit: tex_sample_at's texture-INDEX
// operand, int_bits_to_float, storage_load. Device correctness (the velocity/TAA render on both backends) is the REN-41
// gate in tests/scene-render; the NDC±Y sign is a RUNTIME constant (word 20) so this ONE asset is backend-neutral.
TEST_CASE("CEIR-18p: the committed taa_resolve.ckir parses, round-trips + is a fragment program",
          "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/taa_resolve.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 100); // the TAA resolve is a large graph (3x3 box + Catmull-Rom + reproject), not a trivial FS
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the committed hzb_build.ckir parses, round-trips + is a fragment program. ──────────────────────────────
// ensure_hzb_program (scene_renderer.cpp) is now a THIN asset-load — the body_hzb_build build call is DELETED from the
// wrapper (body_hzb_build STAYS in ckir_technique.hpp as the bootstrap source/oracle, deferred_shade precedent). This gate
// proves the COMMITTED asset parses + round-trips byte-exact + is the HZB FS (exercises tex_gather + Min through the form).
// Device correctness (half-res min-depth) is the REN-40-G3 gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed hzb_build.ckir parses, round-trips + is a fragment program",
          "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/hzb_build.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 5); // gather + 3 Min + vec4 wrap — small but non-trivial
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the 3 committed moment `.ckir` assets parse, round-trip + are load-time-specializable. ─────────────────
// ensure_moment_program (scene_renderer.cpp) becomes a THIN asset-load that patches the layer/dir/inv SPEC-CONSTS per
// moment_prog[kind][index] slot. This gate proves each COMMITTED asset parses + round-trips byte-exact + is a fragment
// program, AND (non-vacuity — the A==A discipline for specialization) that set_spec_const actually LANDS: two different
// inv values on the blur asset serialize to DIFFERENT graphs. Device correctness is the REN-40-D gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed moment .ckir assets parse, round-trip + specialize", "[kir][asset][ckir18q]")
{
    crd::memory::TlsfAllocator a(16U << 20U);
    const auto                 load = [&](const char* rel, kir::KGraph& g, kir::KEntry& e) {
        std::ifstream f(rel, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    };
    const char* const rels[3] = {CRD_REPO_DIR "/assets/ckir/moment_convert_evsm.ckir",
                                 CRD_REPO_DIR "/assets/ckir/moment_convert_msm.ckir",
                                 CRD_REPO_DIR "/assets/ckir/moment_blur.ckir"};
    for (const char* rel : rels)
    {
        kir::KGraph g(&a);
        kir::KEntry e;
        load(rel, g, e);
        CHECK(e.stage == kir::KStage::Fragment);
        CHECK(e.n_out == 1);
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    }
    // non-vacuity: set_spec_const(inv) with two values ⇒ DIFFERENT serialized graphs (the patch LANDS, not a no-op).
    kir::KGraph gb(&a);
    kir::KEntry eb;
    load(CRD_REPO_DIR "/assets/ckir/moment_blur.ckir", gb, eb);
    CHECK(gb.set_spec_const(ad_tech::kMomentInvSpec, 1.0 / 1024.0) >= 1); // the inv spec-const exists and was patched
    const auto blob_a = kir::serialize_graph(gb, eb, &a);
    gb.set_spec_const(ad_tech::kMomentInvSpec, 1.0 / 4096.0);
    const auto blob_b = kir::serialize_graph(gb, eb, &a);
    CHECK(blob_a.size() == blob_b.size());
    CHECK(first_diff(blob_a, blob_b) >= 0); // they DIFFER — the specialization actually changed the graph
}

// ── CEIR-18p: the committed velocity_debug.ckir parses, round-trips + is a fragment program. ─────────────────────────
// ensure_velocity_debug_program (scene_renderer.cpp) is now a THIN asset-load — the inline KGraph is DELETED. Proves the
// COMMITTED asset parses + round-trips byte-exact + is the encode FS (tex_sample + the RG motion→RGBA8 encode). Device
// correctness (static≈0.5, mover=screen delta) is the REN-41 velocity gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed velocity_debug.ckir parses, round-trips + is a fragment program", "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/velocity_debug.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(8U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(g.size() > 5);
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the committed velocity_fs_{plain,dither}.ckir parse, round-trip + differ by the dither discard. ─────────
// cook_velocity_fs (scene_renderer.cpp) is now a THIN per-mode asset-load — build_velocity_fs_cooked's build call is
// DELETED. Proves both COMMITTED assets parse + round-trip byte-exact + are fragment programs, AND (non-vacuity) the two
// variants DIFFER structurally: the dither asset carries `fe.discard_cond` (the LOD stochastic discard), the plain does
// not. The backend clip-Y sign is a spec-const the host patches. Device correctness = the REN-41 velocity gate.
TEST_CASE("CEIR-18p: the committed velocity_fs assets parse, round-trip + differ by the dither discard",
          "[kir][asset][ckir18q]")
{
    crd::memory::TlsfAllocator a(16U << 20U);
    const auto                 load = [&](const char* rel, kir::KGraph& g, kir::KEntry& e) {
        std::ifstream f(rel, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    };
    kir::KGraph gp(&a);
    kir::KEntry ep;
    load(CRD_REPO_DIR "/assets/ckir/velocity_fs_plain.ckir", gp, ep);
    CHECK(ep.stage == kir::KStage::Fragment);
    CHECK(ep.n_out == 1);
    CHECK(ep.discard_cond < 0); // plain has NO dither discard
    CHECK(ckir_roundtrip_diff(gp, ep, &a) == -1);
    kir::KGraph gd(&a);
    kir::KEntry ed;
    load(CRD_REPO_DIR "/assets/ckir/velocity_fs_dither.ckir", gd, ed);
    CHECK(ed.stage == kir::KStage::Fragment);
    CHECK(ed.n_out == 1);
    CHECK(ed.discard_cond >= 0); // the dither variant HAS the stochastic discard — the structural difference
    CHECK(ckir_roundtrip_diff(gd, ed, &a) == -1);
}

// ── CEIR-18p: the committed cluster_mesh.ckir parses, round-trips + is a MESH program. ───────────────────────────────
// ensure_cluster_mesh_program (scene_renderer.cpp) is now a THIN asset-load — the inline KStage::Mesh graph is DELETED.
// This is the FIRST mesh-stage `.ckir` — proves the COMMITTED asset parses + round-trips byte-exact + carries the mesh
// entry roots (mesh_prim = the per-primitive index triple, position = per-vertex clip, mesh_primitives). Device
// correctness (unpack == CPU oracle on a real mesh-shader device) is the REN-41 cluster-mesh gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed cluster_mesh.ckir parses, round-trips + is a mesh program", "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/cluster_mesh.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(16U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(e.stage == kir::KStage::Mesh);
    CHECK(e.mesh_prim >= 0);       // the per-primitive index-triple root round-tripped
    CHECK(e.mesh_primitives > 0U); // the declared max primitives per mesh workgroup
    CHECK(e.position >= 0);        // the per-vertex clip-position root
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the committed skin_compute.ckir parses, round-trips + is a COMPUTE kernel. ─────────────────────────────
// ensure_skin_compute_kernel (scene_renderer.cpp) is now a THIN asset-load — the inline statement graph is DELETED. This
// is the FIRST compute-stage `.ckir` — proves the COMMITTED asset parses + round-trips byte-exact + is a KStage::Compute
// kernel with a live statement body (the 2 FK passes + the kHdrGpuSkinActive gate). Device correctness (palette ==
// CPU, bit-identical) is the REN-40-F GPU-skin gate in tests/scene-render.
TEST_CASE("CEIR-18p: the committed skin_compute.ckir parses, round-trips + is a compute kernel", "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/skin_compute.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(16U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);  // the declared workgroup size
    CHECK(e.kernel_body_count > 0); // the statement body (2 FK passes + the active gate) round-tripped
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the committed palette_snapshot.ckir parses, round-trips + is a COMPUTE kernel. ─────────────────────────
// ensure_palette_snapshot_kernel (scene_renderer.cpp) is now a THIN asset-load — the inline statement graph is DELETED.
// Proves the COMMITTED asset parses + round-trips byte-exact + is a KStage::Compute kernel with a live body (the
// palette->prev_palette copy loop + the kHdrGpuSkinActive gate). Device correctness (prev_palette == last frame) is the
// CEIR-17e / REN-41 velocity gates in tests/scene-render.
TEST_CASE("CEIR-18p: the committed palette_snapshot.ckir parses, round-trips + is a compute kernel", "[kir][asset][ckir18q]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/palette_snapshot.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    REQUIRE(sz > 0);
    f.seekg(0);

    crd::memory::TlsfAllocator   a(16U << 20U);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);

    kir::KGraph g(&a);
    kir::KEntry e;
    REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 64U);  // the declared workgroup size
    CHECK(e.kernel_body_count > 0); // the copy loop + the active gate round-tripped
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
}

// ── CEIR-18p: the committed impostor_{vs,fs}_{plain,dither}.ckir — 4 assets, the FS parameterized by 18 D12 spec-consts. ──
// ensure_impostor_program (scene_renderer.cpp) is now a THIN 4-asset load — the inline KGraph builders build_impostor_vs/
// build_impostor_fs AND the emit hook are DELETED. This is the STRUCTURAL proof the device pixel gate (tests/scene-render,
// [impostor]) CANNOT see: a spec-const silently degraded to a plain constant renders identically until an app patches it.
// Pins: the FS carries EXACTLY 18 spec-consts (gt=0, mips_m1=1, 16 level offsets = ids 2..17 — the LOD config demoted to
// VALUES so the program STRUCTURE is policy-independent); the VS carries ZERO (it reads grid/tile from the kHdrAtlasDims
// header at runtime); the dither variant is STRUCTURALLY bigger (the fade varying + Bayer discard); and a patch LANDS
// (non-vacuity). ⛔ The ids are the scene_renderer.hpp contract, hardcoded here because a kir test must not depend on
// scene-render (layering) — if the ids ever move, this gate is the tripwire.
TEST_CASE("CEIR-18p: the committed impostor assets parse, round-trip; FS carries 18 spec-consts, VS carries none",
          "[kir][asset][ckir18q]")
{
    crd::memory::TlsfAllocator a(48U << 20U);
    const auto                 load = [&](const char* rel, kir::KGraph& g, kir::KEntry& e) {
        std::ifstream f(rel, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
    };
    const auto count_specs = [](const kir::KGraph& g) {
        int n = 0;
        for (int i = 0; i < g.size(); ++i) { if (kir::is_spec_const(g.node(i))) { ++n; } }
        return n;
    };
    const auto has_spec_id = [](const kir::KGraph& g, crd::u32 id) {
        for (int i = 0; i < g.size(); ++i)
        {
            const kir::KNode& nd = g.node(i);
            if (kir::is_spec_const(nd) && kir::spec_const_id(nd) == id) { return true; }
        }
        return false;
    };

    // ── VS: both variants are Vertex programs with a clip-position root, ZERO spec-consts, and differ by n_out (the
    //    dither variant emits a 4th `fade` varying). ──
    {
        kir::KGraph gvp(&a);
        kir::KEntry evp;
        load(CRD_REPO_DIR "/assets/ckir/impostor_vs_plain.ckir", gvp, evp);
        CHECK(evp.stage == kir::KStage::Vertex);
        CHECK(evp.position >= 0);
        CHECK(evp.n_out == 3);        // apx-coords + tint + atlas-base
        CHECK(count_specs(gvp) == 0); // the VS reads grid/tile from kHdrAtlasDims at runtime
        CHECK(ckir_roundtrip_diff(gvp, evp, &a) == -1);

        kir::KGraph gvd(&a);
        kir::KEntry evd;
        load(CRD_REPO_DIR "/assets/ckir/impostor_vs_dither.ckir", gvd, evd);
        CHECK(evd.stage == kir::KStage::Vertex);
        CHECK(evd.position >= 0);
        CHECK(evd.n_out == 4);        // + the fade varying (the structural dither difference)
        CHECK(count_specs(gvd) == 0);
        CHECK(ckir_roundtrip_diff(gvd, evd, &a) == -1);
    }

    // ── FS: both variants are Fragment programs, n_out==1, with EXACTLY 18 spec-consts (ids 0..17). ──
    kir::KGraph gfp(&a);
    kir::KEntry efp;
    load(CRD_REPO_DIR "/assets/ckir/impostor_fs_plain.ckir", gfp, efp);
    kir::KGraph gfd(&a);
    kir::KEntry efd;
    load(CRD_REPO_DIR "/assets/ckir/impostor_fs_dither.ckir", gfd, efd);
    for (const kir::KGraph* g : {&gfp, &gfd})
    {
        CHECK(count_specs(*g) == 18);
        for (crd::u32 id = 0; id < 18U; ++id)
        {
            INFO("spec-const id " << id);
            CHECK(has_spec_id(*g, id));
        }
    }
    CHECK(efp.stage == kir::KStage::Fragment);
    CHECK(efp.n_out == 1);
    CHECK(efp.discard_cond >= 0); // the coverage-alpha discard
    CHECK(ckir_roundtrip_diff(gfp, efp, &a) == -1);
    CHECK(efd.stage == kir::KStage::Fragment);
    CHECK(efd.n_out == 1);
    CHECK(efd.discard_cond >= 0);          // coverage OR the Bayer dither
    CHECK(gfd.size() > gfp.size());        // the dither variant is structurally larger (the Bayer discard block)
    CHECK(ckir_roundtrip_diff(gfd, efd, &a) == -1);

    // ── NON-VACUITY: patching the `gt` spec-const LANDS, and two values yield DIFFERENT serialized graphs (same size,
    //    a real payload change) — the specialization is not a silent no-op. ──
    CHECK(gfp.set_spec_const(0U /*kImpostorGtSpec*/, 512.0) >= 1);
    const auto blob_a = kir::serialize_graph(gfp, efp, &a);
    gfp.set_spec_const(0U, 2048.0);
    const auto blob_b = kir::serialize_graph(gfp, efp, &a);
    CHECK(blob_a.size() == blob_b.size());
    CHECK(first_diff(blob_a, blob_b) >= 0);
}
