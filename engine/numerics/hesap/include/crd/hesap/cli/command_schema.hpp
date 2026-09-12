#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/containers/string_view.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// CommandSchema — typed, introspectable, versioned declaration for a
// single CLI / RPC / MCP command. Per ADR-0081 §2.
//
// v0a ships the protocol-plumbing types ONLY: registry + schema + result.
// Parser / REPL / RPC server / MCP server live in Phase 4.0 crd-cli +
// crd-rpc. Every CLI surface from v0b forward registers a CommandSchema
// here; when crd-cli arrives it inherits hesap's commands for free.
//
// Schema versioning policy (ADR-0081 §2):
//   major bump = breaking change. Old schema stays Deprecated for ≥ 2
//                minor versions before removal.
//   minor bump = additive (new optional param, additional output fields).
// -----------------------------------------------------------------------

struct SchemaVersion
{
    crd::u16 major = 1;
    crd::u16 minor = 0;

    [[nodiscard]] constexpr bool operator==(const SchemaVersion&) const noexcept = default;
};

enum class DeprecationStatus : crd::u8
{
    Active = 0,
    Deprecated = 1,
    Removed = 2,
};

// Parameter kinds. Extended via the ParamSchema.enum_values field for
// constrained string params. The lower-layer kernels see raw f32 / f64
// per ADR-0078 §5; ParamKind models the wire-level CLI / JSON-RPC shape.
enum class ParamKind : crd::u8
{
    None = 0,
    Bool = 1,
    I32 = 2,
    I64 = 3,
    U32 = 4,
    U64 = 5,
    F32 = 6,
    F64 = 7,
    Complex32 = 8,
    Complex64 = 9,
    String = 10,
    Enum = 11,
    MatrixId = 12,
    VectorId = 13,
    EntityId = 14,
    Path = 15,
};

// Output kinds. Maps to CommandResult::Variant alternatives.
enum class OutputKind : crd::u8
{
    Void = 0,
    Scalar = 1,
    Text = 2,
    Table = 3,
    EntityId = 4,
    MatrixId = 5,
    VectorId = 6,
    BinaryBlob = 7,
    StructuredError = 8,
};

// Capability bitset (ADR-0081 §4). Sessions hold a capability mask; the
// command's required_caps must be a subset of the session's mask.
struct Capability
{
    crd::u32 bits = 0;

    static constexpr crd::u32 kHesapRead = 1u << 0;
    static constexpr crd::u32 kHesapWrite = 1u << 1;
    static constexpr crd::u32 kHesapCompute = 1u << 2;
    static constexpr crd::u32 kFsRead = 1u << 3;
    static constexpr crd::u32 kFsWrite = 1u << 4;

    [[nodiscard]] constexpr bool has(crd::u32 flag) const noexcept { return (bits & flag) == flag; }
    [[nodiscard]] constexpr bool subset_of(Capability other) const noexcept { return (bits & ~other.bits) == 0; }

    [[nodiscard]] constexpr bool operator==(const Capability&) const noexcept = default;
};

inline constexpr Capability kCapNone{0};

// ---- Parameter / output declarations ---------------------------------

struct ParamSchema
{
    crd::containers::String name;
    crd::containers::String description;
    ParamKind kind = ParamKind::None;

    // For ParamKind::Enum — pipe-separated values, e.g. "row|col". Empty otherwise.
    crd::containers::String enum_values;

    // Default-value-as-text. Empty string means no default (param is required).
    crd::containers::String default_value;

    bool required = true;

    explicit ParamSchema(crd::memory::IAllocator* alloc = crd::memory::default_allocator())
        : name(alloc), description(alloc), enum_values(alloc), default_value(alloc)
    {
    }
};

struct OutputSchema
{
    OutputKind kind = OutputKind::Void;
    crd::containers::String description;

    explicit OutputSchema(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : description(alloc) {}
};

// ---- The schema itself -----------------------------------------------

struct CommandSchema
{
    crd::containers::String name;        // dotted, e.g. "hesap.dense.matrix.create"
    crd::containers::String description;
    SchemaVersion version{1, 0};
    crd::containers::Array<ParamSchema> params;
    OutputSchema output;
    Capability required_caps{0};
    bool idempotent = false;
    bool reversible = false;
    DeprecationStatus deprecation = DeprecationStatus::Active;

    // For DeprecationStatus::Deprecated: which command replaces this one.
    crd::containers::String replaced_by;

    explicit CommandSchema(crd::memory::IAllocator* alloc = crd::memory::default_allocator())
        : name(alloc), description(alloc), params(alloc), output(alloc), replaced_by(alloc)
    {
    }
};

} // namespace crd::hesap::cli
