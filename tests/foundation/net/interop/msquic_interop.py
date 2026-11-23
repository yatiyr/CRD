#!/usr/bin/env python3
"""QUIC interoperability of crd-net against the pinned msquic over OpenSSL 4.0 (NET-PATH).

Runs crd-quic-peer (crd-net over AWS-LC) against crd-msquic-quic-peer, each in its own process over real UDP sockets on
the loopback, both ways round and in both QUIC versions. The application is the echo of quic_interop.py: a client
writes `streams` streams of `size` bytes with a FIN and a server writes each back in another pattern, its FIN after
the client's. Both ends check every byte. Each case holds the handshake, the version, the server's pinned Ed25519
certificate (whose handshake signature the client checks), the application protocol, every stream and byte both ways
and the client's application close to both reports. A Cerid client checks the server's key itself
(crd-quic-peer's `--seed`), so its handshake ends there when the pin does not hold.

The hostile cases run the same exchange through interop_common.Relay: random loss, duplication, and a delay with
jitter that reorders datagrams, each way, so each end's loss recovery and reassembly meet the other's retransmissions.
"""
import argparse
import subprocess
import sys
import threading

from interop_common import Relay, collect, expect, reports_of

SEED = '11' * 32
RUN_SECONDS = 70
CODE = 0x2A
HOSTILE = dict(loss=0.02, duplicate=0.01, delay=0.005, jitter=0.004)


class Case:
    def __init__(self, cerid, msquic, cerid_is_client, version=1, streams=4, size=256 << 10, path=None):
        self.cerid = cerid
        self.msquic = msquic
        self.cerid_is_client = cerid_is_client
        self.version = version
        self.streams = streams
        self.size = size
        self.path = path  # a Relay's settings; None: the loopback

    def command(self, program, role, port):
        common = ['--role', role, '--port', str(port), '--seed', SEED, '--version', str(self.version), '--streams',
                  str(self.streams), '--bytes', str(self.size), '--code', str(CODE)]
        if program == self.cerid:
            return [program] + common + ['--group', 'x25519', '--cipher', 'aes128', '--closer', 'client',
                                         '--max-streams', str(max(16, self.streams))]
        return [program] + common

    def describe(self):
        path = 'loopback' if self.path is None else ' '.join(f'{name} {value}' for name, value in self.path.items())
        return ('cerid client' if self.cerid_is_client else 'msquic client') + \
            f' v{self.version} {self.streams}x{self.size} {path}'

    def run(self):
        server_program, client_program = (self.msquic, self.cerid) if self.cerid_is_client else (self.cerid,
                                                                                                  self.msquic)
        server = subprocess.Popen(self.command(server_program, 'server', 0), stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, text=True)
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
            detail = f'client {clients} ({client.returncode}) {client_err}\n' \
                     f'server {servers} ({server.returncode}) {server_err}'
            expect(client.returncode == 0 and server.returncode == 0, f'an end failed: {detail}')
            expect(len(clients) == 1 and len(servers) == 1, f'other connections: {detail}')
            self.check(clients[0], servers[0], detail)
        finally:
            if relay is not None:
                relay.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check(self, client, server, detail):
        total = self.streams * self.size
        for name, report in (('client', client), ('server', server)):
            expect(report.get('established') == '1', f'{name} never established: {detail}')
            expect(report.get('version') == str(self.version), f'{name} ran another version: {detail}')
            expect(report.get('verified') == '1', f'{name} read a wrong byte: {detail}')
            expect(report.get('streams') == str(self.streams), f'{name} counted other streams: {detail}')
            expect(report.get('bytes_received') == str(total), f'{name} read other bytes: {detail}')
            expect(report.get('bytes_sent') == str(total), f'{name} wrote other bytes: {detail}')
        expect(client.get('close') == 'local' and client.get('close_code') == str(CODE),
               f'the client closed otherwise: {detail}')
        expect(server.get('close') == 'application' and server.get('close_code') == str(CODE),
               f'the server read another close: {detail}')
        msquic = server if self.cerid_is_client else client
        expect(msquic.get('alpn') == 'cerid-interop', f'msquic negotiated another protocol: {detail}')
        if not self.cerid_is_client:
            expect(client.get('pin') == '1', f'msquic read another certificate key: {detail}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--msquic', required=True)
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    cases = []
    for cerid_is_client in (True, False):
        for version in (1, 2):
            cases.append(Case(arguments.cerid, arguments.msquic, cerid_is_client, version))
        cases.append(Case(arguments.cerid, arguments.msquic, cerid_is_client, streams=16, size=1))
        cases.append(Case(arguments.cerid, arguments.msquic, cerid_is_client, streams=1, size=4 << 20))
        cases.append(Case(arguments.cerid, arguments.msquic, cerid_is_client, size=64 << 10, path=HOSTILE))
    failures = 0
    for case in cases:
        if arguments.only not in case.describe():
            continue
        try:
            case.run()
            print(f'PASS {case.describe()}')
        except AssertionError as error:
            failures += 1
            print(f'FAIL {case.describe()}: {error}')
        sys.stdout.flush()
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
