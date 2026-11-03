#!/usr/bin/env python3
"""Boot the pinned Android emulator and run test programs on it (the Android test lane, NET.2).

  boot  starts the AVD scripts/install-android-emulator.py wrote, headless under KVM, and waits until Android reports
        boot completed (`--github-env` appends ANDROID_SERIAL to that file);
  stop  shuts that emulator down and waits until adb no longer lists it;
  exec  runs a program on the device and returns its exit code: CMAKE_CROSSCOMPILING_EMULATOR for the Android preset,
        so CTest and Catch2's discovery put it before every test command;
  instrument  installs an instrumentation APK (scripts/android-apk.py), runs it with Catch2's arguments as the extras
        `argc` and `arg<i>`, prints the report it wrote to `files/catch.txt` (read through run-as, so the APK is
        debuggable) and returns the run's `exit`; a run that never reports one (a crash, a missing library) fails with
        am's own output. The package is uninstalled before and after, which also deletes its Keystore keys.

exec mirrors host paths onto the device under one rule: host path P is read at MIRROR + P. The program, every
`--push` path (a test's data, named by crd_test_data) and every argument that is an absolute path to an existing host
file or directory (a fuzz corpus) are pushed there when their stamp (size and modification time of every file) differs
from the device's, under a host lock, and the arguments are rewritten to their mirrors. The program runs in a device
working directory of its own for each host working directory, with TMPDIR on the device and the host's CRD_*
variables (but CRD_ANDROID_*); adb's shell protocol returns its exit code.
"""
from pathlib import Path, PurePosixPath
import argparse
import contextlib
import hashlib
import importlib.util
import os
import posixpath
import shlex
import subprocess
import sys
import tempfile
import time

DEVICE_ROOT = PurePosixPath('/data/local/tmp/cerid')
MIRROR = DEVICE_ROOT / 'host'
STAMP_SUFFIX = '.cerid-stamp'


