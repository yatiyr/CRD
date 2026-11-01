// mcp.cpp — MCP-ADAPT.0: ceridc's MCP server over the SAME verbs the CLI runs (one implementation, two transports). It
// is a dual-era server: revision 2026-07-28 serves every request whose params carry the protocol version and the
// client's capabilities in `_meta`, and an `initialize` handshake selects revision 2025-11-25 for the rest of the
// process. Protocol faults answer as JSON-RPC errors; a verb's failure and a refused argument answer as tool results
// with isError, so the model reads the reason and can correct the call. Contract: docs/design/agent-mcp.md.

#include <crd/assetio/json.hpp>
#include <crd/assetio/json_write.hpp>
#include <crd/ceridc/verbs.hpp>
#include <crd/core/build_config.hpp>
#include <crd/perf/diag_commands.hpp>

#include <algorithm>
#include <cfloat> // FLT_MAX: the f32 arguments' range
#include <cmath>
#include <cstdio>
#include <cstring>

namespace crd::ceridc
{

namespace
{
    using crd::assetio::JsonWriter;
    using crd::containers::String;
    using crd::containers::StringView;
    namespace json = crd::assetio::json;

    constexpr const char* kModernVersion = "2026-07-28";
    constexpr const char* kLegacyVersion = "2025-11-25";

    constexpr const char* kMetaProtocolVersion    = "io.modelcontextprotocol/protocolVersion";
    constexpr const char* kMetaClientCapabilities = "io.modelcontextprotocol/clientCapabilities";
    constexpr const char* kMetaClientInfo         = "io.modelcontextprotocol/clientInfo";
    constexpr const char* kMetaLogLevel           = "io.modelcontextprotocol/logLevel";
    constexpr const char* kMetaServerInfo         = "io.modelcontextprotocol/serverInfo";

    // The tool set is fixed when the process starts (the diag tool is listed exactly when the host bound a service), so
    // a discovery or list result stays true for the life of the process; an hour bounds how long a client keeps it
    // across a restart under other flags. Every caller gets the same answer, so the scope is public.
    constexpr crd::u64 kListTtlMs = 3600000U;

    constexpr crd::i64 kParseError                 = -32700;
    constexpr crd::i64 kInvalidRequest             = -32600;
    constexpr crd::i64 kMethodNotFound             = -32601;
    constexpr crd::i64 kInvalidParams              = -32602;
    constexpr crd::i64 kInternalError              = -32603;
    constexpr crd::i64 kUnsupportedProtocolVersion = -32022;

    constexpr crd::f64 kMaxExactWhole = 9007199254740991.0; // 2^53 - 1: every whole number up to it is exact in a f64
    constexpr crd::u32 kPathBytes     = 512U;               // a path argument's buffer: at most 511 bytes and the NUL
    constexpr crd::u32 kClipBytes     = 256U;               // a clip name's buffer

    constexpr const char* kInstructions =
        "Each tool is one ceridc verb. Its JSON report is the structured result; a report with ok false is a tool "
        "error whose reason says what to change. Paths name files on the host. The diag tool, when listed, runs typed "
        "and bounded diagnostic commands under the authority the host granted when it started; no request can raise "
        "it.";

    // How a tool argument is decoded. The schema tools/list publishes and the check every call passes are both written
    // from this, so the two cannot disagree. DIAG.10a: an argument is used exactly as sent or refused; it is never cut,
    // clamped or read as a default because its type is wrong.
    enum class ArgKind : crd::u8
    {
        Text,    // a string of 1 to cap - 1 bytes without NUL (a C string for the verb; a cut path names another file)
        String,  // any string: the diag service bounds it and refuses it with its own reason
        Number,  // a number a f32 holds
        Whole,   // a whole number within +-(2^53 - 1)
        Count,   // a whole number from 0 to 2^53 - 1
        Flag,    // a boolean (a dry_run sent as "true" must not write the file)
        Strings, // an object whose members are strings
    };

    struct ArgSpec
    {
        const char* name;
        ArgKind     kind;
        bool        required;
        crd::u32    cap; // Text: the verb's buffer bytes
        const char* description;
    };

    using RunTool = String (*)(const json::JsonDoc& doc, crd::u32 args, crd::memory::IAllocator* alloc,
                               crd::perf::DiagCommandService* diag);

    struct ToolSpec
    {
        const char*                         name;
        const char*                         title;
        const char*                         description;
        crd::containers::ConstSpan<ArgSpec> args;
        const char*                         output; // the JSON Schema of the report when the verb answers
        bool                                read_only;
        bool                                destructive;
        RunTool                             run;
    };

    // ── decoding validated arguments ────────────────────────────────────────────────────────────────────────────────

    [[nodiscard]] bool key_is(const json::JsonDoc& doc, crd::u32 member, const char* name) noexcept
    {
        const crd::usize len = std::strlen(name);
        return doc.nodes[member].key_len == len &&
               std::memcmp(doc.strings.data() + doc.nodes[member].key_off, name, len) == 0;
    }

    [[nodiscard]] StringView string_of(const json::JsonDoc& doc, crd::u32 node) noexcept
    {
        return StringView{doc.strings.data() + doc.nodes[node].str_off, doc.nodes[node].str_len};
    }

    [[nodiscard]] bool is_whole(crd::f64 v, crd::f64 min) noexcept
    {
        return v >= min && v <= kMaxExactWhole && v == std::floor(v);
    }

