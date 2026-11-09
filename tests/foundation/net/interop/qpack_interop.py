#!/usr/bin/env python3
"""crd-net's QPACK against the qifs corpus (4.2a; docs/design/network-foundation.md#qpack).

Decodes what each encoder in the corpus wrote (encoded/qpack-06/<encoder>/<qif>.out.<capacity>.<blocked>.<ack>) and
compares it with the QIF text; requires a refusal of each file in encoded/errors; runs crd-net's own encoder over every
QIF at the corpus's parameters and reports its size beside each other encoder's.
"""

import argparse
import collections
import subprocess
import sys
from pathlib import Path

RECORD = 12  # the file format's bytes per block: the stream ID and the length
TIMEOUT = 120


def run(args):
    done = subprocess.run(args, capture_output=True, text=True, errors='replace', timeout=TIMEOUT, check=False)
    return done.returncode, done.stdout + done.stderr


def block_bytes(path):
    """The bytes of the sections and instructions in a corpus file, without its framing."""
    data = path.read_bytes()
    total = 0
    at = 0
    while at + RECORD <= len(data):
        length = int.from_bytes(data[at + 8:at + 12], 'big')
        at += RECORD + length
        total += length
    return total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--replay', required=True, help='crd-qpack-replay')
    parser.add_argument('--corpus', required=True, type=Path, help='the qifs repository, extracted')
    args = parser.parse_args()

    encoded_root = args.corpus / 'encoded/qpack-06'
    qifs = args.corpus / 'qifs'
    failures = []
    decoded = collections.Counter()
    sizes = collections.defaultdict(dict)  # (qif, capacity, blocked, ack) -> {encoder: bytes}
    params = set()

    for encoder in sorted(path for path in encoded_root.iterdir() if path.is_dir()):
        for path in sorted(encoder.iterdir()):
            parts = path.name.split('.')
            if len(parts) != 5 or parts[1] != 'out':
                continue
            name, _, capacity, blocked, ack = parts
            qif = qifs / f'{name}.qif'
            if not qif.exists():
                continue
            code, output = run([args.replay, 'decode', str(qif), str(path), capacity, blocked])
            if code != 0:
                failures.append(f'{encoder.name}/{path.name}: exit {code}: {output.strip()[:300]}')
                continue
            decoded[encoder.name] += 1
            sizes[(name, int(capacity), int(blocked), int(ack))][encoder.name] = block_bytes(path)
            params.add((name, capacity, blocked, ack))

    if not decoded:
        print('no corpus file was decoded', file=sys.stderr)
        return 1

    for path in sorted((args.corpus / 'encoded/errors').iterdir()):
        # err9 and err10 index the 61-entry static table of early drafts; RFC 9204's has 99 and they are valid.
        if path.name in ('err9', 'err10'):
            continue
        code, output = run([args.replay, 'refuse', str(path), '4096', '100'])
        if code != 0:
            failures.append(f'errors/{path.name}: not refused (exit {code}): {output.strip()[:200]}')

    ours = {}
    for (name, capacity, blocked, ack) in sorted(sizes):
        code, output = run([args.replay, 'encode', str(qifs / f'{name}.qif'), str(capacity), str(blocked), str(ack)])
        if code != 0:
            failures.append(f'own encoder {name} {capacity} {blocked} {ack}: exit {code}: {output.strip()[:300]}')
            continue
        section, stream = (int(field) for field in output.split())
        ours[(name, capacity, blocked, ack)] = section + stream

    # Each encoder beside crd-net's over exactly the runs that encoder has, overall and by mode.
    groups = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
    for key, bytes_by_encoder in sizes.items():
        if key not in ours:
            continue
        _, capacity, blocked, ack = key
        for encoder, bytes_of in bytes_by_encoder.items():
            for group in ('all', f'capacity {capacity}', f'blocked {blocked} ack {ack}'):
                groups[encoder][group][0] += ours[key]
                groups[encoder][group][1] += bytes_of
    for encoder, rows in sorted(groups.items()):
        cells = '  '.join(f'{group}: {mine}/{theirs} = {mine / theirs:.4f}'
                          for group, (mine, theirs) in sorted(rows.items()))
        print(f'cerid/{encoder:10} {cells}')

    print(f'decoded: {sum(decoded.values())} files from {len(decoded)} encoders: {dict(decoded)}')

    for failure in failures:
        print('FAIL', failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
