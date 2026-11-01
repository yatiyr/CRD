#!/usr/bin/env python3
"""MCP-ADAPT.0 evidence: the pinned MCP Inspector drives the real `ceridc mcp` in both MCP revisions.

Contract: docs/design/agent-mcp.md#conformance.

The Inspector (`@modelcontextprotocol/inspector`, the version and every transitive package locked with its SHA-512
integrity in tests/tools/ceridc/mcp/inspector/) is installed with `npm ci --ignore-scripts` on the pinned Node
(scripts/install-node.py), outside the checkout. `run` drives its command line client against ceridc in each protocol
era it offers: `legacy` (the 2025-11-25 initialize handshake), `modern` (2026-07-28, stateless, every request with its
`_meta`) and `auto` (it must find 2026-07-28 through discovery). Each era connects, lists the tools with the
Inspector's schema portability check (`--strict`), and calls tools that run and tools it must refuse.

The Inspector spawns this script's `relay` as the server: it starts `ceridc mcp` and copies each line through
unchanged while recording it, so the evidence is what crossed the pipe, not the Inspector's account of it. Each
transcript is then checked like the conformance run (tests/tools/ceridc/mcp_conformance.py): every line is one JSON
message ending in LF alone, the revision of each request is the one its era must use, every request and notification
validates against its method's definition in that revision's pinned schema, every reply against its method's result
(or the error response), each call's structured result against the tool's output schema, and what the Inspector
printed must equal the reply the server sent. The Inspector runs with its home, catalog and secrets in the scratch
directory (an in-memory secret store, no browser), so it writes nothing outside it.

  python3 tests/tools/ceridc/mcp_inspector.py install [--destination DIR]
  python3 tests/tools/ceridc/mcp_inspector.py run --ceridc <path to ceridc> [--inspector DIR] [--record FILE]
"""
from pathlib import Path
import argparse
import base64
import datetime
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
PACKAGE = HERE / 'mcp' / 'inspector'
INSPECTOR = '@modelcontextprotocol/inspector'
STAMP = 'cerid-inspector.json'
RECORD_SCHEMA = 'cerid-mcp-inspector-run/1'


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


conformance = load('mcp_conformance', HERE / 'mcp_conformance.py')
Failure = conformance.Failure
require = conformance.require
MODERN, LEGACY = conformance.MODERN, conformance.LEGACY
VERSION_KEY = 'io.modelcontextprotocol/protocolVersion'

# Each method the Inspector may send, with its request and result definitions; anything else fails the run, so a
# new Inspector behaviour is read before it counts as evidence.
REQUESTS = {'initialize': ('InitializeRequest', 'InitializeResult'), 'ping': ('PingRequest', 'EmptyResult'),
            'server/discover': ('DiscoverRequest', 'DiscoverResult'),
            'tools/list': ('ListToolsRequest', 'ListToolsResult'), 'tools/call': ('CallToolRequest', 'CallToolResult')}
NOTIFICATIONS = {'notifications/initialized': 'InitializedNotification',
                 'notifications/cancelled': 'CancelledNotification'}
ERAS = {'legacy': LEGACY, 'modern': MODERN, 'auto': MODERN}
PAYLOAD = {'tools/list': ('tools',), 'tools/call': ('content', 'structuredContent', 'isError')}
PAYLOAD_DEFAULTS = {'isError': False}
TOOL_ERROR_EXIT = 5


def pinned_version():
    return json.loads((PACKAGE / 'package.json').read_text(encoding='utf-8'))['devDependencies'][INSPECTOR]


def lock_digest():
    return hashlib.sha256((PACKAGE / 'package-lock.json').read_bytes()).hexdigest()


def default_destination():
    return Path.home() / '.cerid' / 'tools' / 'mcp-inspector' / pinned_version()


def launcher(destination):
    return Path(destination) / 'node_modules' / INSPECTOR / 'clients' / 'launcher' / 'build' / 'index.js'


def installed_version(destination):
    path = Path(destination) / 'node_modules' / INSPECTOR / 'package.json'
    try:
        return json.loads(path.read_text(encoding='utf-8'))['version']
    except (OSError, ValueError, KeyError):
        return None


