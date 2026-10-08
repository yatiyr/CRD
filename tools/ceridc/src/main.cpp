// main.cpp — GEO-11: `ceridc` — the agent-facing CLI (every verb prints its JSON report to stdout, exit code
// mirrors the report's `ok`) and the MCP stdio loop (`ceridc mcp`: newline-delimited JSON-RPC, one line in →
// one line out — the transport shell over mcp_handle).

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_commands.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::GrowableTlsfAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] const char* flag_of(int argc, char** argv, const char* name, const char* def)
{
    for (int i = 2; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            return argv[i + 1];
        }
    }
    return def;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            return true;
        }
    }
    return false;
}

// Every value of a repeatable flag (`--break 7 --break 9`), in order.
void flags_of(int argc, char** argv, const char* name, crd::containers::Array<const char*>& out)
{
    for (int i = 2; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], name) == 0)
        {
            out.push_back(argv[i + 1]);
            ++i;
        }
    }
}

[[nodiscard]] int emit(const crd::containers::String& report)
{
    std::printf("%s\n", report.c_str());
    return std::strstr(report.c_str(), "\"ok\":true") != nullptr ? 0 : 1;
}

void print_usage()
{
    std::printf(
        "ceridc — the Cerid agent surface (every verb emits a JSON report)\n"
        "  ceridc import <file>\n"
        "  ceridc cook --root <dir> --out <pack.crdr>\n"
        "  ceridc query --pack <pack.crdr>\n"
        "  ceridc instantiate --pack <p> --asset <name> [--x --y --z] --out <scene.scen> [--dry-run]\n"
        "  ceridc sequence --name <n> --clip-a <n> --frames-a <n> --clip-b <n> --frames-b <n>\n"
        "                  [--transition <frames>] --out-timl <f> --out-otio <f>\n"
        "  ceridc render --otio <f> --dir <d> [--max-frames <n>]\n"
        "  ceridc export --timl <f> --out <f.otio>\n"
        "  ceridc inspect --program <f.ceir> [--entry <name>] [--arg <i64>]... [--break <line>]...\n"
        "                 [--watch <line>]... [--step continue|into|over|out|cancel]... [--max-stops <n>]\n"
        "                 [--record <f.crpl>]  (a new run record of the inspected run; never overwrites)\n"
        "                 [--seed <u64>]  (the run's host random streams; without it a draw fails)\n"
        "                 [--clock wall] [--sim-time <ns>] [--sim-step <ns>]  (the run's time domains)\n"
        "                 [--events <list>]  (the run's input event queue, as replay.record's events)\n"
        "  ceridc diag --command <name> [--path <rel>] [--param <name>=<value>]... [--cursor <n>]\n"
        "              [--page-items <n>] [--page-bytes <n>] [--schema <n>] [--grant <list>] [--root <dir>]\n"
        "              (grant defaults to read; program.inspect needs execute; replay.record needs execute,record\n"
        "              and runs on the host provider with --param executor=host)\n"
        "  ceridc mcp [--diag-grant <list>] [--diag-root <dir>]\n"
        "             (JSON-RPC 2.0 over stdio, one message per line; the diag tool serves the grant, default read)\n");
}

int run_mcp_loop(crd::perf::DiagCommandService& diag)
{
    // newline-delimited JSON-RPC: read a line, handle, answer (a growing buffer — requests can be long)
    constexpr crd::usize k_cap = 1U << 20U;
    auto* line = static_cast<char*>(g_alloc.allocate(k_cap, 16));
    while (std::fgets(line, static_cast<int>(k_cap), stdin) != nullptr)
    {
        const crd::usize len = std::strlen(line);
        if (len == 0)
        {
            continue;
        }
        const crd::containers::String response = crd::ceridc::mcp_handle(
            {reinterpret_cast<const crd::u8*>(line), len}, &g_alloc, &diag);
        crd::jobs::frame_reset(); // every job the request ran was joined before it answered
        if (!response.empty())
        {
            std::printf("%s\n", response.c_str());
            (void)std::fflush(stdout);
        }
    }
    g_alloc.deallocate(line);
    return 0;
}

