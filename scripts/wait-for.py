#!/usr/bin/env python3
"""Bounded, event-driven waits for long build and test commands, in place of fixed sleeps.

    wait-for.py file PATH [--contains REGEX] [--fail REGEX]
        until PATH exists; with --contains, until its text matches REGEX. --fail ends the wait with exit 1 when its
        REGEX matches first (for example "FAILED:|ninja: build stopped" in a build log). The log is read
        incrementally, so a long log costs only its new bytes per poll.
    wait-for.py exit-file PATH [--log LOG] [--tail N]
        until PATH holds the integer a background command exited with (`cmd > log 2>&1; echo $? > PATH`), then exits
        with that code and prints the last N lines of LOG.
    wait-for.py pid PID [PID ...]
        until every process has exited (Windows process ids on Windows: from Git Bash, `cat /proc/$!/winpid`).
    wait-for.py run [--log LOG] -- COMMAND ...
        runs COMMAND and kills its whole process tree at the deadline.
    wait-for.py start --log LOG --exit-file PATH [--deadline S] -- COMMAND ...
        returns at once; a detached supervisor runs COMMAND with its output in LOG, kills its process tree after S
        seconds (default and at most 3600) and writes its exit code (124 at the deadline) to PATH. Wait for it with
        `exit-file PATH --log LOG` in calls of up to 1140 s. A WSL command started from Windows this way keeps its
        wsl.exe client attached for the whole run, so the distribution is not stopped as idle under it.

Every waiting mode polls every --interval seconds (default 2) and gives up after --timeout seconds (default 600) with
exit 124, the code coreutils `timeout` uses; a heartbeat line is printed once a minute. The local loop's watchdog
reaps a shell command older than 20 minutes, so a timeout above 1140 s is refused (exit 2): wait again in a new call,
the command keeps running. Exit codes: 0 the condition holds (run: the command's own code), 1 --fail matched, 124
timed out, 2 usage. Contract: docs/design/build-performance.md#waiting-for-long-commands.
"""
from pathlib import Path
import argparse
import os
import re
import signal
import subprocess
import sys
import threading
import time

MAX_TIMEOUT = 1140.0
MAX_DEADLINE = 3600.0
DEFAULT_TIMEOUT = 600.0
DEFAULT_INTERVAL = 2.0
HEARTBEAT = 60.0
TIMED_OUT = 124
USAGE = 2
CARRY = 4096


class Waiter:
    """Polls a condition until it holds or the deadline passes; `clock` and `sleep` are injectable for tests."""

    def __init__(self, timeout, interval, out=sys.stdout, clock=time.monotonic, sleep=time.sleep, heartbeat=HEARTBEAT):
        self.timeout = timeout
        self.interval = interval
        self.out = out
        self.clock = clock
        self.sleep = sleep
        self.heartbeat = heartbeat

    def wait(self, poll, what):
        """Call poll() until it returns a non-None result; return (result, elapsed) or (None, elapsed) on timeout."""
        started = self.clock()
        next_beat = started + self.heartbeat
        while True:
            result = poll()
            now = self.clock()
            if result is not None:
                return result, now - started
            if now - started >= self.timeout:
                return None, now - started
            if now >= next_beat:
                self.out.write('wait-for: still waiting for %s (%.0f s)\n' % (what, now - started))
                self.out.flush()
                next_beat = now + self.heartbeat
            self.sleep(min(self.interval, max(0.0, self.timeout - (now - started))))


class LogScanner:
    """Reads a growing text file from the last offset and matches regexes across chunk boundaries."""

    def __init__(self, path, contains=None, fail=None):
        self.path = Path(path)
        self.contains = re.compile(contains, re.M) if contains else None
        self.fail = re.compile(fail, re.M) if fail else None
        self.offset = 0
        self.carry = ''

    def poll(self):
        """None while waiting; ('ok', match) or ('fail', match) once decided."""
        if not self.path.exists():
            return None
        if self.contains is None and self.fail is None:
            return ('ok', None)
        try:
            with self.path.open('rb') as handle:
                handle.seek(self.offset)
                data = handle.read()
        except OSError:
            return None
        if len(data) == 0 and self.offset > self.path.stat().st_size:
            # The file was truncated and rewritten: start over.
            self.offset = 0
            self.carry = ''
            return None
        self.offset += len(data)
        text = self.carry + data.decode('utf-8', errors='replace')
        fail = self.fail.search(text) if self.fail else None
        match = self.contains.search(text) if self.contains else None
        if fail and (match is None or fail.start() <= match.start()):
            return ('fail', fail.group(0))
        if match:
            return ('ok', match.group(0))
        if self.contains is None and self.fail is not None:
            # Only a failure pattern was given: existence is the success condition.
            return ('ok', None)
        self.carry = text[-CARRY:]
        return None