def install(destination=None, node=None):
    """Install the locked Inspector under `destination` (reused when its stamp names the same lockfile); returns the
    destination and the node binary."""
    install_node = load('install_node', ROOT / 'scripts' / 'install-node.py')
    node = Path(node) if node else install_node.install()
    destination = Path(destination or default_destination()).resolve()
    expected = {'version': pinned_version(), 'lock_sha256': lock_digest()}
    try:
        recorded = json.loads((destination / STAMP).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        recorded = None
    if recorded == expected and installed_version(destination) == expected['version']:
        print('MCP Inspector %s already installed at %s; its lockfile matches.' % (expected['version'], destination))
        return destination, node
    if destination.exists() and recorded is None:
        raise Failure('%s exists and holds no Inspector this script installed' % destination)
    staging = destination.with_name(destination.name + '.partial')
    shutil.rmtree(staging, ignore_errors=True)
    staging.mkdir(parents=True)
    try:
        for name in ('package.json', 'package-lock.json'):
            shutil.copyfile(PACKAGE / name, staging / name)
        environ = dict(os.environ, PATH=str(node.parent) + os.pathsep + os.environ.get('PATH', ''),
                       npm_config_cache=str(staging.parent / 'npm-cache'), npm_config_update_notifier='false')
        subprocess.run(install_node.npm_command(node) + ['ci', '--ignore-scripts', '--no-audit', '--no-fund'],
                       cwd=staging, env=environ, check=True)
        require(installed_version(staging) == expected['version'], 'npm ci installed Inspector %s, the lock pins %s' %
                (installed_version(staging), expected['version']))
        (staging / STAMP).write_text(json.dumps(expected, indent=2) + '\n', encoding='utf-8')
        shutil.rmtree(destination, ignore_errors=True)
        staging.rename(destination)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    print('Installed MCP Inspector %s from its lockfile (every package SHA-512 checked by npm ci).' %
          expected['version'])
    return destination, node


def relay(log, command):
    """Run `command` as the server, copying every line between it and this process's standard streams unchanged and
    recording each one, with its direction, in `log` (JSON lines; the bytes in base64)."""
    child = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    lock = threading.Lock()
    with open(log, 'w', encoding='utf-8') as out:

        def record(**entry):
            with lock:
                out.write(json.dumps(entry) + '\n')
                out.flush()

        def client():
            for line in iter(sys.stdin.buffer.readline, b''):
                record(source='client', data=base64.b64encode(line).decode('ascii'))
                try:
                    child.stdin.write(line)
                    child.stdin.flush()
                except BrokenPipeError:
                    break
            try:
                child.stdin.close()
            except BrokenPipeError:
                pass

        threading.Thread(target=client, daemon=True).start()
        for line in iter(child.stdout.readline, b''):
            record(source='server', data=base64.b64encode(line).decode('ascii'))
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
        code = child.wait()
        record(exit=code)
    return code


class Transcript:
    """The checked messages of one recorded connection."""

    def __init__(self, path, schemas, tools, era):
        self.messages = 0
        self.revisions = set()
        self.replies = {}
        self.methods = []
        self.exit = None
        pending = {}
        for entry in (json.loads(line) for line in Path(path).read_text(encoding='utf-8').splitlines()):
            if 'exit' in entry:
                self.exit = entry['exit']
                continue
            data = base64.b64decode(entry['data'])
            require(data.endswith(b'\n') and b'\r' not in data, 'a %s line is one message ending in LF alone: %r' %
                    (entry['source'], data[:120]))
            message = json.loads(data)
            self.messages += 1
            if entry['source'] == 'client':
                self.client(message, schemas, era, pending)
            else:
                self.server(message, schemas, tools, pending)
        require(not pending, 'requests without a reply: %r' % sorted(pending))

    def client(self, message, schemas, era, pending):
        method = message.get('method')
        meta = (message.get('params') or {}).get('_meta') or {}
        revision = meta.get(VERSION_KEY, LEGACY)
        require(revision == ERAS[era], '%s era: %s was sent in revision %s' % (era, method, revision))
        self.revisions.add(revision)
        schema = schemas[revision]
        if 'id' in message:
            require(method in REQUESTS, 'the Inspector sent %s, a method this run does not know' % method)
            definition = REQUESTS[method][0]
            pending[message['id']] = (method, revision, message.get('params'))
            self.methods.append(method)
        else:
            require(method in NOTIFICATIONS, 'the Inspector sent the notification %s, which this run does not know' %
                    method)
            definition = NOTIFICATIONS[method]
        require(definition in schema.document['$defs'], '%s is not a %s message' % (method, revision))
        problems = schema.ref_errors(message, '#/$defs/%s' % definition)
        require(not problems, 'the Inspector sent a %s that does not validate: %s' % (definition, problems[:3]))

    def server(self, message, schemas, tools, pending):
        require(message.get('id') in pending, 'a reply to no request: %r' % message)
        method, revision, params = pending.pop(message['id'])
        schema = schemas[revision]
        if 'error' in message:
            problems = schema.ref_errors(message, '#/$defs/JSONRPCErrorResponse')
            require(not problems, 'the %s error does not validate: %s' % (method, problems[:3]))
            self.replies[method] = message
            return
        result = message['result']
        problems = schema.ref_errors(message, '#/$defs/JSONRPCResultResponse')
        problems += schema.ref_errors(result, '#/$defs/%s' % REQUESTS[method][1])
        require(not problems, 'the %s result does not validate in %s: %s' % (method, revision, problems[:5]))
        if method == 'tools/list':
            for tool in result['tools']:
                tools[(revision, tool['name'])] = conformance.subset.Validator(tool['outputSchema'])
        if method == 'tools/call':
            output = tools.get((revision, params['name']))
            require(output is not None, '%s was called before its output schema was listed' % params['name'])
            problems = output.errors(result['structuredContent'])
            require(not problems, '%s structuredContent does not fit its outputSchema: %s' % (params['name'],
                                                                                              problems[:3]))
            content = result['content']
            require(len(content) == 1 and content[0]['type'] == 'text', 'one text content block')
            require(json.loads(content[0]['text']) == result['structuredContent'],
                    'the text content is the structured result')
            require(result.get('isError', False) == (result['structuredContent']['ok'] is not True),
                    'isError follows the report\'s ok')
        self.replies[method] = message


class Driver:
    def __init__(self, inspector, node, ceridc, scratch, schemas):
        self.launcher = launcher(inspector)
        self.node = node
        self.ceridc = str(Path(ceridc).resolve())
        self.scratch = scratch
        self.schemas = schemas
        self.tools = {}
        self.runs = []
        home = scratch / 'home'
        home.mkdir()
        self.environ = {key: value for key, value in os.environ.items() if not key.startswith('MCP_')}
        self.environ.update(HOME=str(home), USERPROFILE=str(home), MCP_INSPECTOR_SECRET_STORE='memory',
                            MCP_AUTO_OPEN_ENABLED='false', NO_COLOR='1')

    def inspect(self, era, method, tool=None, arguments=(), extra=(), expect_exit=0):
        """One Inspector run: its printed result, checked against the recorded transcript."""
        log = self.scratch / ('transcript-%d.jsonl' % len(self.runs))
        command = [str(self.node), str(self.launcher), '--cli', '--protocol-era', era, '--format', 'json', '--method',
                   method]
        if tool:
            command += ['--tool-name', tool]
        for key, value in arguments:
            command += ['--tool-arg', '%s=%s' % (key, value)]
        command += list(extra) + ['--', sys.executable, str(Path(__file__).resolve()), 'relay', '--log', str(log),
                                  '--', self.ceridc, 'mcp', '--diag-root', '.']
        done = subprocess.run(command, cwd=self.scratch, env=self.environ, capture_output=True, timeout=120)
        label = '%s %s%s' % (era, method, ' ' + tool if tool else '')
        require(done.returncode == expect_exit, '%s: the Inspector exited %d, expected %d: %s' %
                (label, done.returncode, expect_exit, done.stderr.decode('utf-8', 'replace')[-800:]))
        transcript = Transcript(log, self.schemas, self.tools, era)
        require(transcript.exit in (0, None), '%s: ceridc mcp exited %s' % (label, transcript.exit))
        printed = json.loads(done.stdout)['result']
        # The Inspector prints the method's payload; the protocol members (2026-07-28's resultType, _meta and caching
        # hints) are the client's to consume.
        sent = transcript.replies.get(method, {}).get('result', {})
        for key in PAYLOAD.get(method, ()):
            require(printed.get(key, PAYLOAD_DEFAULTS.get(key)) == sent.get(key, PAYLOAD_DEFAULTS.get(key)),
                    '%s: the Inspector printed a %s other than the one ceridc sent' % (label, key))
        self.runs.append({'era': era, 'method': method, 'tool': tool, 'arguments': dict(arguments),
                          'exit': done.returncode, 'requests': transcript.methods, 'messages': transcript.messages,
                          'revisions': sorted(transcript.revisions),
                          'stderr': done.stderr.decode('utf-8', 'replace').strip().splitlines()})
        return printed, transcript

    def era(self, era):
        revision = ERAS[era]
        connected, transcript = self.inspect(era, 'initialize')
        require(connected['protocolVersion'] == revision, '%s: the Inspector connected in %s' %
                (era, connected['protocolVersion']))
        require(connected['serverInfo']['name'] == 'ceridc', 'the server names itself')
        require(transcript.methods[0] == ('server/discover' if revision == MODERN else 'initialize'),
                '%s: the connection opened with %s' % (era, transcript.methods[0]))
        listed, _ = self.inspect(era, 'tools/list', extra=['--strict'])
        names = [tool['name'] for tool in listed['tools']]
        require(names == conformance.TOOLS, '%s: tools/list answered %r' % (era, names))
        tag = era
        conformance.write_sources(self.scratch / ('src-%s' % tag))
        pack = '%s.pack.crdr' % tag
        calls = [('import', [('path', 'src-%s/tri.stl' % tag)], True),
                 ('cook', [('root', 'src-%s' % tag), ('out', pack)], True),
                 ('query', [('pack', pack)], True),
                 ('query', [('pack', pack), ('extra', '1')], False),
                 ('instantiate', [('pack', pack), ('asset', 'tri.stl'), ('x', '1'), ('out', '%s.scen' % tag),
                                  ('dry_run', 'true')], True),
                 ('diag', [('command', 'gpu.resources')], True),
                 ('diag', [('command', 'diag.commands'), ('grant', 'record')], False)]
        for tool, arguments, ok in calls:
            result, _ = self.inspect(era, 'tools/call', tool, arguments,
                                     expect_exit=0 if ok else TOOL_ERROR_EXIT)
            report = result['structuredContent']
            require(report['ok'] is ok and result.get('isError', False) is not ok,
                    '%s %s %r answered %r' % (era, tool, arguments, report))
            if tool == 'query' and ok:
                require([entry['uuid'] for entry in report['entries']] == [conformance.MESH_UUID],
                        '%s: the cook kept the .meta\'s id: %r' % (era, report['entries']))


def run(ceridc, inspector=None, record=None):
    inspector, node = install(inspector)
    schemas = {MODERN: conformance.load_schema(MODERN), LEGACY: conformance.load_schema(LEGACY)}
    with tempfile.TemporaryDirectory(prefix='mcp-inspector-') as scratch:
        driver = Driver(inspector, node, ceridc, Path(scratch), schemas)
        for era in ERAS:
            driver.era(era)
        leftovers = sorted(p.name for p in (Path(scratch) / 'home').iterdir())
        require(not leftovers, 'the Inspector wrote into its home: %r' % leftovers)
    node_version = subprocess.run([str(node), '--version'], capture_output=True, text=True, check=True).stdout.strip()
    document = {'schema': RECORD_SCHEMA, 'day': datetime.date.today().isoformat(), 'inspector': pinned_version(),
                'lock_sha256': lock_digest(), 'node': node_version, 'eras': {era: ERAS[era] for era in ERAS},
                'runs': driver.runs}
    if record:
        Path(record).write_text(json.dumps(document, indent=1) + '\n', encoding='utf-8')
    messages = sum(entry['messages'] for entry in driver.runs)
    print('mcp-inspector: Inspector %s ran %d times over %d checked messages in %s' %
          (pinned_version(), len(driver.runs), messages, ', '.join('%s (%s)' % item for item in ERAS.items())))
    return document


def main(argv=None):
    parser = argparse.ArgumentParser(prog='mcp_inspector.py', description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='command', required=True)
    setup = sub.add_parser('install', help='install the locked Inspector on the pinned Node')
    setup.add_argument('--destination', type=Path)
    drive = sub.add_parser('run', help='drive ceridc mcp with the Inspector in every era')
    drive.add_argument('--ceridc', required=True, type=Path)
    drive.add_argument('--inspector', type=Path, help='the Inspector install (default: ~/.cerid/tools/mcp-inspector)')
    drive.add_argument('--record', type=Path, help='write the run record (JSON) here')
    hop = sub.add_parser('relay', help='(the server command the Inspector spawns) record ceridc mcp\'s traffic')
    hop.add_argument('--log', required=True, type=Path)
    hop.add_argument('server', nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command == 'relay':
        server = args.server[1:] if args.server[:1] == ['--'] else args.server
        return relay(args.log, server)
    try:
        if args.command == 'install':
            install(args.destination)
        else:
            run(args.ceridc, args.inspector, args.record)
    except (Failure, conformance.subset.SchemaError, OSError, ValueError, KeyError,
            subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
        print('mcp-inspector: FAILED: %s' % error, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
