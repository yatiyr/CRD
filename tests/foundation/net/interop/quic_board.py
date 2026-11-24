#!/usr/bin/env python3
"""The QUIC transport budget board (NET-PATH): crd-net against msquic, each against itself on the loopback.

Each arm is one implementation's board program as server and client (cerid_quic_board.cpp, msquic_quic_board.c), in
their own processes over real UDP, with equal settings: QUIC v1, AES-128-GCM, secp256r1, the server's Ed25519
certificate pinned by the client, CUBIC, one stream's receive window `window` and the connection's 16 of them. Three
workloads every arm runs, and a fourth only crd-net's program has:

  handshakes  a client runs `connections` connections, `parallel` under way at once, each writing an empty stream
              with a FIN, which the server answers with its FIN, and closing once that came: the answer follows both
              ends' handshake, which a close at the client's own completion would not wait for
  bytes       one connection, the client writes `bytes` bytes and a FIN on one stream from a prefilled buffer, the
              server counts and drops them and answers with its FIN
  memory      the handshakes' connections, `parallel` started at once, but the client holds each open, idle, once its
              FIN came, and closes them all once every one's has (--hold): every end must report that all
              `connections` were live at once (`peak_live`); the cost is each end's peak resident set (os.wait4's
              ru_maxrss) per held connection, an idle established one with its stream's buffers as its exchange left
              them
  bursts      one connection, the client writes `burst_bytes` and a FIN on a new stream `bursts` times, `gap` apart
              (from the answer's FIN to the next stream), and the server answers each with its FIN: at a gap past three
              PTOs an idle connection gives back what it grew (its closed streams' buffers, its sent-packet tables) and
              takes it again for the next burst; at a gap below, it keeps them. The client reports the bursts it began
              with its stream buffers given back (`trimmed`) and each end the `allocations` its connection took after
              the second burst: at the short gap no burst may begin trimmed, and the allocations stay below one per
              burst (a kept buffer may still grow to its high-water mark late); at the long one every burst after the
              first begins trimmed and each end retakes at least one allocation per burst, for the arms built from this
              tree. The retake's cost is the long gap's CPU per burst over the short's, each round, and with --baseline
              (a build without the trim) that difference over the baseline's

The driver measures from outside: each process's user and system CPU time (os.wait4) and the client's wall time. Each
workload runs at two sizes, and the cost is the slope between them, (cost at the larger minus cost at the smaller) over
the difference in work, so process start, credential set-up and thread pools cancel. CPU is summed over both ends:
on the loopback the receiver's kernel work is billed to whichever process runs, so one end's share is not comparable.
Each end also reports its datagrams and the messages that carried them each way (a message is one datagram or one
segmented or coalesced train, which the host's stack carries as one buffer): the bulk table gives them per MiB, from
the same slope, so the kernel's share of a byte's cost reads as datagrams per message and datagrams per acknowledgement
(the server sends almost nothing but ACKs). crd-net's ends also report their loop's waits for the next event (how
many, those their timeout ended, the time asked and taken), per MiB in a third table: an end idle while the other works
is wall time the CPU columns do not show. `--baseline` adds another build's crd-quic-board as its own offload arm, for
a before and after on one host; `--workload` runs one workload alone. The arms run interleaved, `rounds` times, and
the board keeps each arm's best round and its median. Every run must
report every connection established, every byte delivered, and AES-128-GCM and version 1 on every connection; crd-net
also reports its group (msquic's handshake information leaves the group out; its OpenSSL list puts secp256r1 first).
"""
WORKLOADS = ('handshakes', 'bytes', 'memory', 'bursts')
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
COUNTS = ('datagrams_sent', 'send_messages', 'datagrams_received', 'receive_messages')
WAITS = ('waits', 'wait_timeouts', 'wait_asked_ns', 'wait_ns')  # crd-net's loop only
OWN = WAITS + ('exchanges', 'allocations', 'trimmed')  # crd-net's program only


class Arm:
    def __init__(self, name, program, offload=None, group=True, trims=False):
        self.name = name
        self.program = program
        self.offload = offload  # None: the program has no switch, and is not crd-net's
        self.group = group      # the program takes --group and reports it
        self.trims = trims      # built from this tree: its idle connection gives back what it grew

    def command(self, role, port, connections, parallel, size, window, hold, bursts=1, gap=0):
        command = [self.program, '--role', role, '--port', str(port), '--seed', SEED, '--connections',
                   str(connections), '--window', str(window)]
        if role == 'client':
            command += ['--parallel', str(parallel), '--bytes', str(size)] + (['--hold', '1'] if hold else [])
            command += ['--bursts', str(bursts), '--gap', str(gap)] if bursts > 1 else []
        if self.group:
            command += ['--group', GROUP]
        if self.offload is not None:
            command += ['--offload', '1' if self.offload else '0']
        return command


