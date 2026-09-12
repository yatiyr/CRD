#!/usr/bin/env python3
"""Human/agent CLI for the shared project-structure transaction engine."""
from pathlib import Path
import argparse
import json
import sys

from project_sync.model import cmake_model
from project_sync.operations import Plan
from project_sync.service import (cmake_executable, configure, finalize, finish_process, hidden_process,
                                  ingest, preconfigure, state_dir, state_file, watch)
from project_sync.service import verify
from project_sync.service import generation_idle, registrations, projection_hashes
from project_sync.storage import Conflict, Transaction, Workspace, incomplete, json_bytes, atomic_write, read_json, recover
from project_sync import ide


def parser():
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    commands = result.add_subparsers(dest='command', required=True)
    for name in ('configure', 'open', 'watch', 'status', 'sync', 'preconfigure', 'finish-generation', 'stop', 'verify'):
        command = commands.add_parser(name)
        command.add_argument('--build', type=Path, default=Path('build/win-vs-debug'))
        if name in {'configure', 'open'}:
            command.add_argument('--preset', default=None,
                                 type=lambda name: 'win-vs' if name == 'win-vs-debug' else name,
                                 help='CMake configure preset; legacy win-vs-debug redirects to win-vs')
        if name == 'sync':
            command.add_argument('--apply', action='store_true', help='Apply the reported plan; default is read-only preview')
        if name == 'preconfigure':
            command.add_argument('--native', action='store_true')
        if name == 'finish-generation':
            command.add_argument('--pid', type=int, required=True)
            command.add_argument('--token', required=True)
    command = commands.add_parser('edit')
    command.add_argument('--build', type=Path, default=Path('build/win-vs-debug'))
    command.add_argument('--operations', type=Path, required=True, help='JSON array of add/remove/delete/mkdir/move operations')
    command.add_argument('--apply', action='store_true')
    command.add_argument('--regenerate', action='store_true')
    command = commands.add_parser('recover')
    command.add_argument('transaction')
    command.add_argument('--build', type=Path, default=Path('build/win-vs-debug'))
    return result


