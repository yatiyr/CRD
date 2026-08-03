// RAF-4 Gate 4 — the shader + program contract (device-free).
//
// Gates (mission §9 · D-007 RAF-4): valid/invalid stage composition; duplicate-binding (conflict) rejection;
// frequency mapping; stable (deterministic) layout; stage-I/O compat; attachment-output compat; bounded variant
// enumeration; interface-change invalidation.
//
// ⛔ named allocator throughout; ASCII-only test names.

#include <crd/renderprogram/program_contract.hpp>

#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <catch2/catch_test_macros.hpp>

using crd::u32;
using crd::u64;
using crd::u8;
using crd::containers::Array;
using namespace crd::renderprogram;

namespace
{
StageIoVar io(u32 loc, u8 comps) { return StageIoVar{loc, IoScalar::F32, comps}; }
ResourceDecl res(u64 name, BindingKind kind, BindingFrequency freq) { return ResourceDecl{name, kind, freq, 1U}; }

ShaderModuleContract make_vs()
{
    ShaderModuleContract m;
    m.stage = ProgramStage::Vertex;
    m.outputs.push_back(io(0U, u8{4})); // one interpolant at location 0
    m.resources.push_back(res(1001U, BindingKind::StorageBuffer, BindingFrequency::Object));
    return m;
}
ShaderModuleContract make_fs()
{
    ShaderModuleContract m;
    m.stage = ProgramStage::Fragment;
    m.inputs.push_back(io(0U, u8{4})); // matches the VS output
    m.outputs.push_back(io(0U, u8{4})); // one colour attachment at location 0
    m.resources.push_back(res(2002U, BindingKind::SampledTexture, BindingFrequency::Material));
    return m;
}
} // namespace

TEST_CASE("raf4 valid vertex-fragment program composes, validates, and resolves a layout")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-valid");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    REQUIRE(prog.add_module(make_vs(), d));
    REQUIRE(prog.add_module(make_fs(), d));
    REQUIRE(prog.validate(d));
    REQUIRE_FALSE(d.has_errors());

    Array<ResolvedBinding> layout(&alloc);
    REQUIRE(prog.resolve_layout(layout, d));
    REQUIRE(layout.size() == 2U);
    // Frequency mapping: Material (2) sorts before Object (3); each gets slot 0 within its group.
    REQUIRE(layout[0].frequency == BindingFrequency::Material);
    REQUIRE(layout[0].slot == 0U);
    REQUIRE(layout[0].name_hash == 2002U);
    REQUIRE(layout[1].frequency == BindingFrequency::Object);
    REQUIRE(layout[1].slot == 0U);
    REQUIRE(layout[1].name_hash == 1001U);

    REQUIRE(prog.interface_hash().value != 0U);
}

TEST_CASE("raf4 duplicate stage is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-dupstage");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    REQUIRE(prog.add_module(make_vs(), d));
    REQUIRE_FALSE(prog.add_module(make_vs(), d)); // second vertex stage
    REQUIRE(d.contains(DiagCode::DuplicateStage));
}

TEST_CASE("raf4 illegal stage compositions are rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-illegal");

    SECTION("vertex and mesh are mutually exclusive")
    {
        DiagnosticList d(&alloc);
        ProgramContract prog(&alloc);
        ShaderModuleContract mesh;
        mesh.stage = ProgramStage::Mesh;
        REQUIRE(prog.add_module(make_vs(), d));
        REQUIRE(prog.add_module(mesh, d));
        REQUIRE_FALSE(prog.validate(d));
        REQUIRE(d.contains(DiagCode::IllegalStageComposition));
    }
    SECTION("compute must stand alone")
    {
        DiagnosticList d(&alloc);
        ProgramContract prog(&alloc);
        ShaderModuleContract comp;
        comp.stage = ProgramStage::Compute;
        REQUIRE(prog.add_module(comp, d));
        REQUIRE(prog.add_module(make_fs(), d));
        REQUIRE_FALSE(prog.validate(d));
        REQUIRE(d.contains(DiagCode::IllegalStageComposition));
    }
    SECTION("tessellation control needs eval")
    {
        DiagnosticList d(&alloc);
        ProgramContract prog(&alloc);
        ShaderModuleContract tc;
        tc.stage = ProgramStage::TessControl;
        REQUIRE(prog.add_module(make_vs(), d));
        REQUIRE(prog.add_module(tc, d));
        REQUIRE(prog.add_module(make_fs(), d));
        REQUIRE_FALSE(prog.validate(d));
        REQUIRE(d.contains(DiagCode::IllegalStageComposition));
    }
    SECTION("fragment alone has no producer")
    {
        DiagnosticList d(&alloc);
        ProgramContract prog(&alloc);
        REQUIRE(prog.add_module(make_fs(), d));
        REQUIRE_FALSE(prog.validate(d));
        REQUIRE(d.contains(DiagCode::IllegalStageComposition));
    }
    SECTION("raygen-less ray program is illegal")
    {
        DiagnosticList d(&alloc);
        ProgramContract prog(&alloc);
        ShaderModuleContract chit;
        chit.stage = ProgramStage::ClosestHit;
        REQUIRE(prog.add_module(chit, d));
        REQUIRE_FALSE(prog.validate(d));
        REQUIRE(d.contains(DiagCode::IllegalStageComposition));
    }
}

