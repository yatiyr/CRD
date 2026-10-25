#!/usr/bin/env python3
"""The local compiler cache and linker: which CMake options cache (and on WSL link) a preset, and the sccache server a
cached tree builds through.

    compiler-cache.py configure-args <preset> [--host windows|linux]
        prints, on one line, the cache entries the local helpers add when they configure <preset>
        (scripts/configure-preset.bat on Windows, scripts/wsl-lane.sh on WSL): the pinned sccache as
        CRD_COMPILER_LAUNCHER and CRD_ENABLE_PCH=OFF (the launcher refuses a precompiled header: MSVC's is not
        cacheable, a cached GCC or Clang one records bytes its key omits). Nothing is printed for a preset that is not cached: a Visual Studio generator (CMake applies no
        launcher there), a clang-tidy preset (the analysis runs on every unit) or an MSVC LTCG preset (cl with
        interprocedural optimization: the link generates the code, so the measured gain is small and converting the
        tree rebuilds every program). With CRD_SCCACHE=off it prints the reverse entries, which return a cached tree
        to the default graph. On a Linux host the measured lanes (MOLD_PRESETS) also link with the pinned mold that
        scripts/install-mold.py installs under ~/.cerid/tools/mold: CRD_LINKER and CRD_LINKER_THREADS (CRD_MOLD_THREADS,
        else the CPUs divided by the lane's CRD_LANE_JOBS, default 16), so the lane's parallel links share the CPUs
        instead of each starting a thread per CPU. CRD_MOLD=<path> names another binary, CRD_MOLD=off prints the
        reverse entries (the toolchain's linker again).
    compiler-cache.py server <build-dir> [--shell cmd|sh]
        when the tree's CMakeCache.txt names a launcher, makes sure that launcher's server runs on the local cache and
        prints the environment a build needs (SCCACHE_DIR, SCCACHE_CACHE_SIZE) as `NAME=value` lines (cmd) or
        `export` lines (sh); for an uncached tree it prints nothing. The server is started explicitly with its
        standard streams closed, so it never holds the calling shell's output open.
    compiler-cache.py stats [--zero]
        the local server's statistics.
    compiler-cache.py port
        the local server's TCP port, which every client of a cached tree must use (SCCACHE_SERVER_PORT).

Locations (each overridable): the launcher is CRD_SCCACHE, else the pinned binary that scripts/install-sccache.py
installs under ~/.cerid/tools/sccache; the cache is CRD_SCCACHE_DIR, else <checkout>/build/sccache on Windows and
~/.cache/cerid-sccache on Linux (outside the WSL source mirror); its size is CRD_SCCACHE_CACHE_SIZE, else 30G; the
server's port is CRD_SCCACHE_PORT, else 4226 on Windows and 4227 on Linux (WSL2 relays a Linux server's localhost port
to Windows, so the two hosts' servers must not share one). A server that cannot start fails the command.
The key of a cached compile includes the build directory, so a tree keeps its path. Nothing here changes a preset or
the default graph. Contract: docs/design/build-performance.md#local-cached-trees.
"""
from pathlib import Path
import argparse
import json
import os
import platform
import re
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PRESETS = ROOT / 'CMakePresets.json'
DISABLED = ('off', '0', 'no', 'none', 'false')
DEFAULT_SIZE = '30G'
DEFAULT_LANE_JOBS = 16
WINDOWS_PORT = '4226'
LINUX_PORT = '4227'
# The WSL lanes whose links the pinned mold measured faster, with every program's suite, gdb and the warning gates
# unchanged (docs/bench/2026-10-18-build-speed-linking.md). Another lane gets no linker entry until it is measured.
MOLD_PRESETS = ('linux-gcc-debug', 'linux-gcc-asan', 'linux-clang-tsan')


def is_on(value):
    return str(value or '').upper() in ('ON', 'TRUE', '1', 'YES')


def host_name():
    return 'windows' if platform.system() == 'Windows' else 'linux'


def default_launcher(host=None, home=None):
    home = Path(home) if home else Path.home()
    name = 'sccache.exe' if (host or host_name()) == 'windows' else 'sccache'
    return home / '.cerid' / 'tools' / 'sccache' / name


