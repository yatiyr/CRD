#pragma once

// crd-crypto tests -- Project Wycheproof's vector files (the pinned C2SP/wycheproof testvectors_v1, read at run time
// from CRD_WYCHEPROOF_DIR) and the byte helpers the provider suites share.

#include <crd/assetio/json.hpp>
#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/memory/allocator.hpp>
#include <crd/platform/filesystem.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>

namespace crd::crypto::test
{
using Bytes = containers::Array<u8>;

inline u8 nibble(char c)
{
    if (c >= '0' && c <= '9')
    {
        return static_cast<u8>(c - '0');
    }
    if (c >= 'a' && c <= 'f')
    {
        return static_cast<u8>(c - 'a' + 10);
    }
    REQUIRE((c >= 'A' && c <= 'F'));
    return static_cast<u8>(c - 'A' + 10);
}

inline Bytes from_hex(containers::StringView text)
{
    REQUIRE(text.size() % 2U == 0U);
    Bytes out;
    out.resize(text.size() / 2U);
    for (usize i = 0U; i < out.size(); ++i)
    {
        out[i] = static_cast<u8>(nibble(text[2U * i]) << 4U | nibble(text[2U * i + 1U]));
    }
    return out;
}

inline bool equal(containers::ConstSpan<u8> a, containers::ConstSpan<u8> b)
{
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size()) == 0);
}

inline bool all_zero(containers::ConstSpan<u8> bytes)
{
    return std::ranges::all_of(bytes, [](u8 b) { return b == 0U; });
}

// One Wycheproof file, parsed: groups and tests are walked through the flat DOM.
class Vectors
{
public:
    explicit Vectors(const char* name)
    {
        containers::String path(CRD_WYCHEPROOF_DIR "/testvectors_v1/");
        path.append(name);
        path.append(".json");
        INFO(path.c_str());
        Bytes bytes;
        REQUIRE(platform::fs::read_file_binary(platform::fs::Path(containers::StringView(path)), bytes));
        REQUIRE(assetio::json::parse(containers::ConstSpan<u8>(bytes.data(), bytes.size()), m_doc));
        m_groups = assetio::json::find(m_doc, m_doc.root, "testGroups");
        REQUIRE(assetio::json::count_of(m_doc, m_groups) > 0U);
    }

    [[nodiscard]] const assetio::json::JsonDoc& doc() const { return m_doc; }
    [[nodiscard]] u32                           groups() const { return m_groups; }

    [[nodiscard]] containers::StringView text(u32 node) const
    {
        REQUIRE(node != assetio::json::kInvalid);
        const assetio::json::JsonNode& n = m_doc.nodes[node];
        REQUIRE(n.type == assetio::json::JsonType::String);
        return containers::StringView(m_doc.strings.data() + n.str_off, n.str_len);
    }
    [[nodiscard]] containers::StringView text(u32 object, const char* key) const
    {
        return text(assetio::json::find(m_doc, object, key));
    }
    [[nodiscard]] Bytes hex(u32 object, const char* key) const { return from_hex(text(object, key)); }
    [[nodiscard]] bool  has(u32 object, const char* key) const
    {
        const u32 node = assetio::json::find(m_doc, object, key);
        return node != assetio::json::kInvalid && m_doc.nodes[node].type != assetio::json::JsonType::Null;
    }
    [[nodiscard]] u64 number(u32 object, const char* key) const
    {
        return static_cast<u64>(assetio::json::as_i64(m_doc, assetio::json::find(m_doc, object, key), -1));
    }
    [[nodiscard]] bool flagged(u32 test, const char* flag) const
    {
        const u32 flags = assetio::json::find(m_doc, test, "flags");
        for (u32 i = 0U; i < assetio::json::count_of(m_doc, flags); ++i)
        {
            if (assetio::json::str_value_eq(m_doc, assetio::json::at(m_doc, flags, i), flag))
            {
                return true;
            }
        }
        return false;
    }

    // Visits every test as f(group, test, result) with result "valid", "invalid" or "acceptable".
    template <typename F> void each(const F& f) const
    {
        for (u32 g = 0U; g < assetio::json::count_of(m_doc, m_groups); ++g)
        {
            const u32 group = assetio::json::at(m_doc, m_groups, g);
            const u32 tests = assetio::json::find(m_doc, group, "tests");
            for (u32 t = 0U; t < assetio::json::count_of(m_doc, tests); ++t)
            {
                const u32 test = assetio::json::at(m_doc, tests, t);
                INFO("tcId " << number(test, "tcId"));
                f(group, test, text(test, "result"));
            }
        }
    }

private:
    assetio::json::JsonDoc m_doc{memory::default_allocator()};
    u32                    m_groups = assetio::json::kInvalid;
};
} // namespace crd::crypto::test
