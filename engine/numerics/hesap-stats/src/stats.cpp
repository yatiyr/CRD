// crd-hesap-stats — translation-unit anchor for the (currently header-only) statistics module, so the static
// library has an object to link. The CLI registration (hesap.stats.*) lands with the v12 cluster.

#include <crd/hesap/stats/stats.hpp>

namespace crd::hesap::stats
{
// Anchor: a consumer can reference this to force the .obj to link if a static-init block is added later (v12 CLI).
void register_stats_anchor() noexcept {}
} // namespace crd::hesap::stats
