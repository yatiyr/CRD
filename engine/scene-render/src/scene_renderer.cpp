// scene_renderer.cpp — GEO-7 (D-007 row 72): chunk-grain extract → cull → partial upload → vertex-pulling
// instanced submission. See scene_renderer.hpp for the pipeline + data contract.

#include <crd/scenerender/scene_renderer.hpp>

#include <crd/anim/pose.hpp>
#include <crd/geometry/primitives/transform.hpp>
#include <crd/gpu/context.hpp>
#include <crd/kir/ckir.hpp>
#include <crd/resources/openpbr_material.hpp>
#include <crd/resources/resource_manager.hpp>
#include <crd/resources/texture_resource.hpp> // REN-2 Half B: the cooked base-color map the forward pass samples
#include <crd/scene/query.hpp>
#include <crd/scene/render_components.hpp>
#include <crd/scene/spatial_bvh_index.hpp>
#include <crd/scene/transform.hpp>
#include <crd/scene/world.hpp>

#include <chrono> // REN-8: CPU wall-clock of render(), to compare against the frame graph's GPU timestamps
#include <cmath>
#include <cstring>

namespace crd::scenerender
{

namespace
{

// ── the CKIR forward pass ──────────────────────────────────────────────────────────────────────────────────────────
// The RET-6 builder scaffolding, self-contained (crd-scene-render does not depend on crd-draw): the u32 storage
// buffer at set 0 / binding 0 (KOp::StorageLoad), floats as bit patterns via int_bits_to_float.
struct Gx
{
    crd::kir::KGraph& g;
    crd::kir::Shape   sh;

    explicit Gx(crd::kir::KGraph& graph) : g(graph), sh(crd::kir::make_shape({1})) {}

    [[nodiscard]] int kf(double v) { return g.constant(v, sh, crd::kir::DType::F32); }
    [[nodiscard]] int ku(crd::u32 v) { return g.constant(static_cast<double>(v), sh, crd::kir::DType::U32); }
    [[nodiscard]] int add(int a, int b) { return g.binary(crd::kir::KOp::Add, a, b); }
    [[nodiscard]] int sub(int a, int b) { return g.binary(crd::kir::KOp::Sub, a, b); }
    [[nodiscard]] int mul(int a, int b) { return g.binary(crd::kir::KOp::Mul, a, b); }
    [[nodiscard]] int dvd(int a, int b) { return g.binary(crd::kir::KOp::Div, a, b); }
    [[nodiscard]] int mx(int a, int b) { return g.binary(crd::kir::KOp::Max, a, b); }
    [[nodiscard]] int loadu(int idx) { return g.storage_load(idx); }
    [[nodiscard]] int loadf(int idx) { return g.int_bits_to_float(g.cast(loadu(idx), crd::kir::DType::I32)); }
    [[nodiscard]] int hdru(crd::u32 word) { return loadu(ku(word)); }
    [[nodiscard]] int hdrf(crd::u32 word) { return loadf(ku(word)); }

