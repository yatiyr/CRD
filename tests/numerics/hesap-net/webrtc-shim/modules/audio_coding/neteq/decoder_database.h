// The plumbing crd-webrtc-fec-peer shims: the three questions RedPayloadSplitter::CheckRedPayloads asks NetEq's decoder
// list. The oracle runs SplitRed, whose answers do not depend on them; which types a block may carry is the session's.
#pragma once

#include <cstdint>

namespace webrtc
{
class DecoderDatabase
{
public:
    [[nodiscard]] bool IsRed(uint8_t) const
    {
        return false;
    }
    [[nodiscard]] bool IsDtmf(uint8_t) const
    {
        return false;
    }
    [[nodiscard]] bool IsComfortNoise(uint8_t) const
    {
        return false;
    }
};
} // namespace webrtc
