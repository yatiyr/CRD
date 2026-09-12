// test_anim.cpp — GEO-8 (D-007 row 73): the animation-resource gates.
//  · builder → loader round-trip for SKEL and ANIM (byte contracts + the topological refusal)
//  · the BIND-POSE IDENTITY gate: pose at rest with IBM = inverse(rest world) → palette == identity
//  · glTF-spec sampling: SLERP for linear rotation, hold/clamp semantics, hierarchy composition oracle
//  · dual-quaternion round-trip: DQ transform == matrix transform on rigid palettes (the B8-j CPU oracle)
//  · THE FOX GATE: the REAL Khronos skinned character cooks through the wave1 handler (SKEL + 3 ANIM clips +
//    SKNV), loads, and RESAMPLES BIT-STABLE at every key time vs the raw glTF channel data.

#include <crd/anim/anim_resources.hpp>
#include <crd/anim/pose.hpp>
#include <crd/assetio/gltf.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/cooker/cook_io.hpp>
#include <crd/math/mat.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/resources/crdr.hpp>
#include <crd/resources/resource_id.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <cstring>

using namespace crd;

namespace crd::cooker
{
void register_wave1_mesh_handler();
}

namespace
{

memory::TlsfAllocator& galloc()
{
    static memory::TlsfAllocator a(128U << 20U);
    return a;
}

// a 3-joint chain: root → mid (t=+2x) → tip (t=+1y), rest rotations identity
anim::SkeletonResource make_chain()
{
    anim::SkeletonResource s(&galloc());
    const f32 rests[3][10] = {{0, 0, 0, 0, 0, 0, 1, 1, 1, 1},
                              {2, 0, 0, 0, 0, 0, 1, 1, 1, 1},
                              {0, 1, 0, 0, 0, 0, 1, 1, 1, 1}};
    const i32 parents[3]   = {-1, 0, 1};
    const char* names[3]   = {"root", "mid", "tip"};
    for (u32 j = 0; j < 3U; ++j)
    {
        s.parents.push_back(parents[j]);
        for (f32 v : rests[j]) { s.rest.push_back(v); }
        // IBM filled by the test cases (identity here)
        for (u32 c = 0; c < 16U; ++c) { s.inverse_binds.push_back((c % 5U) == 0U ? 1.0F : 0.0F); }
        s.name_offsets.push_back(static_cast<u32>(s.name_pool.size()));
        for (const char* p = names[j]; *p != '\0'; ++p) { s.name_pool.push_back(*p); }
        s.name_pool.push_back('\0');
    }
    return s;
}

[[nodiscard]] bool mat_near_identity(const math::Mat4f& m, f32 tol)
{
    const auto* f = reinterpret_cast<const f32*>(&m);
    for (int i = 0; i < 16; ++i)
    {
        const f32 want = (i % 5) == 0 ? 1.0F : 0.0F;
        if (f[i] < want - tol || f[i] > want + tol) { return false; }
    }
    return true;
}

} // namespace

TEST_CASE("anim: SKEL builder/loader round-trip + the topological refusal", "[anim][geo8]")
{
    auto skel = make_chain();
    const auto bytes = anim::skeleton_build(skel, resources::ResourceId::mint_random(), &galloc());
    REQUIRE(bytes.size() > 0U);

    anim::SkeletonLoader        loader;
    crd::resources::LoadContext ctx;
    ctx.bytes     = containers::as_const_span(bytes);
    ctx.allocator = &galloc();
    auto* loaded  = static_cast<anim::SkeletonResource*>(loader.load(ctx));
    REQUIRE(loaded != nullptr);
    CHECK(loaded->joint_count() == 3U);
    CHECK(loaded->parents[2] == 1);
    CHECK(std::strcmp(loaded->joint_name(0), "root") == 0);
    CHECK(std::strcmp(loaded->joint_name(2), "tip") == 0);
    CHECK(loaded->rest[10] == 2.0F); // mid joint's rest t.x survived byte-exact
    loader.unload(loaded);

    // a parents[i] >= i violation REFUSES at build (the topological contract IS the format)
    anim::SkeletonResource bad = make_chain();
    bad.parents[0]             = 2;
    CHECK(anim::skeleton_build(bad, resources::ResourceId::mint_random(), &galloc()).size() == 0U);
}