    // clip = view_proj (header words 6..21, column-major) · vec4(p, 1)
    void mul_view_proj(int px, int py, int pz, int out[4])
    {
        const int v[4] = {px, py, pz, kf(1.0)};
        for (crd::u32 i = 0; i < 4U; ++i)
        {
            int acc = mul(hdrf(6U + 0U * 4U + i), v[0]);
            acc     = add(acc, mul(hdrf(6U + 1U * 4U + i), v[1]));
            acc     = add(acc, mul(hdrf(6U + 2U * 4U + i), v[2]));
            acc     = add(acc, mul(hdrf(6U + 3U * 4U + i), v[3]));
            out[i]  = acc;
        }
    }
};

// VS: VertexIndex → (instance, corner-index) → pull index → vertex → instance matrix → world → clip.
// Varyings: loc 0 world normal (vec3, smooth) · loc 1 instance colour (vec4, flat).
void build_scene_vs(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    Gx c(g);

    const int vid  = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int idxc = c.hdru(0U);
    const int ii   = c.dvd(vid, idxc);                 // instance = vid / index_count (u32 div truncates)
    const int li   = c.sub(vid, c.mul(ii, idxc));      // vid % index_count

    const int vidx  = c.loadu(c.add(c.hdru(2U), li)); // the index buffer entry
    const int vbase = c.add(c.hdru(3U), c.mul(vidx, c.ku(kVertexWords)));
    const int px    = c.loadf(c.add(vbase, c.ku(0U)));
    const int py    = c.loadf(c.add(vbase, c.ku(1U)));
    const int pz    = c.loadf(c.add(vbase, c.ku(2U)));
    const int nx    = c.loadf(c.add(vbase, c.ku(3U)));
    const int ny    = c.loadf(c.add(vbase, c.ku(4U)));
    const int nz    = c.loadf(c.add(vbase, c.ku(5U)));

    const int slot  = c.loadu(c.add(c.hdru(5U), ii)); // the culled visible list indirection
    const int ibase = c.add(c.hdru(4U), c.mul(slot, c.ku(kInstanceWords)));
    int       m[16];
    for (crd::u32 k = 0; k < 16U; ++k) { m[k] = c.loadf(c.add(ibase, c.ku(k))); }

    // world = M · (p, 1)  (column-major: column j = m[4j..4j+3])
    const int wx = c.add(c.add(c.mul(m[0], px), c.mul(m[4], py)), c.add(c.mul(m[8], pz), m[12]));
    const int wy = c.add(c.add(c.mul(m[1], px), c.mul(m[5], py)), c.add(c.mul(m[9], pz), m[13]));
    const int wz = c.add(c.add(c.mul(m[2], px), c.mul(m[6], py)), c.add(c.mul(m[10], pz), m[14]));
    // world normal = upper-3x3 · n (uniform-scale assumption; FS renormalizes)
    const int nwx = c.add(c.add(c.mul(m[0], nx), c.mul(m[4], ny)), c.mul(m[8], nz));
    const int nwy = c.add(c.add(c.mul(m[1], nx), c.mul(m[5], ny)), c.mul(m[9], nz));
    const int nwz = c.add(c.add(c.mul(m[2], nx), c.mul(m[6], ny)), c.mul(m[10], nz));

    int clip[4];
    c.mul_view_proj(wx, wy, wz, clip);

    const int cr = c.loadf(c.add(ibase, c.ku(16U)));
    const int cg = c.loadf(c.add(ibase, c.ku(17U)));
    const int cb = c.loadf(c.add(ibase, c.ku(18U)));
    const int ca = c.loadf(c.add(ibase, c.ku(19U)));

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(clip[0], clip[1], clip[2], clip[3]);
    ve.n_out    = 2;
    ve.out[0]   = {g.vec3(nwx, nwy, nwz), 0, kir::Interp::Smooth};
    ve.out[1]   = {g.vec4(cr, cg, cb, ca), 1, kir::Interp::Flat};
}

// GEO-8 skinned VS: the same pull path, but the vertex ALSO pulls its packed skin record (header [25] = skin
// stream offset, 6 words/vertex: joints as two u16-pairs + 4 weights) and blends FOUR palette matrices from the
// per-instance palette section (header [26] = palette offset, [27] = joint count) — B8-j's LBS formulation:
// blending the transformed positions is affine-equivalent to blending the matrices.
void build_scene_vs_skinned(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    Gx c(g);

    const int vid  = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int idxc = c.hdru(0U);
    const int ii   = c.dvd(vid, idxc);
    const int li   = c.sub(vid, c.mul(ii, idxc));

    const int vidx  = c.loadu(c.add(c.hdru(2U), li));
    const int vbase = c.add(c.hdru(3U), c.mul(vidx, c.ku(kVertexWords)));
    const int px    = c.loadf(c.add(vbase, c.ku(0U)));
    const int py    = c.loadf(c.add(vbase, c.ku(1U)));
    const int pz    = c.loadf(c.add(vbase, c.ku(2U)));
    const int nx    = c.loadf(c.add(vbase, c.ku(3U)));
    const int ny    = c.loadf(c.add(vbase, c.ku(4U)));
    const int nz    = c.loadf(c.add(vbase, c.ku(5U)));

    const int slot  = c.loadu(c.add(c.hdru(5U), ii));
    const int ibase = c.add(c.hdru(4U), c.mul(slot, c.ku(kInstanceWords)));

    // the skin record
    const int sbase = c.add(c.hdru(25U), c.mul(vidx, c.ku(6U)));
    const int jw0   = c.loadu(sbase);
    const int jw1   = c.loadu(c.add(sbase, c.ku(1U)));
    const int mask  = c.ku(0xFFFFU);
    int       joints[4];
    joints[0] = g.binary(kir::KOp::BitAnd, jw0, mask);
    joints[1] = g.binary(kir::KOp::Shr, jw0, c.ku(16U));
    joints[2] = g.binary(kir::KOp::BitAnd, jw1, mask);
    joints[3] = g.binary(kir::KOp::Shr, jw1, c.ku(16U));
    int weights[4];
    for (crd::u32 k = 0; k < 4U; ++k) { weights[k] = c.loadf(c.add(sbase, c.ku(2U + k))); }

    // the per-instance palette: pbase = palette_off + slot·joint_count·16
    const int pbase = c.add(c.hdru(26U), c.mul(slot, c.mul(c.hdru(27U), c.ku(16U))));

    // LBS: Σ wk · (Mk · [p,1]) for position, Σ wk · (Mk · [n,0]) for the normal (B8-j)
    int sx = c.kf(0.0);
    int sy = c.kf(0.0);
    int sz = c.kf(0.0);
    int snx = c.kf(0.0);
    int sny = c.kf(0.0);
    int snz = c.kf(0.0);
    for (crd::u32 k = 0; k < 4U; ++k)
    {
        const int mb = c.add(pbase, c.mul(joints[k], c.ku(16U)));
        int       m[16];
        for (crd::u32 e = 0; e < 16U; ++e) { m[e] = c.loadf(c.add(mb, c.ku(e))); }
        const int tx = c.add(c.add(c.mul(m[0], px), c.mul(m[4], py)), c.add(c.mul(m[8], pz), m[12]));
        const int ty = c.add(c.add(c.mul(m[1], px), c.mul(m[5], py)), c.add(c.mul(m[9], pz), m[13]));
        const int tz = c.add(c.add(c.mul(m[2], px), c.mul(m[6], py)), c.add(c.mul(m[10], pz), m[14]));
        sx           = c.add(sx, c.mul(tx, weights[k]));
        sy           = c.add(sy, c.mul(ty, weights[k]));
        sz           = c.add(sz, c.mul(tz, weights[k]));
        const int tnx = c.add(c.add(c.mul(m[0], nx), c.mul(m[4], ny)), c.mul(m[8], nz));
        const int tny = c.add(c.add(c.mul(m[1], nx), c.mul(m[5], ny)), c.mul(m[9], nz));
        const int tnz = c.add(c.add(c.mul(m[2], nx), c.mul(m[6], ny)), c.mul(m[10], nz));
        snx           = c.add(snx, c.mul(tnx, weights[k]));
        sny           = c.add(sny, c.mul(tny, weights[k]));
        snz           = c.add(snz, c.mul(tnz, weights[k]));
    }

    // then the instance's world matrix (the entity transform on top of the skinned model-space result)
    int m[16];
    for (crd::u32 e = 0; e < 16U; ++e) { m[e] = c.loadf(c.add(ibase, c.ku(e))); }
    const int wx  = c.add(c.add(c.mul(m[0], sx), c.mul(m[4], sy)), c.add(c.mul(m[8], sz), m[12]));
    const int wy  = c.add(c.add(c.mul(m[1], sx), c.mul(m[5], sy)), c.add(c.mul(m[9], sz), m[13]));
    const int wz  = c.add(c.add(c.mul(m[2], sx), c.mul(m[6], sy)), c.add(c.mul(m[10], sz), m[14]));
    const int nwx = c.add(c.add(c.mul(m[0], snx), c.mul(m[4], sny)), c.mul(m[8], snz));
    const int nwy = c.add(c.add(c.mul(m[1], snx), c.mul(m[5], sny)), c.mul(m[9], snz));
    const int nwz = c.add(c.add(c.mul(m[2], snx), c.mul(m[6], sny)), c.mul(m[10], snz));

    int clip[4];
    c.mul_view_proj(wx, wy, wz, clip);

    const int cr = c.loadf(c.add(ibase, c.ku(16U)));
    const int cg = c.loadf(c.add(ibase, c.ku(17U)));
    const int cb = c.loadf(c.add(ibase, c.ku(18U)));
    const int ca = c.loadf(c.add(ibase, c.ku(19U)));

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(clip[0], clip[1], clip[2], clip[3]);
    ve.n_out    = 2;
    ve.out[0]   = {g.vec3(nwx, nwy, nwz), 0, kir::Interp::Smooth};
    ve.out[1]   = {g.vec4(cr, cg, cb, ca), 1, kir::Interp::Flat};
}

// FS: N·L + ambient with the instance colour (light dir from header words 22..24; both renormalized).
void build_scene_fs(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    Gx c(g);

    const int vn = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    const int vc = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 1, kir::Interp::Flat);

    const int nx = g.swizzle(vn, 0);
    const int ny = g.swizzle(vn, 1);
    const int nz = g.swizzle(vn, 2);
    const int nl = c.mx(g.unary(kir::KOp::Sqrt, c.add(c.add(c.mul(nx, nx), c.mul(ny, ny)), c.mul(nz, nz))),
                        c.kf(1.0e-6));
    const int lx = c.hdrf(22U);
    const int ly = c.hdrf(23U);
    const int lz = c.hdrf(24U);
    const int ll = c.mx(g.unary(kir::KOp::Sqrt, c.add(c.add(c.mul(lx, lx), c.mul(ly, ly)), c.mul(lz, lz))),
                        c.kf(1.0e-6));
    const int ndl  = c.mx(c.dvd(c.add(c.add(c.mul(nx, lx), c.mul(ny, ly)), c.mul(nz, lz)), c.mul(nl, ll)), c.kf(0.0));
    const int lit  = c.add(c.kf(0.25), c.mul(c.kf(0.75), ndl)); // ambient + diffuse

    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(c.mul(g.swizzle(vc, 0), lit), c.mul(g.swizzle(vc, 1), lit), c.mul(g.swizzle(vc, 2), lit),
                        g.swizzle(vc, 3)),
                 0};
}

