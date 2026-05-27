// crd-hesap-amg anchor TU — Phase 3.1.6 v4k-a.
//
// The AMG surface (SaAmg<T>, strength/aggregation/prolongator) is header-only
// templated code. This TU exists so the static library has an object file
// (and a home for future CLI registration of the AMG ops). Intentionally empty.

#include <crd/hesap/amg/amg.hpp>

namespace crd::hesap::amg
{
// Anchor symbol so the static library is never dropped as empty by the linker.
void amg_anchor() noexcept {}
} // namespace crd::hesap::amg
