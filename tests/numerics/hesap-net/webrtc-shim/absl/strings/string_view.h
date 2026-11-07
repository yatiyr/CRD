// The plumbing crd-webrtc-fec-peer shims: Abseil's string view is the standard's in the builds libwebrtc ships.
#pragma once

#include <string_view>

namespace absl
{
using string_view = std::string_view;
} // namespace absl
