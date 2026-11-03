// crd-crypto-board -- crd-crypto's boards (NET.2; docs/bench, CRD_BUILD_CRYPTO_VS_PEERS): crd-crypto, the AWS-LC it
// wraps called directly, BoringSSL and OpenSSL 4.0 timed in one process on the same work: AEAD sealing and opening,
// an ephemeral X25519 agreement, ML-KEM-768, Ed25519, ECDSA P-256 and ML-DSA-65 signing and verification, and TLS 1.3
// full handshakes, resumptions and records in memory (board_peer.h holds the operations and the TLS profile).
//
// Usage: crd-crypto-board [--case TEXT] [--budget-ms N] [--rounds N] [--quick] [--check]
//   case       only the cases whose name contains TEXT
//   budget-ms  the time one batch of one arm takes (default 10): the batch's operation count is calibrated to it
//   rounds     batches per arm and case (default 5), the arms rotated each round; an arm's value is their median
//   quick      one operation per batch and one round (the smoke CTest: every case of every arm runs, nothing is timed)
//   check      the checks alone
//
// Every arm's results are checked against every other's before anything is timed: the same AEAD bytes, KEM secrets
// shared across libraries, signatures verified across libraries and tampered ones refused, and each TLS connection's
// group, AEAD and resumption (a check that fails exits 1, as does any failed operation while timing: a faster wrong
// answer is no measurement). A value line is "<arm> <case> <nanoseconds per operation>", which scripts/ab-board.py
// reads (--arm cerid=board:'^cerid (?P<key>\S+) (?P<value>\S+)$').

#include "board_peer.h"

#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>
#include <crd/crypto/verifier.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

[[noreturn]] void fail(const char* what, const char* arm)
{
    std::fprintf(stderr, "crd-crypto-board: %s (%s)\n", what, arm);
    std::exit(1);
}

constexpr AlgorithmId kAeads[]      = {AlgorithmId::Aes128Gcm, AlgorithmId::Aes256Gcm, AlgorithmId::Chacha20Poly1305};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519, AlgorithmId::EcdsaSecp256r1Sha256, AlgorithmId::MlDsa65};
constexpr AlgorithmId kGroups[]     = {AlgorithmId::X25519, AlgorithmId::X25519Mlkem768};

// ---- crd-crypto's arm ----

void* cerid_aead_new(int algorithm, const uint8_t* key)
{
    auto* aead = new (std::nothrow) AeadKey;
    const AlgorithmId id = kAeads[algorithm];
    if (aead != nullptr && aead->init(id, ConstSpan<u8>(key, aead_key_bytes(id))) != CryptoError::None)
    {
        delete aead;
        return nullptr;
    }
    return aead;
}

int cerid_aead_seal(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    return static_cast<AeadKey*>(aead)->seal(ConstSpan<u8>(nonce, kAeadNonceBytes), ConstSpan<u8>(in, size), {},
                                             Span<u8>(out, size + kAeadTagBytes)) == CryptoError::None;
}

int cerid_aead_open(void* aead, const uint8_t* nonce, const uint8_t* in, size_t size, uint8_t* out)
{
    return static_cast<AeadKey*>(aead)->open(ConstSpan<u8>(nonce, kAeadNonceBytes),
                                             ConstSpan<u8>(in, size + kAeadTagBytes), {},
                                             Span<u8>(out, size)) == CryptoError::None;
}

void cerid_aead_free(void* aead)
{
    delete static_cast<AeadKey*>(aead);
}

int cerid_x25519(const uint8_t* peer, uint8_t* secret)
{
    AgreementKey key;
    return key.generate(AlgorithmId::X25519) == CryptoError::None &&
           key.derive(ConstSpan<u8>(peer, 32U), Span<u8>(secret, CRD_BOARD_SECRET_BYTES)) == CryptoError::None;
}

void* cerid_kem_new(uint8_t* public_key)
{
    auto* kem = new (std::nothrow) KemKey;
    if (kem != nullptr && (kem->generate(AlgorithmId::MlKem768) != CryptoError::None ||
                           kem->public_key(Span<u8>(public_key, CRD_BOARD_KEM_PUBLIC_BYTES)) != CryptoError::None))
    {
        delete kem;
        return nullptr;
    }
    return kem;
}

