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
format of tls.cpp). Certificate compression (RFC 8879) runs both ways with zstd and with zlib (OpenSSL's zstd and
zlib decoding Cerid's forms, Cerid decoding OpenSSL's), with the offer read off each ClientHello and each end's report
of the CompressedCertificate it sent or received; an end that decodes nothing is sent its certificate as it is; and a
Cerid client refuses OpenSSL servers that install hostile forms (a claimed length one byte off, claims over the bound,
a zlib claim no DEFLATE can reach) and survives seeded corruptions of real forms, establishing only when a zlib form
still decodes.

The handshake over QUIC (RFC 9001) runs crd-tls-peer's QuicHandshake against OpenSSL's QUIC TLS interface
(SSL_set_quic_tls_cbs), the frames carrying CRYPTO data per encryption level: the three hybrid groups both ways in
QUIC versions 1 and 2, a HelloRetryRequest both ways, mutual authentication both ways, the application protocol
selected in each server's order, no common protocol refused with no_application_protocol at the Initial level both
ways, an unpinned server key refused at the Handshake level, resumption both ways across processes and compressed
certificates both ways. Each ClientHello is read off the wire for its transport parameters, its protocols and an empty
legacy_session_id, beside the key share and GREASE checks above; each end reports the other's transport parameters,
and for each of the four Handshake and Application secrets a probe: a fixed packet sealed and a fixed sample's header
protection mask under the keys RFC 9001 section 5 derives from it, through crd-crypto on one end and OpenSSL's HKDF and
ciphers on the other, which must be equal.

