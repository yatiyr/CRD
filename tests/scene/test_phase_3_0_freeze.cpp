// Phase 3.0 v1p — Reserved-slot freeze + API surface pin (ADRs 0053 / 0056 /
// 0058 / 0059 / 0060).
//
// This file does two jobs at once:
//
//   (1) Round-trip every reserved trait + the ScriptComponent type through
//       `World::register_component(...)` so consumer code written for
//       Phase 4.0 / 4.2 / 7 compiles and registers cleanly today, even
//       though the runtime treats the trait as a no-op until its consumer
//       phase ships.
//
//   (2) Pin the public API surface that v1p formally freezes — every
//       size / alignment / version / FourCC the cooker writes and the
//       loader reads is asserted at compile time, so silent layout drift
//       (the kind that would invalidate every cooked artifact) trips a
//       hard build break instead of a load-time mystery.
//
// What's frozen here = what consumer phases CANNOT change. Anything not
// listed below (e.g. the runtime semantics of `History.at(frame)` —
// reserved but not yet pinned) is free to land in its consumer phase.

#include <crd/memory/allocator.hpp>
#include <crd/preset/camera_preset.hpp>
#include <crd/preset/preset_resource.hpp>
#include <crd/preset/preset_target.hpp>
#include <crd/preset/quality_preset.hpp>
#include <crd/profile/profile_predicate.hpp>
#include <crd/profile/profile_resource.hpp>
#include <crd/scene/async_aware_index.hpp>
#include <crd/scene/change_detect_index.hpp>
#include <crd/scene/component.hpp>
#include <crd/scene/obek.hpp>
#include <crd/scene/reserved_indexes.hpp>
#include <crd/scene/script_component.hpp>
#include <crd/scene/world.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

namespace
{

// Per-test component types. Each carries one reserved trait so the
// auto-register pathway picks up the matching no-op shell index.
struct CompHistorical { crd::f32 v = 0.0F; };
struct CompSpatial    { crd::f32 v = 0.0F; };
struct CompGpuBound   { crd::f32 v = 0.0F; };
struct CompReplicated { crd::f32 v = 0.0F; };
struct CompReflected  { crd::f32 v = 0.0F; };
struct CompAllReserved
{
    // One component carrying every reserved trait at once. Verifies the
    // variadic register_component<>() path walks the entire trait set
    // and the auto-register loop fires every shell index in one go.
    crd::math::Vec3f position{};
    crd::math::Vec3f velocity{};
};

inline const int g_reflection_sentinel = 0;

inline crd::scene::Reflection make_dummy_reflection() noexcept
{
    crd::scene::Reflection r{};
    r.display_name = crd::containers::StringView{"DummyForReservedTest"};
    // `fields` non-null is what triggers the auto-register; the pointer
    // value is opaque to v1p (Phase 7 walks it).
    r.fields = &g_reflection_sentinel;
    return r;
}

} // namespace

// =====================================================================
//   Section A — Reserved-trait registration round-trip (ADR-0056)
// =====================================================================

TEST_CASE("v1p: every reserved trait is accepted by register_component "
          "and stored on ComponentInfo",
          "[scene][v1p][freeze][traits]")
{
    crd::scene::World world{crd::memory::default_allocator()};

    // History{N>0} → history_window stored.
    const auto cid_h = world.register_component<CompHistorical>(
        crd::scene::StorageHint::Archetype, crd::scene::History{60});
    REQUIRE(!cid_h.is_null());
    CHECK(world.component_info(cid_h)->history_window == 60U);

    // SpatialBVH{} → spatial_bvh flag.
    const auto cid_s = world.register_component<CompSpatial>(
        crd::scene::StorageHint::Archetype, crd::scene::SpatialBVH{});
    CHECK(world.component_info(cid_s)->spatial_bvh);

    // GpuResident{} → gpu_resident flag.
    const auto cid_g = world.register_component<CompGpuBound>(
        crd::scene::StorageHint::Archetype, crd::scene::GpuResident{});
    CHECK(world.component_info(cid_g)->gpu_resident);

    // Replication::ServerAuthoritative → replication enum.
    const auto cid_r = world.register_component<CompReplicated>(
        crd::scene::StorageHint::Archetype,
        crd::scene::Replication::ServerAuthoritative);
    CHECK(world.component_info(cid_r)->replication
          == crd::scene::Replication::ServerAuthoritative);

    // Reflection{...} → reflection record stored.
    const auto cid_x = world.register_component<CompReflected>(
        crd::scene::StorageHint::Archetype, make_dummy_reflection());
    CHECK(world.component_info(cid_x)->reflection.fields != nullptr);
}

