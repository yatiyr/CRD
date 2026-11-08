#!/usr/bin/env python3
"""QUIC interoperability of crd-net's server and client with the pinned ngtcp2 over OpenSSL 4.0 (4.2a).

Runs crd-quic-peer (crd-net over AWS-LC) against crd-ngtcp2-quic-peer, each in its own process over real UDP sockets on
the loopback, both ways round. The application is an echo on bidirectional streams: a client opens streams and writes a
pattern with a FIN, a server writes back as many bytes of another pattern as it reads and then its FIN, and each end
checks every byte. Every case reads both ends' reports and holds them to each other: the handshake completed, the
version, the streams and bytes each end counted, the pinned certificate, the group, suite and application protocol
OpenSSL negotiated, and the close the closing end chose as the other end read it.

Cases: QUIC versions 1 and 2 each way with each end closing; the three hybrid groups and X25519; the three AEADs; a
transfer far over the stream and connection windows; more streams than either end allows at once, so MAX_STREAMS turns
over; compatible version negotiation (RFC 9368) to version 2; a Retry from each server, counted off the wire; key
updates each end starts, counted by ngtcp2; transfers through a relay (seeded loss, duplication, delay and jitter,
which reorders) that stands on the path between the two; unreliable datagrams (RFC 9221) at and over the peer's limit,
beside streams and over the relay; resumption, 0-RTT and a server's refusal of 0-RTT on a second connection; Version
Negotiation to the one version a server runs, counted off the wire; and a stateless reset from a server restarted on
the same port, which has forgotten the connection and keeps the key its tokens come from.
"""
import argparse
import heapq
import random
import select
import socket
import subprocess
import sys
import threading
import time

SEED = '11' * 32
OPENSSL_GROUPS = {
    'x25519mlkem768': 'X25519MLKEM768',
    'secp256r1mlkem768': 'SecP256r1MLKEM768',
    'secp384r1mlkem1024': 'SecP384r1MLKEM1024',
    'x25519': 'X25519',
}
OPENSSL_CIPHERS = {
    'aes128': 'TLS_AES_128_GCM_SHA256',
    'aes256': 'TLS_AES_256_GCM_SHA384',
    'chacha20': 'TLS_CHACHA20_POLY1305_SHA256',
}
RUN_SECONDS = 70
PROGRESS_SECONDS = 20
RESET_STREAM_BYTES = 32 << 20
PROGRESS_BYTES = 1 << 20


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


def reports_of(text):
    """One dictionary per connection, each opened by its `connection=N` line."""
    reports = []
    for line in text.splitlines():
        if line.startswith('connection='):
            reports.append({})
        if '=' in line and reports:
            name, value = line.split('=', 1)
            reports[-1][name] = value
    return reports


def collect(process, into):
    into.append(process.stdout.read())


