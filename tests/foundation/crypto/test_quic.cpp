// crd-crypto -- QUIC packet protection (NET.2; docs/design/network-foundation.md#quic-protection): the sample packets
// of RFC 9001 appendix A and RFC 9369 appendix A byte for byte (the Initial secrets, the client's and the server's
// Initial packets sealed and header-protected and then read back, the Retry integrity tags, the ChaCha20-Poly1305
// short header packet and its key update), RFC 9000's packet number examples, every refusal and length rule, the
// usage limits, tampering anywhere in a packet, random round trips in every suite and version, one key opening on
// several threads, and a packet number space's window opening each packet once through reordering, duplication,
// replay and forgery.

#include "wycheproof.hpp"

#include <crd/crypto/quic.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <thread>

namespace
{
using crd::u32;
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;
using namespace crd::crypto::test;

// RFC 9001 appendix A and RFC 9369 appendix A, copied from the RFC Editor's text files.
constexpr const char* kV1ClientProtected =
    "c000000001088394c8f03e5157080000449e7b9aec34d1b1c98dd7689fb8ec11d242b123dc9bd8bab936b47d92ec356c0bab"
    "7df5976d27cd449f63300099f3991c260ec4c60d17b31f8429157bb35a1282a643a8d2262cad67500cadb8e7378c8eb7539e"
    "c4d4905fed1bee1fc8aafba17c750e2c7ace01e6005f80fcb7df621230c83711b39343fa028cea7f7fb5ff89eac2308249a0"
    "2252155e2347b63d58c5457afd84d05dfffdb20392844ae812154682e9cf012f9021a6f0be17ddd0c2084dce25ff9b06cde5"
    "35d0f920a2db1bf362c23e596d11a4f5a6cf3948838a3aec4e15daf8500a6ef69ec4e3feb6b1d98e610ac8b7ec3faf6ad760"
    "b7bad1db4ba3485e8a94dc250ae3fdb41ed15fb6a8e5eba0fc3dd60bc8e30c5c4287e53805db059ae0648db2f64264ed5e39"
    "be2e20d82df566da8dd5998ccabdae053060ae6c7b4378e846d29f37ed7b4ea9ec5d82e7961b7f25a9323851f681d582363a"
    "a5f89937f5a67258bf63ad6f1a0b1d96dbd4faddfcefc5266ba6611722395c906556be52afe3f565636ad1b17d508b73d874"
    "3eeb524be22b3dcbc2c7468d54119c7468449a13d8e3b95811a198f3491de3e7fe942b330407abf82a4ed7c1b311663ac698"
    "90f4157015853d91e923037c227a33cdd5ec281ca3f79c44546b9d90ca00f064c99e3dd97911d39fe9c5d0b23a229a234cb3"
    "6186c4819e8b9c5927726632291d6a418211cc2962e20fe47feb3edf330f2c603a9d48c0fcb5699dbfe5896425c5bac4aee8"
    "2e57a85aaf4e2513e4f05796b07ba2ee47d80506f8d2c25e50fd14de71e6c418559302f939b0e1abd576f279c4b2e0feb85c"
    "1f28ff18f58891ffef132eef2fa09346aee33c28eb130ff28f5b766953334113211996d20011a198e3fc433f9f2541010ae1"
    "7c1bf202580f6047472fb36857fe843b19f5984009ddc324044e847a4f4a0ab34f719595de37252d6235365e9b84392b0610"
    "85349d73203a4a13e96f5432ec0fd4a1ee65accdd5e3904df54c1da510b0ff20dcc0c77fcb2c0e0eb605cb0504db87632cf3"
    "d8b4dae6e705769d1de354270123cb11450efc60ac47683d7b8d0f811365565fd98c4c8eb936bcab8d069fc33bd801b03ade"
    "a2e1fbc5aa463d08ca19896d2bf59a071b851e6c239052172f296bfb5e72404790a2181014f3b94a4e97d117b438130368cc"
    "39dbb2d198065ae3986547926cd2162f40a29f0c3c8745c0f50fba3852e566d44575c29d39a03f0cda721984b6f440591f35"
    "5e12d439ff150aab7613499dbd49adabc8676eef023b15b65bfc5ca06948109f23f350db82123535eb8a7433bdabcb909271"
    "a6ecbcb58b936a88cd4e8f2e6ff5800175f113253d8fa9ca8885c2f552e657dc603f252e1a8e308f76f0be79e2fb8f5d5fbb"
    "e2e30ecadd220723c8c0aea8078cdfcb3868263ff8f0940054da48781893a7e49ad5aff4af300cd804a6b6279ab3ff3afb64"
    "491c85194aab760d58a606654f9f4400e8b38591356fbf6425aca26dc85244259ff2b19c41b9f96f3ca9ec1dde434da7d2d3"
    "92b905ddf3d1f9af93d1af5950bd493f5aa731b4056df31bd267b6b90a079831aaf579be0a39013137aac6d404f518cfd468"
    "40647e78bfe706ca4cf5e9c5453e9f7cfd2b8b4c8d169a44e55c88d4a9a7f9474241e221af44860018ab0856972e194cd934";

constexpr const char* kV1ServerProtected =
    "cf000000010008f067a5502a4262b5004075c0d95a482cd0991cd25b0aac406a5816b6394100f37a1c69797554780bb38cc5"
    "a99f5ede4cf73c3ec2493a1839b3dbcba3f6ea46c5b7684df3548e7ddeb9c3bf9c73cc3f3bded74b562bfb19fb84022f8ef4"
    "cdd93795d77d06edbb7aaf2f58891850abbdca3d20398c276456cbc42158407dd074ee";

constexpr const char* kV1Retry =
    "ff000000010008f067a5502a4262b5746f6b656e04a265ba2eff4d829058fb3f0f2496ba";

constexpr const char* kV2ClientProtected =
    "d76b3343cf088394c8f03e5157080000449ea0c95e82ffe67b6abcdb4298b485dd04de806071bf03dceebfa162e75d6c9605"
    "8bdbfb127cdfcbf903388e99ad049f9a3dd4425ae4d0992cfff18ecf0fdb5a842d09747052f17ac2053d21f57c5d250f2c4f"
    "0e0202b70785b7946e992e58a59ac52dea6774d4f03b55545243cf1a12834e3f249a78d395e0d18f4d766004f1a2674802a7"
    "47eaa901c3f10cda5500cb9122faa9f1df66c392079a1b40f0de1c6054196a11cbea40afb6ef5253cd6818f6625efce3b6de"
    "f6ba7e4b37a40f7732e093daa7d52190935b8da58976ff3312ae50b187c1433c0f028edcc4c2838b6a9bfc226ca4b4530e7a"
    "4ccee1bfa2a3d396ae5a3fb512384b2fdd851f784a65e03f2c4fbe11a53c7777c023462239dd6f7521a3f6c7d5dd3ec9b3f2"
    "33773d4b46d23cc375eb198c63301c21801f6520bcfb7966fc49b393f0061d974a2706df8c4a9449f11d7f3d2dcbb90c6b87"
    "7045636e7c0c0fe4eb0f697545460c806910d2c355f1d253bc9d2452aaa549e27a1fac7cf4ed77f322e8fa894b6a83810a34"
    "b361901751a6f5eb65a0326e07de7c1216ccce2d0193f958bb3850a833f7ae432b65bc5a53975c155aa4bcb4f7b2c4e54df1"
    "6efaf6ddea94e2c50b4cd1dfe06017e0e9d02900cffe1935e0491d77ffb4fdf85290fdd893d577b1131a610ef6a5c32b2ee0"
    "293617a37cbb08b847741c3b8017c25ca9052ca1079d8b78aebd47876d330a30f6a8c6d61dd1ab5589329de714d19d61370f"
    "8149748c72f132f0fc99f34d766c6938597040d8f9e2bb522ff99c63a344d6a2ae8aa8e51b7b90a4a806105fcbca31506c44"
    "6151adfeceb51b91abfe43960977c87471cf9ad4074d30e10d6a7f03c63bd5d4317f68ff325ba3bd80bf4dc8b52a0ba03175"
    "8022eb025cdd770b44d6d6cf0670f4e990b22347a7db848265e3e5eb72dfe8299ad7481a408322cac55786e52f633b2fb6b6"
    "14eaed18d703dd84045a274ae8bfa73379661388d6991fe39b0d93debb41700b41f90a15c4d526250235ddcd6776fc77bc97"
    "e7a417ebcb31600d01e57f32162a8560cacc7e27a096d37a1a86952ec71bd89a3e9a30a2a26162984d7740f81193e8238e61"
    "f6b5b984d4d3dfa033c1bb7e4f0037febf406d91c0dccf32acf423cfa1e7071010d3f270121b493ce85054ef58bada423101"
    "38fe081adb04e2bd901f2f13458b3d6758158197107c14ebb193230cd1157380aa79cae1374a7c1e5bbcb80ee23e06ebfde2"
    "06bfb0fcbc0edc4ebec309661bdd908d532eb0c6adc38b7ca7331dce8dfce39ab71e7c32d318d136b6100671a1ae6a6600e3"
    "899f31f0eed19e3417d134b90c9058f8632c798d4490da4987307cba922d61c39805d072b589bd52fdf1e86215c2d54e6670"
    "e07383a27bbffb5addf47d66aa85a0c6f9f32e59d85a44dd5d3b22dc2be80919b490437ae4f36a0ae55edf1d0b5cb4e9a3ec"
    "abee93dfc6e38d209d0fa6536d27a5d6fbb17641cde27525d61093f1b28072d111b2b4ae5f89d5974ee12e5cf7d5da4d6a31"
    "123041f33e61407e76cffcdcfd7e19ba58cf4b536f4c4938ae79324dc402894b44faf8afbab35282ab659d13c93f70412e85"
    "cb199a37ddec600545473cfb5a05e08d0b209973b2172b4d21fb69745a262ccde96ba18b2faa745b6fe189cf772a9f84cbfc";

constexpr const char* kV2ServerProtected =
    "dc6b3343cf0008f067a5502a4262b5004075d92faaf16f05d8a4398c47089698baeea26b91eb761d9b89237bbf8726301791"
    "5358230035f7fd3945d88965cf17f9af6e16886c61bfc703106fbaf3cb4cfa52382dd16a393e42757507698075b2c984c707"
    "f0a0812d8cd5a6881eaf21ceda98f4bd23f6fe1a3e2c43edd9ce7ca84bed8521e2e140";

constexpr const char* kV2Retry =
    "cf6b3343cf0008f067a5502a4262b5746f6b656ec8646ce8bfe33952d955543665dcc7b6";

constexpr const char* kClientCryptoFrame =
    "060040f1010000ed0303ebf8fa56f12939b9584a3896472ec40bb863cfd3e86804fe3a47f06a2b69484c0000041301130201"
    "0000c000000010000e00000b6578616d706c652e636f6dff01000100000a00080006001d0017001800100007000504616c70"
    "6e000500050100000000003300260024001d00209370b2c9caa47fbabaf4559fedba753de171fa71f50f1ce15d43e994ec74"
    "d748002b0003020304000d0010000e0403050306030203080408050806002d00020101001c00024001003900320408ffffff"
    "ffffffffff05048000ffff07048000ffff0801100104800075300901100f088394c8f03e51570806048000ffff";

constexpr const char* kServerPayload =
    "02000000000600405a020000560303eefce7f7b37ba1d1632e96677825ddf73988cfc79825df566dc5430b9a045a12001301"
    "00002e00330024001d00209d3c940d89690b84d08a60993c144eca684d1081287c834d5311bcf32bb9da1a002b00020304";

constexpr const char* kClientDcid           = "8394c8f03e515708";
constexpr const char* kV1ClientSecret       = "c00cf151ca5be075ed0ebfb5c80323c42d6b7db67881289af4008f1f6c357aea";
constexpr const char* kV1ServerSecret       = "3c199828fd139efd216c155ad844cc81fb82fa8d7446fa7d78be803acdda951b";
constexpr const char* kV2ClientSecret       = "14ec9d6eb9fd7af83bf5a668bc17a7e283766aade7ecd0891f70f9ff7f4bf47b";
constexpr const char* kV2ServerSecret       = "0263db1782731bf4588e7e4d93b7463907cb8cd8200b5da55a8bd488eafc37c1";
constexpr const char* kV1ClientHeader       = "c300000001088394c8f03e5157080000449e00000002";
constexpr const char* kV2ClientHeader       = "d36b3343cf088394c8f03e5157080000449e00000002";
constexpr const char* kV1ServerHeader       = "c1000000010008f067a5502a4262b50040750001";
constexpr const char* kV2ServerHeader       = "d16b3343cf0008f067a5502a4262b50040750001";
constexpr const char* kChachaSecret         = "9ac312a7f877468ebe69422748ad00a15443f18203a07d6060f688f30f21632b";
constexpr const char* kV1ChachaNextSecret   = "1223504755036d556342ee9361d253421a826c9ecdf3c7148684b36b714881f9";
constexpr const char* kV2ChachaNextSecret   = "c69374c49e3d2a9466fa689e49d476db5d0dfbc87d32ceeaa6343fd0ae4c7d88";
constexpr const char* kV1ChachaPacket       = "4cfe4189655e5cd55c41f69080575d7999c25a5bfb";
constexpr const char* kV2ChachaPacket       = "5558b1c60ae7b6b932bc27d786f4bc2bb20f2162ba";
constexpr u64         kChachaPacketNumber   = 654360564U;
constexpr usize       kClientPayloadBytes   = 1162U;

struct InitialSample
{
    QuicVersion version;
    const char* client_secret;
    const char* server_secret;
    const char* client_header;
    const char* client_protected;
    const char* server_header;
    const char* server_protected;
    const char* retry;
    const char* chacha_next_secret;
    const char* chacha_packet;
};

constexpr InitialSample kSamples[] = {
    {QuicVersion::V1, kV1ClientSecret, kV1ServerSecret, kV1ClientHeader, kV1ClientProtected, kV1ServerHeader,
     kV1ServerProtected, kV1Retry, kV1ChachaNextSecret, kV1ChachaPacket},
    {QuicVersion::V2, kV2ClientSecret, kV2ServerSecret, kV2ClientHeader, kV2ClientProtected, kV2ServerHeader,
     kV2ServerProtected, kV2Retry, kV2ChachaNextSecret, kV2ChachaPacket},
};

constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};

