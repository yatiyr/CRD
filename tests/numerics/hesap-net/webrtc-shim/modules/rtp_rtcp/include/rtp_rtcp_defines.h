// The plumbing crd-webrtc-fec-peer shims: the two constants forward_error_correction.cc reads from this large header.
#pragma once

#include <cstdint>

#define IP_PACKET_SIZE 1500

namespace webrtc
{
constexpr uint8_t kRtpHeaderSize = 12;
} // namespace webrtc
