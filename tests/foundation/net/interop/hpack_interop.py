#!/usr/bin/env python3
"""crd-net's HPACK against the hpack-test-case corpus (4.2a; docs/design/network-foundation.md#hpack).

Decodes every block of every story of every encoder in the corpus and compares it with the story's fields, then runs
crd-net's encoder over the raw stories and reports its size beside each other encoder's over the same stories.
"""

import argparse
import collections
import json
import subprocess
import sys
import tempfile
from pathlib import Path

TIMEOUT = 120
ENCODERS_AT_DEFAULT_SIZE_ONLY = ('nghttp2-16384-4096', 'nghttp2-change-table-size')


def run(args):
    done = subprocess.run(args, capture_output=True, text=True, errors='replace', timeout=TIMEOUT, check=False)
    return done.returncode, done.stdout + done.stderr


def lines_of(story):
    out = []
    for case in story['cases']:
        if case.get('header_table_size') is not None:
            out.append(f"S {case['header_table_size']}")
        if 'wire' in case:
            out.append(f"W {case['wire']}")
        for field in case['headers']:
            for name, value in field.items():
                out.append(f"F {name.encode().hex()} {value.encode().hex()}")
        out.append('E')
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--replay', required=True, help='crd-hpack-replay')
    parser.add_argument('--corpus', required=True, type=Path, help='the hpack-test-case repository, extracted')
    args = parser.parse_args()

    failures = []
    decoded = collections.Counter()
    sizes = collections.defaultdict(dict)  # story -> {encoder: bytes}

    with tempfile.TemporaryDirectory() as scratch:
        path = Path(scratch) / 'story.txt'
        for encoder in sorted(entry for entry in args.corpus.iterdir() if entry.is_dir()):
            if encoder.name in ('raw-data', 'util') or encoder.name.startswith('.'):
                continue
            for story_file in sorted(encoder.glob('story_*.json')):
                story = json.loads(story_file.read_text(encoding='utf-8'))
                path.write_text(lines_of(story), encoding='utf-8')
                code, output = run([args.replay, 'decode', str(path)])
                if code != 0:
                    failures.append(f'{encoder.name}/{story_file.name}: exit {code}: {output.strip()[:300]}')
                    continue
                decoded[encoder.name] += 1
                sizes[story_file.name][encoder.name] = sum(len(case['wire']) // 2 for case in story['cases'])

        ours = {}
        for story_file in sorted((args.corpus / 'raw-data').glob('story_*.json')):
            story = json.loads(story_file.read_text(encoding='utf-8'))
            for case in story['cases']:
                case.pop('wire', None)
                case.pop('header_table_size', None)
            path.write_text(lines_of(story), encoding='utf-8')
            code, output = run([args.replay, 'encode', str(path)])
            if code != 0:
                failures.append(f'own encoder {story_file.name}: exit {code}: {output.strip()[:300]}')
                continue
            ours[story_file.name] = int(output.split()[0])

    if not decoded:
        print('no corpus file was decoded', file=sys.stderr)
        return 1

    totals = collections.defaultdict(lambda: [0, 0])
    for story, by_encoder in sizes.items():
        if story not in ours:
            continue
        for encoder, bytes_of in by_encoder.items():
            if encoder in ENCODERS_AT_DEFAULT_SIZE_ONLY:
                continue
            totals[encoder][0] += ours[story]
            totals[encoder][1] += bytes_of
    for encoder, (mine, theirs) in sorted(totals.items()):
        print(f'cerid/{encoder:30} {mine}/{theirs} = {mine / theirs:.4f}')

    print(f'decoded: {sum(decoded.values())} stories from {len(decoded)} encoders: {dict(decoded)}')

    for failure in failures:
        print('FAIL', failure, file=sys.stderr)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
