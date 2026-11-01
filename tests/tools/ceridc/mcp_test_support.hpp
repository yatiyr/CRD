#pragma once

// mcp_test_support.hpp — the request forms ceridc's MCP tests send (MCP-ADAPT.0, docs/design/agent-mcp.md). A revision
// 2026-07-28 request carries its protocol version and the client's capabilities in params._meta; a revision 2025-11-25
// connection opens with the initialize handshake.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>
#include <crd/memory/allocator.hpp>

#include <cstdio>
#include <cstring>

namespace crd::ceridc::test
{

// The params._meta member of every 2026-07-28 request the tests send.
inline constexpr const char* kModernMeta =
    R"("_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28",)"
    R"("io.modelcontextprotocol/clientCapabilities":{},)"
    R"("io.modelcontextprotocol/clientInfo":{"name":"ceridc-tests","version":"1"}})";

// The 2025-11-25 handshake (id 0).
inline constexpr const char* kInitialize =
    R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-11-25",)"
    R"("capabilities":{},"clientInfo":{"name":"ceridc-tests","version":"1"}}})";

// `request` as a 2026-07-28 request: kModernMeta becomes the first member of its params object. A request without a
// params object is returned unchanged (the tests that send one mean it).
[[nodiscard]] inline crd::containers::String modern(const char* request, crd::memory::IAllocator* alloc)
{
    constexpr const char* kParams = R"("params":{)";
    const char* const     params  = std::strstr(request, kParams);
    crd::containers::String out(alloc);
    if (params == nullptr)
    {
        out.append(request);
        return out;
    }
    const char* const rest = params + std::strlen(kParams);
    out.append(request, static_cast<crd::usize>(rest - request));
    out.append(kModernMeta);
    if (*rest != '}')
    {
        out.push_back(',');
    }
    out.append(rest);
    return out;
}

// Appends the opening of a 2026-07-28 tools/call of `tool` with `id`, up to its arguments value.
inline void append_call_head(crd::containers::String& out, crd::i64 id, const char* tool)
{
    char text[128];
    (void)std::snprintf(text, sizeof(text), R"({"jsonrpc":"2.0","id":%lld,"method":"tools/call","params":{)",
                        static_cast<long long>(id));
    out.append(text);
    out.append(kModernMeta);
    (void)std::snprintf(text, sizeof(text), R"(,"name":"%s","arguments":)", tool);
    out.append(text);
}

} // namespace crd::ceridc::test
