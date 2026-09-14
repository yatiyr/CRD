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
#include <crd/kir/ckir_kernel_eval.hpp> // CEIR-25c-0: eval_cpu_kernel — the relu_vjp.ckir reading-gate oracle
#include <crd/kir/ckir_serialize.hpp>
#include <crd/kir/ckir_technique.hpp> // CEIR-18p: body_moment_convert/blur — the library builders the moment bootstrap emits from

#include "../gpu-shared/ui_tint_noise_oracle.hpp" // CEIR-31b-4-b-ii-2: the scalar ref_hash* ORACLE, lifted so the device (e) gate shares it

#include <crd/core/platform.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp> // CEIR-35b: growable arena for the ckir_read mutation fuzz
#include <crd/containers/array.hpp> // CEIR-35b: Array<u8> mutant buffer
#include <crd/containers/span.hpp>  // CEIR-35b: ConstSpan<u8> seed/mutant view

#include <ckir_asset_list.hpp> // CEIR-35a Q1: GENERATED manifest of every committed assets/ckir/*.ckir (the load-sweep gate)

#include <cstdio>  // FILE/fopen/fwrite — the [.emitckir] regen writer (the test_ckir_kernel_emit idiom)
#include <cstring>
#include <fstream> // bootstrap file IO for the .ckir emitter/identity tests (streams, not std containers)

#include <catch2/catch_test_macros.hpp>

// ⛔ NO `#ifndef CRD_REPO_DIR / #define "."` fallback -- crd-kir-tests CMakeLists ALWAYS defines it (PRIVATE
// CRD_REPO_DIR); a "." fallback is the cwd-luck scar pre-armed (Win-greens on ./assets, WSL-reds). A missing define must
// fail LOUD. [[feedback_cuda_test_target_missing_crd_repo_dir_is_cwd_luck]].

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

// ── CEIR-31b-1a-iii part 2: the hash-INTENT oracle for the committed ui_tint_noise.ckir. `emit_hash_u32` builds the EXACT
// 13-node U32 integer-hash sequence the committed file carries at n17..n29; `emit_noise_from_hash` the 3-node F32 tail
// (n30..n32 -- Cast<F32>/·(1/2^32)). ⛔ these are VERIFICATION FIXTURES kept permanently -- NOT a kernel builder (the .ckir
// is hand-authored, so there is nothing to delete): `build_hash_scalar` wraps `emit_hash_u32` as a COMPUTE kernel that
// eval_cpu_kernel-verifies BIT-EXACT vs a C++ u32 reference (a Fragment FragCoord kernel cannot eval -- the scalar evaluator
// is compute-only), and the reading gate NODE+WIRING-matches the committed file against a run of these same helpers.
int emit_hash_u32(kir::KGraph& g, int xu32, int yu32)
{
    const auto sh1 = kir::make_shape({1});
    const auto cu  = [&](double v) { return g.constant(v, sh1, kir::DType::U32); };
    const int  a_k = cu(2654435761.0); // 0x9E3779B1
    const int  x_a = g.binary(kir::KOp::Mul, xu32, a_k);
    const int  b_k = cu(2246822519.0); // 0x85EBCA77
    const int  y_b = g.binary(kir::KOp::Mul, yu32, b_k);
    const int  h1  = g.binary(kir::KOp::BitXor, x_a, y_b);
    const int  s15 = cu(15.0);
    const int  h1s = g.binary(kir::KOp::Shr, h1, s15);
    const int  h2  = g.binary(kir::KOp::BitXor, h1, h1s);
    const int  c_k = cu(668265263.0); // 0x27D4EB2F
    const int  h3  = g.binary(kir::KOp::Mul, h2, c_k);
    const int  s13 = cu(13.0);
    const int  h3s = g.binary(kir::KOp::Shr, h3, s13);
    return g.binary(kir::KOp::BitXor, h3, h3s); // h4
}
int emit_noise_from_hash(kir::KGraph& g, int h4)
{
    const auto sh1 = kir::make_shape({1});
    const int  hf  = g.cast(h4, kir::DType::F32);
    const int  inv = g.constant(2.3283064365386963e-10, sh1, kir::DType::F32); // 1/2^32
    return g.binary(kir::KOp::Mul, hf, inv);
}
// COMPUTE oracle: in[2*lid], in[2*lid+1] = (x,y) as U32; out[lid] = the U32 hash. One lane per (x,y) pair.
kir::KEntry build_hash_scalar(kir::KGraph& g)
{
    const auto sh1    = kir::make_shape({1});
    const auto cu     = [&](double v) { return g.constant(v, sh1, kir::DType::U32); };
    const int  inbuf  = g.buffer_decl(kir::DType::U32, 0, 0, false);
    const int  outbuf = g.buffer_decl(kir::DType::U32, 0, 1, true);
    const int  lid    = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int  mark   = g.kernel_stmt_mark();
    const int  base   = g.binary(kir::KOp::Mul, lid, cu(2.0));
    const int  x_i    = g.buffer_load(inbuf, base);
    const int  y_i    = g.buffer_load(inbuf, g.binary(kir::KOp::Add, base, cu(1.0)));
    g.stmt_buffer_store(outbuf, lid, emit_hash_u32(g, x_i, y_i));
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 8U;
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
// The C++ reference for the eval-verify (ref_hash / _stage1 / _stage3): LIFTED to tests/gpu/gpu-shared/ui_tint_noise_oracle.hpp
// so the on-device arm (e) gate computes GPU==eval from the SAME definition eval==C++ uses here. Pulled into this anon
// namespace so the existing unqualified call sites (the bit-31 probes + the eval CHECK below) resolve unchanged.
using crd::tests::ref_hash;
using crd::tests::ref_hash_stage1;
using crd::tests::ref_hash_stage3;

// ── CEIR-31b-1b: the σ=R/3 Gaussian weight function for ui_blur (R=4, 9 taps). THE PERMANENT ORACLE + SPEC (kept when the
// graph builder is deleted): σ = R/3 = 4/3 puts the ±4 window at ±3σ (~0.27% truncated, so renormalization is a ~1e-3
// correction not a ~1e-1 one). Computes the weights in f64, normalizes, ROUNDS EACH TO f32, then folds the residual
// (1 − Σf32) into the CENTER tap so the stored f32 set stays SYMMETRIC and its f32 left-to-right sum is within 1 ulp of 1.0
// (measured 0.99999994 -- exact 1.0f is unreachable for a symmetric f32 set: the f32-storage-floor scar). The bootstrap
// builder CALLS this (one function, two consumers -- the emit_hash_u32 discipline); the reading gate matches committed cvals
// to it. ⛔ do NOT re-derive the weights anywhere else.
void blur_weights_r4_sigma_r3(float out[9])
{
    const double sigma = 4.0 / 3.0; // R/3, R=4
    double       w64[9];
    double       sum = 0.0;
    for (int k = 0; k < 9; ++k)
    {
        const double xk = static_cast<double>(k - 4);
        w64[k]          = std::exp(-(xk * xk) / (2.0 * sigma * sigma));
        sum += w64[k];
    }
    float fsum = 0.0F;
    for (int k = 0; k < 9; ++k)
    {
        out[k] = static_cast<float>(w64[k] / sum); // normalize in f64, round each to f32
        fsum   = fsum + out[k];                    // left-to-right f32 sum (the order the gate checks)
    }
    out[4] = out[4] + (1.0F - fsum); // fold the residual into the CENTER tap (preserves symmetry)
}
// ⛔ CEIR-31b-1b: the bootstrap graph builder `build_ui_blur` + its `[.emit-ui-blur]` generator were DELETED after the commit
// + reading gate landed (the committed assets/ckir/ui_blur.ckir is the source). `blur_weights_r4_sigma_r3` above is KEPT as
// the permanent weight ORACLE the reading gate matches the committed cvals against. To regenerate: restore the builder from
// git history (it called this oracle) — the reading gate's weight-match guards a drifted regen.
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

    // ── CEIR-35b regression: the mutation fuzz found ckir_read ACCEPTING-then-crashing on two structural defects (a node
    // whose `op` key is absent -> indeterminate op -> kKOpNames[op] OOB; an operand ref past the node array -> downstream OOB).
    // Pin the EXACT reason (gate=identity-not-category) so a future refactor can't silently downgrade the rejection.
    {
        const kir::CkirReadResult r = read("schema = 1\n[[entry]]\nstage = \"Compute\"\n[[node]]\n"); // a [[node]] with no `op =` key
        CHECK_FALSE(r.ok);
        CHECK(std::strcmp(r.error, "node: missing op") == 0);
    }
    {
        const kir::CkirReadResult r = read("schema = 1\n[[entry]]\nstage = \"Compute\"\n[[node]]\nop = \"Const\"\nin = [\"n7\"]\n"); // operand ref to a nonexistent node
        CHECK_FALSE(r.ok);
        CHECK(std::strcmp(r.error, "node operand ref out of range") == 0);
    }
}