TEST_CASE("anim: THE bind-pose identity gate + hierarchy composition oracle", "[anim][geo8]")
{
    auto skel = make_chain();
    // IBM = inverse(world at rest): world(root)=I, world(mid)=T(2,0,0), world(tip)=T(2,1,0)
    const math::Vec3f wt[3] = {{0, 0, 0}, {2, 0, 0}, {2, 1, 0}};
    for (u32 j = 0; j < 3U; ++j)
    {
        const math::Mat4f inv = math::inverse(
            math::from_trs(wt[j], math::Quatf::identity(), math::Vec3f{1, 1, 1}));
        std::memcpy(skel.inverse_binds.data() + static_cast<usize>(j) * 16U, &inv, 64U);
    }

    anim::JointPose poses[3];
    // rest pose (no clip): the palette must be IDENTITY — the classic skinning correctness gate
    for (u32 j = 0; j < 3U; ++j)
    {
        const f32* r          = skel.rest.data() + static_cast<usize>(j) * anim::kRestFloats;
        poses[j].translation  = {r[0], r[1], r[2]};
        poses[j].rotation     = {r[3], r[4], r[5], r[6]};
        poses[j].scale        = {r[7], r[8], r[9]};
    }
    math::Mat4f world[3];
    math::Mat4f palette[3];
    anim::compute_pose_matrices(skel, {poses, 3U}, {world, 3U});
    CHECK(world[2].c3.x == 2.0F); // tip world = root·mid·tip translations composed
    CHECK(world[2].c3.y == 1.0F);
    anim::compute_skin_palette(skel, {world, 3U}, {palette, 3U});
    for (u32 j = 0; j < 3U; ++j) { CHECK(mat_near_identity(palette[j], 1.0e-6F)); }
}

TEST_CASE("anim: clip sampling -- SLERP for linear rotation, exact keys, rest for untracked", "[anim][geo8]")
{
    auto skel = make_chain();

    anim::AnimClipResource clip(&galloc());
    clip.duration = 2.0F;
    // one rotation track on joint 1: identity → 90° about Z, linear
    anim::AnimTrack track;
    track.target     = 1U;
    track.channel    = 1U; // rotation
    track.interp     = 1U; // linear
    track.components = 4U;
    track.key_count  = 2U;
    track.times_off  = 0U;
    track.values_off = 2U;
    clip.tracks.push_back(track);
    const f32 s45 = 0.70710678F;
    const f32 data_vals[10] = {0.0F, 2.0F, /*q0*/ 0, 0, 0, 1, /*q1: 90° Z*/ 0, 0, s45, s45};
    for (f32 v : data_vals) { clip.data.push_back(v); }

    anim::JointPose poses[3];
    anim::sample_clip(clip, skel, 0.0F, {poses, 3U});
    CHECK(poses[1].rotation.w == 1.0F);        // exact key 0
    CHECK(poses[0].translation.x == 0.0F);     // untracked joints hold rest
    CHECK(poses[2].translation.y == 1.0F);

    anim::sample_clip(clip, skel, 1.0F, {poses, 3U}); // midpoint: slerp = 45° about Z
    const f32 s225 = 0.38268343F;                     // sin(22.5°)
    CHECK(poses[1].rotation.z > s225 - 1.0e-5F);
    CHECK(poses[1].rotation.z < s225 + 1.0e-5F);

    anim::sample_clip(clip, skel, 99.0F, {poses, 3U}); // clamp to the last key
    CHECK(poses[1].rotation.z > s45 - 1.0e-6F);
}

