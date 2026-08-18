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

// CEIR-19c: the inline INLINE-RAY-QUERY witness graph — for the SERIALIZER test ONLY (proves ckir_write/read round-trips the
// RayHitResult node + the TraceRayHit stmt's 9 global-ext operands, the encodings every stage-2 wavefront kernel needs). ⛔
// This is a SERIALIZER exercise (the test_lower.cpp precedent), NOT the source of assets/ckir/rt_witness.ckir — nothing
// generates the committed asset from this, and no test compares the asset's emit to it (the mandate-#1 coupling line). The
// authored .ckir is verified independently by its own load/roundtrip/emit gate. Binding contract: TLAS@0, rays@1 (6 f32/thread:
// origin+dir), hit-t@2 (F32), prim@3 (U32 — 0xFFFFFFFF miss; NEVER an F32 buffer, the sentinel is unrepresentable in F32).
kir::KEntry build_rt_witness_inline(kir::KGraph& g)
{
    const auto sh    = kir::make_shape({1});
    const auto cf    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto cu    = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  as    = g.accel_struct_decl(0, 0);
    const int  rays  = g.buffer_decl(kir::DType::F32, 0, 1, false);
    const int  out_t = g.buffer_decl(kir::DType::F32, 0, 2, true);
    const int  out_p = g.buffer_decl(kir::DType::U32, 0, 3, true);
    const int  mark  = g.kernel_stmt_mark();
    const int  tid   = g.binary(kir::KOp::Add,
                                g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64U)),
                                g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int  base  = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto ld    = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const kir::KGraph::RtHit h = g.trace_ray_hit(as, ld(0U), ld(1U), ld(2U), ld(3U), ld(4U), ld(5U), cf(0.001), cf(1.0e30));
    g.stmt_buffer_store(out_t, tid, h.t);    // F32 distance
    g.stmt_buffer_store(out_p, tid, h.prim); // U32 primitive index (the bit-exact decision target)
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64U;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// CEIR-19c STAGE 2: the inline SERIAL-COMPACT witness graph (serializer test ONLY — decoupled from the committed asset, the
// mandate-#1 coupling line). A BRANCHLESS stream-compaction over `n` hit-flags: an UNCONDITIONAL store to compacted[cursor],
// then cursor += flag (0/1) — a miss's tentative write is overwritten by the next hit or sits harmlessly beyond count (no
// inner If ⇒ no if-block-shared-temp scar). local_size=1 (ONE thread ⇒ serial, no race, no tid guard). ⛔ DEAD TLAS@0: the
// bridge's rt.ray_query needs a %tlas operand at descriptor 0, but the compact never traces (the decl only forces #version
// 460 + provides the binding). Bindings: TLAS@0 (dead), hit_flags@1 (read), compacted@2 (write), count@3 (write).
kir::KEntry build_compact_inline(kir::KGraph& g, crd::u32 n)
{
    const auto sh        = kir::make_shape({1});
    const auto cu        = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  as        = g.accel_struct_decl(0, 0);                   // DEAD TLAS@0 — never traced
    const int  flags     = g.buffer_decl(kir::DType::U32, 0, 1, false); // hit_flags@1 (read)
    const int  compacted = g.buffer_decl(kir::DType::U32, 0, 2, true);  // compacted@2 (write)
    const int  count     = g.buffer_decl(kir::DType::U32, 0, 3, true);  // count@3 (write)
    (void)as;                                                           // bound at descriptor 0 by the bridge; unread here
    const int mark   = g.kernel_stmt_mark();
    int       cursor = cu(0U);
    for (crd::u32 i = 0; i < n; ++i)
    {
        g.stmt_materialize(cursor);                        // freeze cursor_i (the scene_light_cull idiom)
        const int fi = g.buffer_load(flags, cu(i));
        g.stmt_buffer_store(compacted, cursor, cu(i));     // UNCONDITIONAL store at cursor (overwritten unless committed)
        cursor = g.binary(kir::KOp::Add, cursor, fi);      // advance by flag (0/1) — only a hit commits the store
    }
    g.stmt_materialize(cursor);
    g.stmt_buffer_store(count, cu(0U), cursor);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 1U; // ONE thread — serial deterministic compact
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// CEIR-19c STAGE 2: the inline TRACE witness graph (serializer test ONLY — decoupled from assets/ckir/wavefront_trace.ckir).
// The wavefront's PRIMARY-ray trace: one thread per ray, inline-ray-query the REAL TLAS, write hit_flag (U32: 1 hit / 0 miss,
// derived from the prim sentinel — the DECISION int the compact consumes) + hit_t (F32 distance, for shade's o+t·d re-derive).
// Bindings: TLAS@0 (REAL), rays@1 (read, 6 f32/thread: origin+dir), hit_flag@2 (write, U32), hit_t@3 (write, F32). local_size=64.
kir::KEntry build_trace_inline(kir::KGraph& g)
{
    const auto sh   = kir::make_shape({1});
    const auto cf   = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto cu   = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  as   = g.accel_struct_decl(0, 0);                   // REAL TLAS@0 — traced
    const int  rays = g.buffer_decl(kir::DType::F32, 0, 1, false); // rays@1 (read)
    const int  flag = g.buffer_decl(kir::DType::U32, 0, 2, true);  // hit_flag@2 (write, 1/0)
    const int  outt = g.buffer_decl(kir::DType::F32, 0, 3, true);  // hit_t@3 (write)
    const int  mark = g.kernel_stmt_mark();
    const int  tid  = g.binary(kir::KOp::Add,
                               g.binary(kir::KOp::Mul, g.builtin(kir::KBuiltin::WorkgroupIndex), cu(64U)),
                               g.builtin(kir::KBuiltin::LocalInvocationIndex));
    const int  base = g.binary(kir::KOp::Mul, tid, cu(6U));
    const auto ld   = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const kir::KGraph::RtHit h = g.trace_ray_hit(as, ld(0U), ld(1U), ld(2U), ld(3U), ld(4U), ld(5U), cf(0.001), cf(1.0e30));
    // hit_flag = (prim != 0xFFFFFFFF) ? 1 : 0 — the decision int; u32 CmpNe wraps identically GPU/oracle.
    const int hitflag = g.cast(g.binary(kir::KOp::CmpNe, h.prim, cu(0xFFFFFFFFU)), kir::DType::U32);
    g.stmt_buffer_store(flag, tid, hitflag);
    g.stmt_buffer_store(outt, tid, h.t);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 64U;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// CEIR-19c STAGE 2: the inline SHADE witness graph (serializer test ONLY — decoupled from assets/ckir/wavefront_shade.ckir).
// One thread per COMPACTED hit slot: re-derive the hit position o+t·d from the ray buffer + hit_t (NO stored hitpos — smaller
// queue; t crosses as an f32 but feeds only the shadow-ray ORIGIN, decision-robust on a clean-separation scene), cast a shadow
// ray toward a BAKED light L=(0,8,0) as P + s·(L-P) with tmax<1 (so the light itself is not an occluder; no normalize/length),
// and write the lit/shadowed DECISION (1 = a shadow-ray MISS = unshadowed). local_size=1 (slot = WorkgroupIndex; the harness
// dispatches groups=count). Bindings: TLAS@0 (REAL), compacted@1 (read), rays@2 (read), hit_t@3 (read), decision@4 (write U32).
kir::KEntry build_shade_inline(kir::KGraph& g)
{
    const auto sh    = kir::make_shape({1});
    const auto cf    = [&](double v) { return g.constant(v, sh, kir::DType::F32); };
    const auto cu    = [&](crd::u32 v) { return g.constant(static_cast<double>(v), sh, kir::DType::U32); };
    const int  as    = g.accel_struct_decl(0, 0);                   // REAL TLAS@0 — shadow rays
    const int  comp  = g.buffer_decl(kir::DType::U32, 0, 1, false); // compacted@1 (read: hit ray indices)
    const int  rays  = g.buffer_decl(kir::DType::F32, 0, 2, false); // rays@2 (read: 6 f32/ray)
    const int  hitt  = g.buffer_decl(kir::DType::F32, 0, 3, false); // hit_t@3 (read)
    const int  dec   = g.buffer_decl(kir::DType::U32, 0, 4, true);  // decision@4 (write: 1 lit / 0 shadowed)
    const int  mark  = g.kernel_stmt_mark();
    const int  slot  = g.builtin(kir::KBuiltin::WorkgroupIndex);    // local_size=1 ⇒ slot = the workgroup index
    const int  ridx  = g.buffer_load(comp, slot);                   // ray_idx
    const int  base  = g.binary(kir::KOp::Mul, ridx, cu(6U));
    const auto rload = [&](crd::u32 k) { return g.buffer_load(rays, g.binary(kir::KOp::Add, base, cu(k))); };
    const int  ox = rload(0U);
    const int  oy = rload(1U);
    const int  oz = rload(2U);
    const int  dx = rload(3U);
    const int  dy = rload(4U);
    const int  dz = rload(5U);
    const int  t  = g.buffer_load(hitt, ridx);
    const int  px = g.binary(kir::KOp::Add, ox, g.binary(kir::KOp::Mul, t, dx)); // hitpos = o + t·d
    const int  py = g.binary(kir::KOp::Add, oy, g.binary(kir::KOp::Mul, t, dy));
    const int  pz = g.binary(kir::KOp::Add, oz, g.binary(kir::KOp::Mul, t, dz));
    const int  sdx = g.binary(kir::KOp::Sub, cf(0.0), px); // shadow-ray dir = L - P, L=(0,8,0) (unnormalized; P + s·(L-P))
    const int  sdy = g.binary(kir::KOp::Sub, cf(8.0), py);
    const int  sdz = g.binary(kir::KOp::Sub, cf(0.0), pz);
    // tmin skips the surface, tmax=0.999 stops just before L (the light is not an occluder — the P + s·(L-P), s<1 idiom).
    const kir::KGraph::RtHit s = g.trace_ray_hit(as, px, py, pz, sdx, sdy, sdz, cf(0.001), cf(0.999));
    const int lit = g.cast(g.binary(kir::KOp::CmpEq, s.prim, cu(0xFFFFFFFFU)), kir::DType::U32); // MISS ⇒ unshadowed ⇒ lit=1
    g.stmt_buffer_store(dec, slot, lit);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 1U; // one thread per compacted slot (the harness dispatches groups=count)
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
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

// ── CEIR-18a-2/18b: the light-cull kernels are AUTHORED `.ckir` assets (assets/ckir/scene_light_cull.ckir + _3d.ckir) — the
// 2D tiled (16 clusters) + 3D clustered (64 = 4×4×4) forms. ⛔⛔ NO C++ BUILDER (the user's #1 mandate 2026-08-16: the
// `.ckir` is the SOLE source, edited DIRECTLY; the [.emitckir] build→emit pattern is RETIRED). This gate LOADS both committed
// assets + proves each is a Compute program (workgroup 64), round-trips byte-exact, AND emits GLSL (a kernel the emitter
// refuses would null the pipeline). Device correctness (list==oracle==analytic, both backends) is the gpu-context
// [lightcull]/[clustered3d] gates, which also LOAD the `.ckir` (read_cull_ckir).
TEST_CASE("CEIR-18a-2/18b: the authored scene_light_cull{,_3d}.ckir are Compute programs that round-trip + emit GLSL",
          "[kir][asset][ceir18a2]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    for (const char* path : {CRD_REPO_DIR "/assets/ckir/scene_light_cull.ckir",
                             CRD_REPO_DIR "/assets/ckir/scene_light_cull_3d.ckir"})
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        kir::KGraph g(&a);
        kir::KEntry e;
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
        CHECK(e.stage == kir::KStage::Compute);
        CHECK(e.local_size[0] == 64U);
        CHECK(g.size() > 0);
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
        kir::GlslKernel kern(&a);
        CHECK(kir::emit_compute_kernel_glsl(g, e, &a, kern));
    }
}