// The crd::jobs pool for the length of one diagnostic verb. The host executor of the replay commands runs programs on
// the host provider, whose pool the process owns, on this (the main) thread; the other verbs start no workers. The
// provider's parallel ranges take their job arrays from the calling thread's frame arena, so the MCP loop resets the
// arenas after every request (every job of it was joined by then).
class ScopedJobPool
{
public:
    ScopedJobPool() { crd::jobs::init(crd::jobs::Config{.num_threads = 4U, .frame_alloc_bytes = 16U << 20U}); }
    ~ScopedJobPool() { crd::jobs::shutdown(); }
    ScopedJobPool(const ScopedJobPool&)            = delete;
    ScopedJobPool& operator=(const ScopedJobPool&) = delete;
    ScopedJobPool(ScopedJobPool&&)                 = delete;
    ScopedJobPool& operator=(ScopedJobPool&&)      = delete;
};

} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        print_usage();
        return 1;
    }
    crd::cooker::register_builtin_handlers(); // cook + every import format the processor speaks

    const char* verb = argv[1];
    if (std::strcmp(verb, "mcp") == 0)
    {
        const ScopedJobPool pool;
        // The diag tool's authority is this process's start-up decision; no request can change it.
        crd::perf::DiagAuthoritySet grant = 0U;
        if (!crd::perf::parse_authority_list(flag_of(argc, argv, "--diag-grant", "read"), grant))
        {
            std::fprintf(stderr, "ceridc mcp: --diag-grant must be 'none' or a comma-separated authority list\n");
            return 2;
        }
        crd::perf::DiagServiceConfig config;
        const char*                  root = flag_of(argc, argv, "--diag-root", nullptr);
        if (root != nullptr)
        {
            config.root = root;
        }
        crd::perf::DiagCommandService diag(grant, config, &g_alloc);
        if (!crd::ceridc::bind_diag_commands(diag))
        {
            std::fprintf(stderr, "ceridc mcp: the diagnostic commands could not be registered\n");
            return 2;
        }
        return run_mcp_loop(diag);
    }
    if (std::strcmp(verb, "diag") == 0)
    {
        const ScopedJobPool pool;
        crd::perf::DiagRequest request;
        const char*            command = flag_of(argc, argv, "--command", nullptr);
        const char*            path    = flag_of(argc, argv, "--path", nullptr);
        request.command                = command != nullptr ? command : "";
        request.path                   = path != nullptr ? path : "";
        request.cursor                 = std::strtoull(flag_of(argc, argv, "--cursor", "0"), nullptr, 10);
        request.page_items = static_cast<crd::u32>(std::strtoul(flag_of(argc, argv, "--page-items", "0"), nullptr, 10));
        request.page_bytes = static_cast<crd::u32>(std::strtoul(flag_of(argc, argv, "--page-bytes", "0"), nullptr, 10));
        request.schema_version =
            static_cast<crd::u32>(std::strtoul(flag_of(argc, argv, "--schema", "1"), nullptr, 10));
        // Each --param is one named argument, split at its first '='; one without '=' is a name with an empty value.
        crd::containers::Array<const char*>        params(&g_alloc);
        crd::containers::Array<crd::perf::DiagArg> named(&g_alloc);
        flags_of(argc, argv, "--param", params);
        for (const char* p : params)
        {
            const crd::containers::StringView text(p);
            const crd::usize                  eq = text.find('=');
            if (eq == crd::containers::StringView::npos)
            {
                named.push_back(crd::perf::DiagArg{text, crd::containers::StringView{}});
            }
            else
            {
                named.push_back(crd::perf::DiagArg{text.substr(0U, eq), text.substr(eq + 1U)});
            }
        }
        request.args = crd::containers::as_const_span(named);
        crd::ceridc::DiagHostOptions host;
        host.grant = flag_of(argc, argv, "--grant", nullptr);
        host.root  = flag_of(argc, argv, "--root", nullptr);
        return emit(crd::ceridc::verb_diag_host(host, request, &g_alloc));
    }
    if (std::strcmp(verb, "import") == 0 && argc >= 3)
    {
        return emit(crd::ceridc::verb_import(argv[2], &g_alloc));
    }
    if (std::strcmp(verb, "cook") == 0)
    {
        return emit(crd::ceridc::verb_cook(flag_of(argc, argv, "--root", nullptr),
                                           flag_of(argc, argv, "--out", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "query") == 0)
    {
        return emit(crd::ceridc::verb_query(flag_of(argc, argv, "--pack", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "instantiate") == 0)
    {
        const crd::f32 translate[3] = {static_cast<crd::f32>(std::atof(flag_of(argc, argv, "--x", "0"))),
                                       static_cast<crd::f32>(std::atof(flag_of(argc, argv, "--y", "0"))),
                                       static_cast<crd::f32>(std::atof(flag_of(argc, argv, "--z", "0")))};
        return emit(crd::ceridc::verb_instantiate(
            flag_of(argc, argv, "--pack", nullptr), flag_of(argc, argv, "--asset", nullptr), translate,
            has_flag(argc, argv, "--dry-run"), flag_of(argc, argv, "--out", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "sequence") == 0)
    {
        return emit(crd::ceridc::verb_sequence(
            flag_of(argc, argv, "--name", "sequence"), flag_of(argc, argv, "--clip-a", nullptr),
            std::atoll(flag_of(argc, argv, "--frames-a", "0")), flag_of(argc, argv, "--clip-b", nullptr),
            std::atoll(flag_of(argc, argv, "--frames-b", "0")),
            std::atoll(flag_of(argc, argv, "--transition", "0")), flag_of(argc, argv, "--out-timl", nullptr),
            flag_of(argc, argv, "--out-otio", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "render") == 0)
    {
        return emit(crd::ceridc::verb_render(flag_of(argc, argv, "--otio", nullptr),
                                             flag_of(argc, argv, "--dir", nullptr),
                                             std::atoll(flag_of(argc, argv, "--max-frames", "0")), &g_alloc));
    }
    if (std::strcmp(verb, "export") == 0)
    {
        return emit(crd::ceridc::verb_export_timeline(flag_of(argc, argv, "--timl", nullptr),
                                                      flag_of(argc, argv, "--out", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "inspect") == 0)
    {
        crd::containers::Array<const char*> raw(&g_alloc);
        crd::containers::Array<crd::i64>    args(&g_alloc);
        crd::containers::Array<crd::u32>    breaks(&g_alloc);
        crd::containers::Array<crd::u32>    watches(&g_alloc);
        crd::containers::Array<const char*> steps(&g_alloc);
        flags_of(argc, argv, "--arg", raw);
        for (const char* a : raw)
        {
            args.push_back(std::atoll(a));
        }
        raw.clear();
        flags_of(argc, argv, "--break", raw);
        for (const char* b : raw)
        {
            breaks.push_back(static_cast<crd::u32>(std::strtoul(b, nullptr, 10)));
        }
        raw.clear();
        flags_of(argc, argv, "--watch", raw);
        for (const char* v : raw)
        {
            watches.push_back(static_cast<crd::u32>(std::strtoul(v, nullptr, 10)));
        }
        flags_of(argc, argv, "--step", steps);
        const crd::ceridc::InspectClockFlags clock{flag_of(argc, argv, "--clock", nullptr),
                                                   flag_of(argc, argv, "--sim-time", nullptr),
                                                   flag_of(argc, argv, "--sim-step", nullptr)};
        return emit(crd::ceridc::verb_inspect(
            flag_of(argc, argv, "--program", nullptr), flag_of(argc, argv, "--entry", nullptr),
            crd::containers::as_const_span(args), crd::containers::as_const_span(breaks),
            crd::containers::as_const_span(watches), crd::containers::as_const_span(steps),
            static_cast<crd::u32>(std::strtoul(flag_of(argc, argv, "--max-stops", "0"), nullptr, 10)), &g_alloc,
            flag_of(argc, argv, "--record", nullptr), flag_of(argc, argv, "--seed", nullptr), &clock,
            flag_of(argc, argv, "--events", nullptr)));
    }
    print_usage();
    return 1;
}