def launcher(environ=None, host=None, home=None):
    """(path or None, disabled): the sccache binary the helpers use."""
    environ = os.environ if environ is None else environ
    value = environ.get('CRD_SCCACHE', '').strip()
    if value.lower() in DISABLED and value:
        return None, True
    if value:
        return Path(value), False
    candidate = default_launcher(host, home)
    return (candidate if candidate.is_file() else None), False


def cache_dir(environ=None, host=None, home=None, root=ROOT):
    environ = os.environ if environ is None else environ
    if environ.get('CRD_SCCACHE_DIR'):
        return Path(environ['CRD_SCCACHE_DIR'])
    if (host or host_name()) == 'windows':
        return Path(root) / 'build' / 'sccache'
    return (Path(home) if home else Path.home()) / '.cache' / 'cerid-sccache'


def resolve_preset(name, document=None):
    """The preset's generator and merged cacheVariables, following `inherits` the way CMake does (first parent wins)."""
    document = json.loads(PRESETS.read_text(encoding='utf-8')) if document is None else document
    presets = {p['name']: p for p in document.get('configurePresets', [])}
    if name not in presets:
        raise KeyError('no configure preset %r in CMakePresets.json' % name)

    def walk(preset_name, seen):
        if preset_name in seen:
            raise ValueError('preset inheritance cycle at %s' % preset_name)
        preset = presets[preset_name]
        parents = preset.get('inherits', [])
        parents = [parents] if isinstance(parents, str) else parents
        generator = None
        variables = {}
        # An earlier parent wins over a later one, and the preset itself over every parent: apply them in reverse.
        for parent in reversed(parents):
            parent_generator, parent_variables = walk(parent, seen | {preset_name})
            variables.update(parent_variables)
            if parent_generator is not None:
                generator = parent_generator
        if preset.get('generator'):
            generator = preset['generator']
        for key, value in (preset.get('cacheVariables') or {}).items():
            if value is None:
                variables.pop(key, None)
            else:
                variables[key] = value['value'] if isinstance(value, dict) else value
        return generator, variables

    return walk(name, frozenset())


def configure_args(preset, environ=None, host=None, home=None, document=None):
    """The -D entries for a local configure of preset: the compiler cache's, then the linker's (empty when neither
    applies)."""
    return cache_args(preset, environ, host, home, document) + linker_args(preset, environ, host, home)


def cache_args(preset, environ=None, host=None, home=None, document=None):
    """The compiler cache's -D entries for a local configure of preset (an empty list when the preset is not cached)."""
    generator, variables = resolve_preset(preset, document)
    if not generator or 'Ninja' not in generator:
        return []
    if is_on(variables.get('CRD_ENABLE_CLANG_TIDY')):
        return []
    compiler = str(variables.get('CMAKE_CXX_COMPILER', '')).lower()
    if compiler == 'cl' and is_on(variables.get('CMAKE_INTERPROCEDURAL_OPTIMIZATION')):
        # MSVC's LTCG compiles only parse and the link generates the code: measured on a focused win-shipping route
        # (docs/bench/2026-10-18-build-speed-cache-mirror.md) a cached restore saves 17% while a cold miss costs 42%
        # and converting the tree rebuilds every LTCG program, so these trees stay uncached unless asked for.
        return []
    path, disabled = launcher(environ, host, home)
    if disabled:
        return ['-DCRD_COMPILER_LAUNCHER=', '-DCRD_ENABLE_PCH=ON']
    if path is None:
        return []
    return ['-DCRD_COMPILER_LAUNCHER=%s' % path.as_posix(), '-DCRD_ENABLE_PCH=OFF']


def default_mold(home=None):
    return (Path(home) if home else Path.home()) / '.cerid' / 'tools' / 'mold' / 'mold'


