#!/usr/bin/env python3
"""TLS 1.3 interoperability of crd-crypto's sessions with the pinned OpenSSL 3.5 (NET.2).

Runs crd-tls-peer (crd-crypto over AWS-LC) against crd-openssl-tls-peer (OpenSSL), each in its own process, and
relays their TLS records over their standard streams (4-byte big-endian frames). Every case checks both ends' own
reports (state, negotiated group, HelloRetryRequest, the pinned peer key, alerts, the application data echoed) and
reads the handshake off the wire independently of both libraries: the key share groups and lengths of the ClientHello,
the ServerHello or HelloRetryRequest and a retried ClientHello against the hybrid groups' layout (RFC 10024 and
draft-ietf-tls-ecdhe-mlkem: ML-KEM-768 encapsulation key 1184 bytes and ciphertext 1088, ML-KEM-1024 1568 and 1568,
X25519 32, uncompressed P-256 65 and P-384 97), and the GREASE values (RFC 8701) of Cerid's ClientHellos, which a
retried one keeps. The three hybrid groups run both ways, through a HelloRetryRequest both ways, beside a classical
share without a downgrade, with mutual authentication, with a key that is not pinned and against a peer that offers
TLS 1.2 only. Key updates (RFC 9846 section 4.7.3) run both ways: a Cerid client updating its key before every record
under a three-record budget, one asking OpenSSL for an update and one not asking, and an OpenSSL client asking a Cerid
server, each end's count of KeyUpdate messages sent and received checked against the other's and the echoed data
intact. Resumption (RFC 9846 sections 2.2 and 4.6.1) runs both ways across processes, with each end's ticket or session
kept in a file: a Cerid client resuming at OpenSSL servers that share a ticket key, and an OpenSSL client resuming at a
second Cerid server of a fleet, after a rotation under the previous key, with a pinned client credential, and not at a
server whose key sealed none of its tickets. On the wire the resumed ClientHello offers one identity in a
pre_shared_key that is its last extension, psk_dhe_ke alone and a fresh hybrid key share; the ServerHello selects
identity 0 beside its own share; and a Cerid server's ticket starts with its key's name (HKDF-SHA256 of the key, the
format of tls.cpp).

Usage: tls_interop.py --cerid <crd-tls-peer> --openssl <crd-openssl-tls-peer>
"""

import argparse
import hashlib
import hmac
import json
import os
import struct
import subprocess
import sys
import tempfile

SERVER_SEED = bytes(range(1, 33)).hex()
CLIENT_SEED = bytes(range(33, 65)).hex()
OTHER_SEED = bytes(range(65, 97)).hex()

# Register name -> (TLS codepoint, client share bytes, server share bytes, OpenSSL's names for it).
GROUPS = {
    'x25519mlkem768': (0x11EC, 1184 + 32, 1088 + 32, ('X25519MLKEM768',)),
    'secp256r1mlkem768': (0x11EB, 65 + 1184, 65 + 1088, ('SecP256r1MLKEM768',)),
    'secp384r1mlkem1024': (0x11ED, 97 + 1568, 97 + 1568, ('SecP384r1MLKEM1024',)),
    'x25519': (0x001D, 32, 32, ('X25519',)),
    'secp256r1': (0x0017, 65, 65, ('P-256', 'secp256r1', 'prime256v1')),
    'secp384r1': (0x0018, 97, 97, ('P-384', 'secp384r1')),
}
CODEPOINTS = {value[0]: name for name, value in GROUPS.items()}
HELLO_RETRY_RANDOM = hashlib.sha256(b'HelloRetryRequest').digest()
PING = 'ping from cerid'
OPENSSL_PING = 'ping from openssl'
BAD_CERTIFICATE = 42
PROTOCOL_VERSION = 70
TICKET_KEYS = [bytes(range(seed, seed + 32)).hex() for seed in (100, 140, 180)]
PRE_SHARED_KEY = 41
PSK_KEY_EXCHANGE_MODES = 45


class Failure(Exception):
    pass


def openssl_names(name):
    return {alias.lower() for alias in GROUPS[name][3]} | {name}


def read_frame(stream):
    header = stream.read(4)
    if len(header) != 4:
        raise Failure('a peer ended its output early')
    (size,) = struct.unpack('>I', header)
    body = stream.read(size)
    if len(body) != size:
        raise Failure('a peer wrote a short frame')
    return body


