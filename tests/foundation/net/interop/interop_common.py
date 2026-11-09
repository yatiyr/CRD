"""What the interoperability drivers of crd-net share: reading a peer's report and an impaired path over UDP."""
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
    """The path between the two: a socket facing the client, one facing the server, and seeded impairments."""

    def __init__(self, server_port, seed=0, loss=0.0, duplicate=0.0, delay=0.0, jitter=0.0):
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
