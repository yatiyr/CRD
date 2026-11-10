#!/usr/bin/env python3
"""QMux interoperability of crd-net's QmuxHost with the pinned `qmux` crate over rustls (4.2a).

Runs crd-qmux-peer (crd-net over AWS-LC) against crd-qmux-crate-peer, each in its own process over real TCP on the
loopback, both ways round. The application is interop_echo.hpp's. Every case reads both ends' reports and holds them to
each other: the handshake completed, the application protocol, the streams and bytes each end counted and checked, the
close the closing end chose as the other end read it.

Cases: each end closing, with a code and a reason; the hybrid groups rustls ships and X25519; the three AEADs; a
transfer far over the stream and connection windows; more streams than either end allows at once, so MAX_STREAMS turns
over; unidirectional streams, RESET_STREAM and STOP_SENDING each way; datagrams at and over the peer's limit, beside
streams; resumption on a second connection, with tickets that allow 0-RTT; QX_PING keeping a connection past its idle
timeout with no application data, and the RTT each end measures; a larger max_record_size than crd-net sends; a relay
that cuts the byte stream into pieces of a few bytes, so Sizes and records split anywhere; a relay that goes silent,
which each end's idle timeout ends; and a refusal when the two share no application protocol.

WebSocket cases run the same application over WebSocket over HTTP/1.1 (RFC 6455) inside TLS, tokio-tungstenite on the
crate's side: one Binary message per record, as the crate's WebSocket transport and crd-net both write them. Tunnel
cases run it over WebSockets on one HTTP/2 connection (RFC 8441), the `h2` crate's extended CONNECT on the crate's side
and crd-net's WebSocketTunnels on the other, one tunnel or two at once, and through the cutting and the silent relay too;
a subprotocol the server does not take is refused with 400 before any QMux. Over `h2` that connection is TLS with ALPN
`h2` on crd-net's QmuxHost, whose server context lists QMux, http/1.1 and h2 on one port, each tunnel a connection of
the host, and the Cerid end reports how the HTTP/2 connection ended (NO_ERROR, closed by the client); over `h2c` it is
TCP with prior knowledge. HTTP/3 tunnel cases
(RFC 9220) run the same between crd-quic-peer's tunnels and the `h3` crate's over Quinn, real UDP, also through a relay
that loses, duplicates, delays and reorders datagrams; the Cerid end reports how the QUIC connection ended (H3_NO_ERROR,
the client closing once the server's GOAWAY came).

0-RTT stays with crd-net's own tests and its fuzzing: the crate reads and writes QMux only after the handshake.
"""
import argparse
import random
import socket
import subprocess
import sys
import threading
import time

import interop_common
from interop_common import collect, expect, reports_of

SEED = '11' * 32
PROTOCOL = 'cerid-interop'
CLOSE_REASON = 'cerid-interop done'
RUN_SECONDS = 70
RESET_CODE = '7'
STOP_CODE = '9'
H3_NO_ERROR = str(0x100)


class Relay:
    """A TCP hop between the two, for one connection at a time: cuts the bytes into seeded pieces, or goes silent."""

    def __init__(self, server_port, seed=0, largest=0, silent_after=None):
        self.server_port = server_port
        self.random = random.Random(seed)
        self.largest = largest
        self.silent_after = silent_after
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(4)
        self.port = self.listener.getsockname()[1]
        self.threads = [threading.Thread(target=self.serve, daemon=True)]
        self.threads[0].start()

    def serve(self):
        while True:
            try:
                near, _ = self.listener.accept()
            except OSError:
                return
            far = socket.create_connection(('127.0.0.1', self.server_port))
            for side in (near, far):
                side.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            started = time.monotonic()
            for source, sink in ((near, far), (far, near)):
                thread = threading.Thread(target=self.pump, args=(source, sink, started), daemon=True)
                thread.start()
                self.threads.append(thread)

    def pump(self, source, sink, started):
        try:
            while True:
                data = source.recv(65536)
                silent = self.silent_after is not None and time.monotonic() - started >= self.silent_after
                if not data:
                    if not silent:
                        sink.shutdown(socket.SHUT_WR)
                    return
                if silent:
                    continue
                at = 0
                while at < len(data):
                    size = self.random.randint(1, self.largest) if self.largest else len(data)
                    sink.sendall(data[at:at + size])
                    at += size
        except OSError:
            return

    def close(self):
        self.listener.close()


