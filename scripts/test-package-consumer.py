#!/usr/bin/env python3
"""Install the Cerid package of the diagnostic host's family, move the prefix, and build and run downstream projects
against it.

Contract: docs/design/public-consumption.md (the package) and docs/design/runtime-diagnostics.md (DIAG.11a, the
installed external consumer). One run proves, for one profile (generator, compiler, build type, profile switches):

- the installed tree is complete: every public header of every packaged module (the diagnostic host's closure, perf,
  and math, whose header-only functions the units headers include), one archive per module and nothing else, no
  template (`build_config.hpp.in`), the authored diagnostic assets byte for byte under share/cerid, and the archives'
  debug information as `CeridConfig.cmake` declares it (`pdb`: each archive's compile PDB beside it);
- the generated `<crd/core/build_config.hpp>` says what the packaging configuration said (asserts, profiling,
  debug/release, log level) when a consumer compiles and runs it;
- every installed CMake file is relocatable: none names the source tree, the build tree or the pre-move prefix;
- with the engine's build tree deleted (the nearest local stand-in for a clean machine), a consumer that only calls
  `find_package(Cerid CONFIG REQUIRED)` compiles, links and runs `Cerid::core` without inheriting the engine's
  warning flags;
- the thin diagnostic application (tests/support/package/diag-consumer) builds against the moved prefix alone, with
  the engine's symbols reaching it (no LNK4099 under `pdb`; the engine's DWARF in the program under `dwarf`), starts
  the engine's DiagnosticHost from a profile authored from the installed assets, reports this binary's capabilities
  (profiling compiled in or out, fault injection compiled out of a shipping build), fails a fatal invariant on
  purpose and leaves a flight bundle, which its own `inspect` route (bundle.inspect) decodes into the trigger at the
  failing line, the forty events before it, this build and its own module identity; the failure runs again with the
  flight policy at the basic detail, where the bundle holds only the five refunds (Warning), and at both details the
  application's event sites (CRD_FLIGHT_RECORD) formatted exactly the messages the recorder kept;
- the application's examples, each a fresh process started from a profile authored from the installed assets, show
  what they must: allocator selection (a TLSF heap or the diagnostic allocator over it, watched by the host, exhausted
  into an allocation-failure bundle; the diagnostic choice also reports a redzone overrun before the failure), jobs
  (the pool configured by the host computes its known total, and a starved main-thread job is captured as a hang),
  reload (live and restart-required profile changes, an edited policy file applied live, and the application's own
  table kept at its last good generation when a seeded read failure is injected through the engine's I/O fault seam;
  no injection without fault seams) and capture (three requests through flight.trigger, each written with the frames
  before and after it, the directory keeping the newest two);
- `CeridConfig.cmake` refuses a consumer configured for another build type, and the engine refuses
  `BUILD_SHARED_LIBS=ON`.

Registered as the `crd-package-consumer` CTest under CRD_PUBLIC_CHECKS (the lane passes its own generator, compiler,
build type and switches); locally `--preset win-debug` resolves the same from CMakePresets.json. The three binaries
of DIAG.11a are this run under a diagnostic profile (`win-debug`), a profiling-disabled one (`win-shipping`) and an
optimized profiling one (`win-shipping-profile`).
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import native_build_profiles  # noqa: E402

MARKER = 'cerid-package-consumer.json'
MULTI_CONFIG = re.compile(r'Visual Studio|Xcode|Multi-Config')
DEBUG_TYPES = ('Debug', 'RelWithDebInfo')
LEVELS = {'Trace': 0, 'Debug': 1, 'Info': 2, 'Warn': 3, 'Error': 4, 'Critical': 5}
FALSE_VALUES = ('', '0', 'OFF', 'NO', 'FALSE', 'N', 'IGNORE', 'NOTFOUND')
CORE_INCLUDE = ROOT / 'engine/foundation/core/include'
# The packaged family (DIAG.11a): the diagnostic host (perf) and its closure, plus math, whose header-only
# transcendental functions the units headers include; crd-simd-flags is the interface target math's consumers compile
# with. Each module installs its include/ tree and one archive.
FAMILY = ('core', 'units', 'time', 'vm', 'log', 'containers', 'memory', 'jobs', 'toml', 'math', 'perf')
INTERFACE_LIBRARIES = ('crd-simd-flags',)
MODULE_INCLUDES = {name: ROOT / 'engine/foundation' / name / 'include' for name in FAMILY}
# Authored assets the packaged libraries read at run time: installed directory under share/cerid -> source.
ASSETS = {'diag': ROOT / 'assets/diag'}
DIAG_CONSUMER = ROOT / 'tests/support/package/diag-consumer'
REQUIRED_FILES = ('include/crd/core/build_config.hpp', 'include/crd/core/core.hpp', 'include/crd/core/platform.hpp',
                  'lib/cmake/Cerid/CeridConfig.cmake', 'lib/cmake/Cerid/CeridConfigVersion.cmake',
                  'lib/cmake/Cerid/CeridTargets.cmake')
FORBIDDEN_FILES = ('include/crd/core/build_config.hpp.in',)
WARNING_FLAGS = ('/WX', '-Werror', '/W4', '-Wall', '-Wextra', '/permissive-')
ENGINE_DEFINES = {'CRD_MODULES': 'perf,math', 'CRD_BUILD_TESTS': 'OFF', 'CRD_BUILD_SANDBOX': 'OFF',
                  'CRD_BUILD_BENCHMARKS': 'OFF', 'CRD_PUBLIC_CHECKS': 'OFF'}
# The diagnostic application's deliberate failure (tests/support/package/diag-consumer/main.cpp).
FAILURE_CODE = 0x00A10002
FAILURE_MESSAGE = 'the order total went negative'
FAILURE_SOURCE = 'CRD_DIAG_INVARIANT(total >= 0'
ORDERS = 40
# The failure runs at both flight details (DIAG.11b's event-site form, CRD_FLIGHT_RECORD). At full every order is
# recorded; at basic only the refunds (Warning) are, and the other orders' sites format no message.
FAILURE_DETAILS = ('full', 'basic')
REFUNDS = tuple(order for order in range(ORDERS) if order % 7 == 6)
RUN_TIMEOUT = 120
# A member of crd::perf::FlightRecorder::Impl, a type defined only in engine/foundation/perf/src/flight_recorder.cpp:
# an application's debug information names it only when the archives' debug information was linked.
ENGINE_ONLY_NAME = b'trigger_replay_length'
# The application's examples (tests/support/package/diag-consumer/example_*.cpp, codes in consumer_support.hpp).
EXAMPLES = ('allocator-tlsf', 'allocator-diagnostic', 'jobs', 'reload', 'capture')
CODE_ORDER_RESERVED = 0x00A10010
CODE_ORDER_RELEASED = 0x00A10011
CODE_HEAP_VIOLATION = 0x00A10012
CODE_SLICE_SUMMED = 0x00A10020
CODE_LIMITS_KEPT = 0x00A10031
CODE_FRAME_WORK = 0x00A10040
CODE_FAULT_INJECTED = 0x0908  # kFlightCodeFaultInjected
# The examples' profile is the installed investigation profile with a short watchdog window; their flight policy is
# the installed default with sixteen post events, a one-second post window (a capture with no later events closes
# by time; a busy one closes by its events long before) and a hitch threshold no frame reaches, so only the example's
# own trigger fires.
EXAMPLE_PROFILE = {'watchdog_period_ms': '50'}
EXAMPLE_POLICY = {'post_events': '16', 'post_window_ns': '1000000000', 'hitch_threshold_ns': '10000000000'}
EXAMPLE_HITCH_NS = 10000000000
CAPTURE_MAX_BUNDLES = 2
CAPTURE_REQUEST_FRAMES = (20, 60, 100)
ORDER_BYTES = 1024
OVERRUN_ORDER = 2
REDZONE_OVERRUN = 1  # crd::memory::ViolationKind::RedzoneOverrun
SLICES = 64
SLICE_LENGTH = 1000
LIMITS = 'kind = "order-limits"\nmax_order = 500\n'
FIELD = re.compile(r'(\w+)=("([^"]*)"|\S+)')

CONSUMER_CMAKE = '''cmake_minimum_required(VERSION 3.25)
project(cerid_consumer LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Cerid CONFIG REQUIRED)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Cerid::core)
message(STATUS "Cerid ${Cerid_VERSION} profile: ${Cerid_PROFILE}; libraries: ${Cerid_LIBRARIES}")
'''

CONSUMER_MAIN = '''#include <crd/core/build_config.hpp>
#include <crd/core/platform.hpp>

#include <cstdio>

int main()
{
    std::printf("version=%d.%d.%d %s\\n", CRD_VERSION_MAJOR, CRD_VERSION_MINOR, CRD_VERSION_PATCH, CRD_VERSION_STRING);
    std::printf("asserts=%d\\n", CRD_ENABLE_ASSERTS);
    std::printf("profiling=%d\\n", CRD_ENABLE_PROFILING);
#if defined(CRD_DEBUG)
    std::printf("debug=1\\n");
#else
    std::printf("debug=0\\n");
#endif
#if defined(CRD_RELEASE)
    std::printf("release=1\\n");
#else
    std::printf("release=0\\n");
#endif
    std::printf("log_min_level=%d\\n", CRD_LOG_MIN_LEVEL_NUM);
    std::printf("platform=%s compiler=%s arch=%s\\n", crd::platform_name(), crd::compiler_name(), crd::arch_name());
    return 0;
}
'''


class ConsumerError(Exception):
    pass


def on(value):
    """CMake truth of a cache value."""
    text = str(value).strip().upper()
    return text not in FALSE_VALUES and not text.endswith('-NOTFOUND')


def normalized(text):
    text = text.replace('\\', '/')
    return text.lower() if os.name == 'nt' else text


def run(command, cwd, timeout=1800):
    return subprocess.run([str(part) for part in command], cwd=str(cwd), capture_output=True, text=True,
                          encoding='utf-8', errors='replace', timeout=timeout)


def output_tail(result, lines=60):
    return '\n'.join((result.stdout + result.stderr).splitlines()[-lines:])


def project_version():
    text = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    match = re.search(r'project\(\s*\w+\s+VERSION\s+([0-9.]+)', text)
    if not match:
        raise ConsumerError('the root CMakeLists.txt declares no project VERSION')
    return match.group(1)


def preset_value(value):
    return value['value'] if isinstance(value, dict) else value


def profile_from_preset(name):
    document = json.loads((ROOT / 'CMakePresets.json').read_text(encoding='utf-8'))
    resolved = native_build_profiles.resolve(document, name)
    values = {key: preset_value(value) for key, value in resolved['cacheVariables'].items()}
    generator = resolved.get('generator', 'Ninja')
    return {'generator': generator,
            'make_program': shutil.which('ninja') if 'Ninja' in generator else None,
            'cxx_compiler': values.pop('CMAKE_CXX_COMPILER', None),
            'c_compiler': values.pop('CMAKE_C_COMPILER', None),
            'build_type': values.pop('CMAKE_BUILD_TYPE', ''),
            'defines': values}


def profile_from_arguments(args):
    defines = {}
    for item in args.define:
        key, _, value = item.partition('=')
        defines[key] = value
    return {'generator': args.generator, 'make_program': args.make_program, 'cxx_compiler': args.cxx_compiler,
            'c_compiler': None, 'build_type': args.build_type, 'defines': defines}


def cmake_arguments(profile, extra, build_type=None, switches=True):
    """Configure arguments of the profile: generator, make program, compilers, build type and, with switches, the
    profile's own cache entries (the engine's CRD_* switches); extra entries come last."""
    arguments = ['-G', profile['generator']]
    if profile.get('make_program'):
        arguments.append('-DCMAKE_MAKE_PROGRAM=' + profile['make_program'])
    if profile.get('cxx_compiler'):
        arguments.append('-DCMAKE_CXX_COMPILER=' + profile['cxx_compiler'])
    if profile.get('c_compiler'):
        arguments.append('-DCMAKE_C_COMPILER=' + profile['c_compiler'])
    build_type = profile['build_type'] if build_type is None else build_type
    if build_type:
        arguments.append('-DCMAKE_BUILD_TYPE=' + build_type)
    defines = {**profile['defines'], **extra} if switches else dict(extra)
    for key, value in defines.items():
        arguments.append(f'-D{key}={value}')
    return arguments


def expected_values(profile):
    defines = profile['defines']
    debug = profile['build_type'] in DEBUG_TYPES
    level = defines.get('CRD_LOG_MIN_LEVEL', '') or ('Trace' if debug else 'Info')
    if level not in LEVELS:
        raise ConsumerError(f'unknown CRD_LOG_MIN_LEVEL {level!r}')
    return {'asserts': int(on(defines.get('CRD_ENABLE_ASSERTS', 'ON'))),
            'profiling': int(on(defines.get('CRD_ENABLE_PROFILING', 'OFF'))),
            'debug': int(debug), 'release': int(not debug), 'log_min_level': LEVELS[level]}


def parse_output(text):
    values = {}
    for line in text.splitlines():
        for item in line.split():
            key, sep, value = item.partition('=')
            if sep:
                values[key] = value
    return values


def archive_module(path):
    """The module an installed archive belongs to (crd-perf.lib, libcrd-perf.a -> perf), or None."""
    match = re.fullmatch(r'(?:lib)?crd-([a-z0-9-]+)\.(?:lib|a)', path.name)
    return match.group(1) if match else None


def check_layout(prefix, modules=FAMILY, debug_information=None, assets=None):
    """Problems of an installed prefix and its archives. Every public header of `modules` and nothing else, one archive
    per module and no other, the debug information `CeridConfig.cmake` declares (None: not judged) and the `assets`
    (installed directory under share/cerid -> source directory) byte for byte."""
    assets = ASSETS if assets is None else assets
    problems = []
    for name in REQUIRED_FILES:
        if not (prefix / name).is_file():
            problems.append(f'missing {name}')
    for name in FORBIDDEN_FILES:
        if (prefix / name).exists():
            problems.append(f'installed {name}')
    expected = set()
    for module in modules:
        include = MODULE_INCLUDES[module]
        expected |= {path.relative_to(include).as_posix() for path in include.rglob('*')
                     if path.is_file() and path.suffix != '.in'}
    installed = set()
    if (prefix / 'include').is_dir():
        installed = {path.relative_to(prefix / 'include').as_posix() for path in (prefix / 'include').rglob('*')
                     if path.is_file() and path.suffix != '.in'}
    installed.discard('crd/core/build_config.hpp')
    for name in sorted(expected - installed):
        problems.append(f'public header not installed: {name}')
    for name in sorted(installed - expected):
        problems.append(f'unexpected installed header: {name}')

    lib = prefix / 'lib'
    archives = sorted(path for path in lib.iterdir() if path.suffix in ('.lib', '.a')) if lib.is_dir() else []
    libraries = [path.name for path in archives]
    found = {archive_module(path): path for path in archives}
    for module in modules:
        if module not in found:
            problems.append(f'no crd-{module} archive under lib/')
    for path in archives:
        if archive_module(path) not in modules:
            problems.append(f'unexpected archive under lib/: {path.name}')
    pdbs = sorted(path.name for path in lib.glob('*.pdb')) if lib.is_dir() else []
    if debug_information == 'pdb':
        for module in modules:
            if module in found and not (lib / f'crd-{module}.pdb').is_file():
                problems.append(f'debug information is pdb but crd-{module}.pdb is not installed beside its archive')
        for name in pdbs:
            if Path(name).stem.removeprefix('crd-') not in modules:
                problems.append(f'unexpected PDB under lib/: {name}')
    elif debug_information is not None and pdbs:
        problems.append(f'debug information is {debug_information} but PDBs are installed: {", ".join(pdbs)}')

    for destination, source in assets.items():
        installed_dir = prefix / 'share/cerid' / destination
        wanted = {path.relative_to(source).as_posix() for path in source.rglob('*') if path.is_file()}
        present = set()
        if installed_dir.is_dir():
            present = {path.relative_to(installed_dir).as_posix() for path in installed_dir.rglob('*')
                       if path.is_file()}
        for name in sorted(wanted - present):
            problems.append(f'asset not installed: share/cerid/{destination}/{name}')
        for name in sorted(present - wanted):
            problems.append(f'unexpected installed asset: share/cerid/{destination}/{name}')
        for name in sorted(wanted & present):
            if (source / name).read_bytes() != (installed_dir / name).read_bytes():
                problems.append(f'installed asset differs from its source: share/cerid/{destination}/{name}')
    return problems, libraries


def package_record(prefix):
    """The Cerid_* values CeridConfig.cmake sets as plain strings (libraries, debug information, assets, profile)."""
    text = (prefix / 'lib/cmake/Cerid/CeridConfig.cmake').read_text(encoding='utf-8')
    return dict(re.findall(r'^set\((Cerid_\w+) "([^"]*)"\)', text, flags=re.M))


def expected_debug_information(profile):
    """How the archives of this profile carry debug information (cmake/CrdPackage.cmake decides the same)."""
    defines = profile['defines']
    shipping = on(defines.get('CRD_SHIPPING', 'OFF'))
    if profile['build_type'] not in DEBUG_TYPES and not shipping:
        return 'none'
    compiler = Path(profile.get('cxx_compiler') or ('cl' if os.name == 'nt' else 'c++')).name.lower()
    compiler = compiler.removesuffix('.exe')
    if compiler == 'cl':
        # Under the compiler launcher every configuration compiles /Z7, Shipping included (CRD_EMBEDDED_DEBUG_INFO).
        launcher = on(defines.get('CRD_COMPILER_LAUNCHER', ''))
        return 'embedded' if launcher else 'pdb'
    if compiler == 'clang-cl':
        return 'embedded'
    return 'dwarf'


def elf_sections(path, names):
    """The bytes of the ELF sections named in `names` that the file has, concatenated (empty when it has none), or
    None when the file is not a 64-bit ELF. A section of type NOBITS reads as empty."""
    data = Path(path).read_bytes()
    if len(data) < 64 or data[:4] != b'\x7fELF' or data[4] != 2:
        return None
    order = '<' if data[5] == 1 else '>'
    (section_offset,) = struct.unpack_from(order + 'Q', data, 0x28)
    entry_size, count, names_index = struct.unpack_from(order + 'HHH', data, 0x3A)
    headers = [struct.unpack_from(order + 'IIQQQQ', data, section_offset + i * entry_size) for i in range(count)]
    table_offset = headers[names_index][4]
    by_name = {}
    for name_offset, kind, _flags, _address, offset, size in headers:
        start = table_offset + name_offset
        name = data[start:data.index(b'\0', start)].decode('ascii', errors='replace')
        by_name[name] = b'' if kind == 8 else data[offset:offset + size]
    return b''.join(by_name[name] for name in names if name in by_name)


def engine_debug_information_problem(executable, debug_information):
    """Whether the engine's own debug information reached a program linked against the package, or why not (None when
    it did, or when the package declares none). The probe is ENGINE_ONLY_NAME, a member of a type defined only inside
    an engine source file: the program's own translation units never see it, so its program database (pdb, embedded)
    or its DWARF (dwarf) names it only when the archives' debug information was found and linked."""
    executable = Path(executable)
    if debug_information in ('pdb', 'embedded'):
        database = executable.with_suffix('.pdb')
        if not database.is_file():
            return f'the application has no program database {database.name}'
        if ENGINE_ONLY_NAME not in database.read_bytes():
            return (f'the engine archives\' debug information did not reach {database.name} (no '
                    f'{ENGINE_ONLY_NAME.decode()}); a compile PDB is missing beside its archive (LNK4099)')
    elif debug_information == 'dwarf':
        sections = elf_sections(executable, ('.debug_info', '.debug_str', '.debug_line_str'))
        if sections is None:
            return f'{executable.name} is not a 64-bit ELF program'
        if ENGINE_ONLY_NAME not in sections:
            return f'the application carries no engine DWARF (no {ENGINE_ONLY_NAME.decode()})'
    return None