Bytes concat(ConstSpan<u8> a, ConstSpan<u8> b, usize total)
{
    Bytes out;
    out.resize(total);
    std::memset(out.data(), 0, total);
    std::memcpy(out.data(), a.data(), a.size());
    std::memcpy(out.data() + a.size(), b.data(), b.size());
    return out;
}

Bytes initial_secret(QuicVersion version, QuicSide side)
{
    Bytes secret;
    secret.resize(kQuicInitialSecretBytes);
    REQUIRE(quic_initial_secret(version, side, from_hex(kClientDcid), secret) == CryptoError::None);
    return secret;
}

// Seals and header-protects `header` + `payload` (zero padding up to the length of `expected`) and compares it with
// `expected`, then removes the protection, decodes the packet number (the receiver's largest is the one before) and
// opens it back to the plaintext.
void protect_and_read_back(QuicVersion version, AlgorithmId aead, ConstSpan<u8> secret, ConstSpan<u8> header,
                           ConstSpan<u8> payload, usize packet_number_offset, u64 packet_number, ConstSpan<u8> expected)
{
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(version, aead, secret) == CryptoError::None);
    REQUIRE(hp.init(version, aead, secret) == CryptoError::None);
    Bytes packet = concat(header, payload, expected.size());
    const Bytes plain = packet;
    REQUIRE(key.seal(packet_number, packet, header.size()) == CryptoError::None);
    CHECK(key.sealed() == 1U);
    REQUIRE(hp.protect(packet, packet_number_offset) == CryptoError::None);
    CHECK(equal(packet, expected));

    usize     number_bytes    = 0U;
    u64       truncated       = 0U;
    const u64 expected_number = packet_number; // the sample's own number: the closest candidate is itself
    REQUIRE(hp.unprotect(packet, packet_number_offset, number_bytes, truncated) == CryptoError::None);
    CHECK(number_bytes == header.size() - packet_number_offset);
    const u64 decoded = quic_decode_packet_number(truncated, number_bytes, expected_number);
    CHECK(decoded == packet_number);
    CHECK(equal(ConstSpan<u8>(packet).first(header.size()), header));
    REQUIRE(key.open(decoded, packet, header.size()) == CryptoError::None);
    CHECK(equal(ConstSpan<u8>(packet).first(packet.size() - kQuicTagBytes),
                ConstSpan<u8>(plain).first(plain.size() - kQuicTagBytes)));
}
} // namespace

