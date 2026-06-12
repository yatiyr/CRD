// crd-hesap-ode — translation-unit anchor for the (currently header-only) ODE module, so the static
// library has an object to link. The CLI registration (hesap.ode.*) lands at v9-z.

#include <crd/hesap/ode/ode.hpp>

namespace crd::hesap::ode
{
// Anchor: a consumer can reference this to force the .obj to link if a static-init block is added later (v9-z CLI).
void register_ode_anchor() noexcept {}
} // namespace crd::hesap::ode
