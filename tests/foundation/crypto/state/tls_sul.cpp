// crd-crypto-tls-sul -- a TlsSession as the system under learning of scripts/check-state-machine.py (NET.2;
// docs/design/network-foundation.md#state-learning): the learner's abstract inputs become calls on the session under
// learning (the subject) and on an honest peer session, and the outcome of each becomes one abstract output.
//
// Usage: crd-crypto-tls-sul --role server|client   (the subject's role; the peer takes the other)
//        crd-crypto-tls-sul --alphabet              (prints the inputs, space-separated, on one line)
//
// Each line read is one query: input symbols separated by spaces. The program starts both sessions afresh (a reset),
// applies the inputs in order and writes one line with one output per input. It exits 0 at the end of input and 2 on
// a usage error or an unknown input.
//
// The network between the two is half honest. What the subject sends reaches the peer at once, and the peer reads
// whatever arrives. What the peer sends waits in one queue that the learner drives: `deliver` hands the subject all of
// it, `drop` discards it and `replay` hands the subject the last delivery again. The other inputs are calls on either
// end: `p_*` on the peer, `s_*` on the subject (data: send() of 16 bytes; update and update_req: update_keys() without
// and with a request; close: close()). The subject's key updates are bounded at kSubjectMaxUpdates so the bound is part
// of the learned machine.
//
// An output is `<result>.<subject>.<peer>` and its flags. The result is the call's TlsError (a delivery's receive();
// `empty` when there was nothing to deliver, drop or replay). Each end's state is I, H, E, C or F (Idle, Handshaking,
// Established, Closed, Failed), a failed end followed by the alert it sent. The flags, in this order, say what the step
// caused: d and D the subject and the peer read application data, k and K a KeyUpdate received, u and U a key update
// sent (one the session made by itself: answering a request, or at its record bound).

#include <crd/containers/array.hpp>
#include <crd/crypto/provider.hpp>
#include <crd/crypto/tls.hpp>

#include <cstdio>
#include <cstring>

namespace
{
using crd::u64;
using crd::u8;
using crd::usize;
using crd::containers::ConstSpan;
using crd::containers::Span;
using namespace crd::crypto;

using Bytes = crd::containers::Array<u8>;

constexpr u64 kSubjectMaxUpdates = 3U; // TlsConfig::max_key_updates of the subject: two updates, then closed
constexpr const char kServerName[] = "game.cerid.test";
constexpr usize kMaxLine = 4096U;
constexpr usize kMaxInputs = 256U;

constexpr AlgorithmId kGroups[] = {AlgorithmId::X25519Mlkem768};
constexpr AlgorithmId kSignatures[] = {AlgorithmId::Ed25519};
constexpr AlgorithmId kAeads[] = {AlgorithmId::Aes128Gcm};

enum class Input : u8
{
    Deliver,
    Drop,
    Replay,
    PeerData,
    PeerUpdate,
    PeerUpdateRequest,
    PeerClose,
    SubjectData,
    SubjectUpdate,
    SubjectUpdateRequest,
    SubjectClose,
};

constexpr const char* kInputNames[] = {"deliver", "drop",   "replay",   "p_data",       "p_update", "p_update_req",
                                       "p_close", "s_data", "s_update", "s_update_req", "s_close"};
constexpr usize kInputCount = sizeof(kInputNames) / sizeof(kInputNames[0]);

const char* result_name(TlsError error)
{
    switch (error)
    {
        case TlsError::None:
            return "ok";
        case TlsError::BadConfig:
            return "cfg";
        case TlsError::NotReady:
            return "nr";
        case TlsError::Full:
            return "full";
        case TlsError::PeerRefused:
            return "pr";
        case TlsError::Failed:
            return "fail";
        case TlsError::Closed:
            return "closed";
        case TlsError::EchRejected:
            return "ech";
        case TlsError::ProviderFailure:
            return "prov";
        case TlsError::PolicyRefused:
            return "pol";
    }
    return "?";
}

char state_letter(TlsState state)
{
    switch (state)
    {
        case TlsState::Idle:
            return 'I';
        case TlsState::Handshaking:
            return 'H';
        case TlsState::Established:
            return 'E';
        case TlsState::Closed:
            return 'C';
        case TlsState::Failed:
            return 'F';
    }
    return '?';
}

struct End
{
    TlsContext context;
    TlsSession session;
};

class Harness
{
public:
    [[nodiscard]] bool init(bool subject_is_server)
    {
        u8 seed[32];
        for (usize i = 0U; i < sizeof(seed); ++i)
        {
            seed[i] = static_cast<u8>(i + 1U);
        }
        if (m_key.from_seed(AlgorithmId::Ed25519, ConstSpan<u8>(seed, sizeof(seed))) != CryptoError::None ||
            m_credential.init(m_key, kServerName) != TlsError::None)
        {
            return false;
        }
        const KeyId pin = m_credential.key_id();
        TlsConfig client;
        client.role = TlsRole::Client;
        client.groups = kGroups;
        client.signatures = kSignatures;
        client.aeads = kAeads;
        client.pinned = ConstSpan<KeyId>(&pin, 1U);
        client.server_name = kServerName;
        TlsConfig server;
        server.role = TlsRole::Server;
        server.groups = kGroups;
        server.aeads = kAeads;
        server.credential = &m_credential;
        server.server_name = kServerName;
        (subject_is_server ? server : client).max_key_updates = kSubjectMaxUpdates;
        m_subject_is_server = subject_is_server;
        return m_subject.context.init(subject_is_server ? server : client) == TlsError::None &&
               m_peer.context.init(subject_is_server ? client : server) == TlsError::None;
    }

