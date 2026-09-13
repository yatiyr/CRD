#!/usr/bin/env python3
"""Cerid developer diagnostics and conservative affected-build planning."""
from pathlib import Path
import argparse
import json
import subprocess
import sys

from cerid_dev.selection import (SelectionError, buildable_targets, changes_from_git, content_identity, load_model, relative_name,
                                 select_targets)
from cerid_dev.evidence import EvidenceError
from project_sync.storage import Conflict


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    commands = result.add_subparsers(dest='command', required=True)
    command = commands.add_parser('plan', help='Read-only working-tree or exact CI-revision scope')
    command.add_argument('--build', type=Path, required=True)
    command.add_argument('--config', help='Required for multi-configuration builds')
    command.add_argument('--base', help='Actual CI comparison base commit')
    command.add_argument('--head', help='Actual CI checkout commit; must accompany --base')
    command.add_argument('--path', action='append', default=[], help='Explicit diagnostic scenario; not whole-tree coverage')
    command.add_argument('--full', action='store_true', help='Explain the full CI requirement; never execute it locally')
    command.add_argument('--git-timeout', type=float, default=60,
                         help='Budget in seconds per Git command; raise it for a checkout on a network or 9p mount')
    command.add_argument('--json', action='store_true')
    command = commands.add_parser('check', help='Bounded focused local verification; never a whole-repository sweep')
    command.add_argument('--build', type=Path, required=True)
    command.add_argument('--config')
    command.add_argument('--base')
    command.add_argument('--head')
    command.add_argument('--path', action='append', default=[], help='Explicit diagnostic paths, not whole-tree coverage')
    command.add_argument('--target', action='append', default=[], help='Exact CMake target for a diagnostic subset')
    command.add_argument('--full', action='store_true', help='Retain full CI requirement; execution needs explicit targets')
    command.add_argument('--jobs', type=int, default=2)
    command.add_argument('--build-timeout', type=float, default=900)
    command.add_argument('--discovery-timeout', type=float, default=120)
    command.add_argument('--test-timeout', type=float, default=180)
    command.add_argument('--git-timeout', type=float, default=60,
                         help='Budget in seconds per Git command; raise it for a checkout on a network or 9p mount')
    command.add_argument('--dry-run', action='store_true')
    command.add_argument('--json', action='store_true')
    command = commands.add_parser('doctor', help='Read-only toolchain, presets, runtime and synchronization diagnosis')
    command.add_argument('--build', type=Path, required=True)
    command.add_argument('--config')
    command.add_argument('--inherit-env', action='store_true', help='Inspect the caller environment without vcvars setup')
    command.add_argument('--json', action='store_true')
    command = commands.add_parser('evidence', help='Verify and read a sealed local result without rerunning commands')
    command.add_argument('--run', type=Path, required=True)
    command.add_argument('--json', action='store_true')
    return result


def make_plan(args):
    root = args.root.resolve(strict=True)
    build = (args.build if args.build.is_absolute() else root / args.build).resolve()
    if args.path and (args.base or args.head):
        raise SelectionError('Explicit paths cannot stand in for a CI revision comparison')
    if args.path:
        changes = [{'path': relative_name(path), 'status': 'M' if (root / path).is_file() else 'D'}
                   for path in sorted(set(args.path))]
        revision = {'mode': 'scenario', 'base': None, 'head': None}
    else:
        changes, revision = changes_from_git(root, args.base, args.head, timeout=getattr(args, 'git_timeout', None))
    model, model_error = None, None
    try:
        model = load_model(root, build, args.config)
    except SelectionError as error:
        model_error = str(error)
    selection = select_targets(root, changes, model, model_error, args.full)
    selection.update({'version': 1, 'root': str(root), 'build': str(build), 'revision': revision,
                      'content': content_identity(root, changes, revision),
                      'configuration': model['configuration'] if model else args.config,
                      'model_sha256': model['sha256'] if model else None,
                      'model_diagnostic': model_error, 'tests': [], 'test_discovery': 'not required',
                      'qualification': 'plan only; no checks executed'})
    if model and selection['scope'] != 'documentation':
        # PRE_TEST can execute list-discovery programs and refresh files. Keep plan/dry-run read-only;
        # check performs contained post-build discovery, then validates fixture ownership before execution.
        selection['test_discovery'] = 'pending check: build, then contained CTest discovery and fixture expansion'
        ctest = model['cmake'].get('paths', {}).get('ctest') or 'ctest'
        selection['discovery_command'] = [ctest, '--test-dir', str(build), '--show-only=json-v1']
        if selection['configuration']:
            selection['discovery_command'] += ['-C', selection['configuration']]
    elif selection['scope'] != 'documentation':
        selection['test_discovery'] = 'unavailable: configure model first'
    selection['selected_test_count'] = len(selection['tests'])
    selection['disabled_test_count'] = sum(test['disabled'] for test in selection['tests'])
    if model:
        selection['build_targets'] = buildable_targets(model['targets'], selection['targets'])
    return selection