int cerid_kem_encapsulate(const uint8_t* public_key, uint8_t* ciphertext, uint8_t* secret)
{
    return kem_encapsulate(AlgorithmId::MlKem768, ConstSpan<u8>(public_key, CRD_BOARD_KEM_PUBLIC_BYTES),
                           Span<u8>(ciphertext, CRD_BOARD_KEM_CIPHERTEXT_BYTES),
                           Span<u8>(secret, CRD_BOARD_SECRET_BYTES)) == CryptoError::None;
}

int cerid_kem_decapsulate(void* kem, const uint8_t* ciphertext, uint8_t* secret)
{
    return static_cast<KemKey*>(kem)->decapsulate(ConstSpan<u8>(ciphertext, CRD_BOARD_KEM_CIPHERTEXT_BYTES),
                                                  Span<u8>(secret, CRD_BOARD_SECRET_BYTES)) == CryptoError::None;
}

void cerid_kem_free(void* kem)
{
    delete static_cast<KemKey*>(kem);
}

void* cerid_signer_new(int algorithm, uint8_t* public_key, size_t* public_bytes)
{
    auto* signer = new (std::nothrow) SigningKey;
    const AlgorithmId id = kSignatures[algorithm];
    *public_bytes        = signature_sizes(id).public_key;
    if (signer != nullptr && (signer->generate(id) != CryptoError::None ||
                              signer->public_key(Span<u8>(public_key, *public_bytes)) != CryptoError::None))
    {
        delete signer;
        return nullptr;
    }
    return signer;
}

int cerid_sign(void* signer, const uint8_t* message, size_t size, uint8_t* signature, size_t* signature_bytes)
{
    usize written = 0U;
    const bool signed_ok = static_cast<SigningKey*>(signer)->sign(ConstSpan<u8>(message, size),
                                                                  Span<u8>(signature, CRD_BOARD_MAX_SIGNATURE_BYTES),
                                                                  written) == CryptoError::None;
    *signature_bytes = written;
    return signed_ok;
}

int cerid_verify(int algorithm, const uint8_t* public_key, size_t public_bytes, const uint8_t* message, size_t size,
                 const uint8_t* signature, size_t signature_bytes)
{
    const ProviderVerifier verifier;
    return verifier.verify(kSignatures[algorithm], ConstSpan<u8>(public_key, public_bytes),
                           ConstSpan<u8>(message, size),
                           ConstSpan<u8>(signature, signature_bytes)) == Verification::Valid;
}

void cerid_signer_free(void* signer)
{
    delete static_cast<SigningKey*>(signer);
}

// The client's tickets: the newest one, offered only to a resumption (a full handshake takes none, as the peers'
// clients set no session).
class BoardTickets final : public TlsTicketStore
{
public:
    void keep(const TlsBinding& binding, ConstSpan<u8> ticket) noexcept override
    {
        if (ticket.size() <= sizeof(m_ticket))
        {
            m_binding = binding;
            m_bytes   = ticket.size();
            std::memcpy(m_ticket, ticket.data(), ticket.size());
            ++kept;
        }
    }

    ConstSpan<u8> take(const TlsBinding& binding) noexcept override
    {
        if (!offer || m_bytes == 0U || !(binding == m_binding))
        {
            return {};
        }
        std::memcpy(m_handed, m_ticket, m_bytes);
        const usize bytes = m_bytes;
        m_bytes           = 0U;
        return ConstSpan<u8>(m_handed, bytes);
    }

    bool  offer = false;
    usize kept  = 0U;

private:
    TlsBinding m_binding;
    u8         m_ticket[16384] = {};
    u8         m_handed[16384] = {};
    usize      m_bytes         = 0U;
};

struct CeridTls
{
    AlgorithmId   group{};
    SigningKey    key;
    TlsCredential credential;
    KeyId         pin;
    TlsContext    server;
    TlsContext    client;
    BoardTickets  tickets;
    TlsSession    writer;
    TlsSession    reader;
};