Usage: tls_interop.py --cerid <crd-tls-peer> --openssl <crd-openssl-tls-peer>
"""

import argparse
import hashlib
import hmac
import json
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib

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
COMPRESS_CERTIFICATE = 27
ZLIB = 1
ZSTD = 3
DECODE_ERROR = 50
ILLEGAL_PARAMETER = 47
MAX_CERTIFICATE = 65536  # kTlsMaxCertificateBytes
# A certificate carrying this name twice or three times (subject, issuer, a Cerid subjectAltName) shrinks under zstd and
# zlib alike; past 64 characters (ub-common-name) a Cerid credential carries its name in the subjectAltName alone.
APPLICATION_LAYER_PROTOCOL_NEGOTIATION = 16
QUIC_TRANSPORT_PARAMETERS = 57
NO_APPLICATION_PROTOCOL = 120
GAME = 'cerid-game/1'
# Transport parameters as transports encode them (RFC 9000 section 18): the handshake carries them as bytes.
QUIC_CLIENT_PARAMETERS = '0f04c1c2c3c40480100000'
QUIC_SERVER_PARAMETERS = '0004aabbccdd0f02d1d2'
QUIC_PROBES = ('handshake-client', 'handshake-server', 'application-client', 'application-server')
NAME = 'a1234567890123456789012345678901234567890123456789.cerid.test'
LONG_NAME = ('a1234567890123456789012345678901234567890123456789012345678.'
             'b12345678901234567890123456789012345678901234567890.cerid.test')


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


def compression_offer(flight):
    """The algorithms a ClientHello offers in compress_certificate (RFC 8879 section 3), in order; [] without it."""
    _, _, _, extensions = hello(flight)
    for kind_of, data in extensions:
        if kind_of == COMPRESS_CERTIFICATE:
            expect(len(data) >= 3 and data[0] + 1 == len(data) and data[0] % 2 == 0,
                   'a malformed compress_certificate: %s' % data.hex())
            return [value for (value,) in struct.iter_unpack('>H', data[1:])]
    return []


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

    names = {ZSTD: 'zstd', ZLIB: 'zlib'}

    def cerid_decodes(algorithm, offered=('both', [ZSTD, ZLIB])):
        """A Cerid client offering `offered` against an OpenSSL server compressing with `algorithm`."""
        client, server, flights = run(
            cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public, ['--compression', offered[0]]),
            openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED,
                    extra=['--name', NAME, '--compression', names[algorithm]]))
        check_established(client, server, 'x25519mlkem768', False, PING)
        expect(compression_offer(flights[0][1]) == offered[1], 'cerid offered %s' % compression_offer(flights[0][1]))
        used = algorithm if algorithm in offered[1] else 0
        expect(client.get('certificate_compression') == used and server.get('compressed_sent') == used,
               'cerid received %s, openssl sent %s, expected %d' % (client.get('certificate_compression'),
                                                                   server.get('compressed_sent'), used))
        expect(client.get('peer_key_id') == key_id(server_public), 'cerid pinned another key')

    def openssl_decodes(algorithm, name=NAME):
        """An OpenSSL client decoding `algorithm` (or nothing) against a Cerid server named `name`, allowed both."""
        client, server, flights = run(
            openssl(openssl_peer, 'client', 'X25519MLKEM768', CLIENT_SEED, server_public,
                    extra=['--name', name, '--compression', names.get(algorithm, 'none')]),
            cerid(cerid_peer, 'server', ['x25519mlkem768'], SERVER_SEED, extra=['--name', name]))
        check_established(server, client, 'x25519mlkem768', False, OPENSSL_PING)
        offered = [algorithm] if algorithm else []
        expect(compression_offer(flights[0][1]) == offered, 'openssl offered %s' % compression_offer(flights[0][1]))
        expect(server.get('certificate_compression') == algorithm and client.get('compressed_received') == algorithm,
               'cerid sent %s, openssl received %s, expected %d' % (server.get('certificate_compression'),
                                                                   client.get('compressed_received'), algorithm))
        expect(client.get('peer_ok'), 'openssl did not find the pinned key')

    for algorithm in (ZSTD, ZLIB):
        yield 'cerid client decodes an openssl server\'s %s certificate' % names[algorithm], (
            lambda a=algorithm: cerid_decodes(a))
        yield 'openssl client decodes a cerid server\'s %s certificate' % names[algorithm], (
            lambda a=algorithm: openssl_decodes(a))
    yield 'cerid server sends its certificate as it is to an openssl client that decodes none', (
        lambda: openssl_decodes(0))
    yield 'cerid client that decodes none is sent the openssl server\'s certificate as it is', (
        lambda: cerid_decodes(ZSTD, ('none', [])))
    yield 'cerid client that decodes zstd alone is sent no zlib', lambda: cerid_decodes(ZLIB, ('zstd', [ZSTD]))
    yield 'openssl client takes a cerid credential named past 64 characters, compressed', (
        lambda: openssl_decodes(ZSTD, LONG_NAME))

    def installed(algorithm, form, claim):
        """A Cerid client against an OpenSSL server that installs `form` claiming `claim` bytes."""
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, 'form')
            with open(path, 'wb') as file:
                file.write(struct.pack('>I', claim) + form)
            client, server, _ = run(
                cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public),
                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED,
                        extra=['--name', NAME, '--compression', names[algorithm], '--compressed-cert', path]))
        return client, server

    def dumped(algorithm):
        """(the form an OpenSSL server compresses its certificate to, its uncompressed length)."""
        with tempfile.TemporaryDirectory() as scratch:
            path = os.path.join(scratch, 'form')
            client, server, _ = run(
                cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public),
                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED,
                        extra=['--name', NAME, '--compression', names[algorithm], '--dump-compressed', path]))
            check_established(client, server, 'x25519mlkem768', False, PING)
            with open(path, 'rb') as file:
                data = file.read()
        return data[4:], struct.unpack('>I', data[:4])[0]

    def refused(client, server, alert):
        expect(client.get('state') == 'failed' and client.get('alert_sent') == alert,
               'cerid answered with %s, expected alert %d' % (client, alert))
        expect(server.get('state') == 'failed' and server.get('alert_received') == alert, 'openssl saw %s' % server)
        expect(client.get('peer_key_id') == '0' * 64, 'cerid took a key from a refused certificate')

    def hostile_forms():
        for algorithm in (ZSTD, ZLIB):
            form, length = dumped(algorithm)
            if algorithm == ZLIB:
                expect(len(zlib.decompress(form)) == length, 'openssl\'s zlib form does not decode to its length')
            client, server = installed(algorithm, form, length)  # the honest form, installed: the control
            check_established(client, server, 'x25519mlkem768', False, PING)
            expect(client.get('certificate_compression') == algorithm, 'cerid received %s' % client)
            for claim in (length + 1, length - 1):
                refused(*installed(algorithm, form, claim), DECODE_ERROR)  # decodes to another length
            for claim in (MAX_CERTIFICATE + 1, 1 << 20):
                refused(*installed(algorithm, form, claim), ILLEGAL_PARAMETER)  # over the bound: the provider's check
        # 9 bytes of zlib hold 3 bytes of DEFLATE, at most 3096 decoded bytes: a longer claim is refused undecoded.
        refused(*installed(ZLIB, zlib.compress(b'a'), 3 * 1032 + 1), DECODE_ERROR)
    yield 'cerid client refuses openssl servers installing compressed certificates of a false length', hostile_forms

    def corrupted_forms():
        shuffle = random.Random(8879)
        for algorithm in (ZSTD, ZLIB):
            form, length = dumped(algorithm)
            for _ in range(12):
                changed = bytearray(form)
                at = shuffle.randrange(len(changed))
                changed[at] ^= 1 << shuffle.randrange(8)
                client, server = installed(algorithm, bytes(changed), length)
                expect(client.get('state') in ('established', 'failed'), 'cerid ended %s' % client)
                if client.get('state') == 'failed':
                    expect(client.get('alert_sent') in (DECODE_ERROR, BAD_CERTIFICATE, ILLEGAL_PARAMETER),
                           'cerid answered a corrupted form with %s' % client)
                    continue
                expect(client.get('peer_key_id') == key_id(server_public), 'cerid established with another key')
                if algorithm == ZLIB:
                    try:
                        decodes = len(zlib.decompress(bytes(changed))) == length
                    except zlib.error:
                        decodes = False
                    expect(decodes, 'cerid accepted a zlib form that does not decode (byte %d)' % at)
    yield 'cerid client survives seeded corruptions of openssl\'s zstd and zlib forms', corrupted_forms
    yield from quic_cases(cerid_peer, openssl_peer, server_public, client_public, other_public)


def quic_pieces(frame):
    """A QUIC frame's CRYPTO data pieces: (level, bytes), a level byte and a 24-bit length each."""
    pieces = []
    at = 0
    while at + 4 <= len(frame):
        size = int.from_bytes(frame[at + 1:at + 4], 'big')
        pieces.append((frame[at], frame[at + 4:at + 4 + size]))
        at += 4 + size
    expect(at == len(frame), 'a QUIC frame ends inside a piece')
    return pieces


