#!/usr/bin/env python3
"""Install a core-only Cerid package, move the prefix, and build and run a downstream project against it.

Contract: docs/design/public-consumption.md. One run proves, for one profile (generator, compiler, build type,
profile switches):

- the installed tree is complete and carries no template (`build_config.hpp.in`) and every public header;
- the generated `<crd/core/build_config.hpp>` says what the packaging configuration said (asserts, profiling,
  debug/release, log level) when a consumer compiles and runs it;
- every installed CMake file is relocatable: none names the source tree, the build tree or the pre-move prefix;
- a consumer that only calls `find_package(Cerid CONFIG REQUIRED)` compiles, links and runs `Cerid::core`
  without inheriting the engine's warning flags;
- `CeridConfig.cmake` refuses a consumer configured for another build type, and the engine refuses
  `BUILD_SHARED_LIBS=ON`.

Registered as the `crd-package-consumer` CTest under CRD_PUBLIC_CHECKS (the lane passes its own generator, compiler,
build type and switches); locally `--preset win-debug` resolves the same from CMakePresets.json.
"""
import argparse
import json
import os
import re
import shutil
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
REQUIRED_FILES = ('include/crd/core/build_config.hpp', 'include/crd/core/core.hpp', 'include/crd/core/platform.hpp',
                  'lib/cmake/Cerid/CeridConfig.cmake', 'lib/cmake/Cerid/CeridConfigVersion.cmake',
                  'lib/cmake/Cerid/CeridTargets.cmake')
FORBIDDEN_FILES = ('include/crd/core/build_config.hpp.in',)
WARNING_FLAGS = ('/WX', '-Werror', '/W4', '-Wall', '-Wextra', '/permissive-')
ENGINE_DEFINES = {'CRD_MODULES': 'core', 'CRD_BUILD_TESTS': 'OFF', 'CRD_BUILD_SANDBOX': 'OFF',
                  'CRD_BUILD_BENCHMARKS': 'OFF', 'CRD_PUBLIC_CHECKS': 'OFF'}

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


def check_layout(prefix):
    problems = []
    for name in REQUIRED_FILES:
        if not (prefix / name).is_file():
            problems.append(f'missing {name}')
    for name in FORBIDDEN_FILES:
        if (prefix / name).exists():
            problems.append(f'installed {name}')
    expected = {path.relative_to(CORE_INCLUDE).as_posix() for path in CORE_INCLUDE.rglob('*')
                if path.is_file() and path.suffix != '.in'}
    installed = {path.relative_to(prefix / 'include').as_posix() for path in (prefix / 'include').rglob('*')
                 if path.is_file() and path.suffix != '.in'}
    installed.discard('crd/core/build_config.hpp')
    for name in sorted(expected - installed):
        problems.append(f'public header not installed: {name}')
    for name in sorted(installed - expected):
        problems.append(f'unexpected installed header: {name}')
    libraries = [path.name for path in (prefix / 'lib').glob('*crd-core*')] if (prefix / 'lib').is_dir() else []
    if not libraries:
        problems.append('no crd-core archive under lib/')
    return problems, sorted(libraries)


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

    def step(self, name, command, cwd, expect_failure=False):
        started = time.monotonic()
        result = run(command, cwd)
        seconds = round(time.monotonic() - started, 1)
        self.report['steps'].append({'name': name, 'seconds': seconds, 'exit_code': result.returncode,
                                     'command': [str(part) for part in command]})
        print(f'[package-consumer] {name}: exit {result.returncode} in {seconds} s', flush=True)
        if expect_failure:
            if result.returncode == 0:
                raise ConsumerError(f'{name} succeeded but was expected to fail')
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
        self.step('configure engine (core only)', ['cmake', '-S', self.source, '-B', build,
                                                   *cmake_arguments(profile, engine_defines)], self.source)
        self.step('build engine', ['cmake', '--build', build, '--parallel', str(self.jobs)], self.source)
        self.step('install', ['cmake', '--install', build], self.source)

        problems, libraries = check_layout(prefix)
        self.report['installed_libraries'] = libraries
        if problems:
            raise ConsumerError('installed layout:\n  ' + '\n  '.join(problems))
        print(f'[package-consumer] installed layout complete: {", ".join(libraries)}', flush=True)

        moved.parent.mkdir(parents=True)
        shutil.move(str(prefix), str(moved))
        problems = relocatability_problems(moved, [self.source, build, prefix])
        if problems:
            raise ConsumerError('installed CMake files are not relocatable:\n  ' + '\n  '.join(problems))
        print('[package-consumer] installed CMake files name no source, build or pre-move path', flush=True)

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
