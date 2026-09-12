#!/usr/bin/env python3
"""Explain affected Cerid targets/tests without building or changing source/project state."""
from pathlib import Path
import argparse
import json
import shutil
import subprocess
import sys

from cerid_dev.selection import (SelectionError, buildable_targets, changes_from_git, content_identity, load_model, relative_name,
                                 run_json, select_targets, select_tests)


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
        changes, revision = changes_from_git(root, args.base, args.head)
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
        ctest = model['cmake'].get('paths', {}).get('ctest') or shutil.which('ctest')
        if not ctest:
            selection['test_discovery'] = 'unavailable: CTest executable not found'
        else:
            command = [ctest, '--test-dir', str(build), '--show-only=json-v1']
            if selection['configuration']:
                command += ['-C', selection['configuration']]
            selection['discovery_command'] = command
            try:
                inventory = run_json(command, root)
                selection['tests'] = select_tests(inventory, model, selection['targets'], selection['scope'] == 'full')
                selection['targets'] = sorted(set(selection['targets']) | {
                    target for test in selection['tests'] for target in test['targets']})
                selection['test_discovery'] = 'available'
                selection['inventory_count'] = len(inventory['tests'])
            except (SelectionError, OSError) as error:
                selection['test_discovery'] = 'unavailable: ' + str(error)
        if selection['test_discovery'].startswith('unavailable:'):
            selection['reasons'].append(selection['test_discovery'])
            selection['scope'] = 'full'
            selection['targets'] = sorted(model['targets'])
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
    except (SelectionError, OSError, UnicodeError, subprocess.TimeoutExpired) as error:
        print('ERROR: ' + str(error), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    raise SystemExit(main())