TEST_CASE("anim: dual-quat transform == matrix transform on a rigid palette (the B8-j CPU oracle)", "[anim][geo8]")
{
    const math::Quatf q = math::normalized(math::Quatf{0.2F, -0.4F, 0.1F, 0.88F});
    const math::Mat4f m = math::from_trs(math::Vec3f{1.5F, -2.0F, 0.75F}, q, math::Vec3f{1, 1, 1});

    anim::DualQuat dq;
    anim::palette_to_dual_quats({&m, 1U}, {&dq, 1U});

    const math::Vec3f p{0.3F, 2.2F, -1.1F};
    const math::Vec4f hp   = m * math::Vec4f{p.x, p.y, p.z, 1.0F};
    const math::Vec3f mres{hp.x, hp.y, hp.z};
    const math::Vec3f dres = anim::dual_quat_transform(dq, p);
    CHECK(dres.x > mres.x - 1.0e-5F);
    CHECK(dres.x < mres.x + 1.0e-5F);
    CHECK(dres.y > mres.y - 1.0e-5F);
    CHECK(dres.y < mres.y + 1.0e-5F);
    CHECK(dres.z > mres.z - 1.0e-5F);
    CHECK(dres.z < mres.z + 1.0e-5F);
}

TEST_CASE("anim: THE FOX GATE -- the real skinned character cooks, loads, and resamples BIT-STABLE at keys",
          "[anim][geo8][fox]")
{
    // locate Fox.glb (the ctest env points at assets/source; repo-root cwd falls back)
    containers::String fox_path(&galloc());
    if (const char* root = std::getenv("CRD_ASSETS_SOURCE"); root != nullptr && root[0] != '\0')
    {
        fox_path.append(root);
    }
    else { fox_path.append("assets/source"); }
    fox_path.append("/Fox.glb");

    containers::Array<u8> glb(&galloc());
    REQUIRE(platform::fs::read_file_binary(
        platform::fs::Path(containers::StringView(fox_path.data(), fox_path.size())), glb));

    // 1. the parser: skins + animations + skin attributes all land
    assetio::ImportedAsset asset(&galloc());
    REQUIRE(assetio::parse_glb(containers::as_const_span(glb), &galloc(), asset) == assetio::ImportStatus::Ok);
    REQUIRE(asset.skins.size() == 1U);
    REQUIRE(asset.animations.size() == 3U); // Survey · Walk · Run
    const u32 nj = static_cast<u32>(asset.skins[0].joints.size());
    CHECK(nj >= 20U);
    bool any_skinned_mesh = false;
    for (const auto& m : asset.meshes) { any_skinned_mesh = any_skinned_mesh || m.has_skin(); }
    CHECK(any_skinned_mesh);

    // keep the RAW reference channel data before the cook mutates/remaps anything
    struct RefKey
    {
        i32 node;
        u8  path;
        f32 t;
        f32 v[4];
        u32 comps;
    };
    containers::Array<RefKey> refs(&galloc());
    {
        const auto& walk = asset.animations[1];
        for (const auto& ch : walk.channels)
        {
            if (ch.interp == 2U) { continue; } // cubic keys checked via the engine gates; Fox is step/linear
            for (usize k = 0; k < ch.times.size(); ++k)
            {
                RefKey r{};
                r.node  = ch.node;
                r.path  = ch.path;
                r.t     = ch.times[k];
                r.comps = ch.components;
                for (u32 c = 0; c < ch.components && c < 4U; ++c) { r.v[c] = ch.values[k * ch.components + c]; }
                refs.push_back(r);
            }
        }
    }
    REQUIRE(refs.size() > 100U);

    // 2. the REAL cook (wave1) — SKEL + ANIM + SKNV artifacts
    crd::cooker::register_wave1_mesh_handler();
    const auto handler = crd::cooker::find_cook_handler(containers::StringView(".glb"));
    REQUIRE(handler != nullptr);
    crd::cooker::CookContext cctx;
    cctx.source_path = containers::StringView(fox_path.data(), fox_path.size());
    cctx.id          = resources::ResourceId::mint_random();
    cctx.allocator   = &galloc();
    crd::cooker::CookIO cio(cctx.source_path, cctx.meta_path, &galloc());
    cctx.io = &cio;
    const auto cooked = handler(cctx);
    REQUIRE(cooked.ok);

    const containers::Array<u8>* skel_bytes = nullptr;
    const containers::Array<u8>* walk_bytes = nullptr;
    for (const auto& extra : cooked.extra_artifacts)
    {
        if (extra.type_fourcc == anim::kFourCC_SKEL) { skel_bytes = &extra.cooked_bytes; }
        if (extra.type_fourcc == anim::kFourCC_ANIM
            && std::strstr(extra.name.c_str(), "Walk") != nullptr)
        {
            walk_bytes = &extra.cooked_bytes;
        }
    }
    REQUIRE(skel_bytes != nullptr);
    REQUIRE(walk_bytes != nullptr);

    // 3. load through the REAL loaders
    anim::SkeletonLoader        sl;
    crd::resources::LoadContext lctx;
    lctx.bytes     = containers::as_const_span(*skel_bytes);
    lctx.allocator = &galloc();
    auto* skel     = static_cast<anim::SkeletonResource*>(sl.load(lctx));
    REQUIRE(skel != nullptr);
    REQUIRE(skel->joint_count() == nj);
    for (u32 j = 0; j < nj; ++j) { CHECK(skel->parents[j] < static_cast<i32>(j)); } // topological, always

    anim::AnimClipLoader al;
    lctx.bytes  = containers::as_const_span(*walk_bytes);
    auto* clip  = static_cast<anim::AnimClipResource*>(al.load(lctx));
    REQUIRE(clip != nullptr);
    CHECK(clip->duration > 0.0F);

    // node → topological joint map, rebuilt the same way the cook built it (via the loaded skeleton's names)
    // — simpler: map by matching node names to joint names (Fox joint names are unique)
    const auto joint_of_node = [&](i32 node) -> i32 {
        const char* nname = asset.nodes[static_cast<usize>(node)].name.c_str();
        for (u32 j = 0; j < nj; ++j)
        {
            if (std::strcmp(skel->joint_name(j), nname) == 0) { return static_cast<i32>(j); }
        }
        return -1;
    };

    // 4. BIT-STABLE RESAMPLE: sampling the loaded clip AT EVERY KEY TIME reproduces the raw glTF values.
    // T/S: bit-exact. R: the sampler normalizes (the spec's unit-quat contract) — 1-ulp-scale tolerance.
    containers::Array<anim::JointPose> poses(&galloc());
    poses.resize(nj);
    u32 checked = 0;
    for (const auto& r : refs)
    {
        const i32 j = joint_of_node(r.node);
        if (j < 0) { continue; }
        anim::sample_clip(*clip, *skel, r.t, {poses.data(), poses.size()});
        const anim::JointPose& pose = poses[static_cast<u32>(j)];
        if (r.path == 0U) // translation — the cook applied position_scale 1.0: bit-exact
        {
            CHECK(pose.translation.x == r.v[0]);
            CHECK(pose.translation.y == r.v[1]);
            CHECK(pose.translation.z == r.v[2]);
        }
        else if (r.path == 1U)
        {
            const f32 tol = 2.0e-6F;
            // q and −q are the same rotation; compare up to sign
            const f32 sign = (pose.rotation.w * r.v[3] + pose.rotation.x * r.v[0] + pose.rotation.y * r.v[1]
                              + pose.rotation.z * r.v[2]) < 0.0F
                                 ? -1.0F
                                 : 1.0F;
            CHECK(pose.rotation.x * sign > r.v[0] - tol);
            CHECK(pose.rotation.x * sign < r.v[0] + tol);
            CHECK(pose.rotation.w * sign > r.v[3] - tol);
            CHECK(pose.rotation.w * sign < r.v[3] + tol);
        }
        else if (r.path == 2U)
        {
            CHECK(pose.scale.x == r.v[0]);
            CHECK(pose.scale.y == r.v[1]);
            CHECK(pose.scale.z == r.v[2]);
        }
        ++checked;
    }
    CHECK(checked > 100U); // the gate covered a real spread of keys

    al.unload(clip);
    sl.unload(skel);
}
