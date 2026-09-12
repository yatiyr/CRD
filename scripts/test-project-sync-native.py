#!/usr/bin/env python3
"""Real CMake/compiler round trips in an isolated fixture; never mutates engine sources."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET

from project_sync.model import MANIFEST, cmake_model
from project_sync.service import (cmake_executable, configure, hidden_process, ingest, state_dir, state_file)
from project_sync.storage import Workspace, atomic_write, json_bytes, read_json


def wait_for(predicate, description, timeout=90):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise RuntimeError('Timed out: ' + description)


def run(root, *arguments):
    subprocess.run(list(arguments), cwd=root, env=dict(os.environ), check=True, timeout=180)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--generator', default='Visual Studio 18 2026' if os.name == 'nt' else 'Ninja')
    parser.add_argument('--ide', action='store_true', help='Also create saved edits through an isolated hidden VS instance')
    args = parser.parse_args()
    repository = Path(__file__).resolve().parents[1]
    native = args.generator.startswith('Visual Studio')
    fixture_base = repository / 'build/project-sync-tests'
    Workspace(repository).path('build/project-sync-tests', internal=True).mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='native-', dir=fixture_base) as temporary:
        root = Path(temporary).resolve()
        if not root.is_relative_to(fixture_base.resolve()):
            raise RuntimeError('Fixture cleanup boundary does not belong to the intended build directory')
        (root / 'scripts').mkdir()
        shutil.copytree(repository / 'scripts/project_sync', root / 'scripts/project_sync',
                        ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copyfile(repository / 'scripts/project-sync.py', root / 'scripts/project-sync.py')
        (root / 'cmake').mkdir()
        for name in ('CrdProjectSync.cmake', 'CrdIdeFolders.cmake'):
            shutil.copyfile(repository / 'cmake' / name, root / 'cmake' / name)
        atomic_write(root / MANIFEST, json_bytes({'version': 1, 'directories': [], 'targets': {}}))
        module = 'engine/numerics/dense'
        def write(name, content):
            atomic_write(root / name, content.encode('utf-8'))
        write(module + '/include/crd/dense/value.hpp', '#pragma once\ninline int value() { return 42; }\n')
        write(module + '/src/main.cpp', '#include <crd/dense/value.hpp>\nint answer() { return value(); }\n')
        write(module + '/src/other.cpp', 'int other() { return 7; }\n')
        write(module + '/CMakeLists.txt', 'add_library(dense STATIC src/main.cpp src/other.cpp include/crd/dense/value.hpp)\n'
              'target_include_directories(dense PUBLIC include)\n'
              'source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES src/main.cpp src/other.cpp include/crd/dense/value.hpp)\n')
        write('CMakeLists.txt', 'cmake_minimum_required(VERSION 3.27)\nproject(Fixture LANGUAGES CXX)\n'
              'include(cmake/CrdProjectSync.cmake)\ninclude(cmake/CrdIdeFolders.cmake)\n'
              'add_subdirectory(engine/numerics/dense engine/dense)\ncrd_apply_project_structure()\n'
              'crd_organize_targets("${PROJECT_SOURCE_DIR}")\n')
        build = root / 'build/native'
        initial = ['-G', args.generator, '-DCMAKE_BUILD_TYPE=Debug']
        if native:
            initial += ['-A', 'x64', '-DCMAKE_CONFIGURATION_TYPES=Debug;Release', '-DCRD_PROJECT_SYNC=ON']
        configure(root, build, initial_args=initial)
        ws = Workspace(root, [build])
        cli = [sys.executable, '-X', 'utf8', str(root / 'scripts/project-sync.py'), '--root', str(root)]
        def edit(operations):
            before = set((root / 'build/project-sync/journal').glob('*/transaction.json'))
            atomic_write(root / 'build/operations.json', json_bytes(operations))
            run(root, *cli, 'edit', '--build', str(build), '--operations', str(root / 'build/operations.json'),
                '--apply', '--regenerate')
            added = set((root / 'build/project-sync/journal').glob('*/transaction.json')) - before
            return max(added, key=lambda path: path.stat().st_mtime_ns).parent.name if added else None
        def compile_target():
            for config in (['Debug', 'Release'] if native else ['Debug']):
                run(root, cmake_executable(), '--build', str(build), '--config', config, '--target', 'dense', '--parallel', '2')
        compile_target()
        if native:
            baseline = read_json(state_file(build))
            project = root / baseline['model']['targets']['dense']['project']
            write(ws.relative(project.parent / 'added.cpp'), 'int added() { return 99; }\n')
            ns = '{http://schemas.microsoft.com/developer/msbuild/2003}'
            if args.ide:
                compiler = Path(os.environ['WINDIR']) / 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
                helper = root / 'build/vs-fixture.exe'
                run(root, str(compiler), '/nologo', '/warn:4', '/warnaserror+', '/platform:x64',
                    '/r:Microsoft.CSharp.dll', '/out:' + str(helper),
                    str(repository / 'scripts/project_sync/vs_fixture.cs'))
                run(root, str(helper), str(root / baseline['solution']['path']), str(project.parent / 'added.cpp'))
            else:
                for path in (project, project.with_suffix('.vcxproj.filters')):
                    tree = ET.parse(path).getroot()
                    item = ET.SubElement(ET.SubElement(tree, ns + 'ItemGroup'), ns + 'ClCompile', Include='added.cpp')
                    if path.suffix == '.filters':
                        ET.SubElement(item, ns + 'Filter').text = 'src'
                    atomic_write(path, ET.tostring(tree))
            # Pending XML exists BEFORE startup. This exercises the real detached watcher lifecycle.
            watcher = hidden_process([*cli, 'watch', '--build', str(build)], root / 'build/watcher.log')
            try:
                def imported():
                    state = read_json(state_file(build), {})
                    return module + '/src/added.cpp' in state.get('model', {}).get('targets', {}).get('dense', {}).get('sources', {})
                wait_for(imported, 'watcher imports saved Add New Item')
                first = state_file(build).stat().st_mtime_ns
                time.sleep(5)
                if state_file(build).stat().st_mtime_ns != first:
                    raise AssertionError('Regeneration fed back into a second generation')
                assert not (project.parent / 'added.cpp').exists()
                if args.ide:
                    assert (root / module / 'empty-from-vs').is_dir()
            finally:
                atomic_write(state_dir(build) / 'stop', b'stop')
                watcher.wait(timeout=20)
                print((root / 'build/watcher.log').read_text(encoding='utf-8', errors='replace'))
            compile_target()
        else:
            edit([{'op': 'add', 'target': 'dense', 'path': module + '/src/added.cpp',
                   'group': 'src', 'content': 'int added() { return 99; }\n'}])
            compile_target()
        edit([{'op': 'remove', 'target': 'dense', 'path': module + '/src/added.cpp'}])
        assert (root / module / 'src/added.cpp').is_file()
        assert module + '/src/added.cpp' not in cmake_model(ws, build)['targets']['dense']['sources']
        deleted = edit([{'op': 'delete', 'target': 'dense', 'path': module + '/src/added.cpp'}])
        assert not (root / module / 'src/added.cpp').exists()
        run(root, *cli, 'recover', deleted, '--build', str(build))
        configure(root, build)
        assert (root / module / 'src/added.cpp').is_file()
        assert module + '/src/added.cpp' not in cmake_model(ws, build)['targets']['dense']['sources']
        edit([{'op': 'delete', 'target': 'dense', 'path': module + '/src/added.cpp'}])
        edit([{'op': 'move', 'source': module + '/include/crd/dense/value.hpp',
               'destination': module + '/include/crd/dense/renamed.hpp'},
              {'op': 'mkdir', 'target': 'dense', 'path': module + '/empty'}])
        assert '#include <crd/dense/renamed.hpp>' in (root / module / 'src/main.cpp').read_text()
        compile_target()
        if native:
            baseline = read_json(state_file(build))
            solution = root / baseline['solution']['path']
            if solution.suffix == '.slnx':
                tree = ET.parse(solution).getroot()
                tree.find("Folder[@Name='/engine/numerics/']").set('Name', '/engine/scientific/')
                atomic_write(solution, ET.tostring(tree))
            else:
                write(ws.relative(solution), solution.read_text(encoding='utf-8-sig').replace('"numerics", "numerics"',
                                                                                            '"scientific", "scientific"'))
            with ws.lock():
                assert ingest(ws, build, apply=True) is not None
            configure(root, build)
        else:
            edit([{'op': 'move', 'source': module, 'destination': 'engine/scientific/dense'}])
        module = 'engine/scientific/dense'
        assert (root / module / 'src/main.cpp').is_file()
        assert (root / module / 'empty').is_dir()
        compile_target()
        if native:
            assert ingest(ws, build) is None
            # Direct CMake invocation exercises the completion observer, not the wrapper's finalize path.
            run(root, cmake_executable(), '-S', str(root), '-B', str(build))
            wait_for(lambda: not (state_dir(build) / 'generation.json').exists(), 'direct CMake generation completion')
            assert ingest(ws, build) is None
        print('PASS: real generator/compiler, add/remove/delete/recover, header/reference/module moves, empty folders'
              + (', watcher startup/no feedback, direct generation observer' if native else ''))
    return 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
