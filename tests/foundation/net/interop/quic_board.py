#!/usr/bin/env python3
"""The QUIC transport budget board (NET-PATH): crd-net against msquic, each against itself on the loopback.

Each arm is one implementation's board program as server and client (cerid_quic_board.cpp, msquic_quic_board.c), in
their own processes over real UDP, with equal settings: QUIC v1, AES-128-GCM, secp256r1, the server's Ed25519
certificate pinned by the client, CUBIC, one stream's receive window `window` and the connection's 16 of them. Two
workloads:

  handshakes  a client runs `connections` connections, `parallel` under way at once, each writing an empty stream
              with a FIN, which the server answers with its FIN, and closing once that came: the answer follows both
              ends' handshake, which a close at the client's own completion would not wait for
  bytes       one connection, the client writes `bytes` bytes and a FIN on one stream from a prefilled buffer, the
              server counts and drops them and answers with its FIN

The driver measures from outside: each process's user and system CPU time (os.wait4) and the client's wall time. Each
workload runs at two sizes, and the cost is the slope between them, (cost at the larger minus cost at the smaller) over
the difference in work, so process start, credential set-up and thread pools cancel. CPU is summed over both ends:
on the loopback the receiver's kernel work is billed to whichever process runs, so one end's share is not comparable.
The arms run interleaved, `rounds` times, and the board keeps each arm's best round and its median. Every run must
report every connection established, every byte delivered, and AES-128-GCM and version 1 on every connection; crd-net
also reports its group (msquic's handshake information leaves the group out; its OpenSSL list puts secp256r1 first).
"""
import argparse
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time

SEED = '11' * 32
RUN_SECONDS = 600
GROUP = 'secp256r1'
MIB = 1 << 20


class Arm:
    def __init__(self, name, program, offload=None, group=True):
        self.name = name
        self.program = program
        self.offload = offload  # None: the program has no switch
        self.group = group      # the program takes --group and reports it

    def command(self, role, port, connections, parallel, size, window):
        command = [self.program, '--role', role, '--port', str(port), '--seed', SEED, '--connections',
                   str(connections), '--window', str(window)]
        if role == 'client':
            command += ['--parallel', str(parallel), '--bytes', str(size)]
        if self.group:
            command += ['--group', GROUP]
        if self.offload is not None:
            command += ['--offload', '1' if self.offload else '0']
        return command


def report_of(text):
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def cpu_of(usage):
    return usage.ru_utime + usage.ru_stime


def run(arm, connections, parallel, size, window):
    """One server and one client of `arm`: (client CPU, server CPU, client wall seconds)."""
    with tempfile.TemporaryFile() as server_err, tempfile.TemporaryFile() as client_err:
        server = subprocess.Popen(arm.command('server', 0, connections, parallel, size, window),
                                  stdout=subprocess.PIPE, stderr=server_err, text=True)
        client = None
        watchdog = threading.Timer(RUN_SECONDS, lambda: [p.kill() for p in (server, client) if p is not None])
        watchdog.start()
        try:
            first = server.stdout.readline().strip()
            if not first.startswith('port='):
                raise AssertionError(f'{arm.name}: the server printed {first!r}')
            started = time.perf_counter()
            client = subprocess.Popen(arm.command('client', int(first[len('port='):]), connections, parallel, size,
                                                  window), stdout=subprocess.PIPE, stderr=client_err, text=True)
            client_out = client.stdout.read()
            _, client_status, client_usage = os.wait4(client.pid, 0)
            wall = time.perf_counter() - started
            server_out = server.stdout.read()
            _, server_status, server_usage = os.wait4(server.pid, 0)
        finally:
            watchdog.cancel()
        for process in (server, client):
            process.returncode = 0  # os.wait4 reaped it
            process.stdout.close()
        detail = ''
        for name, stream in (('server', server_err), ('client', client_err)):
            stream.seek(0)
            detail += f'{name}: {stream.read().decode(errors="replace").strip()} '
        reports = (report_of(client_out), report_of(server_out))
        check(arm, (client_status, server_status), reports, connections, size, detail)
        return cpu_of(client_usage), cpu_of(server_usage), wall


def check(arm, statuses, reports, connections, size, detail):
    client, server = reports
    if any(os.waitstatus_to_exitcode(status) != 0 for status in statuses):
        raise AssertionError(f'{arm.name}: an end failed: {reports} {detail}')
    for name, report in (('client', client), ('server', server)):
        for key, want in (('connections', connections), ('established', connections)):
            if report.get(key) != str(want):
                raise AssertionError(f'{arm.name}: the {name} reported {key}={report.get(key)}, not {want}')
        for key, want in (('cipher', 'aes128'), ('version', '1'), ('group', GROUP if arm.group else 'unreported')):
            if report.get(key) != want:
                raise AssertionError(f'{arm.name}: the {name} negotiated {key}={report.get(key)}, not {want}')
    if client.get('bytes_sent') != str(size) or server.get('bytes_received') != str(size):
        raise AssertionError(f'{arm.name}: {client.get("bytes_sent")} bytes sent, {server.get("bytes_received")} '
                             f'received, not {size}')


