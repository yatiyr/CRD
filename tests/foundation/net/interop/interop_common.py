"""What the interoperability drivers of crd-net share: reading a peer's report and impaired paths over UDP and TCP."""
import heapq
import random
import select
import socket
import threading
import time


def expect(condition, message):
    if not condition:
        raise AssertionError(message)


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
    with `mark` 'ce' each is marked CE, a congested AQM, and with 'ect1' each is re-marked ECT(1)."""

    MARKS = {None: 0, 'ect1': 1, 'ce': 3}

    def __init__(self, server_port, seed=0, loss=0.0, duplicate=0.0, delay=0.0, jitter=0.0, mark=None):
        self.server = ('127.0.0.1', server_port)
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

    def schedule(self, sender, datagram, destination):
        if self.random.random() < self.loss:
            return
        copies = 2 if self.random.random() < self.duplicate else 1
        for _ in range(copies):
            due = time.monotonic() + self.delay + (self.random.random() * self.jitter)
            self.order += 1
            heapq.heappush(self.heap, (due, self.order, sender, datagram, destination))

    def run(self):
        sockets = [self.client_side, self.server_side]
        while self.running:
            wait = max(0.0, self.heap[0][0] - time.monotonic()) if self.heap else 0.05
            ready, _, _ = select.select(sockets, [], [], min(wait, 0.05))
            for side in ready:
                datagram, sender = side.recvfrom(65536)
                if side is self.client_side:
                    self.client = sender
                    self.schedule(self.server_side, datagram, self.server)
                elif self.client is not None:
                    self.retries += 1 if self.is_retry(datagram) else 0
                    self.negotiations += 1 if self.is_version_negotiation(datagram) else 0
                    self.schedule(self.client_side, datagram, self.client)
            while self.heap and self.heap[0][0] <= time.monotonic():
                _, _, sender, datagram, destination = heapq.heappop(self.heap)
                sender.sendto(datagram, destination)

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