TEST_CASE("QUIC: Initial secrets of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    for (const InitialSample& sample : kSamples)
    {
        CHECK(equal(initial_secret(sample.version, QuicSide::Client), from_hex(sample.client_secret)));
        CHECK(equal(initial_secret(sample.version, QuicSide::Server), from_hex(sample.server_secret)));
    }
    CHECK(quic_version_supported(0x00000001U));
    CHECK(quic_version_supported(0x6B3343CFU));
    CHECK_FALSE(quic_version_supported(0U));             // version negotiation
    CHECK_FALSE(quic_version_supported(0x1A2A3A4AU));    // a reserved (greased) version
    CHECK_FALSE(quic_version_supported(0xFF00001DU));    // draft-29

    u8 secret[kQuicInitialSecretBytes];
    u8 cid[kQuicMaxConnectionIdBytes + 1U] = {};
    CHECK(quic_initial_secret(static_cast<QuicVersion>(0xFF00001DU), QuicSide::Client, ConstSpan<u8>(cid, 8U),
                              secret) == CryptoError::Unsupported);
    CHECK(quic_initial_secret(QuicVersion::V1, QuicSide::Client, cid, secret) == CryptoError::BadLength);
    CHECK(quic_initial_secret(QuicVersion::V1, QuicSide::Client, ConstSpan<u8>(cid, 8U), Span<u8>(secret, 31U)) ==
          CryptoError::BadLength);
    // Every length from the empty connection ID up to 20 bytes is a valid Initial (RFC 9000 section 7.2).
    for (usize length = 0U; length <= kQuicMaxConnectionIdBytes; ++length)
    {
        CHECK(quic_initial_secret(QuicVersion::V2, QuicSide::Server, ConstSpan<u8>(cid, length), secret) ==
              CryptoError::None);
    }
}