    // The arguments of a call, checked against the tool's declaration before anything runs. `reason` names the first
    // argument refused and what it must be.
    [[nodiscard]] bool check_arguments(const json::JsonDoc& doc, crd::u32 args,
                                       crd::containers::ConstSpan<ArgSpec> specs, String& reason)
    {
        char line[256];
        for (crd::u32 m = args == json::kInvalid ? json::kInvalid : doc.nodes[args].child; m != json::kInvalid;
             m = doc.nodes[m].next)
        {
            const ArgSpec* spec = nullptr;
            for (const ArgSpec& s : specs)
            {
                if (key_is(doc, m, s.name))
                {
                    spec = &s;
                }
            }
            if (spec == nullptr)
            {
                reason.append("unknown argument '");
                reason.append(StringView{doc.strings.data() + doc.nodes[m].key_off, doc.nodes[m].key_len});
                reason.append("'");
                return false;
            }
            for (crd::u32 e = doc.nodes[args].child; e != m; e = doc.nodes[e].next)
            {
                if (key_is(doc, e, spec->name))
                {
                    (void)std::snprintf(line, sizeof(line), "argument '%s' is given more than once", spec->name);
                    reason.append(line);
                    return false;
                }
            }
        }
        for (const ArgSpec& s : specs)
        {
            const crd::u32 node = json::find(doc, args, s.name);
            if (node == json::kInvalid)
            {
                if (s.required)
                {
                    (void)std::snprintf(line, sizeof(line), "argument '%s' is required", s.name);
                    reason.append(line);
                    return false;
                }
                continue;
            }
            const json::JsonNode& n  = doc.nodes[node];
            bool                  ok = false;
            switch (s.kind)
            {
            case ArgKind::Text:
                ok = n.type == json::JsonType::String && n.str_len > 0U && n.str_len < s.cap &&
                     std::memchr(doc.strings.data() + n.str_off, '\0', n.str_len) == nullptr;
                (void)std::snprintf(line, sizeof(line),
                                    "argument '%s' must be a string of 1 to %u UTF-8 bytes without NUL", s.name,
                                    s.cap - 1U);
                break;
            case ArgKind::String:
                ok = n.type == json::JsonType::String;
                (void)std::snprintf(line, sizeof(line), "argument '%s' must be a string", s.name);
                break;
            case ArgKind::Number:
                ok = n.type == json::JsonType::Number && n.number >= -static_cast<crd::f64>(FLT_MAX) &&
                     n.number <= static_cast<crd::f64>(FLT_MAX);
                (void)std::snprintf(line, sizeof(line), "argument '%s' must be a number within the f32 range",
                                    s.name);
                break;
            case ArgKind::Whole:
                ok = n.type == json::JsonType::Number && is_whole(n.number, -kMaxExactWhole);
                (void)std::snprintf(line, sizeof(line),
                                    "argument '%s' must be a whole number within +-(2^53 - 1)", s.name);
                break;
            case ArgKind::Count:
                ok = n.type == json::JsonType::Number && is_whole(n.number, 0.0);
                (void)std::snprintf(line, sizeof(line), "argument '%s' must be a whole number from 0 to 2^53 - 1",
                                    s.name);
                break;
            case ArgKind::Flag:
                ok = n.type == json::JsonType::Bool;
                (void)std::snprintf(line, sizeof(line), "argument '%s' must be a boolean", s.name);
                break;
            case ArgKind::Strings:
                ok = n.type == json::JsonType::Object;
                for (crd::u32 m = n.child; ok && m != json::kInvalid; m = doc.nodes[m].next)
                {
                    ok = doc.nodes[m].type == json::JsonType::String;
                }
                (void)std::snprintf(line, sizeof(line), "argument '%s' must be an object whose members are strings",
                                    s.name);
                break;
            }
            if (!ok)
            {
                reason.append(line);
                return false;
            }
        }
        return true;
    }

    // A checked Text argument into `buf` (cap bytes); an absent optional one leaves `buf` empty.
    void text_of(const json::JsonDoc& doc, crd::u32 args, const char* key, char* buf, crd::u32 cap)
    {
        buf[0] = '\0';
        (void)json::str_value(doc, json::find(doc, args, key), buf, cap);
    }

    [[nodiscard]] crd::f64 number_of(const json::JsonDoc& doc, crd::u32 args, const char* key, crd::f64 def)
    {
        return json::as_f64(doc, json::find(doc, args, key), def);
    }

    // The tool's own refusal: the report shape every verb answers a failure with ({ok:false, verb, reason}).
    [[nodiscard]] String refusal(const char* tool, StringView reason, crd::memory::IAllocator* alloc)
    {
        JsonWriter w(alloc);
        w.begin_object();
        w.kv("ok", false);
        w.kv("verb", tool);
        w.kv("reason", reason);
        w.end_object();
        return String(w.str());
    }

    // ── the tools ───────────────────────────────────────────────────────────────────────────────────────────────────

    String run_cook(const json::JsonDoc& doc, crd::u32 args, crd::memory::IAllocator* alloc,
                    crd::perf::DiagCommandService* /*diag*/)
    {
        char root[kPathBytes];
        char out[kPathBytes];
        text_of(doc, args, "root", root, kPathBytes);
        text_of(doc, args, "out", out, kPathBytes);
        return verb_cook(root, out, alloc);
    }

    // The diag tool: the request's own fields, then the host's service. A count above a u32 bound reaches the service
    // as the largest u32, so the service refuses it as oversized rather than the transport guessing.
    String run_diag(const json::JsonDoc& doc, crd::u32 args, crd::memory::IAllocator* alloc,
                    crd::perf::DiagCommandService* diag)
    {
        crd::perf::DiagRequest request;
        request.command     = string_of(doc, json::find(doc, args, "command"));
        const crd::u32 path = json::find(doc, args, "path");
        if (path != json::kInvalid)
        {
            request.path = string_of(doc, path);
        }
        crd::containers::Array<crd::perf::DiagArg> named(alloc);
        const crd::u32                             object = json::find(doc, args, "args");
        for (crd::u32 m = object == json::kInvalid ? json::kInvalid : doc.nodes[object].child; m != json::kInvalid;
             m = doc.nodes[m].next)
        {
            named.push_back(crd::perf::DiagArg{
                StringView{doc.strings.data() + doc.nodes[m].key_off, doc.nodes[m].key_len}, string_of(doc, m)});
        }
        const auto u32_of = [&](const char* key, crd::f64 def)
        {
            constexpr crd::f64 u32_max = 4294967295.0;
            const crd::f64     v       = number_of(doc, args, key, def);
            return static_cast<crd::u32>(v < u32_max ? v : u32_max);
        };
        request.args           = crd::containers::as_const_span(named);
        request.cursor         = static_cast<crd::u64>(number_of(doc, args, "cursor", 0.0));
        request.page_items     = u32_of("page_items", 0.0);
        request.page_bytes     = u32_of("page_bytes", 0.0);
        request.schema_version = u32_of("schema", static_cast<crd::f64>(crd::perf::kDiagCommandSchemaVersion));
        return verb_diag(*diag, request, alloc);
    }