class Case:
    def __init__(self, cerid, crate, cerid_is_client, group='x25519mlkem768', cipher='aes128', streams=4,
                 size=256 << 10, closer='client', code=0, max_streams=16, uni=0, uni_size=0, resets=0, stops=0,
                 datagrams=0, datagram_bytes=1000, datagram_limit=None, resume=False, relay=None, idle_ms=None,
                 keepalive_ms=None, hold_ms=None, record_size=None, crate_version=None, transport='tls', tunnels=1,
                 path=None):
        self.cerid = cerid
        self.crate = crate
        self.cerid_is_client = cerid_is_client
        self.group = group
        self.cipher = cipher
        self.streams = streams
        self.size = size
        self.closer = closer
        self.code = code
        self.max_streams = max_streams
        self.uni = uni
        self.uni_size = uni_size
        self.resets = resets
        self.stops = stops
        self.datagrams = datagrams
        self.datagram_bytes = datagram_bytes
        self.datagram_limit = datagram_limit
        self.resume = resume
        self.relay = relay
        self.idle_ms = idle_ms
        self.keepalive_ms = keepalive_ms
        self.hold_ms = hold_ms
        self.record_size = record_size
        self.crate_version = crate_version  # the crate offers or accepts only this QMux draft: a refusal
        # tls, ws (WebSocket over HTTP/1.1), h2, h2c or h3 (WebSocket tunnels over HTTP/2 in TLS or with prior
        # knowledge, or over HTTP/3)
        self.transport = transport
        self.tunnels = tunnels  # tunnels at once on the one HTTP connection
        self.path = path  # the impairments of interop_common.Relay between two HTTP/3 ends

    def command(self, program, role, port):
        cerid = program == self.cerid
        command = [program, '--role', role, '--port', str(port), '--seed', SEED, '--group', self.group,
                   '--cipher', self.cipher, '--streams', str(self.streams), '--bytes', str(self.size),
                   '--closer', self.closer, '--code', str(self.code), '--max-streams', str(self.max_streams)]
        if cerid:
            command += ['--alpn', f'qmux-02.{PROTOCOL}']
        if self.uni or self.resets or self.stops:
            command += ['--uni', str(self.uni), '--uni-bytes', str(self.uni_size), '--resets', str(self.resets),
                        '--stops', str(self.stops)]
        if self.datagrams:
            command += ['--datagrams', str(self.datagrams), '--datagram-bytes', str(self.datagram_bytes)]
        if self.datagram_limit:
            command += ['--datagram-limit', str(self.datagram_limit), '--datagram-probe', '1']
        if self.resume:
            command += ['--connections', '2']
            command += ['--early', '1'] if cerid else []
        if self.tunnels != 1:
            command += ['--connections', str(self.tunnels)]
        if self.idle_ms:
            command += ['--idle-ms', str(self.idle_ms)]
        if self.hold_ms:
            command += ['--hold-ms', str(self.hold_ms)]
        if self.keepalive_ms and cerid:
            command += ['--keepalive-ms', str(self.keepalive_ms)]
        if self.record_size and not cerid:
            command += ['--record-size', str(self.record_size)]
        if self.crate_version and not cerid:
            command += ['--alpn-version', self.crate_version]
        if self.transport != 'tls':
            command += ['--transport', self.transport]
        return command

    def describe(self):
        extra = (f' {self.uni}x{self.uni_size} uni' if self.uni else '') + \
            (f' {self.resets} resets' if self.resets else '') + (f' {self.stops} stops' if self.stops else '') + \
            (f' {self.datagrams} datagrams of {self.datagram_bytes}' if self.datagrams else '') + \
            (f' limit {self.datagram_limit}' if self.datagram_limit else '') + (' resume' if self.resume else '') + \
            (f' relay {self.relay}' if self.relay else '') + (f' path {self.path}' if self.path else '') + \
            (f' idle {self.idle_ms}' if self.idle_ms else '') + \
            (f' hold {self.hold_ms}' if self.hold_ms else '') + \
            (f' record {self.record_size}' if self.record_size else '') \
            + (f' crate draft {self.crate_version}' if self.crate_version else '') \
            + (f' over {self.transport}' if self.transport != 'tls' else '') \
            + (f' {self.tunnels} tunnels' if self.tunnels != 1 else '')
        return ('cerid client' if self.cerid_is_client else 'crate client') + \
            f' {self.group} {self.cipher} {self.streams}x{self.size} {self.closer} closes {self.code}' + extra

    def run(self):
        server_program, client_program = self.crate, self.cerid
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
            if self.relay is not None:
                relay = Relay(port, **self.relay)
                port = relay.port
            elif self.path is not None:
                relay = interop_common.Relay(port, **self.path)
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
            expect(client.returncode == 0 and server.returncode == 0,
                   f'exit codes {client.returncode}, {server.returncode}\n{client_err}\n{server_err}')
            self.check(reports_of(client_out), reports_of(server_out[0]),
                       f'{client_out}\n{server_out[0]}\n{client_err}\n{server_err}')
        finally:
            for process in (client, server):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()
            if isinstance(relay, Relay):
                relay.close()
            elif relay is not None:
                relay.stop()

    def check(self, clients, servers, detail):
        connections = 2 if self.resume else self.tunnels
        # Tunnels end in any order: each report names its tunnel.
        clients, servers = (sorted(reports, key=lambda report: int(report['connection']))
                            for reports in (clients, servers))
        if self.crate_version and self.transport in ('h2', 'h2c', 'h3') and not self.cerid_is_client:
            # crd-net's WebSocketTunnels answers a refused CONNECT itself: no tunnel, so no report.
            expect(len(clients) == connections and not servers, f'other reports: {detail}')
            expect(all('400' in client.get('error', '') for client in clients), f'the crate failed otherwise: {detail}')
            return
        cerid = clients if self.cerid_is_client else servers
        if self.transport == 'h3' and not self.crate_version:
            self.check_carrier_close(cerid, 'h3', H3_NO_ERROR, ('local',), ('application',), detail)
        if self.transport == 'h2':
            # The crate's server sends GOAWAY once its tunnels are done, which may reach the Cerid client before its
            # own close does; behind the silent relay the server's connection idles out once its tunnels have.
            ended = self.check_carrier_close(cerid, 'h2', '0', ('local', 'shutdown'),
                                             ('idle',) if self.closer == 'none' else ('shutdown',), detail)
            expect(ended.get('h2_alpn') == 'h2' and ended.get('h2_tls_error') == 'none',
                   f'TLS agreed or ended otherwise: {detail}')
        expect(len(clients) == connections and len(servers) == connections, f'other reports: {detail}')
        for number in range(connections):
            client, server = clients[number], servers[number]
            if self.crate_version:
                self.check_refused(client, server, detail)
            else:
                self.check_connection(number, client, server, detail)

    def check_carrier_close(self, cerid, carrier, code, as_client, as_server, detail):
        """The Cerid end's HTTP/3 or HTTP/2 connection, reported once behind its last tunnel's report, ended with
        `code` as one of `as_client` or `as_server` says; that report."""
        ended = [report for report in cerid if f'{carrier}_close' in report]
        expect(len(ended) == 1 and ended[0].get(f'{carrier}_close_code') == code, f'{carrier} ended otherwise: {detail}')
        expect(ended[0][f'{carrier}_close'] in (as_client if self.cerid_is_client else as_server),
               f'the other end closed {carrier}: {detail}')
        return ended[0]

    def check_refused(self, client, server, detail):
        cerid, crate = (client, server) if self.cerid_is_client else (server, client)
        expect(cerid.get('established') == '0' and crate.get('established') == '0', f'established: {detail}')
        if self.transport != 'tls':
            expect(cerid.get('close') == 'upgrade-refused' and cerid.get('close_code') == '400',
                   f'crd-net closed otherwise: {detail}')
            expect('400' in crate.get('error', ''), f'the crate failed otherwise: {detail}')
            return
        expect(cerid.get('close') == 'tls', f'crd-net closed otherwise: {detail}')
        if not self.cerid_is_client:
            expect(cerid.get('tls_error') == 'peer-refused', f'crd-net refused otherwise: {detail}')
        expect('NoApplicationProtocol' in crate.get('error', '') or 'no_application_protocol' in crate.get('error', ''),
               f'the crate failed otherwise: {detail}')

    def check_connection(self, number, client, server, detail):
        cerid, crate = (client, server) if self.cerid_is_client else (server, client)
        for name, report in (('client', client), ('server', server)):
            expect(report.get('established') == '1', f'{name} was not established: {detail}')
            expect(report.get('verified') == '1', f'{name} read wrong bytes: {detail}')
            expect(report.get('streams') == str(self.streams), f'{name} counted other streams: {detail}')
            expect(report.get('uni_received') == str(self.uni), f'{name} read other unidirectional streams: {detail}')
            expect(report.get('uni_finished') == str(self.uni), f'{name} finished other streams: {detail}')
            expect(report.get('resets_seen') == str(self.resets), f'{name} saw other resets: {detail}')
            expect(report.get('stops_seen') == str(self.stops), f'{name} saw other stops: {detail}')
            if self.resets:
                expect(report.get('reset_code') == RESET_CODE, f'{name} read another reset code: {detail}')
            if self.stops:
                expect(report.get('stop_code') == STOP_CODE, f'{name} read another stop code: {detail}')
            if not self.uni and not self.resets and not self.stops:
                expected = str(self.streams * self.size)
                expect(report.get('bytes_received') == expected and report.get('bytes_sent') == expected,
                       f'{name} counted other bytes: {detail}')
        if self.transport != 'tls':
            carrier = {'ws': 'http/1.1', 'h2c': 'h2'}.get(self.transport, self.transport)
            expect(cerid.get('alpn') == carrier and cerid.get('websocket_protocol') == f'qmux-02.{PROTOCOL}',
                   f'crd-net agreed on another protocol: {detail}')
        else:
            expect(cerid.get('alpn') == f'qmux-02.{PROTOCOL}', f'crd-net agreed on another protocol: {detail}')
        expect(crate.get('protocol') == PROTOCOL, f'the crate agreed on another protocol: {detail}')
        expect(cerid.get('resumed') == ('1' if self.resume and number == 1 else '0'), f'resumption differs: {detail}')
        expect(cerid.get('early') == 'none', f'early data was used: {detail}')
        if self.datagrams:
            self.check_datagrams(client, server, detail)
        self.check_close(client, server, detail)
        if self.keepalive_ms:
            expect(int(cerid.get('pongs_received', '0')) > 0 and cerid.get('rtt') == '1', f'no RTT here: {detail}')
            expect(crate.get('rtt') == '1', f'no RTT at the crate: {detail}')

    def check_close(self, client, server, detail):
        if self.closer == 'none':
            for name, report in (('client', client), ('server', server)):
                expect(report.get('close') == 'idle', f'{name} closed otherwise: {detail}')
            return
        closing, other = (client, server) if self.closer == 'client' else (server, client)
        expect(closing.get('close') == 'local' and closing.get('close_code') == str(self.code),
               f'the closing end closed otherwise: {detail}')
        expect(other.get('close') == 'application' and other.get('close_code') == str(self.code),
               f'the other end read another close: {detail}')
        expect(other.get('close_reason') == CLOSE_REASON, f'the other end read another reason: {detail}')

    def check_datagrams(self, client, server, detail):
        expect(int(client.get('datagrams_sent', '0')) == self.datagrams, f'the client sent other datagrams: {detail}')
        for name, report in (('client', client), ('server', server)):
            expect(report.get('datagrams_verified') == '1', f'{name} read a wrong datagram: {detail}')
            received = int(report.get('datagrams_received', '0'))
            # The crate drops a datagram rather than wait for a transport its streams keep busy.
            share = 0.5 if self.streams else 0.9
            expect(self.datagrams * share <= received <= self.datagrams, f'{name} read too few datagrams: {detail}')
            if self.datagram_limit:
                expect(report.get('datagrams_refused') == '1', f'{name} sent past the limit: {detail}')