// REN-2 Half B: the TEXTURED scene VS — build_scene_vs + the vertex's uv0 (48-byte record words 6..7) emitted as a
// 3rd varying (loc 2), so the FS can sample the material base-color map at the mesh UV.
void build_scene_vs_textured(crd::kir::KGraph& g, crd::kir::KEntry& ve)
{
    namespace kir = crd::kir;
    Gx c(g);

    const int vid  = g.cast(g.builtin(kir::KBuiltin::VertexIndex), kir::DType::U32);
    const int idxc = c.hdru(0U);
    const int ii   = c.dvd(vid, idxc);
    const int li   = c.sub(vid, c.mul(ii, idxc));

    const int vidx  = c.loadu(c.add(c.hdru(2U), li));
    const int vbase = c.add(c.hdru(3U), c.mul(vidx, c.ku(kVertexWords)));
    const int px    = c.loadf(c.add(vbase, c.ku(0U)));
    const int py    = c.loadf(c.add(vbase, c.ku(1U)));
    const int pz    = c.loadf(c.add(vbase, c.ku(2U)));
    const int nx    = c.loadf(c.add(vbase, c.ku(3U)));
    const int ny    = c.loadf(c.add(vbase, c.ku(4U)));
    const int nz    = c.loadf(c.add(vbase, c.ku(5U)));
    const int u     = c.loadf(c.add(vbase, c.ku(6U))); // uv0 (bytes 24-31)
    const int v     = c.loadf(c.add(vbase, c.ku(7U)));

    const int slot  = c.loadu(c.add(c.hdru(5U), ii));
    const int ibase = c.add(c.hdru(4U), c.mul(slot, c.ku(kInstanceWords)));
    int       m[16];
    for (crd::u32 k = 0; k < 16U; ++k) { m[k] = c.loadf(c.add(ibase, c.ku(k))); }

    const int wx  = c.add(c.add(c.mul(m[0], px), c.mul(m[4], py)), c.add(c.mul(m[8], pz), m[12]));
    const int wy  = c.add(c.add(c.mul(m[1], px), c.mul(m[5], py)), c.add(c.mul(m[9], pz), m[13]));
    const int wz  = c.add(c.add(c.mul(m[2], px), c.mul(m[6], py)), c.add(c.mul(m[10], pz), m[14]));
    const int nwx = c.add(c.add(c.mul(m[0], nx), c.mul(m[4], ny)), c.mul(m[8], nz));
    const int nwy = c.add(c.add(c.mul(m[1], nx), c.mul(m[5], ny)), c.mul(m[9], nz));
    const int nwz = c.add(c.add(c.mul(m[2], nx), c.mul(m[6], ny)), c.mul(m[10], nz));

    int clip[4];
    c.mul_view_proj(wx, wy, wz, clip);

    const int cr = c.loadf(c.add(ibase, c.ku(16U)));
    const int cg = c.loadf(c.add(ibase, c.ku(17U)));
    const int cb = c.loadf(c.add(ibase, c.ku(18U)));
    const int ca = c.loadf(c.add(ibase, c.ku(19U)));

    ve.stage    = kir::KStage::Vertex;
    ve.position = g.vec4(clip[0], clip[1], clip[2], clip[3]);
    ve.n_out    = 3;
    ve.out[0]   = {g.vec3(nwx, nwy, nwz), 0, kir::Interp::Smooth};
    ve.out[1]   = {g.vec4(cr, cg, cb, ca), 1, kir::Interp::Flat};
    ve.out[2]   = {g.vec2(u, v), 2, kir::Interp::Smooth};
}

// REN-2 Half B: the TEXTURED scene FS — samples the material base-color (albedo) map at UV (set 0/binding 1,
// sampler binding 2) and modulates by the per-instance tint × N·L lighting.
void build_scene_fs_textured(crd::kir::KGraph& g, crd::kir::KEntry& fe)
{
    namespace kir = crd::kir;
    Gx c(g);

    const int vn   = g.stage_in(kir::KType::vec(kir::DType::F32, 3), 0, kir::Interp::Smooth);
    const int vc   = g.stage_in(kir::KType::vec(kir::DType::F32, 4), 1, kir::Interp::Flat);
    const int uv   = g.stage_in(kir::KType::vec(kir::DType::F32, 2), 2, kir::Interp::Smooth);
    const int tex  = g.texture(0, 1);
    const int samp = g.sampler(0, 2);
    const int alb  = g.tex_sample(tex, samp, uv); // vec4 base-color

    const int nx = g.swizzle(vn, 0);
    const int ny = g.swizzle(vn, 1);
    const int nz = g.swizzle(vn, 2);
    const int nl = c.mx(g.unary(kir::KOp::Sqrt, c.add(c.add(c.mul(nx, nx), c.mul(ny, ny)), c.mul(nz, nz))), c.kf(1.0e-6));
    const int lx = c.hdrf(22U);
    const int ly = c.hdrf(23U);
    const int lz = c.hdrf(24U);
    const int ll = c.mx(g.unary(kir::KOp::Sqrt, c.add(c.add(c.mul(lx, lx), c.mul(ly, ly)), c.mul(lz, lz))), c.kf(1.0e-6));
    const int ndl  = c.mx(c.dvd(c.add(c.add(c.mul(nx, lx), c.mul(ny, ly)), c.mul(nz, lz)), c.mul(nl, ll)), c.kf(0.0));
    const int lit  = c.add(c.kf(0.25), c.mul(c.kf(0.75), ndl));

    // albedo · tint · lighting
    const int r = c.mul(c.mul(g.swizzle(alb, 0), g.swizzle(vc, 0)), lit);
    const int gg = c.mul(c.mul(g.swizzle(alb, 1), g.swizzle(vc, 1)), lit);
    const int b = c.mul(c.mul(g.swizzle(alb, 2), g.swizzle(vc, 2)), lit);
    fe.stage  = kir::KStage::Fragment;
    fe.n_out  = 1;
    fe.out[0] = {g.vec4(r, gg, b, g.swizzle(vc, 3)), 0};
}

// ── hashing / small helpers ────────────────────────────────────────────────────────────────────────────────────────

constexpr crd::u64 kFnvOffset = 14695981039346656037ULL;
constexpr crd::u64 kFnvPrime  = 1099511628211ULL;

void hash_bytes(crd::u64& h, const void* data, crd::usize n) noexcept
{
    const auto* p = static_cast<const crd::u8*>(data);
    for (crd::usize i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= kFnvPrime;
    }
}

} // namespace

