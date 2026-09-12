#pragma once

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/hesap/handles.hpp>
#include <crd/memory/allocator.hpp>

#include <variant>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// CommandResult — typed return shape for every CLI command, per
// ADR-0081 §3. Different transports (TTY / JSON-RPC / YAML / CRDR binary)
// render the same CommandResult differently.
//
// std::variant is allowed per advisor 2026-05-19: it's a value type, not
// a heap-owning STL container. Variant alternatives that need allocation
// (Text, Table rows, BinaryBlob bytes) wrap crd::containers::String /
// Array — no std::string / std::vector ownership leaks in.
// -----------------------------------------------------------------------

enum class DiagnosticLevel : crd::u8
{
    Hint = 0,
    Warning = 1,
    Error = 2,
};

struct Diagnostic
{
    DiagnosticLevel level = DiagnosticLevel::Hint;
    crd::containers::String message;

    explicit Diagnostic(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : message(alloc) {}
};

// ---- Result value variants ------------------------------------------

struct ResultVoid
{
};

struct ResultScalarF64
{
    crd::f64 value = 0.0;
};

struct ResultText
{
    crd::containers::String text;

    explicit ResultText(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : text(alloc) {}
};

// One row in a TableRows result. v0a ships this as `Array<String>`
// (untyped cells; the schema describes column types). v0b+ may
// promote to a typed cell variant when a table-returning op needs it.
struct ResultTableRow
{
    crd::containers::Array<crd::containers::String> cells;

    explicit ResultTableRow(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : cells(alloc) {}
};

struct ResultTable
{
    crd::containers::Array<crd::containers::String> column_names;
    crd::containers::Array<ResultTableRow> rows;

    explicit ResultTable(crd::memory::IAllocator* alloc = crd::memory::default_allocator())
        : column_names(alloc), rows(alloc)
    {
    }
};

struct ResultMatrixId
{
    crd::hesap::MatrixId value{};
};

struct ResultVectorId
{
    crd::hesap::VectorId value{};
};

struct ResultBinaryBlob
{
    crd::containers::Array<crd::u8> bytes;

    explicit ResultBinaryBlob(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : bytes(alloc) {}
};

struct ResultError
{
    crd::containers::String error_kind;     // e.g. "InvalidArgument", "OutOfMemory"
    crd::containers::String error_message;

    explicit ResultError(crd::memory::IAllocator* alloc = crd::memory::default_allocator())
        : error_kind(alloc), error_message(alloc)
    {
    }
};

// The result variant. Order matters for the std::variant index; do NOT
// reorder once consumers exist (ADR-0081 §2 schema versioning policy).
using ResultValue = std::variant<
    ResultVoid,
    ResultScalarF64,
    ResultText,
    ResultTable,
    ResultMatrixId,
    ResultVectorId,
    ResultBinaryBlob,
    ResultError>;

struct CommandResult
{
    bool ok = false;
    ResultValue value{ResultVoid{}};
    crd::containers::Array<Diagnostic> diagnostics;

    explicit CommandResult(crd::memory::IAllocator* alloc = crd::memory::default_allocator()) : diagnostics(alloc) {}
};

} // namespace crd::hesap::cli