def read_exit_code(path):
    """The integer in an exit-code sentinel, or None while it is absent, empty or still being written."""
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace').strip()
    except OSError:
        return None
    if not re.fullmatch(r'-?\d+', text):
        return None
    return int(text)


def tail(path, lines):
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''
    return '\n'.join(text.splitlines()[-lines:])


def process_alive(pid):
    """True while the process exists (a Linux zombie counts as exited)."""
    if os.name == 'nt':
        import ctypes
        kernel32 = ctypes.windll.kernel32
        synchronize = 0x00100000
        handle = kernel32.OpenProcess(synchronize, False, int(pid))
        if not handle:
            return False
        try:
            return kernel32.WaitForSingleObject(handle, 0) == 0x00000102  # WAIT_TIMEOUT: still running
        finally:
            kernel32.CloseHandle(handle)
    try:
        os.kill(int(pid), 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    try:
        state = Path('/proc/%d/stat' % int(pid)).read_text(encoding='utf-8').rsplit(')', 1)[1].split()[0]
        return state != 'Z'
    except (OSError, IndexError):
        return True


def kill_tree(process):
    """Terminate a command started by run() together with every process it started."""
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)], capture_output=True, check=False)
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def copy_stream(source, log):
    """Append everything the command prints to the log as it arrives. The command never writes the log itself:
    wsl.exe relays a Linux process's standard output and error with two write positions into one shared file
    handle, so the later stream overwrote the earlier one's start (measured on 2026-10-18)."""
    with open(log, 'ab') as output:
        for chunk in iter(lambda: source.read1(65536), b''):
            output.write(chunk)
            output.flush()


