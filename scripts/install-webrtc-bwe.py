#!/usr/bin/env python3
"""Install the pinned source files of libwebrtc's bandwidth estimators (tools/webrtc-bwe-sources in cmake/pins.json).

webrtc.googlesource.com regenerates its source archives with different bytes on every request, so the pin names each
file at a fixed commit through Gitiles' `?format=TEXT`, a stable base64 of the blob; each download is verified by its
SHA-256 and decoded into a staging directory that replaces the destination only when all of them are in, with a stamp
that lets a matching install be reused. Gitiles can answer a burst of requests with a page of its own under status 200,
so a digest mismatch on a download is retried after a wait, a bounded number of times. The destination goes under
`~/.cerid/tools/webrtc-bwe/<commit>` by default.
Prints CRD_WEBRTC_BWE=<directory>, which a CRD_BUILD_ESTIMATORS_VS_WEBRTC configure reads (`--github-env` also appends
it to that file): the oracle and board of hesap-net's estimators (HESAP-NET); never linked or shipped.
Contract: docs/design/pinned-inputs.md.
"""
from pathlib import Path
import argparse
import base64
import json
import shutil
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pins  # noqa: E402

PIN_NAME = 'webrtc-bwe-sources'
STAMP = 'cerid-webrtc-bwe.json'
MISMATCH_WAITS = (30, 120)


def default_destination(pin):
    return Path.home() / '.cerid' / 'tools' / 'webrtc-bwe' / pin['commit'][:12]


def default_download_dir(pin):
    return Path.home() / '.cerid' / 'tools' / 'webrtc-bwe' / 'dl' / pin['commit'][:12]


def fetched(name, spec, download_dir, sleep):
    """The verified download of one file; pins.download has already removed a mismatching one."""
    for wait in MISMATCH_WAITS:
        try:
            return pins.fetch(None, download_dir, url=spec['url'], sha=spec['sha256'], file=name, label=name)
        except pins.PinError as error:
            print(f'{name}: {error}; retrying in {wait} s', file=sys.stderr)
            sleep(wait)
    return pins.fetch(None, download_dir, url=spec['url'], sha=spec['sha256'], file=name, label=name)


def install(destination=None, download_dir=None, pin=None, sleep=time.sleep):
    """Return the source tree under `destination`, fetching and decoding its files unless a matching one is present."""
    pin = pins.entry('tools', PIN_NAME) if pin is None else pin
    destination = Path(destination or default_destination(pin))
    download_dir = Path(download_dir or default_download_dir(pin))
    stamp = {'commit': pin['commit'], 'files': {name: spec['sha256'] for name, spec in pin['files'].items()}}
    stamp_path = destination / STAMP
    if stamp_path.is_file() and json.loads(stamp_path.read_text(encoding='utf-8')) == stamp:
        print(f'CRD_WEBRTC_BWE={destination}')
        return destination
    staging = destination.with_name(destination.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    for name, spec in pin['files'].items():
        encoded = fetched(name, spec, download_dir, sleep)
        member = staging / name
        member.parent.mkdir(parents=True, exist_ok=True)
        member.write_bytes(base64.b64decode(encoded.read_bytes(), validate=True))
    shutil.rmtree(destination, ignore_errors=True)
    staging.replace(destination)
    stamp_path.write_text(json.dumps(stamp, indent=1) + '\n', encoding='utf-8')
    print(f'Installed libwebrtc {pin["version"]} estimators; {len(pin["files"])} files verified by SHA-256.')
    print(f'CRD_WEBRTC_BWE={destination}')
    return destination


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--destination', type=Path, help='directory receiving the tree '
                        '(default: ~/.cerid/tools/webrtc-bwe/<commit>)')
    parser.add_argument('--download-dir', type=Path, help='where the verified downloads are kept (a file already '
                        'there is verified instead of downloaded)')
    parser.add_argument('--github-env', type=Path, help='append CRD_WEBRTC_BWE=<directory> to this file (GitHub Actions)')
    args = parser.parse_args(argv)
    tree = install(args.destination, args.download_dir)
    if args.github_env:
        with args.github_env.open('a', encoding='utf-8') as env:
            env.write(f'CRD_WEBRTC_BWE={tree}\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
