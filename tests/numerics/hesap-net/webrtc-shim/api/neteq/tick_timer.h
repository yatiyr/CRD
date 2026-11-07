// The plumbing crd-webrtc-fec-peer shims: NetEq's Packet holds a waiting-time stopwatch the splitter never sets.
#pragma once

namespace webrtc
{
class TickTimer
{
public:
    class Stopwatch
    {
    };
};
} // namespace webrtc