def inspect_pages(output):
    """The JSON documents a consumer's `inspect` printed, one per page."""
    return [json.loads(line.split(' ', 1)[1]) for line in output.splitlines()
            if line.startswith('CONSUMER_INSPECT_PAGE ')]


def capability(document, name):
    """The diag.capabilities item of one mode, or None."""
    return next((item for item in document.get('items', []) if item.get('name') == name), None)


def kept_orders(detail):
    """The orders the failure's flight recorder keeps at `detail`: every one at full, the refunds at basic."""
    return tuple(range(ORDERS)) if detail == 'full' else REFUNDS


def investigation_problems(pages, failure_line, executable_name, fault_seams, detail='full'):
    """What a bundle.inspect of the consumer's flight bundle, recorded at `detail`, must show, as problems."""
    kept = kept_orders(detail)
    problems = []
    if not pages:
        return ['inspect printed no page']
    summary = pages[0].get('summary', {})
    items = [item for page in pages for item in page.get('items', [])]
    if pages[-1].get('status') != 'ok':
        problems.append(f'bundle.inspect answered {pages[-1].get("status")}')
    for key, value in (('status', 'ok'), ('flight_record', True), ('flight_refused', False), ('build_identity', True)):
        if summary.get(key) != value:
            problems.append(f'summary {key}: expected {value!r}, got {summary.get(key)!r}')
    trigger = next((item for item in items if item.get('kind') == 'trigger'), None)
    if trigger is None:
        problems.append('no trigger item')
    else:
        file = trigger.get('file', '').replace('\\', '/')
        expected = {'trigger': 'fatal-invariant', 'code': FAILURE_CODE, 'line': failure_line,
                    'message': FAILURE_MESSAGE}
        for key, value in expected.items():
            if trigger.get(key) != value:
                problems.append(f'trigger {key}: expected {value!r}, got {trigger.get(key)!r}')
        if not file.endswith('diag-consumer/main.cpp'):
            problems.append(f'trigger file does not name the consumer source: {file!r}')
    flight = next((item for item in items if item.get('kind') == 'flight'), None)
    if flight is None:
        problems.append('no flight item')
    else:
        for key, value in (('end', 'fatal'), ('detail', detail), ('recorded_before_trigger', len(kept)),
                           ('pre_retained', len(kept)), ('events', len(kept)), ('replay_available', True),
                           ('replay', 'diag-consumer fail <profile.host.toml>')):
            if flight.get(key) != value:
                problems.append(f'flight {key}: expected {value!r}, got {flight.get(key)!r}')
    events = [item for item in items if item.get('kind') == 'event']
    if len(events) != len(kept):
        problems.append(f'expected {len(kept)} events, got {len(events)}')
    else:
        for index, (event, order) in enumerate(zip(events, kept)):
            amount = -900 if order % 7 == 6 else 25
            wanted = {'index': index, 'side': 'before', 'code': 0x00A10001, 'value_a': order,
                      'value_b': 1 if amount < 0 else 0, 'message': f'order {order} booked {amount}',
                      'severity': 'warning' if amount < 0 else 'info'}
            wrong = {key: event.get(key) for key, value in wanted.items() if event.get(key) != value}
            if wrong:
                problems.append(f'event {index}: expected {wanted}, got {wrong}')
                break
    build = next((item for item in items if item.get('kind') == 'build'), None)
    if build is None or build.get('same_as_host') is not True:
        problems.append(f'the bundle was not written by this build: {build}')
    elif build.get('fault_seams') is not fault_seams:
        problems.append(f'build fault_seams: expected {fault_seams}, got {build.get("fault_seams")}')
    names = [item.get('name', '').lower() for item in items if item.get('kind') == 'symbol']
    if executable_name.lower() not in names:
        problems.append(f'no module identity names {executable_name}: {names}')
    return problems


