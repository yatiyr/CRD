"""What the interoperability drivers of crd-net share: reading a peer's report and impaired paths over UDP and TCP."""
import heapq
import random
import select
import socket
import struct
import threading
import time


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


CERID_CEILING = 1472  # kQuicDefaultMaxDatagram: Cerid's path MTU search goes no higher
CERID_RECEIVE = 1500  # crd-quic-peer's receive slot by default, which it advertises as max_udp_payload_size
QUIC_MIN = 1200  # every QUIC path carries this (RFC 9000 section 14)


def path_toward(path, toward):
    """The largest datagram a Relay of keyword arguments `path` carries toward 'server' or 'client' at the end."""
    path = path or {}
    fall = path.get('fall')
    return fall[2] if fall and fall[0] == toward else path.get('mtu')


def drops(path):
    """Whether a Relay of keyword arguments `path` may drop datagrams the path MTU allows: random loss, or a
    bottleneck's buffer overflowing."""
    path = path or {}
    return bool(path.get('loss') or path.get('rate'))


def check_cerid_pmtu(report, path, toward_peer, settled, detail, peer_receive=None):
    """Cerid's path MTU search (its report's `pmtu`) against the path toward its peer and the peer's
    max_udp_payload_size (`peer_receive` when the driver knows it): found to the byte when `settled` (a transfer long
    enough for the search to end) over a path that drops none, else a size the path carries; a path that fell was
    found as a black hole."""
    advertised = int(report.get('peer_max_udp_payload', '0'))
    expect(advertised >= QUIC_MIN, f'Cerid read no max_udp_payload_size: {detail}')
    if peer_receive is not None:
        expect(advertised == peer_receive, f'Cerid read max_udp_payload_size {advertised}, not {peer_receive}: '
                                           f'{detail}')
    ceiling = min(CERID_CEILING, advertised, path_toward(path, toward_peer) or CERID_CEILING)
    found = int(report.get('pmtu', '0'))
    if settled and not drops(path):
        expect(found == ceiling, f'Cerid found {found}, not {ceiling}: {detail}')
    else:
        expect(QUIC_MIN <= found <= ceiling, f'Cerid found {found}, more than the path carries: {detail}')
    fall = (path or {}).get('fall')
    if fall and fall[0] == toward_peer:
        expect(int(report.get('black_holes', '0')) >= 1, f'Cerid found no black hole: {detail}')


