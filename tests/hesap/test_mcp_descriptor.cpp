#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/cli/command_schema.hpp>
#include <crd/hesap/cli/json_writer.hpp>
#include <crd/hesap/cli/mcp_descriptor.hpp>
#include <crd/memory/allocators/tlsf_allocator.hpp>

#include <utility>

using crd::hesap::cli::Capability;
using crd::hesap::cli::CommandSchema;
using crd::hesap::cli::emit_mcp_tool;
using crd::hesap::cli::emit_mcp_tool_array_to_string;
using crd::hesap::cli::JsonWriter;
using crd::hesap::cli::OutputKind;
using crd::hesap::cli::ParamKind;
using crd::hesap::cli::ParamSchema;

namespace
{
CommandSchema build_matrix_create(crd::memory::IAllocator* alloc)
{
    CommandSchema s{alloc};
    s.name = crd::containers::String{"hesap.dense.matrix.create", alloc};
    s.description = crd::containers::String{"Create a dense matrix.", alloc};
    s.output.kind = OutputKind::MatrixId;
    s.output.description = crd::containers::String{"Handle to the newly created matrix.", alloc};
    s.required_caps.bits = Capability::kHesapWrite;
    s.idempotent = false;

    ParamSchema rows{alloc};
    rows.name = crd::containers::String{"rows", alloc};
    rows.description = crd::containers::String{"Number of rows.", alloc};
    rows.kind = ParamKind::U32;
    rows.required = true;
    s.params.push_back(std::move(rows));

    ParamSchema layout{alloc};
    layout.name = crd::containers::String{"layout", alloc};
    layout.description = crd::containers::String{"Storage layout.", alloc};
    layout.kind = ParamKind::Enum;
    layout.enum_values = crd::containers::String{"row|col", alloc};
    layout.default_value = crd::containers::String{"row", alloc};
    layout.required = false;
    s.params.push_back(std::move(layout));

    return s;
}

[[nodiscard]] bool contains(crd::containers::StringView haystack, crd::containers::StringView needle) noexcept
{
    return haystack.find(needle) != crd::containers::StringView::npos;
}
} // namespace

TEST_CASE("emit_mcp_tool produces MCP-canonical fields", "[hesap][cli][mcp]")
{
    crd::memory::TlsfAllocator alloc(128 * 1024);
    const CommandSchema s = build_matrix_create(&alloc);

    JsonWriter w{&alloc};
    emit_mcp_tool(s, w);
    const crd::containers::StringView json{w.str().c_str(), w.str().size()};

    // Canonical MCP-spec fields.
    REQUIRE(contains(json, crd::containers::StringView{"\"name\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"description\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"inputSchema\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"properties\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"required\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"type\":\"object\""}));

    // The command name and one param appear.
    REQUIRE(contains(json, crd::containers::StringView{"hesap.dense.matrix.create"}));
    REQUIRE(contains(json, crd::containers::StringView{"\"rows\""}));

    // Enum values are emitted as a JSON array.
    REQUIRE(contains(json, crd::containers::StringView{"\"row\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"col\""}));

    // Cerid extensions present.
    REQUIRE(contains(json, crd::containers::StringView{"\"version\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"outputSchema\""}));
    REQUIRE(contains(json, crd::containers::StringView{"\"capabilities\""}));
}

TEST_CASE("emit_mcp_tool_array wraps tools in a JSON array", "[hesap][cli][mcp]")
{
    crd::memory::TlsfAllocator alloc(128 * 1024);
    const CommandSchema s = build_matrix_create(&alloc);
    const CommandSchema* schemas[1] = {&s};
    const crd::containers::String json =
        emit_mcp_tool_array_to_string(crd::containers::ConstSpan<const CommandSchema*>{schemas, 1}, &alloc);
    REQUIRE(json.c_str()[0] == '[');
    REQUIRE(json.c_str()[json.size() - 1] == ']');
}
