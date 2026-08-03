#pragma once

// crd-render-pass — the PASS-EXECUTOR REGISTRY (RAF-6, mission §8).
//
// Retires the central `FramePassKind` enum + `frame_runtime` switch. A pass names a REGISTERED executor by a stable
// `ExecutorTypeId` (a name hash, resolved ONCE at cook time — never a string at record). Each executor declares a
// typed SCHEMA (versioned): its typed PARAMETERS, its RESOURCE SLOTS (reads/writes with kind + access), and its QUEUE.
// A pass carries a typed PAYLOAD — NO `void*`, no runtime strings — validated against the schema before execution.
// Engine built-ins register the 8 executors; an APP registers its own without editing any engine enum.
//
// Backend-/IR-agnostic; validated and gated with no device. Built on render-asset-core (shared BindingKind + Diagnostic).

#include <crd/containers/array.hpp>
#include <crd/containers/fixed_array.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/renderasset/diagnostic.hpp>

#include <compare>

namespace crd::renderpass
{
using crd::containers::Array;
using crd::containers::FixedArray;
using crd::containers::StringView;
using crd::renderasset::DiagCode;
using crd::renderasset::DiagnosticList;
using crd::renderasset::Severity;

inline constexpr u32 kMaxExecutorParams = 12;
inline constexpr u32 kMaxExecutorSlots = 8;

// A stable executor id — a hash of the executor name ("scene.raster", …). Records store THIS, not the name.
struct ExecutorTypeId
{
    u64 value = 0;
    [[nodiscard]] constexpr bool valid() const noexcept { return value != 0; }
    friend constexpr bool operator==(ExecutorTypeId, ExecutorTypeId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(ExecutorTypeId, ExecutorTypeId) noexcept = default;
};
[[nodiscard]] ExecutorTypeId executor_type_id(StringView name) noexcept;
// Hash an executor param / resource-slot name to its stable id (schema + payload agree via this).
[[nodiscard]] u64 pass_param_id(StringView name) noexcept;

// The queue a pass runs on.
enum class QueueKind : u8
{
    Graphics = 0,
    Compute,
    Transfer,
};

// A typed pass-parameter type (NOT a material param — pass config: scalars, vectors, enums).
enum class ExecutorParamType : u8
{
    Bool = 0,
    I32,
    U32,
    F32,
    Vec4,
    Enum,
};

// A typed value — a tagged union, never a void*.
struct TypedValue
{
    ExecutorParamType type = ExecutorParamType::U32;
    union
    {
        bool b;
        i32 i;
        u32 u;
        f32 f;
        f32 v4[4];
        u32 e;
    };
};

// How a pass touches a resource slot.
enum class SlotAccess : u8
{
    Read = 0,
    Write,
    ReadWrite,
};

// The FRAME-GRAPH resource a slot refers to (attachments + read resources) — distinct from a shader BindingKind.
enum class SlotResourceKind : u8
{
    ColorTarget = 0,
    DepthTarget,
    Texture,
    StorageBuffer,
    UniformBuffer,
    AccelStructure,
};

// The schema for one typed parameter.
struct ParamSpec
{
    u64 name_hash = 0;
    ExecutorParamType type = ExecutorParamType::U32;
    bool required = true;
};

// The schema for one resource slot.
struct ResourceSlotSpec
{
    u64 name_hash = 0;
    SlotResourceKind kind = SlotResourceKind::StorageBuffer;
    SlotAccess access = SlotAccess::Read;
    bool required = true;
};

// An executor's typed, versioned schema.
struct ExecutorSchema
{
    u32 version = 1;
    QueueKind queue = QueueKind::Graphics;
    FixedArray<ParamSpec, kMaxExecutorParams> params;
    FixedArray<ResourceSlotSpec, kMaxExecutorSlots> slots;
};

// A registered executor: id + (diagnostic-only) name + schema. Records reference it by id.
struct PassExecutorDesc
{
    ExecutorTypeId id{};
    StringView name; // stable literal — for diagnostics/tooling, NOT used at record
    ExecutorSchema schema;
};

// One bound parameter value in a pass payload.
struct ParamValue
{
    u64 name_hash = 0;
    TypedValue value{};
};

// One bound resource in a pass payload.
struct ResourceRef
{
    u64 slot_name_hash = 0;
    SlotResourceKind kind = SlotResourceKind::StorageBuffer;
    SlotAccess access = SlotAccess::Read;
    u64 resource_id = 0; // a resolved resource id (a frame-graph resource, later); non-zero ⇒ bound
};

// A pass's typed payload. `executor` is a resolved id (no string at record); `schema_version` is the version it was
// cooked against (checked for old-schema handling).
struct PassPayload
{
    ExecutorTypeId executor{};
    u32 schema_version = 1;
    QueueKind queue = QueueKind::Graphics;
    FixedArray<ParamValue, kMaxExecutorParams> params;
    FixedArray<ResourceRef, kMaxExecutorSlots> resources;
};

// The registry: register built-ins + app executors; look up by id (compact, no string at record).
class ExecutorRegistry
{
public:
    explicit ExecutorRegistry(memory::IAllocator* alloc) noexcept : m_executors(alloc) {}

    // Register an executor. Rejects a duplicate id (DuplicateExecutor).
    bool register_executor(const PassExecutorDesc& desc, DiagnosticList& diags);

    // Look up by id — binary search, no string. nullptr if unregistered.
    [[nodiscard]] const PassExecutorDesc* find(ExecutorTypeId id) const noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(m_executors.size()); }

private:
    [[nodiscard]] usize lower_bound(ExecutorTypeId id) const noexcept;
    Array<PassExecutorDesc> m_executors; // sorted ascending by id
};

// Register the engine's built-in executors: scene.raster · fullscreen.raster · compute.dispatch ·
// transfer.clear/copy/blit/resolve · raytrace.dispatch · present. Returns the number registered.
u32 register_builtin_executors(ExecutorRegistry& registry, DiagnosticList& diags);

// Validate a pass payload against its executor's schema: executor exists (UnknownExecutor); schema version matches
// (SchemaMismatch); queue matches (QueueMismatch); every required param present + correctly typed and no unknown
// param (InvalidParam); every required slot bound + kind/access-correct (InvalidSlot).
[[nodiscard]] bool validate_payload(const ExecutorRegistry& registry, const PassPayload& payload,
                                    DiagnosticList& diags);
} // namespace crd::renderpass