    String run_export(const json::JsonDoc& doc, crd::u32 args,
                      crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* /*diag*/)
    {
        char timl[kPathBytes];
        char out[kPathBytes];
        text_of(doc, args, "timl", timl, kPathBytes);
        text_of(doc, args, "out", out, kPathBytes);
        return verb_export_timeline(timl, out, alloc);
    }

    String run_import(const json::JsonDoc& doc, crd::u32 args,
                      crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* /*diag*/)
    {
        char path[kPathBytes];
        text_of(doc, args, "path", path, kPathBytes);
        return verb_import(path, alloc);
    }

    String run_instantiate(const json::JsonDoc& doc, crd::u32 args,
                           crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* /*diag*/)
    {
        char pack[kPathBytes];
        char asset[kPathBytes];
        char out[kPathBytes];
        text_of(doc, args, "pack", pack, kPathBytes);
        text_of(doc, args, "asset", asset, kPathBytes);
        text_of(doc, args, "out", out, kPathBytes);
        const crd::f32 translate[3] = {static_cast<crd::f32>(number_of(doc, args, "x", 0.0)),
                                       static_cast<crd::f32>(number_of(doc, args, "y", 0.0)),
                                       static_cast<crd::f32>(number_of(doc, args, "z", 0.0))};
        const bool     dry_run      = json::as_bool(doc, json::find(doc, args, "dry_run"), false);
        return verb_instantiate(pack, asset, translate, dry_run, out, alloc);
    }

    String run_query(const json::JsonDoc& doc, crd::u32 args, crd::memory::IAllocator* alloc,
                     crd::perf::DiagCommandService* /*diag*/)
    {
        char pack[kPathBytes];
        text_of(doc, args, "pack", pack, kPathBytes);
        return verb_query(pack, alloc);
    }

    String run_render(const json::JsonDoc& doc, crd::u32 args,
                      crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* /*diag*/)
    {
        char otio[kPathBytes];
        char dir[kPathBytes];
        text_of(doc, args, "otio", otio, kPathBytes);
        text_of(doc, args, "dir", dir, kPathBytes);
        return verb_render(otio, dir, static_cast<crd::i64>(number_of(doc, args, "max_frames", 0.0)), alloc);
    }

    String run_sequence(const json::JsonDoc& doc, crd::u32 args,
                        crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* /*diag*/)
    {
        char name[kPathBytes];
        char clip_a[kClipBytes];
        char clip_b[kClipBytes];
        char out_timl[kPathBytes];
        char out_otio[kPathBytes];
        text_of(doc, args, "name", name, kPathBytes);
        text_of(doc, args, "clip_a", clip_a, kClipBytes);
        text_of(doc, args, "clip_b", clip_b, kClipBytes);
        text_of(doc, args, "out_timl", out_timl, kPathBytes);
        text_of(doc, args, "out_otio", out_otio, kPathBytes);
        return verb_sequence(name, clip_a, static_cast<crd::i64>(number_of(doc, args, "frames_a", 0.0)), clip_b,
                             static_cast<crd::i64>(number_of(doc, args, "frames_b", 0.0)),
                             static_cast<crd::i64>(number_of(doc, args, "transition", 0.0)), out_timl, out_otio,
                             alloc);
    }

