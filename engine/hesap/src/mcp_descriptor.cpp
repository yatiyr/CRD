#include <crd/hesap/cli/mcp_descriptor.hpp>

namespace crd::hesap::cli
{
namespace
{
[[nodiscard]] crd::containers::StringView param_kind_to_json_type(ParamKind k) noexcept
{
    switch (k)
    {
    case ParamKind::None:
        return "null";
    case ParamKind::Bool:
        return "boolean";
    case ParamKind::I32:
    case ParamKind::I64:
    case ParamKind::U32:
    case ParamKind::U64:
        return "integer";
    case ParamKind::F32:
    case ParamKind::F64:
        return "number";
    case ParamKind::Complex32:
    case ParamKind::Complex64:
        // MCP / JSON-Schema: complex is an object {re,im}; we describe it as
        // such here so MCP-aware agents emit the right shape. v0b adds the
        // codec helpers that parse this back.
        return "object";
    case ParamKind::String:
    case ParamKind::Enum:
    case ParamKind::Path:
    case ParamKind::MatrixId:  // Handles travel as their textual form (u64 hex).
    case ParamKind::VectorId:
    case ParamKind::EntityId:
        return "string";
    }
    return "string";
}

[[nodiscard]] crd::containers::StringView output_kind_to_string(OutputKind k) noexcept
{
    switch (k)
    {
    case OutputKind::Void:
        return "void";
    case OutputKind::Scalar:
        return "scalar";
    case OutputKind::Text:
        return "text";
    case OutputKind::Table:
        return "table";
    case OutputKind::EntityId:
        return "entity_id";
    case OutputKind::MatrixId:
        return "matrix_id";
    case OutputKind::VectorId:
        return "vector_id";
    case OutputKind::BinaryBlob:
        return "binary_blob";
    case OutputKind::StructuredError:
        return "structured_error";
    }
    return "void";
}

[[nodiscard]] crd::containers::StringView deprecation_to_string(DeprecationStatus s) noexcept
{
    switch (s)
    {
    case DeprecationStatus::Active:
        return "active";
    case DeprecationStatus::Deprecated:
        return "deprecated";
    case DeprecationStatus::Removed:
        return "removed";
    }
    return "active";
}

void emit_param_property(const ParamSchema& p, JsonWriter& out)
{
    // Each property is keyed by name and is itself a JSON-Schema object.
    out.key(static_cast<crd::containers::StringView>(p.name));
    out.begin_object();
    out.key_value("type", param_kind_to_json_type(p.kind));
    if (!p.description.empty())
    {
        out.key_value("description", static_cast<crd::containers::StringView>(p.description));
    }
    if (p.kind == ParamKind::Enum && !p.enum_values.empty())
    {
        out.key("enum");
        out.begin_array();
        // Split on '|'.
        const crd::containers::StringView all{p.enum_values.c_str(), p.enum_values.size()};
        crd::usize start = 0;
        for (crd::usize i = 0; i <= all.size(); ++i)
        {
            if (i == all.size() || all[i] == '|')
            {
                out.value_string(all.substr(start, i - start));
                start = i + 1;
            }
        }
        out.end_array();
    }
    if (!p.default_value.empty())
    {
        out.key_value("default", static_cast<crd::containers::StringView>(p.default_value));
    }
    out.end_object();
}
} // namespace

void emit_mcp_tool(const CommandSchema& schema, JsonWriter& out)
{
    out.begin_object();
    out.key_value("name", static_cast<crd::containers::StringView>(schema.name));
    out.key_value("description", static_cast<crd::containers::StringView>(schema.description));

    // version: { major, minor }
    out.key("version");
    out.begin_object();
    out.key_value("major", static_cast<crd::u32>(schema.version.major));
    out.key_value("minor", static_cast<crd::u32>(schema.version.minor));
    out.end_object();

    // inputSchema: canonical MCP shape — { type: "object", properties: {...}, required: [...] }
    out.key("inputSchema");
    out.begin_object();
    out.key_value("type", crd::containers::StringView{"object"});
    out.key("properties");
    out.begin_object();
    for (const ParamSchema& p : schema.params)
    {
        emit_param_property(p, out);
    }
    out.end_object();
    out.key("required");
    out.begin_array();
    for (const ParamSchema& p : schema.params)
    {
        if (p.required)
        {
            out.value_string(static_cast<crd::containers::StringView>(p.name));
        }
    }
    out.end_array();
    out.end_object();

    // outputSchema (Cerid extension; MCP clients ignore unknown fields).
    out.key("outputSchema");
    out.begin_object();
    out.key_value("kind", output_kind_to_string(schema.output.kind));
    if (!schema.output.description.empty())
    {
        out.key_value("description", static_cast<crd::containers::StringView>(schema.output.description));
    }
    out.end_object();

    // capabilities (Cerid extension).
    out.key("capabilities");
    out.begin_object();
    out.key_value("required", schema.required_caps.bits);
    out.key_value("idempotent", schema.idempotent);
    out.key_value("reversible", schema.reversible);
    out.end_object();

    // deprecation (Cerid extension).
    out.key("deprecation");
    out.begin_object();
    out.key_value("status", deprecation_to_string(schema.deprecation));
    if (!schema.replaced_by.empty())
    {
        out.key_value("replaced_by", static_cast<crd::containers::StringView>(schema.replaced_by));
    }
    out.end_object();

    out.end_object();
}

void emit_mcp_tool_array(crd::containers::ConstSpan<const CommandSchema*> schemas, JsonWriter& out)
{
    out.begin_array();
    for (const CommandSchema* s : schemas)
    {
        if (s != nullptr)
        {
            emit_mcp_tool(*s, out);
        }
    }
    out.end_array();
}

crd::containers::String emit_mcp_tool_array_to_string(
    crd::containers::ConstSpan<const CommandSchema*> schemas,
    crd::memory::IAllocator* alloc)
{
    JsonWriter w{alloc};
    emit_mcp_tool_array(schemas, w);
    return w.take();
}

} // namespace crd::hesap::cli