def main(arguments=None):
    args = parser().parse_args(arguments)
    root = args.root.resolve(strict=True)
    build = getattr(args, 'build', None)
    if build is not None:
        build = (build if build.is_absolute() else root / build).resolve()
        if not build.is_relative_to(root / 'build') and not (args.command == 'preconfigure' and not args.native):
            raise Conflict('Build directory must be inside this repository/build')
    if args.command == 'preconfigure':
        preconfigure(root, build, args.native)
        return 0
    if args.command == 'finish-generation':
        finish_process(root, build, args.pid, args.token)
        return 0
    if args.command == 'verify':
        verify(root, build)
        return 0
    if args.command in {'configure', 'open'}:
        initial = ['--preset', args.preset] if args.preset else []
        configure(root, build, initial_args=initial)
        if args.command == 'open':
            import os
            import subprocess
            status = read_json(state_dir(build) / 'watcher.json', {})
            # Start/stop control is local to this solution, never an OS startup service.
            alive = status.get('pid') and ide.process_alive(status['pid'])
            if alive and (state_dir(build) / 'stop').exists():
                import time
                deadline = time.monotonic() + 10
                while ide.process_alive(status['pid']) and time.monotonic() < deadline:
                    time.sleep(0.1)
                alive = ide.process_alive(status['pid'])
                if alive:
                    raise Conflict('Previous watcher is still stopping; retry open once its status is stopped')
            if status.get('status') not in {'watching', 'conflict'} or not alive:
                hidden_process([sys.executable, str(root / 'scripts/project-sync.py'), '--root', str(root),
                                'watch', '--build', str(build)], state_dir(build) / 'watcher.log')
            result = subprocess.run([cmake_executable(), '--open', str(build)], env=dict(os.environ))
            if result.returncode:
                raise Conflict('Visual Studio could not open the solution')
        return 0
    ws = Workspace(root, [build] if build else [])
    if args.command == 'watch':
        watch(root, build)
        return 0
    if args.command == 'stop':
        atomic_write(state_dir(build) / 'stop', b'stop\n')
        return 0
    if args.command == 'status':
        watcher = read_json(state_dir(build) / 'watcher.json')
        if watcher:
            watcher['alive'] = bool(watcher.get('pid')) and ide.process_alive(watcher['pid'])
            if not watcher['alive'] and watcher.get('status') != 'stopped':
                watcher['status'] = 'exited'
        generation = read_json(state_dir(build) / 'generation.json')
        if generation:
            # The full recovery snapshot stays on disk; status must remain usable during regeneration.
            generation = {key: generation[key] for key in ('pid', 'token', 'failure') if key in generation}
        print(json.dumps({'baseline': state_file(build).is_file(), 'incomplete': incomplete(ws),
                          'watcher': watcher,
                          'generation': generation}, indent=2))
        return 0
    with ws.lock():
        if args.command == 'recover':
            generation_idle(ws, [build])
            journal = read_json(ws.path('build/project-sync/journal/' + args.transaction + '/transaction.json', internal=True))
            if not journal:
                raise Conflict('Unknown recovery transaction')
            recovery = Transaction(ws, 'Recovery buffer check')
            recovery.writes = {entry['path']: None for entry in journal['records']}
            for active in registrations(ws):
                ide.ready(Workspace(root, [active]), active, recovery)
                # Recovery does not recreate a user gesture. Pause automatic ingestion until regenerated explicitly.
                atomic_write(state_dir(active) / 'stop', b'recovery\n')
            recover(ws, args.transaction)
            for active in registrations(ws):
                consumed = state_dir(active) / 'consumed.json'
                if consumed.exists():
                    consumed.unlink()
                active_ws = Workspace(root, [active])
                atomic_write(state_dir(active) / 'recovery.json', json_bytes(
                    {'transaction': args.transaction, 'projection': projection_hashes(active_ws, active)}))
            print(json.dumps({'recovered': args.transaction,
                              'next': 'Watcher paused. Review recovered source, then configure/open to replace the canceled IDE gesture.'}))
            return 0
        if args.command == 'sync':
            if args.apply:
                generation_idle(ws, [build])
            result = ingest(ws, build, apply=args.apply)
            print(json.dumps(result or {'changes': []}, indent=2))
            return 0
        if args.command == 'edit':
            generation_idle(ws, [build])
            operations = read_json(args.operations)
            if not isinstance(operations, list) or not operations:
                raise Conflict('Operations must be a nonempty JSON array')
            pending = ingest(ws, build, apply=False, source_changes=False)
            if pending:
                raise Conflict('Saved IDE edits are pending; synchronize those before a new agent/CLI operation')
            plan = Plan(ws, cmake_model(ws, build), 'Explicit human/agent project structure operations')
            for op in operations:
                if not isinstance(op, dict):
                    raise Conflict('Each operation must be an object')
                kind = op.get('op')
                fields = {'move': {'source', 'destination', 'already_moved'},
                          'add-module': {'target', 'directory', 'kind', 'files', 'dependencies'},
                          'remove-module': {'directory'}, 'rename-target': {'source', 'destination'},
                          'remove': {'target', 'path'}, 'delete': {'target', 'path'},
                          'remove-directory': {'target', 'path'}, 'delete-directory': {'target', 'path'},
                          'add': {'target', 'path', 'group', 'content'}, 'mkdir': {'target', 'path'}}
                if kind not in fields or set(op) - (fields[kind] | {'op'}):
                    raise Conflict(f'Unsupported operation or fields: {kind!r}')
                if 'already_moved' in op and not isinstance(op['already_moved'], bool):
                    raise Conflict('already_moved must be a JSON boolean')
                if kind == 'move':
                    plan.move(op['source'], op['destination'], already_moved=op.get('already_moved', False))
                elif kind == 'add-module':
                    plan.module(op['target'], op['directory'], op['kind'],
                                {name: data.encode('utf-8') for name, data in op['files'].items()}, op.get('dependencies', []))
                elif kind == 'remove-module':
                    plan.remove_module(op['directory'])
                elif kind == 'rename-target':
                    plan.rename_target(op['source'], op['destination'])
                elif kind in {'remove', 'delete'}:
                    plan.exclude(op['target'], op['path'], delete=kind == 'delete')
                elif kind in {'remove-directory', 'delete-directory'}:
                    plan.remove_directory(op['target'], op['path'], delete=kind == 'delete-directory')
                elif kind == 'add':
                    content = op.get('content')
                    plan.add(op['target'], op['path'], op.get('group'),
                             content=content.encode('utf-8') if content is not None else None)
                elif kind == 'mkdir':
                    plan.directory(op['target'], op['path'])
                else:
                    raise Conflict(f'Unsupported operation: {kind!r}')
            tx = plan.finish()
            result = tx.describe()
            if args.apply:
                state = ide.ready(ws, build, tx)
                result['transaction'] = tx.apply()
                ide.reopen(ws, build, tx, state)
            print(json.dumps(result, indent=2))
    if args.command == 'edit' and args.apply and args.regenerate:
        configure(root, build)
    return 0


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    try:
        raise SystemExit(main())
    except (Conflict, OSError, KeyError, TypeError, ValueError, AttributeError) as error:
        print(json.dumps({'status': 'conflict', 'message': str(error)}, ensure_ascii=False), file=sys.stderr)
        raise SystemExit(2)