    constexpr ArgSpec kCookArgs[] = {
        {"root", ArgKind::Text, true, kPathBytes, "The source directory to cook"},
        {"out", ArgKind::Text, true, kPathBytes, "The PACK file to write"},
    };
    constexpr ArgSpec kDiagArgs[] = {
        {"command", ArgKind::String, true, 0U, "The command to run (diag.commands lists them)"},
        {"path", ArgKind::String, false, 0U, "A path under the host's diagnostic root, for commands that read one"},
        {"cursor", ArgKind::Count, false, 0U, "A cursor from an earlier page (0: a new snapshot)"},
        {"page_items", ArgKind::Count, false, 0U, "The most items one page holds (0: the service's default)"},
        {"page_bytes", ArgKind::Count, false, 0U, "The most item bytes one page holds (0: the service's default)"},
        {"schema", ArgKind::Count, false, 0U, "The request schema version (1)"},
        {"args", ArgKind::Strings, false, 0U, "The command's named arguments, each a string"},
    };
    constexpr ArgSpec kExportArgs[] = {
        {"timl", ArgKind::Text, true, kPathBytes, "The TIML artifact to convert"},
        {"out", ArgKind::Text, true, kPathBytes, "The .otio file to write"},
    };
    constexpr ArgSpec kImportArgs[] = {
        {"path", ArgKind::Text, true, kPathBytes, "The interchange file to read"},
    };
    constexpr ArgSpec kInstantiateArgs[] = {
        {"pack", ArgKind::Text, true, kPathBytes, "The PACK holding the mesh"},
        {"asset", ArgKind::Text, true, kPathBytes, "The mesh's name in the pack"},
        {"x", ArgKind::Number, false, 0U, "The translation's x (default 0)"},
        {"y", ArgKind::Number, false, 0U, "The translation's y (default 0)"},
        {"z", ArgKind::Number, false, 0U, "The translation's z (default 0)"},
        {"out", ArgKind::Text, true, kPathBytes, "The SCEN file to write"},
        {"dry_run", ArgKind::Flag, false, 0U, "Validate and report without writing (default false)"},
    };
    constexpr ArgSpec kQueryArgs[] = {
        {"pack", ArgKind::Text, true, kPathBytes, "The PACK to list"},
    };
    constexpr ArgSpec kRenderArgs[] = {
        {"otio", ArgKind::Text, true, kPathBytes, "The .otio timeline to render"},
        {"dir", ArgKind::Text, true, kPathBytes, "The directory the EXR frames go to"},
        {"max_frames", ArgKind::Whole, false, 0U, "The most frames to render (0 or absent: all)"},
    };
    constexpr ArgSpec kSequenceArgs[] = {
        {"name", ArgKind::Text, true, kPathBytes, "The timeline's name"},
        {"clip_a", ArgKind::Text, true, kClipBytes, "The first clip's name"},
        {"frames_a", ArgKind::Whole, true, 0U, "The first clip's length in frames"},
        {"clip_b", ArgKind::Text, true, kClipBytes, "The second clip's name"},
        {"frames_b", ArgKind::Whole, true, 0U, "The second clip's length in frames"},
        {"transition", ArgKind::Whole, false, 0U, "The centered dissolve's length in frames (default 0)"},
        {"out_timl", ArgKind::Text, true, kPathBytes, "The TIML artifact to write"},
        {"out_otio", ArgKind::Text, true, kPathBytes, "The .otio interchange twin to write"},
    };

    // The reports' JSON Schemas (each verb's success shape; every tool also answers the refusal shape). Keywords are
    // limited to type, properties, required, additionalProperties, items, const, enum, minimum and anyOf, which the
    // conformance oracle (scripts/jsonschema_subset.py) checks.
    constexpr const char* kCookReport =
        R"({"type":"object","properties":{"verb":{"const":"cook"},"ok":{"type":"boolean"},"pack":{"type":"string"},)"
        R"("exit_code":{"type":"integer"}},"required":["verb","ok","pack","exit_code"],"additionalProperties":false})";
    constexpr const char* kDiagReport =
        R"({"type":"object","properties":{"schema":{"type":"integer","minimum":0},"command":{"type":"string"},)"
        R"("ok":{"type":"boolean"},"status":{"enum":["ok","unsupported-schema","unknown-command","unauthorized",)"
        R"("oversized","bad-argument","stale-cursor","cancelled","unavailable","failed"]},"reason":{"type":"string"},)"
        R"("grant":{"type":"string"},"generation":{"type":"integer","minimum":0},"cursor":{"type":"integer",)"
        R"("minimum":0},"next_cursor":{"type":"integer","minimum":0},"total":{"type":"integer","minimum":0},)"
        R"("page_items":{"type":"integer","minimum":0},"dropped":{"type":"integer","minimum":0},)"
        R"("complete":{"type":"boolean"},"byte_bounded":{"type":"boolean"},)"
        R"("summary":{"type":"object"},"items":{"type":"array","items":{"type":"object"}}},)"
        R"("required":["schema","command","ok","status","reason","grant","generation","cursor","next_cursor","total",)"
        R"("page_items","dropped","complete","byte_bounded","summary","items"],"additionalProperties":false})";
    constexpr const char* kExportReport =
        R"({"type":"object","properties":{"verb":{"const":"export"},"ok":{"const":true},"otio":{"type":"string"}},)"
        R"("required":["verb","ok","otio"],"additionalProperties":false})";
    constexpr const char* kImportReport =
        R"({"anyOf":[)"
        R"({"type":"object","properties":{"verb":{"const":"import"},"path":{"type":"string"},"ok":{"const":true},)"
        R"("format":{"const":"mesh-asset"},"meshes":{"type":"integer","minimum":0},"nodes":{"type":"integer",)"
        R"("minimum":0},"materials":{"type":"integer","minimum":0},"skins":{"type":"integer","minimum":0},)"
        R"("animations":{"type":"integer","minimum":0},"mesh_names":{"type":"array","items":{"type":"string"}}},)"
        R"("required":["verb","path","ok","format",)"
        R"("meshes","nodes","materials","skins","animations","mesh_names"],"additionalProperties":false},)"
        R"({"type":"object","properties":{"verb":{"const":"import"},"path":{"type":"string"},"ok":{"const":true},)"
        R"("format":{"const":"timeline"},"name":{"type":"string"},"tracks":{"type":"integer","minimum":0},)"
        R"("items":{"type":"integer","minimum":0},"markers":{"type":"integer","minimum":0}},"required":["verb","path",)"
        R"("ok","format",)"
        R"("name","tracks","items","markers"],"additionalProperties":false},)"
        R"({"type":"object","properties":{"verb":{"const":"import"},"path":{"type":"string"},"ok":{"const":true},)"
        R"("format":{"const":"audio"},"sample_rate":{"type":"integer","minimum":0},"channels":{"type":"integer",)"
        R"("minimum":0},"frames":{"type":"integer","minimum":0}},"required":["verb","path","ok","format",)"
        R"("sample_rate","channels",)"
        R"("frames"],"additionalProperties":false},)"
        R"({"type":"object","properties":{"verb":{"const":"import"},"path":{"type":"string"},"ok":{"const":true},)"
        R"("format":{"const":"midi"},"notes":{"type":"integer","minimum":0},"tempo_changes":{"type":"integer",)"
        R"("minimum":0}},"required":["verb","path","ok","format","notes","tempo_changes"],)"
        R"("additionalProperties":false}]})";
    constexpr const char* kInstantiateReport =
        R"({"anyOf":[)"
        R"({"type":"object","properties":{"verb":{"const":"instantiate"},"ok":{"const":true},)"
        R"("mesh_uuid":{"type":"string"},"applied":{"const":false},"would_apply":{"const":true}},)"
        R"("required":["verb","ok","mesh_uuid","applied","would_apply"],"additionalProperties":false},)"
        R"({"type":"object","properties":{"verb":{"const":"instantiate"},"ok":{"const":true},)"
        R"("mesh_uuid":{"type":"string"},"applied":{"const":true},"scene":{"type":"string"},"entities":{"const":1}},)"
        R"("required":["verb","ok","mesh_uuid","applied","scene","entities"],"additionalProperties":false}]})";
    constexpr const char* kQueryReport =
        R"({"type":"object","properties":{"verb":{"const":"query"},"ok":{"const":true},"pack":{"type":"string"},)"
        R"("count":{"type":"integer","minimum":0},"entries":{"type":"array","items":{"type":"object","properties":{)"
        R"("uuid":{"type":"string"},"type":{"type":"string"},"name":{"type":"string"}},)"
        R"("required":["uuid","type","name"],"additionalProperties":false}}},)"
        R"("required":["verb","ok","pack","count","entries"],"additionalProperties":false})";
    constexpr const char* kRenderReport =
        R"({"type":"object","properties":{"verb":{"const":"render"},"ok":{"type":"boolean"},)"
        R"("frames":{"type":"integer"},"dir":{"type":"string"},"reason":{"type":"string"}},)"
        R"("required":["verb","ok","frames","dir"],"additionalProperties":false})";
    constexpr const char* kSequenceReport =
        R"({"type":"object","properties":{"verb":{"const":"sequence"},"ok":{"const":true},"timl":{"type":"string"},)"
        R"("otio":{"type":"string"},"duration_frames":{"type":"integer"}},)"
        R"("required":["verb","ok","timl","otio","duration_frames"],"additionalProperties":false})";