def quic_initial(frame):
    """A QUIC frame's Initial CRYPTO data as one handshake record, so the record checks read its hellos."""
    data = b''.join(data for level, data in quic_pieces(frame) if level == 0)
    return bytes([22, 3, 3]) + struct.pack('>H', len(data)) + data


def check_quic_hello(frame, parameters, protocols):
    """A QUIC ClientHello: its transport parameters (57) as sent, its application protocols (16) in order, and an
    empty legacy_session_id (RFC 9001 section 8.4)."""
    record = quic_initial(frame)
    _, body = first_handshake(record)
    expect(body[34] == 0, 'a QUIC ClientHello carries a legacy_session_id')
    found = dict(hello(record)[3])
    expect(found.get(QUIC_TRANSPORT_PARAMETERS) == bytes.fromhex(parameters),
           'the ClientHello carries transport parameters %s' % found.get(QUIC_TRANSPORT_PARAMETERS))
    offered = []
    data = found.get(APPLICATION_LAYER_PROTOCOL_NEGOTIATION, b'')
    at = 2
    while at < len(data):
        offered.append(data[at + 1:at + 1 + data[at]].decode())
        at += 1 + data[at]
    expect(offered == list(protocols), 'the ClientHello offers the protocols %s' % offered)


def check_quic(cerid_report, openssl_report, cerid_client, group, retry, protocol=GAME,
               suite='TLS_AES_256_GCM_SHA384'):
    """Both ends established over QUIC with `group`, the same application protocol, each other's transport
    parameters, and the same probe for each of the four secrets: one end's keys open the other's packets."""
    for side, report in (('cerid', cerid_report), ('openssl', openssl_report)):
        expect(report.get('state') == 'established', '%s ended %s' % (side, report))
        expect(report.get('hello_retry') == retry, '%s saw hello_retry %s' % (side, report.get('hello_retry')))
        expect(report.get('alert_sent') == -1, '%s sent alert %s' % (side, report.get('alert_sent')))
        expect(report.get('alpn') == protocol, '%s agreed on %r' % (side, report.get('alpn')))
        expect(report.get('suite') == suite, '%s negotiated %r, expected %s' % (side, report.get('suite'), suite))
    expect(cerid_report.get('group') == group, 'cerid negotiated %r' % cerid_report.get('group'))
    expect(openssl_report.get('group') in openssl_names(group), 'openssl negotiated %r' % openssl_report.get('group'))
    expect(openssl_report.get('version') == 'TLSv1.3', 'openssl ran %r' % openssl_report.get('version'))
    client_parameters, server_parameters = QUIC_CLIENT_PARAMETERS, QUIC_SERVER_PARAMETERS
    cerid_sent, openssl_sent = ((client_parameters, server_parameters) if cerid_client
                                else (server_parameters, client_parameters))
    expect(openssl_report.get('peer_parameters') == cerid_sent and cerid_report.get('peer_parameters') == openssl_sent,
           'transport parameters received: cerid %s, openssl %s' % (cerid_report.get('peer_parameters'),
                                                                    openssl_report.get('peer_parameters')))
    probes = cerid_report.get('probes', {})
    expect(sorted(probes) == sorted(QUIC_PROBES) and probes == openssl_report.get('probes'),
           'the secrets disagree: cerid %s, openssl %s' % (probes, openssl_report.get('probes')))


