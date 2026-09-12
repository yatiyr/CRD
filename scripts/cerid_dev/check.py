"""Scoped CMake/CTest execution with source identity, existing synchronization guards and sealed evidence."""
from __future__ import annotations

import json
import math
import os
from pathlib import Path
import sys
import time
import uuid
import xml.etree.ElementTree as ET

from .environment import build_environment, cache_values, cmake_tools, doctor
from .evidence import seal
from .process import run_command
from .selection import (SelectionError, buildable_targets, changes_from_git, content_identity, load_model,
                        select_tests)


def source_identity(root):
    changes, revision = changes_from_git(root)
    return {'revision': revision, 'content': content_identity(root, changes, revision)}


def junit_counts(path, expected):
    """A zero CTest exit is insufficient: reconcile every selected name with the emitted result."""
    try:
        tree = ET.parse(path)
    except (OSError, ET.ParseError) as error:
        raise SelectionError(f'CTest did not produce valid JUnit evidence: {error}') from error
    cases = list(tree.getroot().iter('testcase'))
    names = [case.get('name') for case in cases]
    if len(names) != len(set(names)) or set(names) != set(expected):
        raise SelectionError('JUnit names do not exactly match the selected CTest inventory')
    counts = {'selected': len(expected), 'reported': len(cases), 'executed': 0, 'passed': 0, 'failed': 0,
              'skipped': 0, 'disabled': sum(test['disabled'] for test in expected.values())}
    for case in cases:
        if case.find('skipped') is not None or case.get('status') in {'notrun', 'disabled'}:
            counts['skipped'] += 1
        elif case.find('failure') is not None or case.find('error') is not None:
            counts['executed'] += 1
            counts['failed'] += 1
        else:
            counts['executed'] += 1
            counts['passed'] += 1
    if not counts['reported']:
        raise SelectionError('Zero CTest results cannot qualify an executable')
    return counts


def test_indices(inventory, selected):
    names = {item['name'] for item in selected}
    indices = [index for index, item in enumerate(inventory['tests'], 1) if item['name'] in names]
    if not indices or len(indices) != len(names):
        raise SelectionError('Selected CTest indices do not match the fresh discovery')
    # Supported by CTest 3.25. Use a singleton range plus explicit indices; never its default all-tests range.
    return ','.join(map(str, [indices[0], indices[0], 1, *indices[1:]])) + '\n'


def sync_ready(root, build):
    """Use the owner's lock/IDE guard; never duplicate its transaction engine or import edits behind its back."""
    from project_sync import ide
    from project_sync.service import generation_idle, ingest, registrations
    from project_sync.storage import Conflict, Workspace, incomplete
    ws = Workspace(root)
    with ws.lock():
        if incomplete(ws):
            raise Conflict('Recover the unfinished project transaction before verification')
        generation_idle(ws)
        for active in registrations(ws):
            state = ide.ready(ws, active)
            unsaved = [item['path'] for item in state.get('documents', []) if not item['saved']]
            if unsaved:
                raise Conflict('Save IDE documents before qualifying their source: ' + ', '.join(unsaved))
            if ingest(Workspace(root, [active]), active, apply=False) is not None:
                raise Conflict('Saved source/IDE structure is awaiting synchronization; let project-sync reconcile it')


def execution_doctor(root, build, configuration):
    report = doctor(root, build, configuration)
    snapshot_issue = 'Watcher recorded a structure conflict; reconcile before execution'
    if snapshot_issue in report['issues']:
        # A writer-lock timeout is persisted by the watcher during guarded generation. Its snapshot may outlive
        # the lock. Re-run the authoritative IDE/journal/generation/source checks; never clear a real conflict.
        sync_ready(root, build)
        report['synchronization']['snapshot_revalidated'] = 'Current guarded IDE/journal/generation/source checks passed'
        report['issues'].remove(snapshot_issue)
        report['environment_ready'] = report['configured'] and not report['issues']
    return report


def native_configure_reason(root, build):
    """A readable File API model does not prove the synchronizer finalized this source projection."""
    from project_sync.service import generation_idle, needs_generation, registrations, state_dir
    from project_sync.storage import Workspace, read_json
    ws = Workspace(root)
    with ws.lock():
        if build not in registrations(ws):
            return None
        generation_idle(ws, [build])
        if read_json(state_dir(build) / 'generation.json'):
            return 'Registered native generation was not finalized; guarded configure must restore its baseline'
        if needs_generation(ws, build):
            return 'Registered native source/configuration inputs changed; guarded configure is required'
    return None


