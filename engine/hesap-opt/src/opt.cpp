// crd-hesap-opt — Phase 3.1.6 v7. Translation-unit anchor for the (currently header-only) optimization module,
// so the static library has an object to link. The CLI registration (hesap.opt.*) lands at v7-z. ADR-0090.

#include <crd/hesap/opt/opt.hpp>

namespace crd::hesap::opt
{
// Anchor: a consumer can reference this to force the .obj to link if a static-init block is added later (v7-z CLI).
void register_opt_anchor() noexcept {}
} // namespace crd::hesap::opt