class Relay:
    """The path between the two: a socket facing the client, one facing the server, and seeded impairments."""

    def __init__(self, server_port, seed=0, loss=0.0, duplicate=0.0, delay=0.0, jitter=0.0):
        self.server = ('127.0.0.1', server_port)
        self.random = random.Random(seed)
        self.loss = loss
        self.duplicate = duplicate
        self.delay = delay
        self.jitter = jitter
        self.client_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client_side.bind(('127.0.0.1', 0))
        self.server_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server_side.bind(('127.0.0.1', 0))
        self.port = self.client_side.getsockname()[1]
        self.client = None
        self.retries = 0
        self.negotiations = 0
        self.running = True
        self.heap = []
        self.order = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    @staticmethod
    def is_retry(datagram):
        if len(datagram) < 5 or (datagram[0] & 0x80) == 0:
            return False
        version = int.from_bytes(datagram[1:5], 'big')
        kind = (datagram[0] >> 4) & 3
        return (version == 1 and kind == 3) or (version == 0x6B3343CF and kind == 0)

    @staticmethod
    def is_version_negotiation(datagram):
        return len(datagram) >= 5 and (datagram[0] & 0x80) != 0 and datagram[1:5] == b'\0\0\0\0'

    def schedule(self, sender, datagram, destination):
        if self.random.random() < self.loss:
            return
        copies = 2 if self.random.random() < self.duplicate else 1
        for _ in range(copies):
            due = time.monotonic() + self.delay + (self.random.random() * self.jitter)
            self.order += 1
            heapq.heappush(self.heap, (due, self.order, sender, datagram, destination))

    def run(self):
        sockets = [self.client_side, self.server_side]
        while self.running:
            wait = max(0.0, self.heap[0][0] - time.monotonic()) if self.heap else 0.05
            ready, _, _ = select.select(sockets, [], [], min(wait, 0.05))
            for side in ready:
                datagram, sender = side.recvfrom(65536)
                if side is self.client_side:
                    self.client = sender
                    self.schedule(self.server_side, datagram, self.server)
                elif self.client is not None:
                    self.retries += 1 if self.is_retry(datagram) else 0
                    self.negotiations += 1 if self.is_version_negotiation(datagram) else 0
                    self.schedule(self.client_side, datagram, self.client)
            while self.heap and self.heap[0][0] <= time.monotonic():
                _, _, sender, datagram, destination = heapq.heappop(self.heap)
                sender.sendto(datagram, destination)

    def stop(self):
        self.running = False
        self.thread.join(5)
        self.client_side.close()
        self.server_side.close()