    // In name order: tools/list answers this order (a deterministic list, as 2026-07-28 recommends), checked below.
    constexpr ToolSpec kTools[] = {
        {"cook", "Cook a source root", "Run the incremental asset processor over a source root into a PACK", kCookArgs,
         kCookReport, false, true, &run_cook},
        {"diag", "Run a diagnostic command",
         "Run one typed, bounded diagnostic command (diag.commands lists them); pages are cut from one retained "
         "snapshot by cursor",
         kDiagArgs, kDiagReport, false, true, &run_diag},
        {"export", "Export a timeline", "Convert a TIML artifact to .otio interchange", kExportArgs, kExportReport,
         false, true, &run_export},
        {"import", "Import a file",
         "Parse an interchange file (.glb/.gltf/.stl/.obj/.ply/.3mf/.otio/.wav/.aiff/.flac/.mid) and report its "
         "contents",
         kImportArgs, kImportReport, true, false, &run_import},
        {"instantiate", "Instance a mesh",
         "Compose a scene: instance a named mesh from a pack at a translation; transactional; dry_run supported",
         kInstantiateArgs, kInstantiateReport, false, true, &run_instantiate},
        {"query", "List a pack", "List a PACK's manifest: every uuid, type, and name", kQueryArgs, kQueryReport, true,
         false, &run_query},
        {"render", "Render a timeline", "Render a .otio timeline to an EXR frame sequence", kRenderArgs, kRenderReport,
         false, true, &run_render},
        {"sequence", "Author a two-shot timeline",
         "Author a 2-shot timeline (two clips + centered dissolve) as TIML + .otio", kSequenceArgs, kSequenceReport,
         false, true, &run_sequence},
    };

    [[nodiscard]] constexpr bool name_before(const char* a, const char* b) noexcept
    {
        while (*a != '\0' && *a == *b)
        {
            ++a;
            ++b;
        }
        return static_cast<unsigned char>(*a) < static_cast<unsigned char>(*b);
    }

    // The 2025-11-25 tool-name guidance: 1 to 128 of A-Z, a-z, 0-9, '_', '-' and '.'.
    [[nodiscard]] constexpr bool name_follows_guidance(const char* name) noexcept
    {
        crd::usize n = 0U;
        for (; name[n] != '\0'; ++n)
        {
            const char c = name[n];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.'))
            {
                return false;
            }
        }
        return n >= 1U && n <= 128U;
    }

    [[nodiscard]] constexpr bool tools_well_formed() noexcept
    {
        for (crd::usize i = 0U; i < sizeof(kTools) / sizeof(kTools[0]); ++i)
        {
            if (!name_follows_guidance(kTools[i].name) || (i > 0U && !name_before(kTools[i - 1U].name, kTools[i].name)))
            {
                return false;
            }
        }
        return true;
    }
    static_assert(tools_well_formed(), "tool names follow the 2025-11-25 guidance and are listed in strict name order");

    // ── writing ─────────────────────────────────────────────────────────────────────────────────────────────────────

    void write_server_info(JsonWriter& w)
    {
        w.begin_object();
        w.kv("name", "ceridc");
        w.kv("title", "Cerid command line");
        w.kv("version", CRD_VERSION_STRING);
        w.end_object();
    }

    void write_capabilities(JsonWriter& w)
    {
        w.begin_object();
        w.key("tools");
        w.begin_object();
        w.kv("listChanged", false); // the tool set never changes while the process runs
        w.end_object();
        w.end_object();
    }

