// crd-net -- IP addresses and endpoints (4.2a; docs/design/network-foundation.md#sockets).

#include <crd/core/platform.hpp>
#include <crd/core/seed_generators.hpp>
#include <crd/net/endpoint.hpp>

#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

#if CRD_OS_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace
{
using crd::u16;
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::net::AddressFamily;
using crd::net::Endpoint;
using crd::net::IpAddress;

// A text of at most kMaxEndpointText characters (plus a terminator for the host's reader).
struct Text
{
    char data[crd::net::kMaxEndpointText + 1U] = {};
    usize size = 0U;

    void push(char c)
    {
        REQUIRE(size < crd::net::kMaxEndpointText);
        data[size++] = c;
    }

    [[nodiscard]] std::string_view view() const
    {
        return std::string_view(data, size);
    }

    [[nodiscard]] const char* c_str() const
    {
        return data;
    }
};

Text text_of(const IpAddress& address)
{
    Text text;
    text.size = crd::net::format_address(address, crd::containers::Span<char>(text.data, crd::net::kMaxAddressText));
    REQUIRE(text.size != 0U);
    return text;
}

Text text_of(const Endpoint& endpoint)
{
    Text text;
    text.size = crd::net::format_endpoint(endpoint, crd::containers::Span<char>(text.data, crd::net::kMaxEndpointText));
    REQUIRE(text.size != 0U);
    return text;
}

IpAddress parsed(std::string_view text)
{
    IpAddress address;
    INFO(text);
    REQUIRE(crd::net::parse_address(text, address));
    return address;
}

IpAddress v6_of(const u16 (&groups)[8], u32 scope = 0U)
{
    u8 bytes[16];
    for (usize index = 0U; index < 8U; ++index)
    {
        bytes[2U * index] = static_cast<u8>(groups[index] >> 8U);
        bytes[2U * index + 1U] = static_cast<u8>(groups[index]);
    }
    return IpAddress::v6(bytes, scope);
}

} // namespace

TEST_CASE("addresses read IPv4 dotted quads and refuse every other IPv4 text", "[net][endpoint]")
{
    CHECK(parsed("192.0.2.1") == IpAddress::v4(192U, 0U, 2U, 1U));
    CHECK(parsed("0.0.0.0") == IpAddress::v4_any());
    CHECK(parsed("255.255.255.255") == IpAddress::v4(255U, 255U, 255U, 255U));
    for (const std::string_view bad : {"", "1", "1.2.3", "1.2.3.4.5", "1.2.3.256", "01.2.3.4", "1.2.3.00", "1..3.4",
                                       ".1.2.3", "1.2.3.", "1.2.3.4 ", " 1.2.3.4", "1.2.3.-4", "1.2.3.4%1", "0x1.2.3.4",
                                       "1.2.3.1000"})
    {
        IpAddress address;
        INFO(bad);
        CHECK_FALSE(crd::net::parse_address(bad, address));
    }
}

