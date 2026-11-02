#!/usr/bin/env python3
"""Run crd-crypto-tests' Secret Service cases against the pinned gnome-keyring in a session of their own.

The session is a private D-Bus session bus (dbus-run-session) and a scratch home: HOME, the XDG data, configuration,
cache and runtime directories all lie in one temporary directory, so no user's keyring or bus is touched and nothing
survives the run. Inside it the pinned daemon (scripts/install-gnome-keyring.py, its GCR libraries on
LD_LIBRARY_PATH) creates the login collection from a test password and becomes the Secret Service. No display is
set, so no prompter can answer a prompt: a prompt the service shows is dismissed.

  unlocked  the login collection stays unlocked: crd-crypto-tests "[secret-service]"
  locked    "[secret-service-seed]" stores a master key, the collection is locked over the bus, then
            "[secret-service-locked]" is refused without a prompt and with a dismissed one

The tests find the scratch directory in CRD_SECRET_SERVICE_SCRATCH. Every daemon the session started is stopped
before it returns; the result is the tests' own exit code.
Contract: docs/design/network-foundation.md#key-store (decision K10).
"""
from pathlib import Path
import argparse
import os
import signal
import subprocess
import sys
import tempfile

PASSWORD = b'cerid-test-keyring'  # a test value: the scratch login collection's, gone with the session
LOGIN = '/org/freedesktop/secrets/collection/login'
SECONDS = 240


def bus(*arguments):
    """One call to the Secret Service over the session bus, with its reply."""
    command = ['dbus-send', '--session', '--print-reply', '--dest=org.freedesktop.secrets'] + list(arguments)
    return subprocess.run(command, capture_output=True, text=True, check=True, timeout=30).stdout


def locked():
    reply = bus(LOGIN, 'org.freedesktop.DBus.Properties.Get', 'string:org.freedesktop.Secret.Collection',
                'string:Locked')
    return 'boolean true' in reply


def inner(daemon, mode, tests):
    """Inside the bus: start the daemon with the login collection, then run the mode's test runs."""
    subprocess.run([daemon, '--daemonize', '--login'], input=PASSWORD, stdout=subprocess.DEVNULL, check=True,
                   timeout=30)
    subprocess.run([daemon, '--start', '--components=secrets'], stdout=subprocess.DEVNULL, check=True, timeout=30)
    if locked():
        print('secret-service-session: the login collection did not start unlocked', file=sys.stderr)
        return 1
    if mode == 'unlocked':
        return subprocess.run(tests + ['[secret-service]'], timeout=SECONDS).returncode
    seeded = subprocess.run(tests + ['[secret-service-seed]'], timeout=SECONDS).returncode
    if seeded != 0:
        return seeded
    bus('/org/freedesktop/secrets', 'org.freedesktop.Secret.Service.Lock', 'array:objpath:' + LOGIN)
    if not locked():
        print('secret-service-session: the login collection did not lock', file=sys.stderr)
        return 1
    return subprocess.run(tests + ['[secret-service-locked]'], timeout=SECONDS).returncode


def stop_daemons(daemon, scratch):
    """Stops every process of the pinned daemon whose environment names this session's scratch directory."""
    marker = ('CRD_SECRET_SERVICE_SCRATCH=' + str(scratch)).encode()
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if os.path.realpath(entry / 'exe') != os.path.realpath(daemon):
                continue
            if marker not in (entry / 'environ').read_bytes().split(b'\0'):
                continue
            os.kill(int(entry.name), signal.SIGTERM)
        except (OSError, ValueError):
            continue


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--daemon', required=True, help='the pinned gnome-keyring-daemon')
    parser.add_argument('--mode', choices=('unlocked', 'locked'), required=True)
    parser.add_argument('--inner', action='store_true', help=argparse.SUPPRESS)
    parser.add_argument('tests', nargs=argparse.REMAINDER, help='-- crd-crypto-tests [arguments]')
    args = parser.parse_args(argv)
    tests = args.tests[1:] if args.tests[:1] == ['--'] else args.tests
    if not tests:
        parser.error('name the test program after --')
    daemon = str(Path(args.daemon).resolve())
    if args.inner:
        return inner(daemon, args.mode, tests)
    with tempfile.TemporaryDirectory(prefix='crd-secret-service-') as temp:
        scratch = Path(temp)
        env = {name: value for name, value in os.environ.items()
               if name not in ('DBUS_SESSION_BUS_ADDRESS', 'DISPLAY', 'WAYLAND_DISPLAY', 'GNOME_KEYRING_CONTROL')}
        for name, leaf in (('HOME', 'home'), ('XDG_DATA_HOME', 'data'), ('XDG_CONFIG_HOME', 'config'),
                           ('XDG_CACHE_HOME', 'cache'), ('XDG_RUNTIME_DIR', 'runtime')):
            path = scratch / leaf
            path.mkdir(mode=0o700)
            env[name] = str(path)
        libraries = Path(daemon).parents[1] / 'lib' / 'x86_64-linux-gnu'
        env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, (str(libraries), env.get('LD_LIBRARY_PATH'))))
        env['CRD_SECRET_SERVICE_SCRATCH'] = str(scratch)
        command = ['dbus-run-session', '--', sys.executable, str(Path(__file__).resolve()), '--daemon', daemon,
                   '--mode', args.mode, '--inner', '--'] + tests
        try:
            return subprocess.run(command, env=env, timeout=2 * SECONDS + 60).returncode
        finally:
            stop_daemons(daemon, scratch)


if __name__ == '__main__':
    sys.exit(main())
