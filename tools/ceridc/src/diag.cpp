// diag.cpp — the diag verb: one request through a host's typed diagnostic command service. The CLI and the MCP tool
// both end here, so a transport adds nothing to the answer: the report is the service's response document, byte for
// byte. The host's grant and file root are chosen when the service is built, never by a request.

#include <crd/ceridc/verbs.hpp>

#include <crd/assetio/json_write.hpp>
#include <crd/perf/diag_commands.hpp>

namespace crd::ceridc
{

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
                       "upload and process-memory");
        w.end_object();
        return crd::containers::String(w.str());
    }
    crd::perf::DiagServiceConfig config;
    if (host.root != nullptr)
    {
        config.root = crd::containers::StringView{host.root};
    }
    crd::perf::DiagCommandService service(grant, config, alloc);
    return verb_diag(service, request, alloc);
}

} // namespace crd::ceridc
