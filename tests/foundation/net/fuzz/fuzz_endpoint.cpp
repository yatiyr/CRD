// crd-fuzz-net-endpoint: the address and endpoint texts of crd-net (4.2a; docs/design/network-foundation.md#sockets)
// under the bounded fuzz harness (REPO.DEV.9; docs/design/test-instruments.md).
//
// Input: any bytes, read once as an address text and once as an endpoint text.
//
// Oracles. What either reader accepts prints in at most its bound, and that text reads back to the same value and
// prints again unchanged (one text per value). An IPv4 address keeps its twelve tail bytes zero and only IPv6 carries
// a zone. On Linux, glibc's inet_pton is an independent reader: for a text without a zone, both accept or both refuse,
// with the same bytes.
#include <crd/core/platform.hpp>
#include <crd/fuzz/harness.hpp>
#include <crd/net/endpoint.hpp>

#include <cstring>
#include <string_view>

#if CRD_OS_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

namespace
{
using crd::u8;
using crd::usize;
using crd::net::AddressFamily;
using crd::net::Endpoint;
using crd::net::IpAddress;

constexpr usize kMaxText = 128U;

void check_address(const IpAddress& address)
{
    for (usize index = (address.family == AddressFamily::V4) ? 4U : 16U; index < 16U; ++index)
    {
        CRD_FUZZ_REQUIRE(address.bytes[index] == 0U);
    }
    CRD_FUZZ_REQUIRE(address.family == AddressFamily::V6 || address.scope == 0U);
    char text[crd::net::kMaxAddressText];
    const usize size = crd::net::format_address(address, text);
    CRD_FUZZ_REQUIRE(size != 0U);
    IpAddress again;
    CRD_FUZZ_REQUIRE(crd::net::parse_address(std::string_view(text, size), again));
    CRD_FUZZ_REQUIRE(again == address);
    char twice[crd::net::kMaxAddressText];
    CRD_FUZZ_REQUIRE(crd::net::format_address(again, twice) == size);
    CRD_FUZZ_REQUIRE(std::memcmp(text, twice, size) == 0);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxText)
    {
        return 0;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    IpAddress address;
    const bool read = crd::net::parse_address(text, address);
    if (read)
    {
        check_address(address);
    }
#if CRD_OS_LINUX
    if (text.find('%') == std::string_view::npos && text.find('\0') == std::string_view::npos)
    {
        char terminated[kMaxText + 1U] = {};
        std::memcpy(terminated, data, size);
        const bool v4 = text.find(':') == std::string_view::npos;
        u8 host[16] = {};
        const bool theirs = inet_pton(v4 ? AF_INET : AF_INET6, terminated, host) == 1;
        CRD_FUZZ_REQUIRE(read == theirs);
        CRD_FUZZ_REQUIRE(!read || std::memcmp(host, address.bytes, v4 ? 4U : 16U) == 0);
    }
#endif

    Endpoint endpoint;
    if (crd::net::parse_endpoint(text, endpoint))
    {
        check_address(endpoint.address);
        char printed[crd::net::kMaxEndpointText];
        const usize printed_size = crd::net::format_endpoint(endpoint, printed);
        CRD_FUZZ_REQUIRE(printed_size != 0U);
        Endpoint again;
        CRD_FUZZ_REQUIRE(crd::net::parse_endpoint(std::string_view(printed, printed_size), again));
        CRD_FUZZ_REQUIRE(again == endpoint);
    }
    return 0;
}

void crd_fuzz_seeds(crd::fuzz::SeedSink& sink)
{
    const char* const seeds[][2] = {
        {"v4", "192.0.2.1"},
        {"v4-endpoint", "203.0.113.9:4433"},
        {"v6-compressed", "2001:db8::8:800:200c:417a"},
        {"v6-full-upper", "2001:0DB8:0000:0000:0008:0800:200C:417A"},
        {"v6-mapped", "::ffff:192.0.2.128"},
        {"v6-dotted-tail", "1:2:3:4:5:6:1.2.3.4"},
        {"v6-zone", "fe80::1%4"},
        {"v6-endpoint-zone", "[fe80::1%4]:65535"},
        {"v6-gap-too-full", "1::2:3:4:5:6:7:8"},
        {"v6-gap-after-eight", "4:5:6:7:80:1:88:88::"}, // once accepted: "::" standing for no group
        {"v4-leading-zero", "01.2.3.4"},
        {"endpoint-unbracketed", "::1:443"},
    };
    for (const auto& seed : seeds)
    {
        sink.add(seed[0], reinterpret_cast<const u8*>(seed[1]), std::strlen(seed[1]));
    }
}
