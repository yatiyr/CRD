#pragma once

#include <crd/containers/span.hpp>
#include <crd/containers/string.hpp>
#include <crd/hesap/cli/command_schema.hpp>
#include <crd/hesap/cli/json_writer.hpp>
#include <crd/memory/allocator.hpp>

namespace crd::hesap::cli
{
// -----------------------------------------------------------------------
// MCP tool-descriptor emission.
//
// Anthropic MCP (Model Context Protocol — https://modelcontextprotocol.io)
// describes a tool with:
//   { "name": "...", "description": "...",
//     "inputSchema": { "type": "object",
//                      "properties": { <param-name>: { "type": "...", ... }, ... },
//                      "required": [<required-param-names>] } }
//
// We extend the canonical shape with:
//   - "version": { "major": <u16>, "minor": <u16> }
//   - "outputSchema": { "kind": "<OutputKind>", "description": "..." }
//   - "capabilities": { "required": <u32-bitset>, "idempotent": <bool>,
//                       "reversible": <bool> }
//   - "deprecation": { "status": "<DeprecationStatus>",
//                      "replaced_by": "<command-name-or-empty>" }
//
// These extensions live alongside the MCP-canonical fields; MCP clients
// that don't know about them ignore them harmlessly. Per ADR-0081 §6.4
// MCP exact compatibility, the canonical fields appear in the MCP-spec
// shape unchanged.
// -----------------------------------------------------------------------

// Emit a single command as an MCP tool descriptor JSON object. Caller
// supplies the writer positioned to accept a value (inside an array or
// at root). The function emits exactly one JSON object.
void emit_mcp_tool(const CommandSchema& schema, JsonWriter& out);

// Emit a JSON array of MCP tool descriptors — one per schema in `schemas`.
// This is what meta.export-mcp-tools (ADR-0081 §8.5) returns.
void emit_mcp_tool_array(
    crd::containers::ConstSpan<const CommandSchema*> schemas,
    JsonWriter& out);

// Convenience: emit_mcp_tool_array into a fresh String, return it.
[[nodiscard]] crd::containers::String emit_mcp_tool_array_to_string(
    crd::containers::ConstSpan<const CommandSchema*> schemas,
    crd::memory::IAllocator* alloc = crd::memory::default_allocator());

} // namespace crd::hesap::cli