def sites_problems(output, detail):
    """What the failure's CONSUMER_SITES line must say at `detail`, as problems: the application's sites formatted the
    message of exactly the orders the recorder kept, and the recorder left the others out unformatted."""
    kept = len(kept_orders(detail))
    return field_problems('CONSUMER_SITES', single(output, 'CONSUMER_SITES'),
                          {'detail': detail, 'formatted': str(kept), 'recorded': str(kept),
                           'filtered': str(ORDERS - kept)})


def author_assignments(text, values, what):
    """`text` (a TOML asset) with each `key = ...` line of `values` replaced by `key = <value>` (a TOML literal)."""
    for key, value in values.items():
        text, count = re.subn(rf'^{key}\s*=.*$', f'{key} = {value}', text, flags=re.M)
        if count != 1:
            raise ConsumerError(f'the installed {what} has no single {key} line')
    return text


def marker_fields(output, marker):
    """The key=value fields of every line that starts with `marker` (a value may be "quoted"), in order."""
    found = []
    for line in output.splitlines():
        if line.startswith(marker + ' '):
            fields = {}
            for match in FIELD.finditer(line[len(marker) + 1:]):
                fields.setdefault(match.group(1), match.group(3) if match.group(3) is not None else match.group(2))
            found.append(fields)
    return found


