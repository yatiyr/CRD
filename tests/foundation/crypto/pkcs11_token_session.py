#!/usr/bin/env python3
"""Run crd-crypto-tests' PKCS #11 cases against a token of their own in the pinned SoftHSM.

The token (scripts/install-softhsm.py) lives in a temporary directory that a SoftHSM configuration of its own names
(SOFTHSM2_CONF); softhsm2-util initialises it with a random SO PIN and user PIN, test values gone with the directory.
The tests reach it through CRD_PKCS11_MODULE, CRD_PKCS11_TOKEN and CRD_PKCS11_PIN, so no system token or
configuration is touched and nothing survives the run.

  1. "[key-store]": every key store case, the PKCS #11 device's among them, against the fresh token; afterwards the
     token must hold no object: every key a case made was destroyed by its store's revocation, erasure or forget
  2. "[.pkcs11-wrong-pin]" with a wrong PIN, through OpenSC's pkcs11-spy.so, which records every call it passes to
     SoftHSM: the device's calls are all Denied, and the recording holds exactly one C_Login, refused

The result is the first failing run's exit code.
Contract: docs/design/network-foundation.md#key-store (decision K14).
"""
from pathlib import Path
import argparse
import os
import re
import secrets
import subprocess
import sys
import tempfile

SECONDS = 600
LABEL = 'cerid-test'


def prefix_libraries(util):
    return Path(util).parents[2] / 'usr' / 'lib' / 'x86_64-linux-gnu'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--softhsm', required=True, help='the pinned softhsm2-util (scripts/install-softhsm.py)')
    parser.add_argument('tests', nargs=argparse.REMAINDER, help='-- crd-crypto-tests [arguments]')
    args = parser.parse_args(argv)
    tests = args.tests[1:] if args.tests[:1] == ['--'] else args.tests
    if not tests:
        parser.error('name the test program after --')
    util = str(Path(args.softhsm).resolve())
    libraries = prefix_libraries(util)
    module = libraries / 'softhsm' / 'libsofthsm2.so'
    spy = libraries / 'pkcs11-spy.so'
    with tempfile.TemporaryDirectory(prefix='crd-pkcs11-') as scratch:
        tokens = Path(scratch) / 'tokens'
        tokens.mkdir()
        config = Path(scratch) / 'softhsm2.conf'
        config.write_text(f'directories.tokendir = {tokens}\nobjectstore.backend = file\nlog.level = ERROR\n'
                          'slots.removable = false\n', encoding='utf-8')
        env = dict(os.environ, SOFTHSM2_CONF=str(config))
        pin = secrets.token_hex(8)
        made = subprocess.run([util, '--module', str(module), '--init-token', '--free', '--label', LABEL,
                               '--so-pin', secrets.token_hex(8), '--pin', pin], env=env, timeout=60,
                              capture_output=True, text=True)
        if made.returncode != 0:
            print(f'pkcs11-token-session: softhsm2-util made no token: {made.stderr.strip()}', file=sys.stderr)
            return made.returncode
        env.update(CRD_PKCS11_MODULE=str(module), CRD_PKCS11_TOKEN=LABEL, CRD_PKCS11_PIN=pin)
        result = subprocess.run(tests + ['[key-store]'], env=env, timeout=SECONDS).returncode
        if result != 0:
            return result
        left = [path.name for path in tokens.rglob('*.object') if path.name != 'token.object']
        if left:
            print(f'pkcs11-token-session: {len(left)} objects left in the token', file=sys.stderr)
            return 1
        recording = Path(scratch) / 'spy.log'
        spied = dict(env, CRD_PKCS11_MODULE=str(spy), PKCS11SPY=str(module), PKCS11SPY_OUTPUT=str(recording),
                     CRD_PKCS11_PIN=pin[::-1] if pin[::-1] != pin else pin + '0')
        result = subprocess.run(tests + ['[.pkcs11-wrong-pin]'], env=spied, timeout=SECONDS).returncode
        if result != 0:
            return result
        calls = recording.read_text(encoding='utf-8', errors='replace') if recording.is_file() else ''
        logins = len(re.findall(r'^\d+: C_Login\b', calls, re.MULTILINE))
        if logins != 1 or 'CKR_PIN_INCORRECT' not in calls:
            print(f'pkcs11-token-session: {logins} logins recorded, not one refused', file=sys.stderr)
            return 1
        print(f'pkcs11-token-session: the token left empty; a wrong PIN tried once ({len(calls)} bytes recorded)')
        return 0


if __name__ == '__main__':
    sys.exit(main())