// ── the Impl ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct SceneRenderer::Impl
{
    crd::memory::IAllocator*         alloc  = nullptr;
    crd::gpu::IGpuContext*           ctx    = nullptr;
    crd::gpu::IRasterContext*        raster = nullptr;
    crd::resources::ResourceManager* rm     = nullptr;

    std::unique_ptr<crd::gpu::IGpuProgram>    vs;
    std::unique_ptr<crd::gpu::IGpuProgram>    fs;
    std::unique_ptr<crd::gpu::IRasterProgram> program;
    // GEO-8: the skinned program pair + the skeleton/clip handle caches + palette scratch
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_skinned;
    std::unique_ptr<crd::gpu::IRasterProgram> program_skinned;
    // REN-2 Half B: the TEXTURED program (samples the material base-color map); the per-material GPU-texture cache
    // is declared next to material_color below (ctor init order).
    std::unique_ptr<crd::gpu::IGpuProgram>    vs_textured;
    std::unique_ptr<crd::gpu::IGpuProgram>    fs_textured;
    std::unique_ptr<crd::gpu::IRasterProgram> program_textured;
    crd::containers::HashMap<crd::resources::ResourceId, crd::resources::ResourceHandle<crd::anim::SkeletonResource>>
        skeleton_cache{nullptr};
    crd::containers::HashMap<crd::resources::ResourceId, crd::resources::ResourceHandle<crd::anim::AnimClipResource>>
        clip_cache{nullptr};
    crd::containers::Array<crd::anim::JointPose>  pose_scratch{nullptr};
    crd::containers::Array<crd::math::Mat4f>      world_scratch{nullptr};
    crd::containers::Array<crd::math::Mat4f>      palette_scratch{nullptr};
    crd::containers::Array<crd::f32>              palette_staging{nullptr};

    crd::u64 structure_sig = 0;
    bool     has_structure = false;

    crd::containers::HashMap<crd::resources::ResourceId, crd::u32> group_of_mesh;
    // material colour cache (linear RGBA); resolved on rebuild
    crd::containers::HashMap<crd::resources::ResourceId, crd::math::Vec4f> material_color;
    // REN-2 Half B: the per-material GPU base-color texture cache (owns the uploaded ITexture)
    crd::containers::HashMap<crd::resources::ResourceId, std::unique_ptr<crd::gpu::ITexture>> material_texture{nullptr};
    // entity → (group << 32 | slot) — the BVH candidate → instance bridge, rebuilt with the structure
    crd::containers::HashMap<crd::scene::EntityId, crd::u64> entity_slot;

    // REN-1: the persistent frame graph (created lazily, reset per frame) — the whole scene's groups compose in
    // ONE submission. Null on a backend without it (DX12 until its port) ⇒ the synchronous per-draw fallback.
    std::unique_ptr<crd::gpu::IFrameGraph> frame_graph;
    bool                                   readback = true; // REN-8: on by default so gates keep read_pixel
    SceneRenderer::FramePassFn             overlay_fn   = nullptr; // the grid/gizmo/debug pass, in OUR graph
    void*                                  overlay_user = nullptr;

    explicit Impl(crd::memory::IAllocator* a)
        : alloc(a), skeleton_cache(a), clip_cache(a), pose_scratch(a), world_scratch(a), palette_scratch(a),
          palette_staging(a), group_of_mesh(a), material_color(a), material_texture(a), entity_slot(a)
    {
    }

    [[nodiscard]] crd::math::Vec4f resolve_color(const crd::resources::ResourceId& material)
    {
        if (material.is_null()) { return {0.8F, 0.8F, 0.8F, 1.0F}; }
        if (const crd::math::Vec4f* cached = material_color.find(material)) { return *cached; }
        crd::math::Vec4f color{0.8F, 0.8F, 0.8F, 1.0F};
        auto handle = rm->load_sync<crd::resources::OpenPbrMaterial>(material);
        if (handle.state() == crd::resources::LoadState::Ready && handle.get() != nullptr)
        {
            const crd::resources::PbrmParams& p = handle.get()->params;
            color = {p.base_color[0], p.base_color[1], p.base_color[2], p.base_alpha};
        }
        material_color.insert(material, color);
        return color;
    }

    // REN-2 Half B: resolve a material's base-color (albedo) map to a GPU texture — load the OpenPbrMaterial, follow
    // its `textures.base_color` ResourceId to the cooked TextureResource, and upload its mip chain VERBATIM via
    // create_texture_from_mips (the RET-3 seam). Cached per material; nullptr if the material has no base-color map
    // (⇒ the flat-colour path). The cache OWNS the texture; the returned pointer is borrowed (stable for the cache's life).
    [[nodiscard]] crd::gpu::ITexture* resolve_base_color_texture(const crd::resources::ResourceId& material)
    {
        if (material.is_null()) { return nullptr; }
        if (auto* cached = material_texture.find(material)) { return cached->get(); }
        std::unique_ptr<crd::gpu::ITexture> owned;
        auto mh = rm->load_sync<crd::resources::OpenPbrMaterial>(material);
        if (mh.state() == crd::resources::LoadState::Ready && mh.get() != nullptr)
        {
            const crd::resources::ResourceId& bc = mh.get()->textures.base_color;
            if (!bc.is_null())
            {
                auto th = rm->load_sync<crd::resources::TextureResource>(bc);
                if (th.state() == crd::resources::LoadState::Ready && th.get() != nullptr && th.get()->mip_count > 0U)
                {
                    const crd::resources::TextureResource& t = *th.get();
                    crd::containers::Array<const void*>     mip_ptrs(alloc);
                    for (crd::u32 i = 0; i < t.mip_count; ++i) { mip_ptrs.push_back(t.mips[i].pixels.data()); }
                    const bool srgb = t.format == crd::resources::TextureFormat::RGBA8UnormSrgb;
                    owned = raster->create_texture_from_mips(t.mips[0].width, t.mips[0].height, t.mip_count,
                                                             mip_ptrs.data(), srgb);
                }
            }
        }
        crd::gpu::ITexture* result = owned.get();
        material_texture.insert(material, std::move(owned));
        return result;
    }
};

SceneRenderer::SceneRenderer(crd::memory::IAllocator* alloc)
    : m_impl(std::make_unique<Impl>(alloc)), m_groups(alloc)
{
}

SceneRenderer::~SceneRenderer() = default;

bool SceneRenderer::init(crd::gpu::IRasterContext& raster, crd::resources::ResourceManager& rm)
{
    m_impl->raster = &raster;
    m_impl->rm     = &rm;
    return raster.valid();
}

void SceneRenderer::set_overlay_pass(FramePassFn fn, void* user) noexcept
{
    Impl& impl        = *m_impl;
    impl.overlay_fn   = fn;
    impl.overlay_user = user;
}

void SceneRenderer::set_readback_enabled(bool on) noexcept
{
    Impl& impl    = *m_impl;
    impl.readback = on;
    // the graph may not exist yet (it is created lazily on the first render) — `render()` re-applies the flag
    if (impl.frame_graph != nullptr) { impl.frame_graph->set_readback_enabled(on); }
}

bool SceneRenderer::init_programs(crd::gpu::IGpuContext& ctx)
{
    if (m_impl->raster == nullptr) { return false; }
    m_impl->ctx = &ctx;

    crd::kir::KGraph vg(m_impl->alloc);
    crd::kir::KEntry ve;
    build_scene_vs(vg, ve);
    crd::kir::KGraph fg(m_impl->alloc);
    crd::kir::KEntry fe;
    build_scene_fs(fg, fe);
    m_impl->vs = ctx.create_program(vg, ve);
    m_impl->fs = ctx.create_program(fg, fe);
    if (m_impl->vs == nullptr || m_impl->fs == nullptr) { return false; }
    m_impl->program = m_impl->raster->create_raster_program(*m_impl->vs, *m_impl->fs);

    crd::kir::KGraph svg(m_impl->alloc);
    crd::kir::KEntry sve;
    build_scene_vs_skinned(svg, sve);
    m_impl->vs_skinned = ctx.create_program(svg, sve);
    if (m_impl->vs_skinned != nullptr)
    {
        m_impl->program_skinned = m_impl->raster->create_raster_program(*m_impl->vs_skinned, *m_impl->fs);
    }

    // REN-2 Half B: the TEXTURED program — a group whose material carries a base-color map draws through this
    // (samples albedo at the mesh UV) instead of the flat program.
    crd::kir::KGraph tvg(m_impl->alloc);
    crd::kir::KEntry tve;
    build_scene_vs_textured(tvg, tve);
    crd::kir::KGraph tfg(m_impl->alloc);
    crd::kir::KEntry tfe;
    build_scene_fs_textured(tfg, tfe);
    m_impl->vs_textured = ctx.create_program(tvg, tve);
    m_impl->fs_textured = ctx.create_program(tfg, tfe);
    if (m_impl->vs_textured != nullptr && m_impl->fs_textured != nullptr)
    {
        m_impl->program_textured = m_impl->raster->create_raster_program(*m_impl->vs_textured, *m_impl->fs_textured);
    }
    return m_impl->program != nullptr;
}

namespace
{

// the chunk-visitor context for the two extraction passes
struct ExtractCtx
{
    crd::scene::World*        world = nullptr;
    SceneRenderer::Impl*      impl  = nullptr;
    crd::containers::Array<MeshGroup>* groups = nullptr;
    crd::scene::ComponentId   transform_id{};
    crd::scene::ComponentId   renderer_id{};
    crd::scene::ComponentId   animator_id{}; // GEO-8
    SyncStats*                stats = nullptr;

    // pass 1 outputs
    crd::u64 sig       = kFnvOffset;
    bool     any_dirty = false;

    // rebuild-pass scratch: per-(chunk × group) run starts
    crd::containers::Array<crd::u32> run_first; // parallel to groups, valid within one chunk visit

