#!/usr/bin/env python3
"""WebTransport interoperability of crd-net with another implementation (4.2a).

Runs crd-quic-peer with --webtransport (crd-net's sessions over its own HTTP/3 and QUIC) against
crd-webtransport-quinn-peer (web-transport-quinn over Quinn and rustls) and crd-webtransport-go-peer (webtransport-go
over quic-go), each in its own process over real UDP sockets on the loopback, both ways round, and against itself in
both revisions over the same sockets. interop_webtransport.hpp has the exchange: both ends check every byte, and each
case holds the two reports to each other.

Cases: streams with no body, small and far over the windows; unidirectional streams; datagrams; resets and STOP_SENDING
with application codes; protocol negotiation; Origin read and refused; a refused session; WT_CLOSE_SESSION from either
end; a lossy, reordering, duplicating path. On `webtransport-h3`, also flow control under a tight window. Over a
relay whose links carry at most 1350 bytes each Cerid end's path MTU search finds that size to the byte, and over one
that falls toward the peer in the middle of the transfer it finds the black hole and the size below; no peer sends a
datagram over Cerid's max_udp_payload_size.

Not asked of web-transport-quinn, and why: flow control and the `webtransport-h3` spelling (it speaks only the legacy
one), RESET_STREAM_AT (Quinn 0.11 has none), the session exporter (it offers none), and 0-RTT, which never opens a
session. Of webtransport-go: the legacy spelling (its client never sends it, and its server does not offer it in the
setting Cerid reads), and the same two last items.
"""
import argparse
import subprocess
import sys
import threading

from interop_common import CERID_RECEIVE, Relay, check_cerid_pmtu, check_relay_sizes, collect, expect, reports_of

SEED = '11' * 32
RUN_SECONDS = 90
CLOSE_CODE = 0x100
WT_CLOSE_CODE = 0x0C1D05E
WT_CLOSE_REASON = 'cerid-interop done'
LOSSY = dict(seed=5, loss=0.04, duplicate=0.03, delay=0.003, jitter=0.012)
GOOD_ORIGIN = 'https://good.test'
BAD_ORIGIN = 'https://bad.test'
CHOSEN = 'cerid-b'
GO_DATAGRAMS = 32
PMTU_SEARCH_BYTES = 4 << 20  # a transfer long enough for the search to end before it does