def write_frame(stream, data):
    stream.write(struct.pack('>I', len(data)) + data)
    stream.flush()


def first_handshake(flight):
    """The first handshake message of a flight: (type, body), skipping change_cipher_spec records."""
    at = 0
    while at + 5 <= len(flight):
        content_type, length = flight[at], struct.unpack('>H', flight[at + 3:at + 5])[0]
        record = flight[at + 5:at + 5 + length]
        at += 5 + length
        if content_type == 20:
            continue
        if content_type != 22 or len(record) < 4:
            raise Failure('the flight does not start with a handshake record (type %d)' % content_type)
        size = int.from_bytes(record[1:4], 'big')
        if 4 + size > len(record):
            raise Failure('a handshake message spans records; the hello flights here fit one')
        return record[0], record[4:4 + size]
    raise Failure('the flight holds no handshake record')


def is_grease(value):
    """RFC 8701's reserved values: 0x?A?A with the same digit in each ?."""
    return value & 0x0F0F == 0x0A0A and value >> 12 == (value >> 4) & 0x0F


def hello(flight):
    """(kind, random, cipher suite bytes, [(extension type, data)]) of a ClientHello or ServerHello (RFC 9846 4.1)."""
    kind, body = first_handshake(flight)
    if kind not in (1, 2):
        raise Failure('expected a hello, found handshake type %d' % kind)
    random = body[2:34]
    at = 34
    at += 1 + body[at]  # legacy_session_id
    if kind == 1:
        size = struct.unpack('>H', body[at:at + 2])[0]
        suites = body[at + 2:at + 2 + size]
        at += 2 + size
        at += 1 + body[at]  # legacy_compression_methods
    else:
        suites = body[at:at + 2]
        at += 3  # cipher_suite, legacy_compression_method
    end = at + 2 + struct.unpack('>H', body[at:at + 2])[0]
    at += 2
    extensions = []
    while at + 4 <= end:
        kind_of, size = struct.unpack('>HH', body[at:at + 4])
        extensions.append((kind_of, body[at + 4:at + 4 + size]))
        at += 4 + size
    if at != end:
        raise Failure('the extensions do not end where their length says')
    return kind, random, suites, extensions


def key_shares(flight):
    """(message, hello_retry, [(group codepoint, share length)]) from a ClientHello or a ServerHello."""
    kind, random, _, extensions = hello(flight)
    shares = []
    for kind_of, data in extensions:
        if kind_of != 51:
            continue
        if kind == 1:
            inner = 2
            while inner + 4 <= len(data):
                group, length = struct.unpack('>HH', data[inner:inner + 4])
                shares.append((group, length))
                inner += 4 + length
        elif len(data) == 2:
            shares.append((struct.unpack('>H', data)[0], 0))
        else:
            group, length = struct.unpack('>HH', data[:4])
            shares.append((group, length))
    return ('client_hello' if kind == 1 else 'server_hello'), (kind == 2 and random == HELLO_RETRY_RANDOM), shares


def grease(flight):
    """A ClientHello's GREASE values by field: cipher suites, supported groups (10), supported versions (43) and the
    reserved extensions with their lengths."""
    _, _, suites, extensions = hello(flight)

    def values(data):
        return [value for (value,) in struct.iter_unpack('>H', data) if is_grease(value)]
    found = {'suites': values(suites), 'groups': [], 'versions': [], 'extensions': []}
    for kind_of, data in extensions:
        if is_grease(kind_of):
            found['extensions'].append((kind_of, len(data)))
        elif kind_of == 10:
            found['groups'] = values(data[2:])
        elif kind_of == 43:
            found['versions'] = values(data[1:])
    return found


