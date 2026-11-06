// The plumbing crd-webrtc-bwe-peer shims: what TrendlineEstimatorSettings asks of libwebrtc's settings parser, on a
// "key:value,key:value" string.
#pragma once

#include "rtc_base/experiments/field_trial_parser.h"

#include <memory>
#include <string_view>
#include <type_traits>

namespace webrtc
{

class StructParametersParser
{
public:
    template <class... Pairs>
    static std::unique_ptr<StructParametersParser> Create(Pairs... pairs)
    {
        auto parser = std::make_unique<StructParametersParser>();
        parser->add(pairs...);
        return parser;
    }

    void Parse(std::string_view text)
    {
        crd_shim::for_each_pair(text,
                                [&](std::string_view key, std::string_view value)
                                {
                                    for (unsigned i = 0U; i < m_count; ++i)
                                    {
                                        if (key == m_fields[i].key)
                                        {
                                            m_fields[i].set(m_fields[i].value, value);
                                        }
                                    }
                                });
    }

    [[nodiscard]] const char* Encode() const
    {
        return "";
    }

private:
    struct Field
    {
        const char* key;
        void* value;
        void (*set)(void*, std::string_view);
    };

    void add() {}

    template <class T, class... Rest>
    void add(const char* key, T* value, Rest... rest)
    {
        m_fields[m_count++] = {key, value,
                               [](void* field, std::string_view text)
                               {
                                   if constexpr (std::is_same_v<T, bool>)
                                   {
                                       *static_cast<T*>(field) = text == "true";
                                   }
                                   else
                                   {
                                       *static_cast<T*>(field) = crd_shim::parsed<T>(text);
                                   }
                               }};
        add(rest...);
    }

    Field m_fields[8] = {};
    unsigned m_count = 0U;
};

} // namespace webrtc