// ── CEIR-20b/20c: the authored work_smoke_*.ckir (device-generated-work smoke kernels) parse + round-trip + emit GLSL,
// DEVICE-FREE. ───────────────────────────────────────────────────────────────────────────────────────────────────────
// The three ceir.work smoke assets are otherwise parsed ONLY inside the DGC / Work-Graphs / executor DEVICE gates, each of
// which cap-skips without the GPU/extension — so on a device-free machine their .ckir format was UNGATED. This gate closes
// that hole (the mandate-#1 decoupled-load discipline, the 19z asset-inventory rule): each asset parses, round-trips
// byte-exact, and emits GLSL, with NO device. produce writes the (count,1,1) queue header; consume atomically counts
// invocations; produce_dgc authors the DGC command stream (five (1,1,1) payloads + count=5).
TEST_CASE("CEIR-20b/20c: the authored work_smoke_*.ckir parse + round-trip + emit GLSL (device-free)",
          "[kir][asset][ceir20b][ceir20c]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    for (const char* path : {CRD_REPO_DIR "/assets/ckir/work_smoke_produce.ckir",
                             CRD_REPO_DIR "/assets/ckir/work_smoke_consume.ckir",
                             CRD_REPO_DIR "/assets/ckir/work_smoke_produce_dgc.ckir"})
    {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        kir::KGraph g(&a);
        kir::KEntry e;
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e).ok);
        CHECK(e.stage == kir::KStage::Compute);
        CHECK(e.local_size[0] == 1U); // 1-thread smoke kernels (the DEVICE count drives the grid, not local_size)
        CHECK(g.size() > 0);
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1); // byte-exact serialize round-trip
        kir::GlslKernel kern(&a);
        CHECK(kir::emit_compute_kernel_glsl(g, e, &a, kern));
    }
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