def report_of(text):
    return dict(line.split('=', 1) for line in text.splitlines() if '=' in line)


def cpu_of(usage):
    return usage.ru_utime + usage.ru_stime


def run(arm, connections, parallel, size, window, hold=False, bursts=1, gap=0):
    """One server and one client of `arm`: (client CPU, server CPU, client wall seconds, counts), the counts by
    (role, COUNTS or OWN name, or `peak_kib`: the end's peak resident set in KiB)."""
    with tempfile.TemporaryFile() as server_err, tempfile.TemporaryFile() as client_err:
        server = subprocess.Popen(arm.command('server', 0, connections, parallel, size, window, hold, bursts, gap),
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
                                                  window, hold, bursts, gap),
                                      stdout=subprocess.PIPE, stderr=client_err, text=True)
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
        check(arm, (client_status, server_status), reports, connections, size, hold, bursts, detail)
        counts = {(role, key): int(report.get(key, 0)) for role, report in zip(('client', 'server'), reports)
                  for key in COUNTS + OWN}
        counts[('client', 'peak_kib')] = client_usage.ru_maxrss  # KiB on Linux
        counts[('server', 'peak_kib')] = server_usage.ru_maxrss
        return cpu_of(client_usage), cpu_of(server_usage), wall, counts


def check(arm, statuses, reports, connections, size, hold, bursts, detail):
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
        for key in COUNTS + (OWN if arm.offload is not None else ()):
            if not report.get(key, '').isdigit():
                raise AssertionError(f'{arm.name}: the {name} reported {key}={report.get(key)}')
        if arm.offload is not None and report.get('exchanges') != str(connections * bursts):
            raise AssertionError(f'{arm.name}: the {name} did {report.get("exchanges")} exchanges, not '
                                 f'{connections * bursts}')
        if hold and report.get('peak_live') != str(connections):
            raise AssertionError(f'{arm.name}: the {name} held {report.get("peak_live")} connections at once, not '
                                 f'{connections}: {detail}')
    if server.get('refused', '0') != '0':
        raise AssertionError(f'{arm.name}: the server refused {server["refused"]} connections')
    if client.get('bytes_sent') != str(size * bursts) or server.get('bytes_received') != str(size * bursts):
        raise AssertionError(f'{arm.name}: {client.get("bytes_sent")} bytes sent, {server.get("bytes_received")} '
                             f'received, not {size * bursts}')


def slope(arm, small, large, parallel, window, workload, burst_bytes=0, gap=0):
    """Per unit of work between the two sizes: (CPU both ends, server CPU, client CPU, wall) in seconds, the counts
    per unit, then each end's peak resident set at the larger size in KiB (server, client)."""
    points = []
    for amount in (small, large):
        if workload == 'bytes':
            points.append(run(arm, 1, 1, amount, window))
        elif workload == 'bursts':
            points.append(run(arm, 1, 1, burst_bytes, window, bursts=amount, gap=gap))
        else:
            points.append(run(arm, amount, parallel, 0, window, hold=workload == 'memory'))
    (client0, server0, wall0, counts0), (client1, server1, wall1, counts1) = points
    units = large - small
    return ((client1 + server1 - client0 - server0) / units, (server1 - server0) / units, (client1 - client0) / units,
            (wall1 - wall0) / units, {key: (counts1[key] - counts0[key]) / units for key in counts1},
            (counts1[('server', 'peak_kib')], counts1[('client', 'peak_kib')]))


def board(arms, rounds, sizes, parallel, window, burst_bytes, gaps):
    """Each arm's slopes by (arm name, workload), the bursts' by (arm name, ('bursts', gap)), one per round."""
    results = {}
    for round_index in range(rounds):
        for workload, (small, large) in sizes.items():
            for gap in (gaps if workload == 'bursts' else (0,)):
                key = ('bursts', gap) if workload == 'bursts' else workload
                for arm in arms:
                    if workload == 'bursts' and arm.offload is None:
                        continue
                    row = slope(arm, small, large, parallel, window, workload, burst_bytes, gap)
                    if workload == 'bursts':
                        check_allocations(arm, gap, gaps, row[4])
                    results.setdefault((arm.name, key), []).append(row)
        print(f'round {round_index + 1} of {rounds} done', file=sys.stderr, flush=True)
    return results


