// crd-hesap-special — translation-unit anchor. The special-function library is header-only (templates over the
// scalar type); this TU exists so the static library has an object to compile. The CLI registration
// (hesap.special.*) lands at v12-z.

#include <crd/hesap/special/special.hpp>

namespace crd::hesap::special
{
// Force-link anchor — referenced by the test/CLI layers so the static lib is pulled in.
void special_anchor() noexcept {}
} // namespace crd::hesap::special