def command_statuses(output, command):
    """The statuses the application printed for each run of `command` (CONSUMER_COMMAND lines), in order."""
    return [fields.get('status') for fields in marker_fields(output, 'CONSUMER_COMMAND')
            if fields.get('command') == command]


def bundle_items(pages):
    """The trigger item, the flight item and the event items of a bundle.inspect answer."""
    items = [item for page in pages or [] for item in page.get('items', [])]
    trigger = next((item for item in items if item.get('kind') == 'trigger'), None)
    flight = next((item for item in items if item.get('kind') == 'flight'), None)
    return trigger, flight, [item for item in items if item.get('kind') == 'event']


def field_problems(what, fields, wanted):
    """Problems of one printed line against the values it must carry (None: the line is missing)."""
    if fields is None:
        return [f'{what}: line missing']
    return [f'{what} {key}: expected {value!r}, got {fields.get(key)!r}' for key, value in wanted.items()
            if fields.get(key) != value]


def single(output, marker):
    lines = marker_fields(output, marker)
    return lines[0] if len(lines) == 1 else None


def allocator_problems(output, pages, choice):
    """What the allocator-selection example must show for `choice` ('tlsf' or 'diagnostic'), as problems."""
    chosen = f'orders-{choice}'
    problems = field_problems('CONSUMER_ALLOCATOR', single(output, 'CONSUMER_ALLOCATOR'),
                              {'chosen': chosen, 'order_bytes': str(ORDER_BYTES)})
    exhausted = single(output, 'CONSUMER_ALLOC_EXHAUSTED')
    problems += field_problems('CONSUMER_ALLOC_EXHAUSTED', exhausted, {'exhausted': '1'})
    booked = int(exhausted.get('booked', '0')) if exhausted else 0
    if booked <= OVERRUN_ORDER:
        problems.append(f'only {booked} orders were booked before the heap was exhausted')
    problems += field_problems('CONSUMER_BUNDLE', single(output, 'CONSUMER_BUNDLE'),
                               {'label': 'allocation-failure', 'name': 'flight-0.cdb', 'failures': '0'})
    diagnostic = marker_fields(output, 'CONSUMER_ALLOC_DIAGNOSTIC')
    if choice == 'diagnostic':
        problems += field_problems('CONSUMER_ALLOC_CHECKED', single(output, 'CONSUMER_ALLOC_CHECKED'),
                                   {'violations': '1'})
        summary = diagnostic[0] if len(diagnostic) == 1 else None
        problems += field_problems('CONSUMER_ALLOC_DIAGNOSTIC', summary,
                                   {'first': 'redzone-overrun', 'offset': str(ORDER_BYTES), 'leaks': '0'})
    elif diagnostic or marker_fields(output, 'CONSUMER_ALLOC_CHECKED'):
        problems.append('the tlsf choice reported diagnostic-allocator results')

    trigger, flight, events = bundle_items(pages)
    problems += field_problems('trigger', trigger, {
        'trigger': 'allocation-failure', 'value_a': ORDER_BYTES,
        'message': f'allocator {chosen} failed to allocate {ORDER_BYTES} bytes (alignment 16)'})
    problems += field_problems('flight', flight, {'end': 'post-events-full'})
    before = [event for event in events if event.get('side') == 'before']
    reserved = [event.get('value_a') for event in before if event.get('code') == CODE_ORDER_RESERVED]
    if reserved != list(range(booked)):
        problems.append(f'the bundle does not hold the {booked} bookings before the failure in order: {reserved}')
    released = [event.get('value_a') for event in events
                if event.get('side') == 'after' and event.get('code') == CODE_ORDER_RELEASED]
    if not released or released[0] != 0:
        problems.append(f'the bundle does not hold the releases after the failure: {released}')
    violations = [event for event in before if event.get('code') == CODE_HEAP_VIOLATION]
    if choice == 'diagnostic':
        codes = [(event.get('code'), event.get('value_a')) for event in before]
        at = codes.index((CODE_HEAP_VIOLATION, REDZONE_OVERRUN)) if (CODE_HEAP_VIOLATION, REDZONE_OVERRUN) in codes \
            else -1
        if len(violations) != 1 or violations[0].get('value_b') != ORDER_BYTES:
            problems.append(f'expected one redzone overrun at byte {ORDER_BYTES} before the failure: {violations}')
        elif at < 1 or codes[at - 1] != (CODE_ORDER_RESERVED, OVERRUN_ORDER):
            problems.append(f'the overrun is not recorded right after order {OVERRUN_ORDER} was reserved')
    elif violations:
        problems.append('the tlsf choice recorded heap violations')
    return problems


