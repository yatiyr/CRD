"""Read-only host/build diagnostics and the existing Windows toolchain environment."""
from __future__ import annotations

import ctypes
import hashlib
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys

from .selection import SelectionError


def cache_values(build):
    path = build / 'CMakeCache.txt'
    values = {}
    if path.is_file():
        for line in path.read_text(encoding='utf-8-sig').splitlines():
            match = re.match(r'^([^/#][^:=]*):[^=]+=(.*)$', line)
            if match:
                values[match[1]] = match[2]
    return values


def native_profile_issues(cache):
    if cache.get('CRD_NATIVE_PROFILES') != 'ON':
        return []
    required = ['CMAKE_CXX_FLAGS', *(f'CMAKE_CXX_FLAGS_{name}' for name in ('DEBUG', 'RELEASE', 'RELWITHDEBINFO'))]
    empty = [name for name in required if not cache.get(name, '').strip().strip("'").strip()]
    return ['Native profile cache has empty compiler defaults: ' + ', '.join(empty)
            + '; restore the corresponding CMake defaults through guarded configuration before qualification'] if empty else []


def compiler_metadata(build):
    """CMake's measured compiler identity, not a guessed version from its executable name."""
    result = {}
    cache = cache_values(build)
    version = '.'.join(cache.get('CMAKE_CACHE_' + part + '_VERSION', '') for part in ('MAJOR', 'MINOR', 'PATCH'))
    path = build / 'CMakeFiles' / version / 'CMakeCXXCompiler.cmake'
    if path.is_file():
        text = path.read_text(encoding='utf-8-sig')
        for name in ('CMAKE_CXX_COMPILER', 'CMAKE_CXX_COMPILER_ID', 'CMAKE_CXX_COMPILER_VERSION',
                     'CMAKE_CXX_COMPILER_ARCHITECTURE_ID'):
            match = re.search(r'set\(' + name + r' "([^"]*)"\)', text)
            result[name] = match[1] if match else None
    return result


