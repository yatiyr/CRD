// mcp.cpp — GEO-11: the MCP surface — JSON-RPC 2.0 dispatch over the SAME verbs the CLI runs (one
// implementation, two transports). Protocol faults answer as JSON-RPC errors; verb failures surface as
// isError tool content (the MCP convention — the agent reads the report either way).

#include <crd/assetio/json.hpp>
#include <crd/assetio/json_write.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/perf/diag_commands.hpp>

#include <cmath>
#include <cstring>

namespace crd::ceridc
{

// `strtok_s` is the MSVC spelling of POSIX `strtok_r` — same signature, same semantics. One portable inline wrapper
// (a function, not a function-like macro — cppcoreguidelines-macro-usage).
inline char* crd_strtok(char* str, const char* delim, char** ctx) noexcept
{
#if defined(_WIN32)
    return strtok_s(str, delim, ctx);
#else
    return strtok_r(str, delim, ctx);
#endif
}

namespace
{
    using crd::assetio::JsonWriter;
    namespace json = crd::assetio::json;

    constexpr const char* kProtocolVersion = "2024-11-05";

    struct ToolSpec
    {
        const char* name;
        const char* description;
        const char* args; // comma-separated "name:type" (string|number|boolean), '!' = required
    };

    constexpr ToolSpec kTools[] = {
        {"import", "Parse an interchange file (.glb/.gltf/.stl/.obj/.ply/.3mf/.otio/.wav/.aiff/.flac/.mid) and report its contents", "path:string!"},
        {"cook", "Run the incremental asset processor over a source root into a PACK", "root:string!,out:string!"},
        {"query", "List a PACK's manifest: every uuid, type, and name", "pack:string!"},
        {"instantiate", "Compose a scene: instance a named mesh from a pack at a translation; transactional; dry_run supported", "pack:string!,asset:string!,x:number,y:number,z:number,out:string!,dry_run:boolean"},
        {"sequence", "Author a 2-shot timeline (two clips + centered dissolve) as TIML + .otio", "name:string!,clip_a:string!,frames_a:number!,clip_b:string!,frames_b:number!,transition:number,out_timl:string!,out_otio:string!"},
        {"render", "Render a .otio timeline to an EXR frame sequence", "otio:string!,dir:string!,max_frames:number"},
        {"export", "Convert a TIML artifact to .otio interchange", "timl:string!,out:string!"},
    };

    // Listed only when the host bound a diagnostic command service. The host's grant is not an argument.
    constexpr ToolSpec kDiagTool = {
        "diag",
        "Run one typed, bounded diagnostic command (diag.commands lists them); pages are cut from one retained "
        "snapshot by cursor",
        "command:string!,path:string,cursor:number,page_items:number,page_bytes:number,schema:number,args:object"};

    void write_input_schema(JsonWriter& w, const char* args)
    {
        w.begin_object();
        w.kv("type", "object");
        w.key("properties");
        w.begin_object();
        char buf[512];
        std::snprintf(buf, sizeof(buf), "%s", args);
        char* save = nullptr;
        for (char* tok = crd_strtok(buf, ",", &save); tok != nullptr; tok = crd_strtok(nullptr, ",", &save))
        {
            char* colon = std::strchr(tok, ':');
            if (colon == nullptr)
            {
                continue;
            }
            *colon      = '\0';
            char* type  = colon + 1;
            char* bang  = std::strchr(type, '!');
            if (bang != nullptr)
            {
                *bang = '\0';
            }
            w.key(tok);
            w.begin_object();
            w.kv("type", type);
            w.end_object();
        }
        w.end_object();
        w.key("required");
        w.begin_array();
        char buf2[512];
        std::snprintf(buf2, sizeof(buf2), "%s", args);
        save = nullptr;
        for (char* tok = crd_strtok(buf2, ",", &save); tok != nullptr; tok = crd_strtok(nullptr, ",", &save))
        {
            if (std::strchr(tok, '!') == nullptr)
            {
                continue;
            }
            char* colon = std::strchr(tok, ':');
            if (colon != nullptr)
            {
                *colon = '\0';
            }
            w.value_string(tok);
        }
        w.end_array();
        w.end_object();
    }