u8 g_flight[kTlsMaxBufferedBytes];

bool deliver(TlsSession& from, TlsSession& to)
{
    usize written = 0U;
    return from.take(Span<u8>(g_flight, sizeof(g_flight)), written) == TlsError::None &&
           (written == 0U || to.receive(ConstSpan<u8>(g_flight, written)) == TlsError::None);
}

bool cerid_tls_pair(CeridTls& tls, bool resume, TlsSession& client, TlsSession& server)
{
    tls.tickets.offer = resume;
    if (client.init(tls.client, &tls.tickets) != TlsError::None || server.init(tls.server) != TlsError::None)
    {
        return false;
    }
    for (int round = 0; round < 8 && (client.pending() > 0U || server.pending() > 0U); ++round)
    {
        if (!deliver(client, server) || !deliver(server, client))
        {
            return false;
        }
    }
    const usize kept    = tls.tickets.kept;
    u8          byte    = 0U;
    usize       written = 0U;
    return client.state() == TlsState::Established && server.state() == TlsState::Established &&
           client.read(Span<u8>(&byte, 1U), written) == TlsError::None && written == 0U &&
           tls.tickets.kept == kept + 1U && client.group() == tls.group && client.aead() == AlgorithmId::Aes128Gcm &&
           client.resumed() == resume;
}

void cerid_tls_free(void* tls)
{
    delete static_cast<CeridTls*>(tls);
}

void* cerid_tls_new(int group)
{
    auto* tls = new (std::nothrow) CeridTls;
    if (tls == nullptr)
    {
        return nullptr;
    }
    tls->group = kGroups[group];
    u8 seed[32];
    for (usize i = 0U; i < sizeof(seed); ++i)
    {
        seed[i] = static_cast<u8>(i + 1U);
    }
    static constexpr AlgorithmId kClientAeads[] = {AlgorithmId::Aes128Gcm};
    static constexpr AlgorithmId kAccepted[]    = {AlgorithmId::Ed25519};
    bool ok = tls->key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) == CryptoError::None &&
              tls->credential.init(tls->key, CRD_BOARD_SERVER_NAME) == TlsError::None;
    tls->pin = tls->credential.key_id();

    TlsConfig server;
    server.role                    = TlsRole::Server;
    server.groups                  = ConstSpan<AlgorithmId>(&tls->group, 1U);
    server.aeads                   = kAeads;
    server.credential              = &tls->credential;
    server.server_name             = CRD_BOARD_SERVER_NAME;
    server.certificate_compression = {};
    TlsConfig client;
    client.role                    = TlsRole::Client;
    client.groups                  = ConstSpan<AlgorithmId>(&tls->group, 1U);
    client.signatures              = kAccepted;
    client.aeads                   = kClientAeads;
    client.pinned                  = ConstSpan<KeyId>(&tls->pin, 1U);
    client.server_name             = CRD_BOARD_SERVER_NAME;
    client.certificate_compression = {};
    client.ech_grease              = false;
    ok = ok && tls->server.init(server) == TlsError::None && tls->client.init(client) == TlsError::None &&
         cerid_tls_pair(*tls, false, tls->writer, tls->reader);
    if (!ok)
    {
        cerid_tls_free(tls);
        return nullptr;
    }
    return tls;
}

int cerid_tls_connect(void* state, int resume)
{
    TlsSession client;
    TlsSession server;
    return cerid_tls_pair(*static_cast<CeridTls*>(state), resume != 0, client, server);
}

int cerid_tls_record(void* state, const uint8_t* data, size_t size, uint8_t* out)
{
    auto& tls = *static_cast<CeridTls*>(state);
    if (tls.writer.send(ConstSpan<u8>(data, size)) != TlsError::None || !deliver(tls.writer, tls.reader))
    {
        return 0;
    }
    for (usize got = 0U; got < size;)
    {
        usize written = 0U;
        if (tls.reader.read(Span<u8>(out + got, size - got), written) != TlsError::None || written == 0U)
        {
            return 0;
        }
        got += written;
    }
    return 1;
}