class Case:
    def __init__(self, cerid, peer, peer_name, cerid_is_client, revision='legacy', streams=0, stream_bytes=0, uni=0,
                 uni_bytes=0, resets=0, stops=0, datagrams=0, datagram_bytes=900, closer='client', protocols=False,
                 origin=None, allow=None, reject=0, refused=None, path=None, window=0):
        self.cerid = cerid
        self.peer = peer
        self.peer_name = peer_name
        self.cerid_is_client = cerid_is_client
        self.revision = revision
        self.streams = streams
        self.stream_bytes = stream_bytes
        self.uni = uni
        self.uni_bytes = uni_bytes
        self.resets = resets
        self.stops = stops
        self.datagrams = datagrams
        self.datagram_bytes = datagram_bytes
        self.closer = closer
        self.protocols = protocols
        self.origin = origin
        self.allow = allow
        self.reject = reject
        self.refused = refused
        self.path = path
        self.window = window

    def command(self, program, role, port):
        command = [program, '--role', role, '--port', str(port), '--seed', SEED, '--group', 'x25519mlkem768',
                   '--cipher', 'aes128', '--version', '1', '--streams', str(self.streams), '--bytes',
                   str(self.stream_bytes),
                   '--closer', self.closer, '--code', str(CLOSE_CODE), '--max-streams',
                   str(self.streams + self.resets + self.stops + 8), '--uni', str(self.uni), '--uni-bytes',
                   str(self.uni_bytes), '--resets', str(self.resets), '--stops', str(self.stops), '--webtransport',
                   self.revision, '--datagrams', str(self.datagrams), '--datagram-bytes', str(self.datagram_bytes)]
        if self.protocols:
            command += ['--wt-protocols', '1']
        if self.window:
            command += ['--wt-window', str(self.window)]
        if role == 'client' and self.origin:
            command += ['--wt-origin', self.origin]
        if role == 'server' and self.reject:
            command += ['--wt-reject', str(self.reject)]
        if role == 'server' and self.allow and program == self.cerid:
            command += ['--wt-allow', self.allow]
        return command

    def describe(self):
        words = [f'with {self.peer_name}', 'cerid client' if self.cerid_is_client else 'peer client', self.revision,
                 f'{self.streams}x{self.stream_bytes}', f'closer {self.closer}']
        for name, value in (('uni', self.uni), ('resets', self.resets), ('stops', self.stops),
                            ('datagrams', self.datagrams), ('protocols', self.protocols), ('origin', self.origin),
                            ('reject', self.reject), ('refused', self.refused), ('path', self.path),
                            ('window', self.window)):
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
            clients, servers = reports_of(client_out), reports_of(server_out[0])
            self.check(clients, servers, client.returncode, server.returncode, client_err, server.stderr.read())
            if relay is not None:
                for toward_cerid in self.cerid_ends('client', 'server'):
                    check_relay_sizes(relay, self.path, toward_cerid, CERID_RECEIVE, f'{clients}\n{servers}')
        finally:
            if relay is not None:
                relay.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check(self, clients, servers, client_code, server_code, client_err, server_err):
        detail = f'client {clients} ({client_code}) {client_err}\nserver {servers} ({server_code}) {server_err}'
        expect(client_code == 0 and server_code == 0, f'an end failed: {detail}')
        expect(len(clients) == 1 and len(servers) == 1, f'other connections: {detail}')
        client, server = clients[0], servers[0]
        expect(client.get('established') == '1' and server.get('established') == '1', f'not established: {detail}')
        expect(client.get('wt_verified') == '1' and server.get('wt_verified') == '1', f'a wrong byte: {detail}')
        if self.reject or self.refused:
            self.check_refused(client, server, detail)
            return
        self.check_session(client, detail)
        self.check_session(server, detail)
        self.check_ecn(client, server, detail)
        settled = 2 * self.streams * self.stream_bytes + self.uni * self.uni_bytes >= PMTU_SEARCH_BYTES
        peer_receive = CERID_RECEIVE if self.peer_name == 'cerid' else None
        for report, toward_peer in self.cerid_ends((client, 'server'), (server, 'client')):
            check_cerid_pmtu(report, self.path, toward_peer, settled, detail, peer_receive)
        closing, other = (client, server) if self.closer == 'client' else (server, client)
        expect(other.get('wt_close_seen') == '1', f'the close was not read: {detail}')
        expect(other.get('wt_close_code') == str(WT_CLOSE_CODE), f'another close code: {detail}')
        expect(other.get('wt_close_reason') == WT_CLOSE_REASON, f'another close reason: {detail}')
        expect(closing.get('wt_close_seen') == '0', f'the closing end read a close: {detail}')
        expect(closing.get('wt_control_done') == '1', f'the closing end closed early: {detail}')
        if self.protocols:
            expect(client.get('wt_protocol') == CHOSEN and server.get('wt_protocol') == CHOSEN, f'protocol: {detail}')
        if self.origin:
            expect(server.get('wt_origin') == self.origin, f'the server read another Origin: {detail}')

    def check_session(self, report, detail):
        expect(report.get('wt_opened') == '1', f'no session: {detail}')
        expect(report.get('wt_echo') == str(2 * self.streams), f'streams: {detail}')
        expect(report.get('wt_uni') == str(2 * self.uni), f'unidirectional streams: {detail}')
        for name, count in (('wt_resets_sent', self.resets), ('wt_resets_seen', self.resets),
                            ('wt_stops_sent', self.stops), ('wt_stops_seen', self.stops)):
            expect(report.get(name) == str(count), f'{name}: {detail}')
        bytes_each = 2 * self.streams * self.stream_bytes + self.uni * self.uni_bytes
        expect(report.get('wt_bytes_received') == str(bytes_each), f'bytes received: {detail}')
        expect(report.get('wt_bytes_sent') == str(bytes_each), f'bytes sent: {detail}')
        expect(report.get('wt_datagrams_sent') == str(self.datagrams), f'datagrams sent: {detail}')
        received = int(report.get('wt_datagrams_received', '-1'))
        if self.path is None:
            expect(received == self.datagrams, f'datagrams lost on a clean path: {detail}')
        else:
            expect(0 <= received <= self.datagrams, f'datagrams: {detail}')

    def cerid_ends(self, client, server):
        """Of what stands for the client and for the server, those of the Cerid ends."""
        return [client, server] if self.peer_name == 'cerid' else [client if self.cerid_is_client else server]

    def check_ecn(self, client, server, detail):
        # Every peer here reports ECN counts on Linux (Quinn, quic-go, Cerid), so each Cerid end validates its path; the
        # driver's relay sends every datagram unmarked, a bleaching path, which fails validation (RFC 9000 13.4.2).
        for report in self.cerid_ends(client, server):
            if self.path is None:
                expect(report.get('ecn') == 'capable', f'ECN not validated: {detail}')
            else:
                expect(report.get('ecn') == 'failed' and report.get('ecn_failure') == 'missing',
                       f'a bleaching relay did not fail ECN validation: {detail}')

    def check_refused(self, client, server, detail):
        status = self.reject or self.refused
        expect(client.get('wt_opened') == '0', f'a session opened: {detail}')
        expect(client.get('wt_status') == str(status), f'the client read another status: {detail}')
        expect(server.get('wt_opened') == '0', f'the server opened one: {detail}')
        if self.reject:
            expect(server.get('wt_status') == str(status), f'the server sent another status: {detail}')