    // pull a string/number/bool argument out of the `arguments` object (buf-copied strings)
    [[nodiscard]] bool arg_str(const json::JsonDoc& doc, crd::u32 args, const char* key, char* buf,
                               crd::u32 cap)
    {
        const crd::u32 node = json::find(doc, args, key);
        return node != json::kInvalid && json::str_value(doc, node, buf, cap) > 0;
    }
    [[nodiscard]] crd::f64 arg_num(const json::JsonDoc& doc, crd::u32 args, const char* key, crd::f64 def)
    {
        return json::as_f64(doc, json::find(doc, args, key), def);
    }
    [[nodiscard]] bool arg_bool(const json::JsonDoc& doc, crd::u32 args, const char* key)
    {
        return json::as_bool(doc, json::find(doc, args, key), false);
    }

    // A whole JSON number in [0, max], or false. An absent argument keeps `out` (the request's default).
    [[nodiscard]] bool arg_count(const json::JsonDoc& doc, crd::u32 args, const char* key, crd::f64 max,
                                 crd::f64& out)
    {
        const crd::u32 node = json::find(doc, args, key);
        if (node == json::kInvalid)
        {
            return true;
        }
        if (doc.nodes[node].type != json::JsonType::Number)
        {
            return false;
        }
        const crd::f64 v = doc.nodes[node].number;
        if (!(v >= 0.0) || v != std::floor(v) || v > max)
        {
            return false;
        }
        out = v;
        return true;
    }

    // The diag tool: the request's own fields only, then the host's service. A count above a u32 bound reaches the
    // service as the largest u32, so the service refuses it as oversized rather than the transport guessing.
    [[nodiscard]] crd::containers::String call_diag(const json::JsonDoc& doc, crd::u32 args,
                                                    crd::perf::DiagCommandService& service,
                                                    crd::memory::IAllocator*       alloc)
    {
        const crd::u32 command = json::find(doc, args, "command");
        if (command == json::kInvalid || doc.nodes[command].type != json::JsonType::String)
        {
            return crd::containers::String("", alloc);
        }
        const auto view_of = [&doc](crd::u32 node) {
            return crd::containers::StringView{doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len};
        };
        crd::perf::DiagRequest request;
        request.command = view_of(command);
        const crd::u32 path = json::find(doc, args, "path");
        if (path != json::kInvalid)
        {
            if (doc.nodes[path].type != json::JsonType::String)
            {
                return crd::containers::String("", alloc);
            }
            request.path = view_of(path);
        }
        constexpr crd::f64 max_exact = 9007199254740991.0; // 2^53 - 1: a cursor is exact below it
        crd::f64           cursor    = 0.0;
        crd::f64           items     = 0.0;
        crd::f64           bytes     = 0.0;
        crd::f64           schema    = static_cast<crd::f64>(crd::perf::kDiagCommandSchemaVersion);
        if (!arg_count(doc, args, "cursor", max_exact, cursor) ||
            !arg_count(doc, args, "page_items", max_exact, items) ||
            !arg_count(doc, args, "page_bytes", max_exact, bytes) || !arg_count(doc, args, "schema", max_exact, schema))
        {
            return crd::containers::String("", alloc);
        }
        // The command's named arguments: an object whose members are all strings. The service bounds and checks them.
        crd::containers::Array<crd::perf::DiagArg> named(alloc);
        const crd::u32                             args_node = json::find(doc, args, "args");
        if (args_node != json::kInvalid)
        {
            if (doc.nodes[args_node].type != json::JsonType::Object)
            {
                return crd::containers::String("", alloc);
            }
            for (crd::u32 m = doc.nodes[args_node].child; m != json::kInvalid; m = doc.nodes[m].next)
            {
                if (doc.nodes[m].type != json::JsonType::String)
                {
                    return crd::containers::String("", alloc);
                }
                const crd::containers::StringView name{doc.strings.data() + doc.nodes[m].key_off,
                                                       doc.nodes[m].key_len};
                named.push_back(crd::perf::DiagArg{name, view_of(m)});
            }
        }
        constexpr crd::f64 u32_max = 4294967295.0;
        request.args               = crd::containers::as_const_span(named);
        request.cursor             = static_cast<crd::u64>(cursor);
        request.page_items         = static_cast<crd::u32>(items < u32_max ? items : u32_max);
        request.page_bytes         = static_cast<crd::u32>(bytes < u32_max ? bytes : u32_max);
        request.schema_version     = static_cast<crd::u32>(schema < u32_max ? schema : u32_max);
        return verb_diag(service, request, alloc);
    }

