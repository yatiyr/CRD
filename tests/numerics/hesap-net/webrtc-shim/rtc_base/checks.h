// The plumbing crd-webrtc-bwe-peer shims: libwebrtc's checks as its release builds compile them, DCHECKs off and
// CHECKs fatal. The estimators and units around it are libwebrtc's own files (tools/webrtc-bwe-sources).
#pragma once

#include <cstdlib>

namespace webrtc::crd_shim
{
struct AnyStream
{
    template <class T>
    AnyStream& operator<<(const T&)
    {
        return *this;
    }
};

struct FatalStream : AnyStream
{
    ~FatalStream()
    {
        std::abort();
    }
};
} // namespace webrtc::crd_shim

#define RTC_DCHECK_IS_ON 0
#define RTC_CHECK(condition)                                                                                           \
    if (condition)                                                                                                     \
    {                                                                                                                  \
    }                                                                                                                  \
    else                                                                                                               \
        ::webrtc::crd_shim::FatalStream {}
#define RTC_CHECK_NOTREACHED() std::abort()
#define RTC_DCHECK(condition)                                                                                          \
    if (true)                                                                                                          \
    {                                                                                                                  \
    }                                                                                                                  \
    else                                                                                                               \
        ::webrtc::crd_shim::AnyStream {}
#define RTC_DCHECK_GT(a, b) RTC_DCHECK(true)
#define RTC_DCHECK_GE(a, b) RTC_DCHECK(true)
#define RTC_DCHECK_LE(a, b) RTC_DCHECK(true)
#define RTC_DCHECK_LT(a, b) RTC_DCHECK(true)
#define RTC_DCHECK_EQ(a, b) RTC_DCHECK(true)
#define RTC_DCHECK_NOTREACHED() (void)0