constexpr crd_board_peer kCerid = {
    "cerid",         "crd-crypto",   cerid_aead_new,   cerid_aead_seal,       cerid_aead_open,
    cerid_aead_free, cerid_x25519,   cerid_kem_new,    cerid_kem_encapsulate, cerid_kem_decapsulate,
    cerid_kem_free,  cerid_signer_new, cerid_sign,     cerid_verify,          cerid_signer_free,
    cerid_tls_new,   cerid_tls_connect, cerid_tls_record, cerid_tls_free,
};

const crd_board_peer* const kArms[] = {&kCerid, &crd_board_peer_awslc, &crd_board_peer_boringssl,
                                       &crd_board_peer_openssl};
constexpr usize kArmCount = sizeof(kArms) / sizeof(kArms[0]);

// ---- The cases ----

enum class Kind : u8
{
    Seal,
    Open,
    Agree,
    KemNew,
    Encapsulate,
    Decapsulate,
    Sign,
    Verify,
    TlsFull,
    TlsResume,
    TlsRecord,
};

struct Case
{
    char  name[48];
    Kind  kind;
    int   algorithm; // the AEAD, signature or TLS group
    usize size;      // the AEAD's or record's plaintext, the signed message
};

constexpr usize kMessageBytes = 128U; // about a TLS 1.3 CertificateVerify's signed content
constexpr usize kAeadSizes[]  = {64U, 1350U, 16384U};
constexpr usize kRecordSizes[] = {256U, 16384U};
constexpr const char* kAeadNames[]      = {"aes-128-gcm", "aes-256-gcm", "chacha20-poly1305"};
constexpr const char* kSignatureNames[] = {"ed25519", "ecdsa-p256", "ml-dsa-65"};
constexpr const char* kGroupNames[]     = {"x25519", "x25519mlkem768"};

Case g_cases[64];
usize g_case_count = 0U;

// A new case, its name written by the caller.
Case& add_case(Kind kind, int algorithm, usize size)
{
    Case& c     = g_cases[g_case_count++];
    c.kind      = kind;
    c.algorithm = algorithm;
    c.size      = size;
    return c;
}

void make_cases()
{
    for (const Kind kind : {Kind::Seal, Kind::Open})
    {
        for (int aead = 0; aead < 3; ++aead)
        {
            for (const usize size : kAeadSizes)
            {
                Case& c = add_case(kind, aead, size);
                std::snprintf(c.name, sizeof(c.name), "aead-%s/%s/%zu", kind == Kind::Seal ? "seal" : "open",
                              kAeadNames[aead], size);
            }
        }
    }
    std::snprintf(add_case(Kind::Agree, 0, 0U).name, sizeof(Case::name), "x25519/agree");
    std::snprintf(add_case(Kind::KemNew, 0, 0U).name, sizeof(Case::name), "ml-kem-768/keygen");
    std::snprintf(add_case(Kind::Encapsulate, 0, 0U).name, sizeof(Case::name), "ml-kem-768/encapsulate");
    std::snprintf(add_case(Kind::Decapsulate, 0, 0U).name, sizeof(Case::name), "ml-kem-768/decapsulate");
    for (int algorithm = 0; algorithm < 3; ++algorithm)
    {
        Case& sign = add_case(Kind::Sign, algorithm, kMessageBytes);
        std::snprintf(sign.name, sizeof(sign.name), "%s/sign", kSignatureNames[algorithm]);
        Case& verify = add_case(Kind::Verify, algorithm, kMessageBytes);
        std::snprintf(verify.name, sizeof(verify.name), "%s/verify", kSignatureNames[algorithm]);
    }
    for (int group = 0; group < 2; ++group)
    {
        Case& full = add_case(Kind::TlsFull, group, 0U);
        std::snprintf(full.name, sizeof(full.name), "tls-full/%s", kGroupNames[group]);
    }
    Case& resume = add_case(Kind::TlsResume, CRD_BOARD_X25519_MLKEM768, 0U);
    std::snprintf(resume.name, sizeof(resume.name), "tls-resume/%s", kGroupNames[CRD_BOARD_X25519_MLKEM768]);
    for (const usize size : kRecordSizes)
    {
        Case& record = add_case(Kind::TlsRecord, CRD_BOARD_X25519_MLKEM768, size);
        std::snprintf(record.name, sizeof(record.name), "tls-record/%zu", size);
    }
}