def link_threads(environ=None):
    """Threads per mold link: CRD_MOLD_THREADS, else the CPUs shared by the lane's parallel links (CRD_LANE_JOBS,
    wsl-lane.sh's job count, default 16), at least one."""
    environ = os.environ if environ is None else environ
    for name in ('CRD_MOLD_THREADS', 'CRD_LANE_JOBS'):
        if environ.get(name) and not re.fullmatch(r'[1-9][0-9]*', environ[name]):
            raise SystemExit('compiler-cache: %s must be a positive integer, got %r' % (name, environ[name]))
    if environ.get('CRD_MOLD_THREADS'):
        return int(environ['CRD_MOLD_THREADS'])
    jobs = int(environ.get('CRD_LANE_JOBS') or DEFAULT_LANE_JOBS)
    return max(1, (os.cpu_count() or 1) // max(1, jobs))


def linker_args(preset, environ=None, host=None, home=None):
    """The -D entries that link a measured WSL lane with the pinned mold (an empty list for every other preset)."""
    environ = os.environ if environ is None else environ
    if (host or host_name()) != 'linux' or preset not in MOLD_PRESETS:
        return []
    value = environ.get('CRD_MOLD', '').strip()
    if value and value.lower() in DISABLED:
        return ['-DCRD_LINKER=', '-DCRD_LINKER_THREADS=']
    mold = Path(value) if value else default_mold(home)
    if not value and not mold.is_file():
        return []
    return ['-DCRD_LINKER=%s' % mold.as_posix(), '-DCRD_LINKER_THREADS=%d' % link_threads(environ)]


def tree_launcher(build_dir):
    cache = Path(build_dir) / 'CMakeCache.txt'
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding='utf-8', errors='replace').splitlines():
        match = re.match(r'^CRD_COMPILER_LAUNCHER:[A-Z]+=(.*)$', line)
        if match:
            return Path(match.group(1)) if match.group(1).strip() else None
    return None


def server_port(environ=None, host=None):
    """The local server's TCP port: CRD_SCCACHE_PORT, else sccache's 4226 on Windows and 4227 on Linux. WSL2 relays
    every localhost port a Linux process listens on to Windows' localhost (wslrelay.exe), so a Linux server on the
    shared default port answered the Windows clients (its cache a Linux path, cl.exe compiles sent to it) and a Windows
    server could not start while it ran (2026-10-18: "Server startup failed: Address in use")."""
    environ = os.environ if environ is None else environ
    value = environ.get('CRD_SCCACHE_PORT', '').strip()
    if value:
        if not re.fullmatch(r'[1-9][0-9]*', value) or int(value) > 65535:
            raise SystemExit('compiler-cache: CRD_SCCACHE_PORT must be a TCP port, got %r' % value)
        return value
    return WINDOWS_PORT if (host or host_name()) == 'windows' else LINUX_PORT


def server_environment(environ=None, host=None, home=None, root=ROOT):
    environ = os.environ if environ is None else environ
    return {'SCCACHE_DIR': str(cache_dir(environ, host, home, root)),
            'SCCACHE_CACHE_SIZE': environ.get('CRD_SCCACHE_CACHE_SIZE') or DEFAULT_SIZE,
            'SCCACHE_SERVER_PORT': server_port(environ, host)}