def slope(arm, small, large, parallel, window, workload):
    """Per unit of work between the two sizes: (CPU both ends, server CPU, client CPU, wall) in seconds."""
    points = []
    for amount in (small, large):
        if workload == 'handshakes':
            points.append(run(arm, amount, parallel, 0, window))
        else:
            points.append(run(arm, 1, 1, amount, window))
    (client0, server0, wall0), (client1, server1, wall1) = points
    units = large - small
    return ((client1 + server1 - client0 - server0) / units, (server1 - server0) / units, (client1 - client0) / units,
            (wall1 - wall0) / units)


def board(arms, rounds, sizes, parallel, window):
    results = {(arm.name, workload): [] for arm in arms for workload in sizes}
    for round_index in range(rounds):
        for workload, (small, large) in sizes.items():
            for arm in arms:
                results[(arm.name, workload)].append(slope(arm, small, large, parallel, window, workload))
        print(f'round {round_index + 1} of {rounds} done', file=sys.stderr, flush=True)
    return results


def rate(seconds, scale=1):
    """Units per second from seconds per unit; a slope the noise made empty or negative (a short --check) reads n/a."""
    return f'{1 / seconds / scale:,.0f}' if seconds > 0 else 'n/a'


def ratio(base, seconds):
    """An arm's rate over the first arm's."""
    return f'{base / seconds:.2f}' if base > 0 and seconds > 0 else 'n/a'


def table(arms, results, sizes):
    base = arms[0].name
    lines = []
    if 'handshakes' in sizes:
        lines += ['| arm | handshakes per CPU-second (both ends) | median | server | client | per wall second | '
                  f'vs {base} |', '|---|---|---|---|---|---|---|']
        best_base = min(r[0] for r in results[(base, 'handshakes')])
        for arm in arms:
            rows = results[(arm.name, 'handshakes')]
            best = min(rows, key=lambda r: r[0])
            median = statistics.median(r[0] for r in rows)
            lines.append(f'| {arm.name} | {rate(best[0])} | {rate(median)} | {rate(best[1])} | {rate(best[2])} | '
                         f'{rate(best[3])} | {ratio(best_base, best[0])} |')
        lines.append('')
    if 'bytes' in sizes:
        lines += ['| arm | MiB per CPU-second (both ends) | median | server | client | MiB per wall second | '
                  f'vs {base} |', '|---|---|---|---|---|---|---|']
        best_base = min(r[0] for r in results[(base, 'bytes')])
        for arm in arms:
            rows = results[(arm.name, 'bytes')]
            best = min(rows, key=lambda r: r[0])
            median = statistics.median(r[0] for r in rows)
            lines.append(f'| {arm.name} | {rate(best[0], MIB)} | {rate(median, MIB)} | {rate(best[1], MIB)} | '
                         f'{rate(best[2], MIB)} | {rate(best[3], MIB)} | {ratio(best_base, best[0])} |')
    return '\n'.join(lines)


def pair(text):
    small, large = (int(value) for value in text.split(','))
    if not 0 < small < large:
        raise argparse.ArgumentTypeError('two sizes, the smaller first')
    return small, large


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cerid', required=True, help='crd-quic-board')
    parser.add_argument('--msquic', help='crd-msquic-quic-board')
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--handshakes', type=pair, default=(200, 2000), help='connections at the two sizes')
    parser.add_argument('--bytes', type=pair, default=(64 * MIB, 1024 * MIB), help='bytes at the two sizes')
    parser.add_argument('--parallel', type=int, default=16, help='connections under way at once')
    parser.add_argument('--window', type=int, default=MIB, help="a stream's receive window")
    parser.add_argument('--check', action='store_true',
                        help='one short round: every arm completes with equal parameters (the CTest)')
    arguments = parser.parse_args()
    if arguments.check:
        arguments.rounds, arguments.handshakes, arguments.bytes = 1, (8, 24), (MIB, 4 * MIB)
    arms = [Arm('crd-net', arguments.cerid, offload=True), Arm('crd-net, no offload', arguments.cerid, offload=False)]
    if arguments.msquic:
        arms.append(Arm('msquic', arguments.msquic, group=False))
    sizes = {'handshakes': arguments.handshakes, 'bytes': arguments.bytes}
    try:
        results = board(arms, arguments.rounds, sizes, arguments.parallel, arguments.window)
    except AssertionError as error:
        print(f'FAIL {error}')
        return 1
    print(table(arms, results, sizes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