def websocket_cases(case, transport):
    """The application's cases over a WebSocket carrier, with the impaired paths it can take: the cutting and the silent
    TCP relay under TLS, HTTP/1.1 and HTTP/2 (inside TLS or not), a lossy and reordering UDP one under HTTP/3."""
    yield case(transport=transport)
    yield case(closer='server', code=0x2A, transport=transport)
    yield case(streams=1, size=16 << 20, transport=transport)
    yield case(streams=8, size=4 << 20, closer='server', code=1, transport=transport)
    yield case(streams=64, size=32 << 10, max_streams=8, transport=transport)
    # The crate's close abandons a write its WebSocket has not finished (`transmit_or_teardown`), so the closing end
    # holds a second after its streams are done, as the reset cases do.
    yield case(streams=0, uni=8, uni_size=1 << 20, hold_ms=1000, transport=transport)
    yield case(streams=0, resets=4, hold_ms=1000, transport=transport)
    yield case(streams=0, stops=4, hold_ms=1000, transport=transport)
    yield case(streams=2, size=64 << 10, uni=4, uni_size=64 << 10, resets=2, stops=2, closer='server', code=3,
               transport=transport)
    yield case(streams=0, datagrams=128, transport=transport)
    yield case(streams=4, datagrams=128, transport=transport)
    yield case(streams=0, datagrams=16, datagram_bytes=597, datagram_limit=600, transport=transport)
    yield case(streams=1, size=1024, idle_ms=900, keepalive_ms=250, hold_ms=2500, transport=transport)
    if transport == 'h3':
        for seed in (1, 2, 3):
            yield case(streams=2, size=256 << 10, transport=transport,
                       path=dict(seed=seed, loss=0.05, duplicate=0.05, delay=0.005, jitter=0.01))
    else:
        for seed in (1, 2, 3):
            yield case(streams=2, size=64 << 10, relay=dict(seed=seed, largest=7), transport=transport)
        yield case(streams=0, closer='none', idle_ms=400, relay=dict(silent_after=0.2), transport=transport)
    yield case(streams=0, crate_version='01', transport=transport)


