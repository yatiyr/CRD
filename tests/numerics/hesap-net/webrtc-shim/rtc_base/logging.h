// The plumbing crd-webrtc-bwe-peer shims: libwebrtc's logging, compiled and never evaluated. Its Kalman filter's one libm
// call, an unqualified pow in namespace webrtc, resolves here to the engine's: the Cerid Math Mandate takes std::pow out
// of crd-hesap-net, so the oracle compares the arithmetic around one shared pow (crd/math/power.hpp holds its own oracle).
#pragma once

#include "rtc_base/checks.h"

#include <crd/math/power.hpp>

namespace webrtc
{
[[nodiscard]] inline double pow(double base, double exponent)
{
    return crd::math::pow(base, exponent);
}
} // namespace webrtc

#define RTC_LOG(severity)                                                                                              \
    if (true)                                                                                                          \
    {                                                                                                                  \
    }                                                                                                                  \
    else                                                                                                               \
        ::webrtc::crd_shim::AnyStream {}
