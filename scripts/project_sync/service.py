"""Generation lifecycle, saved-edit ingestion and the optional local watcher."""
from __future__ import annotations

from contextlib import contextmanager
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import uuid
import zipfile

from .model import MANIFEST, cmake_model, emit_cmake, load_manifest, request_file_api
from . import ide
from .storage import Conflict, Transaction, Workspace, atomic_write, digest, incomplete, json_bytes, read_json
from .visual_studio import capture, ide_plan, source_plan, module_catalog, inventory


def state_dir(build: Path):
    return build / 'cerid-project-sync'


def state_file(build: Path):
    return state_dir(build) / 'baseline.json'


def enroll_existing(ws: Workspace, build: Path):
    """First enrollment never overwrites a saved IDE change without a known common generation."""
    if state_file(build).exists():
        return
    projects = [path for path in build.rglob('*.vcxproj')
                if not {'_deps', 'CMakeFiles'}.intersection(path.relative_to(build).parts)]
    if not projects:
        return
    files = projects + [path.with_suffix('.vcxproj.filters') for path in projects
                        if path.with_suffix('.vcxproj.filters').is_file()]
    files += list(build.glob('*.slnx')) + list(build.glob('*.sln'))
    backup = state_dir(build) / 'enrollment.zip'
    if not backup.exists():
        backup.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(backup, 'x', zipfile.ZIP_DEFLATED) as archive:
            for path in files:
                ws.path(ws.relative(path))
                archive.write(path, path.relative_to(build).as_posix())
    stamp = build / 'CMakeFiles/generate.stamp'
    if not stamp.is_file():
        raise Conflict('Existing native projects have no CMake generation stamp. Saved projects were backed up; '
                       'use a freshly generated build directory to establish the initial baseline')
    newer = [ws.relative(path) for path in files if path.stat().st_mtime_ns > stamp.stat().st_mtime_ns]
    if newer:
        raise Conflict('Initial enrollment found saved IDE edits newer than the CMake generation. '
                       'They are preserved in enrollment.zip. Reconcile those edits into source/CMake before enrolling: '
                       + ', '.join(newer[:5]))


def registrations(ws: Workspace):
    value = read_json(registry_file(ws), {'version': 1, 'builds': []})
    if value.get('version') != 1:
        raise Conflict('Unsupported synchronization registration version')
    result = []
    for name in value['builds']:
        path = ws.path(name, internal=True)
        if not path.is_relative_to(ws.root / 'build') or not path.is_dir():
            raise Conflict(f'Registered native build is missing: {name}; unregister it explicitly')
        result.append(path)
    return result


def registry_file(ws: Workspace):
    host = hashlib.sha256((os.name + ':' + str(ws.root)).encode('utf-8')).hexdigest()[:16]
    return ws.state / ('registrations-' + host + '.json')


def register(ws: Workspace, build: Path):
    names = {ws.relative(path) for path in registrations(ws)}
    names.add(ws.relative(build))
    atomic_write(registry_file(ws), json_bytes({'version': 1, 'builds': sorted(names)}))


def generation_idle(ws: Workspace, builds=(), allowed_pid=None):
    for build in set(registrations(ws)) | set(builds):
        pending = read_json(state_dir(build) / 'generation.json')
        if (pending and pending['pid'] != allowed_pid and 'failure' not in pending
                and ide.process_alive(pending['pid'])):
            raise Conflict(f'CMake generation is running for {build}; wait for completion before changing structure')