    void write_input_schema(JsonWriter& w, crd::containers::ConstSpan<ArgSpec> args)
    {
        w.begin_object();
        w.kv("type", "object");
        w.key("properties");
        w.begin_object();
        for (const ArgSpec& a : args)
        {
            w.key(a.name);
            w.begin_object();
            switch (a.kind)
            {
            case ArgKind::Text:
                w.kv("type", "string");
                w.kv("minLength", crd::u64{1});
                w.kv("maxLength", crd::u64{a.cap - 1U}); // code points; the byte bound is in the description
                break;
            case ArgKind::String:
                w.kv("type", "string");
                break;
            case ArgKind::Number:
                w.kv("type", "number");
                w.kv_f64_exact("minimum", -static_cast<crd::f64>(FLT_MAX));
                w.kv_f64_exact("maximum", static_cast<crd::f64>(FLT_MAX));
                break;
            case ArgKind::Whole:
                w.kv("type", "integer");
                w.kv("minimum", static_cast<crd::i64>(-kMaxExactWhole));
                w.kv("maximum", static_cast<crd::i64>(kMaxExactWhole));
                break;
            case ArgKind::Count:
                w.kv("type", "integer");
                w.kv("minimum", crd::u64{0});
                w.kv("maximum", static_cast<crd::u64>(kMaxExactWhole));
                break;
            case ArgKind::Flag:
                w.kv("type", "boolean");
                break;
            case ArgKind::Strings:
                w.kv("type", "object");
                w.key("additionalProperties");
                w.begin_object();
                w.kv("type", "string");
                w.end_object();
                break;
            }
            if (a.kind == ArgKind::Text)
            {
                char text[256];
                (void)std::snprintf(text, sizeof(text), "%s (1 to %u UTF-8 bytes, no NUL)", a.description,
                                    a.cap - 1U);
                w.kv("description", text);
            }
            else
            {
                w.kv("description", a.description);
            }
            w.end_object();
        }
        w.end_object();
        w.key("required");
        w.begin_array();
        for (const ArgSpec& a : args)
        {
            if (a.required)
            {
                w.value_string(a.name);
            }
        }
        w.end_array();
        w.kv("additionalProperties", false); // an unknown argument is refused, never ignored
        w.end_object();
    }

    // The report schema: the verb's success shape or its refusal.
    void write_output_schema(JsonWriter& w, const ToolSpec& tool)
    {
        w.begin_object();
        w.kv("type", "object");
        w.key("anyOf");
        w.begin_array();
        w.value_raw(StringView{tool.output});
        w.begin_object();
        w.kv("type", "object");
        w.key("properties");
        w.begin_object();
        w.key("ok");
        w.begin_object();
        w.kv("const", false);
        w.end_object();
        w.key("verb");
        w.begin_object();
        w.kv("const", tool.name);
        w.end_object();
        w.key("reason");
        w.begin_object();
        w.kv("type", "string");
        w.end_object();
        w.end_object();
        w.key("required");
        w.begin_array();
        w.value_string("ok");
        w.value_string("verb");
        w.value_string("reason");
        w.end_array();
        w.kv("additionalProperties", false);
        w.end_object();
        w.end_array();
        w.end_object();
    }

    void write_tool(JsonWriter& w, const ToolSpec& tool)
    {
        w.begin_object();
        w.kv("name", tool.name);
        w.kv("title", tool.title);
        w.kv("description", tool.description);
        w.key("inputSchema");
        write_input_schema(w, tool.args);
        w.key("outputSchema");
        write_output_schema(w, tool);
        w.key("annotations");
        w.begin_object();
        w.kv("readOnlyHint", tool.read_only);
        if (!tool.read_only)
        {
            w.kv("destructiveHint", tool.destructive);
        }
        w.kv("openWorldHint", false); // the host's own files only
        w.end_object();
        w.end_object();
    }

    enum class Era : crd::u8
    {
        Modern, // 2026-07-28: per-request _meta, resultType, _meta serverInfo, caching hints
        Legacy, // 2025-11-25: after the initialize handshake
    };

    // One reply: the request's id echoed exactly (none when it cannot be read), the result framed for its era.
    class Reply
    {
    public:
        Reply(const json::JsonDoc& doc, crd::u32 id, crd::memory::IAllocator* alloc)
            : m_doc(doc), m_id(id), m_writer(alloc)
        {
        }

        [[nodiscard]] JsonWriter& w() noexcept { return m_writer; }

        [[nodiscard]] String error(crd::i64 code, StringView message)
        {
            begin_error(code, message);
            return end_error();
        }

        void begin_error(crd::i64 code, StringView message)
        {
            begin();
            m_writer.key("error");
            m_writer.begin_object();
            m_writer.kv("code", code);
            m_writer.kv("message", message);
        }

        [[nodiscard]] String end_error()
        {
            m_writer.end_object();
            m_writer.end_object();
            return String(m_writer.str());
        }

        void begin_result(Era era)
        {
            begin();
            m_writer.key("result");
            m_writer.begin_object();
            if (era == Era::Modern)
            {
                m_writer.kv("resultType", "complete");
            }
        }

        [[nodiscard]] String end_result(Era era)
        {
            if (era == Era::Modern)
            {
                m_writer.key("_meta");
                m_writer.begin_object();
                m_writer.key(kMetaServerInfo);
                write_server_info(m_writer);
                m_writer.end_object();
            }
            m_writer.end_object();
            m_writer.end_object();
            return String(m_writer.str());
        }

        void cacheable()
        {
            m_writer.kv("ttlMs", kListTtlMs);
            m_writer.kv("cacheScope", "public");
        }

    private:
        void begin()
        {
            m_writer.begin_object();
            m_writer.kv("jsonrpc", "2.0");
            if (m_id == json::kInvalid)
            {
                return; // an id that cannot be read is left out (MCP's error response; JSON-RPC 2.0 would write null)
            }
            const json::JsonNode& id = m_doc.nodes[m_id];
            m_writer.key("id");
            if (id.type == json::JsonType::String)
            {
                m_writer.value_string(string_of(m_doc, m_id));
            }
            else
            {
                m_writer.value_i64(static_cast<crd::i64>(id.number));
            }
        }

