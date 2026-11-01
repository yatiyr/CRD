#!/usr/bin/env python3
"""TLS 1.3 interoperability of crd-crypto's sessions with the pinned OpenSSL 3.5 (NET.2).

Runs crd-tls-peer (crd-crypto over AWS-LC) against crd-openssl-tls-peer (OpenSSL), each in its own process, and
relays their TLS records over their standard streams (4-byte big-endian frames). Every case checks both ends' own
reports (state, negotiated group, HelloRetryRequest, the pinned peer key, alerts, the application data echoed) and
reads the handshake off the wire independently of both libraries: the key share groups and lengths of the ClientHello,
the ServerHello or HelloRetryRequest and a retried ClientHello against the hybrid groups' layout (RFC 10024 and
draft-ietf-tls-ecdhe-mlkem: ML-KEM-768 encapsulation key 1184 bytes and ciphertext 1088, ML-KEM-1024 1568 and 1568,
X25519 32, uncompressed P-256 65 and P-384 97). The three hybrid groups run both ways, through a HelloRetryRequest both
ways, beside a classical share without a downgrade, with mutual authentication, with a key that is not pinned and
against a peer that offers TLS 1.2 only.

Usage: tls_interop.py --cerid <crd-tls-peer> --openssl <crd-openssl-tls-peer>
"""

import argparse
import hashlib
import json
import struct
import subprocess
import sys

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


def key_shares(flight):
    """(message, hello_retry, [(group codepoint, share length)]) from a ClientHello or a ServerHello (RFC 9846 4.1)."""
    kind, body = first_handshake(flight)
    if kind not in (1, 2):
        raise Failure('expected a hello, found handshake type %d' % kind)
    random = body[2:34]
    at = 34
    at += 1 + body[at]  # legacy_session_id
    if kind == 1:
        at += 2 + struct.unpack('>H', body[at:at + 2])[0]  # cipher_suites
        at += 1 + body[at]  # legacy_compression_methods
    else:
        at += 3  # cipher_suite, legacy_compression_method
    end = at + 2 + struct.unpack('>H', body[at:at + 2])[0]
    at += 2
    shares = []
    while at + 4 <= end:
        kind_of, size = struct.unpack('>HH', body[at:at + 4])
        data = body[at + 4:at + 4 + size]
        at += 4 + size
        if kind_of != 51:
            continue
        if kind == 1:
            inner = 2
            while inner + 4 <= len(data):
                group, length = struct.unpack('>HH', data[inner:inner + 4])
                shares.append((group, length))
                inner += 4 + length
        elif size == 2:
            shares.append((struct.unpack('>H', data)[0], 0))
        else:
            group, length = struct.unpack('>HH', data[:4])
            shares.append((group, length))
    if at != end:
        raise Failure('the extensions do not end where their length says')
    return ('client_hello' if kind == 1 else 'server_hello'), (kind == 2 and random == HELLO_RETRY_RANDOM), shares


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


def cerid(binary, role, groups, seed, pin=None):
    command = [binary, '--role', role, '--groups', ','.join(groups), '--seed', seed]
    return command + (['--pin', pin] if pin else [])


def openssl(binary, role, groups, seed, pin=None, max_version=None):
    command = [binary, '--role', role, '--groups', groups, '--seed', seed]
    command += ['--pin', pin] if pin else []
    return command + (['--max-version', max_version] if max_version else [])


def public_key(binary, seed):
    result = subprocess.run([binary, '--public', seed], capture_output=True, text=True, check=True)
    return result.stdout.strip()


def expect(condition, message):
    if not condition:
        raise Failure(message)


def check_wire(flights, group, retry, offered):
    """The hello flights on the wire: `offered` shares first, then (after a retry) one share of `group`."""
    hello_kind, _, shares = key_shares(flights[0][1])
    expect(hello_kind == 'client_hello', 'the first flight is not a ClientHello')
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
        _, again, answer = key_shares(flights[3][1])
        expect(not again, 'a second HelloRetryRequest')
    expect(answer == [(GROUPS[group][0], GROUPS[group][2])],
           'the ServerHello share is %s, expected %d bytes of %s' % (answer, GROUPS[group][2], group))


def check_established(cerid_report, openssl_report, group, retry, ping):
    for side, report in (('cerid', cerid_report), ('openssl', openssl_report)):
        expect(report.get('state') == 'established', '%s ended %s' % (side, report))
        expect(report.get('hello_retry') == retry, '%s saw hello_retry %s' % (side, report.get('hello_retry')))
        expect(report.get('received') == ping, '%s received %r, expected %r' % (side, report.get('received'), ping))
        expect(report.get('alert_sent') == -1 and report.get('alert_received') == -1, '%s alerts: %s' % (side, report))
    expect(cerid_report.get('group') == group, 'cerid negotiated %r' % cerid_report.get('group'))
    expect(openssl_report.get('group') in openssl_names(group), 'openssl negotiated %r' % openssl_report.get('group'))
    expect(openssl_report.get('version') == 'TLSv1.3', 'openssl ran %r' % openssl_report.get('version'))


def key_id(public_hex):
    return hashlib.sha256(bytes.fromhex(public_hex)).hexdigest()


def cases(cerid_peer, openssl_peer):
    server_public = public_key(cerid_peer, SERVER_SEED)
    client_public = public_key(cerid_peer, CLIENT_SEED)
    other_public = public_key(cerid_peer, OTHER_SEED)

    def cerid_client(groups, server_groups, group, retry, offered):
        client, server, flights = run(cerid(cerid_peer, 'client', groups, CLIENT_SEED, server_public),
                                      openssl(openssl_peer, 'server', server_groups, SERVER_SEED))
        check_wire(flights, group, retry, offered)
        check_established(client, server, group, retry, PING)
        expect(client.get('peer_key_id') == key_id(server_public), 'cerid pinned another key')

    def openssl_client(groups, server_groups, group, retry, offered, mutual=False):
        client, server, flights = run(
            openssl(openssl_peer, 'client', groups, CLIENT_SEED, server_public),
            cerid(cerid_peer, 'server', server_groups, SERVER_SEED, client_public if mutual else None))
        check_wire(flights, group, retry, offered)
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
