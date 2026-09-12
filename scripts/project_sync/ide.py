"""Windows IDE buffer/build coordination; other platforms use the same transaction engine."""
from pathlib import Path
import json
import os
import subprocess

from .storage import Conflict, Workspace, atomic_write, digest, json_bytes, read_json


def executable(ws: Workspace):
    source = Path(__file__).with_name('vs_bridge.cs')
    output = ws.state / 'bridge/vs_bridge.exe'
    stamp = output.with_suffix('.sha256')
    expected = digest(source.read_bytes())
    if output.is_file() and stamp.is_file() and stamp.read_text(encoding='utf-8') == expected:
        return output
    compiler = Path(os.environ.get('WINDIR', 'C:/Windows')) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    if not compiler.is_file():
        raise Conflict('Visual Studio coordination requires the installed .NET Framework C# compiler')
    output.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(compiler), '/nologo', '/warn:4', '/warnaserror+', '/target:exe', '/platform:x64',
                             '/reference:Microsoft.CSharp.dll', '/reference:System.Web.Extensions.dll',
                             '/out:' + str(output), str(source)], env=dict(os.environ), capture_output=True, timeout=60,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise Conflict('IDE coordinator compile failed: ' + result.stdout.decode('utf-8', errors='replace'))
    atomic_write(stamp, expected.encode('utf-8'))
    return output


def inspect(ws: Workspace, build: Path, request='inspect'):
    if os.name != 'nt':
        return {'attached': False}
    solutions = list(build.glob('*.slnx')) or list(build.glob('*.sln'))
    if len(solutions) != 1:
        return {'attached': False}
    result = subprocess.run([str(executable(ws)), str(solutions[0]), str(request)], env=dict(os.environ),
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=20,
                            creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise Conflict(result.stderr.decode('utf-8', errors='replace').strip())
    value = json.loads(result.stdout.decode('utf-8-sig'))
    session = build / 'cerid-project-sync/ide-session.json'
    if value.get('attached'):
        atomic_write(session, json_bytes({'pid': value['pid']}))
    else:
        previous = read_json(session, {})
        if previous.get('pid') and process_alive(previous['pid']):
            raise Conflict('The attached Visual Studio process is still running but cannot be reached. '
                           'Run synchronization in the same desktop/user access context; no source changes were made')
    return value


def process_alive(pid):
    if os.name != 'nt':
        try:
            os.kill(pid, 0)
            return True
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
    import ctypes
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [ctypes.c_uint32, ctypes.c_int, ctypes.c_uint32]
    kernel.OpenProcess.restype = ctypes.c_void_p
    kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.OpenProcess(0x1000, 0, pid)
    if not handle:
        return ctypes.get_last_error() == 5  # Access denied is not evidence that the process exited.
    try:
        code = ctypes.c_uint32()
        return not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
    finally:
        kernel.CloseHandle(handle)


def ready(ws: Workspace, build: Path, tx=None, *, allow_build=False):
    state = inspect(ws, build)
    if not state.get('attached'):
        return state
    if state.get('building') and not allow_build:
        raise Conflict('Visual Studio is building; synchronization waits until the build completes')
    if not state.get('solution_saved') or state.get('dirty_projects'):
        raise Conflict('Visual Studio has unsaved project/solution structure. Save the solution/projects before synchronizing')
    changed = {str(ws.root / name).casefold() for name in tx.writes} if tx else set()
    for document in state.get('documents', []):
        structural = document['path'].endswith(('CMakeLists.txt', '.cmake', '.vcxproj', '.vcxproj.filters',
                                               '.slnx', '.sln', 'project-structure.json'))
        if not document['saved'] and (document['path'].casefold() in changed or (tx is None and structural)):
            raise Conflict('Save the affected document before structural synchronization: ' + document['path'])
    return state


def reopen(ws: Workspace, build: Path, tx, state):
    if not state.get('attached'):
        return
    # Use explicit move identity, not a guess based on equal file contents.
    mapping = {}
    from .operations import remap
    for document in state.get('documents', []):
        path = Path(document['path'])
        if not path.is_relative_to(ws.root):
            continue
        name = ws.relative(path)
        destination = remap(name, tx.moves)
        if destination != name:
            mapping[str(path)] = str(ws.root / destination)
        elif name in tx.writes and tx.writes[name] is None:
            mapping[str(path)] = ''
    if mapping:
        request = ws.state / 'bridge/reopen.json'
        atomic_write(request, json_bytes(mapping))
        inspect(ws, build, request)