TEST_CASE("REPO.DEV.11: the .ckir text form keeps a -0.0 constant and rejects an output that names no node",
          "[kir][asset][fuzz][repodev11]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    {
        // The writer elided `cval` with `!= 0.0`, which is also false for -0.0: the sign bit died in the text form while
        // the blob kept it (pending fuzz artifact 0add989c, now in the corpus). Byte-identical through write/read/serialize.
        kir::KGraph       g(&a);
        const kir::KEntry e = build_scale(g, -0.0);
        CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    }
    {
        // `[[out]]` without `node =` left out[0].node at -1; the reader accepted it, the writer emitted "n-1" and the reader
        // rejected its own output (pending artifact dd98295f, now in the corpus). Rejected at the first read instead.
        kir::KGraph               g(&a);
        kir::KEntry               e;
        const kir::CkirReadResult r = kir::ckir_read(
            crd::containers::StringView("schema = 1\n[[entry]]\nstage = \"Fragment\"\nn_out = 1\n[[out]]\nlocation = 0\n"
                                        "[[node]]\nid = \"n0\"\nop = \"Const\"\n"),
            g, e);
        CHECK_FALSE(r.ok);
        CHECK(std::strcmp(r.error, "stage output names no node") == 0);
    }
}

// ── CEIR-35b: systematic MUTATION-robustness fuzz for ckir_read (the .ckir text loader). ──────────────────────────────
// Extends the hand-picked malformed corpus above into exhaustive coverage: mutate a valid .ckir seed thousands of ways
// and prove ckir_read NEVER crashes/throws (ASan-clean under the asan configs) and ALWAYS returns a WELL-FORMED result.
// Four teeth: (1) well-formed result; (2) any ACCEPTED mutant round-trips BYTE-IDENTICAL (ckir_roundtrip_diff == -1),
// pushing the fuzz into ckir_write + serialize_graph; (3) determinism (same seed -> identical (ok,off) trace); (4)
// non-vacuity (>=1 accept AND >=1 reject). Deterministic splitmix64 (no <random>); the mutator is duplicated per exe (not
// shared-headered) at this size, per the advisor. GrowableTlsfAllocator: ckir_read appends incrementally (no header-count
// pre-alloc), so there is no count-inflation blowup; the arena just recycles each per-mutant KGraph's allocations.
namespace
{
struct FuzzRng
{
    crd::u64 state;
    explicit FuzzRng(crd::u64 seed) noexcept : state(seed) {}
    crd::u64 next() noexcept
    {
        crd::u64 z = (state += 0x9E3779B97F4A7C15ULL);
        z          = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        z          = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31U);
    }
    crd::u32 below(crd::u32 n) noexcept { return n == 0U ? 0U : static_cast<crd::u32>(next() % n); }
};

// The first `count` bytes of `src` in a fresh Array — the TRUNCATION primitive (count == size copies whole).
[[nodiscard]] crd::containers::Array<crd::u8> fuzz_prefix(crd::containers::ConstSpan<crd::u8> src, crd::usize count,
                                                          crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> b(alloc);
    b.reserve(count);
    for (crd::usize i = 0U; i < count; ++i) { b.push_back(src[i]); }
    return b;
}

// ONE byte-level mutation chosen by `rng` (bit-flip / delete / insert-any-byte / duplicate / swap), copy-with-transform
// (needs only push_back). Insert spans the FULL byte range incl. NUL and 0x80-0xFF (the signed-char tokenizer scar).
[[nodiscard]] crd::containers::Array<crd::u8> fuzz_mutate(crd::containers::ConstSpan<crd::u8> src, FuzzRng& rng,
                                                          crd::memory::IAllocator* alloc)
{
    crd::containers::Array<crd::u8> b(alloc);
    const crd::usize                n = src.size();
    if (n == 0U)
    {
        b.push_back(static_cast<crd::u8>(rng.next()));
        return b;
    }
    const crd::u32   kind = rng.below(5U);
    const crd::usize pos  = rng.below(static_cast<crd::u32>(n));
    switch (kind)
    {
    case 0U: // bit-flip one byte
        b.reserve(n);
        for (crd::usize i = 0U; i < n; ++i)
        {
            b.push_back(i == pos ? static_cast<crd::u8>(src[i] ^ static_cast<crd::u8>(1U << rng.below(8U))) : src[i]);
        }
        break;
    case 1U: // delete the byte at pos
        b.reserve(n - 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            if (i != pos) { b.push_back(src[i]); }
        }
        break;
    case 2U: // insert an arbitrary byte before pos
    {
        const crd::u8 v = static_cast<crd::u8>(rng.next());
        b.reserve(n + 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            if (i == pos) { b.push_back(v); }
            b.push_back(src[i]);
        }
        break;
    }
    case 3U: // duplicate the byte at pos
        b.reserve(n + 1U);
        for (crd::usize i = 0U; i < n; ++i)
        {
            b.push_back(src[i]);
            if (i == pos) { b.push_back(src[i]); }
        }
        break;
    default: // swap two bytes
    {
        const crd::usize q = rng.below(static_cast<crd::u32>(n));
        b.reserve(n);
        for (crd::usize i = 0U; i < n; ++i)
        {
            crd::u8 v = src[i];
            if (i == pos) { v = src[q]; }
            else if (i == q) { v = src[pos]; }
            b.push_back(v);
        }
        break;
    }
    }
    return b;
}

// A CkirReadResult is WELL-FORMED iff a rejection carries an in-range offset + a non-empty static reason. (ckir_read
// fills KGraph&/KEntry& by reference and has no module pointer, so an acceptance is simply ok.)
[[nodiscard]] bool ckir_result_wf(const kir::CkirReadResult& r, crd::usize input_size) noexcept
{
    if (r.ok) { return true; }
    return r.error_offset <= input_size && r.error != nullptr && r.error[0] != '\0';
}
} // namespace