def ingest(ws: Workspace, build: Path, *, apply=False, allow_build=False, source_changes=True):
    baseline = read_json(state_file(build))
    if baseline is None:
        return None
    if baseline.get('root') != str(ws.root) or baseline.get('build') != str(build):
        raise Conflict('Synchronization baseline belongs to another repository/build')
    recovery = read_json(state_dir(build) / 'recovery.json')
    if recovery:
        if projection_hashes(ws, build) != recovery['projection']:
            raise Conflict('Saved IDE structure changed after recovery; preserve/reconcile it before replacing the projection')
        return None  # Explicit recovery cancels that imported gesture; the next configure projects recovered source.
    consumed = read_json(state_dir(build) / 'consumed.json')
    if consumed:
        for name, expected in consumed['outputs'].items():
            current = ws.bytes(name)
            structural = name == MANIFEST or name.endswith(('CMakeLists.txt', '.cmake'))
            if ((expected is None) != (current is None)) or (structural and digest(current) != expected):
                raise Conflict(f'Synchronized structure changed before regeneration completed: {name}; reconcile it')
        return None
    tx = ide_plan(ws, baseline)
    if tx is None and source_changes:
        tx = source_plan(ws, baseline)
    if tx is None:
        return None
    result = tx.describe()
    if apply:
        state = ide.ready(ws, build, tx, allow_build=allow_build)
        identity = tx.apply()
        result['transaction'] = identity
        atomic_write(state_dir(build) / 'consumed.json', json_bytes({
            'transaction': identity, 'outputs': {name: digest(value) for name, value in tx.writes.items()}}))
        ide.reopen(ws, build, tx, state)
    return result


def finalize(root: Path, build: Path):
    ws = Workspace(root, [build])
    with ws.lock():
        generation = read_json(state_dir(build) / 'generation.json', {})
        snapshot = capture(ws, build)
        if 'filesystem' in generation:
            # Do not absorb an external source add/rename made while CMake was running.
            # Leave it as a delta for the next watcher step/build guard.
            snapshot['filesystem'] = generation['filesystem']
        for name, expected in generation.get('inputs', {}).items():
            if name in snapshot['model']['inputs']:
                snapshot['model']['inputs'][name] = expected
        atomic_write(state_file(build), json_bytes(snapshot))
        consumed = state_dir(build) / 'consumed.json'
        if consumed.exists():
            consumed.unlink()
        pending = state_dir(build) / 'generation.json'
        if pending.exists():
            pending.unlink()
        recovery = state_dir(build) / 'recovery.json'
        if recovery.exists():
            recovery.unlink()
        register(ws, build)


def projection_hashes(ws: Workspace, build: Path):
    baseline = read_json(state_file(build))
    if baseline is None:
        return {}
    paths = {baseline['solution']['path']}
    for owner in baseline['model']['targets'].values():
        paths.add(owner['project'])
        paths.add(owner['project'] + '.filters')
    return {name: digest(ws.bytes(name)) for name in sorted(paths)}


def wait_cmake_exit(pid: int, ready=None):
    """Keep a real process handle, avoiding PID-reuse and inferred-completion races."""
    if os.name != 'nt':
        raise Conflict('Automatic native generation completion requires Windows; use the portable configure command')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x00100000 | 0x1000, 0, pid)
    if not handle:
        raise Conflict(f'Cannot observe CMake process {pid}; run sync configure to establish a new baseline')
    try:
        if ready is not None:
            ready()
        while kernel.WaitForSingleObject(handle, 1000) == 0x102:
            pass
        code = ctypes.c_uint32()
        if not kernel.GetExitCodeProcess(handle, ctypes.byref(code)):
            raise Conflict('Cannot read CMake completion status')
        return code.value
    finally:
        kernel.CloseHandle(handle)


def finish_process(root: Path, build: Path, pid: int, token: str):
    code = wait_cmake_exit(pid, lambda: atomic_write(state_dir(build) / 'generation-ready.json',
                                                   json_bytes({'token': token})))
    pending = read_json(state_dir(build) / 'generation.json')
    if not pending or pending.get('token') != token:
        raise Conflict('Generation completion token changed; refusing to capture a different generation')
    if code != 0:
        pending['failure'] = code
        atomic_write(state_dir(build) / 'generation.json', json_bytes(pending))
        raise Conflict(f'CMake exited {code}; preserved edits remain pending, with no successful baseline claim')
    finalize(root, build)


def hidden_process(arguments, log: Path):
    log.parent.mkdir(parents=True, exist_ok=True)
    options = {}
    if os.name == 'nt':
        options['creationflags'] = subprocess.CREATE_NO_WINDOW
    else:
        options['start_new_session'] = True
    with log.open('ab') as stream:
        return subprocess.Popen(arguments, env=dict(os.environ), stdin=subprocess.DEVNULL,
                                stdout=stream, stderr=subprocess.STDOUT, close_fds=True, **options)


