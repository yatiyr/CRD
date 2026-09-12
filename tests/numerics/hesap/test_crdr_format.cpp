#include <catch2/catch_test_macros.hpp>

#include <crd/hesap/crdr_format.hpp>

TEST_CASE("HDV0 FourCC has the documented little-endian byte layout", "[hesap][crdr]")
{
    // 'H'=0x48, 'D'=0x44, 'V'=0x56, '0'=0x30; little-endian => 0x30564448.
    REQUIRE((crd::hesap::kHesapDenseFourCC & 0xFF) == 'H');
    REQUIRE(((crd::hesap::kHesapDenseFourCC >> 8) & 0xFF) == 'D');
    REQUIRE(((crd::hesap::kHesapDenseFourCC >> 16) & 0xFF) == 'V');
    REQUIRE(((crd::hesap::kHesapDenseFourCC >> 24) & 0xFF) == '0');
    REQUIRE(crd::hesap::kHesapDenseFourCC == 0x30564448U);
}