    explicit ExtractCtx(crd::memory::IAllocator* a) : run_first(a) {}
};

// resolve-or-create the group for a mesh id (loads the mesh resource; null on load failure)
[[nodiscard]] crd::i64 group_for_mesh(ExtractCtx& ctx, const crd::resources::ResourceId& mesh_id)
{
    if (const crd::u32* found = ctx.impl->group_of_mesh.find(mesh_id)) { return static_cast<crd::i64>(*found); }

    auto handle = ctx.impl->rm->load_sync<crd::resources::MeshResource>(mesh_id);
    if (handle.state() != crd::resources::LoadState::Ready || handle.get() == nullptr)
    {
        ++ctx.stats->meshes_pending;
        return -1;
    }
    const crd::resources::MeshResource* mesh = handle.get();
    if (mesh->indices.size() < 4U || !mesh->has_bounds()) { return -1; }

    MeshGroup group(ctx.impl->alloc);
    group.mesh_id     = mesh_id;
    group.mesh        = handle; // keeps the payload resident
    group.index_count = static_cast<crd::u32>(mesh->indices.size() / 4U);
    ctx.groups->push_back(static_cast<MeshGroup&&>(group));
    const crd::u32 gi = static_cast<crd::u32>(ctx.groups->size() - 1U);
    ctx.impl->group_of_mesh.insert(mesh_id, gi);
    return static_cast<crd::i64>(gi);
}

// write one instance record + world AABB into a group slot
void write_slot(MeshGroup& group, crd::u32 slot, const crd::scene::Transform& t, const crd::math::Vec4f& color)
{
    InstanceGpu& rec = group.instances[slot];
    std::memcpy(rec.world, &t.world, sizeof(rec.world)); // Mat4f = 4 packed Vec4 columns = column-major floats
    rec.color[0] = color.x;
    rec.color[1] = color.y;
    rec.color[2] = color.z;
    rec.color[3] = color.w;

    const crd::resources::MeshResource* mesh = group.mesh.get();
    crd::geometry::primitives::AABB3<crd::f32> local;
    local.min = {mesh->bounds_min[0], mesh->bounds_min[1], mesh->bounds_min[2]};
    local.max = {mesh->bounds_max[0], mesh->bounds_max[1], mesh->bounds_max[2]};
    group.world_bounds[slot] = crd::geometry::primitives::transform_aabb(t.world, local);
}

// pass 1: the structure signature + any-dirty detection (no mutation)
void pass_signature(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* renderers = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (renderers == nullptr || view.entity_count == 0U) { return; }

    hash_bytes(ctx.sig, &view.entities, sizeof(view.entities)); // the chunk key (entity-array pointer)
    hash_bytes(ctx.sig, view.entities, sizeof(crd::scene::EntityId) * view.entity_count);
    hash_bytes(ctx.sig, renderers, sizeof(crd::scene::MeshRenderer) * view.entity_count);
}

// pass 2a: FULL rebuild — append every instance, group runs per (chunk × group)
void pass_rebuild(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    // per-chunk: remember each group's size BEFORE this chunk appends (the run start)
    ctx.run_first.clear();
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        ctx.run_first.push_back(static_cast<crd::u32>((*ctx.groups)[gi].instances.size()));
    }

    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::i64 gi64 = group_for_mesh(ctx, renderers[i].mesh);
        if (gi64 < 0) { continue; }
        const auto gi = static_cast<crd::usize>(gi64);
        while (ctx.run_first.size() < ctx.groups->size()) // groups created mid-chunk start their run at 0
        {
            ctx.run_first.push_back(0U);
        }
        MeshGroup& group = (*ctx.groups)[gi];
        const auto slot  = static_cast<crd::u32>(group.instances.size());
        group.instances.push_back(InstanceGpu{});
        group.slot_entity.push_back(view.entities[i]);
        group.world_bounds.push_back({});
        group.slot_skeleton.push_back({});
        group.slot_clip.push_back({});
        group.slot_time.push_back(0.0F);
        if (group.material.is_null()) { group.material = renderers[i].material; } // REN-2 Half B: representative material
        write_slot(group, slot, transforms[i], ctx.impl->resolve_color(renderers[i].material));
        ctx.impl->entity_slot.insert(view.entities[i],
                                     (static_cast<crd::u64>(gi) << 32U) | static_cast<crd::u64>(slot));
    }

    // record this chunk's runs
    const crd::u64 tversion = view.version_of(ctx.transform_id);
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        const crd::u32 first = ctx.run_first[gi];
        const auto     now   = static_cast<crd::u32>((*ctx.groups)[gi].instances.size());
        if (now > first)
        {
            ChunkRun run;
            run.chunk_key = view.entities;
            run.version   = tversion;
            run.first     = first;
            run.count     = now - first;
            run.dirty     = true;
            (*ctx.groups)[gi].runs.push_back(run);
        }
    }
}

// pass 2b: INCREMENTAL — re-extract only chunks whose Transform version moved (slots unchanged by construction:
// the structure signature matched)
void pass_update(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* transforms = view.array<const crd::scene::Transform>(ctx.transform_id);
    const auto* renderers  = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    if (transforms == nullptr || renderers == nullptr || view.entity_count == 0U) { return; }

    const crd::u64 tversion = view.version_of(ctx.transform_id);

    // is any run of this chunk stale?
    bool stale = false;
    for (crd::usize gi = 0; gi < ctx.groups->size() && !stale; ++gi)
    {
        for (const ChunkRun& run : (*ctx.groups)[gi].runs)
        {
            if (run.chunk_key == view.entities && run.version != tversion)
            {
                stale = true;
                break;
            }
        }
    }
    if (!stale) { return; }

    // rewrite this chunk's slots group by group, walking entities in visit order (the rebuild's order)
    ctx.run_first.clear();
    for (crd::usize gi = 0; gi < ctx.groups->size(); ++gi)
    {
        crd::u32 first = 0U;
        for (ChunkRun& run : (*ctx.groups)[gi].runs)
        {
            if (run.chunk_key == view.entities)
            {
                first       = run.first;
                run.version = tversion;
                run.dirty   = true;
                ++ctx.stats->dirty_runs;
            }
        }
        ctx.run_first.push_back(first);
    }
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::u32* gi_found = ctx.impl->group_of_mesh.find(renderers[i].mesh);
        if (gi_found == nullptr) { continue; }
        const crd::usize gi   = *gi_found;
        const crd::u32   slot = ctx.run_first[gi]++;
        write_slot((*ctx.groups)[gi], slot, transforms[i], ctx.impl->resolve_color(renderers[i].material));
    }
}

// GEO-8: the animator pass — every sync, over the SKINNED chunks only (Transform+MeshRenderer+SkeletonAnimator):
// mirror the animator state into the groups' slot arrays (palettes re-sample from these each frame)
void pass_animators(const crd::scene::ChunkView& view, void* ud)
{
    auto& ctx = *static_cast<ExtractCtx*>(ud);
    const auto* renderers = view.array<const crd::scene::MeshRenderer>(ctx.renderer_id);
    const auto* animators = view.array<const crd::scene::SkeletonAnimator>(ctx.animator_id);
    if (renderers == nullptr || animators == nullptr) { return; }
    for (crd::u32 i = 0; i < view.entity_count; ++i)
    {
        const crd::u64* packed = ctx.impl->entity_slot.find(view.entities[i]);
        if (packed == nullptr) { continue; }
        const auto gi   = static_cast<crd::u32>(*packed >> 32U);
        const auto slot = static_cast<crd::u32>(*packed & 0xFFFFFFFFU);
        MeshGroup& group = (*ctx.groups)[gi];
        if (slot >= group.slot_skeleton.size()) { continue; }
        group.slot_skeleton[slot] = animators[i].skeleton;
        group.slot_clip[slot]     = animators[i].clip;
        group.slot_time[slot]     = animators[i].time * animators[i].speed;
    }
}

} // namespace