def preconfigure(root: Path, build: Path, native: bool):
    build.mkdir(parents=True, exist_ok=True)
    ws = Workspace(root, [build] if native else [])
    wrapper = os.environ.get('CRD_PROJECT_SYNC_WRAPPER') == str(build)
    with ws.lock():
        generation_idle(ws, [build] if native else [], allowed_pid=os.getppid())
        if incomplete(ws):
            raise Conflict('An incomplete project-structure transaction must be recovered before configuring')
        changed = False
        if native:
            enroll_existing(ws, build)
        for active in registrations(ws):
            active_ws = Workspace(root, [active])
            own_regeneration = native and active == build
            ide.ready(active_ws, active, allow_build=own_regeneration)
            changed |= ingest(active_ws, active, apply=True, allow_build=own_regeneration) is not None
        manifest = load_manifest(ws)
        directories = Transaction(ws, 'Restore authored empty project directories')
        for name in manifest['directories']:
            directories.mkdir(name)
        for settings in manifest['targets'].values():
            for name in settings.get('directories', []):
                directories.mkdir(name)
        directories.apply()
        emit_cmake(ws, state_dir(build) / 'structure.cmake')
        if native:
            request_file_api(build)
            token = uuid.uuid4().hex
            pending = {'pid': os.getppid(), 'token': token}
            baseline = read_json(state_file(build))
            if baseline:
                pending['filesystem'] = inventory(ws, baseline['model'])
                pending['inputs'] = {name: digest(ws.bytes(name)) for name in baseline['model']['inputs']}
            atomic_write(state_dir(build) / 'generation.json', json_bytes(pending))
        if changed:
            # The current CMake file has already been parsed, so a physical module move needs a fresh parse.
            raise Conflict('Synchronized saved IDE edits before regeneration. Run configure again to read the updated CMake files')
    if native and not wrapper:
        hidden_process([sys.executable, str(root / 'scripts/project-sync.py'), '--root', str(root),
                        'finish-generation', '--build', str(build), '--pid', str(os.getppid()), '--token', token],
                       state_dir(build) / 'generation.log')
        deadline = time.monotonic() + 10.0
        while read_json(state_dir(build) / 'generation-ready.json', {}).get('token') != token:
            if time.monotonic() >= deadline:
                raise Conflict('Generation observer did not acquire the CMake process handle; inspect generation.log')
            time.sleep(0.025)


def cmake_executable():
    if os.name == 'nt':
        standalone = Path(os.environ.get('ProgramFiles', 'C:/Program Files')) / 'CMake/bin/cmake.exe'
        if standalone.is_file():
            return str(standalone)
    path = shutil.which('cmake')
    if not path:
        raise Conflict('CMake is not on PATH')
    return path