def stats(exe, env):
    """The statistics document of the server on the environment's port, or None (no answer: not an sccache server)."""
    try:
        completed = subprocess.run([str(exe), '--show-stats', '--stats-format', 'json'], env=env, capture_output=True,
                                   text=True, encoding='utf-8', errors='replace', timeout=60, check=False,
                                   stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        # Measured: a listener that accepts and never answers holds the client until the timeout.
        return None
    if completed.returncode != 0:
        return None
    try:
        return json.loads(completed.stdout)
    except ValueError:
        return None


def serves(document, directory):
    """True when a statistics document comes from a server on this local cache directory."""
    if not document:
        return False
    location = str(document.get('cache_location', '')).replace('\\\\', '\\').replace('\\', '/').lower()
    return str(directory).replace('\\', '/').lower().rstrip('/') in location


def listening(port, host='127.0.0.1'):
    """True when a process accepts connections on the local TCP port (sccache's 4226 when none is named)."""
    try:
        with socket.create_connection((host, int(port or WINDOWS_PORT)), timeout=2):
            return True
    except OSError:
        return False


def ensure_server(exe, variables, environ=None):
    """Start (or restart) exe's server on the local cache; the server's standard streams are closed."""
    env = dict(os.environ if environ is None else environ)
    env.update(variables)
    Path(variables['SCCACHE_DIR']).mkdir(parents=True, exist_ok=True)
    quiet = {'stdin': subprocess.DEVNULL, 'stdout': subprocess.DEVNULL, 'stderr': subprocess.DEVNULL}
    # A server that is already running answers on the port whatever its cache, so ask it where it caches and restart it
    # when that is another directory (a board's scratch cache, another checkout). Ask only when something listens: a
    # statistics query with no server starts one that exits with the query (measured 2026-10-18; `--start-server`'s
    # and a compile's servers stay), so the check would pass on a server the build never sees.
    if listening(variables.get('SCCACHE_SERVER_PORT')):
        if serves(stats(exe, env), variables['SCCACHE_DIR']):
            return 'running'
    try:
        subprocess.run([str(exe), '--stop-server'], env=env, timeout=60, check=False, **quiet)
        started = subprocess.run([str(exe), '--start-server'], env=env, timeout=60, check=False, **quiet)
    except subprocess.TimeoutExpired as expired:
        started = subprocess.CompletedProcess(expired.cmd, 124)
    # The proof is a server on the port that caches here (a refused start is fine when one now does, e.g. a race);
    # asked only when something listens, for the reason above.
    port = variables.get('SCCACHE_SERVER_PORT')
    if not (listening(port) and serves(stats(exe, env), variables['SCCACHE_DIR'])):
        raise SystemExit('compiler-cache: the sccache server did not start on %s (port %s, exit %d); another process '
                         'holds the port (a WSL server is relayed to Windows), set CRD_SCCACHE_PORT or stop it'
                         % (variables['SCCACHE_DIR'], variables.get('SCCACHE_SERVER_PORT', 'default'),
                            started.returncode))
    return 'restarted'


def shell_lines(variables, shell):
    if shell == 'sh':
        return ['export %s=%s' % (key, "'%s'" % value.replace("'", "'\\''")) for key, value in variables.items()]
    return ['%s=%s' % (key, value) for key, value in variables.items()]


def main(argv=None, out=sys.stdout):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    args_mode = sub.add_parser('configure-args', help='the cache entries the local helpers add to a preset')
    args_mode.add_argument('preset')
    args_mode.add_argument('--host', choices=('windows', 'linux'))
    server_mode = sub.add_parser('server', help='start the server a cached tree builds through; print its environment')
    server_mode.add_argument('build_dir')
    server_mode.add_argument('--shell', choices=('cmd', 'sh'), default='sh' if os.name != 'nt' else 'cmd')
    stats_mode = sub.add_parser('stats', help="the local server's statistics")
    stats_mode.add_argument('--zero', action='store_true')
    sub.add_parser('port', help="the local server's TCP port (for SCCACHE_SERVER_PORT)")
    args = parser.parse_args(argv)
    if args.command == 'configure-args':
        out.write(' '.join(configure_args(args.preset, host=args.host)) + '\n')
        return 0
    if args.command == 'port':
        out.write(server_port() + '\n')
        return 0
    variables = server_environment()
    if args.command == 'stats':
        exe, _ = launcher()
        if exe is None:
            out.write('compiler-cache: no sccache installed (scripts/install-sccache.py)\n')
            return 1
        env = dict(os.environ, **variables)
        if args.zero:
            subprocess.run([str(exe), '--zero-stats'], env=env, check=False, stdin=subprocess.DEVNULL)
        return subprocess.run([str(exe), '--show-stats'], env=env, check=False, stdin=subprocess.DEVNULL).returncode
    exe = tree_launcher(args.build_dir)
    if exe is None:
        return 0
    if not exe.is_file():
        raise SystemExit('compiler-cache: %s names the launcher %s, which does not exist (scripts/install-sccache.py)'
                         % (Path(args.build_dir) / 'CMakeCache.txt', exe))
    ensure_server(exe, variables)
    out.write('\n'.join(shell_lines(variables, args.shell)) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
