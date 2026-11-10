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

ECN (RFC 9000 section 13.4) is held in every case: each end marks ECT(0), counts the codepoints it reads and validates
its path from the other's ACK frames. ngtcp2 fails a path on any ECT(1) count, a count above what it sent or one short
of what was acknowledged, so its verdict holds Cerid's ACK_ECN counts to what Cerid read. On the loopback both ends
validate; the relay bleaches every datagram, and both fail; a relay that marks every datagram CE leaves both capable and
reacting to congestion; one that re-marks ECT(1) fails both.

Path MTU discovery (RFC 8899, RFC 9000 section 14.3) is held in every case: neither end sends a datagram over the
other's max_udp_payload_size, which Cerid sets to its loop's receive slot. Over a relay whose links carry at most `mtu`
bytes, Cerid's search finds that size to the byte and ngtcp2's settles on the largest of its four fixed probe sizes the
path carries; a Cerid end with a smaller receive slot holds ngtcp2's probes under it; a path toward the peer that falls
in the middle of a transfer, which nothing reports, is found by Cerid's black hole detection and searched again; and
over a lossy path both still find a size the path carries.

Congestion control: each end runs the controller a case names (CUBIC by default; ngtcp2's own BBR, Reno or CUBIC on
its side), and Cerid reports its own. Cerid's BBRv3 runs on the loopback, over the impaired relays above, and through
a relay that is a bottleneck each way (a rate, a drop-tail buffer of two BDPs and a propagation delay): its model's
minimum round trip matches the path's, it keeps the queue short and the buffer from overflowing, and a Cerid client,
which no echo holds back, finds the rate and keeps the link busy, against ngtcp2's CUBIC, Reno and BBR, with and
without random loss under BBR's threshold (where CUBIC uses a quarter of the link).
"""
import argparse
import subprocess
import sys
import threading

from interop_common import (CERID_RECEIVE, QUIC_MIN, Relay, check_cerid_pmtu, check_relay_sizes, collect,
                            drops, expect, path_toward, reports_of)

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
PMTU_SEARCH_BYTES = 4 << 20  # a transfer long enough for both searches to end before it does
ECN_TESTING_PACKETS = 10  # NGTCP2_ECN_MAX_NUM_VALIDATION_PKTS: ngtcp2 marks no more until its path is validated
ECN_VERDICT_BYTES = 64 << 10  # enough each way for ngtcp2 to test, validate and go on marking
NGTCP2_MAX_TX = 1452  # ngtcp2_settings_default's max_tx_udp_payload_size, which caps its probes
NGTCP2_ADVERTISED = 65527  # ngtcp2_transport_params_default's max_udp_payload_size, which the peer keeps
NGTCP2_PROBES = (1406, 1342, 1232, 1444)  # ngtcp2_pmtud.c's default probes, in the order it sends them
BOTTLENECK_RATE = 2_500_000  # bytes per second each way: 20 Mbit/s
BOTTLENECK_DELAY = 0.015  # seconds each way, a 30 ms round trip
BOTTLENECK_BDP = int(BOTTLENECK_RATE * 2 * BOTTLENECK_DELAY)
BOTTLENECK = dict(rate=BOTTLENECK_RATE, buffer=2 * BOTTLENECK_BDP, delay=BOTTLENECK_DELAY)


class Case:
    def __init__(self, cerid, ngtcp2, cerid_is_client, version=1, group='x25519mlkem768', cipher='aes128', streams=1,
                 size=4096, closer='client', code=0, max_streams=16, compat=False, retry=False, updater=None,
                 updates=64, path=None, datagrams=0, datagram_bytes=1000, datagram_limit=None, resume=None,
                 negotiate=False, reset=False, receive_bytes=None, congestion=None, peer_cc=None):
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
        self.receive_bytes = receive_bytes  # Cerid's receive slot, CERID_RECEIVE when None
        self.congestion = congestion  # Cerid's controller: 'newreno', 'cubic' or 'bbr'; CUBIC when None
        self.peer_cc = peer_cc  # ngtcp2's: 'reno', 'cubic' or 'bbr'; CUBIC when None

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
        if cerid and self.receive_bytes:
            command += ['--receive-bytes', str(self.receive_bytes)]
        if cerid and self.congestion:
            command += ['--congestion', self.congestion]
        if not cerid and self.peer_cc:
            command += ['--cc', self.peer_cc]
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
            (' stateless reset' if self.reset else '') + \
            (f' receive {self.receive_bytes}' if self.receive_bytes else '') + \
            (f' cerid {self.congestion}' if self.congestion else '') + \
            (f' ngtcp2 {self.peer_cc}' if self.peer_cc else '')
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
            clients, servers = reports_of(client_out), reports_of(server_out[0])
            self.check(clients, servers, client.returncode, server.returncode, client_err, server_err)
            if relay is not None:
                check_relay_sizes(relay, self.path, 'client' if self.cerid_is_client else 'server',
                                  self.receive_bytes or CERID_RECEIVE, f'client {clients}\nserver {servers}')
                self.check_bottleneck(relay, clients[0] if self.cerid_is_client else servers[0])
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
        self.check_ecn(client, server, detail)
        self.check_pmtu(client, server, detail)
        self.check_congestion(client if self.cerid_is_client else server, detail)

    def check_congestion(self, cerid, detail):
        """Cerid ran the controller the case names; BBR's model holds a bandwidth and a round trip, a bottleneck's
        within reach of its rate and its propagation delay, and a window controller has none."""
        algorithm = self.congestion or 'cubic'
        expect(cerid.get('congestion') == algorithm, f'Cerid ran another controller: {detail}')
        bandwidth, min_rtt = int(cerid.get('bandwidth', '-1')), int(cerid.get('min_rtt_ns', '-1'))
        if algorithm != 'bbr':
            expect(bandwidth == 0 and min_rtt == 0, f'a window controller reported a model: {detail}')
            return
        expect(bandwidth > 0 and min_rtt > 0, f'BBR measured no bandwidth or round trip: {detail}')
        rate = (self.path or {}).get('rate')
        if rate is None:
            return
        # The model's rate is the bottleneck's, measured by a peer's ACK frames over a real socket and a relay
        # scheduled by Python's clock; random loss under BBR's 2% threshold leaves it there. A relay thread that falls
        # behind hands datagrams on in bursts, and ACK frames so compressed let a sample read the sending rate, up to
        # ProbeBW_UP's 1.25 times the model's (draft-ietf-ccwg-bbr-07 section 4.1.2.3; 1.26 times the link was seen).
        # A server's echo sends what it reads, so behind a client that loss holds back its model may hold no more
        # than the client's rate (0.31 times the link was seen behind CUBIC under 1% loss).
        floor = 0.6 * rate if self.cerid_is_client else 0
        expect(floor <= bandwidth <= rate * 1.5, f'BBR found {bandwidth} B/s through {rate} B/s: {detail}')
        base = int(2 * self.path['delay'] * 1e9)
        expect(base <= min_rtt <= base + 10_000_000, f'BBR found a {min_rtt} ns round trip over {base} ns: {detail}')

    def check_bottleneck(self, relay, cerid):
        """Through a bottleneck, Cerid's BBR overflows the buffer toward its peer at most in Startup's overshoot and
        keeps the standing queue under the round trip, which its window of two BDPs bounds even when a sample
        overestimates the rate (seen 5% over through the Python relay). A Cerid client, whose sending no echo holds
        back, keeps the link busy, random loss included, where CUBIC uses a quarter of it. ngtcp2's direction is
        reported."""
        if self.congestion != 'bbr' or relay.rate is None:
            return
        toward = 'server' if self.cerid_is_client else 'client'
        other = 'client' if self.cerid_is_client else 'server'
        entered, overflowed = relay.entered[toward], relay.tail_drops[toward]
        queue = relay.queued[toward] / max(1, entered)
        used = relay.utilisation(toward)
        detail = f'toward the {toward}: {entered} entered, {overflowed} tail drops, a {queue * 1e3:.1f} ms mean ' \
                 f'queue, {used:.2f} of the link; the other way {relay.tail_drops[other]} tail drops, ' \
                 f'{relay.utilisation(other):.2f} of the link; cerid {cerid}'
        expect(entered > 0 and overflowed <= 0.01 * (entered + overflowed), f'BBR overflowed the buffer: {detail}')
        expect(queue <= 2 * self.path['delay'], f'BBR kept more than a round trip queued: {detail}')
        if self.cerid_is_client:
            expect(used >= 0.6, f'BBR left the link idle: {detail}')

    def check_ecn(self, client, server, detail):
        cerid, ngtcp2 = (client, server) if self.cerid_is_client else (server, client)
        relayed = self.path is not None or self.retry or self.negotiate
        mark = (self.path or {}).get('mark')
        marked = int(ngtcp2.get('ecn_sent_ect0', '-1'))
        verdict = self.streams * self.size + self.datagrams * self.datagram_bytes >= ECN_VERDICT_BYTES
        if not relayed or mark == 'ce':
            expect(cerid.get('ecn') == 'capable', f'Cerid did not validate ECN: {detail}')
            read = 'ecn_received_ce' if relayed else 'ecn_received_ect0'
            expect(int(ngtcp2.get(read, '0')) > 0, f'ngtcp2 read no {read[13:]} datagram: {detail}')
            if verdict:
                expect(marked > ECN_TESTING_PACKETS and ngtcp2.get('ecn_last') == 'ect0',
                       f'ngtcp2 did not validate ECN: {detail}')
        else:
            failure = 'exceeded' if mark == 'ect1' else 'missing'
            expect(cerid.get('ecn') == 'failed' and cerid.get('ecn_failure') == failure,
                   f'Cerid did not fail ECN validation as {failure}: {detail}')
            # A Retry or Version Negotiation starts a client's testing again (a new connection, for the latter).
            tests = 2 if self.retry or self.negotiate else 1
            expect(0 <= marked <= tests * ECN_TESTING_PACKETS, f'ngtcp2 marked past its testing: {detail}')
            if verdict:
                expect(ngtcp2.get('ecn_last') == 'not-ect', f'ngtcp2 did not fail ECN validation: {detail}')
            if mark == 'ect1':
                expect(int(ngtcp2.get('ecn_received_ect1', '0')) > 0, f'ngtcp2 read no ECT(1) datagram: {detail}')
            else:
                expect(ngtcp2.get('ecn_received_ect0') == '0', f'ngtcp2 read through a bleaching path: {detail}')
        if mark == 'ce':
            # Every datagram CE: each end takes the counts as congestion (RFC 9002 section 7.1, B.7).
            expect(int(cerid.get('ecn_ce', '0')) > 0, f'ngtcp2 reported no CE: {detail}')
            expect(int(cerid.get('reductions', '0')) > 0, f'Cerid did not reduce its window on CE: {detail}')
            expect(ngtcp2.get('ssthresh_set') == '1', f'ngtcp2 did not react to CE: {detail}')

    def check_pmtu(self, client, server, detail):
        cerid, ngtcp2 = (client, server) if self.cerid_is_client else (server, client)
        settled = self.datagrams == 0 and self.streams * self.size >= PMTU_SEARCH_BYTES
        check_cerid_pmtu(cerid, self.path, 'server' if self.cerid_is_client else 'client', settled, detail,
                         NGTCP2_ADVERTISED)
        receive = self.receive_bytes or CERID_RECEIVE
        expect(ngtcp2.get('peer_max_udp_payload') == str(receive),
               f'ngtcp2 read another max_udp_payload_size: {detail}')
        toward_cerid = path_toward(self.path, 'client' if self.cerid_is_client else 'server')
        expected = self.ngtcp2_reach(min(toward_cerid or receive, receive), min(receive, NGTCP2_MAX_TX))
        reached = int(ngtcp2.get('pmtu', '0'))
        if settled and not drops(self.path):
            expect(reached == expected, f'ngtcp2 reached {reached}, not {expected}: {detail}')
        else:
            expect(QUIC_MIN <= reached <= expected, f'ngtcp2 reached {reached}, more than the path carries: {detail}')

    @staticmethod
    def ngtcp2_reach(path, hard_max):
        """The size ngtcp2's search ends at (ngtcp2_pmtud.c): its probes in order, each above the size reached, under
        its hard maximum and under the smallest that failed."""
        reached, failed = QUIC_MIN, None
        for probe in NGTCP2_PROBES:
            if probe <= reached or probe > hard_max or (failed is not None and probe >= failed):
                continue
            if probe <= path:
                reached = probe
            else:
                failed = probe if failed is None else min(failed, probe)
        return reached

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
            yield case(version=version, streams=4, size=256 << 10, path=dict(mark='ce'))
            yield case(version=version, streams=4, size=256 << 10,
                       path=dict(seed=version, loss=0.02, delay=0.003, jitter=0.012, mark='ce'))
            yield case(version=version, streams=4, size=256 << 10, path=dict(mark='ect1'))
        for version in (1, 2):
            for mtu in (1350, 1280):
                yield case(version=version, streams=4, size=4 << 20, path=dict(mtu=mtu))
        yield case(streams=4, size=4 << 20, receive_bytes=1300)
        yield case(streams=4, size=4 << 20, path=dict(mtu=1300), receive_bytes=1250)
        toward_peer = 'server' if cerid_is_client else 'client'
        for fall in (1300, 1252):
            yield case(streams=4, size=4 << 20, path=dict(fall=(toward_peer, 3000, fall)))
        for seed in (1, 2):
            yield case(streams=4, size=1 << 20,
                       path=dict(seed=seed, loss=0.02, duplicate=0.02, delay=0.003, jitter=0.006, mtu=1350))
        yield case(congestion='newreno', size=4 << 20)
        yield case(congestion='bbr', size=8 << 20)
        for version in (1, 2):
            yield case(version=version, congestion='bbr', streams=4, size=1 << 20, path=BOTTLENECK)
        yield case(congestion='bbr', peer_cc='bbr', streams=4, size=1 << 20, path=BOTTLENECK)
        yield case(congestion='bbr', peer_cc='reno', streams=4, size=1 << 20, path=BOTTLENECK)
        yield case(congestion='bbr', streams=4, size=1 << 20, path=dict(BOTTLENECK, seed=1, loss=0.01))
        for seed in (1, 2):
            yield case(congestion='bbr', streams=4, size=256 << 10,
                       path=dict(seed=seed, loss=0.05, duplicate=0.03, delay=0.003, jitter=0.012))
        yield case(congestion='bbr', streams=4, size=256 << 10, path=dict(mark='ce'))
        yield case(congestion='bbr', streams=4, size=4 << 20, path=dict(mtu=1350))
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
