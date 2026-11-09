#!/usr/bin/env python3
"""HTTP/2 interoperability of crd-net with h2spec and nghttp2 (4.2a).

h2spec runs every generic, HPACK and RFC 7540 conformance case, the strict ones included, against crd-http2-peer as a
server; each failure is named by its test ID and only those in ACCEPTED pass, each with its reason. crd-http2-peer
(Http2Connection over TCP, prior knowledge) and crd-nghttp2-peer run http_exchange.h both ways round in separate
processes over real TCP on the loopback: every field and body byte is checked by both ends and each case holds the two
reports to each other.

Cases: no body, small bodies, bodies far over the windows, more requests than the stream limit lets open at once;
HPACK's table off, small enough to evict and large; field sections that need CONTINUATION and one over the peer's
limit; small
windows and large frames; padding; trailers; 103; Extended CONNECT tunnels; GOAWAY in two steps, rejecting requests and
from a client; each end cancelling; and a path that hands the bytes on a few at a time.
"""
import argparse
import subprocess
import sys
import tempfile
import threading
import xml.etree.ElementTree as ElementTree
from pathlib import Path

from interop_common import Chopper, collect, expect, reports_of

RUN_SECONDS = 70
H2SPEC_SECONDS = 300
CANCEL_CODE = 8
REFUSED_CODE = 7
# What h2spec expects that RFC 9113 or this implementation's documented choice answers otherwise: ID to reason.
ACCEPTED = {}


def brief(reports):
    return [{name: value for name, value in report.items() if name.startswith('h2_') and value != '0'}
            for report in reports]