// The inputs every arm shares, fixed for the run.
struct Inputs
{
    u8 key[32];
    u8 nonce[CRD_BOARD_NONCE_BYTES];
    u8 plain[CRD_BOARD_MAX_RECORD_BYTES + CRD_BOARD_TAG_BYTES];
    u8 x25519_peer[32];
};
Inputs g_in;

// One arm's state for one case, and its scratch.
struct Bench
{
    const crd_board_peer* peer  = nullptr;
    void*                 state = nullptr;
    u8    public_key[CRD_BOARD_MAX_PUBLIC_BYTES] = {};
    usize public_bytes                          = 0U;
    u8    signature[CRD_BOARD_MAX_SIGNATURE_BYTES] = {};
    usize signature_bytes                         = 0U;
    u8    ciphertext[CRD_BOARD_KEM_CIPHERTEXT_BYTES] = {};
    u8    secret[CRD_BOARD_SECRET_BYTES]             = {};
    u8    sealed[CRD_BOARD_MAX_RECORD_BYTES + CRD_BOARD_TAG_BYTES] = {};
    u8    out[CRD_BOARD_MAX_RECORD_BYTES + CRD_BOARD_TAG_BYTES]    = {};
};

void setup(const Case& c, Bench& b)
{
    const crd_board_peer& p = *b.peer;
    bool ok = true;
    switch (c.kind)
    {
    case Kind::Seal:
    case Kind::Open:
        b.state = p.aead_new(c.algorithm, g_in.key);
        ok = b.state != nullptr && p.aead_seal(b.state, g_in.nonce, g_in.plain, c.size, b.sealed);
        break;
    case Kind::Agree:
    case Kind::KemNew:
        break;
    case Kind::Encapsulate:
    case Kind::Decapsulate:
        b.state = p.kem_new(b.public_key);
        ok = b.state != nullptr && p.kem_encapsulate(b.public_key, b.ciphertext, b.secret);
        break;
    case Kind::Sign:
    case Kind::Verify:
        b.state = p.signer_new(c.algorithm, b.public_key, &b.public_bytes);
        ok = b.state != nullptr && p.sign(b.state, g_in.plain, c.size, b.signature, &b.signature_bytes);
        break;
    case Kind::TlsFull:
    case Kind::TlsResume:
    case Kind::TlsRecord:
        b.state = p.tls_new(c.algorithm);
        ok      = b.state != nullptr;
        break;
    }
    if (!ok)
    {
        fail(c.name, p.name);
    }
}

void teardown(const Case& c, Bench& b)
{
    const crd_board_peer& p = *b.peer;
    switch (c.kind)
    {
    case Kind::Seal:
    case Kind::Open:
        p.aead_free(b.state);
        break;
    case Kind::Encapsulate:
    case Kind::Decapsulate:
        p.kem_free(b.state);
        break;
    case Kind::Sign:
    case Kind::Verify:
        p.signer_free(b.state);
        break;
    case Kind::TlsFull:
    case Kind::TlsResume:
    case Kind::TlsRecord:
        p.tls_free(b.state);
        break;
    case Kind::Agree:
    case Kind::KemNew:
        break;
    }
    b.state = nullptr;
}

