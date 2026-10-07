// diag.cpp — the diag verb: one request through a host's typed diagnostic command service. The CLI and the MCP tool
// both end here, so a transport adds nothing to the answer: the report is the service's response document, byte for
// byte. The host's grant and file root are chosen when the service is built, never by a request. Every ceridc service
// binds the same commands (bind_diag_commands), so the two transports list and answer the same set.

#include <crd/ceridc/verbs.hpp>

#include "host_dialects.hpp"

#include <crd/assetio/json_write.hpp>
#include <crd/ceir/cook/inspect_diag.hpp>
#include <crd/ceir/cook/program_diag.hpp>
#include <crd/ceir/cook/replay_diag.hpp>
#include <crd/ceir/func.hpp>
#include <crd/ceir/gen/arith_ops.hpp>
#include <crd/ceir/gen/core_ops.hpp>
#include <crd/perf/diag_commands.hpp>
#include <crd/perf/gpu/gpu_resources_diag.hpp>

namespace crd::ceridc
{

void register_host_dialects(crd::ceir::Context& ctx, void* /*user*/)
{
    (void)crd::ceir::arith::register_arith_ops(ctx);
    (void)crd::ceir::core::register_core_ops(ctx);
    (void)crd::ceir::func::register_dialect(ctx);
}

bool bind_diag_commands(crd::perf::DiagCommandService& service)
{
    // One configuration per command for the process, alive for as long as any service it is registered with. ceridc
    // opens no GPU context, so gpu.resources answers the process's identity counts and says no context is registered.
    static crd::ceir::cook::ProgramProvenanceCommand provenance{&register_host_dialects, nullptr};
    static crd::ceir::cook::ProgramInspectCommand    inspect{&register_host_dialects, nullptr};
    static crd::ceir::cook::ReplayPrepareCommand     replay{&register_host_dialects, nullptr};
    static crd::ceir::cook::ReplayCommands           replay_runs{&register_host_dialects, nullptr};
    static crd::perf::gpu::GpuResourcesCommand       gpu_resources;
    return crd::ceir::cook::register_program_provenance(service, provenance) &&
           crd::ceir::cook::register_program_inspect(service, inspect) &&
           crd::ceir::cook::register_replay_prepare(service, replay) &&
           crd::ceir::cook::register_replay_record(service, replay_runs) &&
           crd::ceir::cook::register_replay_run(service, replay_runs) &&
           crd::perf::gpu::register_gpu_resources(service, gpu_resources);
}

crd::containers::String verb_diag(crd::perf::DiagCommandService& service, const crd::perf::DiagRequest& request,
                                  crd::memory::IAllocator* alloc)
{
    const crd::perf::DiagResult result = service.execute(request);
    return crd::containers::String(result.json.data(), result.json.size(), alloc);
}

crd::containers::String verb_diag_host(const DiagHostOptions& host, const crd::perf::DiagRequest& request,
                                       crd::memory::IAllocator* alloc)
{
    crd::perf::DiagAuthoritySet grant = 0U;
    const crd::containers::StringView grant_text =
        host.grant != nullptr ? crd::containers::StringView{host.grant} : crd::containers::StringView{"read"};
    if (!crd::perf::parse_authority_list(grant_text, grant))
    {
        crd::assetio::JsonWriter w(alloc);
        w.begin_object();
        w.kv("ok", false);
        w.kv("verb", "diag");
        w.kv("reason", "the grant must be 'none' or a comma-separated list of read, record, inject, remote-enable, "
                       "upload, process-memory and execute");
        w.end_object();
        return crd::containers::String(w.str());
    }
    crd::perf::DiagServiceConfig config;
    if (host.root != nullptr)
    {
        config.root = crd::containers::StringView{host.root};
    }
    crd::perf::DiagCommandService service(grant, config, alloc);
    if (!bind_diag_commands(service))
    {
        crd::assetio::JsonWriter w(alloc);
        w.begin_object();
        w.kv("ok", false);
        w.kv("verb", "diag");
        w.kv("reason", "the host's diagnostic commands could not be registered");
        w.end_object();
        return crd::containers::String(w.str());
    }
    return verb_diag(service, request, alloc);
}

} // namespace crd::ceridc