TEST_CASE("QUIC: the client's and the server's Initial packets of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        protect_and_read_back(sample.version, AlgorithmId::Aes128Gcm, initial_secret(sample.version, QuicSide::Client),
                              from_hex(sample.client_header), from_hex(kClientCryptoFrame), 18U, 2U,
                              from_hex(sample.client_protected));
        CHECK(from_hex(sample.client_protected).size() == 22U + kClientPayloadBytes + kQuicTagBytes);
        protect_and_read_back(sample.version, AlgorithmId::Aes128Gcm, initial_secret(sample.version, QuicSide::Server),
                              from_hex(sample.server_header), from_hex(kServerPayload), 18U, 1U,
                              from_hex(sample.server_protected));
    }
}

TEST_CASE("QUIC: the ChaCha20-Poly1305 short header packet and its key update", "[crypto][net-2][quic]")
{
    const Bytes secret = from_hex(kChachaSecret);
    const Bytes header = from_hex("4200bff4");
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        const u8 ping = 0x01U;
        protect_and_read_back(sample.version, AlgorithmId::Chacha20Poly1305, secret, header, ConstSpan<u8>(&ping, 1U),
                              1U, kChachaPacketNumber, from_hex(sample.chacha_packet));
        // The next key phase seals exactly as a key made from RFC's "ku" secret, and differs from the first.
        QuicPacketKey first;
        QuicPacketKey next;
        QuicPacketKey expected;
        REQUIRE(first.init(sample.version, AlgorithmId::Chacha20Poly1305, secret) == CryptoError::None);
        REQUIRE(next.update(first) == CryptoError::None);
        REQUIRE(expected.init(sample.version, AlgorithmId::Chacha20Poly1305, from_hex(sample.chacha_next_secret)) ==
                CryptoError::None);
        CHECK(next.version() == sample.version);
        CHECK(next.algorithm() == AlgorithmId::Chacha20Poly1305);
        Bytes a = concat(header, ConstSpan<u8>(&ping, 1U), 21U);
        Bytes b = a;
        Bytes c = a;
        REQUIRE(next.seal(kChachaPacketNumber, a, 4U) == CryptoError::None);
        REQUIRE(expected.seal(kChachaPacketNumber, b, 4U) == CryptoError::None);
        REQUIRE(first.seal(kChachaPacketNumber, c, 4U) == CryptoError::None);
        CHECK(equal(a, b));
        CHECK_FALSE(equal(a, c));
        // A packet of the old phase does not open under the new one: the receiver keeps both while packets reorder.
        CHECK(next.open(kChachaPacketNumber, c, 4U) == CryptoError::Forged);
    }
}

