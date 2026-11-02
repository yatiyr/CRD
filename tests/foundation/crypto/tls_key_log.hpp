#pragma once

// The RFC 9850 key log a context's sink received (network foundation S19), each line read by the format's own grammar
// apart from the provider: "<label> <client random> <secret>", the label one of RFC 9850 section 3's, the random 64
// and the secret an even count of lower-case hex digits (the provider's case), no terminator. Shared by the TLS, ECH,
// early data and QUIC tests.

#include <crd/containers/array.hpp>
#include <crd/crypto/tls.hpp>

#include <algorithm>
#include <cstring>

namespace crd::crypto::test
{
inline constexpr const char* kKeyLogLabels[] = {
    "CLIENT_EARLY_TRAFFIC_SECRET", "EARLY_EXPORTER_SECRET",   "CLIENT_HANDSHAKE_TRAFFIC_SECRET",
    "SERVER_HANDSHAKE_TRAFFIC_SECRET", "CLIENT_TRAFFIC_SECRET_0", "SERVER_TRAFFIC_SECRET_0",
    "EXPORTER_SECRET",             "CLIENT_RANDOM",           "ECH_SECRET",
    "ECH_CONFIG"};

// The labels of a full TLS 1.3 handshake without early data, EXPORTER_SECRET apart.
inline constexpr const char* kKeyLogTraffic[] = {"CLIENT_HANDSHAKE_TRAFFIC_SECRET", "SERVER_HANDSHAKE_TRAFFIC_SECRET",
                                                 "CLIENT_TRAFFIC_SECRET_0", "SERVER_TRAFFIC_SECRET_0"};

struct KeyLogLine
{
    char  label[32]    = {};
    u8    random[32]   = {};
    u8    secret[64]   = {};
    usize secret_bytes = 0U;

    [[nodiscard]] containers::StringView name() const noexcept { return containers::StringView(label); }
    [[nodiscard]] containers::ConstSpan<u8> value() const noexcept
    {
        return containers::ConstSpan<u8>(secret, secret_bytes);
    }
    [[nodiscard]] bool operator==(const KeyLogLine& other) const noexcept
    {
        return name() == other.name() && std::memcmp(random, other.random, sizeof(random)) == 0 &&
               secret_bytes == other.secret_bytes && std::memcmp(secret, other.secret, secret_bytes) == 0;
    }
};

class KeyLog
{
public:
    // Points `config`'s sink at this log.
    void attach(TlsConfig& config, bool exporters = false) noexcept
    {
        config.key_log           = &KeyLog::sink;
        config.key_log_user      = this;
        config.key_log_exporters = exporters;
    }

    [[nodiscard]] usize size() const noexcept { return m_lines.size(); }
    [[nodiscard]] bool  malformed() const noexcept { return m_malformed; }
    [[nodiscard]] const KeyLogLine& operator[](usize i) const noexcept { return m_lines[i]; }

    // The first line of `label`; null without one.
    [[nodiscard]] const KeyLogLine* find(containers::StringView label) const noexcept
    {
        for (const KeyLogLine& line : m_lines)
        {
            if (line.name() == label)
            {
                return &line;
            }
        }
        return nullptr;
    }

    // Whether `other` holds the same lines, in any order (RFC 9850 section 3 fixes none).
    [[nodiscard]] bool same(const KeyLog& other) const noexcept
    {
        return other.size() == size() && std::ranges::all_of(m_lines, [&other](const KeyLogLine& line) {
                   const KeyLogLine* match = other.find(line.name());
                   return match != nullptr && *match == line;
               });
    }

    // Whether every line carries `random`.
    [[nodiscard]] bool all_of_random(containers::ConstSpan<u8> random) const noexcept
    {
        return !m_lines.empty() && std::ranges::all_of(m_lines, [random](const KeyLogLine& line) {
                   return random.size() == sizeof(line.random) &&
                          std::memcmp(line.random, random.data(), random.size()) == 0;
               });
    }

    void clear() noexcept
    {
        m_lines.clear();
        m_malformed = false;
    }

private:
    static void sink(void* user, containers::StringView line) noexcept { static_cast<KeyLog*>(user)->take(line); }

    static int digit(char c) noexcept
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    }

    static bool hex(containers::StringView text, u8* out, usize capacity, usize& bytes) noexcept
    {
        if (text.empty() || text.size() % 2U != 0U || text.size() / 2U > capacity)
        {
            return false;
        }
        for (usize i = 0U; i < text.size(); i += 2U)
        {
            const int high = digit(text[i]);
            const int low  = digit(text[i + 1U]);
            if (high < 0 || low < 0)
            {
                return false;
            }
            out[i / 2U] = static_cast<u8>(high * 16 + low);
        }
        bytes = text.size() / 2U;
        return true;
    }

    void take(containers::StringView line) noexcept
    {
        const usize first  = line.find(' ');
        const usize second = first == containers::StringView::npos ? first : line.find(' ', first + 1U);
        KeyLogLine  parsed;
        usize       random_bytes = 0U;
        bool        known        = false;
        if (second != containers::StringView::npos && line.find(' ', second + 1U) == containers::StringView::npos)
        {
            const containers::StringView label = line.substr(0U, first);
            for (const char* name : kKeyLogLabels)
            {
                known = known || label == name;
            }
            if (known)
            {
                std::memcpy(parsed.label, label.data(), label.size());
            }
            known = known && hex(line.substr(first + 1U, second - first - 1U), parsed.random, sizeof(parsed.random),
                                 random_bytes) &&
                    random_bytes == sizeof(parsed.random) &&
                    hex(line.substr(second + 1U), parsed.secret, sizeof(parsed.secret), parsed.secret_bytes);
        }
        if (!known)
        {
            m_malformed = true;
            return;
        }
        m_lines.push_back(parsed);
    }

    containers::Array<KeyLogLine> m_lines;
    bool                          m_malformed = false;
};
} // namespace crd::crypto::test