def build_environment(root, cache, initialize_msvc=True):
    """Capture vcvars into this process only; never print or persist the full environment."""
    environment = dict(os.environ)
    if os.name != 'nt' or not initialize_msvc:
        return environment, 'inherited'
    compiler = Path(cache.get('CMAKE_CXX_COMPILER', '')).name.lower()
    windows_toolchain = (compiler in {'cl', 'cl.exe', 'clang-cl', 'clang-cl.exe'}
                         or cache.get('CMAKE_GENERATOR', '').startswith('Visual Studio'))
    if not windows_toolchain:
        return environment, 'inherited (compiler environment not requested)'
    if not (root / 'scripts/msvc-env.bat').is_file():
        if cache.get('CMAKE_GENERATOR', '').startswith('Visual Studio'):
            return environment, 'inherited (native MSBuild resolves its compiler toolchain)'
        raise SelectionError('MSVC environment helper is missing from this checkout')
    # Fixed command text, with the caller's source directory passed as cwd, not interpolated shell syntax.
    # /u makes SET's output UTF-16 so non-ASCII SDK/user paths survive any console code page.
    result = subprocess.run([os.environ.get('COMSPEC', 'cmd.exe'), '/u', '/d', '/s', '/c',
                             'call scripts\\msvc-env.bat >nul && set'], cwd=root, env=environment,
                            capture_output=True, timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
    if result.returncode:
        raise SelectionError(f'MSVC environment helper exited {result.returncode}; use its direct diagnostic command')
    for line in result.stdout.decode('utf-16-le').splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            # Windows environment names are case-insensitive; avoid retaining two PATH spellings.
            for old in list(environment):
                if old.casefold() == key.casefold():
                    del environment[old]
            environment[key] = value
    return environment, 'scripts/msvc-env.bat (process-local capture)'


def env_value(environment, name):
    return next((value for key, value in environment.items() if key.casefold() == name.casefold()), None)


def resolve_tool(name, environment):
    if not name:
        return None
    path = Path(name)
    if path.is_absolute():
        return str(path.resolve()) if path.is_file() else None
    return shutil.which(name, path=env_value(environment, 'PATH'))


REQUIRED_TIDY_MAJOR = 20
PINNED_WINDOWS_TIDY = 'C:/LLVM-20.1.8/bin/clang-tidy.exe'
TIDY_CANDIDATES = ('clang-tidy-20', 'clang-tidy')
LLVM_VERSION = re.compile(r'LLVM version (\d+)\.(\d+)\.(\d+)')


def probe_llvm_version(path, environment):
    """The tool's own --version report; a probe that names no LLVM version is a rejection, not a guess."""
    try:
        result = subprocess.run([path, '--version'], capture_output=True, env=environment, timeout=30)
    except (OSError, subprocess.TimeoutExpired) as error:
        return None, f'{path}: version probe failed ({error.__class__.__name__})'
    match = LLVM_VERSION.search(result.stdout.decode('utf-8', errors='replace'))
    if result.returncode or not match:
        return None, f'{path}: version probe exited {result.returncode} without an LLVM version'
    return '.'.join(match.groups()), None


def resolve_clang_tidy(environment, override=None):
    """First candidate whose reported LLVM major is the gate's: an explicit tool (argument or CRD_CLANG_TIDY) is the
    only candidate; otherwise the pinned Windows install, then the PATH names. Any other version is unavailable,
    never a substitute; the reason names each rejection."""
    explicit = override or env_value(environment, 'CRD_CLANG_TIDY')
    searched = [explicit] if explicit else [PINNED_WINDOWS_TIDY if os.name == 'nt' else None, *TIDY_CANDIDATES]
    candidates = []
    for name in searched:
        path = resolve_tool(name, environment) if name else None
        if path and path not in [candidate for _, candidate in candidates]:
            candidates.append((name, path))
    rejected = []
    for name, path in candidates:
        version, error = probe_llvm_version(path, environment)
        if version and int(version.split('.')[0]) == REQUIRED_TIDY_MAJOR:
            return {'path': path, 'version': version, 'candidate': name, 'rejected': rejected, 'reason': None}
        rejected.append(error or f'{path} reports LLVM {version}; the gate is LLVM {REQUIRED_TIDY_MAJOR}')
    detail = '; '.join(rejected) if rejected else 'no candidate (' + ', '.join(filter(None, searched)) + ')'
    return {'path': None, 'version': None, 'candidate': None, 'rejected': rejected,
            'reason': f'clang-tidy {REQUIRED_TIDY_MAJOR} is unavailable: {detail}'}


def runtime_file(name, environment):
    # DLLs are data loaded by the executable; PATHEXT/shutil.which is the wrong existence test.
    for entry in (env_value(environment, 'PATH') or '').split(os.pathsep):
        if entry:
            path = Path(entry.strip('"')) / name
            if path.is_file():
                return str(path.resolve())
    return None


def cmake_tools(cache, environment):
    # Match the existing standalone Windows helper; an inherited VS-bundled CMake must not take precedence.
    cmake = cache.get('CMAKE_COMMAND')
    if not cmake and os.name == 'nt':
        standalone = Path(env_value(environment, 'ProgramFiles') or 'C:/Program Files') / 'CMake/bin/cmake.exe'
        cmake = str(standalone) if standalone.is_file() else None
    cmake = resolve_tool(cmake or 'cmake', environment)
    ctest = None
    if cmake:
        candidate = Path(cmake).with_name('ctest.exe' if os.name == 'nt' else 'ctest')
        ctest = str(candidate) if candidate.is_file() else None
    return cmake, ctest


def ram_bytes():
    if os.name == 'nt':
        class MemoryStatus(ctypes.Structure):
            _fields_ = [('length', ctypes.c_uint32), ('load', ctypes.c_uint32)] + [
                (name, ctypes.c_uint64) for name in ('total', 'available', 'total_page', 'available_page',
                                                    'total_virtual', 'available_virtual', 'extended')]
        status = MemoryStatus()
        status.length = ctypes.sizeof(status)
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.GlobalMemoryStatusEx.argtypes = [ctypes.POINTER(MemoryStatus)]
        kernel.GlobalMemoryStatusEx.restype = ctypes.c_int
        if kernel.GlobalMemoryStatusEx(ctypes.byref(status)):
            return {'total': status.total, 'available': status.available}
        return {'total': None, 'available': None, 'error': ctypes.get_last_error()}
    try:
        page = os.sysconf('SC_PAGE_SIZE')
        return {'total': page * os.sysconf('SC_PHYS_PAGES'), 'available': page * os.sysconf('SC_AVPHYS_PAGES')}
    except (ValueError, OSError, AttributeError):
        return {'total': None, 'available': None}


def synchronization_state(root, build):
    """Read persisted conflicts and validate process liveness; do not compile/open/write the IDE bridge."""
    from project_sync import ide
    from project_sync.service import registrations, state_dir
    from project_sync.storage import Conflict, Workspace, incomplete, read_json
    result = {'builds': [], 'conflicts': [], 'ide_buffers': 'not inspected; execution must use the existing IDE guard'}
    try:
        ws = Workspace(root)
        result['incomplete_transactions'] = incomplete(ws)
        if result['incomplete_transactions']:
            result['conflicts'].append('Unfinished project-structure transaction requires recovery')
        enrolled = registrations(ws)
        if build.is_dir() and state_dir(build).is_dir() and build not in enrolled:
            enrolled.append(build)
        for current in enrolled:
            state = state_dir(current)
            watcher = read_json(state / 'watcher.json', {})
            generation = read_json(state / 'generation.json', {})
            watcher_alive = bool(watcher.get('pid')) and ide.process_alive(watcher['pid'])
            generation_alive = bool(generation.get('pid')) and ide.process_alive(generation['pid'])
            result['builds'].append({'build': str(current), 'watcher_status': watcher.get('status'),
                                    'watcher_alive': watcher_alive, 'generation_alive': generation_alive,
                                    'generation_record': bool(generation)})
            if generation:
                result['conflicts'].append('Generation is active' if generation_alive else 'Generation needs reconciliation')
            if watcher.get('status') == 'conflict':
                result['conflicts'].append('Watcher recorded a structure conflict; reconcile before execution')
            if (state / 'recovery.json').is_file():
                result['conflicts'].append('Recovered projection requires regeneration')
    except (Conflict, OSError, ValueError) as error:
        result['conflicts'].append(str(error))
    return result


def doctor(root, build, configuration=None, initialize_msvc=True):
    root, build = root.resolve(strict=True), build.resolve()
    cache = cache_values(build)
    issues = []
    if cache and any(key not in cache for key in ('CMAKE_HOME_DIRECTORY', 'CMAKE_GENERATOR', 'CMAKE_COMMAND')):
        issues.append('CMake cache is incomplete')
    if cache.get('CMAKE_HOME_DIRECTORY') and Path(cache['CMAKE_HOME_DIRECTORY']).resolve() != root:
        issues.append('Build cache belongs to another source directory')
    configurations = cache.get('CMAKE_CONFIGURATION_TYPES', '').split(';')
    if configurations != ['']:
        if configuration not in configurations:
            issues.append('Choose an actual native configuration with --config')
    elif configuration and configuration != cache.get('CMAKE_BUILD_TYPE'):
        issues.append('--config differs from the configured single-configuration build')
    try:
        environment, environment_source = build_environment(root, cache, initialize_msvc)
    except (SelectionError, OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        issues.append(str(error))
        environment, environment_source = dict(os.environ), 'unavailable; inherited environment reported'
    cmake, ctest = cmake_tools(cache, environment)
    measured = compiler_metadata(build)
    compiler_name = cache.get('CMAKE_CXX_COMPILER') or measured.get('CMAKE_CXX_COMPILER')
    compiler = resolve_tool(compiler_name, environment)
    native = cache.get('CMAKE_GENERATOR', '').startswith('Visual Studio')
    make_name = cache.get('CMAKE_MAKE_PROGRAM')
    if not make_name and native:
        instance = cache.get('CMAKE_GENERATOR_INSTANCE', '').split(',')[0]
        if instance:
            make_name = str(Path(instance) / 'MSBuild/Current/Bin/MSBuild.exe')
    make = resolve_tool(make_name or ('MSBuild.exe' if native else 'ninja'), environment)
    analysis = resolve_clang_tidy(environment)
    tools = {'cmake': cmake, 'ctest': ctest, 'compiler': compiler, 'build_tool': make,
             'python': sys.executable, 'clang_tidy_20': analysis['path']}
    if not cmake or not ctest:
        issues.append('Matching CMake/CTest executables are unavailable')
    if compiler_name and not compiler:
        issues.append('Configured compiler could not be resolved')
    if native and compiler and not runtime_file('dumpbin.exe', environment):
        issues.append('Native CTest object guards require dumpbin on the effective runtime path')
    presets, versions = [], {}
    for name in ('cmake', 'build_tool'):
        tool = tools[name]
        if tool and (name == 'cmake' or 'ninja' in Path(tool).name.lower()):
            result = subprocess.run([tool, '--version'], capture_output=True, env=environment, timeout=15)
            versions[name] = {'exit': result.returncode,
                              'text': result.stdout.decode('utf-8', errors='replace').strip().splitlines()[:1]}
            if result.returncode:
                issues.append(f'{name} version probe failed ({result.returncode})')
    if cmake and (root / 'CMakePresets.json').is_file():
        result = subprocess.run([cmake, '--list-presets=configure'], cwd=root, env=environment,
                                capture_output=True, timeout=30)
        if result.returncode:
            issues.append(f'CMake preset eligibility query failed ({result.returncode})')
        else:
            presets = re.findall(r'^\s*"([^"]+)"', result.stdout.decode('utf-8', errors='replace'), re.M)
    location = build
    while not location.exists():
        location = location.parent
    disk = shutil.disk_usage(location)
    cache_path = build / 'CMakeCache.txt'
    runtime = {name: env_value(environment, name) for name in ('VULKAN_SDK', 'VK_LAYER_PATH', 'LD_LIBRARY_PATH')}
    runtime['configured_vulkan_include'] = cache.get('Vulkan_INCLUDE_DIR')
    runtime['configured_vulkan_loader'] = cache.get('Vulkan_LIBRARY')
    runtime['configured_sysroot'] = cache.get('CMAKE_SYSROOT')
    if os.name == 'nt':
        runtime['asan_dll'] = runtime_file('clang_rt.asan_dynamic-x86_64.dll', environment)
        if cache.get('CRD_ENABLE_ASAN') == 'ON' and not runtime['asan_dll']:
            issues.append('ASan runtime DLL is not on the effective runtime path')
    issues += native_profile_issues(cache)
    sync = synchronization_state(root, build)
    issues += sync['conflicts']
    return {'version': 1, 'kind': 'doctor', 'root': str(root), 'build': str(build),
            'configuration': configuration or cache.get('CMAKE_BUILD_TYPE'), 'configured': bool(cache),
            'host': {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(),
                     'python_version': platform.python_version()},
            'tools': tools, 'versions': versions, 'compiler_metadata': compiler_metadata(build),
            'generator': cache.get('CMAKE_GENERATOR'), 'eligible_configure_presets': presets,
            'cache_sha256': hashlib.sha256(cache_path.read_bytes()).hexdigest() if cache_path.is_file() else None,
            'strict_analysis': analysis,
            'environment_source': environment_source, 'runtime': runtime, 'ram_bytes': ram_bytes(),
            'disk_bytes': {'path': str(location), 'total': disk.total, 'free': disk.free},
            'synchronization': sync, 'issues': issues, 'environment_ready': bool(cache) and not issues,
            'qualification': 'environment diagnosis only; no build/tests executed'}