// `count` operations of the case, in a loop of their own; the nanoseconds they took.
double run(const Case& c, Bench& b, u64 count)
{
    const crd_board_peer& p  = *b.peer;
    int                   ok = 1;
    const auto            start = std::chrono::steady_clock::now();
    switch (c.kind)
    {
    case Kind::Seal:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.aead_seal(b.state, g_in.nonce, g_in.plain, c.size, b.out);
        }
        break;
    case Kind::Open:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.aead_open(b.state, g_in.nonce, b.sealed, c.size, b.out);
        }
        break;
    case Kind::Agree:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.x25519(g_in.x25519_peer, b.secret);
        }
        break;
    case Kind::KemNew:
        for (u64 i = 0U; i < count; ++i)
        {
            void* kem = p.kem_new(b.public_key);
            ok &= kem != nullptr ? 1 : 0;
            p.kem_free(kem);
        }
        break;
    case Kind::Encapsulate:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.kem_encapsulate(b.public_key, b.ciphertext, b.secret);
        }
        break;
    case Kind::Decapsulate:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.kem_decapsulate(b.state, b.ciphertext, b.secret);
        }
        break;
    case Kind::Sign:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.sign(b.state, g_in.plain, c.size, b.signature, &b.signature_bytes);
        }
        break;
    case Kind::Verify:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.verify(c.algorithm, b.public_key, b.public_bytes, g_in.plain, c.size, b.signature,
                           b.signature_bytes);
        }
        break;
    case Kind::TlsFull:
    case Kind::TlsResume:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.tls_connect(b.state, c.kind == Kind::TlsResume ? 1 : 0);
        }
        break;
    case Kind::TlsRecord:
        for (u64 i = 0U; i < count; ++i)
        {
            ok &= p.tls_record(b.state, g_in.plain, c.size, b.out);
        }
        break;
    }
    const auto stop = std::chrono::steady_clock::now();
    if (ok == 0)
    {
        fail(c.name, p.name);
    }
    return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(stop - start).count());
}

// ---- The checks ----

void check(bool ok, const char* what, const crd_board_peer& peer)
{
    if (!ok)
    {
        fail(what, peer.name);
    }
}

usize check_aeads()
{
    usize checked = 0U;
    for (int aead = 0; aead < 3; ++aead)
    {
        for (const usize size : {usize{0U}, usize{64U}, usize{1350U}, usize{CRD_BOARD_MAX_RECORD_BYTES}})
        {
            static Bench reference;
            reference.peer  = &kCerid;
            reference.state = kCerid.aead_new(aead, g_in.key);
            check(reference.state != nullptr && kCerid.aead_seal(reference.state, g_in.nonce, g_in.plain, size,
                                                                 reference.sealed),
                  "AEAD seal", kCerid);
            for (const crd_board_peer* peer : kArms)
            {
                static Bench arm;
                void* state = peer->aead_new(aead, g_in.key);
                check(state != nullptr && peer->aead_seal(state, g_in.nonce, g_in.plain, size, arm.sealed) &&
                          std::memcmp(arm.sealed, reference.sealed, size + CRD_BOARD_TAG_BYTES) == 0,
                      "AEAD bytes differ from crd-crypto's", *peer);
                check(peer->aead_open(state, g_in.nonce, reference.sealed, size, arm.out) &&
                          std::memcmp(arm.out, g_in.plain, size) == 0,
                      "AEAD open", *peer);
                reference.sealed[size / 2U] ^= 0x01U;
                check(!peer->aead_open(state, g_in.nonce, reference.sealed, size, arm.out), "AEAD forgery opened",
                      *peer);
                reference.sealed[size / 2U] ^= 0x01U;
                peer->aead_free(state);
                ++checked;
            }
            kCerid.aead_free(reference.state);
        }
    }
    return checked;
}