    // dispatch one tools/call — returns the verb's JSON report (ok:false reports become isError content)
    [[nodiscard]] crd::containers::String call_tool(const json::JsonDoc& doc, crd::u32 params,
                                                    crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* diag)
    {
        char name[64] = {};
        (void)json::str_value(doc, json::find(doc, params, "name"), name, sizeof(name));
        const crd::u32 args = json::find(doc, params, "arguments");
        if (std::strcmp(name, "diag") == 0)
        {
            if (diag == nullptr || args == json::kInvalid || doc.nodes[args].type != json::JsonType::Object)
            {
                return crd::containers::String("", alloc);
            }
            return call_diag(doc, args, *diag, alloc);
        }

        char a[512];
        char b[512];
        char c[512];
        char d[512];
        if (std::strcmp(name, "import") == 0)
        {
            if (!arg_str(doc, args, "path", a, sizeof(a)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_import(a, alloc);
        }
        if (std::strcmp(name, "cook") == 0)
        {
            if (!arg_str(doc, args, "root", a, sizeof(a)) || !arg_str(doc, args, "out", b, sizeof(b)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_cook(a, b, alloc);
        }
        if (std::strcmp(name, "query") == 0)
        {
            if (!arg_str(doc, args, "pack", a, sizeof(a)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_query(a, alloc);
        }
        if (std::strcmp(name, "instantiate") == 0)
        {
            if (!arg_str(doc, args, "pack", a, sizeof(a)) || !arg_str(doc, args, "asset", b, sizeof(b)) ||
                !arg_str(doc, args, "out", c, sizeof(c)))
            {
                return crd::containers::String("", alloc);
            }
            const crd::f32 translate[3] = {static_cast<crd::f32>(arg_num(doc, args, "x", 0.0)),
                                           static_cast<crd::f32>(arg_num(doc, args, "y", 0.0)),
                                           static_cast<crd::f32>(arg_num(doc, args, "z", 0.0))};
            return verb_instantiate(a, b, translate, arg_bool(doc, args, "dry_run"), c, alloc);
        }
        if (std::strcmp(name, "sequence") == 0)
        {
            char clip_a[256];
            char clip_b[256];
            if (!arg_str(doc, args, "name", a, sizeof(a)) ||
                !arg_str(doc, args, "clip_a", clip_a, sizeof(clip_a)) ||
                !arg_str(doc, args, "clip_b", clip_b, sizeof(clip_b)) ||
                !arg_str(doc, args, "out_timl", c, sizeof(c)) || !arg_str(doc, args, "out_otio", d, sizeof(d)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_sequence(a, clip_a, static_cast<crd::i64>(arg_num(doc, args, "frames_a", 0.0)),
                                 clip_b, static_cast<crd::i64>(arg_num(doc, args, "frames_b", 0.0)),
                                 static_cast<crd::i64>(arg_num(doc, args, "transition", 0.0)), c, d, alloc);
        }
        if (std::strcmp(name, "render") == 0)
        {
            if (!arg_str(doc, args, "otio", a, sizeof(a)) || !arg_str(doc, args, "dir", b, sizeof(b)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_render(a, b, static_cast<crd::i64>(arg_num(doc, args, "max_frames", 0.0)), alloc);
        }
        if (std::strcmp(name, "export") == 0)
        {
            if (!arg_str(doc, args, "timl", a, sizeof(a)) || !arg_str(doc, args, "out", b, sizeof(b)))
            {
                return crd::containers::String("", alloc);
            }
            return verb_export_timeline(a, b, alloc);
        }
        return crd::containers::String("", alloc); // unknown tool → protocol error upstream
    }
} // namespace

crd::containers::String mcp_handle(crd::containers::ConstSpan<crd::u8> request, crd::memory::IAllocator* alloc)
{
    return mcp_handle(request, alloc, nullptr);
}

crd::containers::String mcp_handle(crd::containers::ConstSpan<crd::u8> request, crd::memory::IAllocator* alloc,
                                   crd::perf::DiagCommandService* diag)
{
    json::JsonDoc doc(alloc);
    JsonWriter    w(alloc);
    if (!json::parse(request, doc) || doc.root == json::kInvalid)
    {
        w.begin_object();
        w.kv("jsonrpc", "2.0");
        w.key("id");
        w.value_null();
        w.key("error");
        w.begin_object();
        w.kv("code", static_cast<crd::i64>(-32700));
        w.kv("message", "parse error");
        w.end_object();
        w.end_object();
        return crd::containers::String(w.str());
    }

    char method[64] = {};
    (void)json::str_value(doc, json::find(doc, doc.root, "method"), method, sizeof(method));
    const crd::u32 id_node = json::find(doc, doc.root, "id");
    if (id_node == json::kInvalid) // notification — no reply
    {
        return crd::containers::String("", alloc);
    }
    const crd::i64 id = json::as_i64(doc, id_node, 0);

    const auto begin_result = [&] {
        w.begin_object();
        w.kv("jsonrpc", "2.0");
        w.kv("id", id);
        w.key("result");
    };

    if (std::strcmp(method, "initialize") == 0)
    {
        begin_result();
        w.begin_object();
        w.kv("protocolVersion", kProtocolVersion);
        w.key("capabilities");
        w.begin_object();
        w.key("tools");
        w.begin_object();
        w.end_object();
        w.end_object();
        w.key("serverInfo");
        w.begin_object();
        w.kv("name", "ceridc");
        w.kv("version", "1.0.0");
        w.end_object();
        w.end_object();
        w.end_object();
        return crd::containers::String(w.str());
    }
    if (std::strcmp(method, "ping") == 0)
    {
        begin_result();
        w.begin_object();
        w.end_object();
        w.end_object();
        return crd::containers::String(w.str());
    }
    if (std::strcmp(method, "tools/list") == 0)
    {
        begin_result();
        w.begin_object();
        w.key("tools");
        w.begin_array();
        for (const ToolSpec& t : kTools)
        {
            w.begin_object();
            w.kv("name", t.name);
            w.kv("description", t.description);
            w.key("inputSchema");
            write_input_schema(w, t.args);
            w.end_object();
        }
        if (diag != nullptr)
        {
            w.begin_object();
            w.kv("name", kDiagTool.name);
            w.kv("description", kDiagTool.description);
            w.key("inputSchema");
            write_input_schema(w, kDiagTool.args);
            w.end_object();
        }
        w.end_array();
        w.end_object();
        w.end_object();
        return crd::containers::String(w.str());
    }
    if (std::strcmp(method, "tools/call") == 0)
    {
        const crd::u32                params = json::find(doc, doc.root, "params");
        const crd::containers::String report = call_tool(doc, params, alloc, diag);
        if (report.empty()) // unknown tool / missing required args = a PROTOCOL fault
        {
            w.begin_object();
            w.kv("jsonrpc", "2.0");
            w.kv("id", id);
            w.key("error");
            w.begin_object();
            w.kv("code", static_cast<crd::i64>(-32602));
            w.kv("message", "unknown tool or missing required arguments");
            w.end_object();
            w.end_object();
            return crd::containers::String(w.str());
        }
        // the report is a JSON object with an "ok" field — surface !ok as isError
        const bool is_error = std::strstr(report.c_str(), "\"ok\":false") != nullptr;
        begin_result();
        w.begin_object();
        w.key("content");
        w.begin_array();
        w.begin_object();
        w.kv("type", "text");
        w.kv("text", report.c_str());
        w.end_object();
        w.end_array();
        w.kv("isError", is_error);
        w.end_object();
        w.end_object();
        return crd::containers::String(w.str());
    }

    w.begin_object();
    w.kv("jsonrpc", "2.0");
    w.kv("id", id);
    w.key("error");
    w.begin_object();
    w.kv("code", static_cast<crd::i64>(-32601));
    w.kv("message", "method not found");
    w.end_object();
    w.end_object();
    return crd::containers::String(w.str());
}

} // namespace crd::ceridc