class Case:
    def __init__(self, cerid, peer, cerid_is_client, requests=1, request_bytes=0, response_bytes=4096, tunnels=0,
                 table=4096, field_bytes=0, field_limit=16384, window=1 << 20, frame_bytes=16384, padding=False,
                 trailers=False, interim=False, goaway=None, cancel=None, over_limit=False, chop=None, max_streams=64):
        self.cerid = cerid
        self.peer = peer
        self.cerid_is_client = cerid_is_client
        self.requests = requests
        self.request_bytes = request_bytes
        self.response_bytes = response_bytes
        self.tunnels = tunnels
        self.table = table
        self.field_bytes = field_bytes
        self.field_limit = field_limit
        self.window = window
        self.frame_bytes = frame_bytes
        self.padding = padding
        self.trailers = trailers
        self.interim = interim
        self.goaway = goaway
        self.cancel = cancel
        self.over_limit = over_limit
        self.chop = chop
        self.max_streams = max_streams

    def command(self, program, role, port):
        command = [program, '--role', role, '--port', str(port), '--max-streams', str(self.max_streams),
                   '--window', str(self.window), '--frame-bytes', str(self.frame_bytes),
                   '--requests', str(self.requests), '--request-bytes', str(self.request_bytes),
                   '--response-bytes', str(self.response_bytes), '--tunnels', str(self.tunnels),
                   '--table', str(self.table), '--field-bytes', str(self.field_bytes),
                   '--field-limit', str(self.field_limit), '--trailers', str(int(self.trailers)),
                   '--interim', str(int(self.interim)), '--over-limit', str(int(self.over_limit))]
        if self.goaway:
            command += ['--goaway', self.goaway]
        if self.cancel:
            command += ['--cancel', self.cancel]
        if self.padding and program == self.peer:
            command += ['--padding', '1']
        return command

    def describe(self):
        words = ['cerid client' if self.cerid_is_client else 'nghttp2 client',
                 f'{self.requests}x{self.request_bytes}/{self.response_bytes}', f'table {self.table}']
        for name, value in (('tunnels', self.tunnels), ('fields', self.field_bytes), ('trailers', self.trailers),
                            ('interim', self.interim), ('goaway', self.goaway), ('cancel', self.cancel),
                            ('over-limit', self.over_limit), ('padding', self.padding), ('chop', self.chop),
                            ('window', self.window != 1 << 20 and self.window),
                            ('frame', self.frame_bytes != 16384 and self.frame_bytes),
                            ('max-streams', self.max_streams != 64 and self.max_streams)):
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
        chopper = None
        try:
            first = server.stdout.readline().strip()
            if not first.startswith('port='):
                raise AssertionError(f'the server printed {first!r}; {server.stderr.read()}')
            port = int(first[len('port='):])
            if self.chop is not None:
                chopper = Chopper(port, **self.chop)
                port = chopper.port
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
            if chopper is not None:
                chopper.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check(self, clients, servers, client_code, server_code, client_err, server_err):
        detail = f'client {brief(clients)} ({client_code}) {client_err}\n' \
                 f'server {brief(servers)} ({server_code}) {server_err}'
        expect(client_code == 0 and server_code == 0, f'an end failed: {detail}')
        expect(len(clients) == 1 and len(servers) == 1, f'other connections: {detail}')
        client, server = clients[0], servers[0]
        for name, report in (('client', client), ('server', server)):
            expect(report.get('h2_verified') == '1', f'{name} read a wrong field or byte: {detail}')
            expect(report.get('h2_peer_known') == '1', f'{name} never read SETTINGS: {detail}')
            expect(report.get('h2_peer_table') == str(self.table), f'{name} read another table: {detail}')
            expect(report.get('h2_peer_field_limit') == str(self.field_limit), f'{name} read another limit: {detail}')
            expect(report.get('h2_peer_streams') == str(self.max_streams), f'{name} read another limit: {detail}')
            expect(report.get('h2_peer_window') == str(self.window), f'{name} read another window: {detail}')
            expect(report.get('h2_peer_frame') == str(self.frame_bytes), f'{name} read another frame limit: {detail}')
        expect(client.get('h2_peer_connect') == '1', f'the client read no Extended CONNECT: {detail}')
        self.check_requests(client, server, detail)
        self.check_tables(client if self.cerid_is_client else server, detail)

    def check_tables(self, cerid, detail):
        """nghttp2 and Cerid each index a field they see more than once, and never into a table of size 0."""
        if self.table == 0:
            expect(self.number(cerid, 'h2_inserts_received') == 0, f'nghttp2 inserted into nothing: {detail}')
            expect(self.number(cerid, 'h2_inserts_sent') == 0, f'Cerid inserted into nothing: {detail}')
        elif self.requests >= 4 and self.goaway != 'reject':
            expect(self.number(cerid, 'h2_inserts_received') > 0, f'nghttp2 inserted nothing: {detail}')
            expect(self.number(cerid, 'h2_inserts_sent') > 0, f'Cerid inserted nothing: {detail}')

    @staticmethod
    def number(report, name):
        return int(report.get(name, '-1'))

    def check_requests(self, client, server, detail):
        cancelled = self.cancel is not None
        expected = self.requests - (1 if cancelled else 0)
        answered = self.requests - (1 if self.over_limit else 0)
        if self.goaway != 'reject':
            refused = 1 if self.over_limit and self.cerid_is_client else 0
            expect(self.number(client, 'h2_completed') == expected - refused,
                   f'the client completed other requests: {detail}')
            expect(self.number(server, 'h2_seen') == answered, f'the server saw other requests: {detail}')
            if not cancelled:
                plain = answered * self.request_bytes
                expect(self.number(server, 'h2_body_received') == plain, f'the server read other bytes: {detail}')
                plain = answered * self.response_bytes
                expect(self.number(client, 'h2_body_received') == plain, f'the client read other bytes: {detail}')
                if self.trailers:
                    messages = answered - self.tunnels
                    expect(self.number(client, 'h2_trailers') == messages, f'the client read no trailers: {detail}')
                    expect(self.number(server, 'h2_trailers') == messages, f'the server read no trailers: {detail}')
        if self.interim:
            expect(self.number(client, 'h2_interims') >= expected, f'the client read too few interims: {detail}')
        expect(self.number(client, 'h2_tunnels') == self.tunnels, f'the client opened other tunnels: {detail}')
        expect(self.number(server, 'h2_tunnels') == self.tunnels, f'the server served other tunnels: {detail}')
        if self.over_limit:
            name = 'h2_refused_too_large' if self.cerid_is_client else 'h2_status_431'
            expect(self.number(client, name) == 1, f'the client was not refused: {detail}')
        if self.cancel:
            canceller, cancelled_end = (client, server) if self.cancel == 'client' else (server, client)
            expect(self.number(canceller, 'h2_cancelled') == 1, f'the cancel was not made: {detail}')
            expect(self.number(cancelled_end, 'h2_aborted') == 1, f'the cancel was not seen: {detail}')
            expect(self.number(cancelled_end, 'h2_abort_code') == CANCEL_CODE, f'another code: {detail}')
        self.check_goaway(client, server, detail)

    def check_goaway(self, client, server, detail):
        if self.goaway == 'notice':
            expect(self.number(client, 'h2_goaway_seen') == 2, f'the client read other GOAWAYs: {detail}')
            expect(self.number(client, 'h2_goaway_id') == (self.requests * 2) - 1, f'the last names another: {detail}')
            expect(self.number(client, 'h2_refused_after_goaway') == 1, f'the client could still open: {detail}')
        elif self.goaway == 'reject':
            completed = self.number(client, 'h2_completed')
            rejected = self.number(client, 'h2_rejected')
            expect(self.number(client, 'h2_goaway_seen') == 1, f'the client read other GOAWAYs: {detail}')
            expect(completed + rejected == self.requests, f'requests went unaccounted: {detail}')
            expect(completed >= 1 and (completed * 2) - 1 == self.number(client, 'h2_goaway_id'),
                   f'the GOAWAY named other requests than were answered: {detail}')
        elif self.goaway == 'client':
            expect(self.number(server, 'h2_goaway_seen') >= 1, f'the server read no GOAWAY: {detail}')
            expect(self.number(server, 'h2_goaway_id') == 0, f'the last GOAWAY names another: {detail}')


