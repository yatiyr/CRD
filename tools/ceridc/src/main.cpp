// main.cpp — GEO-11: `ceridc` — the agent-facing CLI (every verb prints its JSON report to stdout, exit code
// mirrors the report's `ok`) and the MCP stdio loop (`ceridc mcp`: newline-delimited JSON-RPC, one line in →
// one line out — the transport shell over McpServer).

#include <crd/ceridc/verbs.hpp>
#include <crd/containers/array.hpp>
#include <crd/cooker/cook_handler.hpp>
#include <crd/crypto/key_store.hpp>
#include <crd/jobs/jobs.hpp>
#include <crd/memory/allocators/growable_tlsf_allocator.hpp>
#include <crd/perf/diag_assets.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/diagnostic_host.hpp>
#include <crd/platform/arguments.hpp>
#include <crd/platform/filesystem.hpp>
#include <crd/platform/standard_streams.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

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

// The key store access of `policy key|sign`: the protector --protector names (platform, or tpm with --tcti) and the
// device --device names (tpm, or pkcs11 with --module, --label and the PIN in --pin-file, never on the command line).
struct PolicyKeys
{
    crd::crypto::PlatformKeyProtector           platform;
    crd::crypto::TpmKeyProtector                tpm;
    crd::crypto::TpmKeyDevice                   tpm_device;
    std::optional<crd::crypto::Pkcs11KeyDevice> pkcs11;
    crd::crypto::KeyProtector*                  protector = nullptr;
    crd::crypto::KeyDevice*                     device    = nullptr;
};

[[nodiscard]] bool policy_keys(int argc, char** argv, PolicyKeys& keys)
{
    const char*                        protector = flag_of(argc, argv, "--protector", nullptr);
    const char*                        device    = flag_of(argc, argv, "--device", nullptr);
    const crd::crypto::TpmConnection   tpm(crd::containers::StringView(flag_of(argc, argv, "--tcti", "")));
    keys.tpm        = crd::crypto::TpmKeyProtector(tpm);
    keys.tpm_device = crd::crypto::TpmKeyDevice(tpm);
    if (protector != nullptr && std::strcmp(protector, "platform") == 0)
    {
        keys.protector = &keys.platform;
    }
    else if (protector != nullptr && std::strcmp(protector, "tpm") == 0)
    {
        keys.protector = &keys.tpm;
    }
    else if (protector != nullptr)
    {
        std::fprintf(stderr, "ceridc policy: --protector must be platform or tpm\n");
        return false;
    }
    if (device == nullptr)
    {
        return true;
    }
    if (std::strcmp(device, "tpm") == 0)
    {
        keys.device = &keys.tpm_device;
        return true;
    }
    const char* module   = flag_of(argc, argv, "--module", nullptr);
    const char* label    = flag_of(argc, argv, "--label", nullptr);
    const char* pin_file = flag_of(argc, argv, "--pin-file", nullptr);
    if (std::strcmp(device, "pkcs11") != 0 || module == nullptr || label == nullptr || pin_file == nullptr)
    {
        std::fprintf(stderr, "ceridc policy: --device must be tpm, or pkcs11 with --module, --label and --pin-file\n");
        return false;
    }
    crd::containers::Array<crd::u8> pin(&g_alloc);
    if (!crd::platform::fs::read_file_binary(crd::platform::fs::Path(crd::containers::StringView(pin_file)), pin))
    {
        std::fprintf(stderr, "ceridc policy: --pin-file is unreadable\n");
        return false;
    }
    crd::usize length = pin.size();
    while (length > 0U && (pin[length - 1U] == '\n' || pin[length - 1U] == '\r'))
    {
        --length; // the line end an editor or `echo` adds
    }
    keys.pkcs11.emplace(crd::crypto::Pkcs11Token(crd::containers::StringView(module),
                                                 crd::containers::StringView(label),
                                                 crd::containers::ConstSpan<crd::u8>(pin.data(), length)));
    crd::crypto::secure_zero(crd::containers::Span<crd::u8>(pin.data(), pin.size()));
    keys.device = &*keys.pkcs11;
    return true;
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
        "  ceridc comply --rules <dir> --scope <id> --jurisdiction <code,...> --day <YYYY-MM-DD>\n"
        "                (the reviewed rules that bind the scope that day, each proven or unproven)\n"
        "  ceridc screen --lists <dir> --authority <id,...> --day <YYYY-MM-DD> [--destination <region>]\n"
        "                [--kind <individual|entity|vessel|aircraft> --name <n>... [--birth <date>]\n"
        "                [--country <cc>]... [--identifier <type:value>]...] [--review <n>] [--match <n>]\n"
        "                (sanctions and embargo screening: clear, review, unavailable or blocked, every hit)\n"
        "  ceridc policy body --source <generation.policy.toml> --out <body>\n"
        "  ceridc policy assemble --in <signed>... --out <generation>\n"
        "  ceridc policy verify --generation <f> --trust <trust.toml> --now <YYYY-MM-DD|seconds> [--floor <n>]\n"
        "                [--source <generation.policy.toml>]\n"
        "  ceridc policy key --store <dir> --name <key> --algorithm <register name> [--placement <p>] <keys>\n"
        "  ceridc policy sign --store <dir> --name <key> (--source <f> | --body <f>) --out <signed> <keys>\n"
        "                <keys>: --protector platform|tpm [--tcti <conf>] [--device tpm|pkcs11 [--module <so>\n"
        "                --label <token> --pin-file <f>]]\n"
        "                (the crypto policy generation: render the reviewed source, join its signatures, judge it)\n"
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
        "  ceridc doctor [--hardware [--seconds <per arm>] [--memory-mib <per CPU>]]\n"
        "              (this build's diagnostics; --hardware runs the Cerid-free stress check on every CPU at\n"
        "              once and exits 1 when any arm saw an event; one report line per page)\n"
        "  ceridc mcp [--diag-grant <list>] [--diag-root <dir>] [--diag-host <profile.host.toml>]\n"
        "             (JSON-RPC 2.0 over stdio, one message per line; the diag tool serves the grant, default read;\n"
        "             with --diag-host the engine's diagnostic host starts from the authored profile first: its crash\n"
        "             handler, its flight recorder (flight.* served, bundles to its capture_dir) and its grant)\n");
}