TEST_CASE("raf4 stage-io mismatch is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-io");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    REQUIRE(prog.add_module(make_vs(), d)); // outputs location 0
    ShaderModuleContract fs;
    fs.stage = ProgramStage::Fragment;
    fs.inputs.push_back(io(1U, u8{4})); // location 1 — no matching VS output
    fs.outputs.push_back(io(0U, u8{4}));
    REQUIRE(prog.add_module(fs, d));
    REQUIRE_FALSE(prog.validate(d));
    REQUIRE(d.contains(DiagCode::StageIoMismatch));
}

TEST_CASE("raf4 binding-name conflict is rejected")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-conflict");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    ShaderModuleContract vs = make_vs(); // declares name 1001 as StorageBuffer/Object
    ShaderModuleContract fs = make_fs();
    fs.resources.push_back(res(1001U, BindingKind::SampledTexture, BindingFrequency::Material)); // same name, diff kind
    REQUIRE(prog.add_module(vs, d));
    REQUIRE(prog.add_module(fs, d));
    Array<ResolvedBinding> layout(&alloc);
    REQUIRE_FALSE(prog.resolve_layout(layout, d));
    REQUIRE(d.contains(DiagCode::BindingConflict));
}

TEST_CASE("raf4 layout is deterministic across resolutions")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-stable");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    REQUIRE(prog.add_module(make_vs(), d));
    REQUIRE(prog.add_module(make_fs(), d));

    Array<ResolvedBinding> a(&alloc);
    Array<ResolvedBinding> b(&alloc);
    REQUIRE(prog.resolve_layout(a, d));
    REQUIRE(prog.resolve_layout(b, d));
    REQUIRE(a.size() == b.size());
    for (u32 i = 0; i < a.size(); ++i)
    {
        REQUIRE(a[i] == b[i]);
    }
}

TEST_CASE("raf4 attachment-output compatibility")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-attach");
    DiagnosticList d(&alloc);
    ProgramContract prog(&alloc);
    REQUIRE(prog.add_module(make_vs(), d));
    REQUIRE(prog.add_module(make_fs(), d)); // one output: location 0, 4 components

    const StageIoVar rt_ok[1] = {io(0U, u8{4})};
    REQUIRE(prog.validate_attachment_compat(rt_ok, 1U, d));

    const StageIoVar rt_bad_components[1] = {io(0U, u8{2})};
    DiagnosticList d2(&alloc);
    REQUIRE_FALSE(prog.validate_attachment_compat(rt_bad_components, 1U, d2));
    REQUIRE(d2.contains(DiagCode::AttachmentMismatch));

    const StageIoVar rt_two[2] = {io(0U, u8{4}), io(1U, u8{4})};
    DiagnosticList d3(&alloc);
    REQUIRE_FALSE(prog.validate_attachment_compat(rt_two, 2U, d3)); // count mismatch
    REQUIRE(d3.contains(DiagCode::AttachmentMismatch));
}

TEST_CASE("raf4 interface hash invalidates on any interface change")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-iface");
    DiagnosticList d(&alloc);

    ProgramContract base(&alloc);
    REQUIRE(base.add_module(make_vs(), d));
    REQUIRE(base.add_module(make_fs(), d));

    ProgramContract same(&alloc);
    REQUIRE(same.add_module(make_vs(), d));
    REQUIRE(same.add_module(make_fs(), d));
    REQUIRE(base.interface_hash() == same.interface_hash()); // identical interfaces ⇒ identical hash

    ProgramContract changed(&alloc);
    ShaderModuleContract vs2 = make_vs();
    vs2.resources.push_back(res(3003U, BindingKind::UniformBuffer, BindingFrequency::Frame)); // a new resource
    REQUIRE(changed.add_module(vs2, d));
    REQUIRE(changed.add_module(make_fs(), d));
    REQUIRE(base.interface_hash() != changed.interface_hash()); // a changed interface ⇒ a changed hash
}

TEST_CASE("raf4 variant space is bounded and keys are deterministic")
{
    crd::memory::TlsfAllocator alloc(1U << 20U, nullptr, "raf4-variant");

    // A bounded variant space: 3 x 2 x 4 = 24 variants.
    VariantAxis axes[3] = {VariantAxis(&alloc), VariantAxis(&alloc), VariantAxis(&alloc)};
    axes[0].options.push_back(0U);
    axes[0].options.push_back(1U);
    axes[0].options.push_back(2U);
    axes[1].options.push_back(0U);
    axes[1].options.push_back(1U);
    axes[2].options.push_back(10U);
    axes[2].options.push_back(20U);
    axes[2].options.push_back(30U);
    axes[2].options.push_back(40U);
    REQUIRE(variant_space_size(axes, 3U) == 24U);

    // Deterministic + discriminating variant keys.
    const VariantKey k1{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    const VariantKey k1b{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 9U};
    const VariantKey k2{1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 10U}; // one axis differs
    REQUIRE(k1.hash() == k1b.hash());
    REQUIRE(k1.hash() != k2.hash());
    REQUIRE(k1 == k1b);
    REQUIRE_FALSE(k1 == k2);
}