TEST_CASE("ceir fuzz: ckir_read survives byte mutation of a .ckir and never crashes", "[kir][asset][fuzz][ceir35b]")
{
    crd::memory::GrowableTlsfAllocator root;

    // Seed = a valid .ckir text (ckir_write of the D1 scale kernel), built IN-TEST (no file I/O, no CRD_REPO_DIR).
    kir::KGraph                   seed_g(&root);
    const kir::KEntry             seed_e    = build_scale(seed_g, 2.0);
    const crd::containers::String seed_text = kir::ckir_write(seed_g, seed_e, &root);
    const crd::containers::ConstSpan<crd::u8> seed(reinterpret_cast<const crd::u8*>(seed_text.c_str()), seed_text.size());
    REQUIRE(seed.size() > 16U);
    {
        kir::KGraph g(&root); // (0) non-vacuity floor: the unmutated seed reads ok
        kir::KEntry e;
        REQUIRE(kir::ckir_read(crd::containers::StringView(seed_text.c_str(), seed_text.size()), g, e).ok);
    }

    crd::usize accepts = 0U;
    crd::usize rejects = 0U;

    auto run_one = [&](crd::containers::ConstSpan<crd::u8> mutant) -> crd::u64 {
        kir::KGraph               g(&root);
        kir::KEntry               e;
        const kir::CkirReadResult r = kir::ckir_read(
            crd::containers::StringView(reinterpret_cast<const char*>(mutant.data()), mutant.size()), g, e);
        CHECK(ckir_result_wf(r, mutant.size())); // (invariant 1) well-formedness
        if (r.ok)
        {
            ++accepts;
            // (invariant 2) an ACCEPTED mutant round-trips BYTE-IDENTICAL (write->read->serialize == serialize);
            // ckir_roundtrip_diff returns -1 on byte-identity (-2 if the re-read fails). Pushes into ckir_write/serialize.
            CHECK(ckir_roundtrip_diff(g, e, &root) == -1);
        }
        else { ++rejects; }
        return (static_cast<crd::u64>(r.ok) << 63U) ^ static_cast<crd::u64>(r.error_offset);
    };

    for (crd::usize off = 0U; off <= seed.size(); ++off) // (a) exhaustive truncation
    {
        const crd::containers::Array<crd::u8> t = fuzz_prefix(seed, off, &root);
        (void)run_one(crd::containers::ConstSpan<crd::u8>(t.data(), t.size()));
    }

    auto random_pass = [&](crd::u64 seed_val) -> crd::u64 { // (b) random byte-level mutations
        FuzzRng            rng(seed_val);
        crd::u64           trace        = 1469598103934665603ULL; // FNV-1a offset basis
        constexpr crd::u32 mutant_count = 256U * 5U;              // ~256 per mutation kind
        for (crd::u32 i = 0U; i < mutant_count; ++i)
        {
            const crd::containers::Array<crd::u8> m = fuzz_mutate(seed, rng, &root);
            trace = (trace ^ run_one(crd::containers::ConstSpan<crd::u8>(m.data(), m.size()))) * 1099511628211ULL;
        }
        return trace;
    };
    CHECK(random_pass(0xCC1235B0ULL) == random_pass(0xCC1235B0ULL)); // (invariant 3) determinism

    CHECK(accepts > 0U); // (invariant 4) non-vacuity
    CHECK(rejects > 0U);
}

// ── CEIR-35a Q1: the committed-asset LOAD SWEEP. Every `.ckir` under assets/ckir/ (enumerated by the CMake-generated
// manifest, CONFIGURE_DEPENDS) MUST ckir_read OK. This is the HONEST close of the CEIR-35b over-rejection claim: the
// hand-picked [asset] gates below cover only ~1/3 of the 40+ committed assets; the rest are loaded only by engine-runtime
// / other-exe paths (which ran STALE when only crd-kir-tests was rebuilt for the header-only fix). Loading them ALL here
// proves the 35b kir fix (op/kind/stage requires + index-range validation) does NOT over-reject a committed (incl.
// hand-authored) asset. A failure prints the file + the EXACT reason, so "fix the file" vs "the require is too strict for
// hand-authoring" becomes a real decision, never a silent regression. Standing Q1 gate; a new asset auto-joins on reconfigure.
TEST_CASE("CEIR-35a: every committed assets/ckir/*.ckir loads via ckir_read (the over-rejection sweep)",
          "[kir][asset][ceir35a]")
{
    crd::memory::GrowableTlsfAllocator root; // grows; each per-asset KGraph/String/Array recycles into the arena
    REQUIRE(crd::kir::test::kCommittedCkirCount > 0); // a glob that found nothing is a config bug, not a pass
    for (int i = 0; i < crd::kir::test::kCommittedCkirCount; ++i)
    {
        const char* const name = crd::kir::test::kCommittedCkir[i];
        crd::containers::String path(&root);
        path.append(CRD_REPO_DIR "/assets/ckir/");
        path.append(name);

        std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
        INFO("asset: " << name);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        REQUIRE(sz > 0);
        f.seekg(0);
        crd::containers::Array<char> src(&root);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);

        kir::KGraph               g(&root);
        kir::KEntry               e;
        const kir::CkirReadResult r =
            kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e);
        INFO("reason: " << r.error);
        CHECK(r.ok);
    }
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

// ── CEIR-31b-1a: the committed ui_backdrop_fetch.ckir (the §141 frosted-glass chain's first pass) LOADS + round-trips +
// LOWERS on both backends. ⛔ a FULLSCREEN Vec4 COLOR kernel has NO device-free NUMERIC gate -- eval_cpu_kernel is SCALAR
// and refuses Vec nodes ([[scars_ckir_emitter_eval]]); its numerics ride the 31b-4 ON-DEVICE gate. So this device-free
// gate is STRUCTURAL, with teeth that pin the pass as a FETCH (not a masked composite): parses, byte-exact round-trip (a
// hand-broken file fails LOUD), the right shape, EXACTLY ONE TexSample and ZERO StorageLoad (the discriminator vs
// rt_composite's storage read), and emits GLSL AND HLSL whose source actually USES the sampler (`texture(` / `.Sample(` --
// a dead binding would emit neither). The 2x2 box average is NOT in this kernel: it is the frame-graph contract (a LINEAR
// sampler + a 2:1 dest transient), declared + gated at 31b-3 and proven on-device at 31b-4.
TEST_CASE("CEIR-31b-1a: the committed ui_backdrop_fetch.ckir parses, round-trips, and lowers on both backends",
          "[kir][asset][ceir31b]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/ui_backdrop_fetch.ckir", std::ios::binary | std::ios::ate);
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

    // STRUCTURE: a 4-node fullscreen fragment fetch (StageIn uv -> Texture + Sampler -> TexSample -> @output).
    CHECK(g.size() == 4);
    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);

    // TEETH: exactly ONE TexSample and ZERO StorageLoad -- a fetch, not rt_composite-minus-mask (which reads storage).
    int n_texsample  = 0;
    int n_storageload = 0;
    for (int i = 0; i < static_cast<int>(g.size()); ++i)
    {
        const kir::KOp op = g.node(i).op;
        if (op == kir::KOp::TexSample) { ++n_texsample; }
        if (op == kir::KOp::StorageLoad) { ++n_storageload; }
    }
    CHECK(n_texsample == 1);
    CHECK(n_storageload == 0);

    // ROUND-TRIP byte-exact (the anti-drift contract).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);

    // BOTH-BACKEND lowering: the fragment stage emits GLSL AND HLSL that ACTUALLY SAMPLE the bound texture (a dead binding
    // would compile to neither call). GLSL uses implicit-LOD `texture(`, HLSL uses `.Sample(`.
    kir::GlslKernel gk_glsl(&a);
    CHECK(kir::emit_stage_glsl(g, e, &a, gk_glsl));
    CHECK(gk_glsl.source.size() > 0U);
    CHECK(std::strstr(gk_glsl.source.c_str(), "texture(") != nullptr);
    kir::GlslKernel gk_hlsl(&a);
    CHECK(kir::emit_stage_hlsl(g, e, &a, gk_hlsl));
    CHECK(gk_hlsl.source.size() > 0U);
    CHECK(std::strstr(gk_hlsl.source.c_str(), ".Sample(") != nullptr);
}