TEST_CASE("QUIC: Retry integrity tags of RFC 9001 and RFC 9369", "[crypto][net-2][quic]")
{
    const Bytes dcid = from_hex(kClientDcid);
    for (const InitialSample& sample : kSamples)
    {
        CAPTURE(static_cast<u32>(sample.version));
        const Bytes retry = from_hex(sample.retry);
        CHECK(quic_retry_verify(sample.version, dcid, retry) == CryptoError::None);
        u8 tag[kQuicTagBytes];
        REQUIRE(quic_retry_tag(sample.version, dcid, ConstSpan<u8>(retry).first(retry.size() - kQuicTagBytes), tag) ==
                CryptoError::None);
        CHECK(equal(tag, ConstSpan<u8>(retry).last(kQuicTagBytes)));

        // Every bit of the packet, and of the original connection ID it does not carry, is authenticated.
        for (usize bit = 0U; bit < retry.size() * 8U; ++bit)
        {
            Bytes changed = retry;
            changed[bit / 8U] ^= static_cast<u8>(1U << (bit % 8U));
            CHECK(quic_retry_verify(sample.version, dcid, changed) == CryptoError::Forged);
        }
        Bytes other = dcid;
        other[7] ^= 0x01U;
        CHECK(quic_retry_verify(sample.version, other, retry) == CryptoError::Forged);
        CHECK(quic_retry_verify(sample.version, ConstSpan<u8>(dcid).first(7U), retry) == CryptoError::Forged);
        const QuicVersion wrong = sample.version == QuicVersion::V1 ? QuicVersion::V2 : QuicVersion::V1;
        CHECK(quic_retry_verify(wrong, dcid, retry) == CryptoError::Forged);
    }

    u8 big[kQuicMaxRetryBytes + 1U] = {};
    u8 cid[kQuicMaxConnectionIdBytes + 1U] = {};
    u8 tag[kQuicTagBytes];
    CHECK(quic_retry_verify(QuicVersion::V1, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, kQuicTagBytes - 1U)) ==
          CryptoError::BadLength);
    CHECK(quic_retry_verify(QuicVersion::V1, ConstSpan<u8>(cid, 8U), big) == CryptoError::BadLength);
    CHECK(quic_retry_verify(QuicVersion::V1, cid, ConstSpan<u8>(big, 40U)) == CryptoError::BadLength);
    CHECK(quic_retry_verify(static_cast<QuicVersion>(7U), ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, 40U)) ==
          CryptoError::Unsupported);
    // The longest Retry and the longest connection ID fit the pseudo-packet; one byte more is refused.
    REQUIRE(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, kQuicMaxConnectionIdBytes),
                           ConstSpan<u8>(big, kQuicMaxRetryBytes - kQuicTagBytes), tag) == CryptoError::None);
    std::memcpy(big + kQuicMaxRetryBytes - kQuicTagBytes, tag, kQuicTagBytes);
    CHECK(quic_retry_verify(QuicVersion::V2, ConstSpan<u8>(cid, kQuicMaxConnectionIdBytes),
                            ConstSpan<u8>(big, kQuicMaxRetryBytes)) == CryptoError::None);
    std::memset(tag, 0xAA, sizeof(tag));
    CHECK(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, kQuicMaxRetryBytes), tag) ==
          CryptoError::BadLength);
    CHECK(all_zero(tag));
    CHECK(quic_retry_tag(QuicVersion::V2, ConstSpan<u8>(cid, 8U), ConstSpan<u8>(big, 20U), Span<u8>(tag, 15U)) ==
          CryptoError::BadLength);
}

TEST_CASE("QUIC: packet number encoding and decoding of RFC 9000", "[crypto][net-2][quic]")
{
    // Appendix A.2: 0xac5c02 after 0xabe8b3 was acknowledged needs 16 bits; 0xace8fe needs 24.
    CHECK(quic_packet_number_bytes(0xAC5C02U, 0xABE8B3U, true) == 2U);
    CHECK(quic_packet_number_bytes(0xACE8FEU, 0xABE8B3U, true) == 3U);
    // Appendix A.3: 0x9b32 after 0xa82f30ea decodes to 0xa82f9b32.
    CHECK(quic_decode_packet_number(0x9B32U, 2U, 0xA82F30EBU) == 0xA82F9B32U);

    // Twice the unacknowledged range: up to 2^(8n-1) packets fit n bytes.
    CHECK(quic_packet_number_bytes(0U, 0U, false) == 1U);
    CHECK(quic_packet_number_bytes(127U, 0U, false) == 1U);
    CHECK(quic_packet_number_bytes(128U, 0U, false) == 2U);
    CHECK(quic_packet_number_bytes(128U, 0U, true) == 1U);
    CHECK(quic_packet_number_bytes(129U, 0U, true) == 2U);
    CHECK(quic_packet_number_bytes(1000U + (u64{1} << 23U), 1000U, true) == 3U);
    CHECK(quic_packet_number_bytes(1001U + (u64{1} << 23U), 1000U, true) == 4U);
    CHECK(quic_packet_number_bytes(1000U + (u64{1} << 31U), 1000U, true) == 4U);
    CHECK(quic_packet_number_bytes(1001U + (u64{1} << 31U), 1000U, true) == 0U); // no encoding reaches it
    CHECK(quic_packet_number_bytes(kQuicMaxPacketNumber, kQuicMaxPacketNumber - 1U, true) == 1U);
    CHECK(quic_packet_number_bytes(kQuicMaxPacketNumber + 1U, kQuicMaxPacketNumber, true) == 0U);
    CHECK(quic_packet_number_bytes(5U, 5U, true) == 0U);
    CHECK(quic_packet_number_bytes(4U, 5U, true) == 0U);

    // Every encodable distance round-trips, at the edges of each window and of the packet number space.
    const u64 largest[] = {0U, 1U, 126U, 127U, 128U, 255U, 256U, 0xFFFFU, 0x10000U, 0xA82F30EAU,
                           kQuicMaxPacketNumber - (u64{1} << 31U), kQuicMaxPacketNumber - 2U};
    for (const u64 acked : largest)
    {
        for (usize bytes = 1U; bytes <= 4U; ++bytes)
        {
            const u64 half = u64{1} << (8U * bytes - 1U);
            for (const u64 ahead : {u64{1}, u64{2}, half - 1U, half})
            {
                const u64 number = acked + ahead;
                if (number > kQuicMaxPacketNumber)
                {
                    continue;
                }
                const usize length = quic_packet_number_bytes(number, acked, true);
                REQUIRE(length != 0U);
                CHECK(length <= bytes);
                const u64 mask = (u64{1} << (8U * length)) - 1U;
                // The receiver's largest received is at least the sender's largest acknowledged.
                CHECK(quic_decode_packet_number(number & mask, length, acked + 1U) == number);
            }
        }
    }
    // At exactly half a window either way the higher candidate is kept.
    CHECK(quic_decode_packet_number(0x00U, 1U, 0x80U) == 0x100U);
    CHECK(quic_decode_packet_number(0x80U, 1U, 0x100U) == 0x180U);
    CHECK(quic_decode_packet_number(0x80U, 1U, 0x00U) == 0x80U);
    CHECK(quic_decode_packet_number(0x7FU, 1U, 0x100U) == 0x17FU);
    // Near zero and at the top the candidate never leaves [0, 2^62).
    CHECK(quic_decode_packet_number(0xFFU, 1U, 0U) == 0xFFU);
    CHECK(quic_decode_packet_number(0x00U, 1U, kQuicMaxPacketNumber) == kQuicMaxPacketNumber - 0xFFU);
    CHECK(quic_decode_packet_number(0xFFFFFFFFU, 4U, kQuicMaxPacketNumber) == kQuicMaxPacketNumber);
    CHECK(quic_decode_packet_number(0x1234U, 0U, 77U) == 0x1234U);
    CHECK(quic_decode_packet_number(0x1234U, 5U, 77U) == 0x1234U);
}

