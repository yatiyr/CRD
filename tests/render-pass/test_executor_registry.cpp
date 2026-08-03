// RAF-6 Gate 6 — the pass-executor registry (device-free).
//
// Gates (mission §8 · D-007 RAF-6): built-in registration; duplicate-id rejection; schema-version mismatch; param /
// resource-slot / queue validation; APP-defined executor registration; a custom executor usable from an app payload
// WITHOUT editing an engine enum; no authoring void* (the payload is typed); NO runtime string lookup at record
// (executors are referenced by a resolved ExecutorTypeId, not a string).
//
// ⛔ named allocator throughout; ASCII-only test names.

#include <crd/renderpass/executor_registry.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::f32;
using crd::u32;
using crd::u64;
using namespace crd::renderpass;

namespace
{
TypedValue tv_f32(f32 v)
{
    TypedValue t;
    t.type = ExecutorParamType::F32;
    t.f = v;
    return t;
}
TypedValue tv_vec4()
{
    TypedValue t;
    t.type = ExecutorParamType::Vec4;
    t.v4[0] = 0.0F;
    t.v4[1] = 0.0F;
    t.v4[2] = 0.0F;
    t.v4[3] = 1.0F;
    return t;
}
TypedValue tv_enum(u32 e)
{
    TypedValue t;
    t.type = ExecutorParamType::Enum;
    t.e = e;
    return t;
}

// A fully-valid scene.raster payload — referenced by RESOLVED id (no string at record).
PassPayload valid_scene_payload()
{
    PassPayload p;
    p.executor = executor_type_id("scene.raster");
    p.schema_version = 1U;
    p.queue = QueueKind::Graphics;
    p.params.push_back(ParamValue{pass_param_id("clear_color"), tv_vec4()});
    p.params.push_back(ParamValue{pass_param_id("clear_depth"), tv_f32(1.0F)});
    p.params.push_back(ParamValue{pass_param_id("depth_compare"), tv_enum(0U)});
    p.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, 1U});
    p.resources.push_back(ResourceRef{pass_param_id("geometry"), SlotResourceKind::StorageBuffer, SlotAccess::Read, 2U});
    return p;
}
} // namespace

TEST_CASE("raf6 built-in executors register and look up by id")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf6-builtins");
    DiagnosticList d(&alloc);
    ExecutorRegistry reg(&alloc);
    REQUIRE(register_builtin_executors(reg, d) == 14U);
    REQUIRE(reg.size() == 14U);
    REQUIRE_FALSE(d.has_errors());

    // Lookup is by resolved id (no string at record).
    REQUIRE(reg.find(executor_type_id("scene.raster")) != nullptr);
    REQUIRE(reg.find(executor_type_id("transfer.resolve")) != nullptr);
    REQUIRE(reg.find(executor_type_id("present")) != nullptr);
    REQUIRE(reg.find(executor_type_id("does.not.exist")) == nullptr);
}

TEST_CASE("raf6 duplicate executor id is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf6-dup");
    DiagnosticList d(&alloc);
    ExecutorRegistry reg(&alloc);
    REQUIRE(register_builtin_executors(reg, d) == 14U);

    // Re-registering the built-ins now conflicts on every id.
    DiagnosticList d2(&alloc);
    REQUIRE(register_builtin_executors(reg, d2) == 0U);
    REQUIRE(d2.contains(DiagCode::DuplicateExecutor));
    REQUIRE(reg.size() == 14U);
}

TEST_CASE("raf6 an app registers a custom executor without an engine-enum edit")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf6-app");
    DiagnosticList d(&alloc);
    ExecutorRegistry reg(&alloc);
    REQUIRE(register_builtin_executors(reg, d) == 14U);

    // The APP defines its own executor purely by registration — no engine code is touched.
    PassExecutorDesc app;
    app.id = executor_type_id("app.myeffect");
    app.name = "app.myeffect";
    app.schema.version = 1U;
    app.schema.queue = QueueKind::Graphics;
    app.schema.params.push_back(ParamSpec{pass_param_id("strength"), ExecutorParamType::F32, true});
    app.schema.slots.push_back(ResourceSlotSpec{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, true});
    REQUIRE(reg.register_executor(app, d));
    REQUIRE(reg.size() == 15U);

    // An app PAYLOAD referencing it validates through the SAME registry as engine passes.
    PassPayload p;
    p.executor = executor_type_id("app.myeffect");
    p.schema_version = 1U;
    p.queue = QueueKind::Graphics;
    p.params.push_back(ParamValue{pass_param_id("strength"), tv_f32(0.5F)});
    p.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, 7U});
    REQUIRE(validate_payload(reg, p, d));
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("raf6 a valid built-in payload passes validation")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf6-valid");
    DiagnosticList d(&alloc);
    ExecutorRegistry reg(&alloc);
    REQUIRE(register_builtin_executors(reg, d) == 14U);
    REQUIRE(validate_payload(reg, valid_scene_payload(), d));
    REQUIRE_FALSE(d.has_errors());
}

TEST_CASE("raf6 payload validation rejects every malformed shape")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf6-reject");
    DiagnosticList setup(&alloc);
    ExecutorRegistry reg(&alloc);
    REQUIRE(register_builtin_executors(reg, setup) == 14U);

    SECTION("unknown executor")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.executor = executor_type_id("nope");
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::UnknownExecutor));
    }
    SECTION("schema-version mismatch")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.schema_version = 99U;
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::SchemaMismatch));
    }
    SECTION("queue mismatch")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.queue = QueueKind::Compute; // scene.raster is Graphics
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::QueueMismatch));
    }
    SECTION("missing required param")
    {
        DiagnosticList d(&alloc);
        PassPayload p;
        p.executor = executor_type_id("scene.raster");
        p.schema_version = 1U;
        p.queue = QueueKind::Graphics;
        // omit clear_color (required)
        p.params.push_back(ParamValue{pass_param_id("clear_depth"), tv_f32(1.0F)});
        p.params.push_back(ParamValue{pass_param_id("depth_compare"), tv_enum(0U)});
        p.resources.push_back(ResourceRef{pass_param_id("color"), SlotResourceKind::ColorTarget, SlotAccess::Write, 1U});
        p.resources.push_back(ResourceRef{pass_param_id("geometry"), SlotResourceKind::StorageBuffer, SlotAccess::Read, 2U});
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidParam));
    }
    SECTION("param type mismatch")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.params[1].value = tv_enum(0U); // clear_depth should be F32, not Enum
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidParam));
    }
    SECTION("unknown param")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.params.push_back(ParamValue{pass_param_id("not_a_param"), tv_f32(0.0F)});
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidParam));
    }
    SECTION("missing required slot")
    {
        // RAF-8: scene.raster's slots are all OPTIONAL now (color/depth/geometry/inputs — a depth-only or draw-list
        // pass binds a subset), so a REQUIRED-slot omission is tested on `present`, whose `source` IS required.
        DiagnosticList d(&alloc);
        PassPayload p;
        p.executor = executor_type_id("present");
        p.schema_version = 1U;
        p.queue = QueueKind::Graphics;
        // `source` (required) omitted entirely.
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidSlot));
    }
    SECTION("slot kind/access mismatch")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.resources[0].access = SlotAccess::Read; // color is a Write slot
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidSlot));
    }
    SECTION("unbound slot")
    {
        DiagnosticList d(&alloc);
        PassPayload p = valid_scene_payload();
        p.resources[0].resource_id = 0U; // bound to nothing
        REQUIRE_FALSE(validate_payload(reg, p, d));
        REQUIRE(d.contains(DiagCode::InvalidSlot));
    }
}
