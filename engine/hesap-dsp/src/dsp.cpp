// crd-hesap-dsp — translation-unit anchor (Phase 3.1.6 v11-a).
//
// The DSP surface is header-template-heavy (the two-layer typed API + the
// streaming kernels instantiate at the consumer). This TU gives the static
// library a compiled object so the build/link graph is well-formed, and is
// where any non-template helpers + the CLI registration block land.

#include <crd/hesap/dsp/dsp.hpp>

namespace crd::hesap::dsp
{
// Intentionally empty for v11-a — the substrate is header-only templates.
} // namespace crd::hesap::dsp