def jobs_problems(output, pages, watchdog_ms):
    """What the jobs example must show, as problems."""
    n = SLICES * SLICE_LENGTH
    problems = field_problems('CONSUMER_JOBS_POOL', single(output, 'CONSUMER_JOBS_POOL'),
                              {'threads': '4', 'watchdog_ms': str(watchdog_ms)})
    problems += field_problems('CONSUMER_JOBS_SUM', single(output, 'CONSUMER_JOBS_SUM'),
                               {'slices': str(SLICES), 'total': str(n * (n + 1) // 2),
                                'expected': str(n * (n + 1) // 2)})
    problems += field_problems('CONSUMER_JOBS_STALL', single(output, 'CONSUMER_JOBS_STALL'),
                               {'captured': '1', 'late_job_ran_before_pump': '0'})
    problems += field_problems('CONSUMER_BUNDLE', single(output, 'CONSUMER_BUNDLE'),
                               {'label': 'hang', 'name': 'flight-0.cdb', 'failures': '0'})
    problems += field_problems('CONSUMER_JOBS_RECOVERED', single(output, 'CONSUMER_JOBS_RECOVERED'),
                               {'late_job_ran': '1'})
    trigger, _flight, events = bundle_items(pages)
    problems += field_problems('trigger', trigger, {'trigger': 'hang', 'value_a': 0})  # HangKind::ExecutorStarved
    if trigger is not None and not trigger.get('value_b', 0) >= 1:
        problems.append(f'the hang names no outstanding counter: {trigger}')
    slices = sorted((event.get('value_a'), event.get('value_b')) for event in events
                    if event.get('code') == CODE_SLICE_SUMMED and event.get('side') == 'before')
    wanted = [(i, sum(range(i * SLICE_LENGTH + 1, (i + 1) * SLICE_LENGTH + 1))) for i in range(SLICES)]
    if slices != wanted:
        problems.append(f'the bundle does not hold the {SLICES} summed slices before the hang')
    return problems


def reload_problems(output, pages, fault_seams, watchdog_ms):
    """What the reload example must show, as problems. Without fault seams fault.* is not served, nothing is injected
    and every reload loads."""
    problems = []
    data = marker_fields(output, 'CONSUMER_RELOAD_DATA')
    loaded = {'result': 'loaded', 'why': 'none'}
    wanted = [{**loaded, 'attempt': '1', 'generation': '1', 'max_order': '500'}]
    if fault_seams:
        wanted += [{'attempt': '2', 'result': 'kept', 'why': 'read-failed', 'generation': '1', 'max_order': '500'},
                   {**loaded, 'attempt': '3', 'generation': '2', 'max_order': '750'}]
    else:
        wanted += [{**loaded, 'attempt': '2', 'generation': '2', 'max_order': '750'},
                   {**loaded, 'attempt': '3', 'generation': '3', 'max_order': '750'}]
    if len(data) != len(wanted):
        problems.append(f'expected {len(wanted)} reloads of the limits table, got {len(data)}')
    for index, (got, want) in enumerate(zip(data, wanted)):
        problems += field_problems(f'reload {index + 1}', got, want)

    applies = {fields.get('step'): fields for fields in marker_fields(output, 'CONSUMER_RELOAD_APPLY')}
    for step, answer in (('unchanged', 'unchanged'), ('policy-edited', 'applied'),
                         ('watchdog-changed', 'restart-required'), ('capture-dir-changed', 'applied')):
        problems += field_problems(f'apply {step}', applies.get(step),
                                   {'answer': answer, 'watchdog_ms': str(watchdog_ms)})
    if 'unchanged' in applies and 'policy-edited' in applies:
        before, after = applies['unchanged'].get('policy_generation'), applies['policy-edited'].get('policy_generation')
        if not (before or '').isdigit() or after != str(int(before) + 1):
            problems.append(f'the edited policy did not take a new generation ({before} -> {after})')
    if 'watchdog_period_ms' not in applies.get('watchdog-changed', {}).get('reason', ''):
        problems.append('the restart-required answer does not name watchdog_period_ms')
    problems += field_problems('CONSUMER_RELOAD_POLICY', single(output, 'CONSUMER_RELOAD_POLICY'),
                               {'hitch_threshold_ns': str(2 * EXAMPLE_HITCH_NS)})

    if command_statuses(output, 'fault.arm') != ['ok' if fault_seams else 'unknown-command']:
        problems.append(f'fault.arm answered {command_statuses(output, "fault.arm")}')
    bundles = marker_fields(output, 'CONSUMER_BUNDLE')
    if not fault_seams:
        if bundles or pages:
            problems.append('a build without fault seams captured an injected fault')
        return problems
    if command_statuses(output, 'fault.disarm') != ['ok']:
        problems.append(f'fault.disarm answered {command_statuses(output, "fault.disarm")}')
    problems += field_problems('CONSUMER_BUNDLE', bundles[0] if len(bundles) == 1 else None,
                               {'label': 'injected-fault', 'name': 'flight-0.cdb', 'failures': '0'})
    trigger, _flight, events = bundle_items(pages)
    problems += field_problems('trigger', trigger, {'trigger': 'injected-fault'})
    if trigger is not None and not trigger.get('message', '').startswith('fault-plan/1 fired=failed-read#0'):
        problems.append(f'the trigger does not name the fired read: {trigger.get("message")!r}')
    if not any(event.get('side') == 'before' and event.get('code') == CODE_FAULT_INJECTED and
               event.get('message', '').endswith('on limits.toml') for event in events):
        problems.append('no event before the trigger names the injected read of limits.toml')
    if not any(event.get('side') == 'after' and event.get('code') == CODE_LIMITS_KEPT and
               (event.get('value_a'), event.get('value_b')) == (1, 500) for event in events):
        problems.append('no event after the trigger says generation 1 (max_order 500) was kept')
    return problems


def capture_problems(output, pages, bundles):
    """What the capture example must show, as problems; `bundles` are the file names left in its capture directory."""
    requests = len(CAPTURE_REQUEST_FRAMES)
    problems = []
    if command_statuses(output, 'flight.trigger') != ['ok'] * requests:
        problems.append(f'flight.trigger answered {command_statuses(output, "flight.trigger")}')
    if command_statuses(output, 'flight.status') != ['ok']:
        problems.append(f'flight.status answered {command_statuses(output, "flight.status")}')
    problems += field_problems('CONSUMER_CAPTURE', single(output, 'CONSUMER_CAPTURE'),
                               {'requested': str(requests), 'written': str(requests), 'all_written': '1',
                                'max_bundles': str(CAPTURE_MAX_BUNDLES)})
    last = f'flight-{requests - 1}.cdb'
    problems += field_problems('CONSUMER_BUNDLE', single(output, 'CONSUMER_BUNDLE'),
                               {'label': 'manual', 'name': last, 'failures': '0'})
    kept = [f'flight-{i}.cdb' for i in range(requests - CAPTURE_MAX_BUNDLES, requests)]
    if sorted(bundles) != kept:
        problems.append(f'the capture directory keeps {sorted(bundles)}; the newest {CAPTURE_MAX_BUNDLES} are {kept}')
    frame = CAPTURE_REQUEST_FRAMES[-1]
    trigger, flight, events = bundle_items(pages)
    problems += field_problems('trigger', trigger,
                               {'trigger': 'manual', 'message': f'capture requested at frame {frame}'})
    problems += field_problems('flight', flight, {'end': 'post-events-full', 'post_retained': 16})
    before = [event.get('value_a') for event in events
              if event.get('side') == 'before' and event.get('code') == CODE_FRAME_WORK]
    after = [event.get('value_a') for event in events
             if event.get('side') == 'after' and event.get('code') == CODE_FRAME_WORK]
    if not before or before[-1] != frame:
        problems.append(f'the frames before the request do not end at frame {frame}: {before[-3:]}')
    if not after or after[0] != frame + 1 or after != list(range(frame + 1, frame + 1 + len(after))):
        problems.append(f'the frames after the request do not follow frame {frame}: {after}')
    return problems


def relocatability_problems(prefix, forbidden):
    needles = [normalized(str(path)) for path in forbidden]
    problems = []
    for file in sorted(prefix.rglob('*.cmake')):
        text = normalized(file.read_text(encoding='utf-8', errors='replace'))
        for needle in needles:
            if needle in text:
                problems.append(f'{file.relative_to(prefix).as_posix()} names {needle}')
    return problems


def compile_command_flags(build_dir):
    database = build_dir / 'compile_commands.json'
    if not database.is_file():
        raise ConsumerError('the consumer build exported no compile_commands.json')
    entries = json.loads(database.read_text(encoding='utf-8'))
    found = set()
    for entry in entries:
        command = entry.get('command') or ' '.join(entry.get('arguments', []))
        for flag in WARNING_FLAGS:
            if re.search(r'(?<!\S)' + re.escape(flag) + r'(?=\s|$)', command):
                found.add(flag)
    return sorted(found)


class Session:
    def __init__(self, profile, source, scratch, keep, jobs):
        self.profile = profile
        self.source = Path(source).resolve()
        self.scratch = Path(scratch).resolve()
        self.keep = keep
        self.jobs = jobs
        self.report = {'schema': 'cerid-package-consumer/1', 'profile': profile, 'steps': [], 'problems': []}

    def step(self, name, command, cwd, expect_failure=False, timeout=1800):
        started = time.monotonic()
        result = run(command, cwd, timeout=timeout)
        seconds = round(time.monotonic() - started, 1)
        self.report['steps'].append({'name': name, 'seconds': seconds, 'exit_code': result.returncode,
                                     'command': [str(part) for part in command]})
        print(f'[package-consumer] {name}: exit {result.returncode} in {seconds} s', flush=True)
        if expect_failure:
            if result.returncode == 0:
                raise ConsumerError(f'{name} succeeded but was expected to fail:\n{output_tail(result)}')
        elif result.returncode != 0:
            raise ConsumerError(f'{name} failed with exit code {result.returncode}:\n{output_tail(result)}')
        return result

    def prepare_scratch(self):
        if self.scratch.exists():
            if not (self.scratch / MARKER).is_file():
                raise ConsumerError(f'{self.scratch} exists and is not a scratch directory of this test')
            shutil.rmtree(self.scratch)
        self.scratch.mkdir(parents=True)
        (self.scratch / MARKER).write_text(json.dumps({'owner': 'scripts/test-package-consumer.py'}), encoding='utf-8')

    def check_record(self, prefix):
        """CeridConfig.cmake names the packaged libraries and declares the debug information this profile has."""
        record = package_record(prefix)
        self.report['package_record'] = record
        wanted = sorted([f'crd-{module}' for module in FAMILY] + list(INTERFACE_LIBRARIES))
        listed = sorted(record.get('Cerid_LIBRARIES', '').split())
        if listed != wanted:
            raise ConsumerError(f'CeridConfig.cmake lists {listed}; the packaged family is {wanted}')
        debug_information = record.get('Cerid_DEBUG_INFORMATION')
        if debug_information != expected_debug_information(self.profile):
            raise ConsumerError(f'CeridConfig.cmake declares debug information {debug_information!r}; this profile '
                                f'has {expected_debug_information(self.profile)!r}')
        if sorted(record.get('Cerid_ASSETS', '').split()) != sorted(ASSETS):
            raise ConsumerError(f'CeridConfig.cmake lists assets {record.get("Cerid_ASSETS")!r}')
        return debug_information

    def execute(self):
        profile = self.profile
        if MULTI_CONFIG.search(profile['generator']):
            raise ConsumerError(f'{profile["generator"]} is a multi-configuration generator; one installed prefix is one '
                                'profile, so the consumer test runs on single-configuration generators only')
        if not profile['build_type']:
            raise ConsumerError('the profile names no CMAKE_BUILD_TYPE')
        self.prepare_scratch()
        build = self.scratch / 'build'
        prefix = self.scratch / 'prefix'
        moved = self.scratch / 'moved' / 'cerid'
        consumer = self.scratch / 'consumer'
        consumer_build = self.scratch / 'consumer-build'
        version = project_version()

        engine_defines = dict(ENGINE_DEFINES)
        engine_defines['CMAKE_INSTALL_PREFIX'] = prefix.as_posix()
        self.step('configure engine (the diagnostic family)', ['cmake', '-S', self.source, '-B', build,
                                                               *cmake_arguments(profile, engine_defines)], self.source)
        self.step('build engine', ['cmake', '--build', build, '--parallel', str(self.jobs)], self.source)
        self.step('install', ['cmake', '--install', build], self.source)

        debug_information = self.check_record(prefix)
        problems, libraries = check_layout(prefix, debug_information=debug_information)
        self.report['installed_libraries'] = libraries
        if problems:
            raise ConsumerError('installed layout:\n  ' + '\n  '.join(problems))
        print(f'[package-consumer] installed layout complete: {", ".join(libraries)}; debug information '
              f'{debug_information}; assets {", ".join(sorted(ASSETS))}', flush=True)

        moved.parent.mkdir(parents=True)
        shutil.move(str(prefix), str(moved))
        problems = relocatability_problems(moved, [self.source, build, prefix])
        if problems:
            raise ConsumerError('installed CMake files are not relocatable:\n  ' + '\n  '.join(problems))
        print('[package-consumer] installed CMake files name no source, build or pre-move path', flush=True)
        # The nearest local stand-in for a clean machine: nothing the engine's build left behind is reachable while the
        # consumers configure, build, link and run (a compile PDB the linker would find at its build path included).
        shutil.rmtree(build)
        print('[package-consumer] engine build tree deleted; the consumers see the moved prefix only', flush=True)

        consumer.mkdir()
        (consumer / 'CMakeLists.txt').write_text(CONSUMER_CMAKE, encoding='utf-8')
        (consumer / 'main.cpp').write_text(CONSUMER_MAIN, encoding='utf-8')
        consumer_defines = {'CMAKE_PREFIX_PATH': moved.as_posix(), 'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON'}
        result = self.step('configure consumer', ['cmake', '-S', consumer, '-B', consumer_build,
                                                  *cmake_arguments(profile, consumer_defines, switches=False)], consumer)
        if f'Cerid {version} profile:' not in result.stdout:
            raise ConsumerError(f'the consumer did not see Cerid {version}:\n{output_tail(result)}')
        self.step('build consumer', ['cmake', '--build', consumer_build, '--parallel', str(self.jobs)], consumer)
        flags = compile_command_flags(consumer_build)
        if flags:
            raise ConsumerError('the consumer inherited engine warning flags: ' + ' '.join(flags))
        executable = next((path for path in consumer_build.rglob('consumer*')
                           if path.is_file() and path.suffix in ('', '.exe')), None)
        if executable is None:
            raise ConsumerError('the consumer executable was not built')
        result = self.step('run consumer', [executable], consumer_build)
        values = parse_output(result.stdout)
        expected = expected_values(profile)
        self.report['consumer_output'] = values
        mismatches = [f'{key}: expected {value}, got {values.get(key)}' for key, value in expected.items()
                      if values.get(key) != str(value)]
        if values.get('version') != version:
            mismatches.append(f'version: expected {version}, got {values.get("version")}')
        if mismatches:
            raise ConsumerError('the consumer saw another profile:\n  ' + '\n  '.join(mismatches))
        print(f'[package-consumer] consumer ran: {result.stdout.strip().splitlines()[-1]}', flush=True)
        print(f'[package-consumer] profile header as packaged: {expected}', flush=True)

        self.diagnostic_application(moved, version, debug_information)

        other = 'Release' if profile['build_type'] in DEBUG_TYPES else 'Debug'
        result = self.step(f'configure consumer as {other} (must be refused)',
                           ['cmake', '-S', consumer, '-B', self.scratch / 'consumer-mismatch',
                            *cmake_arguments(profile, consumer_defines, build_type=other, switches=False)], consumer,
                           expect_failure=True)
        if 'was installed from the' not in result.stdout + result.stderr:
            raise ConsumerError(f'the mismatching consumer failed for another reason:\n{output_tail(result)}')
        result = self.step('configure engine with BUILD_SHARED_LIBS=ON (must be refused)',
                           ['cmake', '-S', self.source, '-B', self.scratch / 'shared',
                            *cmake_arguments(profile, {**ENGINE_DEFINES, 'BUILD_SHARED_LIBS': 'ON'})], self.source,
                           expect_failure=True)
        if 'BUILD_SHARED_LIBS=ON is unsupported' not in result.stdout + result.stderr:
            raise ConsumerError(f'the shared configure failed for another reason:\n{output_tail(result)}')
        self.report['outcome'] = 'pass'

    def diagnostic_application(self, moved, version, debug_information):
        """Build the thin diagnostic application against the moved prefix, let it fail on purpose at each flight
        detail, investigate the bundle it leaves with its own inspect route, and run its examples."""
        profile = self.profile
        source = self.scratch / 'diag-consumer'
        build = self.scratch / 'diag-consumer-build'
        shutil.copytree(DIAG_CONSUMER, source)
        defines = {'CMAKE_PREFIX_PATH': moved.as_posix(), 'CMAKE_EXPORT_COMPILE_COMMANDS': 'ON'}
        result = self.step('configure the diagnostic application',
                           ['cmake', '-S', source, '-B', build, *cmake_arguments(profile, defines, switches=False)],
                           source)
        if f'Cerid {version} profile:' not in result.stdout:
            raise ConsumerError(f'the diagnostic application did not see Cerid {version}:\n{output_tail(result)}')
        match = re.search(r'Cerid asset directory: (.+)', result.stdout)
        asset_dir = Path(match.group(1).strip()) if match else None
        if asset_dir is None or normalized(str(asset_dir)) != normalized(str(moved / 'share/cerid')):
            raise ConsumerError(f'Cerid_ASSET_DIR is {asset_dir}; the moved prefix installs its assets at '
                                f'{moved / "share/cerid"}')
        if f'Cerid debug information: {debug_information}' not in result.stdout:
            raise ConsumerError(f'the diagnostic application saw other debug information:\n{output_tail(result)}')
        result = self.step('build the diagnostic application',
                           ['cmake', '--build', build, '--parallel', str(self.jobs)], source)
        flags = compile_command_flags(build)
        if flags:
            raise ConsumerError('the diagnostic application inherited engine warning flags: ' + ' '.join(flags))
        executable = next((path for path in build.rglob('diag-consumer*')
                           if path.is_file() and path.suffix in ('', '.exe')), None)
        if executable is None:
            raise ConsumerError('the diagnostic application was not built')

        # The engine's symbols reach the application as the package declares, judged in the program's own debug
        # information (CMake's MSVC link step prints LNK4099 only when a link fails, so its output cannot tell).
        problem = engine_debug_information_problem(executable, debug_information)
        if problem:
            raise ConsumerError(problem)
        print(f'[package-consumer] the diagnostic application built against the moved prefix; the engine\'s debug '
              f'information ({debug_information}) reached its symbols', flush=True)

        fault_seams = not on(profile['defines'].get('CRD_SHIPPING', 'OFF'))
        self.report['diagnostic_application'] = {}
        for detail in FAILURE_DETAILS:
            self.failure(executable, asset_dir, detail, fault_seams)
        self.examples(executable, asset_dir, fault_seams)

    def failure(self, executable, asset_dir, detail, fault_seams):
        """Run the application to its deliberate failure with its flight recorder at `detail`, and investigate the
        bundle it leaves with its own inspect route."""
        profile = self.profile
        # The application's profile, authored from the installed investigation profile: crash dumps and flight bundles
        # kept in this run's directory, and the installed flight policy (at basic, a copy recording at that detail).
        run_dir = self.scratch / ('diag-run' if detail == 'full' else f'diag-run-{detail}')
        captures = run_dir / 'captures'
        captures.mkdir(parents=True)
        policy = asset_dir / 'diag/flight_default.flight.toml'
        if detail != 'full':
            text = author_assignments(policy.read_text(encoding='utf-8'), {'detail': f'"{detail}"'}, 'flight policy')
            policy = run_dir / 'application.flight.toml'
            policy.write_text(text, encoding='utf-8')
        text = (asset_dir / 'diag/host_investigate.host.toml').read_text(encoding='utf-8')
        replacements = {'crash_dir': f'"{(run_dir / "crashes").as_posix()}"', 'capture_dir': f'"{captures.as_posix()}"',
                        'flight_policy': f'"{policy.as_posix()}"'}
        text = author_assignments(text, replacements, 'investigation profile')
        profile_path = run_dir / 'application.host.toml'
        profile_path.write_text(text, encoding='utf-8')

        try:
            result = self.step(f'run the diagnostic application to its deliberate failure (flight detail {detail})',
                               [executable, 'fail', profile_path], run_dir, expect_failure=True, timeout=RUN_TIMEOUT)
        except subprocess.TimeoutExpired as error:
            raise ConsumerError(f'the diagnostic application did not end within {RUN_TIMEOUT} s') from error
        output = result.stdout + result.stderr
        self.report['diagnostic_application'][detail] = {'exit_code': result.returncode,
                                                         'output': output.splitlines()[-40:]}
        expected = expected_values(profile)
        values = parse_output(next((line for line in output.splitlines() if line.startswith('CONSUMER_BUILD ')), ''))
        build_expected = {'asserts': str(expected['asserts']), 'profiling': str(expected['profiling']),
                          'fault_seams': str(int(fault_seams))}
        mismatches = {key: values.get(key) for key, value in build_expected.items() if values.get(key) != value}
        if mismatches:
            raise ConsumerError(f'the application reports another build: expected {build_expected}, got {mismatches}')
        faults = 'active' if fault_seams else 'unavailable'
        started = f'CONSUMER_HOST_STARTED crash=active flight=active faults={faults}'
        if started not in output:
            raise ConsumerError(f'the host did not start as the profile asks ({started}):\n{output_tail(result)}')
        if 'CONSUMER_RETURNED_FROM_FATAL' in output:
            raise ConsumerError('the fatal invariant returned to the application')
        caps_line = next((line for line in output.splitlines() if line.startswith('CONSUMER_CAPABILITIES ')), None)
        if caps_line is None:
            raise ConsumerError(f'the application printed no capabilities:\n{output_tail(result)}')
        caps = json.loads(caps_line.split(' ', 1)[1])
        for name, compiled in (('basic-recording', bool(expected['profiling'])), ('fault-injection', fault_seams),
                               ('flight-recorder', True)):
            item = capability(caps, name)
            if item is None or item.get('compiled') is not compiled:
                raise ConsumerError(f'diag.capabilities {name}: expected compiled={compiled}, got {item}')
        if capability(caps, 'flight-recorder').get('usable') is not True:
            raise ConsumerError('the live flight recorder is not usable')
        problems = sites_problems(output, detail)
        if problems:
            raise ConsumerError(f'the application\'s event sites at the {detail} detail:\n  ' + '\n  '.join(problems) +
                                f'\n{output_tail(result)}')
        bundles = sorted(path.name for path in captures.glob('flight-*.cdb'))
        if bundles != ['flight-0.cdb']:
            raise ConsumerError(f'expected one flight bundle in the capture directory, found {bundles}')
        kept = len(kept_orders(detail))
        print(f'[package-consumer] the application failed on purpose (exit {result.returncode}) and left '
              f'{bundles[0]}; profiling compiled={bool(expected["profiling"])}, fault injection {faults}; at the '
              f'{detail} detail its sites formatted {kept} of {ORDERS} messages', flush=True)

        result = self.step(f"investigate the {detail} bundle with the application's inspect route",
                           [executable, 'inspect', captures, bundles[0]], run_dir, timeout=RUN_TIMEOUT)
        failure_line = next(number for number, line in
                            enumerate((DIAG_CONSUMER / 'main.cpp').read_text(encoding='utf-8').splitlines(), 1)
                            if FAILURE_SOURCE in line)
        problems = investigation_problems(inspect_pages(result.stdout), failure_line, executable.name, fault_seams,
                                          detail)
        if problems:
            raise ConsumerError(f'the investigation at the {detail} detail did not find the failure:\n  ' +
                                '\n  '.join(problems))
        print(f'[package-consumer] bundle.inspect named the fatal invariant at main.cpp:{failure_line}, the {kept} '
              f"orders the {detail} detail kept before it, this build and the application's own module", flush=True)

    def examples(self, executable, asset_dir, fault_seams):
        """Run the application's examples (allocator selection, jobs, reload, capture), each in a fresh process from a
        profile authored from the installed assets, and judge what each prints and leaves, reading every bundle back
        through the application's own inspect route."""
        policy_text = author_assignments((asset_dir / 'diag/flight_default.flight.toml').read_text(encoding='utf-8'),
                                         EXAMPLE_POLICY, 'flight policy')
        investigate = (asset_dir / 'diag/host_investigate.host.toml').read_text(encoding='utf-8')
        watchdog_ms = int(EXAMPLE_PROFILE['watchdog_period_ms'])
        self.report['examples'] = {}
        for name in EXAMPLES:
            run_dir = self.scratch / 'examples' / name
            captures = run_dir / 'captures'
            captures.mkdir(parents=True)
            policy = run_dir / 'example.flight.toml'
            policy.write_text(policy_text, encoding='utf-8')
            values = {**EXAMPLE_PROFILE, 'crash_dir': f'"{(run_dir / "crashes").as_posix()}"',
                      'capture_dir': f'"{captures.as_posix()}"', 'flight_policy': f'"{policy.as_posix()}"'}
            if name == 'capture':
                values['max_bundles'] = str(CAPTURE_MAX_BUNDLES)
            profile = run_dir / 'example.host.toml'
            profile.write_text(author_assignments(investigate, values, 'investigation profile'), encoding='utf-8')
            route, _, choice = name.partition('-')
            arguments = [route, profile, *([choice] if choice else [])]
            second = run_dir / 'captures-after-apply'
            if route == 'reload':
                second.mkdir()
                (run_dir / 'limits.toml').write_text(LIMITS, encoding='utf-8')
                arguments = [route, profile, run_dir / 'limits.toml', second]
            try:
                result = self.step(f'run the {name} example', [executable, *arguments], run_dir, timeout=RUN_TIMEOUT)
            except subprocess.TimeoutExpired as error:
                raise ConsumerError(f'the {name} example did not end within {RUN_TIMEOUT} s') from error
            output = result.stdout + result.stderr
            self.report['examples'][name] = output.splitlines()[-40:]
            written = second if route == 'reload' else captures
            bundles = sorted(path.name for path in written.glob('flight-*.cdb'))
            pages = None
            if bundles:
                inspected = self.step(f'inspect the {name} example\'s last bundle',
                                      [executable, 'inspect', written, bundles[-1]], run_dir, timeout=RUN_TIMEOUT)
                pages = inspect_pages(inspected.stdout)
            if route == 'allocator':
                problems = allocator_problems(output, pages, choice)
            elif route == 'jobs':
                problems = jobs_problems(output, pages, watchdog_ms)
            elif route == 'reload':
                problems = reload_problems(output, pages, fault_seams, watchdog_ms)
                left = sorted(path.name for path in captures.glob('flight-*.cdb'))
                if left:
                    problems.append(f'a bundle was written to the capture directory the apply replaced: {left}')
            else:
                problems = capture_problems(output, pages, bundles)
            if problems:
                raise ConsumerError(f'the {name} example did not show what it must:\n  ' + '\n  '.join(problems) +
                                    f'\n{output_tail(result)}')
            print(f'[package-consumer] the {name} example showed what it must ({", ".join(bundles) or "no bundle"})',
                  flush=True)

    def finish(self, failed):
        (self.scratch / 'report.json').write_text(json.dumps(self.report, indent=2), encoding='utf-8')
        if self.keep or failed:
            print(f'[package-consumer] scratch kept at {self.scratch}', flush=True)
        else:
            shutil.rmtree(self.scratch, ignore_errors=True)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--source', default=str(ROOT), help='engine source tree (default: this repository)')
    parser.add_argument('--scratch', required=True, help='scratch directory owned by this test (recreated)')
    parser.add_argument('--preset', help='resolve generator, compiler, build type and switches from this configure preset')
    parser.add_argument('--generator', default='Ninja')
    parser.add_argument('--make-program')
    parser.add_argument('--cxx-compiler')
    parser.add_argument('--build-type', default='')
    parser.add_argument('--define', action='append', default=[], metavar='KEY=VALUE',
                        help='profile switch of the packaging configuration (repeatable)')
    parser.add_argument('--jobs', type=int, default=max(1, min(16, os.cpu_count() or 1)))
    parser.add_argument('--keep', action='store_true', help='keep the scratch directory after a pass')
    args = parser.parse_args(argv)
    try:
        profile = profile_from_preset(args.preset) if args.preset else profile_from_arguments(args)
        session = Session(profile, args.source, args.scratch, args.keep, args.jobs)
    except (ConsumerError, ValueError) as error:
        print(f'[package-consumer] FAIL: {error}', file=sys.stderr)
        return 1
    failed = False
    try:
        session.execute()
    except (ConsumerError, subprocess.TimeoutExpired) as error:
        failed = True
        session.report['outcome'] = 'fail'
        session.report['problems'].append(str(error))
        print(f'[package-consumer] FAIL: {error}', file=sys.stderr)
    finally:
        session.finish(failed)
    if not failed:
        print('[package-consumer] PASS', flush=True)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