def configure(root: Path, build: Path, *, initial_args=()):
    build.mkdir(parents=True, exist_ok=True)
    ws = Workspace(root, [build])
    with ws.lock():
        generation_idle(ws, [build])
        ide.ready(ws, build)
        enroll_existing(ws, build)
        for active in registrations(ws):
            ingest(Workspace(root, [active]), active, apply=True)
        request_file_api(build)
    environment = dict(os.environ)
    environment['CRD_PROJECT_SYNC_WRAPPER'] = str(build)
    command = [cmake_executable(), '-S', str(root), '-B', str(build), *initial_args]
    result = subprocess.run(command, cwd=root, env=environment,
                            stdout=sys.stdout, stderr=sys.stderr,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    if result.returncode:
        raise Conflict(f'CMake configure failed ({result.returncode}); edits/journal retained; see configure output')
    if list(build.glob('*.slnx')) or list(build.glob('*.sln')):
        finalize(root, build)


def verify(root: Path, build: Path):
    ws = Workspace(root, [build])
    deadline = time.monotonic() + 10
    while read_json(state_dir(build) / 'generation.json') and time.monotonic() < deadline:
        pending = read_json(state_dir(build) / 'generation.json')
        if pending and 'failure' in pending:
            raise Conflict('The last CMake generation failed; inspect generation.log and reconfigure')
        time.sleep(0.05)
    with ws.lock():
        if incomplete(ws):
            raise Conflict('Recover the incomplete project transaction before building')
        if not state_file(build).exists() or read_json(state_dir(build) / 'generation.json'):
            raise Conflict('The native synchronization baseline is not ready; run project-sync configure')
        ide.ready(ws, build, allow_build=True)
        if ingest(ws, build) is not None or needs_generation(ws, build):
            raise Conflict('Saved structure edits are pending. Let project-sync finish/reload before building; '
                           'run project-sync open to enable automatic synchronization')


def needs_generation(ws: Workspace, build: Path):
    baseline = read_json(state_file(build))
    return (baseline is None or bool(read_json(state_dir(build) / 'consumed.json'))
            or bool(read_json(state_dir(build) / 'recovery.json'))
            or any(digest(ws.bytes(name)) != expected for name, expected in baseline['model']['inputs'].items()))


def reconcile(root: Path, build: Path):
    """One idempotent watcher step; a fresh external generation needs no second configure."""
    ws = Workspace(root, [build])
    with ws.lock():
        if incomplete(ws):
            raise Conflict('Recover the incomplete project transaction before synchronization')
        pending = read_json(state_dir(build) / 'generation.json')
        if pending:
            if 'failure' in pending or not ide.process_alive(pending['pid']):
                raise Conflict('Generation did not finish; inspect generation.log and run project-sync configure')
            return False, None
        result = ingest(ws, build, apply=True)
        regenerate = result is not None or needs_generation(ws, build)
    if regenerate:
        configure(root, build)
    return True, result


def signature(root: Path, build: Path):
    baseline = read_json(state_file(build))
    if baseline is None:
        return None
    paths = [root / baseline['solution']['path'], state_file(build)]
    for owner in baseline['model']['targets'].values():
        project = root / owner['project']
        paths.extend([project, project.with_suffix('.vcxproj.filters')])
    paths.extend(root / name for name in baseline['model']['inputs'])
    result = []
    for path in paths:
        try:
            info = path.stat()
            result.append((str(path), info.st_size, info.st_mtime_ns))
        except FileNotFoundError:
            result.append((str(path), None, None))
    # Directory metadata observes adds/moves/removes without hashing or reacting to ordinary C++ edits.
    ws = Workspace(root, [build])
    roots = sorted({owner['source_dir'] for owner in baseline['model']['targets'].values()} | set(module_catalog(ws)))
    for name in roots:
        for directory, children, _ in os.walk(root / name, followlinks=False):
            children[:] = [child for child in children if child not in {'.vs', '__pycache__'}]
            path = Path(directory)
            result.append((str(path), path.stat().st_mtime_ns))
    return result


def watch(root: Path, build: Path, interval=2.0):
    ws = Workspace(root, [build])
    name = 'watcher-' + hashlib.sha256(str(build).encode('utf-8')).hexdigest()[:16]
    with ws.lock(timeout=0.1, name=name):
        watch_loop(root, build, interval)


def watch_loop(root: Path, build: Path, interval=2.0):
    ws = Workspace(root, [build])
    state_dir(build).mkdir(parents=True, exist_ok=True)
    stop = state_dir(build) / 'stop'
    status = state_dir(build) / 'watcher.json'
    if stop.exists():
        stop.unlink()
    # An initial pending edit must be processed even if nothing changes after startup.
    previous, stable, last_error = object(), None, None
    atomic_write(status, json_bytes({'pid': os.getpid(), 'status': 'watching', 'build': str(build)}))
    while not stop.exists():
        time.sleep(interval)
        try:
            current = signature(root, build)
            if current == previous and last_error is None:
                continue
            if current != stable:
                stable = current
                continue
            complete, result = reconcile(root, build)
            if not complete:
                continue
            previous, stable, last_error = signature(root, build), None, None
            atomic_write(status, json_bytes({'pid': os.getpid(), 'status': 'watching', 'last_import': result}))
        except (Conflict, OSError, KeyError, TypeError, ValueError, AttributeError, subprocess.TimeoutExpired) as error:
            if str(error) != last_error:
                last_error = str(error)
                print('PROJECT SYNC CONFLICT: ' + last_error, flush=True)
                atomic_write(status, json_bytes({'pid': os.getpid(), 'status': 'conflict', 'message': last_error}))
            # Retry preflight after transient IDE build/buffer conflicts without requiring another disk edit.
            # Transactions remain CAS protected, and incomplete journals block reapplication.
    atomic_write(status, json_bytes({'pid': os.getpid(), 'status': 'stopped'}))
