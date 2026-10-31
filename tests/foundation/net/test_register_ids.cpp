// crd-net -- the identifiers generated from the NET-STD standards register (register_ids.hpp). The drift CTest
// (crd-net-register-ids-drift) proves the header is the generator's output for the committed register; these tests
// prove the generated values keep the register's rules.

#include <crd/net/register_ids.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

namespace
{
using crd::u16;
using crd::u32;
using crd::usize;
using crd::net::is_grease;
using crd::net::kGreaseIds;
using crd::net::kProtocolIds;
using crd::net::protocol_register_id;
using crd::net::ProtocolId;
} // namespace

TEST_CASE("Register ids: every Cerid protocol has a unique identifier and its register entry", "[net][net-std]")
{
    static_assert(sizeof(kProtocolIds) / sizeof(kProtocolIds[0]) == 8U, "the eight Cerid wire protocols");
    for (usize i = 0U; i < sizeof(kProtocolIds) / sizeof(kProtocolIds[0]); ++i)
    {
        const ProtocolId id = kProtocolIds[i];
        CHECK(static_cast<u16>(id) == i + 1U);
        CHECK(std::strncmp(protocol_register_id(id), "cerid.", 6U) == 0);
        CHECK_FALSE(is_grease(static_cast<u16>(id)));
        for (usize j = 0U; j < i; ++j)
        {
            CHECK(kProtocolIds[j] != id);
            CHECK(std::strcmp(protocol_register_id(kProtocolIds[j]), protocol_register_id(id)) != 0);
        }
    }
    CHECK(std::strcmp(protocol_register_id(ProtocolId::SessionControl), "cerid.session-control") == 0);
    CHECK(std::strcmp(protocol_register_id(ProtocolId::RelayAllocation), "cerid.relay-allocation") == 0);
    CHECK(std::strcmp(protocol_register_id(static_cast<ProtocolId>(0x0009U)), "") == 0);
    CHECK(std::strcmp(protocol_register_id(static_cast<ProtocolId>(0x0000U)), "") == 0);
}

TEST_CASE("Register ids: GREASE values are exactly the RFC 8701 pattern 0x?A?A", "[net][net-std][grease]")
{
    static_assert(sizeof(kGreaseIds) / sizeof(kGreaseIds[0]) == 16U, "one value per hexadecimal digit");
    u32 matches = 0U;
    for (u32 value = 0U; value <= 0xFFFFU; ++value)
    {
        const auto id      = static_cast<u16>(value);
        const bool pattern = (value & 0x0F0FU) == 0x0A0AU && (value >> 12U) == ((value >> 4U) & 0x0FU);
        CHECK(is_grease(id) == pattern);
        matches += pattern ? 1U : 0U;
    }
    CHECK(matches == 16U);
    for (usize i = 0U; i < 16U; ++i)
    {
        CHECK(kGreaseIds[i] == static_cast<u16>(0x0A0AU + 0x1010U * i));
    }
    static_assert(is_grease(0x0A0AU) && is_grease(0xFAFAU) && !is_grease(0x0A1AU), "usable in constant expressions");
}

TEST_CASE("Register ids: unverified IANA codepoints stay apart from verified ones", "[net][net-std]")
{
    // Taken from knowledge in the 2026-10-31 snapshot; they move to crd::net::iana once checked against IANA.
    CHECK(crd::net::iana::unverified::kQuicVersion1 == 0x00000001U);
    CHECK(crd::net::iana::unverified::kQuicVersion2 == 0x6B3343CFU);
    CHECK(crd::net::iana::unverified::kQuicTpGreaseQuicBit == 0x2AB2U);
    CHECK(crd::net::iana::unverified::kAlpnHttp3 == "h3");
}