class Case:
    def __init__(self, cerid, ngtcp2, cerid_is_client, version=1, group='x25519mlkem768', cipher='aes128', streams=1,
                 size=4096, closer='client', code=0, max_streams=16, compat=False, retry=False, updater=None,
                 updates=64, path=None, datagrams=0, datagram_bytes=1000, datagram_limit=None, resume=None,
                 negotiate=False, reset=False):
        self.cerid = cerid
        self.ngtcp2 = ngtcp2
        self.cerid_is_client = cerid_is_client
        self.version = version
        self.group = group
        self.cipher = cipher
        self.streams = streams
        self.size = size
        self.closer = closer
        self.code = code
        self.max_streams = max_streams
        self.compat = compat
        self.retry = retry
        self.updater = updater  # 'cerid' or 'ngtcp2': the end that starts key updates
        self.updates = updates  # every this many of its packets
        self.path = path  # a Relay's impairments, as a dictionary
        self.datagrams = datagrams
        self.datagram_bytes = datagram_bytes
        self.datagram_limit = datagram_limit  # each end's max_datagram_frame_size; datagrams then fill it exactly
        self.resume = resume  # 'plain', 'early' or 'reject': a second connection on the first's ticket
        self.negotiate = negotiate  # the client starts in `version`, and the server runs only the other one
        self.reset = reset

    def command(self, program, role, port):
        cerid = program == self.cerid
        command = [program, '--role', role, '--port', str(port), '--seed', SEED,
                   '--group', self.group if cerid else OPENSSL_GROUPS[self.group],
                   '--cipher', self.cipher if cerid else OPENSSL_CIPHERS[self.cipher],
                   '--version', str(self.version), '--streams', str(self.streams), '--bytes', str(self.size),
                   '--closer', self.closer, '--code', str(self.code), '--max-streams', str(self.max_streams)]
        if self.compat:
            command += ['--compat', '1']
        if self.retry and role == 'server':
            command += ['--retry', '1']
        if self.updater == ('cerid' if cerid else 'ngtcp2'):
            command += ['--key-update', str(self.updates)]
        if self.datagrams:
            command += ['--datagrams', str(self.datagrams), '--datagram-bytes', str(self.datagram_bytes)]
        if self.datagram_limit:
            command += ['--datagram-limit', str(self.datagram_limit), '--datagram-probe', '1']
        if self.resume:
            command += ['--connections', '2']
        if self.resume in ('early', 'reject'):
            command += ['--early', '1']
        if self.resume == 'reject' and role == 'server':
            command += ['--reject-early', '1']
        if self.negotiate:
            if role == 'server':
                command[command.index('--version') + 1] = str(3 - self.version)
                command += ['--only-version', '1']
            else:
                command += ['--compat', '1']
        if self.reset and role == 'server':
            command += ['--progress-at', str(PROGRESS_BYTES)]
            command += [] if cerid else ['--resets', '1']
        return command

    def describe(self):
        extra = (' compat' if self.compat else '') + (' retry' if self.retry else '') + \
            (f' {self.updater} updates' if self.updater else '') + (f' path {self.path}' if self.path else '') + \
            (f' {self.datagrams} datagrams of {self.datagram_bytes}' if self.datagrams else '') + \
            (f' limit {self.datagram_limit}' if self.datagram_limit else '') + \
            (f' resume {self.resume}' if self.resume else '') + (' version negotiation' if self.negotiate else '') + \
            (' stateless reset' if self.reset else '')
        return ('cerid client' if self.cerid_is_client else 'ngtcp2 client') + \
            f' v{self.version} {self.group} {self.cipher} {self.streams}x{self.size} {self.closer} closes {self.code}' \
            + extra

    def run(self):
        server_program, client_program = self.ngtcp2, self.cerid
        if not self.cerid_is_client:
            server_program, client_program = client_program, server_program
        server = subprocess.Popen(self.command(server_program, 'server', 0), stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
        client = None
        relay = None
        try:
            first = server.stdout.readline().strip()
            if not first.startswith('port='):
                raise AssertionError(f'the server printed {first!r}; {server.stderr.read()}')
            port = int(first[len('port='):])
            if self.path is not None or self.retry or self.negotiate:
                relay = Relay(port, **(self.path or {}))
                port = relay.port
            client = subprocess.Popen(self.command(client_program, 'client', port), stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE, text=True)
            server_out = []
            thread = threading.Thread(target=collect, args=(server, server_out), daemon=True)
            thread.start()
            client_out, client_err = client.communicate(timeout=RUN_SECONDS)
            thread.join(RUN_SECONDS)
            expect(not thread.is_alive(), f'the server did not end; the client reported {client_out}')
            server.wait(10)
            server_err = server.stderr.read()
            self.check(reports_of(client_out), reports_of(server_out[0]), client.returncode, server.returncode,
                       client_err, server_err)
            if self.retry:
                # One per Initial of the first flight, which a hybrid key share makes two datagrams long.
                expect(1 <= relay.retries <= 2, f'{relay.retries} Retry packets crossed the path')
            if self.negotiate:
                expect(1 <= relay.negotiations <= 2, f'{relay.negotiations} Version Negotiation packets crossed')
        finally:
            if relay is not None:
                relay.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def run_reset(self):
        """A server killed in the middle of a transfer, and another on its port with the same key: the client's next
        packet draws a stateless reset that ends its connection."""
        server_program, client_program = self.ngtcp2, self.cerid
        if not self.cerid_is_client:
            server_program, client_program = client_program, server_program
        pipes = dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        first = subprocess.Popen(self.command(server_program, 'server', 0), **pipes)
        client = None
        second = None
        try:
            line = first.stdout.readline().strip()
            if not line.startswith('port='):
                raise AssertionError(f'the server printed {line!r}; {first.stderr.read()}')
            port = int(line[len('port='):])
            client = subprocess.Popen(self.command(client_program, 'client', port), **pipes)
            timer = threading.Timer(PROGRESS_SECONDS, first.kill)
            timer.start()
            progress = first.stdout.readline().strip()
            timer.cancel()
            expect(progress == 'progress=1', f'the server printed {progress!r}')
            first.kill()
            first.wait()
            second = subprocess.Popen(self.command(server_program, 'server', port), **pipes)
            expect(second.stdout.readline().strip() == f'port={port}', 'the second server did not take the port')
            client_out, client_err = client.communicate(timeout=RUN_SECONDS)
            reports = reports_of(client_out)
            detail = f'client {reports} ({client.returncode}) {client_err}'
            expect(client.returncode == 0 and len(reports) == 1, f'the client failed: {detail}')
            expect(reports[0].get('established') == '1', f'the client never established: {detail}')
            expect(reports[0].get('verified') == '1', f'the client read a wrong byte: {detail}')
            expect(reports[0].get('close') == 'reset', f'no stateless reset ended the connection: {detail}')
        finally:
            for process in (first, client, second):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check(self, clients, servers, client_code, server_code, client_err, server_err):
        detail = f'client {clients} ({client_code}) {client_err}\nserver {servers} ({server_code}) {server_err}'
        expect(client_code == 0 and server_code == 0, f'an end failed: {detail}')
        connections = 2 if self.resume else 1
        expect(len(clients) == connections and len(servers) == connections, f'other connections: {detail}')
        for number in range(connections):
            self.check_connection(number, clients[number], servers[number], detail)

    def check_connection(self, number, client, server, detail):
        total = self.streams * self.size
        for name, report in (('client', client), ('server', server)):
            expect(report.get('established') == '1', f'{name} never established: {detail}')
            version = '2' if self.compat else str(3 - self.version if self.negotiate else self.version)
            expect(report.get('version') == version, f'{name} ran another version: {detail}')
            expect(report.get('verified') == '1', f'{name} read a wrong byte: {detail}')
            expect(report.get('streams') == str(self.streams), f'{name} counted other streams: {detail}')
            expect(report.get('bytes_received') == str(total), f'{name} read other bytes: {detail}')
            expect(report.get('bytes_sent') == str(total), f'{name} wrote other bytes: {detail}')
        closing, other = (client, server) if self.closer == 'client' else (server, client)
        expect(closing.get('close') == 'local', f'the closing end reports otherwise: {detail}')
        expect(closing.get('close_code') == str(self.code), f'the closing end names another code: {detail}')
        # A close is not retransmitted for ever (RFC 9000 section 10.2): on a lossy path the other end may idle out.
        if self.path is None or other.get('close') != 'idle':
            expect(other.get('close') == 'application', f'the other end read another close: {detail}')
            expect(other.get('close_code') == str(self.code), f'the other end read another code: {detail}')
        openssl = server if self.cerid_is_client else client
        group = openssl.get('group', '')
        expect(group.casefold() == OPENSSL_GROUPS[self.group].casefold(), f'OpenSSL negotiated another group: {detail}')
        expect(openssl.get('cipher') == OPENSSL_CIPHERS[self.cipher], f'OpenSSL negotiated another suite: {detail}')
        expect(openssl.get('alpn') == 'cerid-interop', f'OpenSSL negotiated another protocol: {detail}')
        if not self.cerid_is_client:
            expect(client.get('pin') == '1', f'ngtcp2 read another certificate key: {detail}')
        if self.updater:
            expect(int(openssl.get('key_updates', '0')) >= 2, f'too few key updates: {detail}')
        self.check_early(number, client, server, detail)
        if self.datagrams:
            self.check_datagrams(client, server, detail)

    def check_early(self, number, client, server, detail):
        resumed = '1' if number == 1 else '0'
        expect(client.get('resumed') == resumed and server.get('resumed') == resumed, f'resumption differs: {detail}')
        accepted = number == 1 and self.resume == 'early'
        rejected = number == 1 and self.resume == 'reject'
        expect(client.get('early') == ('accepted' if accepted else 'rejected' if rejected else 'none'),
               f'the client reports other early data: {detail}')
        if accepted:
            expect(server.get('early') == 'accepted', f'the server reports other early data: {detail}')
        # What is left of the flight after the datagrams may not reach a stream, so the two are counted together.
        early = int(server.get('early_bytes', '0')) + int(server.get('early_datagrams', '0'))
        expect(early > 0 if accepted else early == 0, f'the server read other 0-RTT data: {detail}')

    def check_datagrams(self, client, server, detail):
        sent = int(client.get('datagrams_sent', '0'))
        expect(sent == self.datagrams, f'the client sent other datagrams: {detail}')
        # A datagram may be lost even on the loopback, when a socket's buffer is full; the relay loses more.
        share = 0.5 if self.path else 0.9
        for name, report in (('client', client), ('server', server)):
            expect(report.get('datagrams_verified') == '1', f'{name} read a wrong datagram: {detail}')
            received = int(report.get('datagrams_received', '0'))
            expect(self.datagrams * share <= received <= self.datagrams, f'{name} read too few datagrams: {detail}')
            if self.datagram_limit:
                expect(report.get('datagrams_refused') == '1', f'{name} sent past the limit: {detail}')
        # The close may overtake the last acknowledgements, but not half of them. On the relay a reordered flight is
        # declared lost before its acknowledgement comes, and a datagram's outcome is reported once.
        if self.path is None:
            expect(int(client.get('datagrams_acked', '0')) >= self.datagrams / 2, f'too few acknowledged: {detail}')


def cases(cerid, ngtcp2):
    for cerid_is_client in (True, False):
        def case(**settings):
            return Case(cerid, ngtcp2, cerid_is_client, **settings)

        for version in (1, 2):
            for closer, code in (('client', 0), ('server', 0x2A)):
                yield case(version=version, closer=closer, code=code)
        for group in OPENSSL_GROUPS:
            yield case(group=group)
        for cipher in OPENSSL_CIPHERS:
            yield case(cipher=cipher)
        for version in (1, 2):
            yield case(version=version, size=8 << 20)
            yield case(version=version, streams=48, size=64 << 10, max_streams=8)
            yield case(version=version, retry=True, streams=4, size=64 << 10)
            for updater in ('cerid', 'ngtcp2'):
                yield case(version=version, updater=updater, size=4 << 20)
            for seed in (1, 2, 3):
                yield case(version=version, streams=4, size=256 << 10,
                           path=dict(seed=seed, loss=0.05, duplicate=0.03, delay=0.003, jitter=0.012))
        yield case(compat=True)
        yield case(compat=True, streams=8, size=64 << 10)
        for version in (1, 2):
            yield case(version=version, negotiate=True, streams=2, size=64 << 10)
            yield case(version=version, datagrams=256, streams=0)
            yield case(version=version, datagrams=256, streams=4, size=256 << 10)
            yield case(version=version, datagrams=64, datagram_bytes=597, datagram_limit=600, streams=0)
            for seed in (1, 2):
                yield case(version=version, datagrams=256, streams=2, size=64 << 10,
                           path=dict(seed=seed, loss=0.05, duplicate=0.03, delay=0.003, jitter=0.012))
            for resume in ('plain', 'early', 'reject'):
                yield case(version=version, resume=resume, streams=4, size=16 << 10)
            yield case(version=version, resume='early', streams=2, size=16 << 10, datagrams=32)
            yield case(version=version, reset=True, size=RESET_STREAM_BYTES)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--ngtcp2', required=True)
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    all_cases = [case for case in cases(arguments.cerid, arguments.ngtcp2) if arguments.only in case.describe()]
    failures = 0
    for index, case in enumerate(all_cases):
        try:
            case.run_reset() if case.reset else case.run()
            print(f'[{index + 1}/{len(all_cases)}] ok: {case.describe()}', flush=True)
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(f'[{index + 1}/{len(all_cases)}] FAILED: {case.describe()}\n{error}', flush=True)
    print(f'{len(all_cases) - failures} of {len(all_cases)} cases passed', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
