#!/usr/bin/env python3
"""HTTP/3 interoperability of crd-net with two other implementations (4.2a).

Runs crd-quic-peer with --http3 1 (Http3Connection over crd-net's QUIC) against crd-ngtcp2-quic-peer with --http3 1
(nghttp3 over ngtcp2) or crd-quiche-peer (Cloudflare's quiche over BoringSSL), each in its own process over real UDP
sockets on the loopback, both ways round. The exchange is http_exchange.h's: every field and every body byte is checked
by both ends, and each case holds the two ends' reports to each other.

Cases: requests with no body, a small body, bodies far over the stream and connection windows, more requests than the
stream limit lets open at once; QUIC versions 1 and 2; QPACK's dynamic table on, off, small enough to evict, with blocked
streams allowed and not, counted off Cerid's table (what nghttp3 inserted, what Cerid inserted and nghttp3 acknowledged);
large field sections and one over the peer's limit; trailers; 103 before the responses; Extended CONNECT tunnels with and
without HTTP datagrams; GOAWAY from a server in two steps and rejecting requests, and from a client; each end cancelling
a request; 0-RTT resumption accepted and refused; and a lossy, reordering, duplicating path.

quiche speaks QUIC version 1 only and its QPACK has no dynamic table (it advertises none and Cerid must never insert), so
those cases are not asked of it.
"""
import argparse
import subprocess
import sys
import threading

from interop_common import Relay, collect, expect, reports_of

SEED = '11' * 32
RUN_SECONDS = 70
CLOSE_CODE = 0x100
CANCEL_CODE = 0x10C
LOSSY = dict(loss=0.04, duplicate=0.03, delay=0.003, jitter=0.012)


def brief(reports):
    """What a failure shows of the reports: the connection's end and what the exchange counted."""
    shown = ('established', 'close', 'close_code', 'early', 'resumed')
    return [{name: value for name, value in report.items()
             if name in shown or (name.startswith(('h3_', 'datagrams_')) and value not in ('0', '1'))}
            for report in reports]