def cases(cerid, peer):
    for cerid_is_client in (True, False):
        def case(**settings):
            return Case(cerid, peer, cerid_is_client, **settings)

        yield case()
        yield case(requests=8, request_bytes=4096, response_bytes=8192)
        yield case(requests=2, request_bytes=4 << 20, response_bytes=8 << 20)
        yield case(requests=256, request_bytes=512, response_bytes=2048, max_streams=8)
        yield case(requests=32, response_bytes=256, table=0)
        yield case(requests=32, response_bytes=256, table=256)
        yield case(requests=64, response_bytes=256, table=65536)
        yield case(requests=8, field_bytes=12000)
        yield case(requests=4, field_bytes=20000, field_limit=24000)
        yield case(requests=4, over_limit=True, field_limit=8192)
        yield case(requests=4, request_bytes=256 << 10, response_bytes=256 << 10, window=1024)
        yield case(requests=4, request_bytes=256 << 10, response_bytes=256 << 10, window=65535)
        yield case(requests=2, request_bytes=4 << 20, response_bytes=4 << 20, frame_bytes=1 << 20, window=1 << 22)
        yield case(requests=8, request_bytes=64 << 10, response_bytes=64 << 10, padding=True)
        yield case(requests=8, request_bytes=2048, trailers=True)
        yield case(requests=8, request_bytes=0, response_bytes=0, trailers=True)
        yield case(requests=8, interim=True, response_bytes=2048)
        yield case(requests=4, tunnels=4, request_bytes=64 << 10, response_bytes=128 << 10)
        yield case(requests=6, tunnels=2, request_bytes=1024, response_bytes=1024, trailers=True, interim=True)
        yield case(requests=8, goaway='notice')
        yield case(requests=8, goaway='reject')
        yield case(requests=8, goaway='client')
        yield case(requests=4, cancel='client', response_bytes=1 << 20)
        yield case(requests=4, cancel='server')
        for seed in (1, 2, 3):
            yield case(requests=16, request_bytes=32 << 10, response_bytes=32 << 10, padding=True,
                       chop=dict(seed=seed, piece=48))
        yield case(requests=48, request_bytes=256, response_bytes=256, field_bytes=6000, chop=dict(seed=4, piece=7))


def run_h2spec(cerid, h2spec, only):
    server = subprocess.Popen([cerid, '--role', 'server', '--port', '0', '--h2spec', '1'], stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True)
    try:
        first = server.stdout.readline().strip()
        expect(first.startswith('port='), f'the server printed {first!r}')
        with tempfile.TemporaryDirectory() as scratch:
            report = Path(scratch) / 'h2spec.xml'
            command = [h2spec, '--host', '127.0.0.1', '--port', first[len('port='):], '--strict', '--junit-report',
                       str(report), '--timeout', '3']
            command += [only] if only else []
            output = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                    timeout=H2SPEC_SECONDS).stdout
            expect(report.is_file(), f'h2spec wrote no report:\n{output}')
            root = ElementTree.parse(report).getroot()
    finally:
        server.kill()
        server.wait()
    return judge_h2spec(root, output)


def judge_h2spec(root, output):
    total = 0
    skipped = 0
    failures = []
    for suite in root.iter('testsuite'):
        for case in suite.iter('testcase'):
            total += 1
            name = f'{suite.get("package", "")} {case.get("classname", "")}'.strip()
            if case.find('skipped') is not None:
                skipped += 1
            if case.find('failure') is not None or case.find('error') is not None:
                failures.append(name)
    unexpected = [name for name in failures if name not in ACCEPTED]
    print(f'h2spec: {total} cases, {skipped} skipped, {len(failures)} failed, {len(unexpected)} not accepted',
          flush=True)
    for name in sorted(failures):
        print(f'  {"accepted" if name in ACCEPTED else "FAILED"}: {name}', flush=True)
    if unexpected:
        print(output, flush=True)
    expect(total > 0, 'h2spec ran nothing')
    expect(skipped == 0, f'h2spec skipped {skipped} cases')
    stale = [name for name in ACCEPTED if name not in failures]
    expect(not stale, f'accepted entries that no longer fail: {stale}')
    return not unexpected


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--nghttp2', required=True)
    parser.add_argument('--h2spec', required=True)
    parser.add_argument('--only', default='', help='run only the exchange cases whose description contains this text')
    parser.add_argument('--h2spec-only', default=None, help='run only this h2spec section and no exchange case')
    arguments = parser.parse_args()
    failures = 0
    if arguments.h2spec_only is not None or not arguments.only:
        try:
            failures += 0 if run_h2spec(arguments.cerid, arguments.h2spec, arguments.h2spec_only) else 1
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(f'h2spec FAILED: {error}', flush=True)
    if arguments.h2spec_only is not None:
        return 1 if failures else 0
    all_cases = [case for case in cases(arguments.cerid, arguments.nghttp2) if arguments.only in case.describe()]
    for index, case in enumerate(all_cases):
        try:
            case.run()
            print(f'[{index + 1}/{len(all_cases)}] ok: {case.describe()}', flush=True)
        except (AssertionError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(f'[{index + 1}/{len(all_cases)}] FAILED: {case.describe()}\n{error}', flush=True)
    print(f'{len(all_cases)} exchange cases, {failures} failures in all', flush=True)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
