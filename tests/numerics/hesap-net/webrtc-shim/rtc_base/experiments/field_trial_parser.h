// The plumbing crd-webrtc-bwe-peer shims: what BitrateEstimator asks of libwebrtc's field trial parameters, on a
// "key:value,key:value" string. A rate is in kbit/s and a size in bytes, as its parser reads a bare number.
#pragma once

#include "api/units/data_rate.h"
#include "api/units/data_size.h"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string_view>

namespace webrtc
{

namespace crd_shim
{
// Calls visit(key, value) for each "key:value" of a comma-separated list.
template <class Visit>
void for_each_pair(std::string_view text, Visit&& visit)
{
    while (!text.empty())
    {
        const std::string_view pair = text.substr(0, text.find(','));
        text.remove_prefix(pair.size() + (pair.size() < text.size() ? 1 : 0));
        const auto colon = pair.find(':');
        if (colon != std::string_view::npos)
        {
            visit(pair.substr(0, colon), pair.substr(colon + 1));
        }
    }
}

[[nodiscard]] inline double number(std::string_view text)
{
    char buffer[64] = {};
    std::memcpy(buffer, text.data(), text.size() < 63 ? text.size() : 63);
    return std::strtod(buffer, nullptr);
}

template <class T>
[[nodiscard]] T parsed(std::string_view text)
{
    return static_cast<T>(number(text));
}
template <>
[[nodiscard]] inline DataSize parsed<DataSize>(std::string_view text)
{
    return DataSize::Bytes(static_cast<int>(number(text)));
}
template <>
[[nodiscard]] inline DataRate parsed<DataRate>(std::string_view text)
{
    return DataRate::KilobitsPerSec(static_cast<int>(number(text)));
}
} // namespace crd_shim

class FieldTrialParameterInterface
{
public:
    explicit FieldTrialParameterInterface(const char* key) : m_key(key) {}
    virtual ~FieldTrialParameterInterface() = default;
    virtual void Set(std::string_view value) = 0;

    [[nodiscard]] std::string_view Key() const
    {
        return m_key;
    }

private:
    const char* m_key;
};

inline void ParseFieldTrial(std::initializer_list<FieldTrialParameterInterface*> fields, std::string_view trial)
{
    crd_shim::for_each_pair(trial,
                            [&](std::string_view key, std::string_view value)
                            {
                                for (FieldTrialParameterInterface* field : fields)
                                {
                                    if (key == field->Key())
                                    {
                                        field->Set(value);
                                    }
                                }
                            });
}

template <class T>
class FieldTrialParameter : public FieldTrialParameterInterface
{
public:
    FieldTrialParameter(const char* key, T value) : FieldTrialParameterInterface(key), m_value(value) {}

    void Set(std::string_view value) override
    {
        m_value = crd_shim::parsed<T>(value);
    }

    [[nodiscard]] T Get() const
    {
        return m_value;
    }

    operator T() const
    {
        return m_value;
    }

    const T* operator->() const
    {
        return &m_value;
    }

private:
    T m_value;
};

template <class T>
class FieldTrialConstrained : public FieldTrialParameter<T>
{
public:
    FieldTrialConstrained(const char* key, T value, T low, T high)
        : FieldTrialParameter<T>(key, value), m_low(low), m_high(high)
    {
    }

    void Set(std::string_view value) override
    {
        const T candidate = crd_shim::parsed<T>(value);
        if (candidate >= m_low && candidate <= m_high)
        {
            FieldTrialParameter<T>::Set(value);
        }
    }

private:
    T m_low;
    T m_high;
};

} // namespace webrtc
