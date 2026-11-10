#!/usr/bin/env python3
"""Congestion control of crd-net against the pinned picoquic over picotls (NET-PATH): Prague (L4S) both ways.

Runs crd-quic-peer (crd-net over AWS-LC) against crd-picoquic-quic-peer, each in its own process over real UDP sockets
on the loopback, both ways round. The application is the echo of quic_interop.py: a client writes `streams` streams
of `size` bytes and a server writes each back, so each direction carries one end's controller: a Cerid client's
Prague toward the server and picoquic's back, or the other way round. Both ends check every byte, and each case holds
the handshake, the version, the pinned certificate, the application protocol and the close to both reports.

The relay keeps each datagram's ECN codepoint (Relay `mark='keep'`) and makes each way a bottleneck (2.5 MB/s, a 30 ms
round trip, a two-BDP drop-tail buffer) whose queue marks CE: an L4S step at 1 ms (RFC 9332's native L4S AQM, as
draft-briscoe-iccrg-prague-congestion-control's evaluations use), a ramp from 0.5 to 2.5 ms, or a classic single-queue
AQM at 5 ms that marks ECT(0) and ECT(1) alike. Cerid runs Prague; picoquic runs its own Prague (prague.c, ECT(1)) or
its CUBIC, NewReno or BBR (ECT(0)).

Held in every case: each end marks the codepoint its controller asks for and the other reads it; Cerid validates ECN
(RFC 9000 section 13.4.2) on ACK frames that count ECT(1) and CE. Through an AQM, every CE an end counts is one the
relay marked: picoquic reads CE on Cerid's datagrams and Cerid's ACK frames report back the CE it read on picoquic's.
Cerid's Prague moves its alpha off 1 exactly when CE came (section 2.3.2), overflows no buffer and keeps its standing
queue within three times the step or the ramp's top; its classic detector stays at 0 when picoquic runs Prague too (the
detector reads the round trip, so picoquic's CUBIC, NewReno or BBR queued in front of Cerid's acknowledgements reads as
a classic bottleneck); a Cerid client, which no echo holds back, keeps its link busy; and picoquic's Prague keeps its
own queue to the same bound. Through the classic AQM, which also marks picoquic's CUBIC, Prague holds its queue near the
classic depth and a Cerid client its link busy; alone in its direction, Prague may keep that queue too steady for the
detector (TR-BB-2019-002), which test_prague.cpp proves beside a classic flow. A Cerid server echoes what its client
sends, so under loss its direction may build no queue and see no CE.
"""
import argparse
import subprocess
import sys
import threading

from interop_common import Relay, collect, expect, reports_of

SEED = '11' * 32
RUN_SECONDS = 70
CODE = 0x2A
RATE = 2_500_000  # bytes per second each way: 20 Mbit/s
DELAY = 0.015  # seconds each way, a 30 ms round trip
BDP = int(RATE * 2 * DELAY)
BOTTLENECK = dict(rate=RATE, buffer=2 * BDP, delay=DELAY, mark='keep')
L4S_STEP = dict(BOTTLENECK, step=0.001)
L4S_RAMP = dict(BOTTLENECK, ramp=(0.0005, 0.0025))
CLASSIC = dict(BOTTLENECK, classic=0.005)
CLASSIC_FULL = 1 << 16  # Prague's classic share, in 2^-16, once its detector has reached 1
ALPHA_ONE = 1 << 20  # Prague's alpha of 1, in 2^-20
# Seconds of a transfer before its utilisation counts: the first CE in slow start leaves Prague about a quarter of
# the BDP to grow back a datagram a round trip (S70), some 40 round trips; 0.79 of the link was seen from 1 s.
SETTLE = 2.0
BULK = 4 << 20  # bytes per stream through an AQM: about 7 s of transfer each way
CODEPOINTS = {'prague': 'ect1', 'cubic': 'ect0', 'newreno': 'ect0', 'bbr': 'ect0'}