def check_allocations(arm, gap, gaps, counts):
    """The short gap keeps what each end grew (no burst begins trimmed, under one allocation per burst), and the long
    gap's trim gave it back (every burst trimmed, at least one allocation per end per burst) where the arm is built
    from this tree: the two gaps measure what they claim."""
    trimmed = counts[('client', 'trimmed')]
    if gap == gaps[0] and trimmed != 0:
        raise AssertionError(f'{arm.name}: {trimmed:.2f} of the bursts {gap} ms apart began trimmed')
    if gap == gaps[1] and arm.trims and trimmed != 1:
        raise AssertionError(f'{arm.name}: {trimmed:.2f} of the bursts {gap} ms apart began trimmed, not all')
    for role in ('server', 'client'):
        taken = counts[(role, 'allocations')]
        if gap == gaps[0] and taken >= 1:
            raise AssertionError(f'{arm.name}: the {role} took {taken:.2f} allocations per burst {gap} ms apart')
        if gap == gaps[1] and arm.trims and taken < 1:
            raise AssertionError(f'{arm.name}: the {role} took {taken:.2f} allocations per burst {gap} ms apart: '
                                 'nothing was retaken')


def rate(seconds, scale=1):
    """Units per second from seconds per unit; a slope the noise made empty or negative (a short --check) reads n/a."""
    return f'{1 / seconds / scale:,.0f}' if seconds > 0 else 'n/a'


def ratio(base, seconds):
    """An arm's rate over the first arm's."""
    return f'{base / seconds:.2f}' if base > 0 and seconds > 0 else 'n/a'


def kib(value):
    """KiB, n/a where the noise made a slope empty or negative."""
    return f'{value:,.1f}' if value > 0 else 'n/a'


def per(numerator, denominator):
    """One count over another, n/a where the slope left the second empty."""
    return f'{numerator / denominator:.1f}' if denominator > 0 else 'n/a'


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
        lines += ['', '| arm (best round) | data datagrams per MiB | per send message | ACK datagrams per MiB | '
                  'data per ACK | server datagrams per receive message | client datagrams per receive message |',
                  '|---|---|---|---|---|---|---|']
        for arm in arms:
            counts = min(results[(arm.name, 'bytes')], key=lambda r: r[0])[4]
            data, acks = counts[('client', 'datagrams_sent')] * MIB, counts[('server', 'datagrams_sent')] * MIB
            lines.append(f'| {arm.name} | {data:,.0f} | '
                         f'{per(counts[("client", "datagrams_sent")], counts[("client", "send_messages")])} | '
                         f'{acks:,.0f} | {per(data, acks)} | '
                         f'{per(counts[("server", "datagrams_received")], counts[("server", "receive_messages")])} | '
                         f'{per(counts[("client", "datagrams_received")], counts[("client", "receive_messages")])} |')
        lines += ['', '| arm (best round), per MiB | client waits | timed out | asked us | took us | server waits | '
                  'timed out | asked us | took us |', '|---|---|---|---|---|---|---|---|---|']
        for arm in arms:
            if arm.offload is None:
                continue
            counts = min(results[(arm.name, 'bytes')], key=lambda r: r[0])[4]
            cells = []
            for role in ('client', 'server'):
                cells += [f'{counts[(role, "waits")] * MIB:,.0f}', f'{counts[(role, "wait_timeouts")] * MIB:,.0f}',
                          f'{counts[(role, "wait_asked_ns")] * MIB / 1000:,.0f}',
                          f'{counts[(role, "wait_ns")] * MIB / 1000:,.0f}']
            lines.append(f'| {arm.name} | ' + ' | '.join(cells) + ' |')
        lines.append('')
    if 'memory' in sizes:
        held = sizes['memory'][1]
        lines += [f'| arm | KiB per held connection (both ends) | median | server | client | server peak MiB at '
                  f'{held:,} | client peak MiB at {held:,} | vs {base} |', '|---|---|---|---|---|---|---|---|']

        def both(row):
            return row[4][('server', 'peak_kib')] + row[4][('client', 'peak_kib')]

        best_base = min(both(r) for r in results[(base, 'memory')])
        for arm in arms:
            rows = results[(arm.name, 'memory')]
            best = min(rows, key=both)
            median = statistics.median(both(r) for r in rows)
            counts = best[4]
            lines.append(f'| {arm.name} | {kib(both(best))} | {kib(median)} | {kib(counts[("server", "peak_kib")])} | '
                         f'{kib(counts[("client", "peak_kib")])} | {best[5][0] / 1024:,.1f} | {best[5][1] / 1024:,.1f} '
                         f'| {ratio(both(best), best_base)} |')
    if 'bursts' in sizes:
        lines += bursts_table(arms, results, sizes['bursts'])
    return '\n'.join(lines).rstrip()