// ── CEIR-31b-1a-iii: the committed ui_tint_noise.ckir (the §141 frosted-glass chain's grain pass) LOADS + round-trips +
// LOWERS on both backends. Device-free gate is STRUCTURAL WITH TEETH (eval_cpu_kernel refuses this Vec4 output --
// [[scars_ckir_emitter_eval]]): the tint+grain OP-VOCABULARY pins it -- exactly 1 TexSample (the backdrop fetch), exactly
// 5 spec-consts (4 tint channels + amp), a REAL integer hash (>=2 BitXor + >=2 Shr on U32 => unsigned uint `>>`), 0
// StorageLoad (a fetch+grain, NOT rt_composite's storage read), byte-exact roundtrip, and emits GLSL (`texture(` + `>>`)
// AND HLSL. ⛔ THREE-TIER honesty about what this buys: (1) the roundtrip pins the file is CANONICAL (a non-shortest hex /
// stray field / wrong dtype token fails write->read, since ckir_roundtrip_diff is serialize(g)==serialize(parse(write(g))),
// a fixed-point test); (2) the teeth pin the OP-VOCABULARY is right; (3) NEITHER proves the cvals + wiring are the hash you
// INTENDED -- a wrong magic constant (a valid f64 bit pattern) passes every tooth here. That last proof is 31b-1a-iii part 2
// (NEXT tick): a scalar-output `build_hash_scalar` oracle eval_cpu_kernel-verified vs a C++ reference + a NODE-SEQUENCE match
// asserting THIS file embeds that same hash. Device numerics ride 31b-4 arm (e).
TEST_CASE("CEIR-31b-1a-iii: the committed ui_tint_noise.ckir parses, round-trips, and lowers on both backends",
          "[kir][asset][ceir31b]")
{
    std::ifstream f(CRD_REPO_DIR "/assets/ckir/ui_tint_noise.ckir", std::ios::binary | std::ios::ate);
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

    // TEETH (identity, not category): the tint+grain vocabulary -- 1 texture sample, 5 spec-consts (4 tint + 1 amp), a
    // real integer hash (>=2 BitXor + >=2 Shr), and NO storage read.
    int n_texsample   = 0;
    int n_specconst   = 0;
    int n_bitxor      = 0;
    int n_shr         = 0;
    int n_storageload = 0;
    for (int i = 0; i < static_cast<int>(g.size()); ++i)
    {
        const kir::KNode& nd = g.node(i);
        if (nd.op == kir::KOp::TexSample) { ++n_texsample; }
        if (nd.op == kir::KOp::BitXor) { ++n_bitxor; }
        if (nd.op == kir::KOp::Shr) { ++n_shr; }
        if (nd.op == kir::KOp::StorageLoad) { ++n_storageload; }
        if (kir::is_spec_const(nd)) { ++n_specconst; }
    }
    CHECK(n_texsample == 1);
    CHECK(n_specconst == 5);
    CHECK(n_bitxor >= 2);
    CHECK(n_shr >= 2);
    CHECK(n_storageload == 0);

    // ROUND-TRIP byte-exact (the anti-drift contract; a hand-broken cval/edge fails LOUD).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);

    // BOTH-BACKEND lowering: GLSL samples the backdrop (`texture(`) AND shifts (`>>`, the integer hash); HLSL emits.
    kir::GlslKernel gk_glsl(&a);
    CHECK(kir::emit_stage_glsl(g, e, &a, gk_glsl));
    CHECK(gk_glsl.source.size() > 0U);
    CHECK(std::strstr(gk_glsl.source.c_str(), "texture(") != nullptr);
    CHECK(std::strstr(gk_glsl.source.c_str(), ">>") != nullptr);
    kir::GlslKernel gk_hlsl(&a);
    CHECK(kir::emit_stage_hlsl(g, e, &a, gk_hlsl));
    CHECK(gk_hlsl.source.size() > 0U);
}

// ── CEIR-31b-1a-iii PART 2: the hash-INTENT proof that flips ui_tint_noise 🔄→✅. Two claims, ONE transitive statement
// (the committed file computes the eval-verified integer hash, WIRING included): (1) EVAL-VERIFY -- `build_hash_scalar`, a
// COMPUTE oracle wrapping the SAME `emit_hash_u32` the committed file's n17..n29 is matched against, evaluated by the scalar
// `eval_cpu_kernel` == a C++ u32 reference BIT-EXACT over 8 (x,y) pairs incl. ≥2 with bit 31 set at BOTH shift stages (the
// logical-vs-arithmetic `>>` probe, one per shift). ⛔ SCOPE: eval == C++ u32 is proven HERE; GPU == eval is a SEPARATE claim
// resting on the emitter's uint lowering (U32 dtype → `uint` ctype, grep-verified) + casting before `>>`, proven ON-DEVICE at
// 31b-4 arm (e) — do NOT let "bit-exact" bleed onto the untested GPU layer. (2) NODE+WIRING
// MATCH -- the committed n17..n32 == a run of `emit_hash_u32`+`emit_noise_from_hash` on op/dtype/cval AND operand wiring
// (constant-offset map for intra-run operands; the x/y inputs correspond to committed n15/n16), so a swapped-input or A·A
// mis-wire that survives emit is caught here. Together: the structural-teeth gate above proves the op-vocabulary; THIS proves
// the cvals + wiring are the intended hash. Device numerics still ride 31b-4 arm (e).
TEST_CASE("CEIR-31b-1a-iii: the committed ui_tint_noise.ckir embeds the eval-verified integer hash (node+wiring match)",
          "[kir][asset][ceir31b]")
{
    crd::memory::TlsfAllocator a(8U << 20U);

    // (1) EVAL-VERIFY the hash MATH: the compute oracle == the C++ u32 reference, bit-exact.
    {
        kir::KGraph    g(&a);
        kir::KEntry    e        = build_hash_scalar(g);
        const crd::u32 xs[8]    = {0U, 1U, 7U, 100U, 1920U, 3U, 65535U, 12345U};
        const crd::u32 ys[8]    = {0U, 1U, 13U, 200U, 1080U, 99999U, 4U, 54321U};
        int            n_b31_s1 = 0;
        int            n_b31_s3 = 0;
        for (int i = 0; i < 8; ++i)
        {
            if ((ref_hash_stage1(xs[i], ys[i]) >> 31U) == 1U) { ++n_b31_s1; }
            if ((ref_hash_stage3(xs[i], ys[i]) >> 31U) == 1U) { ++n_b31_s3; }
        }
        // ⛔ BOTH `>>` shifts (>>15 on stage1, >>13 on stage3) must be logical; probe bit 31 at BOTH so the coverage can't
        // silently drop if a constant is edited. (stage1 gives 3, stage3 gives 5 with these 8 pairs.)
        REQUIRE(n_b31_s1 >= 2);
        REQUIRE(n_b31_s3 >= 2);
        crd::f64 in[16];
        crd::f64 out[8];
        for (int i = 0; i < 8; ++i)
        {
            in[2 * i]     = static_cast<crd::f64>(xs[i]);
            in[2 * i + 1] = static_cast<crd::f64>(ys[i]);
        }
        for (double& o : out) { o = -1.0; }
        kir::KernelBuffer bufs[2] = {{in, 16, 0, 0}, {out, 8, 0, 1}};
        kir::eval_cpu_kernel(g, e, bufs, 2, 8U, &a, 1U);
        for (int i = 0; i < 8; ++i) { CHECK(static_cast<crd::u32>(out[i]) == ref_hash(xs[i], ys[i])); }
    }

    // (2) NODE + WIRING MATCH: parse the committed file; assert n17..n32 == a run of the SAME helpers.
    kir::KGraph committed(&a);
    kir::KEntry ce;
    {
        std::ifstream f(CRD_REPO_DIR "/assets/ckir/ui_tint_noise.ckir", std::ios::binary | std::ios::ate);
        REQUIRE(f.good());
        const std::streamsize sz = f.tellg();
        f.seekg(0);
        crd::containers::Array<char> src(&a);
        src.resize(static_cast<crd::usize>(sz), '\0');
        f.read(src.data(), sz);
        REQUIRE(kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), committed, ce).ok);
    }
    kir::KGraph ref(&a);
    const auto  sh1   = kir::make_shape({1});
    const int   rx    = ref.constant(0.0, sh1, kir::DType::U32); // placeholder x input
    const int   ry    = ref.constant(0.0, sh1, kir::DType::U32); // placeholder y input
    const int   first = static_cast<int>(ref.size());            // index of the first hash node (Const A)
    const int   rh4   = emit_hash_u32(ref, rx, ry);
    (void)emit_noise_from_hash(ref, rh4);
    const int c0    = 17;        // committed n17 = Const A (the hash run start; n15/n16 = the Cast x/y inputs)
    const int delta = c0 - first;
    const auto match_operand = [&](int rop, int cop) {
        if (rop < 0) { CHECK(cop < 0); }                      // unused-slot sentinel
        else if (rop >= first) { CHECK(cop == rop + delta); } // intra-run operand -> constant offset
        else if (rop == rx) { CHECK(cop == 15); }             // the x input <-> committed n15
        else if (rop == ry) { CHECK(cop == 16); }             // the y input <-> committed n16
        else { CHECK(false); }                                // an operand outside the run that is not x/y
    };
    for (int k = 0; k < 16; ++k)
    {
        const kir::KNode& cn = committed.node(c0 + k);
        const kir::KNode& rn = ref.node(first + k);
        CHECK(cn.op == rn.op);
        CHECK(cn.dtype() == rn.dtype());
        CHECK(cn.cval == rn.cval);
        match_operand(rn.a, cn.a);
        match_operand(rn.b, cn.b);
        match_operand(rn.c, cn.c);
    }
}