def main(arguments=None):
    args = parser().parse_args(arguments)
    try:
        if args.command == 'check':
            from cerid_dev.check import check
            report = check(args, make_plan)
            if args.json:
                print(json.dumps(report, indent=2))
            else:
                print(f"Check: {report['status']}; {report['qualification']}")
                if report.get('error'):
                    print(report['error'])
                if report.get('evidence_directory'):
                    print('Evidence: ' + report['evidence_directory'])
                if report.get('plan'):
                    print(json.dumps(report['plan'], indent=2))
            return report['exit_code']
        if args.command == 'evidence':
            from cerid_dev.evidence import inspect
            report = inspect(args.run)
            if args.json:
                print(json.dumps(report, indent=2))
            else:
                print(f"Integrity: {report['integrity']}; {report['artifact_count']} artifacts")
                print(json.dumps(report['record'], indent=2))
                print(report['qualification'])
            return 0
        if args.command == 'doctor':
            from cerid_dev.environment import doctor
            root = args.root.resolve(strict=True)
            build = (args.build if args.build.is_absolute() else root / args.build).resolve()
            report = doctor(root, build, args.config, not args.inherit_env)
            if args.json:
                print(json.dumps(report, indent=2))
            else:
                print(f"Host: {report['host']['system']} {report['host']['machine']}; "
                      f"{report['generator']} / {report['configuration']}")
                for name, value in report['tools'].items():
                    print(f'{name}: {value or "unavailable"}')
                print('Eligible presets: ' + ', '.join(report['eligible_configure_presets']))
                print(f"Available RAM: {report['ram_bytes']['available']} bytes; "
                      f"free build disk: {report['disk_bytes']['free']} bytes")
                print('Environment: ' + report['environment_source'])
                for issue in report['issues']:
                    print('Needs attention: ' + issue)
                print(report['qualification'])
            return 0 if not report['issues'] else 2
        plan = make_plan(args)
        if args.json:
            print(json.dumps(plan, indent=2))
        else:
            print(f"Scope: {plan['scope']} ({plan['revision']['mode']}); {plan['qualification']}")
            print(f"Build: {plan['build']}  Configuration: {plan['configuration']}")
            for reason in plan['reasons']:
                print('Broaden: ' + reason)
            print('Build targets: ' + (', '.join(plan['build_targets']) or '(none available/required; inspect scope)'))
            other = sorted(set(plan['targets']) - set(plan['build_targets']))
            if other:
                print('Other graph requirements (not automatic commands): ' + ', '.join(other))
            print(f"Tests: {plan['selected_test_count']} selected, {plan['disabled_test_count']} disabled; "
                  + plan['test_discovery'])
            print('Guards: ' + ', '.join(plan['guards']))
            print('Tidy: ' + (', '.join(plan['tidy_files']) or '(none)'))
            for risk in plan['risk_checks']:
                print('Risk gate: ' + risk)
            print('Content identity: ' + plan['content']['sha256'])
            if plan['scope'] == 'full':
                print('Full scope belongs to CI. This command does not launch a local repository sweep.')
        return 0
    except (SelectionError, EvidenceError, Conflict, OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        if args.json:
            print(json.dumps({'version': 1, 'kind': args.command, 'status': 'instrument_failure',
                              'error': str(error), 'qualification': 'none'}))
        else:
            print('ERROR: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
