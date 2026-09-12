// crd-hesap-wavelet — translation-unit anchor (Phase 3.1.6 v11w-a).
//
// The wavelet surface is header-template-heavy (DWT/IDWT/SWT/WPT/CWT instantiate
// at the consumer over <MathScalar T>). This TU gives the static library a
// compiled object so the build/link graph is well-formed, and is where any
// non-template helpers + the CLI registration block land at v11-z.

#include <crd/hesap/wavelet/wavelet.hpp>

namespace crd::hesap::wavelet
{
// Intentionally empty for v11w-a/b — the substrate is header-only templates over
// the generated (machine-precision-vs-pywt) coefficient table.
} // namespace crd::hesap::wavelet