def check_relay_sizes(relay, path, toward_cerid, receive, detail):
    """No datagram toward Cerid exceeded its receive slot, and over a path with an MTU some probe met it."""
    sizes = f'relay: largest {relay.largest}, dropped {relay.oversized}; {detail}'
    expect(relay.largest[toward_cerid] <= receive, f'a datagram over Cerid\'s max_udp_payload_size: {sizes}')
    if path_toward(path, 'server') or path_toward(path, 'client'):
        expect(sum(relay.oversized.values()) > 0, f'no probe met the path MTU: {sizes}')


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
    """The path between the two: a socket facing the client, one facing the server, and seeded impairments.

    Every datagram leaves the relay with its ECN codepoint rewritten (RFC 3168): unmarked by default, a bleaching path;
    with `mark` 'ce' each is marked CE, a congested AQM, and with 'ect1' each is re-marked ECT(1). With `mark` 'keep'
    each leaves with the codepoint it came with, counted by codepoint in `codepoints`, and a bottleneck's queue is an
    AQM: past `step` seconds of queueing delay an ECT(1) datagram leaves CE (an L4S step, RFC 9332 section 2.5.1.2),
    `ramp` = (low, high) marks one with a probability rising from 0 at `low` seconds to 1 at `high`, and past `classic`
    seconds ECT(0) and ECT(1) alike leave CE, a classic single-queue AQM (RFC 3168). `marked` counts the datagrams
    turned CE each way.

    `mtu` is the largest UDP payload the path carries each way: a longer datagram is dropped, as a link that cannot
    fragment drops it, and counted. `fall` = (toward, after, mtu) lowers it toward 'server' or 'client' alone once that
    many datagrams have crossed that way, a route change onto a tunnel that no ICMP reports. `largest` keeps the longest
    datagram that crossed each way.

    `rate` in bytes per second makes each way a bottleneck: a datagram leaves after those queued before it, and one
    that would take the queue past `buffer` bytes is dropped (drop-tail) and counted in `tail_drops`. `queued` sums
    the seconds each datagram that entered waited, over `entered` of them, so their mean is the standing queue.
    `utilisation` is the share of the link's rate that full datagrams (data, not acknowledgements) used each way, from
    `after` seconds past the first that entered until the last left."""

    FULL = 1000  # bytes from which a datagram counts as carrying data

    MARKS = {None: 0, 'ect1': 1, 'ce': 3}
    ECT1, ECT0, CE = 1, 2, 3
    RECVTOS = getattr(socket, 'IP_RECVTOS', 13)  # Linux's value, where Python does not name it

    def __init__(self, server_port, seed=0, loss=0.0, duplicate=0.0, delay=0.0, jitter=0.0, mark=None, mtu=None,
                 fall=None, rate=None, buffer=None, step=None, ramp=None, classic=None):
        expect((rate is None) == (buffer is None), 'a bottleneck needs a rate and a buffer')
        aqm = step is not None or ramp is not None or classic is not None
        expect(not aqm or (mark == 'keep' and rate is not None), 'an AQM marks kept codepoints at a bottleneck')
        self.keep = mark == 'keep'
        self.step = step
        self.ramp = ramp
        self.classic = classic
        self.codepoints = {'server': [0, 0, 0, 0], 'client': [0, 0, 0, 0]}
        self.marked = {'server': 0, 'client': 0}
        self.server = ('127.0.0.1', server_port)
        self.mtu = {'server': mtu, 'client': mtu}
        self.fall = fall
        self.rate = rate
        self.buffer = buffer
        self.link_free = {'server': 0.0, 'client': 0.0}
        self.tail_drops = {'server': 0, 'client': 0}
        self.queued = {'server': 0.0, 'client': 0.0}
        self.entered = {'server': 0, 'client': 0}
        self.full = {'server': [], 'client': []}  # each full datagram's start on the link, departure and bytes
        self.crossed = {'server': 0, 'client': 0}
        self.largest = {'server': 0, 'client': 0}
        self.oversized = {'server': 0, 'client': 0}
        self.random = random.Random(seed)
        self.loss = loss
        self.duplicate = duplicate
        self.delay = delay
        self.jitter = jitter
        self.client_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client_side.bind(('127.0.0.1', 0))
        self.server_side = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.server_side.bind(('127.0.0.1', 0))
        for side in (self.client_side, self.server_side):
            if self.keep:
                side.setsockopt(socket.IPPROTO_IP, self.RECVTOS, 1)
            else:
                side.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, self.MARKS[mark])
        self.port = self.client_side.getsockname()[1]
        self.client = None
        self.retries = 0
        self.negotiations = 0
        self.running = True
        self.heap = []
        self.order = 0
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def utilisation(self, toward, after=0.0):
        full = self.full[toward]
        if not full:
            return 0.0
        begin = full[0][0] + after
        carried = sum(size for start, _, size in full if start >= begin)
        last = full[-1][1]
        return carried / (self.rate * (last - begin)) if last > begin else 0.0

    def congested(self, codepoint, waited):
        """Whether the AQM marks CE a datagram of `codepoint` that waited `waited` seconds in the queue."""
        if codepoint == self.ECT1:
            if self.step is not None and waited > self.step:
                return True
            if self.ramp is not None:
                low, high = self.ramp
                if self.random.random() < min(1.0, max(0.0, (waited - low) / (high - low))):
                    return True
        return codepoint in (self.ECT0, self.ECT1) and self.classic is not None and waited > self.classic

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

    def carries(self, toward, datagram):
        """Whether the path carries `datagram` toward 'server' or 'client', before any impairment."""
        if self.fall is not None and self.fall[0] == toward and self.crossed[toward] == self.fall[1]:
            self.mtu[toward] = self.fall[2]
        self.crossed[toward] += 1
        if self.mtu[toward] is not None and len(datagram) > self.mtu[toward]:
            self.oversized[toward] += 1
            return False
        self.largest[toward] = max(self.largest[toward], len(datagram))
        return True

    def schedule(self, sender, datagram, destination, codepoint=0):
        toward = 'server' if destination == self.server else 'client'
        self.codepoints[toward][codepoint] += 1
        if not self.carries(toward, datagram) or self.random.random() < self.loss:
            return
        leaves = time.monotonic()
        if self.rate is not None:
            start = max(leaves, self.link_free[toward])
            if (start - leaves) * self.rate + len(datagram) > self.buffer:
                self.tail_drops[toward] += 1
                return
            if self.keep and self.congested(codepoint, start - leaves):
                codepoint = self.CE
                self.marked[toward] += 1
            self.queued[toward] += start - leaves
            self.entered[toward] += 1
            leaves = self.link_free[toward] = start + len(datagram) / self.rate
            if len(datagram) >= self.FULL:
                self.full[toward].append((start, leaves, len(datagram)))
        copies = 2 if self.random.random() < self.duplicate else 1
        for _ in range(copies):
            due = leaves + self.delay + (self.random.random() * self.jitter)
            self.order += 1
            heapq.heappush(self.heap, (due, self.order, sender, datagram, destination, codepoint))

    def receive(self, side):
        """A datagram, its sender and, on a path that keeps codepoints, its ECN codepoint."""
        if not self.keep:
            datagram, sender = side.recvfrom(65536)
            return datagram, sender, 0
        datagram, ancillary, _, sender = side.recvmsg(65536, socket.CMSG_SPACE(4))
        codepoint = 0
        for level, kind, data in ancillary:
            if level == socket.IPPROTO_IP and kind == socket.IP_TOS and data:
                codepoint = data[0] & 3
        return datagram, sender, codepoint

    def send(self, sender, datagram, destination, codepoint):
        if self.keep:
            sender.sendmsg([datagram], [(socket.IPPROTO_IP, socket.IP_TOS, struct.pack('i', codepoint))], 0,
                           destination)
        else:
            sender.sendto(datagram, destination)

    def run(self):
        sockets = [self.client_side, self.server_side]
        while self.running:
            wait = max(0.0, self.heap[0][0] - time.monotonic()) if self.heap else 0.05
            ready, _, _ = select.select(sockets, [], [], min(wait, 0.05))
            for side in ready:
                datagram, sender, codepoint = self.receive(side)
                if side is self.client_side:
                    self.client = sender
                    self.schedule(self.server_side, datagram, self.server, codepoint)
                elif self.client is not None:
                    self.retries += 1 if self.is_retry(datagram) else 0
                    self.negotiations += 1 if self.is_version_negotiation(datagram) else 0
                    self.schedule(self.client_side, datagram, self.client, codepoint)
            while self.heap and self.heap[0][0] <= time.monotonic():
                _, _, sender, datagram, destination, codepoint = heapq.heappop(self.heap)
                self.send(sender, datagram, destination, codepoint)

    def stop(self):
        self.running = False
        self.thread.join(5)
        self.client_side.close()
        self.server_side.close()


class Chopper:
    """A TCP path that hands the bytes on a few at a time, at seeded boundaries."""

    def __init__(self, server_port, seed=0, piece=16):
        self.server = ('127.0.0.1', server_port)
        self.seed = seed
        self.piece = piece
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.bind(('127.0.0.1', 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.sockets = [self.listener]
        self.pumps = []
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        client, _ = self.listener.accept()
        server = socket.create_connection(self.server)
        for side in (client, server):
            side.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.sockets.append(side)
        for number, (source, target) in enumerate(((client, server), (server, client))):
            pump = threading.Thread(target=self.pump, args=(source, target, random.Random(self.seed + number)),
                                    daemon=True)
            pump.start()
            self.pumps.append(pump)

    def pump(self, source, target, chance):
        try:
            while True:
                data = source.recv(65536)
                if not data:
                    target.shutdown(socket.SHUT_WR)
                    return
                at = 0
                while at < len(data):
                    size = chance.randint(1, self.piece)
                    target.sendall(data[at:at + size])
                    at += size
        except OSError:
            return

    def stop(self):
        self.thread.join(1)
        for pump in self.pumps:
            pump.join(1)
        for side in self.sockets:
            side.close()