SHAPES = (
    dict(streams=1),
    dict(streams=16, stream_bytes=65536),
    dict(streams=4, stream_bytes=3145728),
    dict(uni=16, uni_bytes=32768),
    dict(datagrams=64),
    dict(resets=8),
    dict(stops=8),
    dict(streams=8, stream_bytes=4096, uni=8, uni_bytes=4096, resets=4, stops=4, datagrams=32, closer='server'),
    dict(streams=1, protocols=True, origin=GOOD_ORIGIN),
    dict(reject=403),
    dict(reject=404),
    dict(streams=8, stream_bytes=262144, uni=4, uni_bytes=65536, resets=2, stops=2, datagrams=32, path=LOSSY),
)


def capped_for_go(shape):
    """A Go end's HTTP/3 layer holds GO_DATAGRAMS datagrams for a session nobody reads yet and drops the rest, so a
    burst that lands with the 2xx would lose the remainder on a clean path, as datagrams may."""
    return dict(shape, datagrams=min(shape.get('datagrams', 0), GO_DATAGRAMS))


def cases(cerid, quinn, go):
    pairs = [(quinn, 'quinn', 'legacy')] if quinn else []
    pairs += [(go, 'go', 'h3')] if go else []
    pairs += [(cerid, 'cerid', 'legacy'), (cerid, 'cerid', 'h3')]
    for peer, name, revision in pairs:
        for cerid_is_client in (True, False):
            def case(**options):
                return Case(cerid, peer, name, cerid_is_client, revision, **options)

            for shape in SHAPES:
                yield case(**(capped_for_go(shape) if name == 'go' else shape))
            if not cerid_is_client:
                yield case(streams=1, origin=BAD_ORIGIN, allow=GOOD_ORIGIN, refused=403)
                yield case(streams=1, origin=GOOD_ORIGIN, allow=GOOD_ORIGIN)
            yield case(streams=4, stream_bytes=1 << 20, path=dict(mtu=1350))
            yield case(streams=4, stream_bytes=1 << 20,
                       path=dict(fall=('server' if cerid_is_client else 'client', 3000, 1300)))
            if revision == 'h3':
                yield case(streams=4, stream_bytes=1048576, window=65536)
                yield case(streams=8, stream_bytes=65536, uni=8, uni_bytes=65536, window=131072)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--quinn')
    parser.add_argument('--go')
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    all_cases = [case for case in cases(arguments.cerid, arguments.quinn, arguments.go) if arguments.only in case.describe()]
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
