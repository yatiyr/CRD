#!/usr/bin/env python3
"""Run crd-crypto-tests' platform vault cases against a scratch macOS keychain of their own.

The session creates a keychain file in a temporary directory from a test password, unlocked and without an automatic
lock, and makes it the user's default keychain and whole search list for the run, so no keychain of the user's is
read or written; the previous default and search list are restored and the scratch keychain deleted before it
returns. A test program is unsigned, so the protector keeps master keys in the default file keychain (the
data-protection keychain is a signed application's).

  unlocked  the keychain stays unlocked: crd-crypto-tests "[platform-vault]"
  locked    "[platform-vault-seed]" stores a master key, the keychain is locked, then "[platform-vault-locked]" is
            refused without a prompt
  absent    the default keychain and the search list name a keychain whose file is gone: "[platform-vault-absent]"

The tests find the scratch directory in CRD_PLATFORM_VAULT_SCRATCH; the result is the tests' own exit code.
Contract: docs/design/network-foundation.md#key-store (decision K16).
"""
from pathlib import Path
import argparse
import os
import subprocess
import sys
import tempfile

PASSWORD = 'cerid-test-keychain'  # a test value: the scratch keychain's, gone with the session
SECONDS = 240


def security(*arguments):
    """One call to security(1), with its output."""
    return subprocess.run(['security'] + list(arguments), capture_output=True, text=True, check=True,
                          timeout=30).stdout


def keychains(output):
    """The keychain paths security(1) lists, one quoted path a line."""
    return [line.strip().strip('"') for line in output.splitlines() if line.strip()]


def run(tests, tag, env):
    return subprocess.run(tests + [tag], env=env, timeout=SECONDS).returncode


def session(mode, tests, keychain, env):
    """The mode's test runs with `keychain` the default and only keychain."""
    security('create-keychain', '-p', PASSWORD, str(keychain))
    security('set-keychain-settings', str(keychain))  # no timeout, no lock on sleep
    security('unlock-keychain', '-p', PASSWORD, str(keychain))
    security('list-keychains', '-d', 'user', '-s', str(keychain))
    security('default-keychain', '-d', 'user', '-s', str(keychain))
    if mode == 'unlocked':
        return run(tests, '[platform-vault]', env)
    if mode == 'absent':
        keychain.unlink()
        return run(tests, '[platform-vault-absent]', env)
    seeded = run(tests, '[platform-vault-seed]', env)
    if seeded != 0:
        return seeded
    security('lock-keychain', str(keychain))
    return run(tests, '[platform-vault-locked]', env)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--mode', choices=('unlocked', 'locked', 'absent'), required=True)
    parser.add_argument('tests', nargs=argparse.REMAINDER, help='-- crd-crypto-tests [arguments]')
    args = parser.parse_args(argv)
    tests = args.tests[1:] if args.tests[:1] == ['--'] else args.tests
    if not tests:
        parser.error('name the test program after --')
    default = keychains(security('default-keychain', '-d', 'user'))
    search = keychains(security('list-keychains', '-d', 'user'))
    with tempfile.TemporaryDirectory(prefix='crd-keychain-') as temp:
        scratch = Path(temp)
        keychain = scratch / 'cerid-test.keychain-db'
        env = dict(os.environ, CRD_PLATFORM_VAULT_SCRATCH=str(scratch))
        try:
            return session(args.mode, tests, keychain, env)
        finally:
            security('list-keychains', '-d', 'user', '-s', *search)
            if default:
                security('default-keychain', '-d', 'user', '-s', default[0])
            if keychain.exists():
                security('delete-keychain', str(keychain))


if __name__ == '__main__':
    sys.exit(main())
