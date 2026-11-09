"""What the interoperability drivers of crd-net (quic_interop.py, qmux_interop.py) share: reading a peer's report."""


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