// The stdio transport: one JSON-RPC message per line in, its reply as one line out. One server serves the process, so
// an initialize handshake selects revision 2025-11-25 for every later line. A line is read whole with its length (a
// NUL byte in it is the parser's to refuse, not the message's end); one longer than the bound is answered once as an
// invalid request and dropped, never cut into pieces that would each be read as a message. Replies go to `out`, the
// stream the protocol claimed (crd/platform/standard_streams.hpp).
int run_mcp_loop(crd::perf::DiagCommandService& diag, crd::perf::DiagnosticHost& host, std::FILE* out)
{
    constexpr crd::usize            k_cap = crd::usize{1} << 20U;
    crd::ceridc::McpServer          server(&g_alloc, &diag);
    crd::containers::Array<crd::u8> line(&g_alloc);
    line.reserve(k_cap);
    bool overlong = false;
    const auto answer = [&]
    {
        if (overlong)
        {
            (void)std::fprintf(out,
                               "{\"jsonrpc\":\"2.0\",\"error\":{\"code\":-32600,\"message\":\"invalid request: a "
                               "message is at most %zu bytes\"}}\n",
                               static_cast<std::size_t>(k_cap));
            (void)std::fflush(out);
        }
        else if (!line.empty())
        {
            const crd::containers::String response = server.handle(crd::containers::as_const_span(line));
            (void)host.poll(); // DIAG.11a: a capture whose post window closed is delivered (and written) here
            if (!response.empty())
            {
                (void)std::fprintf(out, "%s\n", response.c_str());
                (void)std::fflush(out);
            }
        }
        line.clear();
        overlong = false;
    };
    for (int c = std::getc(stdin); c != EOF; c = std::getc(stdin))
    {
        if (c == '\n')
        {
            answer();
        }
        else if (line.size() < k_cap)
        {
            line.push_back(static_cast<crd::u8>(c));
        }
        else
        {
            overlong = true;
        }
    }
    answer(); // a last message without its newline is still a message
    return 0;
}

