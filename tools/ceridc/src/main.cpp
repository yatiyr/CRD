// main.cpp — GEO-11: `ceridc` — the agent-facing CLI (every verb prints its JSON report to stdout, exit code
// mirrors the report's `ok`) and the MCP stdio loop (`ceridc mcp`: newline-delimited JSON-RPC, one line in →
// one line out — the transport shell over mcp_handle).

#include <crd/ceridc/verbs.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/memory/allocators/malloc_allocator.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

// NOLINTBEGIN(cppcoreguidelines-avoid-non-const-global-variables)
crd::memory::MallocAllocator g_alloc;
// NOLINTEND(cppcoreguidelines-avoid-non-const-global-variables)

[[nodiscard]] const char* flag_of(int argc, char** argv, const char* name, const char* def)
{
    for (int i = 2; i < argc - 1; ++i)
    {
        if (std::strcmp(argv[i], name) == 0) { return argv[i + 1]; }
    }
    return def;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], name) == 0) { return true; }
    }
    return false;
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
        "  ceridc mcp    (JSON-RPC 2.0 over stdio — one message per line)\n");
}

int run_mcp_loop()
{
    // newline-delimited JSON-RPC: read a line, handle, answer (a growing buffer — requests can be long)
    constexpr crd::usize k_cap = 1U << 20U;
    auto* line = static_cast<char*>(g_alloc.allocate(k_cap, 16));
    while (std::fgets(line, static_cast<int>(k_cap), stdin) != nullptr)
    {
        const crd::usize len = std::strlen(line);
        if (len == 0) { continue; }
        const crd::containers::String response = crd::ceridc::mcp_handle(
            {reinterpret_cast<const crd::u8*>(line), len}, &g_alloc);
        if (!response.empty())
        {
            std::printf("%s\n", response.c_str());
            (void)std::fflush(stdout);
        }
    }
    g_alloc.deallocate(line);
    return 0;
}

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
    if (std::strcmp(verb, "mcp") == 0) { return run_mcp_loop(); }
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
    print_usage();
    return 1;
}