        const json::JsonDoc& m_doc;
        crd::u32             m_id;
        JsonWriter           m_writer;
    };

    [[nodiscard]] bool is_object(const json::JsonDoc& doc, crd::u32 node) noexcept
    {
        return node != json::kInvalid && doc.nodes[node].type == json::JsonType::Object;
    }

    [[nodiscard]] bool is_string(const json::JsonDoc& doc, crd::u32 node) noexcept
    {
        return node != json::kInvalid && doc.nodes[node].type == json::JsonType::String;
    }

    // An Implementation (clientInfo): an object with a string name and a string version.
    [[nodiscard]] bool is_implementation(const json::JsonDoc& doc, crd::u32 node) noexcept
    {
        return is_object(doc, node) && is_string(doc, json::find(doc, node, "name")) &&
               is_string(doc, json::find(doc, node, "version"));
    }

    [[nodiscard]] bool is_log_level(const json::JsonDoc& doc, crd::u32 node) noexcept
    {
        constexpr const char* levels[] = {"debug", "info", "notice", "warning", "error", "critical", "alert",
                                          "emergency"};
        return std::ranges::any_of(levels,
                                   [&doc, node](const char* level)
                                   {
                                       return json::str_value_eq(doc, node, level);
                                   });
    }

    [[nodiscard]] String tools_list(Reply& reply, Era era, const json::JsonDoc& doc, crd::u32 params,
                                    const crd::perf::DiagCommandService* diag)
    {
        // The list is one page and issues no cursor, so any cursor names no page of it.
        if (json::find(doc, params, "cursor") != json::kInvalid)
        {
            return reply.error(kInvalidParams, "unknown cursor: tools/list answers one page");
        }
        reply.begin_result(era);
        JsonWriter& w = reply.w();
        w.key("tools");
        w.begin_array();
        for (const ToolSpec& tool : kTools)
        {
            if (tool.run != &run_diag || diag != nullptr)
            {
                write_tool(w, tool);
            }
        }
        w.end_array();
        if (era == Era::Modern)
        {
            reply.cacheable();
        }
        return reply.end_result(era);
    }

