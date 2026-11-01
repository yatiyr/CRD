#!/usr/bin/env python3
"""Install the hosted Linux lanes' system packages through apt-get with every network wait bounded.

A stalled package-index fetch once held a hosted lane's install step for five hours until the job limit cancelled
the lane (complete-tier run 41254049879, linux-clang-tsan, 2026-11-01). Each apt-get here runs under coreutils
`timeout` inside sudo, so the kill reaches the root-owned process; acquisition retries and transfer timeouts are set
on the command line, a held dpkg lock is waited for a bounded time, a timed-out or failed attempt is reported and
repeated a bounded number of times (an interrupted install first finishes dpkg's pending configuration), and the
last failure fails the step with the command and its outcome.
"""
import argparse
import subprocess
import sys

ACQUIRE = ['-o', 'Acquire::Retries=3', '-o', 'Acquire::http::Timeout=60', '-o', 'Acquire::https::Timeout=60',
           '-o', 'DPkg::Lock::Timeout=120']
KILL_AFTER_SECONDS = 30
TIMED_OUT = (124, 137)


class AptError(RuntimeError):
    pass


def bounded(arguments, seconds):
    """Return the sudo command that runs `arguments` for at most `seconds`, then terminates and kills it."""
    return ['sudo', 'timeout', '--kill-after=%d' % KILL_AFTER_SECONDS, str(seconds)] + arguments


def attempt(arguments, seconds, attempts, run, recover=None):
    """Run `arguments` under its bound until one attempt succeeds; raise AptError naming every outcome."""
    outcomes = []
    for index in range(1, attempts + 1):
        if index > 1 and recover is not None:
            run(bounded(recover, seconds))
        command = bounded(arguments, seconds)
        print('apt attempt %d of %d: %s' % (index, attempts, ' '.join(command)), flush=True)
        code = run(command)
        if code == 0:
            return index
        outcome = 'timed out after %d s' % seconds if code in TIMED_OUT else 'exited %d' % code
        print('apt attempt %d of %d %s' % (index, attempts, outcome), flush=True)
        outcomes.append(outcome)
    raise AptError('%s failed on all %d attempts (%s)' % (' '.join(arguments), attempts, '; '.join(outcomes)))


def install(packages, update, attempts=3, update_seconds=180, install_seconds=600, run=None):
    """Refresh the package index when `update` is set, then install `packages`."""
    run = run or (lambda command: subprocess.run(command, check=False).returncode)
    if update:
        attempt(['apt-get', '-q'] + ACQUIRE + ['update'], update_seconds, attempts, run)
    if packages:
        attempt(['apt-get', '-q', '-y'] + ACQUIRE + ['install'] + list(packages), install_seconds, attempts, run,
                recover=['dpkg', '--configure', '-a'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('packages', nargs='*', help='packages to install')
    parser.add_argument('--update', action='store_true', help='refresh the package index first')
    parser.add_argument('--attempts', type=int, default=3, help='attempts per apt-get command (default 3)')
    parser.add_argument('--update-seconds', type=int, default=180, help='bound of one index refresh (default 180)')
    parser.add_argument('--install-seconds', type=int, default=600, help='bound of one install (default 600)')
    arguments = parser.parse_args(argv)
    if not arguments.update and not arguments.packages:
        parser.error('nothing to do: give --update, packages or both')
    if arguments.attempts < 1 or arguments.update_seconds < 1 or arguments.install_seconds < 1:
        parser.error('attempts and bounds must be positive')
    try:
        install(arguments.packages, arguments.update, arguments.attempts, arguments.update_seconds,
                arguments.install_seconds)
    except AptError as error:
        print('error: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