SyncStats SceneRenderer::sync(crd::scene::World& world)
{
    SyncStats stats;
    Impl&     impl = *m_impl;

    ExtractCtx ctx(impl.alloc);
    ctx.world        = &world;
    ctx.impl         = &impl;
    ctx.groups       = &m_groups;
    ctx.transform_id = world.component_id<crd::scene::Transform>();
    ctx.renderer_id  = world.component_id<crd::scene::MeshRenderer>();
    ctx.animator_id  = world.component_id<crd::scene::SkeletonAnimator>();
    ctx.stats        = &stats;

    // pass 1: the structure signature
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_signature, &ctx);
    }

    const bool structural = !impl.has_structure || ctx.sig != impl.structure_sig;
    if (structural)
    {
        stats.structural_rebuild = true;
        for (MeshGroup& group : m_groups) // keep buffers/meshes; drop the instance tables
        {
            group.instances.clear();
            group.slot_entity.clear();
            group.world_bounds.clear();
            group.runs.clear();
            group.slot_skeleton.clear();
            group.slot_clip.clear();
            group.slot_time.clear();
        }
        impl.entity_slot.clear();
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_rebuild, &ctx);
        impl.structure_sig = ctx.sig;
        impl.has_structure = true;
    }
    else
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer>();
        q.for_each_chunk(&pass_update, &ctx);
    }

    // GEO-8: mirror the animator state (skinned chunks only — the third required component narrows the walk)
    {
        auto q = world.query<crd::scene::Transform, crd::scene::MeshRenderer, crd::scene::SkeletonAnimator>();
        q.for_each_chunk(&pass_animators, &ctx);
    }

    // ── GPU: (re)create buffers + upload geometry once + instance payloads by dirty grain ──────────────────────
    for (MeshGroup& group : m_groups)
    {
        const auto count = static_cast<crd::u32>(group.instances.size());
        stats.total_instances += count;
        if (count == 0U) { continue; }

        const crd::resources::MeshResource* mesh = group.mesh.get();
        const auto vertex_words = static_cast<crd::u32>(mesh->vertices.size() / 4U);
        const auto vcount       = static_cast<crd::u32>(mesh->vertices.size() / 48U);
        const crd::u32 needed_capacity = count;

        // GEO-8: a skinned group's JOINT COUNT comes from the first slot's skeleton (uniform per mesh in practice)
        if (mesh->has_skin() && group.joint_count == 0U)
        {
            for (crd::usize s = 0; s < group.slot_skeleton.size(); ++s)
            {
                if (group.slot_skeleton[s].is_null()) { continue; }
                auto handle = impl.rm->load_sync<crd::anim::SkeletonResource>(group.slot_skeleton[s]);
                if (handle.state() == crd::resources::LoadState::Ready && handle.get() != nullptr)
                {
                    group.joint_count = handle.get()->joint_count();
                    group.skinned     = true;
                    group.buffer.reset(); // relayout with the skin + palette sections
                    break;
                }
            }
        }

        if (group.buffer == nullptr || group.capacity < needed_capacity)
        {
            group.indices_off   = kHeaderWords;
            group.vertices_off  = group.indices_off + group.index_count;
            group.skin_off      = group.vertices_off + vertex_words;
            const crd::u32 skin_words = group.skinned ? vcount * 6U : 0U;
            group.instances_off = group.skin_off + skin_words;
            group.palette_off   = group.instances_off + needed_capacity * kInstanceWords;
            const crd::u32 palette_words = group.skinned ? needed_capacity * group.joint_count * 16U : 0U;
            group.visible_off   = group.palette_off + palette_words;
            const crd::u32 total_words = group.visible_off + needed_capacity;
            group.buffer             = impl.raster->create_storage_buffer(total_words * 4U);
            group.capacity           = needed_capacity;
            group.geometry_uploaded  = false;
        }
        if (group.buffer == nullptr) { continue; }

        if (!group.geometry_uploaded || structural)
        {
            if (!group.geometry_uploaded)
            {
                (void)impl.raster->upload_storage(*group.buffer, group.indices_off * 4U, mesh->indices.data(),
                                                  static_cast<crd::u32>(mesh->indices.size()));
                (void)impl.raster->upload_storage(*group.buffer, group.vertices_off * 4U, mesh->vertices.data(),
                                                  static_cast<crd::u32>(mesh->vertices.size()));
                if (group.skinned) // the packed skin stream: 2 words of u16-pair joints + 4 weight words
                {
                    crd::containers::Array<crd::u32> packed(impl.alloc);
                    packed.resize(static_cast<crd::usize>(vcount) * 6U);
                    for (crd::u32 v = 0; v < vcount; ++v)
                    {
                        const crd::u16* j = mesh->skin_joints.data() + static_cast<crd::usize>(v) * 4U;
                        const crd::f32* w = mesh->skin_weights.data() + static_cast<crd::usize>(v) * 4U;
                        packed[static_cast<crd::usize>(v) * 6U + 0U] =
                            static_cast<crd::u32>(j[0]) | (static_cast<crd::u32>(j[1]) << 16U);
                        packed[static_cast<crd::usize>(v) * 6U + 1U] =
                            static_cast<crd::u32>(j[2]) | (static_cast<crd::u32>(j[3]) << 16U);
                        std::memcpy(packed.data() + static_cast<crd::usize>(v) * 6U + 2U, w, 16U);
                    }
                    (void)impl.raster->upload_storage(*group.buffer, group.skin_off * 4U, packed.data(),
                                                      static_cast<crd::u32>(packed.size() * 4U));
                }
                group.geometry_uploaded = true;
            }
            // a fresh buffer / a rebuilt table needs the FULL instance payload regardless of per-run dirt
            const crd::u32 bytes = count * static_cast<crd::u32>(sizeof(InstanceGpu));
            (void)impl.raster->upload_storage(*group.buffer, group.instances_off * 4U, group.instances.data(), bytes);
            stats.uploaded_bytes += bytes;
            for (ChunkRun& run : group.runs) { run.dirty = false; }
            continue;
        }

        // incremental: upload ONLY the dirty runs' byte ranges (the chunk-grain partial re-upload — the gate)
        for (ChunkRun& run : group.runs)
        {
            if (!run.dirty) { continue; }
            run.dirty            = false;
            const crd::u32 bytes = run.count * static_cast<crd::u32>(sizeof(InstanceGpu));
            (void)impl.raster->upload_storage(*group.buffer,
                                              (group.instances_off + run.first * kInstanceWords) * 4U,
                                              group.instances.data() + run.first, bytes);
            stats.uploaded_bytes += bytes;
        }
    }

    // ── GEO-8: the skinned palettes — sample every skinned instance's clip, upload the palette section.
    // Animation is ALWAYS dirty by definition: this is per-frame data, deliberately outside the partial-upload
    // accounting the static gate measures.
    for (MeshGroup& group : m_groups)
    {
        if (!group.skinned || group.buffer == nullptr || group.instances.size() == 0U) { continue; }
        const crd::u32 jc    = group.joint_count;
        const auto     count = static_cast<crd::u32>(group.instances.size());
        impl.palette_staging.resize(static_cast<crd::usize>(count) * jc * 16U);
        impl.pose_scratch.resize(jc);
        impl.world_scratch.resize(jc);
        impl.palette_scratch.resize(jc);

        for (crd::u32 slot = 0; slot < count; ++slot)
        {
            crd::f32* dst = impl.palette_staging.data() + static_cast<crd::usize>(slot) * jc * 16U;
            const crd::anim::SkeletonResource* skel = nullptr;
            if (!group.slot_skeleton[slot].is_null())
            {
                auto* cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                if (cached == nullptr)
                {
                    impl.skeleton_cache.insert(
                        group.slot_skeleton[slot],
                        impl.rm->load_sync<crd::anim::SkeletonResource>(group.slot_skeleton[slot]));
                    cached = impl.skeleton_cache.find(group.slot_skeleton[slot]);
                }
                if (cached != nullptr && cached->get() != nullptr && cached->get()->joint_count() == jc)
                {
                    skel = cached->get();
                }
            }
            if (skel == nullptr) // no skeleton → identity palette (bind pose renders)
            {
                for (crd::u32 j = 0; j < jc; ++j)
                {
                    for (crd::u32 c = 0; c < 16U; ++c)
                    {
                        dst[j * 16U + c] = (c % 5U) == 0U ? 1.0F : 0.0F;
                    }
                }
                continue;
            }

            const crd::anim::AnimClipResource* clip = nullptr;
            if (!group.slot_clip[slot].is_null())
            {
                auto* ccached = impl.clip_cache.find(group.slot_clip[slot]);
                if (ccached == nullptr)
                {
                    impl.clip_cache.insert(group.slot_clip[slot],
                                           impl.rm->load_sync<crd::anim::AnimClipResource>(group.slot_clip[slot]));
                    ccached = impl.clip_cache.find(group.slot_clip[slot]);
                }
                if (ccached != nullptr) { clip = ccached->get(); }
            }

            if (clip != nullptr && clip->duration > 0.0F)
            {
                const crd::f32 t = group.slot_time[slot] - clip->duration
                                       * static_cast<crd::f32>(static_cast<crd::i64>(group.slot_time[slot]
                                                                                     / clip->duration));
                crd::anim::sample_clip(*clip, *skel, t, {impl.pose_scratch.data(), impl.pose_scratch.size()});
            }
            else // no clip: the rest pose
            {
                for (crd::u32 j = 0; j < jc; ++j)
                {
                    const crd::f32* r                 = skel->rest.data() + static_cast<crd::usize>(j) * crd::anim::kRestFloats;
                    impl.pose_scratch[j].translation = {r[0], r[1], r[2]};
                    impl.pose_scratch[j].rotation    = {r[3], r[4], r[5], r[6]};
                    impl.pose_scratch[j].scale       = {r[7], r[8], r[9]};
                }
            }
            crd::anim::compute_pose_matrices(*skel, {impl.pose_scratch.data(), impl.pose_scratch.size()},
                                             {impl.world_scratch.data(), impl.world_scratch.size()});
            crd::anim::compute_skin_palette(*skel, {impl.world_scratch.data(), impl.world_scratch.size()},
                                            {impl.palette_scratch.data(), impl.palette_scratch.size()});
            std::memcpy(dst, impl.palette_scratch.data(), static_cast<crd::usize>(jc) * 64U);
        }
        (void)impl.raster->upload_storage(*group.buffer, group.palette_off * 4U, impl.palette_staging.data(),
                                          static_cast<crd::u32>(impl.palette_staging.size() * 4U));
    }

    stats.groups = static_cast<crd::u32>(m_groups.size());
    return stats;
}

