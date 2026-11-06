// The plumbing crd-webrtc-bwe-peer shims: libwebrtc's field trial lookup without Abseil.
#pragma once

#include <string>
#include <string_view>

namespace webrtc
{

class FieldTrialsView
{
public:
    virtual ~FieldTrialsView() = default;
    virtual std::string Lookup(std::string_view key) const = 0; // crd-lint-allow-std-container: its interface

    bool IsEnabled(std::string_view key) const
    {
        return Lookup(key).starts_with("Enabled");
    }

    bool IsDisabled(std::string_view key) const
    {
        return Lookup(key).starts_with("Disabled");
    }
};

} // namespace webrtc
