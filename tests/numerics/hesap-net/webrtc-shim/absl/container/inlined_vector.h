// The plumbing crd-webrtc-fec-peer shims: Abseil's small vector, one protected stream in a std::vector.
#pragma once

#include <cstddef>
#include <vector>

namespace absl
{
template <class T, std::size_t N>
using InlinedVector = std::vector<T>; // crd-lint-allow-std-container: what it shims
} // namespace absl