// REN-1: one culled group's draw (the frame graph records the whole list in ONE submission).
namespace
{
struct SceneDraw
{
    crd::gpu::IRasterProgram* program    = nullptr;
    crd::gpu::IStorageBuffer* buffer     = nullptr;
    crd::gpu::ITexture*       base_color = nullptr; // REN-2 Half B: the material albedo map (null ⇒ the flat draw)
    crd::u32                  vertex_count = 0;
};
struct SceneDrawState
{
    crd::gpu::IRasterTarget*                target = nullptr;
    crd::gpu::ClearColor                    clear{};
    const crd::containers::Array<SceneDraw>* draws = nullptr;
};
// REN-2 Half B: record ONE scene group — the first clears colour+depth, later groups LOAD; a group whose material
// carries a base-color map draws through draw_storage_textured_depth (samples albedo), else the flat draw.
void record_one_group(crd::gpu::IRasterContext& r, crd::gpu::IRasterTarget& t, const SceneDraw& d,
                      crd::gpu::ClearColor clear, bool first)
{
    const auto cmp = crd::gpu::DepthCompare::GreaterEqual;
    if (d.base_color != nullptr)
    {
        if (first) { r.draw_storage_textured_depth(t, *d.program, clear, 0.0F, cmp, *d.buffer, *d.base_color, d.vertex_count); }
        else { r.draw_storage_textured_depth_load(t, *d.program, cmp, *d.buffer, *d.base_color, d.vertex_count); }
    }
    else if (first) { r.draw_storage_depth(t, *d.program, clear, 0.0F, cmp, *d.buffer, d.vertex_count); }
    else { r.draw_storage_depth_load(t, *d.program, cmp, *d.buffer, d.vertex_count); }
}
// the scene pass's recording callback: the FIRST group clears colour+depth, every later group LOADs and
// composes through the frame's real depth (the GEO-8 multi-group contract, now in ONE submission).
void record_scene_groups(crd::gpu::IFrameContext& ctx, void* user)
{
    const auto* s = static_cast<const SceneDrawState*>(user);
    for (crd::usize i = 0; i < s->draws->size(); ++i)
    {
        record_one_group(ctx.raster(), *s->target, (*s->draws)[i], s->clear, i == 0U);
    }
}
} // namespace