def sdk_root():
    """ANDROID_SDK_ROOT, else the installer's default SDK root."""
    root = os.environ.get('ANDROID_SDK_ROOT') or os.environ.get('ANDROID_HOME')
    if root:
        return Path(root)
    spec = importlib.util.spec_from_file_location('install_android_emulator',
                                                  Path(__file__).resolve().parent / 'install-android-emulator.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.default_sdk()


def adb_command(serial=None):
    adb = os.environ.get('CRD_ANDROID_ADB') or str(sdk_root() / 'platform-tools' / 'adb')
    serial = serial or os.environ.get('ANDROID_SERIAL')
    return [adb, '-s', serial] if serial else [adb]


def mirror(host_path):
    """The device path the host path `host_path` (absolute) is mirrored at."""
    return MIRROR / PurePosixPath(posixpath.abspath(Path(host_path).as_posix())).relative_to('/')


def stamp(host_path):
    """A digest of the size and modification time of every file under `host_path` (or of the file itself)."""
    path = Path(host_path)
    digest = hashlib.sha256()
    members = [path] if path.is_file() else sorted(p for p in path.rglob('*') if p.is_file())
    for member in members:
        info = member.stat()
        relative = member.relative_to(path).as_posix() if member != path else '.'
        digest.update(f'{relative}\0{info.st_size}\0{info.st_mtime_ns}\0{info.st_mode & 0o111}\n'.encode())
    return digest.hexdigest()


@contextlib.contextmanager
def host_lock(serial):
    import fcntl  # the launcher runs on Linux hosts only (the Android presets' condition)
    name = 'cerid-android-device-%s.lock' % (serial or os.environ.get('ANDROID_SERIAL') or 'default')
    with open(Path(tempfile.gettempdir()) / name, 'w') as handle:
        fcntl.flock(handle, fcntl.LOCK_EX)
        yield


def shell(adb, command, check=True, capture=True):
    result = subprocess.run(adb + ['shell', command], stdout=subprocess.PIPE if capture else None, text=True)
    if check and result.returncode != 0:
        raise RuntimeError('adb shell %r exited %d' % (command, result.returncode))
    return result


def sync(adb, paths):
    """Push each host path in `paths` to its mirror unless the device holds the same stamp; return the mirrors."""
    paths = list(dict.fromkeys(Path(os.path.abspath(p)) for p in paths))
    targets = [mirror(p) for p in paths]
    stamps = [stamp(p) for p in paths]
    probe = '; '.join("cat %s 2>/dev/null || echo -" % shlex.quote(str(t) + STAMP_SUFFIX) for t in targets)
    held = shell(adb, probe).stdout.split() if targets else []
    for path, target, digest, current in zip(paths, targets, stamps, held):
        if current == digest:
            continue
        stamp_file = shlex.quote(str(target) + STAMP_SUFFIX)
        shell(adb, 'rm -rf %s %s && mkdir -p %s' % (shlex.quote(str(target)), stamp_file,
                                                     shlex.quote(str(target.parent))))
        if subprocess.run(adb + ['push', str(path), str(target)], stdout=subprocess.DEVNULL).returncode != 0:
            raise RuntimeError('adb push %s %s failed' % (path, target))
        shell(adb, 'echo %s > %s' % (digest, stamp_file))
    return targets


def remote_command(program, arguments, cwd, environment):
    """The device shell command running `program` with `arguments` in `cwd` under `environment`."""
    tmp = DEVICE_ROOT / 'tmp'
    assignments = ['TMPDIR=' + shlex.quote(str(tmp))]
    assignments += ['%s=%s' % (key, shlex.quote(value)) for key, value in sorted(environment.items())]
    words = ' '.join(shlex.quote(str(word)) for word in [program, *arguments])
    return 'mkdir -p %s %s && cd %s && export %s && exec %s' % (shlex.quote(str(cwd)), shlex.quote(str(tmp)),
                                                                shlex.quote(str(cwd)), ' '.join(assignments), words)


def forwarded_environment(environ):
    return {k: v for k, v in environ.items() if k.startswith('CRD_') and not k.startswith('CRD_ANDROID_')}


def run(args):
    adb = adb_command(args.serial)
    program = Path(os.path.abspath(args.program))
    host_args = [a for a in args.arguments if os.path.isabs(a) and os.path.exists(a)]
    with host_lock(args.serial):
        sync(adb, [program, *args.push, *host_args])
    rewritten = [str(mirror(a)) if a in host_args else a for a in args.arguments]
    cwd = DEVICE_ROOT / 'cwd' / hashlib.sha256(os.getcwd().encode()).hexdigest()[:16]
    command = remote_command(mirror(program), rewritten, cwd, forwarded_environment(os.environ))
    sys.stdout.flush()
    return subprocess.run(adb + ['shell', command]).returncode


def instrument_command(package, runner, arguments):
    """The device shell command running `package`'s instrumentation `runner` with `arguments` as its extras."""
    extras = ['-e argc %d' % len(arguments)]
    extras += ['-e arg%d %s' % (i, shlex.quote(argument)) for i, argument in enumerate(arguments)]
    return 'am instrument -w -r %s %s/%s' % (' '.join(extras), shlex.quote(package), shlex.quote(runner))


def instrumentation_result(text):
    """(INSTRUMENTATION_CODE or None, the result bundle) from `am instrument -w -r` output."""
    code, bundle = None, {}
    for line in text.splitlines():
        key, _, value = line.partition(': ')
        if key == 'INSTRUMENTATION_RESULT':
            name, _, entry = value.partition('=')
            bundle[name] = entry
        elif key == 'INSTRUMENTATION_CODE' and value.strip().lstrip('-').isdigit():
            code = int(value)
    return code, bundle


def instrumentation_exit(code, bundle):
    """The run's exit code: its `exit` when the result code agrees with it (RESULT_OK, -1, exactly when it is 0)."""
    exit_text = bundle.get('exit', '')
    if code is None or not exit_text.lstrip('-').isdigit():
        return None
    exit_code = int(exit_text)
    return exit_code if (code == -1) == (exit_code == 0) else None


def instrument(args):
    adb = adb_command(args.serial)
    subprocess.run(adb + ['uninstall', args.package], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    installed = subprocess.run(adb + ['install', '-t', str(args.apk)], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True)
    if installed.returncode != 0:
        print('adb install %s failed:\n%s' % (args.apk, installed.stdout), file=sys.stderr)
        return 1
    try:
        command = instrument_command(args.package, args.runner, args.arguments)
        output = subprocess.run(adb + ['shell', command], stdout=subprocess.PIPE, text=True).stdout
        report = shell(adb, 'run-as %s cat files/catch.txt' % shlex.quote(args.package), check=False).stdout
    finally:
        subprocess.run(adb + ['uninstall', args.package], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    sys.stdout.write(report or '')
    exit_code = instrumentation_exit(*instrumentation_result(output))
    if exit_code is None:
        print('the instrumentation reported no exit code:\n' + output, file=sys.stderr)
        return 1
    return exit_code


def emulator_environment(sdk, environ):
    """The emulator's environment: `sdk` as its SDK root under both names, since it takes ANDROID_HOME first and a
    hosted runner's image sets ANDROID_HOME to an SDK without the pinned system image; the AVD home stays the one
    given, else the installer's."""
    return dict(environ, ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
                ANDROID_AVD_HOME=environ.get('ANDROID_AVD_HOME') or str(sdk / 'avd'))


def booted(adb):
    result = subprocess.run(adb + ['shell', 'getprop', 'sys.boot_completed'], stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    return result.returncode == 0 and result.stdout.strip() == '1'


def boot(args):
    sdk = sdk_root()
    serial = 'emulator-%d' % args.port
    adb = adb_command(serial)
    options = ['-avd', args.avd, '-port', str(args.port), '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot',
               '-gpu', 'swiftshader_indirect', '-accel', 'on', '-camera-back', 'none', '-camera-front', 'none']
    log = open(args.log, 'w') if args.log else subprocess.DEVNULL
    emulator = subprocess.Popen([str(sdk / 'emulator' / 'emulator'), *options], stdin=subprocess.DEVNULL, stdout=log,
                                stderr=subprocess.STDOUT, env=emulator_environment(sdk, os.environ),
                                start_new_session=True)
    deadline = time.monotonic() + args.timeout
    while not booted(adb):
        if emulator.poll() is not None:
            print('the emulator exited %d before Android booted' % emulator.returncode, file=sys.stderr)
            if args.log:
                log.close()
                sys.stderr.write(''.join(Path(args.log).read_text(errors='replace').splitlines(True)[-40:]))
            return 1
        if time.monotonic() > deadline:
            print('Android did not report boot completed within %d s' % args.timeout, file=sys.stderr)
            emulator.kill()
            return 1
        time.sleep(2)
    print(f'ANDROID_SERIAL={serial}')
    if args.github_env:
        with open(args.github_env, 'a', encoding='utf-8') as env:
            env.write(f'ANDROID_SERIAL={serial}\n')
    return 0


def stop(args):
    serial = args.serial or os.environ.get('ANDROID_SERIAL') or 'emulator-5554'
    adb = adb_command(serial)
    subprocess.run(adb + ['emu', 'kill'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        listed = subprocess.run(adb[:1] + ['devices'], stdout=subprocess.PIPE, text=True).stdout.split()
        if serial not in listed:
            return 0
        time.sleep(1)
    print('%s is still listed after %d s' % (serial, args.timeout), file=sys.stderr)
    return 1


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    commands = parser.add_subparsers(dest='command', required=True)
    start = commands.add_parser('boot', help='boot the AVD and wait for Android')
    start.add_argument('--avd', default='cerid-api31-x86_64')
    start.add_argument('--port', type=int, default=5554)
    start.add_argument('--timeout', type=int, default=600, help='seconds to wait for boot completed')
    start.add_argument('--log', help='file receiving the emulator output')
    start.add_argument('--github-env', help='append ANDROID_SERIAL to this file')
    halt = commands.add_parser('stop', help='shut the emulator down')
    halt.add_argument('--serial')
    halt.add_argument('--timeout', type=int, default=60)
    execute = commands.add_parser('exec', help='run a program on the device')
    execute.add_argument('--serial')
    execute.add_argument('--push', action='append', default=[], help='a host path the program reads at its mirror')
    execute.add_argument('program')
    execute.add_argument('arguments', nargs=argparse.REMAINDER)
    instrumented = commands.add_parser('instrument', help='run an instrumentation APK with Catch2 arguments')
    instrumented.add_argument('--serial')
    instrumented.add_argument('--apk', type=Path, required=True)
    instrumented.add_argument('--package', required=True)
    instrumented.add_argument('--runner', required=True, help='the instrumentation class, e.g. .KeyStoreRunner')
    instrumented.add_argument('arguments', nargs=argparse.REMAINDER, help='Catch2 arguments, after --')
    args = parser.parse_args(argv)
    if args.command == 'instrument' and args.arguments[:1] == ['--']:
        args.arguments = args.arguments[1:]
    return {'boot': boot, 'stop': stop, 'exec': run, 'instrument': instrument}[args.command](args)


if __name__ == '__main__':
    sys.exit(main())