TEST_CASE("v1p: registering all reserved traits at once on a single "
          "component fires every auto-register shell index",
          "[scene][v1p][freeze][auto-register]")
{
    crd::scene::World world{crd::memory::default_allocator()};

    const auto cid = world.register_component<CompAllReserved>(
        crd::scene::StorageHint::Archetype,
        crd::scene::AsyncAware{},
        crd::scene::History{8},
        crd::scene::SpatialBVH{},
        crd::scene::GpuResident{},
        crd::scene::Replication::ClientPredicted,
        make_dummy_reflection());
    REQUIRE(!cid.is_null());

    // ChangeDetect always auto-registers (every component is observable).
    REQUIRE(world.find_index<crd::scene::ChangeDetectIndex>() != nullptr);

    // The other five fire only because their trait was set.
    REQUIRE(world.find_index<crd::scene::AsyncAwareIndex>()   != nullptr);
    REQUIRE(world.find_index<crd::scene::HistoryIndex>()      != nullptr);
    REQUIRE(world.find_index<crd::scene::SpatialBVHIndex>()   != nullptr);
    REQUIRE(world.find_index<crd::scene::GpuResidentIndex>()  != nullptr);
    REQUIRE(world.find_index<crd::scene::ReplicationIndex>()  != nullptr);
    REQUIRE(world.find_index<crd::scene::ReflectionIndex>()   != nullptr);
}

// =====================================================================
//   Section B — ScriptComponent type freeze (ADR-0056 §2)
// =====================================================================

TEST_CASE("v1p: ScriptComponent is a regular registrable component, not a "
          "trait - Phase 4.0 lights it up via ScriptSystem",
          "[scene][v1p][freeze][script]")
{
    crd::scene::World world{crd::memory::default_allocator()};

    // ADR-0056 §2: ScriptComponent registers as a normal component with
    // SparseSet hint (sparse across the world; few entities script).
    const auto cid = world.register_component<crd::scene::ScriptComponent>(
        crd::scene::StorageHint::SparseSet);
    REQUIRE(!cid.is_null());
    CHECK(world.component_info(cid)->storage_hint
          == crd::scene::StorageHint::SparseSet);
    CHECK(world.component_info(cid)->size      == sizeof(crd::scene::ScriptComponent));
    CHECK(world.component_info(cid)->alignment == alignof(crd::scene::ScriptComponent));

    // Round-trip an inert script attachment (handle = 0 = "no script").
    const auto e = world.spawn();
    world.add_component<crd::scene::ScriptComponent>(
        e, crd::scene::ScriptComponent{crd::scene::ScriptHandle{}, 0U, 0U});

    const auto* sc = world.get_component<crd::scene::ScriptComponent>(e);
    REQUIRE(sc != nullptr);
    CHECK(sc->script.is_null());
    CHECK(sc->state_size == 0U);
}

// =====================================================================
//   Section C — Reserved spatial DSL operator passthrough (ADR-0053 §6)
// =====================================================================

TEST_CASE("v1p: .in_aabb / .within_radius compile, chain, and pass through "
          "every entity in Phase 3.0 (no-op SpatialBVHIndex)",
          "[scene][v1p][freeze][query][spatial]")
{
    crd::scene::World world{crd::memory::default_allocator()};
    world.register_component<CompSpatial>(
        crd::scene::StorageHint::SparseSet, crd::scene::SpatialBVH{});

    const auto e1 = world.spawn();
    const auto e2 = world.spawn();
    const auto e3 = world.spawn();
    world.add_component<CompSpatial>(e1, CompSpatial{1.0F});
    world.add_component<CompSpatial>(e2, CompSpatial{2.0F});
    world.add_component<CompSpatial>(e3, CompSpatial{3.0F});

    // .in_aabb passthrough: every entity matches.
    {
        const crd::math::AABB<crd::f32> box{
            crd::math::Vec3f{-1, -1, -1}, crd::math::Vec3f{1, 1, 1}};
        crd::u32 count = 0;
        for (auto&& [e, c] : world.query<CompSpatial>().in_aabb(box))
        {
            (void)e; (void)c; ++count;
        }
        CHECK(count == 3U);
    }

    // .within_radius passthrough: every entity matches.
    {
        crd::u32 count = 0;
        for (auto&& [e, c] : world.query<CompSpatial>()
                                  .within_radius(crd::math::Vec3f{0, 0, 0}, 100.0F))
        {
            (void)e; (void)c; ++count;
        }
        CHECK(count == 3U);
    }

    // Both chained — composition is part of the API freeze.
    {
        const crd::math::AABB<crd::f32> box{
            crd::math::Vec3f{-100, -100, -100}, crd::math::Vec3f{100, 100, 100}};
        crd::u32 count = 0;
        for (auto&& [e, c] : world.query<CompSpatial>()
                                  .in_aabb(box)
                                  .within_radius(crd::math::Vec3f{0, 0, 0}, 50.0F))
        {
            (void)e; (void)c; ++count;
        }
        CHECK(count == 3U);
    }
}