TEST_CASE("QUIC: packet keys refuse what is outside their contract", "[crypto][net-2][quic]")
{
    u8            secret32[32] = {1, 2, 3};
    u8            secret48[48] = {4, 5, 6};
    QuicPacketKey key;
    QuicHeaderKey hp;
    u8            packet[64] = {};
    CHECK_FALSE(key.ready());
    CHECK(key.seal(0U, packet, 8U) == CryptoError::NotReady);
    CHECK(key.open(0U, packet, 8U) == CryptoError::NotReady);
    usize bytes = 9U;
    u64   truncated = 9U;
    CHECK(hp.protect(packet, 8U) == CryptoError::NotReady);
    CHECK(hp.unprotect(packet, 8U, bytes, truncated) == CryptoError::NotReady);
    CHECK((bytes == 0U && truncated == 0U));
    u8 mask[kQuicMaskBytes];
    CHECK(hp.mask(ConstSpan<u8>(packet, kQuicSampleBytes), mask) == CryptoError::NotReady);

    // Suites: the hash follows the AEAD; any other algorithm or version is refused and leaves no key.
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret48) == CryptoError::BadLength);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes256Gcm, secret32) == CryptoError::BadLength);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::Aes256Gcm, secret48) == CryptoError::None);
    CHECK(key.init(QuicVersion::V1, AlgorithmId::HkdfSha256, secret32) == CryptoError::Unsupported);
    CHECK_FALSE(key.ready());
    CHECK(key.init(static_cast<QuicVersion>(0xFF00001DU), AlgorithmId::Aes128Gcm, secret32) ==
          CryptoError::Unsupported);
    CHECK(hp.init(QuicVersion::V2, AlgorithmId::Chacha20Poly1305, secret48) == CryptoError::BadLength);
    CHECK(hp.init(QuicVersion::V2, AlgorithmId::Siphash24, secret32) == CryptoError::Unsupported);
    CHECK_FALSE(hp.ready());

    REQUIRE(key.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret32) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret32) == CryptoError::None);
    CHECK(key.seal(kQuicMaxPacketNumber + 1U, packet, 8U) == CryptoError::BadLength);
    CHECK(key.open(kQuicMaxPacketNumber + 1U, packet, 8U) == CryptoError::BadLength);
    CHECK(key.seal(0U, packet, 65U) == CryptoError::BadLength);
    CHECK(key.seal(0U, packet, 64U - kQuicTagBytes + 1U) == CryptoError::BadLength);
    CHECK(key.open(0U, Span<u8>(packet, 10U), 0U) == CryptoError::BadLength);
    CHECK(key.sealed() == 0U);
    CHECK(key.seal(kQuicMaxPacketNumber, packet, 64U - kQuicTagBytes) == CryptoError::None); // an empty payload
    CHECK(key.open(kQuicMaxPacketNumber, packet, 64U - kQuicTagBytes) == CryptoError::None);

    // Header protection needs the 16-byte sample 4 bytes past the packet number's start.
    CHECK(hp.protect(packet, 0U) == CryptoError::BadLength);
    CHECK(hp.protect(Span<u8>(packet, 8U + 4U + kQuicSampleBytes - 1U), 8U) == CryptoError::BadLength);
    CHECK(hp.protect(Span<u8>(packet, 8U + 4U + kQuicSampleBytes), 8U) == CryptoError::None);
    CHECK(hp.protect(packet, 65U) == CryptoError::BadLength);
    CHECK(hp.mask(ConstSpan<u8>(packet, 15U), mask) == CryptoError::BadLength);
    CHECK(hp.mask(ConstSpan<u8>(packet, 16U), Span<u8>(mask, 4U)) == CryptoError::BadLength);

    // Updates: from nothing, onto itself.
    QuicPacketKey empty;
    QuicPacketKey next;
    CHECK(next.update(empty) == CryptoError::NotReady);
    CHECK(key.update(key) == CryptoError::Overlap);
    CHECK(key.ready());
    REQUIRE(next.update(key) == CryptoError::None);
    CHECK(next.version() == QuicVersion::V2);
    key.clear();
    CHECK_FALSE(key.ready());
    CHECK(key.sealed() == 0U);
    hp.clear();
    CHECK_FALSE(hp.ready());
}