// ── CEIR-18z: the visibility-buffer FS (id → graded-grey, (primId+1)·0.25) is an AUTHORED `.ckir` asset
// (assets/ckir/visbuffer_fs.ckir). ⛔⛔ NO C++ BUILDER (the user's #1 mandate: the `.ckir` is the sole source, edited
// directly). The load gate below is its only test here; the BEHAVIORAL verdict is the REN-38-F6 device gate.
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
    // ⛔⛔ CEIR-19z-3 (F2): the shadow-multiply is now AUTHORED IN (was a 4-node passthrough) — scene_hdr is sampled AND
    // shadow_mask_buf is read via StorageLoad (the fullscreen `constants` slot, the taa_resolve precedent) indexed
    // py*TexSize(scene_hdr).x + px from FragCoord, then Splat×Mul darkens the shadowed pixels. Device correctness (the
    // occluded pixel is DARKER than the lit one) is the CEIR-19b scene-render gate; here: it round-trips byte-exact.
    CHECK(g.size() >= 18); // the F2 multiply chain (FragCoord + TexSize + StorageLoad + Splat/Mul), not the passthrough
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1); // the TexSize a/b/d operand gap + the Builtin vec4 survive the text form
}

// ── CEIR-19c: the SERIALIZER round-trips the inline-ray-query encodings (the RayHitResult node + the TraceRayHit stmt's 9
// global-ext operands). A SERIALIZER proof over an INLINE graph (the test_lower.cpp precedent — NOT an asset builder;
// assets/ckir/rt_witness.ckir is authored + verified separately, the mandate-#1 coupling line). It pins the text form every
// stage-2 wavefront kernel needs BEFORE one is authored — these encodings were never exercised through the text form.
TEST_CASE("CEIR-19c: ckir_write/read round-trips an inline-ray-query kernel (AccelStructDecl + TraceRayHit) + emits GLSL/HLSL",
          "[kir][asset][ceir19c]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    const kir::KEntry          e = build_rt_witness_inline(g);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Compute);
    // ⭐ the FORMAT PROOF: write -> read -> re-serialize is BYTE-IDENTICAL (the RayHitResult node + the TraceRayHit stmt's
    // global-ext operand array survive the text round-trip — never exercised before this kernel).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    // ⛔ it must EMIT on BOTH device paths — rayQuery needs GLSL #version 460 (the 450 scar) + HLSL cs_6_5 (SM 6.5 inline RayQuery).
    kir::GlslKernel gk(&a);
    CHECK(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-18p: the committed taa_resolve.ckir parses, round-trips + is a fragment program. ────────────────────────────
// ensure_taa_program (scene_renderer.cpp) is now a THIN asset-load — the hand-built FS (a large reproject + variance-clamp
// + Catmull-Rom history resolve) is DELETED. This gate proves the COMMITTED asset parses + round-trips byte-exact + has the
// TAA FS shape, and exercises the ckir form's harder corners the small fixtures never hit: tex_sample_at's texture-INDEX
// operand, int_bits_to_float, storage_load. Device correctness (the velocity/TAA render on both backends) is the REN-41
// gate in tests/scene-render; the NDC±Y sign is a RUNTIME constant (word 20) so this ONE asset is backend-neutral.
// ── CEIR-19c STAGE 2: the SERIALIZER round-trips the serial-compact kernel (Materialize + unconditional-BufferStore cursor
// chains). A SERIALIZER proof over an INLINE graph — NOT the source of assets/ckir/wavefront_compact.ckir (the coupling line).
TEST_CASE("CEIR-19c: ckir_write/read round-trips the serial-compact kernel + emits GLSL/HLSL", "[kir][asset][ceir19c]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    const kir::KEntry          e = build_compact_inline(g, 8U);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 1U);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr); // the DEAD AccelStructDecl forces #version 460
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c STAGE 2: the AUTHORED wavefront_compact.ckir is the serial stream-compaction kernel. NO C++ BUILDER (mandate #1:
// the .ckir is the SOLE source). Pins ASSET-INTRINSIC facts ONLY (never compares to a builder's output — the coupling line):
// loads, is a Compute program (local_size=1, the serial single thread), round-trips byte-exact, emits GLSL (#version 460, the
// dead AccelStructDecl forces it) + HLSL. Device correctness (GPU compacted+count == oracle == hand-computed) is the isolated
// device gate in tests/gpu-context-{vulkan,dx12}.
TEST_CASE("CEIR-19c: the authored wavefront_compact.ckir is a serial-compact Compute program that round-trips + emits GLSL+HLSL",
          "[kir][asset][ceir19c]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_compact.ckir", std::ios::binary | std::ios::ate);
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
    CHECK(e.local_size[0] == 1U); // ONE thread — the serial deterministic compact
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c STAGE 2: the SERIALIZER round-trips the wavefront TRACE kernel (TraceRayHit + a CmpNe/Cast hit_flag). A
// SERIALIZER proof over an INLINE graph — NOT the source of assets/ckir/wavefront_trace.ckir (the coupling line).
TEST_CASE("CEIR-19c: ckir_write/read round-trips the wavefront trace kernel + emits GLSL/HLSL", "[kir][asset][ceir19c]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    const kir::KEntry          e = build_trace_inline(g);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    CHECK(std::strstr(gk.source.c_str(), "rayQuery") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c STAGE 2: the AUTHORED wavefront_trace.ckir is the primary-ray trace kernel. NO C++ BUILDER (mandate #1). Pins
// ASSET-INTRINSIC facts ONLY (never compares to a builder — the coupling line): loads, is a Compute program (local_size=64),
// round-trips byte-exact, emits rayQuery on BOTH device paths. Device correctness (GPU hit_flag/hit_t == oracle + analytic) is
// the isolated device gate in tests/gpu-context-{vulkan,dx12}.
TEST_CASE("CEIR-19c: the authored wavefront_trace.ckir is a Compute inline-ray-query kernel that round-trips + emits GLSL+HLSL",
          "[kir][asset][ceir19c]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_trace.ckir", std::ios::binary | std::ios::ate);
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
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    CHECK(std::strstr(gk.source.c_str(), "rayQuery") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c STAGE 2: the SERIALIZER round-trips the wavefront SHADE kernel (hitpos re-derive + shadow-ray + a CmpEq decision).
// A SERIALIZER proof over an INLINE graph — NOT the source of assets/ckir/wavefront_shade.ckir (the coupling line).
TEST_CASE("CEIR-19c: ckir_write/read round-trips the wavefront shade kernel + emits GLSL/HLSL", "[kir][asset][ceir19c]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    const kir::KEntry          e = build_shade_inline(g);
    CHECK(g.size() > 0);
    CHECK(e.stage == kir::KStage::Compute);
    CHECK(e.local_size[0] == 1U);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    CHECK(std::strstr(gk.source.c_str(), "rayQuery") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c STAGE 2: the AUTHORED wavefront_shade.ckir is the shade kernel. NO C++ BUILDER (mandate #1). Pins ASSET-INTRINSIC
// facts ONLY (the coupling line): loads, is a Compute program (local_size=1), round-trips byte-exact, emits rayQuery on BOTH
// device paths. Device correctness (GPU lit/shadowed decision == oracle + analytic) is the isolated device gate + the wavefront.
TEST_CASE("CEIR-19c: the authored wavefront_shade.ckir is a Compute shadow-ray kernel that round-trips + emits GLSL+HLSL",
          "[kir][asset][ceir19c]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/wavefront_shade.ckir", std::ios::binary | std::ios::ate);
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
    CHECK(e.local_size[0] == 1U);
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    CHECK(std::strstr(gk.source.c_str(), "rayQuery") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

// ── CEIR-19c: the AUTHORED rt_witness.ckir (assets/ckir/rt_witness.ckir) is the STAGE-1 bridge witness — the trivial
// one-ray_query inline-rayQuery kernel. NO C++ BUILDER (mandate #1: the .ckir is the SOLE source, authored directly). This
// gate LOADS the committed asset + proves it is a Compute program, round-trips byte-exact, AND emits rayQuery on BOTH device
// paths (GLSL #version 460 + rayQueryEXT; HLSL cs_6_5). Pins ASSET-INTRINSIC facts ONLY — it never compares to a C++ builder's
// output (the mandate coupling line). Device correctness (execute_rt_lowered + the oracle prim-id compare, both backends +
// lavapipe) is the CEIR-19c stage-1 device gate.
TEST_CASE("CEIR-19c: the authored rt_witness.ckir is a Compute inline-ray-query kernel that round-trips + emits GLSL+HLSL",
          "[kir][asset][ceir19c]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/rt_witness.ckir", std::ios::binary | std::ios::ate);
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
    // ⛔ BOTH device paths — an AccelStructDecl forces GLSL #version 460 (the 450 rayQuery scar); the TraceRayHit lowers to
    // rayQueryEXT. A refused RT op would null the pipeline (the compute-emitter-lag scar).
    kir::GlslKernel gk(&a);
    REQUIRE(kir::emit_compute_kernel_glsl(g, e, &a, gk));
    CHECK(std::strstr(gk.source.c_str(), "#version 460") != nullptr);
    CHECK(std::strstr(gk.source.c_str(), "rayQuery") != nullptr);
    kir::GlslKernel hk(&a);
    CHECK(kir::emit_compute_kernel_hlsl(g, e, &a, hk));
}

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