RenderStats SceneRenderer::render(crd::gpu::IRasterTarget& target, const crd::math::Mat4f& view_proj,
                                  const crd::math::Vec3f& light_dir, crd::gpu::ClearColor clear,
                                  const crd::scene::SpatialBVHIndex* bvh)
{
    RenderStats stats;
    Impl&       impl = *m_impl;
    // REN-8: wall-clock the WHOLE render call, including the frame graph's fence wait. render() has several
    // early returns, so an RAII stamp is the only way to time it without a leak-prone exit in each branch.
    struct CpuStamp
    {
        RenderStats*                                   s;
        std::chrono::steady_clock::time_point          t0 = std::chrono::steady_clock::now();
        ~CpuStamp()
        {
            s->cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        }
    } cpu_stamp{&stats};
    if (impl.program == nullptr) { return stats; }

    crd::math::Vec4f planes[6];
    frustum_planes(view_proj, planes);
    crd::containers::Array<SceneDraw> draw_list(impl.alloc); // the frame's culled groups → one submission

    // broad phase: a configured BVH prunes to the frustum's AABB; otherwise every slot is a candidate
    const bool use_bvh = bvh != nullptr && bvh->is_configured();
    crd::containers::Array<crd::scene::EntityId> candidates(impl.alloc);
    if (use_bvh) { bvh->overlap(frustum_aabb(view_proj), candidates); }

    for (MeshGroup& group : m_groups)
    {
        group.visible.clear();
        const auto count = static_cast<crd::u32>(group.instances.size());
        if (count == 0U || group.buffer == nullptr) { continue; }

        if (use_bvh)
        {
            for (const crd::scene::EntityId e : candidates)
            {
                const crd::u64* packed = impl.entity_slot.find(e);
                if (packed == nullptr) { continue; }
                const auto gi   = static_cast<crd::u32>(*packed >> 32U);
                const auto slot = static_cast<crd::u32>(*packed & 0xFFFFFFFFU);
                if (&m_groups[gi] != &group) { continue; }
                if (aabb_in_frustum(group.world_bounds[slot], planes)) { group.visible.push_back(slot); }
            }
        }
        else
        {
            for (crd::u32 slot = 0; slot < count; ++slot)
            {
                if (aabb_in_frustum(group.world_bounds[slot], planes)) { group.visible.push_back(slot); }
            }
        }

        const auto visible_count = static_cast<crd::u32>(group.visible.size());
        stats.culled_instances += count - visible_count;
        if (visible_count == 0U) { continue; }

        // per-frame uploads: the 32-word header + the visible list
        crd::u32 header[kHeaderWords] = {};
        header[0] = group.index_count;
        header[1] = visible_count;
        header[2] = group.indices_off;
        header[3] = group.vertices_off;
        header[4] = group.instances_off;
        header[5] = group.visible_off;
        std::memcpy(&header[6], &view_proj, 16U * 4U);
        std::memcpy(&header[22], &light_dir, 3U * 4U);
        header[25] = group.skin_off;    // GEO-8: the skinned VS's extra sections
        header[26] = group.palette_off;
        header[27] = group.joint_count;
        (void)impl.raster->upload_storage(*group.buffer, 0U, header, sizeof(header));
        (void)impl.raster->upload_storage(*group.buffer, group.visible_off * 4U, group.visible.data(),
                                          visible_count * 4U);
        stats.uploaded_bytes += sizeof(header) + static_cast<crd::u64>(visible_count) * 4U;

        // REN-2 Half B: a group whose material carries a base-color map draws TEXTURED (samples albedo); else flat.
        // (Groups batch by MESH; the group's representative material drives the map — correct for one-material meshes.
        // Per-instance material textures are a bindless follow-up. Skinned takes precedence — no textured-skinned yet.)
        crd::gpu::ITexture*       base_color = group.skinned ? nullptr : impl.resolve_base_color_texture(group.material);
        crd::gpu::IRasterProgram* program    = impl.program.get();
        if (group.skinned && impl.program_skinned != nullptr) { program = impl.program_skinned.get(); }
        else if (base_color != nullptr && impl.program_textured != nullptr) { program = impl.program_textured.get(); }
        else { base_color = nullptr; } // no textured program available ⇒ the flat path (drop the map)
        SceneDraw d;
        d.program      = program;
        d.buffer       = group.buffer.get();
        d.base_color   = base_color;
        d.vertex_count = visible_count * group.index_count;
        draw_list.push_back(d);
        ++stats.draws;
        stats.drawn_instances += visible_count;
    }

    if (draw_list.size() == 0U) { return stats; }

    // REN-1: compose all N culled groups in ONE submission through the frame graph (the async single-submission
    // surface). The per-frame header/visible uploads already ran (synchronous transfers, complete before this).
    if (impl.frame_graph == nullptr) { impl.frame_graph = impl.raster->create_frame_graph(); }
    if (impl.frame_graph != nullptr)
    {
        crd::gpu::IFrameGraph& fg = *impl.frame_graph;
        fg.set_readback_enabled(impl.readback); // re-applied per frame: the graph is created lazily
        fg.reset();
        const crd::gpu::FgImage img = fg.import_target(target);
        crd::gpu::IFramePassBuilder& scene = fg.add_pass("scene");
        scene.writes(img);
        for (crd::usize i = 0; i < draw_list.size(); ++i) { scene.reads(fg.import_storage(*draw_list[i].buffer)); }
        SceneDrawState state{&target, clear, &draw_list};
        scene.execute(&record_scene_groups, &state);
        // ⛔ HARD RULE: the overlay is a PASS in this graph, not a separate submission. `read_writes` (not
        // `writes`) is what tells the graph it composites ON TOP of the scene — declaring it a plain write would
        // let the scheduler believe the scene's output is dead and reorder or alias it away.
        if (impl.overlay_fn != nullptr)
        {
            fg.add_pass("overlay").read_writes(img).execute(impl.overlay_fn, impl.overlay_user);
        }
        if (fg.build())
        {
            fg.execute();
            // REN-8: what the DEVICE spent. Compared against `cpu_ms` below, the gap is the frame's stall.
            stats.gpu_ms       = fg.gpu_ms_total();
            stats.timed_passes = fg.pass_count();
        }
    }
    else // synchronous fallback (a backend without the frame graph): the GEO-8 first-clears-rest-load loop
    {
        for (crd::usize i = 0; i < draw_list.size(); ++i)
        {
            record_one_group(*impl.raster, target, draw_list[i], clear, i == 0U);
        }
    }
    return stats;
}

// ── frustum helpers ────────────────────────────────────────────────────────────────────────────────────────────────

void frustum_planes(const crd::math::Mat4f& view_proj, crd::math::Vec4f out_planes[6])
{
    // Gribb–Hartmann on the column-major matrix: row_i[j] = m.c{j}[i]. Clip volume: |x|≤w, |y|≤w, 0≤z≤w.
    const auto row = [&](int i) -> crd::math::Vec4f {
        const crd::f32* m = reinterpret_cast<const crd::f32*>(&view_proj);
        return {m[0 * 4 + i], m[1 * 4 + i], m[2 * 4 + i], m[3 * 4 + i]};
    };
    const crd::math::Vec4f r0 = row(0);
    const crd::math::Vec4f r1 = row(1);
    const crd::math::Vec4f r2 = row(2);
    const crd::math::Vec4f r3 = row(3);
    const auto add4 = [](const crd::math::Vec4f& a, const crd::math::Vec4f& b) {
        return crd::math::Vec4f{a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
    };
    const auto sub4 = [](const crd::math::Vec4f& a, const crd::math::Vec4f& b) {
        return crd::math::Vec4f{a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
    };
    out_planes[0] = add4(r3, r0); // left:   x ≥ −w
    out_planes[1] = sub4(r3, r0); // right:  x ≤ +w
    out_planes[2] = add4(r3, r1); // bottom
    out_planes[3] = sub4(r3, r1); // top
    out_planes[4] = r2;           // near:   z ≥ 0 (the [0,1] clip convention — reverse-Z far plane in practice)
    out_planes[5] = sub4(r3, r2); // far:    z ≤ w
}

bool aabb_in_frustum(const crd::geometry::primitives::AABB3<crd::f32>& box, const crd::math::Vec4f planes[6]) noexcept
{
    for (int p = 0; p < 6; ++p)
    {
        const crd::math::Vec4f& pl = planes[p];
        // the positive vertex: the corner farthest along the plane normal
        const crd::f32 px = pl.x >= 0.0F ? box.max.x : box.min.x;
        const crd::f32 py = pl.y >= 0.0F ? box.max.y : box.min.y;
        const crd::f32 pz = pl.z >= 0.0F ? box.max.z : box.min.z;
        if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0.0F) { return false; }
    }
    return true;
}

crd::geometry::primitives::AABB3<crd::f32> frustum_aabb(const crd::math::Mat4f& view_proj)
{
    const crd::math::Mat4f inv = crd::math::inverse(view_proj);
    crd::geometry::primitives::AABB3<crd::f32> box;
    bool first = true;
    for (int zi = 0; zi < 2; ++zi)
    {
        for (int yi = 0; yi < 2; ++yi)
        {
            for (int xi = 0; xi < 2; ++xi)
            {
                const crd::math::Vec4f clip{xi != 0 ? 1.0F : -1.0F, yi != 0 ? 1.0F : -1.0F,
                                            zi != 0 ? 1.0F : 0.0F, 1.0F};
                const crd::math::Vec4f h = inv * clip;
                if (h.w == 0.0F || !std::isfinite(h.w)) // degenerate → an everything-box (prunes nothing — safe)
                {
                    constexpr crd::f32 big = 1.0e18F;
                    return {{-big, -big, -big}, {big, big, big}};
                }
                const crd::math::Vec3f w{h.x / h.w, h.y / h.w, h.z / h.w};
                if (first)
                {
                    box.min = w;
                    box.max = w;
                    first   = false;
                }
                else
                {
                    box.min.x = w.x < box.min.x ? w.x : box.min.x;
                    box.min.y = w.y < box.min.y ? w.y : box.min.y;
                    box.min.z = w.z < box.min.z ? w.z : box.min.z;
                    box.max.x = w.x > box.max.x ? w.x : box.max.x;
                    box.max.y = w.y > box.max.y ? w.y : box.max.y;
                    box.max.z = w.z > box.max.z ? w.z : box.max.z;
                }
            }
        }
    }
    return box;
}

} // namespace crd::scenerender