def quic_cases(cerid_peer, openssl_peer, server_public, client_public, other_public):
    """The TLS 1.3 handshake over QUIC (RFC 9001) against OpenSSL's QUIC TLS interface."""

    def quic(version, parameters, protocols=(GAME,)):
        return ['--quic', str(version), '--parameters', parameters, '--alpn', ','.join(protocols)]

    def cerid_client(groups, server_groups, group, retry, offered, version, mutual=False, extra=(), openssl_extra=(),
                     suite='TLS_AES_256_GCM_SHA384'):
        client, server, flights = run(
            cerid(cerid_peer, 'client', groups, CLIENT_SEED, server_public,
                  quic(version, QUIC_CLIENT_PARAMETERS) + list(extra)),
            openssl(openssl_peer, 'server', server_groups, SERVER_SEED, client_public if mutual else None,
                    extra=quic(version, QUIC_SERVER_PARAMETERS) + list(openssl_extra)))
        check_quic_hello(flights[0][1], QUIC_CLIENT_PARAMETERS, [GAME])
        check_wire([(who, quic_initial(frame)) for who, frame in flights], group, retry, offered, True)
        check_quic(client, server, True, group, retry, suite=suite)
        expect(client.get('peer_key_id') == key_id(server_public), 'cerid pinned another key')
        expect(not mutual or server.get('peer_ok'), 'openssl did not find the pinned cerid client key')
        return client, server, flights

    def openssl_client(groups, server_groups, group, retry, offered, version, mutual=False, extra=(),
                       openssl_extra=(), suite='TLS_AES_256_GCM_SHA384'):
        client, server, flights = run(
            openssl(openssl_peer, 'client', groups, CLIENT_SEED, server_public,
                    extra=quic(version, QUIC_CLIENT_PARAMETERS) + list(openssl_extra)),
            cerid(cerid_peer, 'server', server_groups, SERVER_SEED, client_public if mutual else None,
                  quic(version, QUIC_SERVER_PARAMETERS) + list(extra)))
        check_quic_hello(flights[0][1], QUIC_CLIENT_PARAMETERS, [GAME])
        check_wire([(who, quic_initial(frame)) for who, frame in flights], group, retry, offered, False)
        check_quic(server, client, False, group, retry, suite=suite)
        expect(client.get('peer_ok') or client.get('resumed'), 'openssl did not find the pinned key')
        expected = key_id(client_public) if mutual else '0' * 64
        expect(server.get('peer_key_id') == expected, 'cerid server saw client key %s' % server.get('peer_key_id'))
        return client, server, flights

    hybrids = ('x25519mlkem768', 'secp256r1mlkem768', 'secp384r1mlkem1024')
    for index, name in enumerate(hybrids):
        openssl_name = GROUPS[name][3][0]
        version = 1 + index % 2
        yield 'QUIC v%d: cerid client, openssl server, %s' % (version, name), (
            lambda n=name, o=openssl_name, v=version: cerid_client([n], o, n, False, [n], v))
        yield 'QUIC v%d: openssl client, cerid server, %s' % (3 - version, name), (
            lambda n=name, o=openssl_name, v=3 - version: openssl_client(o, [n], n, False, [n], v))
    # OpenSSL lists TLS_AES_256_GCM_SHA384 first (SHA-384 secrets of 48 bytes); the other suites, AES-128-GCM and
    # ChaCha20-Poly1305 with its ChaCha20 header protection, are chosen here. An OpenSSL server's order decides; a Cerid
    # server takes its client's first suite.
    for suite, version in (('TLS_AES_128_GCM_SHA256', 2), ('TLS_CHACHA20_POLY1305_SHA256', 1)):
        yield 'QUIC v%d: cerid client, openssl server preferring %s' % (version, suite), (
            lambda s=suite, v=version: cerid_client(['x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', False,
                                                    ['x25519mlkem768'], v, openssl_extra=['--suites', s], suite=s))
        yield 'QUIC v%d: openssl client offering %s, cerid server' % (3 - version, suite), (
            lambda s=suite, v=3 - version: openssl_client('X25519MLKEM768', ['x25519mlkem768'], 'x25519mlkem768',
                                                          False, ['x25519mlkem768'], v,
                                                          openssl_extra=['--suites', s], suite=s))
    yield 'QUIC: cerid client retries for openssl server, x25519mlkem768', lambda: cerid_client(
        ['secp256r1mlkem768', 'x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', True, ['secp256r1mlkem768'], 1)
    yield 'QUIC: openssl client retries for cerid server, secp384r1mlkem1024', lambda: openssl_client(
        '*X25519MLKEM768:SecP384r1MLKEM1024', ['secp384r1mlkem1024'], 'secp384r1mlkem1024', True, ['x25519mlkem768'],
        2)
    yield 'QUIC: openssl client with a pinned credential, cerid server', lambda: openssl_client(
        'X25519MLKEM768', ['x25519mlkem768'], 'x25519mlkem768', False, ['x25519mlkem768'], 1, mutual=True)
    yield 'QUIC: cerid client with a pinned credential, openssl server', lambda: cerid_client(
        ['x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', False, ['x25519mlkem768'], 2, mutual=True)

    def selects(cerid_server):
        """The server's order decides the application protocol both ways."""
        offer = ('cerid-project/1', GAME)
        order = (GAME, 'cerid-project/1')
        if cerid_server:
            client, server, flights = run(
                openssl(openssl_peer, 'client', 'X25519MLKEM768', CLIENT_SEED, server_public,
                        extra=quic(1, QUIC_CLIENT_PARAMETERS, offer)),
                cerid(cerid_peer, 'server', ['x25519mlkem768'], SERVER_SEED,
                      extra=quic(1, QUIC_SERVER_PARAMETERS, order)))
            check_quic(server, client, False, 'x25519mlkem768', False)
        else:
            client, server, flights = run(
                cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public,
                      quic(1, QUIC_CLIENT_PARAMETERS, offer)),
                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED,
                        extra=quic(1, QUIC_SERVER_PARAMETERS, order)))
            check_quic(client, server, True, 'x25519mlkem768', False)
        check_quic_hello(flights[0][1], QUIC_CLIENT_PARAMETERS, offer)
    yield 'QUIC: a cerid server selects the application protocol in its own order', lambda: selects(True)
    yield 'QUIC: an openssl server selects the application protocol in its own order', lambda: selects(False)

    def no_protocol(cerid_server):
        """No common application protocol: the server refuses with no_application_protocol at the Initial level."""
        other = ('cerid-agent/1',)
        if cerid_server:
            client, server, _ = run(
                openssl(openssl_peer, 'client', 'X25519MLKEM768', CLIENT_SEED, server_public,
                        extra=quic(1, QUIC_CLIENT_PARAMETERS, other)),
                cerid(cerid_peer, 'server', ['x25519mlkem768'], SERVER_SEED, extra=quic(1, QUIC_SERVER_PARAMETERS)))
            expect(server.get('state') == 'failed' and server.get('alert_sent') == NO_APPLICATION_PROTOCOL and
                   server.get('alert_level') == 0, 'cerid answered with %s' % server)
            expect(client.get('state') != 'established', 'openssl established without a protocol')
        else:
            client, server, _ = run(
                cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, server_public,
                      quic(1, QUIC_CLIENT_PARAMETERS, other)),
                openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED, extra=quic(1, QUIC_SERVER_PARAMETERS)))
            expect(server.get('state') == 'failed' and server.get('alert_sent') == NO_APPLICATION_PROTOCOL and
                   server.get('alert_level') == 0, 'openssl answered with %s' % server)
            expect(client.get('state') == 'handshaking' and client.get('peer_key_id') == '0' * 64,
                   'cerid went on with %s' % client)
    yield 'QUIC: a cerid server refuses an openssl client without a common protocol', lambda: no_protocol(True)
    yield 'QUIC: an openssl server refuses a cerid client without a common protocol', lambda: no_protocol(False)

    def unpinned():
        client, server, _ = run(
            cerid(cerid_peer, 'client', ['x25519mlkem768'], CLIENT_SEED, other_public, quic(1, QUIC_CLIENT_PARAMETERS)),
            openssl(openssl_peer, 'server', 'X25519MLKEM768', SERVER_SEED, extra=quic(1, QUIC_SERVER_PARAMETERS)))
        expect(client.get('state') == 'failed' and client.get('alert_sent') == BAD_CERTIFICATE and
               client.get('alert_level') == 1, 'cerid accepted or misreported an unpinned key: %s' % client)
        expect(client.get('peer_key_id') == '0' * 64, 'cerid took the unpinned key')
    yield 'QUIC: cerid client refuses an openssl server key that is not pinned at the Handshake level', unpinned

    def cerid_resumes():
        with tempfile.TemporaryDirectory() as scratch:
            store = ['--tickets', os.path.join(scratch, 'tickets')]
            for again in (False, True, True):
                client, server, flights = cerid_client(
                    ['x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', False, ['x25519mlkem768'], 1,
                    extra=store, openssl_extra=['--ticket-key', TICKET_KEYS[0]])
                expect(client.get('resumed') is again and server.get('resumed') is again,
                       'resumed: cerid %s, openssl %s, expected %s' % (client.get('resumed'), server.get('resumed'),
                                                                     again))
                if again:
                    check_resumed_wire([(who, quic_initial(frame)) for who, frame in flights], 'x25519mlkem768')
    yield 'QUIC: cerid client resumes with the tickets of openssl servers sharing a ticket key', cerid_resumes

    def openssl_resumes():
        with tempfile.TemporaryDirectory() as scratch:
            session = ['--session', os.path.join(scratch, 'session')]
            for again in (False, True, True):
                client, server, flights = openssl_client(
                    'X25519MLKEM768', ['x25519mlkem768'], 'x25519mlkem768', False, ['x25519mlkem768'], 2,
                    extra=['--ticket-key', TICKET_KEYS[1]], openssl_extra=session)
                expect(client.get('resumed') is again and server.get('resumed') is again,
                       'resumed: cerid %s, openssl %s, expected %s' % (server.get('resumed'), client.get('resumed'),
                                                                     again))
                if again:
                    check_resumed_wire([(who, quic_initial(frame)) for who, frame in flights], 'x25519mlkem768',
                                       ticket_name(TICKET_KEYS[1]))
    yield 'QUIC: openssl client resumes at a second cerid server of the fleet', openssl_resumes

    def compressed(cerid_server):
        if cerid_server:
            client, server, _ = openssl_client('X25519MLKEM768', ['x25519mlkem768'], 'x25519mlkem768', False,
                                               ['x25519mlkem768'], 1, extra=['--name', NAME],
                                               openssl_extra=['--name', NAME, '--compression', 'zstd'])
            expect(server.get('certificate_compression') == ZSTD and client.get('compressed_received') == ZSTD,
                   'cerid sent %s, openssl received %s' % (server.get('certificate_compression'),
                                                          client.get('compressed_received')))
        else:
            client, server, _ = cerid_client(['x25519mlkem768'], 'X25519MLKEM768', 'x25519mlkem768', False,
                                             ['x25519mlkem768'], 2,
                                             openssl_extra=['--name', NAME, '--compression', 'zlib'])
            expect(client.get('certificate_compression') == ZLIB and server.get('compressed_sent') == ZLIB,
                   'cerid received %s, openssl sent %s' % (client.get('certificate_compression'),
                                                          server.get('compressed_sent')))
    yield 'QUIC: openssl client decodes a cerid server\'s zstd certificate', lambda: compressed(True)
    yield 'QUIC: cerid client decodes an openssl server\'s zlib certificate', lambda: compressed(False)


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