TEST_CASE("QUIC: usage limits refuse a packet past the key's limit", "[crypto][net-2][quic]")
{
    CHECK(quic_confidentiality_limit(AlgorithmId::Aes128Gcm) == (u64{1} << 23U));
    CHECK(quic_confidentiality_limit(AlgorithmId::Aes256Gcm) == (u64{1} << 23U));
    CHECK(quic_confidentiality_limit(AlgorithmId::Chacha20Poly1305) == kQuicMaxPacketNumber + 1U);
    CHECK(quic_confidentiality_limit(AlgorithmId::HkdfSha256) == 0U);
    CHECK(quic_integrity_limit(AlgorithmId::Aes128Gcm) == (u64{1} << 52U));
    CHECK(quic_integrity_limit(AlgorithmId::Aes256Gcm) == (u64{1} << 52U));
    CHECK(quic_integrity_limit(AlgorithmId::Chacha20Poly1305) == (u64{1} << 36U));
    CHECK(quic_integrity_limit(AlgorithmId::Ed25519) == 0U);

    const u8      secret[32] = {7};
    QuicPacketKey key;
    key.set_limit(5U); // no key: ignored
    CHECK(key.limit() == 0U);
    REQUIRE(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    CHECK(key.limit() == (u64{1} << 23U));
    key.set_limit(u64{1} << 40U); // never above the AEAD's limit
    CHECK(key.limit() == (u64{1} << 23U));
    key.set_limit(0U);
    CHECK(key.limit() == 1U);
    key.set_limit(8U);
    u8 packet[40] = {};
    for (u64 n = 0U; n < 8U; ++n)
    {
        CHECK(key.update_due() == (n >= 6U));
        REQUIRE(key.seal(n, packet, 8U) == CryptoError::None);
    }
    CHECK(key.update_due());
    u8 before[40];
    std::memcpy(before, packet, sizeof(packet));
    CHECK(key.seal(8U, packet, 8U) == CryptoError::Exhausted);
    CHECK(std::memcmp(before, packet, sizeof(packet)) == 0);
    CHECK(key.sealed() == 8U);
    CHECK(key.open(7U, packet, 8U) == CryptoError::None); // opening is never limited
    QuicPacketKey next;
    REQUIRE(next.update(key) == CryptoError::None);
    CHECK(next.sealed() == 0U);
    CHECK(next.limit() == (u64{1} << 23U));
    CHECK_FALSE(next.update_due());
}

TEST_CASE("QUIC: a changed byte anywhere in a protected packet is refused", "[crypto][net-2][quic]")
{
    const Bytes secret = initial_secret(QuicVersion::V1, QuicSide::Server);
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    const Bytes protected_packet = from_hex(kV1ServerProtected);
    for (usize at = 0U; at < protected_packet.size(); ++at)
    {
        Bytes packet = protected_packet;
        packet[at] ^= 0x40U;
        usize bytes = 0U;
        u64   truncated = 0U;
        REQUIRE(hp.unprotect(packet, 18U, bytes, truncated) == CryptoError::None);
        const usize header = 18U + bytes;
        const u64   number = quic_decode_packet_number(truncated, bytes, 0U);
        if (header > packet.size() - kQuicTagBytes)
        {
            continue; // a changed length bit made the header swallow the payload
        }
        CHECK(key.open(number, packet, header) == CryptoError::Forged);
        CHECK(all_zero(ConstSpan<u8>(packet).subspan(header, packet.size() - header - kQuicTagBytes)));
    }
}

TEST_CASE("QUIC: random packets round-trip in every suite and version", "[crypto][net-2][quic]")
{
    u8 secret[kQuicMaxSecretBytes];
    u8 buffer[1500];
    u8 original[1500];
    for (const QuicVersion version : {QuicVersion::V1, QuicVersion::V2})
    {
        for (const AlgorithmId aead : kAeads)
        {
            const usize secret_bytes = aead == AlgorithmId::Aes256Gcm ? 48U : 32U;
            REQUIRE(random_bytes(Span<u8>(secret, secret_bytes)) == CryptoError::None);
            QuicPacketKey key;
            QuicHeaderKey hp;
            REQUIRE(key.init(version, aead, ConstSpan<u8>(secret, secret_bytes)) == CryptoError::None);
            REQUIRE(hp.init(version, aead, ConstSpan<u8>(secret, secret_bytes)) == CryptoError::None);
            u64 largest_acked = 0U;
            u64 number        = 0U;
            for (usize round = 0U; round < 200U; ++round)
            {
                u8 random[8];
                REQUIRE(random_bytes(random) == CryptoError::None);
                number += 1U + random[0] % 50U;
                if (random[1] % 4U == 0U)
                {
                    largest_acked = number - 1U - random[2] % (number < 64U ? number : 64U);
                }
                const usize length = quic_packet_number_bytes(number, largest_acked, round != 0U);
                REQUIRE(length != 0U);
                const bool  long_header = (random[3] & 1U) != 0U;
                const usize pn_offset   = long_header ? 1U + 4U + 1U + 8U + 1U + 8U + 2U : 1U + 8U;
                const usize header      = pn_offset + length;
                const usize size        = header + 4U + random[4] + (random[5] & 3U) * 256U + kQuicTagBytes;
                REQUIRE(random_bytes(Span<u8>(buffer, size)) == CryptoError::None);
                buffer[0] = static_cast<u8>((long_header ? 0xC0U : 0x40U) | (random[6] & 0x3CU) | (length - 1U));
                for (usize i = 0U; i < length; ++i)
                {
                    buffer[pn_offset + i] = static_cast<u8>(number >> (8U * (length - 1U - i)));
                }
                std::memcpy(original, buffer, size);
                REQUIRE(key.seal(number, Span<u8>(buffer, size), header) == CryptoError::None);
                REQUIRE(hp.protect(Span<u8>(buffer, size), pn_offset) == CryptoError::None);
                CHECK((buffer[0] & (long_header ? 0xF0U : 0xE0U)) == (original[0] & (long_header ? 0xF0U : 0xE0U)));

                usize bytes = 0U;
                u64   truncated = 0U;
                REQUIRE(hp.unprotect(Span<u8>(buffer, size), pn_offset, bytes, truncated) == CryptoError::None);
                CHECK(bytes == length);
                const u64 decoded = quic_decode_packet_number(truncated, bytes, largest_acked + 1U);
                CHECK(decoded == number);
                REQUIRE(key.open(decoded, Span<u8>(buffer, size), header) == CryptoError::None);
                CHECK(std::memcmp(buffer, original, size - kQuicTagBytes) == 0);
            }
        }
    }
}

TEST_CASE("QUIC: one packet key and one header key serve several threads", "[crypto][net-2][quic]")
{
    const Bytes   secret = initial_secret(QuicVersion::V2, QuicSide::Client);
    QuicPacketKey key;
    QuicHeaderKey hp;
    REQUIRE(key.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V2, AlgorithmId::Aes128Gcm, secret) == CryptoError::None);
    const Bytes protected_packet = from_hex(kV2ClientProtected);
    const Bytes expected_header  = from_hex(kV2ClientHeader);
    bool        ok[4]            = {};
    std::thread threads[4];
    for (usize t = 0U; t < 4U; ++t)
    {
        threads[t] = std::thread(
            [&, t]
            {
                bool good = true;
                for (usize i = 0U; i < 50U; ++i)
                {
                    Bytes packet = protected_packet;
                    usize bytes  = 0U;
                    u64   truncated = 0U;
                    good = good && hp.unprotect(packet, 18U, bytes, truncated) == CryptoError::None &&
                           key.open(truncated, packet, 18U + bytes) == CryptoError::None &&
                           equal(ConstSpan<u8>(packet).first(22U), expected_header);
                }
                ok[t] = good;
            });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    CHECK((ok[0] && ok[1] && ok[2] && ok[3]));
}

TEST_CASE("QUIC: a packet number space's window opens each packet once and a forged packet moves nothing",
          "[crypto][net-2][quic][replay]")
{
    // A sender's 600 short header packets, delivered with local reordering, every seventh one twice, every fiftieth
    // again 300 packets later, and a forged packet claiming a far higher number. The receiver unprotects the header,
    // decodes the number against its window, checks it before the AEAD and accepts it after (RFC 9000 section 12.3).
    constexpr usize count = 600U;
    constexpr usize size  = 1U + 8U + 2U + 40U + kQuicTagBytes; // a 2-byte packet number
    constexpr usize pn_at = 1U + 8U;
    u8              secret[32];
    REQUIRE(random_bytes(Span<u8>(secret)) == CryptoError::None);
    QuicPacketKey sender;
    QuicPacketKey receiver;
    QuicHeaderKey hp;
    REQUIRE(sender.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    REQUIRE(receiver.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    REQUIRE(hp.init(QuicVersion::V1, AlgorithmId::Aes128Gcm, ConstSpan<u8>(secret)) == CryptoError::None);
    static u8 packets[count][size];
    for (usize n = 0U; n < count; ++n)
    {
        u8* const packet = packets[n];
        for (usize i = 0U; i < size; ++i)
        {
            packet[i] = static_cast<u8>(n + 3U * i);
        }
        packet[0]         = 0x41U;
        packet[pn_at]     = static_cast<u8>(n >> 8U);
        packet[pn_at + 1] = static_cast<u8>(n);
        REQUIRE(sender.seal(n, Span<u8>(packet, size), pn_at + 2U) == CryptoError::None);
        REQUIRE(hp.protect(Span<u8>(packet, size), pn_at) == CryptoError::None);
    }

    QuicPacketWindow window;
    usize            opened    = 0U;
    usize            dropped   = 0U;
    bool             seen[count] = {};
    // One received datagram: true when its packet is processed.
    const auto receive = [&](const u8* datagram) -> bool
    {
        u8 packet[size];
        std::memcpy(packet, datagram, size);
        usize bytes     = 0U;
        u64   truncated = 0U;
        REQUIRE(hp.unprotect(Span<u8>(packet), pn_at, bytes, truncated) == CryptoError::None);
        const u64 number = quic_decode_packet_number(truncated, bytes, window.any() ? window.largest() + 1U : 0U);
        if (window.check(number) != ReplayVerdict::Fresh)
        {
            ++dropped;
            return false;
        }
        ++opened;
        if (receiver.open(number, Span<u8>(packet), pn_at + bytes) != CryptoError::None)
        {
            return false;
        }
        REQUIRE(window.accept(number) == ReplayVerdict::Fresh);
        REQUIRE(number < count);
        CHECK_FALSE(seen[number]);
        seen[number] = true;
        return true;
    };

    usize expected_drops = 0U;
    for (usize block = 0U; block < count; block += 8U)
    {
        // Each run of eight arrives in a fixed shuffled order.
        constexpr usize order[8] = {2U, 0U, 1U, 5U, 3U, 7U, 4U, 6U};
        for (const usize at : order)
        {
            const usize n = block + at;
            CHECK(receive(packets[n]));
            if (n % 7U == 0U)
            {
                CHECK_FALSE(receive(packets[n])); // duplicated on the path
                ++expected_drops;
            }
            if (n >= 300U && (n - 300U) % 50U == 0U)
            {
                CHECK_FALSE(receive(packets[n - 300U])); // replayed from 300 packets back: Stale
                ++expected_drops;
            }
        }
        if (block == 296U)
        {
            // A forged packet: header protection under the right key, a number far ahead, a wrong tag.
            u8 forged[size];
            std::memcpy(forged, packets[block], size);
            forged[pn_at] ^= 0x40U;
            forged[size - 1U] ^= 0x01U;
            const u64 before = window.largest();
            CHECK_FALSE(receive(forged));
            CHECK(window.largest() == before);
        }
    }
    CHECK(window.largest() == count - 1U);
    CHECK(dropped == expected_drops);
    CHECK(opened == count + 1U); // every packet once, and the forged one, whose AEAD fails
    for (usize n = 0U; n < count; ++n)
    {
        CHECK(seen[n]);
    }
}