usize check_kems()
{
    usize checked = 0U;
    for (const crd_board_peer* owner : kArms)
    {
        static u8 public_key[CRD_BOARD_KEM_PUBLIC_BYTES];
        void*     kem = owner->kem_new(public_key);
        check(kem != nullptr, "ML-KEM-768 key", *owner);
        for (const crd_board_peer* sender : kArms)
        {
            u8 ciphertext[CRD_BOARD_KEM_CIPHERTEXT_BYTES];
            u8 sent[CRD_BOARD_SECRET_BYTES];
            u8 received[CRD_BOARD_SECRET_BYTES];
            check(sender->kem_encapsulate(public_key, ciphertext, sent), "ML-KEM-768 encapsulation", *sender);
            check(owner->kem_decapsulate(kem, ciphertext, received) && std::memcmp(sent, received, sizeof(sent)) == 0,
                  "ML-KEM-768 secrets differ across libraries", *owner);
            ciphertext[7] ^= 0x01U; // implicit rejection: another secret, never a failure
            check(owner->kem_decapsulate(kem, ciphertext, received) && std::memcmp(sent, received, sizeof(sent)) != 0,
                  "ML-KEM-768 implicit rejection", *owner);
            ++checked;
        }
        owner->kem_free(kem);
    }
    return checked;
}

usize check_signatures()
{
    usize checked = 0U;
    for (int algorithm = 0; algorithm < 3; ++algorithm)
    {
        for (const crd_board_peer* owner : kArms)
        {
            static Bench signed_by;
            void*        signer = owner->signer_new(algorithm, signed_by.public_key, &signed_by.public_bytes);
            check(signer != nullptr && owner->sign(signer, g_in.plain, kMessageBytes, signed_by.signature,
                                                   &signed_by.signature_bytes),
                  kSignatureNames[algorithm], *owner);
            for (const crd_board_peer* verifier : kArms)
            {
                check(verifier->verify(algorithm, signed_by.public_key, signed_by.public_bytes, g_in.plain,
                                       kMessageBytes, signed_by.signature, signed_by.signature_bytes),
                      "a signature of another library refused", *verifier);
                signed_by.signature[signed_by.signature_bytes - 1U] ^= 0x01U;
                check(!verifier->verify(algorithm, signed_by.public_key, signed_by.public_bytes, g_in.plain,
                                        kMessageBytes, signed_by.signature, signed_by.signature_bytes),
                      "a tampered signature accepted", *verifier);
                signed_by.signature[signed_by.signature_bytes - 1U] ^= 0x01U;
                ++checked;
            }
            owner->signer_free(signer);
        }
    }
    return checked;
}

usize check_tls()
{
    usize checked = 0U;
    for (const crd_board_peer* peer : kArms)
    {
        for (int group = 0; group < 2; ++group)
        {
            void* tls = peer->tls_new(group);
            check(tls != nullptr, "TLS contexts and a first connection", *peer);
            check(peer->tls_connect(tls, 0), "TLS full handshake", *peer);
            check(peer->tls_connect(tls, 1) && peer->tls_connect(tls, 1), "TLS resumption", *peer);
            static u8 out[CRD_BOARD_MAX_RECORD_BYTES];
            check(peer->tls_record(tls, g_in.plain, CRD_BOARD_MAX_RECORD_BYTES, out) &&
                      std::memcmp(out, g_in.plain, CRD_BOARD_MAX_RECORD_BYTES) == 0,
                  "TLS record", *peer);
            peer->tls_free(tls);
            ++checked;
        }
    }
    return checked;
}

// ---- The board ----

struct Options
{
    const char* filter    = nullptr;
    double      budget_ns = 10e6;
    int         rounds    = 5;
    bool        quick     = false;
    bool        check     = false;
};

Options parse(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i)
    {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--case") == 0 && has_value)
        {
            options.filter = argv[++i];
        }
        else if (std::strcmp(argv[i], "--budget-ms") == 0 && has_value)
        {
            options.budget_ns = std::atof(argv[++i]) * 1e6;
        }
        else if (std::strcmp(argv[i], "--rounds") == 0 && has_value)
        {
            options.rounds = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--quick") == 0)
        {
            options.quick = true;
        }
        else if (std::strcmp(argv[i], "--check") == 0)
        {
            options.check = true;
        }
        else
        {
            std::fprintf(stderr, "usage: crd-crypto-board [--case TEXT] [--budget-ms N] [--rounds N] [--quick] "
                                 "[--check]\n");
            std::exit(2);
        }
    }
    if (options.rounds < 1 || options.rounds > 64 || options.budget_ns <= 0.0)
    {
        std::fprintf(stderr, "crd-crypto-board: --rounds is 1 to 64 and --budget-ms above 0\n");
        std::exit(2);
    }
    if (options.quick)
    {
        options.rounds = 1;
    }
    return options;
}