def run_command(command, waiter, log=None):
    """Run command under the waiter's deadline; return its exit code or TIMED_OUT."""
    kwargs = {}
    if os.name == 'nt':
        kwargs['creationflags'] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kwargs['start_new_session'] = True
    if log:
        kwargs.update(stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    process = subprocess.Popen(command, **kwargs)
    copier = None
    if log:
        copier = threading.Thread(target=copy_stream, args=(process.stdout, log), daemon=True)
        copier.start()
    result, elapsed = waiter.wait(process.poll, 'command %s' % command[0])
    if result is None:
        kill_tree(process)
        process.wait()
    if copier:
        # A grandchild that outlives the command can keep the pipe open; the log then ends with what arrived.
        copier.join(timeout=30)
        process.stdout.close()
    if result is None:
        waiter.out.write('wait-for: %s still running after %.0f s; its process tree was killed\n'
                         % (command[0], elapsed))
        return TIMED_OUT
    waiter.out.write('wait-for: %s exited %d after %.1f s\n' % (command[0], result, elapsed))
    return result


def write_exit_code(path, code):
    """Write the sentinel through a temporary name so a reader never sees a partial number."""
    path = Path(path)
    partial = path.with_name(path.name + '.partial')
    partial.write_text('%d\n' % code, encoding='utf-8')
    os.replace(partial, path)


def supervise(command, deadline, log, exit_file, out=sys.stdout):
    """The detached half of start: run, bound, record."""
    started = time.monotonic()
    code = run_command(command, Waiter(deadline, 1.0, out=out, heartbeat=float('inf')), log)
    with open(log, 'a', encoding='utf-8') as record:
        record.write('\nwait-for: %s %s after %.1f s\n' % (
            command[0], 'reached the %.0f s deadline; its process tree was killed' % deadline
            if code == TIMED_OUT else 'exited %d' % code, time.monotonic() - started))
    write_exit_code(exit_file, code)
    return code


def start(command, deadline, log, exit_file, out=sys.stdout):
    """Launch a detached supervisor for command and return its process id."""
    exit_file = Path(exit_file).resolve()
    log = Path(log).resolve()
    exit_file.unlink(missing_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)
    exit_file.parent.mkdir(parents=True, exist_ok=True)
    arguments = [sys.executable, str(Path(__file__).resolve()), 'supervise', '--deadline', str(deadline), '--log',
                 str(log), '--exit-file', str(exit_file), '--', *command]
    kwargs = {'stdin': subprocess.DEVNULL, 'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    if os.name == 'nt':
        detached = 0x00000008 | subprocess.CREATE_NEW_PROCESS_GROUP
        try:
            # Leave the caller's job object when it allows that, so ending the calling shell does not end the run.
            process = subprocess.Popen(arguments, creationflags=detached | 0x01000000, **kwargs)
        except OSError:
            process = subprocess.Popen(arguments, creationflags=detached, **kwargs)
    else:
        process = subprocess.Popen(arguments, start_new_session=True, **kwargs)
    # Detached on purpose and never waited for: mark it so Popen does not warn that it is still running.
    process.returncode = 0
    out.write('wait-for: started %s (supervisor pid %d, deadline %.0f s); '
              'wait with: wait-for.py exit-file %s --log %s\n' % (command[0], process.pid, deadline, exit_file, log))
    return process.pid


def parse(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--timeout', type=float, default=DEFAULT_TIMEOUT,
                        help='give up after this many seconds (default %%(default)s, at most %d)' % MAX_TIMEOUT)
    parser.add_argument('--interval', type=float, default=DEFAULT_INTERVAL, help='seconds between polls')
    sub = parser.add_subparsers(dest='mode', required=True)
    file_mode = sub.add_parser('file', help='wait until a file exists, optionally until its text matches')
    file_mode.add_argument('path')
    file_mode.add_argument('--contains', help='regex the text must match')
    file_mode.add_argument('--fail', help='regex that ends the wait with exit 1 when it matches first')
    exit_mode = sub.add_parser('exit-file', help='wait for an exit-code sentinel and exit with its code')
    exit_mode.add_argument('path')
    exit_mode.add_argument('--log', help='log whose tail is printed when the code arrives')
    exit_mode.add_argument('--tail', type=int, default=20)
    pid_mode = sub.add_parser('pid', help='wait until processes exit')
    pid_mode.add_argument('pids', nargs='+', type=int)
    run_mode = sub.add_parser('run', help='run a command with a deadline that kills its process tree')
    run_mode.add_argument('--log', help='append the command output here instead of the console')
    run_mode.add_argument('command', nargs=argparse.REMAINDER)
    for name, text in (('start', 'run a command detached under a supervisor with a deadline'),
                       ('supervise', argparse.SUPPRESS)):
        mode = sub.add_parser(name, help=text)
        mode.add_argument('--log', required=True, help='the command output is appended here')
        mode.add_argument('--exit-file', required=True, help='receives the exit code (124 at the deadline)')
        mode.add_argument('--deadline', type=float, default=MAX_DEADLINE,
                          help='seconds before the process tree is killed (default and at most %d)' % MAX_DEADLINE)
        mode.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.mode in ('run', 'start', 'supervise'):
        if args.command and args.command[0] == '--':
            args.command = args.command[1:]
        if not args.command:
            parser.error('%s needs a command after --' % args.mode)
    return parser, args


def main(argv=None, out=sys.stdout):
    parser, args = parse(argv)
    if args.mode in ('start', 'supervise'):
        if not 0 < args.deadline <= MAX_DEADLINE:
            out.write('wait-for: --deadline must be in (0, %d] s: nothing in the loop runs longer than an hour\n'
                      % MAX_DEADLINE)
            return USAGE
        if args.mode == 'supervise':
            return supervise(args.command, args.deadline, args.log, args.exit_file, out=out)
        start(args.command, args.deadline, args.log, args.exit_file, out=out)
        return 0
    if not 0 < args.timeout <= MAX_TIMEOUT:
        out.write('wait-for: --timeout must be in (0, %d] s: the watchdog reaps a call older than 20 minutes; '
                  'wait again in a new call\n' % MAX_TIMEOUT)
        return USAGE
    if not 0 < args.interval <= 30:
        out.write('wait-for: --interval must be in (0, 30] s\n')
        return USAGE
    waiter = Waiter(args.timeout, args.interval, out=out)
    if args.mode == 'run':
        return run_command(args.command, waiter, args.log)
    if args.mode == 'file':
        scanner = LogScanner(args.path, args.contains, args.fail)
        result, elapsed = waiter.wait(scanner.poll, args.path)
        if result is None:
            out.write('wait-for: %s did not %s within %.0f s\n'
                      % (args.path, 'match' if args.contains else 'appear', elapsed))
            return TIMED_OUT
        verdict, text = result
        out.write('wait-for: %s %s after %.1f s%s\n' % (args.path, 'matched' if verdict == 'ok' else 'FAILED',
                                                      elapsed, (': ' + text.strip()[:200]) if text else ''))
        return 0 if verdict == 'ok' else 1
    if args.mode == 'exit-file':
        code, elapsed = waiter.wait(lambda: read_exit_code(args.path), args.path)
        if code is None:
            out.write('wait-for: no exit code in %s within %.0f s\n' % (args.path, elapsed))
            return TIMED_OUT
        if args.log:
            out.write(tail(args.log, args.tail) + '\n')
        out.write('wait-for: %s recorded exit %d after %.1f s\n' % (args.path, code, elapsed))
        return code if 0 <= code <= 255 else 1
    remaining = list(args.pids)

    def poll():
        remaining[:] = [pid for pid in remaining if process_alive(pid)]
        return True if not remaining else None

    result, elapsed = waiter.wait(poll, 'process %s' % ' '.join(map(str, remaining)))
    if result is None:
        out.write('wait-for: still running after %.0f s: %s\n' % (elapsed, ' '.join(map(str, remaining))))
        return TIMED_OUT
    out.write('wait-for: processes exited after %.1f s\n' % elapsed)
    return 0


if __name__ == '__main__':
    # A build log carries whatever the tools printed; never fail on a console that cannot show a character.
    sys.stdout.reconfigure(errors='replace')
    sys.exit(main())