def cases(cerid, quic, crate):
    for cerid_is_client in (True, False):
        def case(**settings):
            program = quic if settings.get('transport') == 'h3' else cerid
            return Case(program, crate, cerid_is_client, **settings)

        for closer, code in (('client', 0), ('server', 0x2A)):
            yield case(closer=closer, code=code)
        for group in ('x25519mlkem768', 'secp256r1mlkem768', 'x25519'):
            yield case(group=group)
        for cipher in ('aes128', 'aes256', 'chacha20'):
            yield case(cipher=cipher)
        yield case(streams=1, size=16 << 20)
        yield case(streams=8, size=4 << 20, closer='server', code=1)
        yield case(streams=64, size=32 << 10, max_streams=8)
        yield case(streams=0, uni=8, uni_size=1 << 20)
        # Nothing tells the closing end its resets were read, and a CONNECTION_CLOSE ends the crate's readers: the hold
        # gives them a second, the settling time the datagram cases use.
        yield case(streams=0, resets=4, hold_ms=1000)
        # The closing end completes in the turn it asks for its last stop, and its CONNECTION_CLOSE is immediate (RFC
        # 9000 section 10.2): a STOP_SENDING still queued then goes nowhere. The hold lets it out first (1 run in 80).
        yield case(streams=0, stops=4, hold_ms=1000)
        yield case(streams=2, size=64 << 10, uni=4, uni_size=64 << 10, resets=2, stops=2, closer='server', code=3)
        yield case(streams=0, datagrams=128)
        yield case(streams=4, datagrams=128)
        yield case(streams=0, datagrams=16, datagram_bytes=597, datagram_limit=600)
        yield case(streams=2, size=16 << 10, resume=True)
        yield case(streams=1, size=1024, idle_ms=900, keepalive_ms=250, hold_ms=2500)
        yield case(streams=2, size=1 << 20, record_size=65535)
        for seed in (1, 2, 3):
            yield case(streams=2, size=64 << 10, relay=dict(seed=seed, largest=7))
        yield case(streams=0, closer='none', idle_ms=400, relay=dict(silent_after=0.2))
        yield case(streams=0, crate_version='01')
        for transport in ('ws', 'h2c', 'h2'):
            yield from websocket_cases(case, transport)
        for transport in ('h2c', 'h2'):
            yield case(transport=transport, tunnels=2)
            yield case(streams=8, size=1 << 20, datagrams=64, closer='server', code=5, transport=transport, tunnels=2)
            yield case(streams=2, size=64 << 10, relay=dict(seed=4, largest=7), transport=transport, tunnels=2)
        yield case(group='secp256r1mlkem768', cipher='chacha20', transport='h2', tunnels=2)
        yield from websocket_cases(case, 'h3')
        yield case(transport='h3', tunnels=2)
        yield case(streams=8, size=1 << 20, datagrams=64, closer='server', code=5, transport='h3', tunnels=2)
        yield case(streams=2, size=256 << 10, transport='h3', tunnels=2,
                   path=dict(seed=4, loss=0.05, duplicate=0.05, delay=0.005, jitter=0.01))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True, help='crd-qmux-peer')
    parser.add_argument('--quic', required=True, help='crd-quic-peer, the Cerid end of the HTTP/3 tunnels')
    parser.add_argument('--crate', required=True)
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    all_cases = [case for case in cases(arguments.cerid, arguments.quic, arguments.crate)
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
