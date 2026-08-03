#pragma once

// crd-render-program — the shader + program CONTRACT (RAF-4, mission §4.1/4.2/§9).
//
// The single, IR-agnostic description of what a GPU program IS: its stages, each stage's typed I/O, its declared
// resources (with binding KIND + FREQUENCY), the resolved deterministic binding LAYOUT, the interface SIGNATURE hash
// (interface-change invalidation), and the VARIANT KEY axes. High-level rendering stops hard-coding "binding 1 = base
// colour"; it reads the resolved layout instead. A cooker maps a CKIR `KEntry` → `ShaderModuleContract`; this module
// stays free of the IR (and of any backend), so the contract is validated and gated with no device.
//
// Built on render-asset-core: shared `BindingFrequency`/`BindingKind`, `InterfaceHash`, structured `Diagnostic`.

#include <crd/containers/array.hpp>
#include <crd/containers/fixed_array.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/binding.hpp>
#include <crd/renderasset/cooked.hpp>
#include <crd/renderasset/diagnostic.hpp>

#include <compare>

namespace crd::renderprogram
{
using crd::containers::Array;
using crd::containers::FixedArray;
using crd::containers::StringView;
using crd::renderasset::BindingFrequency;
using crd::renderasset::BindingKind;
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using crd::renderasset::InterfaceHash;
using crd::renderasset::Severity;

inline constexpr u32 kMaxIoVars = 16;         // interpolants / attachments per stage
inline constexpr u32 kMaxResourceDecls = 32;  // declared resources per stage

// A program's shader stages (IR-agnostic — the contract mirror of KStage).
enum class ProgramStage : u8
{
    Vertex = 0,
    TessControl,
    TessEval,
    Geometry,
    Fragment,
    Task,
    Mesh,
    Compute,
    RayGen,
    Miss,
    ClosestHit,
    AnyHit,
    Intersection,
    Callable,
};
[[nodiscard]] StringView program_stage_name(ProgramStage stage) noexcept;

enum class IoScalar : u8
{
    F32 = 0,
    I32,
    U32,
};

// One stage input or output: a location + a typed vector (scalar × components).
struct StageIoVar
{
    u32 location = 0;
    IoScalar scalar = IoScalar::F32;
    u8 components = 4; // 1..4
    friend constexpr bool operator==(const StageIoVar&, const StageIoVar&) noexcept = default;
};

// A resource a stage declares: a stable name id + kind + frequency + array count.
struct ResourceDecl
{
    u64 name_hash = 0; // stable id of the authored binding name
    BindingKind kind = BindingKind::StorageBuffer;
    BindingFrequency frequency = BindingFrequency::Draw;
    u32 array_count = 1;
    friend constexpr bool operator==(const ResourceDecl&, const ResourceDecl&) noexcept = default;
};

// One shader module's contract: its stage + typed I/O + declared resources.
struct ShaderModuleContract
{
    ProgramStage stage = ProgramStage::Vertex;
    FixedArray<StageIoVar, kMaxIoVars> inputs;
    FixedArray<StageIoVar, kMaxIoVars> outputs;
    FixedArray<ResourceDecl, kMaxResourceDecls> resources;
};

// A resource in the RESOLVED cooked layout: its frequency group + compact slot within that group. Deterministic —
// this is what a renderer binds against instead of a hard-coded register.
struct ResolvedBinding
{
    BindingFrequency frequency = BindingFrequency::Draw;
    u32 slot = 0; // compact slot WITHIN the frequency group
    BindingKind kind = BindingKind::StorageBuffer;
    u64 name_hash = 0;
    u32 array_count = 1;
    u32 stage_mask = 0; // bit i set ⇒ ProgramStage(i) references this binding
    friend constexpr bool operator==(const ResolvedBinding&, const ResolvedBinding&) noexcept = default;
};

// The variant-key axes (mission §9). A deterministic key identifying one program variant; `hash()` keys the variant
// cache. Interface changes invalidate through the program's InterfaceHash, orthogonally.
struct VariantKey
{
    u64 technique = 0;            // technique id (+generation folded in)
    u64 material_definition = 0;  // material definition id (+generation)
    u64 material_feature = 0;     // material feature bitset
    u32 render_phase = 0;         // render-phase id
    u32 vertex_variant = 0;       // vertex/geometry variant
    u32 skinning_variant = 0;     // rigid / skinned / morph
    u32 attachment_signature = 0; // output/RT signature
    u32 capability_tier = 0;      // device capability tier
    u64 technique_options = 0;    // technique option bitset

    friend constexpr bool operator==(const VariantKey&, const VariantKey&) noexcept = default;
    [[nodiscard]] u64 hash() const noexcept;
};

// One axis of the variant space: a bounded list of option values. Used to enumerate the (bounded) variant set.
struct VariantAxis
{
    Array<u64> options; // the finite set of values this axis can take
    explicit VariantAxis(memory::IAllocator* alloc) : options(alloc) {}
};

// The composed program contract: stages + validation + resolved layout + interface signature.
class ProgramContract
{
public:
    explicit ProgramContract(memory::IAllocator* alloc) noexcept : m_modules(alloc) {}

    // Add a stage module. Rejects a second module of the same stage (DuplicateStage).
    bool add_module(const ShaderModuleContract& module, DiagnosticList& diags);

    // Validate the composition: a legal stage set (IllegalStageComposition) + downstream inputs matched by upstream
    // outputs (StageIoMismatch).
    [[nodiscard]] bool validate(DiagnosticList& diags) const;

    // Resolve the deterministic cooked binding layout: unique resources grouped by frequency, compact slots by
    // ascending name_hash within each group. Conflicting decls (one name, different kind/frequency) ⇒ BindingConflict.
    [[nodiscard]] bool resolve_layout(Array<ResolvedBinding>& out, DiagnosticList& diags) const;

    // Fragment outputs must match a render-target signature (location + components), attachment-for-attachment.
    [[nodiscard]] bool validate_attachment_compat(const StageIoVar* rt_outputs, u32 rt_count,
                                                  DiagnosticList& diags) const;

    // The interface signature hash over stages + sorted I/O + sorted resources (interface-change invalidation).
    [[nodiscard]] InterfaceHash interface_hash() const noexcept;

    [[nodiscard]] u32 module_count() const noexcept { return static_cast<u32>(m_modules.size()); }
    [[nodiscard]] const ShaderModuleContract* find_stage(ProgramStage stage) const noexcept;

private:
    Array<ShaderModuleContract> m_modules;
};

// Enumerate the BOUNDED variant set from per-axis option lists (a cartesian product). `axes` maps a subset of the
// VariantKey fields; `emit` receives each key. Returns the total count — provably finite (∏ axis sizes).
[[nodiscard]] u64 variant_space_size(const VariantAxis* axes, u32 axis_count) noexcept;
} // namespace crd::renderprogram