    // A fresh pair of sessions: the client's first flight sent (into the queue, or from the subject to the peer).
    [[nodiscard]] bool reset()
    {
        m_queue.clear();
        m_last.clear();
        m_flags[0] = '\0';
        End& client = m_subject_is_server ? m_peer : m_subject;
        End& server = m_subject_is_server ? m_subject : m_peer;
        if (server.session.init(server.context) != TlsError::None ||
            client.session.init(client.context) != TlsError::None)
        {
            return false;
        }
        settle();
        return true;
    }

    // Applies one input; writes its output into `out` (zero-terminated).
    void step(Input input, char* out, usize out_size)
    {
        m_flags_used = 0U;
        const u64 subject_sent = m_subject.session.key_updates_sent();
        const u64 subject_received = m_subject.session.key_updates_received();
        const u64 peer_sent = m_peer.session.key_updates_sent();
        const u64 peer_received = m_peer.session.key_updates_received();
        const char* result = apply(input);
        settle();
        if (m_subject.session.key_updates_received() != subject_received)
        {
            flag('k');
        }
        if (m_peer.session.key_updates_received() != peer_received)
        {
            flag('K');
        }
        // An update the call itself made is its result; one the session made by itself is a flag.
        const u64 own = (input == Input::SubjectUpdate || input == Input::SubjectUpdateRequest) ? 1U : 0U;
        if (m_subject.session.key_updates_sent() > subject_sent + own)
        {
            flag('u');
        }
        const u64 peer_own = (input == Input::PeerUpdate || input == Input::PeerUpdateRequest) ? 1U : 0U;
        if (m_peer.session.key_updates_sent() > peer_sent + peer_own)
        {
            flag('U');
        }
        m_flags[m_flags_used] = '\0';
        char subject[8];
        char peer[8];
        describe(m_subject.session, subject, sizeof(subject));
        describe(m_peer.session, peer, sizeof(peer));
        (void)std::snprintf(out, out_size, "%s.%s.%s%s", result, subject, peer, m_flags);
    }

private:
    const char* apply(Input input)
    {
        static constexpr u8 kData[16] = {'s', 't', 'a', 't', 'e', ' ', 'l', 'e',
                                         'a', 'r', 'n', 'i', 'n', 'g', '.', '\n'};
        const ConstSpan<u8> data(kData, sizeof(kData));
        switch (input)
        {
            case Input::Deliver:
                if (m_queue.empty())
                {
                    return "empty";
                }
                m_last = m_queue;
                m_queue.clear();
                return result_name(m_subject.session.receive(ConstSpan<u8>(m_last.data(), m_last.size())));
            case Input::Drop:
                if (m_queue.empty())
                {
                    return "empty";
                }
                m_queue.clear();
                return "ok";
            case Input::Replay:
                if (m_last.empty())
                {
                    return "empty";
                }
                return result_name(m_subject.session.receive(ConstSpan<u8>(m_last.data(), m_last.size())));
            case Input::PeerData:
                return result_name(m_peer.session.send(data));
            case Input::PeerUpdate:
                return result_name(m_peer.session.update_keys(false));
            case Input::PeerUpdateRequest:
                return result_name(m_peer.session.update_keys(true));
            case Input::PeerClose:
                return result_name(m_peer.session.close());
            case Input::SubjectData:
                return result_name(m_subject.session.send(data));
            case Input::SubjectUpdate:
                return result_name(m_subject.session.update_keys(false));
            case Input::SubjectUpdateRequest:
                return result_name(m_subject.session.update_keys(true));
            case Input::SubjectClose:
                return result_name(m_subject.session.close());
        }
        return "?";
    }

    // Each end reads what it received (answering a KeyUpdate request); the subject's bytes go to the peer, the peer's
    // into the queue, until the subject has nothing more to send.
    void settle()
    {
        for (;;)
        {
            if (read_all(m_subject.session))
            {
                flag('d');
            }
            if (m_subject.session.pending() == 0U)
            {
                break;
            }
            const Bytes sent = take(m_subject.session);
            (void)m_peer.session.receive(ConstSpan<u8>(sent.data(), sent.size()));
            if (read_all(m_peer.session))
            {
                flag('D');
            }
            queue_peer();
        }
        queue_peer();
    }