// ── CEIR-31b-1b: the committed ui_blur.ckir — a σ=R/3 separable Gaussian blur (H/V selected by the dir spec-const). The
// device-free gate is STRUCTURAL teeth + a NUMERIC WEIGHT check (the only device-free numeric surface; the blurred pixels
// ride 31b-4). ⛔ 0 Vec3 is the 2D discriminator — a moment_blur copy-paste (9 Vec3, array coords) fails here. The 9 weights
// (the non-spec F32 Consts with cval∈(0,1); the tap-index consts -4..4 are excluded by the OPEN interval) == the σ=R/3 oracle
// bit-exact, are symmetric + monotone-from-center, and their f32 LEFT-TO-RIGHT sum is within 1 ulp of 1.0 (measured
// 0.99999994 — exact 1.0f is unreachable for a symmetric f32 set). That ±2^-23 ⇒ blur-of-a-constant returns c·(1±2^-23);
// 31b-4 arm (a)'s ±1 LSB at 8-bit output has ~50x headroom (NOT "unbiased by construction" — the device adds its own
// accumulation ulps; arm (a) measures the real total).
TEST_CASE("CEIR-31b-1b: the committed ui_blur.ckir is a sigma=R/3 separable Gaussian (weights + structure)",
          "[kir][asset][ceir31b]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    std::ifstream              f(CRD_REPO_DIR "/assets/ckir/ui_blur.ckir", std::ios::binary | std::ios::ate);
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

    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);

    // STRUCTURAL TEETH + the weight-cval harvest (in file order = tap order -4..4).
    int   n_tex       = 0;
    int   n_samp      = 0;
    int   n_texsample = 0;
    int   n_spec      = 0;
    int   n_storage   = 0;
    int   n_vec3      = 0;
    int   n_weight    = 0;
    float wfile[9]    = {0.0F};
    for (int i = 0; i < static_cast<int>(g.size()); ++i)
    {
        const kir::KNode& nd = g.node(i);
        if (nd.op == kir::KOp::Texture) { ++n_tex; }
        if (nd.op == kir::KOp::Sampler) { ++n_samp; }
        if (nd.op == kir::KOp::TexSample) { ++n_texsample; }
        if (nd.op == kir::KOp::StorageLoad) { ++n_storage; }
        if (nd.op == kir::KOp::Vec3) { ++n_vec3; }
        if (kir::is_spec_const(nd)) { ++n_spec; }
        else if (nd.op == kir::KOp::Const && nd.dtype() == kir::DType::F32 && nd.cval > 0.0 && nd.cval < 1.0)
        {
            if (n_weight < 9) { wfile[n_weight] = static_cast<float>(nd.cval); }
            ++n_weight;
        }
    }
    CHECK(n_texsample == 9);
    CHECK(n_spec == 3); // dir.x@0, dir.y@1, step@2
    CHECK(n_tex == 1);
    CHECK(n_samp == 1);
    CHECK(n_storage == 0);
    CHECK(n_vec3 == 0); // ⛔ 2D discriminator — NOT moment_blur's array coords
    CHECK(n_weight == 9);

    // WEIGHTS == the σ=R/3 oracle bit-exact; symmetric; monotone-from-center; f32 left-to-right sum within 1 ulp of 1.0.
    float wref[9] = {0.0F};
    blur_weights_r4_sigma_r3(wref);
    for (int k = 0; k < 9; ++k) { CHECK(wfile[k] == wref[k]); }
    for (int k = 0; k < 4; ++k) { CHECK(wfile[k] == wfile[8 - k]); }
    CHECK(wfile[4] > wfile[3]);
    CHECK(wfile[3] > wfile[2]);
    CHECK(wfile[2] > wfile[1]);
    CHECK(wfile[1] > wfile[0]);
    float ssum = 0.0F;
    for (float wv : wfile) { ssum = ssum + wv; } // left-to-right (the named order)
    const float dev = ssum - 1.0F;
    CHECK(dev <= 0x1p-23F); // one f32 ulp
    CHECK(dev >= -0x1p-23F);

    // ROUND-TRIP byte-exact + both-backend lowering (a fragment stage sampling the backdrop).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk_glsl(&a);
    CHECK(kir::emit_stage_glsl(g, e, &a, gk_glsl));
    CHECK(gk_glsl.source.size() > 0U);
    CHECK(std::strstr(gk_glsl.source.c_str(), "texture(") != nullptr);
    kir::GlslKernel gk_hlsl(&a);
    CHECK(kir::emit_stage_hlsl(g, e, &a, gk_hlsl));
    CHECK(gk_hlsl.source.size() > 0U);
}

// ── CEIR-31b-3-b: the committed ui_composite.ckir — the §141 frosted-glass chain's LAST pass, lerp(scene, effect, mask.r)
// over THREE textures via the BINDLESS idiom (the deferred_lighting arrayed-Texture + SampleIndexed precedent, NOT three
// single-texture bindings — the multi-read fullscreen contract; a 1-read pass binds a single texture at binding 1, a
// multi-read pass binds the reads into the descriptor-array heap at binding 16). The device-free gate is STRUCTURAL only —
// a Vec4 color kernel has NO CPU-eval numeric surface ([[scars_ckir_emitter_eval]] Vec-refusal), and the mixed pixels +
// the reads→heap-layer BINDING ride 31b-4. ⛔ TEETH: the descriptor-array length (Texture count==3) rejects a single-texture
// copy; the layer tooth (the 3 SampleIndexed index operands are U32 Consts 0/1/2 EACH ONCE) rejects a `0,0,1` typo that
// reads scene twice; TexSample==0 is the discriminator vs the other 3 ui kernels (they sample via TexSample, composite via
// SampleIndexed); StorageLoad==0 vs rt_composite's storage-mask multiply.
TEST_CASE("CEIR-31b-3-b: the committed ui_composite.ckir is a bindless 3-texture mix(scene,effect,mask)",
          "[kir][asset][ceir31b]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    std::ifstream              f(CRD_REPO_DIR "/assets/ckir/ui_composite.ckir", std::ios::binary | std::ios::ate);
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

    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(g.node(e.out[0].node).type.rows == 4); // ⛔ the output is vec4 — a VecConcat that dropped the alpha (vec3) fails HERE, not as black-alpha on device

    int n_tex       = 0;
    int n_samp      = 0;
    int n_texsample = 0;
    int n_sampidx   = 0;
    int n_mix       = 0;
    int n_storage   = 0;
    int tex_count   = 0;             // the descriptor-array length of the (single) Texture node
    int layer_seen[3] = {0, 0, 0};   // times each bindless layer 0/1/2 is a SampleIndexed index operand
    int layer_other   = 0;           // a SampleIndexed indexing anything but 0/1/2 (a wrong-layer typo)
    for (int i = 0; i < static_cast<int>(g.size()); ++i)
    {
        const kir::KNode& nd = g.node(i);
        if (nd.op == kir::KOp::Texture) { ++n_tex; tex_count = static_cast<int>(nd.type.count); }
        if (nd.op == kir::KOp::Sampler) { ++n_samp; }
        if (nd.op == kir::KOp::TexSample) { ++n_texsample; }
        if (nd.op == kir::KOp::StorageLoad) { ++n_storage; }
        if (nd.op == kir::KOp::Mix) { ++n_mix; }
        if (nd.op == kir::KOp::SampleIndexed)
        {
            ++n_sampidx;
            const kir::KNode& idx   = g.node(nd.d); // the 4th operand = the bindless layer index
            const int         layer = static_cast<int>(idx.cval);
            if (idx.op == kir::KOp::Const && layer >= 0 && layer < 3) { ++layer_seen[layer]; }
            else { ++layer_other; }
        }
    }
    CHECK(n_tex == 1);
    CHECK(tex_count == 3);   // ⛔ a descriptor-array of 3 (the bindless heap) — a count==1 single-texture copy is wrong
    CHECK(n_samp == 1);      // ONE Sampler (the one-sampler-per-pass contract)
    CHECK(n_sampidx == 3);   // scene / effect / mask
    CHECK(n_mix == 1);       // the lerp
    CHECK(n_texsample == 0); // discriminator: composite samples via SampleIndexed, not TexSample
    CHECK(n_storage == 0);   // vs rt_composite's StorageLoad mask
    CHECK(layer_other == 0);
    CHECK(layer_seen[0] == 1); // layer 0 = scene, exactly once
    CHECK(layer_seen[1] == 1); // layer 1 = effect, exactly once
    CHECK(layer_seen[2] == 1); // layer 2 = mask, exactly once (a 0,0,1 typo trips one of these)

    // ROUND-TRIP fixed-point + both-backend lowering (a fragment stage sampling the bindless heap).
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk_glsl(&a);
    CHECK(kir::emit_stage_glsl(g, e, &a, gk_glsl));
    CHECK(gk_glsl.source.size() > 0U);
    CHECK(std::strstr(gk_glsl.source.c_str(), "texture(") != nullptr); // the bindless sample is emitted, not dead
    kir::GlslKernel gk_hlsl(&a);
    CHECK(kir::emit_stage_hlsl(g, e, &a, gk_hlsl));
    CHECK(gk_hlsl.source.size() > 0U);
}