def run(client_command, server_command):
    """Relays one connection; (client report, server report, flights)."""
    client = subprocess.Popen(client_command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    server = subprocess.Popen(server_command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    flights = []
    try:
        frame = read_frame(client.stdout)
        for _ in range(16):
            flights.append(('client', frame))
            write_frame(server.stdin, frame)
            answer = read_frame(server.stdout)
            flights.append(('server', answer))
            write_frame(client.stdin, answer)
            frame = read_frame(client.stdout)
            if not frame and not answer:
                break
        else:
            raise Failure('the peers did not go quiet in 16 rounds')
    finally:
        reports = []
        for process in (client, server):
            try:
                _, error = process.communicate(timeout=60)
            except subprocess.TimeoutExpired:
                process.kill()
                raise Failure('a peer did not exit')
            if process.returncode != 0:
                raise Failure('a peer exited %d: %s' % (process.returncode, error.decode(errors='replace')))
            lines = [line for line in error.decode(errors='replace').splitlines() if line.startswith('{')]
            reports.append(json.loads(lines[-1]) if lines else {})
    return reports[0], reports[1], flights


def cerid(binary, role, groups, seed, pin=None, extra=()):
    command = [binary, '--role', role, '--groups', ','.join(groups), '--seed', seed]
    return command + (['--pin', pin] if pin else []) + list(extra)


def openssl(binary, role, groups, seed, pin=None, max_version=None, update=None, extra=()):
    command = [binary, '--role', role, '--groups', groups, '--seed', seed]
    command += ['--pin', pin] if pin else []
    command += ['--update', update] if update else []
    return command + (['--max-version', max_version] if max_version else []) + list(extra)


def public_key(binary, seed):
    result = subprocess.run([binary, '--public', seed], capture_output=True, text=True, check=True)
    return result.stdout.strip()


def expect(condition, message):
    if not condition:
        raise Failure(message)


def check_grease(found):
    expect(all(len(found[field]) == 1 for field in ('suites', 'groups', 'versions')),
           'the ClientHello lacks a GREASE suite, group or version: %s' % found)
    expect(len(found['extensions']) == 2 and found['extensions'][0][0] != found['extensions'][1][0] and
           [size for _, size in found['extensions']] == [0, 1], 'the GREASE extensions are %s' % found['extensions'])


def check_wire(flights, group, retry, offered, greased):
    """The hello flights on the wire: `offered` shares first, then (after a retry) one share of `group`. A greased
    ClientHello (RFC 8701, Cerid's) leads its shares with a one-byte GREASE share and carries GREASE values, which a
    retried ClientHello repeats without the share."""
    hello_kind, _, shares = key_shares(flights[0][1])
    expect(hello_kind == 'client_hello', 'the first flight is not a ClientHello')
    if greased:
        first = grease(flights[0][1])
        check_grease(first)
        expect(shares and is_grease(shares[0][0]) and shares[0][1] == 1, 'no GREASE key share leads %s' % shares)
        shares = shares[1:]
    expect([CODEPOINTS.get(code) for code, _ in shares] == list(offered),
           'ClientHello shares %s, expected %s' % (shares, offered))
    for code, length in shares:
        name = CODEPOINTS[code]
        expect(length == GROUPS[name][1], '%s client share is %d bytes, not %d' % (name, length, GROUPS[name][1]))
    kind, was_retry, answer = key_shares(flights[1][1])
    expect(kind == 'server_hello' and was_retry == retry, 'HelloRetryRequest on the wire: %s, expected %s' % (
        was_retry, retry))
    expect(len(answer) == 1 and CODEPOINTS.get(answer[0][0]) == group, 'the server chose %s' % answer)
    if retry:
        expect(answer[0][1] == 0, 'a HelloRetryRequest carries a share')
        _, _, second = key_shares(flights[2][1])
        expect(second == [(GROUPS[group][0], GROUPS[group][1])], 'the retried ClientHello shares %s' % second)
        if greased:
            expect(grease(flights[2][1]) == first, 'the retried ClientHello changed its GREASE values')
        _, again, answer = key_shares(flights[3][1])
        expect(not again, 'a second HelloRetryRequest')
    expect(answer == [(GROUPS[group][0], GROUPS[group][2])],
           'the ServerHello share is %s, expected %d bytes of %s' % (answer, GROUPS[group][2], group))


def ticket_name(key_hex):
    """A Cerid ticket key's public name: HKDF-SHA256 (RFC 5869) of the key with an empty salt, 16 bytes expanded under
    'cerid tls 1.3 ticket name'."""
    prk = hmac.new(bytes(32), bytes.fromhex(key_hex), hashlib.sha256).digest()
    return hmac.new(prk, b'cerid tls 1.3 ticket name' + b'\x01', hashlib.sha256).digest()[:16]


def psk(flight):
    """A hello's pre_shared_key: a ClientHello's (identities, last extension, psk_key_exchange_modes), a ServerHello's
    selected identity (None when it has none)."""
    kind, _, _, extensions = hello(flight)
    found = {kind_of: data for kind_of, data in extensions}
    if kind == 2:
        data = found.get(PRE_SHARED_KEY)
        return None if data is None else struct.unpack('>H', data)[0]
    identities = []
    data = found.get(PRE_SHARED_KEY, b'')
    if data:
        end = 2 + struct.unpack('>H', data[:2])[0]
        at = 2
        while at + 2 <= end:
            size = struct.unpack('>H', data[at:at + 2])[0]
            identities.append(data[at + 2:at + 2 + size])
            at += 2 + size + 4
    modes = found.get(PSK_KEY_EXCHANGE_MODES, b'')
    last = bool(extensions) and extensions[-1][0] == PRE_SHARED_KEY
    return identities, last, list(modes[1:1 + modes[0]]) if modes else []


def check_resumed_wire(flights, group, name=None):
    """A resumed handshake on the wire: one identity (led by `name`, a Cerid ticket key's), pre_shared_key last,
    psk_dhe_ke alone and a fresh share of `group` each way, the ServerHello selecting identity 0."""
    identities, last, modes = psk(flights[0][1])
    expect(len(identities) == 1 and last, 'the ClientHello offers %d identities, pre_shared_key last: %s' % (
        len(identities), last))
    expect(modes == [1], 'the ClientHello allows the key exchange modes %s, not psk_dhe_ke alone' % modes)
    if name is not None:
        expect(identities[0][:16] == name and len(identities[0]) > 64,
               'the ticket does not start with its key name: %s' % identities[0][:16].hex())
    _, _, shares = key_shares(flights[0][1])
    expect(any(CODEPOINTS.get(code) == group and length == GROUPS[group][1] for code, length in shares),
           'the resumed ClientHello shares %s' % shares)
    expect(psk(flights[1][1]) == 0, 'the ServerHello selected %s' % psk(flights[1][1]))
    _, _, answer = key_shares(flights[1][1])
    expect(answer == [(GROUPS[group][0], GROUPS[group][2])], 'the resumed ServerHello shares %s' % answer)


def check_established(cerid_report, openssl_report, group, retry, ping):
    for side, report in (('cerid', cerid_report), ('openssl', openssl_report)):
        expect(report.get('state') == 'established', '%s ended %s' % (side, report))
        expect(report.get('hello_retry') == retry, '%s saw hello_retry %s' % (side, report.get('hello_retry')))
        expect(report.get('received') == ping, '%s received %r, expected %r' % (side, report.get('received'), ping))
        expect(report.get('alert_sent') == -1 and report.get('alert_received') == -1, '%s alerts: %s' % (side, report))
    expect(cerid_report.get('group') == group, 'cerid negotiated %r' % cerid_report.get('group'))
    expect(openssl_report.get('group') in openssl_names(group), 'openssl negotiated %r' % openssl_report.get('group'))
    expect(openssl_report.get('version') == 'TLSv1.3', 'openssl ran %r' % openssl_report.get('version'))


def check_updates(cerid_report, openssl_report, cerid_sent, cerid_received):
    """Each end's KeyUpdate counts: what one sent, the other received."""
    expect(cerid_report.get('key_updates_sent') == cerid_sent and
           openssl_report.get('key_updates_received') == cerid_sent,
           'cerid sent %s key updates, openssl received %s, expected %d' % (
               cerid_report.get('key_updates_sent'), openssl_report.get('key_updates_received'), cerid_sent))
    expect(cerid_report.get('key_updates_received') == cerid_received and
           openssl_report.get('key_updates_sent') == cerid_received,
           'openssl sent %s key updates, cerid received %s, expected %d' % (
               openssl_report.get('key_updates_sent'), cerid_report.get('key_updates_received'), cerid_received))


def key_id(public_hex):
    return hashlib.sha256(bytes.fromhex(public_hex)).hexdigest()


def cases(cerid_peer, openssl_peer):
    server_public = public_key(cerid_peer, SERVER_SEED)
    client_public = public_key(cerid_peer, CLIENT_SEED)
    other_public = public_key(cerid_peer, OTHER_SEED)

    def cerid_client(groups, server_groups, group, retry, offered):
        client, server, flights = run(cerid(cerid_peer, 'client', groups, CLIENT_SEED, server_public),
                                      openssl(openssl_peer, 'server', server_groups, SERVER_SEED))
        check_wire(flights, group, retry, offered, True)
        check_established(client, server, group, retry, PING)
        expect(client.get('peer_key_id') == key_id(server_public), 'cerid pinned another key')

    def openssl_client(groups, server_groups, group, retry, offered, mutual=False):
        client, server, flights = run(
            openssl(openssl_peer, 'client', groups, CLIENT_SEED, server_public),
            cerid(cerid_peer, 'server', server_groups, SERVER_SEED, client_public if mutual else None))
        check_wire(flights, group, retry, offered, False)
        check_established(server, client, group, retry, OPENSSL_PING)
        expect(client.get('peer_ok'), 'openssl did not find the pinned key')
        expected = key_id(client_public) if mutual else '0' * 64
        expect(server.get('peer_key_id') == expected, 'cerid server saw client key %s' % server.get('peer_key_id'))

    hybrids = ('x25519mlkem768', 'secp256r1mlkem768', 'secp384r1mlkem1024')
    for name in hybrids:
        openssl_name = GROUPS[name][3][0]
        yield 'cerid client, openssl server, %s' % name, lambda n=name, o=openssl_name: cerid_client(
            [n], o, n, False, [n])
        yield 'openssl client, cerid server, %s' % name, lambda n=name, o=openssl_name: openssl_client(
            o, [n], n, False, [n])
    yield 'cerid client retries for openssl server, x25519mlkem768', lambda: cerid_client(
        ['secp256r1mlkem768', 'x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', True, ['secp256r1mlkem768'])
    yield 'openssl client retries for cerid server, secp384r1mlkem1024', lambda: openssl_client(
        '*X25519MLKEM768:SecP384r1MLKEM1024', ['secp384r1mlkem1024'], 'secp384r1mlkem1024', True, ['x25519mlkem768'])
    yield 'openssl client shares hybrid and classical, cerid server keeps the hybrid', lambda: openssl_client(
        '*X25519MLKEM768:*X25519', ['x25519mlkem768', 'x25519'], 'x25519mlkem768', False,
        ['x25519mlkem768', 'x25519'])
    yield 'cerid client shares classical and hybrid, openssl server keeps the hybrid', lambda: cerid_client(
        ['x25519', 'x25519mlkem768'], 'X25519MLKEM768:X25519', 'x25519mlkem768', False, ['x25519', 'x25519mlkem768'])
    yield 'cerid client, openssl server, x25519', lambda: cerid_client(['x25519'], 'X25519', 'x25519', False,
                                                                       ['x25519'])
    yield 'openssl client, cerid server, secp384r1', lambda: openssl_client('P-384', ['secp384r1'], 'secp384r1',
                                                                            False, ['secp384r1'])
    yield 'openssl client with a pinned credential, cerid server', lambda: openssl_client(
        'X25519MLKEM768', ['x25519mlkem768'], 'x25519mlkem768', False, ['x25519mlkem768'], mutual=True)

    def cerid_updates(extra, pings, sent, received, records):
        client, server, _ = run(cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public, extra),
                                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED))
        check_established(client, server, 'x25519mlkem768', False, PING * pings)
        check_updates(client, server, sent, received)
        expect(client.get('key_update_records') == records, 'cerid kept %s records per key, expected %d' % (
            client.get('key_update_records'), records))
    aes_limit = 23726566
    yield 'cerid client updates its key before every record under a three-record budget, openssl reads on', (
        lambda: cerid_updates(['--key-update-records', '3', '--pings', '6'], 6, 5, 0, 3))
    yield 'cerid client asks openssl for a key update and gets one', lambda: cerid_updates(
        ['--update', 'requested'], 1, 1, 1, aes_limit)
    yield 'cerid client updates its key without asking, openssl answers nothing', lambda: cerid_updates(
        ['--update', 'not-requested'], 1, 1, 0, aes_limit)
    yield 'cerid client under a three-record budget asks for an update after six records', lambda: cerid_updates(
        ['--key-update-records', '3', '--pings', '6', '--update', 'requested'], 6, 6, 1, 3)

    def openssl_asks():
        client, server, _ = run(openssl(openssl_peer, 'client', 'X25519MLKEM768', CLIENT_SEED, server_public,
                                        update='requested'),
                                cerid(cerid_peer, 'server', ['x25519mlkem768'], SERVER_SEED))
        check_established(server, client, 'x25519mlkem768', False, OPENSSL_PING)
        check_updates(server, client, 1, 1)
    yield 'openssl client asks a cerid server for a key update and gets one', openssl_asks

    def unpinned():
        client, server, _ = run(cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, other_public),
                                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED))
        expect(client.get('state') == 'failed' and client.get('alert_sent') == BAD_CERTIFICATE,
               'cerid accepted or misreported an unpinned key: %s' % client)
        expect(server.get('state') == 'failed' and server.get('alert_received') == BAD_CERTIFICATE,
               'openssl saw %s' % server)
    yield 'cerid client refuses an openssl server whose key is not pinned', unpinned

    def tls12():
        client, server, _ = run(openssl(openssl_peer, 'client', 'X25519', CLIENT_SEED, server_public, '1.2'),
                                cerid(cerid_peer, 'server', ['x25519'], SERVER_SEED))
        expect(server.get('state') == 'failed' and server.get('alert_sent') == PROTOCOL_VERSION,
               'cerid answered TLS 1.2 with %s' % server)
        expect(client.get('state') == 'failed' and client.get('alert_received') == PROTOCOL_VERSION,
               'openssl saw %s' % client)
    yield 'cerid server refuses a TLS 1.2 client with protocol_version', tls12

    def cerid_resumes():
        with tempfile.TemporaryDirectory() as scratch:
            tickets = os.path.join(scratch, 'tickets')
            store = ['--tickets', tickets]
            server_key = ['--ticket-key', TICKET_KEYS[0]]
            for again in (False, True, True):
                client, server, flights = run(
                    cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public, store),
                    openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED, extra=server_key))
                check_established(client, server, 'x25519mlkem768', False, PING)
                expect(client.get('resumed') is again and server.get('resumed') is again,
                       'resumed: cerid %s, openssl %s, expected %s' % (client.get('resumed'), server.get('resumed'),
                                                                     again))
                expect(client.get('peer_key_id') == key_id(server_public), 'cerid pinned another key')
                expect(os.path.exists(tickets), 'cerid kept no ticket of the openssl server')
                if again:
                    check_resumed_wire(flights, 'x25519mlkem768')
                else:
                    expect(not psk(flights[0][1])[0], 'a client with an empty store offered a ticket')
    yield 'cerid client resumes with the tickets of openssl servers sharing a ticket key', cerid_resumes

    def openssl_resumes(rounds, mutual=False):
        """`rounds`: (server keys, whether the connection resumes), one server process each."""
        with tempfile.TemporaryDirectory() as scratch:
            session = ['--session', os.path.join(scratch, 'session')]
            sealer = None  # the ticket key that sealed the ticket the client holds
            for keys, again in rounds:
                server_keys = ['--ticket-key', TICKET_KEYS[keys[0]]]
                server_keys += ['--previous-ticket-key', TICKET_KEYS[keys[1]]] if len(keys) > 1 else []
                client, server, flights = run(
                    openssl(openssl_peer, 'client', 'X25519MLKEM768', CLIENT_SEED, server_public, extra=session),
                    cerid(cerid_peer, 'server', ['x25519mlkem768'], SERVER_SEED, client_public if mutual else None,
                          server_keys))
                check_established(server, client, 'x25519mlkem768', False, OPENSSL_PING)
                expect(server.get('resumed') is again and client.get('resumed') is again,
                       'resumed: cerid %s, openssl %s, expected %s' % (server.get('resumed'), client.get('resumed'),
                                                                     again))
                expected = key_id(client_public) if mutual else '0' * 64
                expect(server.get('peer_key_id') == expected, 'cerid server saw client key %s' % server.get(
                    'peer_key_id'))
                if again:
                    check_resumed_wire(flights, 'x25519mlkem768', ticket_name(TICKET_KEYS[sealer]))
                else:
                    expect(client.get('peer_ok'), 'openssl did not find the pinned key in a full handshake')
                sealer = keys[0]
    yield 'openssl client resumes at a second cerid server of the fleet', lambda: openssl_resumes(
        [((0,), False), ((0,), True), ((0,), True)])
    yield 'openssl client resumes at a cerid server rotated to a new key under the previous one', lambda: (
        openssl_resumes([((0,), False), ((1, 0), True), ((2, 1), True), ((0,), False)]))
    yield 'openssl client with a pinned credential resumes, and the cerid server knows its key', lambda: (
        openssl_resumes([((0,), False), ((0,), True)], mutual=True))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--openssl', required=True)
    args = parser.parse_args()
    failures = 0
    count = 0
    for name, case in cases(args.cerid, args.openssl):
        count += 1
        try:
            case()
            print('ok      %s' % name)
        except Failure as failure:
            failures += 1
            print('FAILED  %s: %s' % (name, failure))
    print('%d of %d cases passed' % (count - failures, count))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