    void queue_peer()
    {
        const Bytes sent = take(m_peer.session);
        for (usize i = 0U; i < sent.size(); ++i)
        {
            m_queue.push_back(sent[i]);
        }
    }

    static Bytes take(TlsSession& session)
    {
        Bytes bytes;
        bytes.resize(session.pending());
        usize written = 0U;
        if (!bytes.empty() && session.take(Span<u8>(bytes.data(), bytes.size()), written) != TlsError::None)
        {
            written = 0U;
        }
        bytes.resize(written);
        return bytes;
    }

    // Whether an established session read any application data (read() answers a pending KeyUpdate request too).
    static bool read_all(TlsSession& session)
    {
        bool any = false;
        while (session.state() == TlsState::Established)
        {
            u8 chunk[256];
            usize written = 0U;
            if (session.read(Span<u8>(chunk, sizeof(chunk)), written) != TlsError::None || written == 0U)
            {
                break;
            }
            any = true;
        }
        return any;
    }

    static void describe(const TlsSession& session, char* out, usize out_size)
    {
        const TlsState state = session.state();
        if (state == TlsState::Failed && session.alert_sent().present)
        {
            (void)std::snprintf(out, out_size, "F%u", static_cast<unsigned>(session.alert_sent().description));
            return;
        }
        (void)std::snprintf(out, out_size, "%c", state_letter(state));
    }

    void flag(char letter)
    {
        for (usize i = 0U; i < m_flags_used; ++i)
        {
            if (m_flags[i] == letter)
            {
                return;
            }
        }
        if (m_flags_used + 1U < sizeof(m_flags))
        {
            m_flags[m_flags_used++] = letter;
        }
    }

    SigningKey m_key;
    TlsCredential m_credential;
    End m_subject;
    End m_peer;
    Bytes m_queue; // the peer's bytes not yet delivered
    Bytes m_last;  // the last delivery, for replay
    char m_flags[8] = {};
    usize m_flags_used = 0U;
    bool m_subject_is_server = true;
};

bool parse_input(const char* token, Input& input)
{
    for (usize i = 0U; i < kInputCount; ++i)
    {
        if (std::strcmp(token, kInputNames[i]) == 0)
        {
            input = static_cast<Input>(i);
            return true;
        }
    }
    return false;
}

int usage()
{
    std::fputs("usage: crd-crypto-tls-sul --role server|client | --alphabet\n", stderr);
    return 2;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--alphabet") == 0)
    {
        for (usize i = 0U; i < kInputCount; ++i)
        {
            std::printf("%s%s", i == 0U ? "" : " ", kInputNames[i]);
        }
        std::printf("\n");
        return 0;
    }
    if (argc != 3 || std::strcmp(argv[1], "--role") != 0 ||
        (std::strcmp(argv[2], "server") != 0 && std::strcmp(argv[2], "client") != 0))
    {
        return usage();
    }
    static Harness harness;
    if (!harness.init(std::strcmp(argv[2], "server") == 0))
    {
        std::fputs("crd-crypto-tls-sul: the contexts could not be made\n", stderr);
        return 1;
    }
    static char line[kMaxLine];
    static char reply[kMaxInputs * 24U];
    while (std::fgets(line, sizeof(line), stdin) != nullptr)
    {
        if (std::strchr(line, '\n') == nullptr && !std::feof(stdin))
        {
            std::fputs("crd-crypto-tls-sul: a query longer than the line bound\n", stderr);
            return 2;
        }
        if (!harness.reset())
        {
            std::fputs("crd-crypto-tls-sul: the sessions could not be started\n", stderr);
            return 1;
        }
        usize used = 0U;
        usize inputs = 0U;
        reply[0] = '\0';
        for (char* cursor = line; *cursor != '\0';)
        {
            while (*cursor == ' ' || *cursor == '\r' || *cursor == '\n')
            {
                ++cursor;
            }
            if (*cursor == '\0')
            {
                break;
            }
            char* token = cursor;
            while (*cursor != '\0' && *cursor != ' ' && *cursor != '\r' && *cursor != '\n')
            {
                ++cursor;
            }
            if (*cursor != '\0')
            {
                *cursor++ = '\0';
            }
            Input input{};
            if (!parse_input(token, input) || ++inputs > kMaxInputs)
            {
                std::fprintf(stderr, "crd-crypto-tls-sul: unknown input or too many: %s\n", token);
                return 2;
            }
            char output[24];
            harness.step(input, output, sizeof(output));
            used += static_cast<usize>(
                std::snprintf(reply + used, sizeof(reply) - used, "%s%s", used == 0U ? "" : " ", output));
        }
        std::printf("%s\n", reply);
        (void)std::fflush(stdout);
    }
    return 0;
}