class Case:
    def __init__(self, cerid, peer, peer_name, cerid_is_client, version=1, requests=1, request_bytes=0, response_bytes=4096,
                 tunnels=0, table=4096, blocked=16, field_bytes=0, field_limit=16384, trailers=False, interim=False,
                 goaway=None, cancel=None, over_limit=False, datagrams=0, datagram_bytes=900, resume=None, path=None,
                 max_streams=64):
        self.cerid = cerid
        self.peer = peer
        self.peer_name = peer_name
        self.cerid_is_client = cerid_is_client
        self.version = version
        self.requests = requests
        self.request_bytes = request_bytes
        self.response_bytes = response_bytes
        self.tunnels = tunnels
        self.table = table
        self.blocked = blocked
        self.field_bytes = field_bytes
        self.field_limit = field_limit
        self.trailers = trailers
        self.interim = interim
        self.goaway = goaway
        self.cancel = cancel
        self.over_limit = over_limit
        self.datagrams = datagrams
        self.datagram_bytes = datagram_bytes
        self.resume = resume  # 'early' or 'reject'
        self.path = path
        self.max_streams = max_streams

    def command(self, program, role, port):
        command = [program, '--role', role, '--port', str(port), '--seed', SEED, '--group', self.group(program),
                   '--cipher', self.cipher(program), '--version', str(self.version), '--streams', '0', '--bytes', '0',
                   '--closer', 'client', '--code', str(CLOSE_CODE), '--max-streams', str(self.max_streams),
                   '--http3', '1', '--requests', str(self.requests), '--request-bytes', str(self.request_bytes),
                   '--response-bytes', str(self.response_bytes), '--tunnels', str(self.tunnels),
                   '--table', str(self.table), '--blocked', str(self.blocked), '--field-bytes', str(self.field_bytes),
                   '--field-limit', str(self.field_limit), '--trailers', str(int(self.trailers)),
                   '--interim', str(int(self.interim)), '--over-limit', str(int(self.over_limit))]
        if self.goaway:
            command += ['--goaway', self.goaway]
        if self.cancel:
            command += ['--cancel', self.cancel]
        if self.datagrams:
            command += ['--datagrams', str(self.datagrams), '--datagram-bytes', str(self.datagram_bytes)]
        if self.resume:
            command += ['--connections', '2', '--early', '1']
            if self.resume == 'reject':
                command += ['--reject-early', '1']
        return command

    def group(self, program):
        return 'x25519mlkem768' if program == self.cerid else 'X25519MLKEM768'

    def cipher(self, program):
        return 'aes128' if program == self.cerid else 'TLS_AES_128_GCM_SHA256'

    def describe(self):
        words = ['cerid client' if self.cerid_is_client else f'{self.peer_name} client', f'v{self.version}',
                 f'{self.requests}x{self.request_bytes}/{self.response_bytes}', f'table {self.table}/{self.blocked}']
        for name, value in (('tunnels', self.tunnels), ('fields', self.field_bytes), ('trailers', self.trailers),
                            ('interim', self.interim), ('goaway', self.goaway), ('cancel', self.cancel),
                            ('over-limit', self.over_limit), ('datagrams', self.datagrams), ('resume', self.resume),
                            ('path', self.path), ('max-streams', self.max_streams != 64 and self.max_streams)):
            if value:
                words.append(f'{name} {value}')
        return ' '.join(words)

    def run(self):
        server_program, client_program = self.peer, self.cerid
        if not self.cerid_is_client:
            server_program, client_program = client_program, server_program
        pipes = dict(stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        server = subprocess.Popen(self.command(server_program, 'server', 0), **pipes)
        client = None
        relay = None
        try:
            first = server.stdout.readline().strip()
            if not first.startswith('port='):
                raise AssertionError(f'the server printed {first!r}; {server.stderr.read()}')
            port = int(first[len('port='):])
            if self.path is not None:
                relay = Relay(port, **self.path)
                port = relay.port
            client = subprocess.Popen(self.command(client_program, 'client', port), **pipes)
            server_out = []
            thread = threading.Thread(target=collect, args=(server, server_out), daemon=True)
            thread.start()
            client_out, client_err = client.communicate(timeout=RUN_SECONDS)
            thread.join(RUN_SECONDS)
            expect(not thread.is_alive(), f'the server did not end; the client reported {client_out}')
            server.wait(10)
            self.check(reports_of(client_out), reports_of(server_out[0]), client.returncode, server.returncode,
                       client_err, server.stderr.read())
        finally:
            if relay is not None:
                relay.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check(self, clients, servers, client_code, server_code, client_err, server_err):
        detail = f'client {brief(clients)} ({client_code}) {client_err}\n' \
                 f'server {brief(servers)} ({server_code}) {server_err}'
        expect(client_code == 0 and server_code == 0, f'an end failed: {detail}')
        connections = 2 if self.resume else 1
        expect(len(clients) == connections and len(servers) == connections, f'other connections: {detail}')
        for number in range(connections):
            self.check_connection(number, clients[number], servers[number], detail)

    def check_connection(self, number, client, server, detail):
        cerid_report, peer_report = (client, server) if self.cerid_is_client else (server, client)
        for name, report in (('client', client), ('server', server)):
            table, blocked = self.table, self.blocked
            if self.peer_name == 'quiche' and report is cerid_report:
                table, blocked = 0, 0
            expect(report.get('established') == '1', f'{name} never established: {detail}')
            expect(report.get('version') == str(self.version), f'{name} ran another version: {detail}')
            expect(report.get('h3_verified') == '1', f'{name} read a wrong field or byte: {detail}')
            expect(report.get('h3_peer_known') == '1', f'{name} never read SETTINGS: {detail}')
            expect(report.get('h3_peer_table') == str(table), f'{name} read another table: {detail}')
            expect(report.get('h3_peer_blocked') == str(blocked), f'{name} read another blocked limit: {detail}')
            expect(report.get('h3_peer_field_limit') == str(self.field_limit), f'{name} read another limit: {detail}')
            expect(report.get('h3_peer_datagram') == str(int(self.datagrams != 0)), f'{name} read otherwise: {detail}')
        expect(client.get('h3_peer_connect') == '1', f'the client read no Extended CONNECT: {detail}')
        expect(peer_report.get('alpn') == 'h3', f'the peer negotiated another protocol: {detail}')
        expect(client.get('close') == 'local' and client.get('close_code') == str(CLOSE_CODE),
               f'the client closed otherwise: {detail}')
        if self.path is None or server.get('close') != 'idle':
            expect(server.get('close') == 'application' and server.get('close_code') == str(CLOSE_CODE),
                   f'the server read another close: {detail}')
        self.check_requests(client, server, detail)
        self.check_tables(cerid_report, detail)
        self.check_datagrams(client, server, detail)
        self.check_early(number, client, server, detail)

    def number(self, report, name):
        return int(report.get(name, '-1'))

    def check_requests(self, client, server, detail):
        requests = self.requests
        cancelled = self.cancel is not None
        expected = requests - (1 if cancelled else 0)
        answered = requests - (1 if self.over_limit else 0)
        if self.goaway != 'reject':
            expect(self.number(client, 'h3_completed') == expected - (1 if self.over_limit and self.cerid_is_client
                                                                     else 0),
                   f'the client completed other requests: {detail}')
            expect(self.number(server, 'h3_seen') == answered, f'the server saw other requests: {detail}')
            if not cancelled:
                plain = answered * self.request_bytes
                expect(self.number(server, 'h3_body_received') == plain, f'the server read other bytes: {detail}')
                plain = answered * self.response_bytes
                expect(self.number(client, 'h3_body_received') == plain, f'the client read other bytes: {detail}')
                if self.trailers:
                    messages = answered - self.tunnels
                    expect(self.number(client, 'h3_trailers') == messages, f'the client read no trailers: {detail}')
                    expect(self.number(server, 'h3_trailers') == messages, f'the server read no trailers: {detail}')
        if self.interim:
            expect(self.number(client, 'h3_interims') >= expected, f'the client read too few interims: {detail}')
        expect(self.number(client, 'h3_tunnels') == self.tunnels, f'the client opened other tunnels: {detail}')
        expect(self.number(server, 'h3_tunnels') == self.tunnels, f'the server served other tunnels: {detail}')
        if self.over_limit:
            name = 'h3_refused_too_large' if self.cerid_is_client else 'h3_status_431'
            expect(self.number(client, name) == 1, f'the client was not refused: {detail}')
        if self.cancel:
            canceller, cancelled_end = (client, server) if self.cancel == 'client' else (server, client)
            expect(self.number(canceller, 'h3_cancelled') == 1, f'the cancel was not made: {detail}')
            expect(self.number(cancelled_end, 'h3_aborted') == 1, f'the cancel was not seen: {detail}')
            expect(self.number(cancelled_end, 'h3_abort_code') == CANCEL_CODE, f'another code: {detail}')
        self.check_goaway(client, server, detail)

    def check_goaway(self, client, server, detail):
        if self.goaway == 'notice':
            expect(self.number(client, 'h3_goaway_seen') == 2, f'the client read other GOAWAYs: {detail}')
            expect(self.number(client, 'h3_goaway_id') == self.requests * 4, f'the last GOAWAY names another: {detail}')
            expect(self.number(client, 'h3_refused_after_goaway') == 1, f'the client could still open: {detail}')
        elif self.goaway == 'reject':
            completed = self.number(client, 'h3_completed')
            rejected = self.number(client, 'h3_rejected')
            expect(self.number(client, 'h3_goaway_seen') == 1, f'the client read other GOAWAYs: {detail}')
            expect(completed + rejected == self.requests, f'requests went unaccounted: {detail}')
            expect(completed >= 1 and completed * 4 == self.number(client, 'h3_goaway_id'),
                   f'the GOAWAY named other requests than were answered: {detail}')
        elif self.goaway == 'client':
            expect(self.number(server, 'h3_goaway_seen') == 2, f'the server read other GOAWAYs: {detail}')
            expect(self.number(server, 'h3_goaway_id') == 0, f'the last GOAWAY names another: {detail}')

    def check_tables(self, cerid, detail):
        """nghttp3 and Cerid each insert a field they have seen, and Cerid's first use asks a second sight."""
        if self.table == 0 or self.peer_name == 'quiche':
            expect(self.number(cerid, 'h3_qpack_inserts_received') == 0, f'the peer inserted into nothing: {detail}')
            expect(self.number(cerid, 'h3_qpack_inserts_sent') == 0, f'Cerid inserted into nothing: {detail}')
        elif self.requests >= 4 and self.goaway != 'reject':
            expect(self.number(cerid, 'h3_qpack_inserts_received') > 0, f'nghttp3 inserted nothing: {detail}')
            expect(self.number(cerid, 'h3_qpack_inserts_sent') > 0, f'Cerid inserted nothing: {detail}')
            if self.path is None:
                expect(self.number(cerid, 'h3_qpack_acknowledged') > 0, f'nghttp3 acknowledged nothing: {detail}')

    def check_datagrams(self, client, server, detail):
        if not self.datagrams:
            return
        share = 0.5 if self.path else 0.9
        for name, report in (('client', client), ('server', server)):
            received = int(report.get('h3_datagrams_received', report.get('datagrams_received', '0')))
            verified = report.get('h3_datagrams_verified', report.get('datagrams_verified'))
            expect(verified == '1', f'{name} read a wrong datagram: {detail}')
            expect(self.datagrams * share <= received <= self.datagrams, f'{name} read too few datagrams: {detail}')

    def check_early(self, number, client, server, detail):
        if not self.resume:
            return
        resumed = '1' if number == 1 else '0'
        expect(client.get('resumed') == resumed and server.get('resumed') == resumed, f'resumption differs: {detail}')
        accepted = number == 1 and self.resume == 'early'
        rejected = number == 1 and self.resume == 'reject'
        expect(client.get('early') == ('accepted' if accepted else 'rejected' if rejected else 'none'),
               f'the client reports other early data: {detail}')
        early = self.number(server, 'h3_early_requests')
        expect(early == self.requests if accepted else early == 0, f'other requests in 0-RTT: {detail}')


def cases(cerid, peer, peer_name):
    quiche = peer_name == 'quiche'
    for cerid_is_client in (True, False):
        def case(**settings):
            return Case(cerid, peer, peer_name, cerid_is_client, **settings)

        for version in (1,) if quiche else (1, 2):
            yield case(version=version)
            yield case(version=version, requests=8, request_bytes=4096, response_bytes=8192)
            yield case(version=version, requests=2, request_bytes=4 << 20, response_bytes=8 << 20)
            yield case(version=version, requests=256, request_bytes=512, response_bytes=2048, max_streams=8)
        yield case(requests=32, response_bytes=256, table=0, blocked=0)
        yield case(requests=32, response_bytes=256, table=4096, blocked=0)
        if not quiche:
            yield case(requests=32, response_bytes=256, table=256, blocked=8)
            yield case(requests=64, response_bytes=256, table=65536, blocked=64)
        yield case(requests=8, field_bytes=12000)
        yield case(requests=4, over_limit=True, field_limit=8192)
        yield case(requests=8, request_bytes=2048, trailers=True)
        yield case(requests=8, response_bytes=0, request_bytes=0, trailers=True)
        yield case(requests=8, interim=True, response_bytes=2048)
        yield case(requests=4, tunnels=4, request_bytes=64 << 10, response_bytes=128 << 10)
        yield case(requests=4, tunnels=4, request_bytes=4096, response_bytes=4096, datagrams=64)
        yield case(requests=6, tunnels=2, request_bytes=1024, response_bytes=1024, datagrams=32, trailers=True,
                   interim=True)
        yield case(requests=8, goaway='notice')
        yield case(requests=8, goaway='reject')
        yield case(requests=8, goaway='client')
        yield case(requests=4, cancel='client', response_bytes=1 << 20)
        yield case(requests=4, cancel='server')
        for resume in ('early', 'reject'):
            yield case(requests=2, request_bytes=2048, resume=resume)
        for seed in (1, 2, 3):
            yield case(requests=16, request_bytes=64 << 10, response_bytes=128 << 10, path=dict(seed=seed, **LOSSY))
        for seed in (4, 5):
            yield case(requests=48, request_bytes=256, response_bytes=256, table=8192, blocked=48,
                       path=dict(seed=seed, **LOSSY))
        yield case(requests=4, tunnels=4, datagrams=64, path=dict(seed=6, **LOSSY))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True)
    peer = parser.add_mutually_exclusive_group(required=True)
    peer.add_argument('--nghttp3')
    peer.add_argument('--quiche')
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    peer_name = 'quiche' if arguments.quiche else 'nghttp3'
    all_cases = [case for case in cases(arguments.cerid, arguments.quiche or arguments.nghttp3, peer_name)
                 if arguments.only in case.describe()]
    failures = 0
    for index, case in enumerate(all_cases):
        try:
            case.run()
            print(f'[{index + 1}/{len(all_cases)}] ok: {case.describe()}', flush=True)
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(f'[{index + 1}/{len(all_cases)}] FAILED: {case.describe()}\n{error}', flush=True)
    print(f'{len(all_cases) - failures} of {len(all_cases)} cases passed', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