class Case:
    def __init__(self, cerid, picoquic, cerid_is_client, version=1, streams=4, size=256 << 10, path=None,
                 peer_cc='prague'):
        self.cerid = cerid
        self.picoquic = picoquic
        self.cerid_is_client = cerid_is_client
        self.version = version
        self.streams = streams
        self.size = size
        self.path = path  # a Relay's settings, as a dictionary; None: the loopback
        self.peer_cc = peer_cc  # picoquic's controller

    def command(self, program, role, port):
        common = ['--role', role, '--port', str(port), '--seed', SEED, '--version', str(self.version), '--streams',
                  str(self.streams), '--bytes', str(self.size), '--code', str(CODE)]
        if program == self.cerid:
            return [program] + common + ['--group', 'x25519', '--cipher', 'aes128', '--closer', 'client',
                                         '--max-streams', '16', '--congestion', 'prague']
        return [program] + common + ['--cc', self.peer_cc]

    def describe(self):
        path = 'loopback'
        if self.path is not None:
            path = ' '.join(f'{name} {self.path[name]}' for name in ('step', 'ramp', 'classic', 'loss')
                            if name in self.path) or 'bottleneck'
        return ('cerid client' if self.cerid_is_client else 'picoquic client') + \
            f' v{self.version} {self.streams}x{self.size} picoquic {self.peer_cc} {path}'

    def run(self):
        server_program, client_program = self.picoquic, self.cerid
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
            cerid, picoquic = (clients[0], servers[0]) if self.cerid_is_client else (servers[0], clients[0])
            self.check_connection(clients[0], servers[0], detail)
            self.check_ecn(cerid, picoquic, detail)
            if relay is not None:
                self.check_relay(relay, cerid, picoquic, detail)
        finally:
            if relay is not None:
                relay.stop()
            for process in (server, client):
                if process is not None and process.poll() is None:
                    process.kill()
                    process.wait()

    def check_connection(self, client, server, detail):
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
        picoquic = client if not self.cerid_is_client else server
        expect(picoquic.get('alpn') == 'cerid-interop', f'picoquic negotiated another protocol: {detail}')
        if not self.cerid_is_client:
            expect(client.get('pin') == '1', f'picoquic read another certificate key: {detail}')

    def check_ecn(self, cerid, picoquic, detail):
        """Each end marks its controller's codepoint and the other reads it; Cerid validates on counts of ECT(1)."""
        expect(cerid.get('congestion') == 'prague', f'Cerid ran another controller: {detail}')
        expect(picoquic.get('congestion') == self.peer_cc, f'picoquic ran another controller: {detail}')
        expect(picoquic.get('ecn_mark') == CODEPOINTS[self.peer_cc], f'picoquic marks another codepoint: {detail}')
        expect(cerid.get('ecn') == 'capable', f'Cerid did not validate ECN: {detail}')
        expect(int(picoquic.get('ecn_received_ect1', '0')) > 0, f'picoquic read no ECT(1) from Cerid: {detail}')
        expect(int(picoquic.get('ecn_acked_' + CODEPOINTS[self.peer_cc], '0')) > 0,
               f'Cerid reported none of picoquic\'s {CODEPOINTS[self.peer_cc]}: {detail}')
        if self.path is None:
            expect(int(cerid.get('ecn_ce', '0')) == 0 and picoquic.get('ecn_received_ce') == '0',
                   f'CE on the loopback: {detail}')

    def check_relay(self, relay, cerid, picoquic, detail):
        """The AQM's marks against both ends' counts, and Prague's state, queue and rate through it. A Cerid client
        drives its direction; a Cerid server echoes what its client sends, which loss holds back."""
        toward_peer = 'server' if self.cerid_is_client else 'client'
        toward_cerid = 'client' if self.cerid_is_client else 'server'
        mean = {way: relay.queued[way] / max(1, relay.entered[way]) for way in ('server', 'client')}
        used = {way: round(relay.utilisation(way, SETTLE), 3) for way in ('server', 'client')}
        state = f'relay: codepoints {relay.codepoints}, marked {relay.marked}, tail drops {relay.tail_drops}, ' \
                f'mean queue {mean}, utilisation {used}; {detail}'
        # Each CE an end counts is one the relay marked: picoquic reads Cerid's and Cerid's ACK frames report back
        # what picoquic read, less what was lost after the AQM or what a close overtook.
        ce, read, reported = (int(cerid.get('ecn_ce', '0')), int(picoquic.get('ecn_received_ce', '0')),
                              int(picoquic.get('ecn_acked_ce', '0')))
        expect(ce <= read <= relay.marked[toward_peer], f'CE counts past the marks toward picoquic: {state}')
        expect(reported <= relay.marked[toward_cerid], f'Cerid reported CE never marked: {state}')
        if relay.marked[toward_cerid] > 0:
            expect(reported > 0, f'Cerid reported none of the CE it read: {state}')
        driving = self.cerid_is_client or not self.path.get('loss')
        if driving:
            expect(relay.marked[toward_peer] > 0 and ce > 0, f'no CE reached Cerid\'s ACK frames: {state}')
        expect(relay.tail_drops[toward_peer] <= 0.01 * max(1, relay.entered[toward_peer]),
               f'Prague overflowed the buffer: {state}')
        alpha, classic = int(cerid.get('alpha', '0')), int(cerid.get('classic', '-1'))
        if self.path.get('classic') is not None:
            # Alone in its direction, Prague may hold the classic queue too shallow and steady for the detector, which
            # matters only beside a classic flow (test_prague.cpp proves that); seen at 1 and at 0 in 13 s runs.
            expect(0 <= classic <= CLASSIC_FULL, f'the detector left its range ({classic}): {state}')
            expect(mean[toward_peer] <= 3 * self.path['classic'], f'Prague let the classic queue grow: {state}')
            if self.cerid_is_client:
                expect(used[toward_peer] >= 0.8, f'Prague left the link idle: {state}')
            return
        depth = self.path.get('step') or self.path['ramp'][1]
        # Alpha is 1 until the first CE and then follows the marked share (section 2.3.2); no CE leaves it at 1.
        expect((alpha < ALPHA_ONE) == (ce > 0), f"Prague's alpha did not follow the marks ({alpha}): {state}")
        # The detector reads the round trip's variation (TR-BB-2019-002 section 3.2), so the queue on the way back,
        # where Cerid's acknowledgements wait behind its peer's data, counts too: picoquic's CUBIC, NewReno or BBR
        # (whose probing varies its queue by milliseconds) reads as a classic bottleneck, seen in both roles. Only
        # picoquic's Prague keeps the way back as short as Cerid's own.
        if self.peer_cc == 'prague':
            expect(classic == 0, f'Prague took the L4S step for a classic AQM ({classic}): {state}')
        expect(mean[toward_peer] <= 3 * depth, f'Cerid\'s Prague kept a standing queue: {state}')
        if self.peer_cc == 'prague':
            expect(mean[toward_cerid] <= 3 * depth, f'picoquic\'s Prague kept a standing queue: {state}')
        if self.cerid_is_client and not self.path.get('loss'):
            expect(used[toward_peer] >= 0.8, f'Prague left the link idle: {state}')
        print(f'  {self.describe()}: alpha {alpha} classic {classic} ce {ce}/{read}/{relay.marked[toward_peer]} '
              f'reported {reported}/{relay.marked[toward_cerid]} queue {mean} used {used}', flush=True)


def cases(cerid, picoquic):
    for cerid_is_client in (True, False):
        def case(**settings):
            return Case(cerid, picoquic, cerid_is_client, **settings)

        for version in (1, 2):
            yield case(version=version)
        for peer_cc in ('cubic', 'bbr'):
            yield case(peer_cc=peer_cc)
        for version in (1, 2):
            yield case(version=version, size=BULK, path=L4S_STEP)
        yield case(size=BULK, path=L4S_RAMP)
        yield case(size=1 << 20, path=dict(L4S_STEP, seed=1, loss=0.01))  # Reno's answer to loss: 0.2 of the link
        for peer_cc in ('cubic', 'newreno', 'bbr'):
            yield case(size=BULK, path=L4S_STEP, peer_cc=peer_cc)
        yield case(size=BULK, path=CLASSIC, peer_cc='cubic')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--picoquic', required=True)
    parser.add_argument('--only', default='', help='run only the cases whose description contains this text')
    arguments = parser.parse_args()
    all_cases = [case for case in cases(arguments.cerid, arguments.picoquic) if arguments.only in case.describe()]
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