// The operations one batch of `b` runs: doubled until a batch takes a quarter of the budget, then scaled to it.
u64 calibrate(const Case& c, Bench& b, const Options& options)
{
    if (options.quick)
    {
        return 1U;
    }
    u64 count = 1U;
    for (;;)
    {
        const double elapsed = run(c, b, count);
        if (elapsed >= options.budget_ns / 4.0 || count >= (u64{1} << 32))
        {
            const double per_operation = elapsed / static_cast<double>(count);
            return std::max<u64>(1U, static_cast<u64>(options.budget_ns / per_operation));
        }
        count *= 2U;
    }
}

void measure(const Case& c, usize index, const Options& options)
{
    static Bench benches[kArmCount];
    u64          counts[kArmCount];
    double       samples[kArmCount][64];
    for (usize arm = 0U; arm < kArmCount; ++arm)
    {
        benches[arm].peer = kArms[arm];
        setup(c, benches[arm]);
        counts[arm] = calibrate(c, benches[arm], options);
    }
    for (int round = 0; round < options.rounds; ++round)
    {
        for (usize turn = 0U; turn < kArmCount; ++turn)
        {
            const usize arm = (turn + static_cast<usize>(round) + index) % kArmCount;
            samples[arm][round] = run(c, benches[arm], counts[arm]) / static_cast<double>(counts[arm]);
        }
    }
    for (usize arm = 0U; arm < kArmCount; ++arm)
    {
        double* first = samples[arm];
        double* last  = first + options.rounds;
        std::sort(first, last);
        const int    half   = options.rounds / 2;
        const double median = options.rounds % 2 == 1 ? first[half] : (first[half - 1] + first[half]) / 2.0;
        std::printf("%s %s %.1f\n", kArms[arm]->name, c.name, median);
        teardown(c, benches[arm]);
    }
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    const Options options = parse(argc, argv);
    for (usize i = 0U; i < sizeof(g_in.plain); ++i)
    {
        g_in.plain[i] = static_cast<u8>(i * 7U + 3U);
    }
    for (usize i = 0U; i < sizeof(g_in.key); ++i)
    {
        g_in.key[i] = static_cast<u8>(0xA0U + i);
    }
    for (usize i = 0U; i < sizeof(g_in.nonce); ++i)
    {
        g_in.nonce[i] = static_cast<u8>(0x50U + i);
    }
    AgreementKey x25519_peer;
    if (x25519_peer.generate(AlgorithmId::X25519) != CryptoError::None ||
        x25519_peer.public_key(Span<u8>(g_in.x25519_peer, sizeof(g_in.x25519_peer))) != CryptoError::None)
    {
        fail("X25519 peer key", "cerid");
    }

    std::printf("crd-crypto-board: provider %.*s (assembly %s)", static_cast<int>(provider_name().size()),
                provider_name().data(), provider_assembly() ? "on" : "off");
    for (const crd_board_peer* peer : kArms)
    {
        std::printf(", %s %s", peer->name, peer->version);
    }
    std::printf("\n");
    const usize aeads      = check_aeads();
    const usize kems       = check_kems();
    const usize signatures = check_signatures();
    const usize tls        = check_tls();
    std::printf("check: every arm agrees (%zu AEAD cases, %zu ML-KEM pairs, %zu signature pairs, %zu TLS profiles)\n",
                aeads, kems, signatures, tls);
    if (options.check)
    {
        return 0;
    }

    make_cases();
    usize measured = 0U;
    for (usize i = 0U; i < g_case_count; ++i)
    {
        if (options.filter == nullptr || std::strstr(g_cases[i].name, options.filter) != nullptr)
        {
            measure(g_cases[i], i, options);
            ++measured;
        }
    }
    if (measured == 0U)
    {
        std::fprintf(stderr, "crd-crypto-board: no case matches %s\n", options.filter);
        return 2;
    }
    return 0;
}
