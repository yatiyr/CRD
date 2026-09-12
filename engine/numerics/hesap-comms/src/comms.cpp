// crd-hesap-comms — translation-unit anchor (Phase 3.1.6 v11c).
//
// The comms surface is header-template-heavy (modems / loops / equalizers / OFDM instantiate at the consumer over
// <MathScalar T>). This TU gives the static library a compiled object so the build/link graph is well-formed, and
// is where any non-template helpers + the CLI registration block land at v11-z.

#include <crd/hesap/comms/comms.hpp>

namespace crd::hesap::comms
{
// Intentionally empty for v11c-a — the substrate is header-only templates.
} // namespace crd::hesap::comms