def bursts_table(arms, results, bursts):
    """Each crd-net arm's cost per burst at each gap, then the retake: the long gap's over the short's, round by
    round, and against the first arm that does not trim (a --baseline build) where there is one."""
    own = [arm for arm in arms if arm.offload is not None]
    gaps = sorted({key[1][1] for key in results if isinstance(key[1], tuple)})
    lines = ['', '| arm | gap ms | CPU us per burst (both ends) | median | server | client | ms per burst past the '
             'gap | loop waits per burst (both ends) | server allocations per burst | client allocations per burst |',
             '|---' * 10 + '|']
    for arm in own:
        for gap in gaps:
            rows = results[(arm.name, ('bursts', gap))]
            best = min(rows, key=lambda r: r[0])
            median = statistics.median(r[0] for r in rows)
            counts = best[4]
            lines.append(f'| {arm.name} | {gap} | {best[0] * 1e6:,.1f} | {median * 1e6:,.1f} | {best[1] * 1e6:,.1f} | '
                         f'{best[2] * 1e6:,.1f} | {(best[3] - gap / 1000) * 1000:,.2f} | '
                         f'{counts[("server", "waits")] + counts[("client", "waits")]:,.1f} | '
                         f'{counts[("server", "allocations")]:.2f} | {counts[("client", "allocations")]:.2f} |')

    def retake(arm):
        short, long = (results[(arm.name, ('bursts', gap))] for gap in (gaps[0], gaps[-1]))
        return [(b[0] - a[0]) * 1e6 for a, b in zip(short, long)]

    lines += ['', f'| arm | CPU us per burst {gaps[-1]} ms apart over {gaps[0]} ms apart, median of rounds | least | '
              'most |', '|---|---|---|---|']
    base = next((arm for arm in own if not arm.trims), None)
    for arm in own:
        values = retake(arm)
        lines.append(f'| {arm.name} | {statistics.median(values):,.1f} | {min(values):,.1f} | {max(values):,.1f} |')
    for arm in (own if base is not None else ()):
        if arm.trims:
            values = [a - b for a, b in zip(retake(arm), retake(base))]
            lines.append(f'| {arm.name} over {base.name} | {statistics.median(values):,.1f} | {min(values):,.1f} | '
                         f'{max(values):,.1f} |')
    lines.append(f'\n{bursts[0]:,} and {bursts[1]:,} bursts a run')
    return lines


def pair(text):
    small, large = (int(value) for value in text.split(','))
    if not 0 < small < large:
        raise argparse.ArgumentTypeError('two sizes, the smaller first')
    return small, large


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cerid', required=True, help='crd-quic-board')
    parser.add_argument('--msquic', help='crd-msquic-quic-board')
    parser.add_argument('--baseline', help="another build's crd-quic-board, run interleaved as its own offload arm "
                        "(built with this tree's cerid_quic_board.cpp: an older program reports no exchanges)")
    parser.add_argument('--workload', choices=('all',) + WORKLOADS, default='all')
    parser.add_argument('--rounds', type=int, default=5)
    parser.add_argument('--handshakes', type=pair, default=(200, 2000), help='connections at the two sizes')
    parser.add_argument('--bytes', type=pair, default=(64 * MIB, 1024 * MIB), help='bytes at the two sizes')
    parser.add_argument('--memory', type=pair, default=(200, 2000), help='held connections at the two sizes')
    parser.add_argument('--bursts', type=pair, default=(10, 50), help='bursts at the two sizes')
    parser.add_argument('--burst-bytes', type=int, default=4096, help="a burst's bytes")
    parser.add_argument('--gaps', type=pair, default=(20, 300),
                        help='ms between bursts: below three PTOs (kept), then past them (trimmed)')
    parser.add_argument('--parallel', type=int, default=16, help='connections under way at once')
    parser.add_argument('--window', type=int, default=MIB, help="a stream's receive window")
    parser.add_argument('--check', action='store_true',
                        help='one short round: every arm completes with equal parameters (the CTest)')
    arguments = parser.parse_args()
    if arguments.check:
        arguments.rounds, arguments.handshakes, arguments.bytes, arguments.memory = 1, (8, 24), (MIB, 4 * MIB), (8, 24)
        arguments.bursts = (2, 4)
    arms = [Arm('crd-net', arguments.cerid, offload=True, trims=True),
            Arm('crd-net, no offload', arguments.cerid, offload=False, trims=True)]
    if arguments.baseline:
        arms.insert(1, Arm('crd-net baseline', arguments.baseline, offload=True))
    if arguments.msquic:
        arms.append(Arm('msquic', arguments.msquic, group=False))
    sizes = {workload: getattr(arguments, workload) for workload in WORKLOADS
             if arguments.workload in ('all', workload)}
    try:
        results = board(arms, arguments.rounds, sizes, arguments.parallel, arguments.window, arguments.burst_bytes,
                        arguments.gaps)
    except AssertionError as error:
        print(f'FAIL {error}')
        return 1
    print(table(arms, results, sizes))
    return 0


if __name__ == '__main__':
    sys.exit(main())