// ── CEIR-31b-4-a: the committed ui_mask_rect.ckir — the panel-coverage source that REPLACES the placeholder
// engine://ui/panel_fill geometry pass (a fullscreen fixed-rect touches nothing parked; the ADR-0107 UiPanel is a PARKED
// Track-B concept). cov = step(x0,uv.x)*step(uv.x,x1)*step(y0,uv.y)*step(uv.y,y1) → the R8 mask.r the composite reads as
// bindless layer 2. The rect bounds are 4 SPEC-CONSTS (x0@0/y0@1/x1@2/y1@3, defaults 0.25/0.25/0.75/0.75) so 31b-4 arm (a)
// can MOVE the rect (a [pass.params] edit) and watch the composite output move (baked bounds would need a 2nd asset). The
// device-free gate is STRUCTURAL (a Vec-math kernel, no CPU-eval — [[scars_ckir_emitter_eval]] Vec-refusal). ⛔ TEETH: 4
// spec-consts with the EXACT rect defaults (a swapped x0/x1 or a wrong bound fails HERE); 4 Step (the 4 half-planes);
// 0 Texture/Sampler/TexSample/SampleIndexed/StorageLoad — the discriminator: the ONE ui kernel that reads NO texture (its
// pass is reads=[]); the Step ternary is EMITTED (not dead) on both backends; the output is Vec4 (the R8 target takes .r).
TEST_CASE("CEIR-31b-4-a: the committed ui_mask_rect.ckir is a 4-spec-const hard rect (no textures)",
          "[kir][asset][ceir31b]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    std::ifstream              f(CRD_REPO_DIR "/assets/ckir/ui_mask_rect.ckir", std::ios::binary | std::ios::ate);
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

    CHECK(e.stage == kir::KStage::Fragment);
    CHECK(e.n_out == 1);
    CHECK(g.node(e.out[0].node).type.rows == 4); // Vec4 output; the R8 mask target stores .r = cov

    int    n_tex       = 0;
    int    n_samp      = 0;
    int    n_texsample = 0;
    int    n_sampidx   = 0;
    int    n_storage   = 0;
    int    n_step      = 0;
    int    n_splat     = 0;
    int    n_stagein   = 0;
    int    n_swizzle   = 0;
    int    n_spec      = 0;
    double spec_def[4] = {-99.0, -99.0, -99.0, -99.0}; // default cval by spec id 0..3
    for (int i = 0; i < static_cast<int>(g.size()); ++i)
    {
        const kir::KNode& nd = g.node(i);
        if (nd.op == kir::KOp::Texture) { ++n_tex; }
        if (nd.op == kir::KOp::Sampler) { ++n_samp; }
        if (nd.op == kir::KOp::TexSample) { ++n_texsample; }
        if (nd.op == kir::KOp::SampleIndexed) { ++n_sampidx; }
        if (nd.op == kir::KOp::StorageLoad) { ++n_storage; }
        if (nd.op == kir::KOp::Step) { ++n_step; }
        if (nd.op == kir::KOp::Splat) { ++n_splat; }
        if (nd.op == kir::KOp::StageIn) { ++n_stagein; }
        if (nd.op == kir::KOp::Swizzle) { ++n_swizzle; }
        if (kir::is_spec_const(nd))
        {
            ++n_spec;
            const crd::u32 id = kir::spec_const_id(nd);
            if (id < 4U) { spec_def[id] = nd.cval; }
        }
    }
    CHECK(n_tex == 0);       // ⛔ the discriminator: the ONLY ui kernel reading NO texture (its pass is reads=[])
    CHECK(n_samp == 0);
    CHECK(n_texsample == 0);
    CHECK(n_sampidx == 0);
    CHECK(n_storage == 0);
    CHECK(n_step == 4);      // the 4 half-plane edges
    CHECK(n_splat == 1);     // cov → Vec4
    CHECK(n_stagein == 1);   // the fullscreen uv
    CHECK(n_swizzle == 2);   // uv.x, uv.y
    CHECK(n_spec == 4);      // x0@0 / y0@1 / x1@2 / y1@3

    // the EXACT rect defaults — a swapped x0/x1 or a wrong bound fails HERE, device-free (0.25/0.75 are exact in f64)
    CHECK(spec_def[0] == 0.25); // x0
    CHECK(spec_def[1] == 0.25); // y0
    CHECK(spec_def[2] == 0.75); // x1
    CHECK(spec_def[3] == 0.75); // y1

    // ROUND-TRIP fixed-point + both-backend emit; the Step coverage is EMITTED (not dead), and NOTHING samples a texture.
    CHECK(ckir_roundtrip_diff(g, e, &a) == -1);
    kir::GlslKernel gk_glsl(&a);
    CHECK(kir::emit_stage_glsl(g, e, &a, gk_glsl));
    CHECK(gk_glsl.source.size() > 0U);
    CHECK(std::strstr(gk_glsl.source.c_str(), "? 0.0 : 1.0") != nullptr); // Step emitted inline — the coverage math is live
    CHECK(std::strstr(gk_glsl.source.c_str(), "texture(") == nullptr);    // ⛔ NO texture sample — the reads=[] discriminator
    kir::GlslKernel gk_hlsl(&a);
    CHECK(kir::emit_stage_hlsl(g, e, &a, gk_hlsl));
    CHECK(gk_hlsl.source.size() > 0U);
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
// verdict on both backends) is the CEIR-18c gate in tests/rendering/scene-render.
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
// 0.25/0.5 graded-grey per-primitive pixels) is the REN-38-F6 device gate in tests/rendering/scene-render, which now renders THROUGH
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
// (world-pos vs a CPU-analytic probe, then the RT shadow term) is the CEIR-19b gate in tests/rendering/scene-render.
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
// gate in tests/rendering/scene-render; the NDC±Y sign is a RUNTIME constant (word 20) so this ONE asset is backend-neutral.
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
// Device correctness (half-res min-depth) is the REN-40-G3 gate in tests/rendering/scene-render.
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
// inv values on the blur asset serialize to DIFFERENT graphs. Device correctness is the REN-40-D gate in tests/rendering/scene-render.
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
// correctness (static≈0.5, mover=screen delta) is the REN-41 velocity gate in tests/rendering/scene-render.
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
// correctness (unpack == CPU oracle on a real mesh-shader device) is the REN-41 cluster-mesh gate in tests/rendering/scene-render.
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
// CPU, bit-identical) is the REN-40-F GPU-skin gate in tests/rendering/scene-render.
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
// CEIR-17e / REN-41 velocity gates in tests/rendering/scene-render.
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
// build_impostor_fs AND the emit hook are DELETED. This is the STRUCTURAL proof the device pixel gate (tests/rendering/scene-render,
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

// ── CEIR-25c-0: assets/ckir/relu_vjp.ckir — the ReLU VJP kernel (the relu.ckir <-> relu_vjp.ckir asset-driven differentiable
//    pair, §57). gx[i] = (x[i] > 0) ? gy[i] : 0 — the reverse of out=max(in,0): the gradient passes through where the forward
//    was active, zeroed elsewhere. `x` is the forward PRE-activation (relu's input), `gy` the upstream adjoint. 3 buffers:
//    x@0 (read), gy@1 (read), gx@2 (write). ⛔ CEIR-26d-4: local_size is the SENTINEL (0 = "bind from the write numel at cook", like
//    relu.ckir) — the resolver cook-binds it; the M*hidden==32 baked contract is RETIRED (a vjp runs at any interior width).
//    ⛔ CmpGt gives EXACT `x>0?1:0` (matches hesap nn_reverse::relu_vjp's STRICT `>0` at x==0, where GLSL step() would differ),
//    consumed by Select (bool cond). Authored via the BOOTSTRAP path (build -> ckir_write -> eval-verify -> commit) because the
//    bool-typed CmpGt + the Select ternary make the type fields error-prone to hand-author; the builder types them by construction.
namespace
{
kir::KEntry build_relu_vjp(kir::KGraph& g)
{
    const int        xbuf  = g.buffer_decl(kir::DType::F32, 0, 0, false); // x@0  — the forward pre-activation (read)
    const int        gybuf = g.buffer_decl(kir::DType::F32, 0, 1, false); // gy@1 — the upstream adjoint (read)
    const int        gxbuf = g.buffer_decl(kir::DType::F32, 0, 2, true);  // gx@2 — the operand adjoint (write)
    const int        lid   = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const kir::Shape sh1   = kir::make_shape({1});
    const int        zero  = g.constant(0.0, sh1, kir::DType::F32);
    const int        x     = g.buffer_load(xbuf, lid);
    const int        gy    = g.buffer_load(gybuf, lid);
    const int        mask  = g.binary(kir::KOp::CmpGt, x, zero); // (x > 0) — Bool
    const int        gx    = g.select(mask, gy, zero);           // (x>0) ? gy : 0
    const int        mark  = g.kernel_stmt_mark();
    g.stmt_buffer_store(gxbuf, lid, gx);
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 0; // ⛔ CEIR-26d-4b: the SENTINEL (0 = bind at cook) — the resolver cook-binds local_size ← the write numel; MUST match the committed relu_vjp.ckir (0), else a [.emit-relu-vjp] regen reverts the sentinel
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}

// ── CEIR-26d-3b: assets/ckir/softmax.ckir — the GENERALIZED scaled row-wise softmax (Sk any width via a spec-const loop bound; the
//    24b baked Sk=3 unroll retired). probs[r,c] = exp(s·scores[r,c] − m_r)/Σ_c exp(s·scores[r,c] − m_r), m_r = max_c(s·scores[r,c]),
//    s = scale[0] = 1/√D. One lane per OUTPUT ROW r = LocalInvocationIndex in [0, Sq); local_size = Sq (the resolver cook-binds it
//    to dim0(scores); the eval/reading gates set it). Sk is a SPEC-CONST (constant_id 0, default 3) — the For loop bound. The
//    per-row max m_r + normalizer d_r are carried across the loop in GROUPSHARED slots (kernel-tier For has NO register carry —
//    26d-3b-1; each lane touches only its own [r] slot ⇒ no barrier). Three UNIFORM `For c in [0,Sk)` passes: (1) m = max; (2)
//    d += exp(·−m); (3) probs = exp(·−m)/d (RECOMPUTE exp — no store-and-reload, the inline-load RAW scar). Bootstrapped via
//    ckir_write (build → eval-verify (2,3)+(3,5) → commit → reading gate → DELETE builder; the 23e-a mold).
kir::KEntry build_softmax(kir::KGraph& g)
{
    using kir::KOp;
    const kir::Shape sh1 = kir::make_shape({1});
    const int        scores = g.buffer_decl(kir::DType::F32, 0, 0, false); // scores@0 (read)
    const int        scale  = g.buffer_decl(kir::DType::F32, 0, 1, false); // scale@1 (read, [1])
    const int        probs  = g.buffer_decl(kir::DType::F32, 0, 2, true);  // probs@2 (write)
    constexpr int    cap   = 1024;                                        // shared cap ≥ max Sq (avoids a 3rd cook-bind; r<Sq≤cap)
    const int        m_sh   = g.shared_decl(kir::DType::F32, cap);        // per-row running max
    const int        d_sh   = g.shared_decl(kir::DType::F32, cap);        // per-row normalizer
    const int        r      = g.builtin(kir::KBuiltin::LocalInvocationIndex);
    const int        sk     = g.spec_constant(0U, 3.0, kir::DType::U32);   // Sk = the loop bound (spec-const, default 3)
    const int        base   = g.binary(KOp::Mul, r, sk);                   // r·Sk (U32)
    const int        zero_u  = g.constant(0.0, sh1, kir::DType::U32);
    const int        s      = g.buffer_load(scale, zero_u);                 // scale[0] = 1/√D
    const int        negmax = g.constant(-static_cast<crd::f64>(3.4028234663852886e38), sh1, kir::DType::F32); // −FLT_MAX (f64→f32 exact)
    const int        zero_f  = g.constant(0.0, sh1, kir::DType::F32);

    const int mark = g.kernel_stmt_mark();
    // ⛔ base = r·Sk is consumed inside ALL THREE loop bodies — MATERIALIZE it at the enclosing (body) scope, else the emitter
    //    declares its temp inside the FIRST loop and passes 2/3 reference an OUT-OF-SCOPE temp (glslang rejects it; the temp-scope
    //    materialize scar [[feedback_ckir_if_block_shared_temp_scope_materialize]]).
    g.stmt_materialize(base);
    g.stmt_shared_store(m_sh, r, negmax); // m_r = −FLT_MAX
    g.stmt_shared_store(d_sh, r, zero_f);  // d_r = 0
    // PASS 1 — m_r = max_c(s·scores[base+c])
    {
        const int f   = g.stmt_for_begin(sk);
        const int c   = g.kernel_loop_var(f);
        const int idx = g.binary(KOp::Add, base, c);
        const int e   = g.binary(KOp::Mul, s, g.buffer_load(scores, idx));
        g.stmt_shared_store(m_sh, r, g.binary(KOp::Max, g.shared_load(m_sh, r), e));
        g.stmt_for_end(f);
    }
    // PASS 2 — d_r = Σ_c exp(s·scores[base+c] − m_r)
    {
        const int f   = g.stmt_for_begin(sk);
        const int c   = g.kernel_loop_var(f);
        const int idx = g.binary(KOp::Add, base, c);
        const int e   = g.binary(KOp::Mul, s, g.buffer_load(scores, idx));
        const int ex  = g.unary(KOp::Exp, g.binary(KOp::Sub, e, g.shared_load(m_sh, r)));
        g.stmt_shared_store(d_sh, r, g.binary(KOp::Add, g.shared_load(d_sh, r), ex));
        g.stmt_for_end(f);
    }
    // PASS 3 — probs[base+c] = exp(s·scores[base+c] − m_r) / d_r
    {
        const int f   = g.stmt_for_begin(sk);
        const int c   = g.kernel_loop_var(f);
        const int idx = g.binary(KOp::Add, base, c);
        const int e   = g.binary(KOp::Mul, s, g.buffer_load(scores, idx));
        const int ex  = g.unary(KOp::Exp, g.binary(KOp::Sub, e, g.shared_load(m_sh, r)));
        g.stmt_buffer_store(probs, idx, g.binary(KOp::Div, ex, g.shared_load(d_sh, r)));
        g.stmt_for_end(f);
    }
    kir::KEntry e;
    e.stage             = kir::KStage::Compute;
    e.local_size[0]     = 0; // ⛔ CEIR-26d-4e: the SENTINEL (0 = bind at cook) — the resolver cook-binds local_size ← Sq=dim0(probs) via bind_authored_local_size (typed cap); eval/reading gates set it explicitly
    e.kernel_body_begin = mark;
    e.kernel_body_count = g.stmt_count() - mark;
    return e;
}
} // namespace

// HIDDEN GENERATOR ([.emit-relu-vjp]): (re)write assets/ckir/relu_vjp.ckir from build_relu_vjp. Run explicitly to regenerate:
//   crd-kir-tests.exe "[.emit-relu-vjp]"  — the committed .ckir is the asset; this builder is the regen source (kept in-tree,
//   the [.emit-fft-cuda] idiom). Regenerate whenever the ReLU VJP formulation changes.
TEST_CASE("CEIR-25c-0: (re)generate assets/ckir/relu_vjp.ckir from the builder", "[.emit-relu-vjp]")
{
    crd::memory::TlsfAllocator         a(8U << 20U);
    kir::KGraph                        g(&a);
    const kir::KEntry                  e    = build_relu_vjp(g);
    const crd::containers::String      text = kir::ckir_write(g, e, &a);
    FILE*                              f    = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir", "wb") != 0) { f = nullptr; }
#else
    f = std::fopen(CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir", "wb");
#endif
    REQUIRE(f != nullptr);
    fwrite(text.c_str(), 1, text.size(), f);
    fclose(f);
}

// CEIR-26d-3b-2: the BOOTSTRAP eval-verify — build_softmax's graph, spec-const Sk set + local_size Sq, eval_cpu_kernel == a CPU
// scaled row-wise softmax at BOTH the 24b proof dims (Sq=2, Sk=3) AND a generic (Sq=3, Sk=5) — proves the loop kernel is
// dimension-general BEFORE the .ckir is generated. f64 eval vs f64 ref (algorithm check, not device f32); DELETE with the builder.
namespace
{
void softmax_eval_case(crd::u32 sq, crd::u32 sk)
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    kir::KEntry                e = build_softmax(g);
    (void)g.set_spec_const(0U, static_cast<crd::f64>(sk)); // Sk = the loop bound
    e.local_size[0]        = sq;                           // one lane per row
    const crd::u32 n       = sq * sk;
    crd::f64       scores[16];
    crd::f64       probs[16];
    crd::f64       scale[1] = {0.5}; // 1/√D stand-in
    for (crd::u32 i = 0; i < n; ++i) { scores[i] = 0.3 * (static_cast<crd::f64>(i) - static_cast<crd::f64>(n) * 0.5); }
    for (crd::u32 i = 0; i < n; ++i) { probs[i] = -999.0; }
    kir::KernelBuffer bufs[3] = {{scores, static_cast<int>(n), 0, 0}, {scale, 1, 0, 1}, {probs, static_cast<int>(n), 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, sq, &a, 1U);
    const auto ad = [](crd::f64 x) { return x < 0.0 ? -x : x; };
    for (crd::u32 r = 0; r < sq; ++r)
    {
        crd::f64 m = -1e300;
        for (crd::u32 c = 0; c < sk; ++c) { const crd::f64 v = scale[0] * scores[r * sk + c]; if (v > m) { m = v; } }
        crd::f64 denom = 0.0;
        for (crd::u32 c = 0; c < sk; ++c) { denom += std::exp(scale[0] * scores[r * sk + c] - m); }
        for (crd::u32 c = 0; c < sk; ++c)
        {
            const crd::f64 ref = std::exp(scale[0] * scores[r * sk + c] - m) / denom;
            CHECK(ad(probs[r * sk + c] - ref) <= 1e-6 * (1.0 + ad(ref)));
        }
    }
}
} // namespace
TEST_CASE("CEIR-26d-3b: build_softmax evals == CPU scaled row-wise softmax at (Sq=2,Sk=3) and (Sq=3,Sk=5)", "[kir][asset]")
{
    softmax_eval_case(2U, 3U); // the 24b attention proof dims
    softmax_eval_case(3U, 5U); // a generic width — the dimension-general proof (previously BakedKernelShapeUnsupported)
}

// HIDDEN GENERATOR ([.emit-softmax]): (re)write assets/ckir/softmax.ckir from build_softmax. Run explicitly to regenerate:
//   crd-kir-tests.exe "[.emit-softmax]". ⛔ per the 23e-a mold this builder is DELETED after the commit + reading gate land
//   (softmax.ckir's header records the bootstrap-then-delete; NOT relu_vjp's keep-as-regen exception).
TEST_CASE("CEIR-26d-3b: (re)generate assets/ckir/softmax.ckir from the builder", "[.emit-softmax]")
{
    crd::memory::TlsfAllocator    a(8U << 20U);
    kir::KGraph                   g(&a);
    const kir::KEntry             e    = build_softmax(g);
    const crd::containers::String text = kir::ckir_write(g, e, &a);
    FILE*                         f    = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, CRD_REPO_DIR "/assets/ckir/softmax.ckir", "wb") != 0) { f = nullptr; }
#else
    f = std::fopen(CRD_REPO_DIR "/assets/ckir/softmax.ckir", "wb");
#endif
    REQUIRE(f != nullptr);
    fwrite(text.c_str(), 1, text.size(), f);
    fclose(f);
}