def check(args, make_plan):
    from project_sync.storage import Conflict, Workspace
    from project_sync.model import request_file_api
    root = args.root.resolve(strict=True)
    build = (args.build if args.build.is_absolute() else root / args.build).resolve()
    if build == root or root.is_relative_to(build):
        raise SelectionError('Build directory must not contain the source checkout')
    for name in ('build_timeout', 'discovery_timeout', 'test_timeout'):
        if not math.isfinite(getattr(args, name)) or getattr(args, name) <= 0:
            raise SelectionError(f'{name} must be finite and positive')
    if args.jobs not in (1, 2):
        raise SelectionError('Local builds use one or two compile workers')
    if args.dry_run:
        plan = make_plan(args)
        return {'version': 1, 'kind': 'check', 'status': 'dry_run', 'exit_code': 0, 'plan': plan,
                'requested_targets': sorted(set(args.target)), 'jobs': args.jobs,
                'qualification': 'no commands executed; build and fresh discovery are required'}
    # Separate OS-backed lock from the synchronizer's writer lock, which configure must acquire itself.
    ws = Workspace(root)
    with ws.lock(timeout=0, name='developer-check'):
        directory = build / 'cerid-dev/runs' / (time.strftime('%Y%m%dT%H%M%S') + '-' + uuid.uuid4().hex[:12])
        directory.mkdir(parents=True, exist_ok=False)
        record = {'version': 1, 'kind': 'check', 'root': str(root), 'build': str(build), 'status': 'instrument_failure',
                  'exit_code': 2, 'phases': [], 'evidence_directory': str(directory), 'jobs': args.jobs,
                  'qualification': 'none; no remote or unavailable-hardware qualification'}

        def save(name, value):
            (directory / name).write_text(json.dumps(value, indent=2), encoding='utf-8')

        def run(phase, command, timeout, environment, require_success=True):
            sync_ready(root, build)
            result = run_command(command, root, environment, directory / f'{len(record["phases"]):02d}-{phase}', timeout)
            record['phases'].append({'phase': phase, **result})
            if require_success and result['exit_code']:
                record.update(status=result['status'], exit_code=result['exit_code'])
                raise SelectionError(f'{phase} did not pass ({result["exit_code"]}); inspect {result["log"]}')
            return result

        def guards(plan, environment):
            for script in plan['guards']:
                if not (root / script).is_file():
                    raise SelectionError('Mandatory repository guard is missing: ' + script)
                run(Path(script).stem, [sys.executable, str(root / script)], 120, environment)

        try:
            sync_ready(root, build)
            initial_plan = make_plan(args)
            if initial_plan['scope'] == 'documentation' and not args.target:
                record.update(scope='documentation', scenario=bool(args.path), source_before=source_identity(root))
                save('plan.json', initial_plan)
                guards(initial_plan, dict(os.environ, PYTHONUTF8='1'))
                record['source_after'] = source_identity(root)
                if record['source_after'] != record['source_before']:
                    raise SelectionError('Checkout/revision changed during documentation verification')
                record.update(status='passed', exit_code=0, qualification='documentation guards passed')
                return record
            cache = cache_values(build)
            if cache and Path(cache.get('CMAKE_HOME_DIRECTORY', '')).resolve() != root:
                raise SelectionError('Build cache belongs to another checkout')
            if not cache:
                raise SelectionError('Configure the chosen preset with the existing helper first; check never guesses a toolchain')
            environment, _ = build_environment(root, cache)
            environment['CMAKE_BUILD_PARALLEL_LEVEL'] = str(args.jobs)
            environment['PYTHONUTF8'] = '1'
            cmake, ctest = cmake_tools(cache, environment)
            if not cmake or not ctest:
                raise SelectionError('Matching configured CMake/CTest tools are unavailable')
            model = None
            try:
                model = load_model(root, build, args.config)
            except SelectionError as error:
                record['configure_reason'] = str(error)
            reason = native_configure_reason(root, build)
            if reason:
                record['configure_reason'] = reason
                model = None
            if model is None:
                request_file_api(build)
                if cache.get('CMAKE_GENERATOR', '').startswith('Visual Studio'):
                    if not build.is_relative_to(root / 'build'):
                        raise SelectionError('Native VS configuration must use its existing repository sync workspace')
                    command = [sys.executable, str(root / 'scripts/project-sync.py'), '--root', str(root),
                               'configure', '--build', str(build)]
                else:
                    command = [cmake, '-S', str(root), '-B', str(build)]
                run('configure', command, args.build_timeout, environment)
                model = load_model(root, build, args.config)
            args.config = model['configuration']
            report = execution_doctor(root, build, args.config)
            save('doctor.json', report)
            if report['issues']:
                raise SelectionError('Environment is not ready: ' + '; '.join(report['issues']))
            record['source_before'] = source_identity(root)
            record['model_sha256'] = model['sha256']
            plan = make_plan(args)
            save('plan.json', plan)
            record['scope'] = plan['scope']
            record['scenario'] = bool(args.path or args.target)
            if args.target:
                targets = sorted(set(args.target))
                if any(name not in model['targets'] for name in targets):
                    raise SelectionError('Every explicit target must exist in the current CMake configuration')
                if set(targets) != set(buildable_targets(model['targets'], targets)):
                    raise SelectionError('Explicit local targets must be libraries/executables, never utilities or aggregates')
            elif plan['scope'] == 'full':
                raise SelectionError('Full qualification belongs to CI; specify --target for a recorded diagnostic subset')
            else:
                targets = plan['build_targets']
            if len(targets) > 32:
                raise SelectionError('More than 32 affected build targets requires a smaller explicit diagnostic scope and CI')
            record['build_targets'] = list(targets)
            config = ['--config', args.config] if args.config else []
            test_config = ['-C', args.config] if args.config else []
            if targets:
                run('build', [cmake, '--build', str(build), *config, '--target', *targets, '--parallel', str(args.jobs)],
                    args.build_timeout, environment)
                built = set(targets)
                # Fixture providers can add executable owners. Build each only once, then rediscover all names.
                while True:
                    result = run('discover', [ctest, '--test-dir', str(build), *test_config, '--show-only=json-v1'],
                                 args.discovery_timeout, environment)
                    inventory = json.loads(Path(result['log']).read_text(encoding='utf-8-sig'))
                    selected = select_tests(inventory, model, targets, allow_pending=True)
                    added = {target for test in selected for target in test['targets']} - built
                    if not added:
                        break
                    if len(built | added) > 32:
                        raise SelectionError('Fixture expansion exceeds the local target budget; explicit scope/CI required')
                    run('build-fixtures', [cmake, '--build', str(build), *config, '--target', *sorted(added),
                                          '--parallel', str(args.jobs)], args.build_timeout, environment)
                    built.update(added)
                selected = select_tests(inventory, model, targets)
                save('inventory.json', inventory)
                save('selected-tests.json', selected)
                record['build_targets'] = sorted(built)
                indices = directory / 'test-indices.txt'
                indices.write_text(test_indices(inventory, selected), encoding='utf-8')
                result_path = directory / 'ctest.xml'
                # Honor per-test TIMEOUT over the default. The outer budget permits every selected case its allowance.
                budget = 60 + sum(float(test['properties'].get('TIMEOUT') or args.test_timeout) for test in selected)
                result = run('ctest', [ctest, '--test-dir', str(build), *test_config, '-I', str(indices), '-j', '1',
                                      '--timeout', str(args.test_timeout), '--no-tests=error', '--output-on-failure',
                                      '--output-junit', str(result_path)], budget, environment, require_success=False)
                if result['exit_code']:
                    record.update(status=result['status'], exit_code=result['exit_code'])
                counts = junit_counts(result_path, {test['name']: test for test in selected})
                record['tests'] = counts
                if result['exit_code'] or counts['failed']:
                    record.update(status='failed', exit_code=result['exit_code'] or 1)
                    raise SelectionError('Selected CTests failed; native exit and JUnit evidence retained')
                if counts['skipped'] or counts['disabled']:
                    record.update(status='incomplete', exit_code=3)
                    raise SelectionError('Skipped/disabled tests retain an unqualified gate')
            guards(plan, environment)
            if plan['tidy_files']:
                if os.name != 'nt':
                    raise SelectionError('Portable strict-analysis execution is not yet qualified; changed C++ remains ungated')
                run('tidy', ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                             str(root / 'scripts/tidy-files.ps1'), *plan['tidy_files']], args.build_timeout, environment)
            record['source_after'] = source_identity(root)
            if record['source_after'] != record['source_before']:
                raise SelectionError('Checkout/revision changed during execution; results cannot qualify mixed source states')
            if load_model(root, build, args.config)['sha256'] != model['sha256']:
                raise SelectionError('CMake model changed during execution; rerun against a stable generation')
            record.update(status='passed', exit_code=0,
                          qualification='diagnostic subset passed' if record['scenario'] else 'focused local checks passed')
        except (SelectionError, Conflict, OSError, ValueError) as error:
            record['error'] = str(error)
        except KeyboardInterrupt:
            record.update(status='interrupted', exit_code=130, error='Verification interrupted')
        finally:
            seal(directory, record)
        return record