TEST_CASE("addresses read every RFC 4291 form of IPv6", "[net][endpoint]")
{
    CHECK(parsed("::") == IpAddress::v6_any());
    CHECK(parsed("::1") == IpAddress::v6_loopback());
    CHECK(parsed("2001:DB8:0:0:8:800:200C:417A") == v6_of({0x2001, 0xdb8, 0, 0, 8, 0x800, 0x200c, 0x417a}));
    CHECK(parsed("2001:db8::8:800:200c:417a") == v6_of({0x2001, 0xdb8, 0, 0, 8, 0x800, 0x200c, 0x417a}));
    CHECK(parsed("ff01::101") == v6_of({0xff01, 0, 0, 0, 0, 0, 0, 0x101}));
    CHECK(parsed("1::") == v6_of({1, 0, 0, 0, 0, 0, 0, 0}));
    CHECK(parsed("1:2:3:4:5:6:7::") == v6_of({1, 2, 3, 4, 5, 6, 7, 0}));
    CHECK(parsed("::2:3:4:5:6:7:8") == v6_of({0, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(parsed("0:0:0:0:0:0:13.1.68.3") == v6_of({0, 0, 0, 0, 0, 0, 0x0d01, 0x4403}));
    CHECK(parsed("::FFFF:129.144.52.38") == v6_of({0, 0, 0, 0, 0, 0xffff, 0x8190, 0x3426}));
    CHECK(parsed("1:2:3:4:5:6:1.2.3.4") == v6_of({1, 2, 3, 4, 5, 6, 0x0102, 0x0304}));
    CHECK(parsed("0001:0002:0003:0004:0005:0006:0007:0008") == v6_of({1, 2, 3, 4, 5, 6, 7, 8}));
    CHECK(parsed("fe80::1%3") == v6_of({0xfe80, 0, 0, 0, 0, 0, 0, 1}, 3U));
    CHECK(parsed("fe80::1%4294967295").scope == 0xffffffffU);
    for (const std::string_view bad :
         {":", ":::", "1:2:3:4:5:6:7", "1:2:3:4:5:6:7:8:9", "1::2:3:4:5:6:7:8", "1:2:3:4:5:6:7:8::", "1::2::3",
          ":1::2", "1::2:", "1:", ":1", "12345::", "g::", "1:2:3:4:5:6:7:1.2.3.4", "1.2.3.4::", "::1.2.3",
          "::1.2.3.04", "::1.2.3.4:5",
          "fe80::1%", "fe80::1%x", "fe80::1%01", "fe80::1%4294967296", "fe80::1%1%2", "[::1]", ":: ", "1 ::"})
    {
        IpAddress address;
        INFO(bad);
        CHECK_FALSE(crd::net::parse_address(bad, address));
    }
}

TEST_CASE("addresses print RFC 5952 text", "[net][endpoint]")
{
    // RFC 5952 sections 4 and 5, and the address forms of section 2.
    const std::pair<std::string_view, std::string_view> cases[] = {
        {"2001:0db8::0001", "2001:db8::1"},
        {"2001:db8:0:0:0:0:2:1", "2001:db8::2:1"},
        {"2001:db8:0:1:1:1:1:1", "2001:db8:0:1:1:1:1:1"},
        {"2001:0:0:1:0:0:0:1", "2001:0:0:1::1"},
        {"2001:db8:0:0:1:0:0:1", "2001:db8::1:0:0:1"},
        {"2001:DB8::AAAA", "2001:db8::aaaa"},
        {"::ffff:c000:0201", "::ffff:192.0.2.1"},
        {"::ffff:0.0.0.0", "::ffff:0.0.0.0"},
        {"0:0:0:0:0:0:0:0", "::"},
        {"0:0:0:0:0:0:0:1", "::1"},
        {"1:0:0:0:0:0:0:0", "1::"},
        {"0:1:0:1:0:1:0:1", "0:1:0:1:0:1:0:1"},
        {"1:0:0:2:0:0:0:3", "1:0:0:2::3"},
        {"::1.2.3.4", "::102:304"},
        {"fe80::0:1%7", "fe80::1%7"},
        {"ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295", "ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295"},
        {"10.0.0.1", "10.0.0.1"},
    };
    for (const auto& [in, out] : cases)
    {
        INFO(in);
        CHECK(text_of(parsed(in)).view() == out);
    }
    // The longest text fills kMaxAddressText exactly; one byte less is refused, not cut.
    const IpAddress widest = parsed("ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff%4294967295");
    char exact[crd::net::kMaxAddressText];
    CHECK(crd::net::format_address(widest, exact) == crd::net::kMaxAddressText);
    CHECK(crd::net::format_address(widest, crd::containers::Span<char>(exact, crd::net::kMaxAddressText - 1U)) == 0U);
    Endpoint widest_endpoint{widest, 65535U};
    char endpoint_exact[crd::net::kMaxEndpointText];
    CHECK(crd::net::format_endpoint(widest_endpoint, endpoint_exact) == crd::net::kMaxEndpointText);
}

TEST_CASE("endpoints read and print bracketed IPv6 and plain IPv4 with a port", "[net][endpoint]")
{
    Endpoint endpoint;
    REQUIRE(crd::net::parse_endpoint("[2001:db8::1]:443", endpoint));
    CHECK(endpoint.address == parsed("2001:db8::1"));
    CHECK(endpoint.port == 443U);
    CHECK(text_of(endpoint).view() == "[2001:db8::1]:443");
    REQUIRE(crd::net::parse_endpoint("192.0.2.1:0", endpoint));
    CHECK(endpoint == Endpoint{IpAddress::v4(192U, 0U, 2U, 1U), 0U});
    CHECK(text_of(endpoint).view() == "192.0.2.1:0");
    REQUIRE(crd::net::parse_endpoint("[fe80::1%2]:65535", endpoint));
    CHECK(text_of(endpoint).view() == "[fe80::1%2]:65535");
    const Endpoint before = endpoint;
    for (const std::string_view bad : {"", "1.2.3.4", "1.2.3.4:", "1.2.3.4:65536", "1.2.3.4:01", "1.2.3.4:-1",
                                       "::1:443", "[::1]", "[::1]:", "[::1]443", "[1.2.3.4]:5", "[::1:5", "::1]:5",
                                       "1.2.3.4:5:6", "[::1]:5 ", "[]:5", "1.2.3.4:123456"})
    {
        INFO(bad);
        CHECK_FALSE(crd::net::parse_endpoint(bad, endpoint));
        CHECK(endpoint == before); // a refused text leaves the output alone
    }
}

TEST_CASE("addresses map, unmap and cut to S3's prefixes", "[net][endpoint]")
{
    const IpAddress v4 = IpAddress::v4(192U, 0U, 2U, 77U);
    CHECK(v4.mapped() == parsed("::ffff:192.0.2.77"));
    CHECK(v4.mapped().unmapped() == v4);
    CHECK(v4.unmapped() == v4);
    CHECK(IpAddress::v6_loopback().mapped() == IpAddress::v6_loopback());
    CHECK(parsed("::ffff:127.0.0.9").is_loopback());
    CHECK(parsed("127.255.0.1").is_loopback());
    CHECK(IpAddress::v6_loopback().is_loopback());
    CHECK_FALSE(parsed("::2").is_loopback());
    CHECK_FALSE(parsed("128.0.0.1").is_loopback());
    CHECK(IpAddress::v4_any().is_unspecified());
    CHECK(IpAddress::v6_any().is_unspecified());
    CHECK_FALSE(parsed("::1").is_unspecified());
    CHECK_FALSE(parsed("::1.2.3.4").is_v4_mapped());
    CHECK_FALSE(v4.is_v4_mapped());

    CHECK(v4.prefix(24U) == IpAddress::v4(192U, 0U, 2U, 0U));
    CHECK(v4.prefix(20U) == IpAddress::v4(192U, 0U, 0U, 0U));
    CHECK(v4.prefix(29U) == IpAddress::v4(192U, 0U, 2U, 72U));
    CHECK(v4.prefix(32U) == v4);
    CHECK(v4.prefix(33U) == v4); // past the width: every bit kept, the zero tail kept zero
    CHECK(v4.prefix(0U) == IpAddress::v4_any());
    const IpAddress v6 = parsed("2001:db8:1234:5678:9abc:def0:1:2%5");
    CHECK(v6.prefix(64U) == parsed("2001:db8:1234:5678::"));
    CHECK(v6.prefix(56U) == parsed("2001:db8:1234:5600::"));
    CHECK(v6.prefix(128U) == parsed("2001:db8:1234:5678:9abc:def0:1:2"));
    CHECK(v6.prefix(200U).scope == 0U);
    CHECK(v6.prefix(0U) == IpAddress::v6_any());
    CHECK(v6.family == AddressFamily::V6);
}

TEST_CASE("addresses read and print what the host's own reader and printer do", "[net][endpoint]")
{
    // Every address prints, then reads back to itself; on Linux, glibc's inet_pton and inet_ntop are the independent
    // oracle (both follow RFC 4291 and RFC 5952; glibc also prints ::/96 with a dotted tail, which RFC 5952 leaves to
    // mapped addresses, so those are left out of the printed comparison).
    u64 state = 0x4e45542d55445031ULL;
    for (u32 round = 0U; round < 20000U; ++round)
    {
        const u64 high = crd::splitmix64_next(state);
        const u64 low = crd::splitmix64_next(state);
        const u64 shape = crd::splitmix64_next(state);
        u8 bytes[16];
        for (usize index = 0U; index < 8U; ++index)
        {
            bytes[index] = static_cast<u8>(high >> (8U * index));
            bytes[8U + index] = static_cast<u8>(low >> (8U * index));
        }
        // Zero runs and mapped prefixes are where the printing rules bite: make most groups zero.
        for (usize group = 0U; group < 8U; ++group)
        {
            if (((shape >> group) & 1U) != 0U)
            {
                bytes[2U * group] = 0U;
                bytes[2U * group + 1U] = 0U;
            }
        }
        if ((shape >> 8U) % 8U == 0U)
        {
            for (usize index = 0U; index < 10U; ++index)
            {
                bytes[index] = 0U;
            }
            bytes[10] = 0xffU;
            bytes[11] = 0xffU;
        }
        const bool v4 = (shape >> 12U) % 4U == 0U;
        const IpAddress address = v4 ? IpAddress::v4(bytes[0], bytes[1], bytes[2], bytes[3]) : IpAddress::v6(bytes);
        const Text text = text_of(address);
        INFO(text.view());
        CHECK(parsed(text.view()) == address);
#if CRD_OS_LINUX
        u8 host[16] = {};
        const int family = v4 ? AF_INET : AF_INET6;
        REQUIRE(inet_pton(family, text.c_str(), host) == 1);
        CHECK(std::string_view(reinterpret_cast<const char*>(host), v4 ? 4U : 16U) ==
              std::string_view(reinterpret_cast<const char*>(address.bytes), v4 ? 4U : 16U));
        bool compatible = !v4;
        for (usize index = 0U; compatible && index < 12U; ++index)
        {
            compatible = bytes[index] == 0U;
        }
        if (!compatible)
        {
            char printed[INET6_ADDRSTRLEN];
            REQUIRE(inet_ntop(family, address.bytes, printed, sizeof(printed)) != nullptr);
            CHECK(text.view() == printed);
        }
        // The same address in its longest form, upper case, reads the same here and in glibc.
        if (!v4)
        {
            Text wide;
            for (usize group = 0U; group < 8U; ++group)
            {
                constexpr char digits[] = "0123456789ABCDEF";
                if (group != 0U)
                {
                    wide.push(':');
                }
                wide.push(digits[address.bytes[2U * group] >> 4U]);
                wide.push(digits[address.bytes[2U * group] & 0xfU]);
                wide.push(digits[address.bytes[2U * group + 1U] >> 4U]);
                wide.push(digits[address.bytes[2U * group + 1U] & 0xfU]);
            }
            CHECK(parsed(wide.view()) == address);
        }
#endif
    }
}

#if CRD_OS_LINUX
TEST_CASE("addresses accept exactly the texts glibc's inet_pton accepts", "[net][endpoint]")
{
    // Random texts over the address alphabet, biased toward near-misses: each verdict and value agrees with glibc.
    constexpr char alphabet[] = "0123456789abcdefABCDEF:.:.:";
    u64 state = 0x5245414445525321ULL;
    u32 accepted = 0U;
    for (u32 round = 0U; round < 200000U; ++round)
    {
        const u64 length = 1U + crd::splitmix64_next(state) % 24U;
        Text text;
        for (u64 index = 0U; index < length; ++index)
        {
            text.push(alphabet[crd::splitmix64_next(state) % (sizeof(alphabet) - 1U)]);
        }
        INFO(text.view());
        IpAddress mine;
        const bool ours = crd::net::parse_address(text.view(), mine);
        u8 host[16] = {};
        const bool v4 = text.view().find(':') == std::string_view::npos;
        const bool theirs = inet_pton(v4 ? AF_INET : AF_INET6, text.c_str(), host) == 1;
        REQUIRE(ours == theirs);
        if (ours)
        {
            ++accepted;
            CHECK(std::string_view(reinterpret_cast<const char*>(host), v4 ? 4U : 16U) ==
                  std::string_view(reinterpret_cast<const char*>(mine.bytes), v4 ? 4U : 16U));
        }
    }
    CHECK(accepted > 100U); // the texts reach acceptance, so the agreement is not only on refusals
}
#endif