// =====================================================================
//   Section D — API surface freeze (sizes, versions, FourCCs)
// =====================================================================
//
// Compile-time pins on every public layout v1p commits to keeping
// stable. Editing any of these is a deliberate schema break — the
// loader's payload-size check turns a mismatch into LoadState::Failed,
// every cooked artifact in the field becomes invalid, and every
// downstream consumer needs an explicit migration. v1p says: don't.
//
// New fields land via version bumps (the QualityPreset v1→v2 in v1o3
// is the canonical example — one byte of `_reserved` repurposed,
// total size unchanged, version constant moved). The cooker's
// payload-size check then keeps old artifacts loadable until they're
// re-cooked.

// ---- Öbek (ADR-0058) ----
static_assert(sizeof(crd::scene::ObekInfo)                == 24U,
              "ObekInfo size pinned at 24 bytes for OBEK schema v1");
static_assert(sizeof(crd::scene::ObekComponentDescriptor) == 32U,
              "ObekComponentDescriptor size pinned at 32 bytes");
static_assert(sizeof(crd::scene::ObekEntityRecord)        == 16U,
              "ObekEntityRecord size pinned at 16 bytes");
static_assert(sizeof(crd::scene::ObekRelationRecord)      == 16U,
              "ObekRelationRecord size pinned at 16 bytes");
static_assert(sizeof(crd::scene::ObekChainEntryRecord)    == 24U,
              "ObekChainEntryRecord size pinned at 24 bytes");
static_assert(crd::scene::kObekSchemaVersion == 1U,
              "Öbek schema version pinned at 1 — bump on any record-layout change");

// ---- Preset (ADR-0059) ----
static_assert(sizeof(crd::preset::QualityPreset)  == 144U,
              "QualityPreset size pinned at 144 bytes (v2 layout — repurposed _reserved byte)");
static_assert(alignof(crd::preset::QualityPreset) == 8U);
static_assert(crd::preset::QualityPreset::version == 2U,
              "QualityPreset version pinned at 2 (v1o3); bump on field add");

static_assert(sizeof(crd::preset::CameraPreset)   == 40U,
              "CameraPreset size pinned at 40 bytes for v1");
static_assert(alignof(crd::preset::CameraPreset)  == 4U);
static_assert(crd::preset::CameraPreset::version  == 1U);

// ---- Profile (ADR-0060) ----
static_assert(sizeof(crd::profile::ProfileFileInfo) == 16U,
              "ProfileFileInfo size pinned at 16 bytes for PROF v1");
static_assert(sizeof(crd::profile::PredicateRecord) == 8U,
              "PredicateRecord size pinned at 8 bytes — predicate schema is closed");
static_assert(alignof(crd::profile::PredicateRecord) == 4U);

// ---- Closed enum value-pinning (ADR-0058 / 0060) ----
static_assert(static_cast<crd::u8>(crd::scene::InheritPolicy::Override)    == 0U);
static_assert(static_cast<crd::u8>(crd::scene::InheritPolicy::Inherit)     == 1U);
static_assert(static_cast<crd::u8>(crd::scene::InheritPolicy::DontInherit) == 2U);

static_assert(static_cast<crd::u8>(crd::scene::Replication::Local)               == 0U);
static_assert(static_cast<crd::u8>(crd::scene::Replication::ServerAuthoritative) == 1U);
static_assert(static_cast<crd::u8>(crd::scene::Replication::ClientPredicted)     == 2U);
static_assert(static_cast<crd::u8>(crd::scene::Replication::Remote)              == 3U);

static_assert(static_cast<crd::u8>(crd::profile::PredicateField::Os)        == 0U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateField::GpuTier)   == 1U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateField::Domain)    == 2U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateField::Mode)      == 3U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateField::TargetFps) == 4U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateField::CpuCores)  == 5U);

static_assert(static_cast<crd::u8>(crd::profile::PredicateOp::Equal)     == 0U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateOp::GreaterEq) == 1U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateOp::LessEq)    == 2U);
static_assert(static_cast<crd::u8>(crd::profile::PredicateOp::InMask)    == 3U);

// ---- ScriptComponent (ADR-0056 §2) ----
static_assert(sizeof(crd::scene::ScriptHandle)    == 8U);
static_assert(sizeof(crd::scene::ScriptComponent) == 16U);

TEST_CASE("v1p: API surface freeze static-asserts all hold (compile-time)",
          "[scene][v1p][freeze][api]")
{
    // The asserts above run at translation-unit load. If any tripped,
    // the file would not have compiled. This case exists so the freeze
    // is visible in `ctest --list-tests` and a green run logs the
    // intent of the freeze on every CI sweep.
    SUCCEED("Phase 3.0 v1p API surface frozen — every cooked artifact "
            "schema is layout-stable until a versioned migration");
}
