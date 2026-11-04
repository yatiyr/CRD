#pragma once

// crd-crypto tests -- the pinned JSON vector files read at run time (Project Wycheproof's testvectors_v1 from
// CRD_WYCHEPROOF_DIR, the MLS working group's from CRD_MLS_VECTORS_DIR, RFC 9180's from CRD_HPKE_VECTORS) and the
// byte helpers the suites share.

#include <crd/containers/array.hpp>
#include <crd/containers/string.hpp>
#include <crd/json/json.hpp>
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

// `size` bytes of `value`.
inline Bytes filled(usize size, u8 value)
{
    Bytes out;
    out.resize(size, value);
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

// One JSON vector file, parsed: its values are read through the flat DOM by node.
class JsonFile
{
public:
    explicit JsonFile(const containers::String& path)
    {
        INFO(path.c_str());
        Bytes bytes;
        REQUIRE(platform::fs::read_file_binary(platform::fs::Path(containers::StringView(path)), bytes));
        REQUIRE(json::parse(containers::ConstSpan<u8>(bytes.data(), bytes.size()), m_doc));
    }

    [[nodiscard]] const json::JsonDoc& doc() const { return m_doc; }
    [[nodiscard]] u32                           root() const { return m_doc.root; }

    [[nodiscard]] containers::StringView text(u32 node) const
    {
        REQUIRE(node != json::kInvalid);
        const json::JsonNode& n = m_doc.nodes[node];
        REQUIRE(n.type == json::JsonType::String);
        return containers::StringView(m_doc.strings.data() + n.str_off, n.str_len);
    }
    [[nodiscard]] containers::StringView text(u32 object, const char* key) const
    {
        return text(json::find(m_doc, object, key));
    }
    [[nodiscard]] Bytes hex(u32 object, const char* key) const { return from_hex(text(object, key)); }
    [[nodiscard]] bool  has(u32 object, const char* key) const
    {
        const u32 node = json::find(m_doc, object, key);
        return node != json::kInvalid && m_doc.nodes[node].type != json::JsonType::Null;
    }
    [[nodiscard]] u64 number(u32 node) const { return static_cast<u64>(json::as_i64(m_doc, node, -1)); }
    [[nodiscard]] u64 number(u32 object, const char* key) const
    {
        return number(json::find(m_doc, object, key));
    }

private:
    json::JsonDoc m_doc{memory::default_allocator()};
};

// The path of `name` in a pinned vector directory.
inline containers::String vector_path(const char* directory, const char* name)
{
    containers::String path(directory);
    path.append("/");
    path.append(name);
    return path;
}

// One Wycheproof file, parsed: groups and tests are walked through the flat DOM.
class Vectors : public JsonFile
{
public:
    explicit Vectors(const char* name) : JsonFile(wycheproof_path(name))
    {
        m_groups = json::find(doc(), root(), "testGroups");
        REQUIRE(json::count_of(doc(), m_groups) > 0U);
    }

    [[nodiscard]] u32 groups() const { return m_groups; }

    [[nodiscard]] bool flagged(u32 test, const char* flag) const
    {
        const u32 flags = json::find(doc(), test, "flags");
        for (u32 i = 0U; i < json::count_of(doc(), flags); ++i)
        {
            if (json::str_value_eq(doc(), json::at(doc(), flags, i), flag))
            {
                return true;
            }
        }
        return false;
    }

    // Visits every test as f(group, test, result) with result "valid", "invalid" or "acceptable".
    template <typename F> void each(const F& f) const
    {
        for (u32 g = 0U; g < json::count_of(doc(), m_groups); ++g)
        {
            const u32 group = json::at(doc(), m_groups, g);
            const u32 tests = json::find(doc(), group, "tests");
            for (u32 t = 0U; t < json::count_of(doc(), tests); ++t)
            {
                const u32 test = json::at(doc(), tests, t);
                INFO("tcId " << number(test, "tcId"));
                f(group, test, text(test, "result"));
            }
        }
    }

private:
    static containers::String wycheproof_path(const char* name)
    {
        containers::String path = vector_path(CRD_WYCHEPROOF_DIR "/testvectors_v1", name);
        path.append(".json");
        return path;
    }

    u32 m_groups = json::kInvalid;
};
} // namespace crd::crypto::test