// The crd::jobs pool for the length of one diagnostic verb. The host executor of the replay commands runs programs on
// the host provider, whose pool the process owns, on this (the main) thread; the other verbs start no workers. The
// provider's parallel ranges take their job arrays from the calling thread's frame arena, which the pool enrolled.
class ScopedJobPool
{
public:
    static constexpr crd::jobs::Config kConfig{.num_threads = 4U};

    explicit ScopedJobPool(const crd::jobs::Config& config = kConfig) { crd::jobs::init(config); }
    ~ScopedJobPool() { crd::jobs::shutdown(); }
    ScopedJobPool(const ScopedJobPool&)            = delete;
    ScopedJobPool& operator=(const ScopedJobPool&) = delete;
    ScopedJobPool(ScopedJobPool&&)                 = delete;
    ScopedJobPool& operator=(ScopedJobPool&&)      = delete;
};

} // namespace

int main(int argc, char* argv[])
{
    const crd::platform::Arguments arguments(argc, argv); // UTF-8 on every platform (crd/platform/arguments.hpp)
    argc = arguments.count();
    argv = arguments.values();
    if (argc < 2)
    {
        print_usage();
        return 1;
    }
    crd::cooker::register_builtin_handlers(); // cook + every import format the processor speaks

    const char* verb = argv[1];
    if (std::strcmp(verb, "mcp") == 0)
    {
        // DIAG.11a: with --diag-host the engine's diagnostic host starts from the authored profile before the pool and
        // the service; it outlives both (declared first). --diag-grant, when given, overrides the profile's grant and
        // is held to the profile's rules. Without --diag-host nothing is installed, as before.
        crd::perf::DiagnosticHost   host;
        const char*                 host_asset = flag_of(argc, argv, "--diag-host", nullptr);
        const char*                 grant_flag = flag_of(argc, argv, "--diag-grant", nullptr);
        crd::perf::DiagAuthoritySet grant      = 0U;
        if (host_asset != nullptr)
        {
            crd::perf::DiagnosticHostProfile profile;
            crd::containers::String          reason;
            if (!crd::perf::load_diagnostic_host_profile(host_asset, profile, reason))
            {
                std::fprintf(stderr, "ceridc mcp: --diag-host: %s\n", reason.c_str());
                return 2;
            }
            if (grant_flag != nullptr && !crd::perf::parse_authority_list(grant_flag, profile.command_grant))
            {
                std::fprintf(stderr, "ceridc mcp: --diag-grant must be 'none' or a comma-separated authority list\n");
                return 2;
            }
            const crd::perf::DiagHostStart start = host.start(profile, reason);
            if (start != crd::perf::DiagHostStart::Started)
            {
                const crd::containers::StringView name = crd::perf::diag_host_start_name(start);
                std::fprintf(stderr, "ceridc mcp: the diagnostic host refused to start: %.*s (%s)\n",
                             static_cast<int>(name.size()), name.data(), reason.c_str());
                return 2;
            }
            grant = host.profile().command_grant;
        }
        // The diag tool's authority is this process's start-up decision; no request can change it.
        else if (!crd::perf::parse_authority_list(grant_flag != nullptr ? grant_flag : "read", grant))
        {
            std::fprintf(stderr, "ceridc mcp: --diag-grant must be 'none' or a comma-separated authority list\n");
            return 2;
        }
        crd::jobs::Config pool_config = ScopedJobPool::kConfig;
        host.configure_jobs(pool_config); // the watchdog window the host's starvation and hang triggers need
        const ScopedJobPool          pool(pool_config);
        crd::perf::DiagServiceConfig config;
        const char*                  root = flag_of(argc, argv, "--diag-root", nullptr);
        if (root != nullptr)
        {
            config.root = root;
        }
        host.configure_service(config);
        crd::perf::DiagCommandService diag(grant, config, &g_alloc);
        if (!crd::ceridc::bind_diag_commands(diag) || !host.serve(diag))
        {
            std::fprintf(stderr, "ceridc mcp: the diagnostic commands could not be registered\n");
            return 2;
        }
        // The protocol owns stdout: its bytes exactly (no CRLF, no 0x1A end of input), and a verb's stray print goes
        // to stderr instead of corrupting a message.
        std::FILE* const protocol = crd::platform::claim_protocol_streams();
        if (protocol == nullptr)
        {
            std::fprintf(stderr, "ceridc mcp: the standard streams could not be claimed for the protocol\n");
            return 2;
        }
        const int rc = run_mcp_loop(diag, host, protocol);
        (void)std::fclose(protocol);
        return rc;
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
    if (std::strcmp(verb, "doctor") == 0)
    {
        crd::ceridc::DoctorOptions options;
        options.hardware = has_flag(argc, argv, "--hardware");
        options.seconds = flag_of(argc, argv, "--seconds", nullptr);
        options.memory_mib = flag_of(argc, argv, "--memory-mib", nullptr);
        bool passed = false;
        const crd::containers::String report = crd::ceridc::verb_doctor(options, passed, &g_alloc);
        std::printf("%s", report.c_str());
        return passed ? 0 : 1;
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
    if (std::strcmp(verb, "comply") == 0)
    {
        return emit(crd::ceridc::verb_comply(
            flag_of(argc, argv, "--rules", nullptr), flag_of(argc, argv, "--scope", nullptr),
            flag_of(argc, argv, "--jurisdiction", nullptr), flag_of(argc, argv, "--day", nullptr), &g_alloc));
    }
    if (std::strcmp(verb, "screen") == 0)
    {
        crd::containers::Array<const char*> names(&g_alloc);
        crd::containers::Array<const char*> countries(&g_alloc);
        crd::containers::Array<const char*> identifiers(&g_alloc);
        flags_of(argc, argv, "--name", names);
        flags_of(argc, argv, "--country", countries);
        flags_of(argc, argv, "--identifier", identifiers);
        crd::ceridc::ScreenFlags flags;
        flags.lists = flag_of(argc, argv, "--lists", nullptr);
        flags.authorities = flag_of(argc, argv, "--authority", nullptr);
        flags.day = flag_of(argc, argv, "--day", nullptr);
        flags.destination = flag_of(argc, argv, "--destination", nullptr);
        flags.kind = flag_of(argc, argv, "--kind", nullptr);
        flags.names = crd::containers::ConstSpan<const char*>(names.data(), names.size());
        flags.birth = flag_of(argc, argv, "--birth", nullptr);
        flags.countries = crd::containers::ConstSpan<const char*>(countries.data(), countries.size());
        flags.identifiers = crd::containers::ConstSpan<const char*>(identifiers.data(), identifiers.size());
        flags.review = flag_of(argc, argv, "--review", nullptr);
        flags.match = flag_of(argc, argv, "--match", nullptr);
        return emit(crd::ceridc::verb_screen(flags, &g_alloc));
    }
    if (std::strcmp(verb, "policy") == 0)
    {
        crd::containers::Array<const char*> in(&g_alloc);
        flags_of(argc, argv, "--in", in);
        crd::ceridc::PolicyFlags flags;
        flags.action = argc >= 3 ? argv[2] : nullptr;
        flags.source = flag_of(argc, argv, "--source", nullptr);
        flags.generation = flag_of(argc, argv, "--generation", nullptr);
        flags.in = crd::containers::ConstSpan<const char*>(in.data(), in.size());
        flags.trust = flag_of(argc, argv, "--trust", nullptr);
        flags.now = flag_of(argc, argv, "--now", nullptr);
        flags.floor = flag_of(argc, argv, "--floor", nullptr);
        flags.out = flag_of(argc, argv, "--out", nullptr);
        flags.body = flag_of(argc, argv, "--body", nullptr);
        flags.store = flag_of(argc, argv, "--store", nullptr);
        flags.name = flag_of(argc, argv, "--name", nullptr);
        flags.algorithm = flag_of(argc, argv, "--algorithm", nullptr);
        flags.placement = flag_of(argc, argv, "--placement", nullptr);
        PolicyKeys keys;
        if (!policy_keys(argc, argv, keys))
        {
            return 2;
        }
        flags.protector = keys.protector;
        flags.device = keys.device;
        return emit(crd::ceridc::verb_policy(flags, &g_alloc));
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