// The READING GATE: ckir_read the COMMITTED assets/ckir/relu_vjp.ckir and eval_cpu_kernel it — gx == (x>0)?gy:0 EXACTLY (a
// mask/select, no arithmetic rounding; x/gy chosen f32-exact so the F32 store round-trips identically). Proves the committed
// asset (not a rebuilt graph) matches the analytic ReLU VJP over positive / zero / negative pre-activations.
TEST_CASE("CEIR-25c-0: the committed relu_vjp.ckir computes the analytic ReLU VJP (gx = (x>0)?gy:0)", "[kir][asset]")
{
    crd::memory::TlsfAllocator a(8U << 20U);
    kir::KGraph                g(&a);
    kir::KEntry                e;
    std::ifstream              f(CRD_REPO_DIR "/assets/ckir/relu_vjp.ckir", std::ios::binary | std::ios::ate);
    REQUIRE(f.good());
    const std::streamsize sz = f.tellg();
    f.seekg(0);
    crd::containers::Array<char> src(&a);
    src.resize(static_cast<crd::usize>(sz), '\0');
    f.read(src.data(), sz);
    const auto rr = kir::ckir_read(crd::containers::StringView(src.data(), static_cast<crd::usize>(sz)), g, e);
    REQUIRE(rr.ok);
    REQUIRE(e.stage == kir::KStage::Compute);
    REQUIRE(e.local_size[0] == 0U); // ⛔ CEIR-26d-4: the SENTINEL (0 = "bind from the write numel at cook") — the resolver cook-binds
                                    //    it (bind_authored_local_size) on the device path; this device-free eval passes ls explicitly.

    constexpr int ls = 32;
    crd::f64      x[ls];
    crd::f64      gy[ls];
    crd::f64      gx[ls];
    const crd::f64 xvals[3] = {-1.0, 0.0, 2.0}; // negative / zero / positive — all f32-exact
    for (int i = 0; i < ls; ++i)
    {
        x[i]  = xvals[i % 3];
        gy[i] = static_cast<crd::f64>(i) / 32.0; // dyadic ⇒ f32-exact (the store round-trips identically)
        gx[i] = -9.0;
    }
    kir::KernelBuffer bufs[3] = {{x, ls, 0, 0}, {gy, ls, 0, 1}, {gx, ls, 0, 2}};
    kir::eval_cpu_kernel(g, e, bufs, 3, static_cast<crd::u32>(ls), &a);
    for (int i = 0; i < ls; ++i)
    {
        const crd::f64 ref = x[i] > 0.0 ? gy[i] : 0.0; // the definition hesap nn_reverse::relu_vjp implements (strict >0 at x==0; computed inline — tests/gpu/kir doesn't link hesap)
        CHECK(gx[i] == ref);
    }
}