    [[nodiscard]] String tools_call(Reply& reply, Era era, const json::JsonDoc& doc, crd::u32 params,
                                    crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* diag)
    {
        const crd::u32 name = json::find(doc, params, "name");
        if (!is_string(doc, name))
        {
            return reply.error(kInvalidParams, "tools/call needs the tool's name as a string");
        }
        const ToolSpec* tool = nullptr;
        for (const ToolSpec& t : kTools)
        {
            // Names compare whole (DIAG.10a): "import\u0000x" names no tool, it is not "import".
            if (json::str_value_eq(doc, name, t.name) && (t.run != &run_diag || diag != nullptr))
            {
                tool = &t;
            }
        }
        if (tool == nullptr)
        {
            String message("unknown tool: ", alloc);
            message.append(string_of(doc, name));
            return reply.error(kInvalidParams, StringView{message.data(), message.size()});
        }
        const crd::u32 args = json::find(doc, params, "arguments");
        if (args != json::kInvalid && doc.nodes[args].type != json::JsonType::Object)
        {
            return reply.error(kInvalidParams, "tools/call arguments must be an object");
        }

        String       reason(alloc);
        const String report = check_arguments(doc, args, tool->args, reason)
                                  ? tool->run(doc, args, alloc, diag)
                                  : refusal(tool->name, StringView{reason.data(), reason.size()}, alloc);
        json::JsonDoc parsed(alloc);
        const bool    read = json::parse({reinterpret_cast<const crd::u8*>(report.data()), report.size()}, parsed);
        const crd::u32 ok_node = read ? json::find(parsed, parsed.root, "ok") : json::kInvalid;
        if (ok_node == json::kInvalid || parsed.nodes[ok_node].type != json::JsonType::Bool)
        {
            // Every verb answers an object with a boolean ok (find answers kInvalid when the root is not an object);
            // anything else is this server's fault, not the call's.
            return reply.error(kInternalError, "the tool's report is not a JSON object with a boolean ok");
        }
        const bool ok = parsed.nodes[ok_node].boolean;

        reply.begin_result(era);
        JsonWriter& w = reply.w();
        w.key("content");
        w.begin_array();
        w.begin_object();
        w.kv("type", "text");
        w.kv("text", StringView{report.data(), report.size()}); // the serialized report, for clients without
        w.end_object();                                          // structured content
        w.end_array();
        w.key("structuredContent");
        w.value_raw(StringView{report.data(), report.size()});
        w.kv("isError", !ok);
        return reply.end_result(era);
    }
} // namespace

McpServer::McpServer(crd::memory::IAllocator* alloc, crd::perf::DiagCommandService* diag) noexcept
    : m_alloc(alloc), m_diag(diag)
{
}

crd::containers::String McpServer::handle(crd::containers::ConstSpan<crd::u8> message)
{
    json::JsonDoc doc(m_alloc);
    if (!json::parse(message, doc) || doc.root == json::kInvalid)
    {
        return Reply(doc, json::kInvalid, m_alloc).error(kParseError, "parse error");
    }
    // DIAG.10a: a message is an object (a batch array or a bare value is an invalid request, never a silent drop), and
    // its reply echoes its id exactly: a string as the same string, an integer as the same integer. Any other id (null,
    // a fraction, a number past 2^53 - 1 that a double cannot hold exactly, an array or object) cannot be echoed and is
    // an invalid request.
    if (doc.nodes[doc.root].type != json::JsonType::Object)
    {
        return Reply(doc, json::kInvalid, m_alloc).error(kInvalidRequest, "invalid request: not an object");
    }
    const crd::u32 id_node = json::find(doc, doc.root, "id");
    if (id_node == json::kInvalid) // a notification (notifications/initialized, notifications/cancelled): no reply
    {
        return String("", m_alloc);
    }
    const json::JsonNode& id = doc.nodes[id_node];
    const bool whole_id = id.type == json::JsonType::Number && is_whole(id.number, -kMaxExactWhole);
    if (id.type != json::JsonType::String && !whole_id)
    {
        return Reply(doc, json::kInvalid, m_alloc).error(kInvalidRequest, "invalid request: the id is not a string or "
                                                                          "an integer");
    }
    Reply          reply(doc, id_node, m_alloc);
    const crd::u32 method = json::find(doc, doc.root, "method");
    if (!json::str_value_eq(doc, json::find(doc, doc.root, "jsonrpc"), "2.0") || !is_string(doc, method))
    {
        return reply.error(kInvalidRequest, "invalid request: a JSON-RPC 2.0 request has jsonrpc \"2.0\" and a "
                                            "string method");
    }
    // The method is compared whole: "tools/call\u0000x" is not "tools/call".
    const auto method_is = [&doc, method](const char* m)
    {
        return json::str_value_eq(doc, method, m);
    };
    const crd::u32 params = json::find(doc, doc.root, "params");
    if (params != json::kInvalid && doc.nodes[params].type != json::JsonType::Object)
    {
        return reply.error(kInvalidParams, "params must be an object");
    }

    // The legacy handshake: it selects 2025-11-25 for this process (the stdio transport's scope) and names that
    // revision whatever the client proposed, as a 2025-11-25 server does when it does not speak the proposal.
    if (method_is("initialize"))
    {
        if (!is_string(doc, json::find(doc, params, "protocolVersion")) ||
            !is_object(doc, json::find(doc, params, "capabilities")) ||
            !is_implementation(doc, json::find(doc, params, "clientInfo")))
        {
            return reply.error(kInvalidParams, "initialize needs protocolVersion, capabilities and clientInfo (name "
                                               "and version)");
        }
        m_legacy = true;
        reply.begin_result(Era::Legacy);
        JsonWriter& w = reply.w();
        w.kv("protocolVersion", kLegacyVersion);
        w.key("capabilities");
        write_capabilities(w);
        w.key("serverInfo");
        write_server_info(w);
        w.kv("instructions", kInstructions);
        return reply.end_result(Era::Legacy);
    }

    const crd::u32 meta = json::find(doc, params, "_meta");
    if (meta != json::kInvalid && doc.nodes[meta].type != json::JsonType::Object)
    {
        return reply.error(kInvalidParams, "_meta must be an object");
    }
    const crd::u32 version = json::find(doc, meta, kMetaProtocolVersion);
    if (version == json::kInvalid)
    {
        // No per-request version: a 2025-11-25 request after the handshake (ping is served before it too, as that
        // revision allows), otherwise a malformed 2026-07-28 request.
        if (!m_legacy && !method_is("ping"))
        {
            return reply.error(kInvalidParams, "the request's params._meta needs io.modelcontextprotocol/"
                                               "protocolVersion and clientCapabilities (revision 2026-07-28), or an "
                                               "initialize handshake first (revision 2025-11-25)");
        }
        if (method_is("ping"))
        {
            reply.begin_result(Era::Legacy);
            return reply.end_result(Era::Legacy);
        }
        if (method_is("tools/list"))
        {
            return tools_list(reply, Era::Legacy, doc, params, m_diag);
        }
        if (method_is("tools/call"))
        {
            return tools_call(reply, Era::Legacy, doc, params, m_alloc, m_diag);
        }
        return reply.error(kMethodNotFound, "method not found");
    }

    // 2026-07-28: the request carries its version and the client's capabilities; nothing earlier is assumed.
    if (!is_string(doc, version))
    {
        return reply.error(kInvalidParams, "io.modelcontextprotocol/protocolVersion must be a string");
    }
    if (json::str_value_eq(doc, version, kLegacyVersion))
    {
        return reply.error(kInvalidParams, "revision 2025-11-25 is served after an initialize handshake, not by "
                                           "per-request _meta");
    }
    if (!json::str_value_eq(doc, version, kModernVersion))
    {
        reply.begin_error(kUnsupportedProtocolVersion, "Unsupported protocol version");
        JsonWriter& w = reply.w();
        w.key("data");
        w.begin_object();
        w.key("supported");
        w.begin_array();
        w.value_string(kModernVersion);
        w.value_string(kLegacyVersion);
        w.end_array();
        w.kv("requested", string_of(doc, version));
        w.end_object();
        return reply.end_error();
    }
    const crd::u32 info  = json::find(doc, meta, kMetaClientInfo);
    const crd::u32 level = json::find(doc, meta, kMetaLogLevel);
    if (!is_object(doc, json::find(doc, meta, kMetaClientCapabilities)) ||
        (info != json::kInvalid && !is_implementation(doc, info)) ||
        (level != json::kInvalid && !is_log_level(doc, level)))
    {
        return reply.error(kInvalidParams, "_meta needs io.modelcontextprotocol/clientCapabilities as an object; "
                                           "clientInfo, when sent, names the client and its version, and logLevel "
                                           "is a logging level");
    }
    if (method_is("server/discover"))
    {
        reply.begin_result(Era::Modern);
        JsonWriter& w = reply.w();
        w.key("supportedVersions");
        w.begin_array();
        w.value_string(kModernVersion);
        w.value_string(kLegacyVersion);
        w.end_array();
        w.key("capabilities");
        write_capabilities(w);
        w.kv("instructions", kInstructions);
        reply.cacheable();
        return reply.end_result(Era::Modern);
    }
    if (method_is("tools/list"))
    {
        return tools_list(reply, Era::Modern, doc, params, m_diag);
    }
    if (method_is("tools/call"))
    {
        return tools_call(reply, Era::Modern, doc, params, m_alloc, m_diag);
    }
    return reply.error(kMethodNotFound, "method not found");
}

} // namespace crd::ceridc
