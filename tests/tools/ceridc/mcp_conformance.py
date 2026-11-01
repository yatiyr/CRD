#!/usr/bin/env python3
"""MCP-ADAPT.0 conformance: the real `ceridc mcp` process against the published MCP schemas (docs/design/agent-mcp.md).

One process is driven in revision 2026-07-28 (every request with its per-request _meta), another opens with the
initialize handshake and is driven in revision 2025-11-25, then sends a 2026-07-28 request on the same connection.
Every request the test sends validates against its revision's request definition, every reply against the result or
error definition of its method, and the published schemas are the pinned files in tests/tools/ceridc/mcp/ (their
fetch records name the source commit and SHA-256, checked here before use). Each tool listed must declare an input and
an output schema in the keywords the oracle implements (scripts/json-schema-subset.py refuses any other). Each call's
structured result must validate against its tool's output schema, equal the JSON of its text content, and be a tool
error exactly when its report says ok false. Each call's arguments validate against the tool's input schema exactly
when the server ran the verb rather than refusing an argument, so the published schema and the server's check agree.
The verbs run on files the test writes in its own scratch directory; nothing is left behind.

  python tests/tools/ceridc/mcp_conformance.py --ceridc <path to ceridc>
"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
_spec = importlib.util.spec_from_file_location('json_schema_subset', ROOT / 'scripts/json-schema-subset.py')
subset = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(subset)

MODERN = '2026-07-28'
LEGACY = '2025-11-25'
META = {'io.modelcontextprotocol/protocolVersion': MODERN, 'io.modelcontextprotocol/clientCapabilities': {},
        'io.modelcontextprotocol/clientInfo': {'name': 'mcp-conformance', 'version': '1'}}
TOOLS = ['cook', 'diag', 'export', 'import', 'instantiate', 'query', 'render', 'sequence']


class Failure(Exception):
    pass


def require(condition, message):
    if not condition:
        raise Failure(message)


def load_schema(revision):
    """The pinned schema of `revision`, its bytes checked against the fetch record before use."""
    data = (HERE / 'mcp' / ('schema-%s.json' % revision)).read_bytes()
    record = json.loads((HERE / 'mcp' / ('schema-%s.fetch.json' % revision)).read_text(encoding='utf-8'))
    require(record['revision'] == revision and record['bytes'] == len(data), 'the %s fetch record' % revision)
    require(hashlib.sha256(data).hexdigest() == record['sha256'], 'schema-%s.json differs from its fetch record' %
            revision)
    return subset.Validator(json.loads(data))


def binary_stl(path):
    """A one-triangle binary STL."""
    triangle = struct.pack('<12f', 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0) + b'\0\0'
    path.write_bytes(b'\0' * 80 + struct.pack('<I', 1) + triangle)


class Session:
    """One ceridc mcp process: a request line in, its reply line out."""

    def __init__(self, ceridc, cwd, schema, era):
        self.schema = schema
        self.era = era
        self.next_id = 1
        self.process = subprocess.Popen([ceridc, 'mcp', '--diag-root', '.'], cwd=cwd, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.tools = {}
        self.checked = 0

    def close(self):
        self.process.stdin.close()
        require(self.process.wait(timeout=60) == 0, 'ceridc mcp exited %s' % self.process.returncode)
        self.process.stdout.close()
        self.process.stderr.close()

    def send(self, method, params, request_def, *, modern):
        message = {'jsonrpc': '2.0', 'id': self.next_id, 'method': method}
        self.next_id += 1
        if params is not None or modern:
            message['params'] = dict(params or {})
            if modern:
                message['params']['_meta'] = META
        problems = self.schema.ref_errors(message, '#/$defs/%s' % request_def)
        require(not problems, 'the test sent a malformed %s: %s' % (request_def, problems[:3]))
        line = json.dumps(message, separators=(',', ':')).encode('utf-8') + b'\n'
        self.process.stdin.write(line)
        self.process.stdin.flush()
        reply = self.process.stdout.readline()
        require(reply.endswith(b'\n') and not reply.endswith(b'\r\n'), 'a reply is one line ending in LF alone')
        answer = json.loads(reply)
        require(answer.get('id') == message['id'], 'the reply echoes the id: %r' % reply[:200])
        return answer

    def result(self, answer, result_def):
        require('result' in answer, 'an error where a result was expected: %r' % answer)
        problems = self.schema.ref_errors(answer, '#/$defs/JSONRPCResultResponse')
        problems += self.schema.ref_errors(answer['result'], '#/$defs/%s' % result_def)
        require(not problems, '%s does not validate: %s' % (result_def, problems[:5]))
        self.checked += 1
        return answer['result']

    def error(self, answer, code, error_def='JSONRPCErrorResponse'):
        require('error' in answer and answer['error']['code'] == code,
                'expected error %d, got %r' % (code, answer))
        problems = self.schema.ref_errors(answer, '#/$defs/%s' % error_def)
        require(not problems, '%s does not validate: %s' % (error_def, problems[:5]))
        self.checked += 1
        return answer['error']

    def list_tools(self, modern):
        listed = self.result(self.send('tools/list', None, 'ListToolsRequest', modern=modern), 'ListToolsResult')
        names = [tool['name'] for tool in listed['tools']]
        require(names == TOOLS, 'tools/list answers every tool in name order: %r' % names)
        for tool in listed['tools']:
            for key in ('inputSchema', 'outputSchema'):
                try:
                    subset.check_schema(tool[key])
                except subset.SchemaError as error:
                    raise Failure('%s %s: %s' % (tool['name'], key, error)) from error
            require(tool['inputSchema'].get('additionalProperties') is False, '%s refuses unknown arguments' %
                    tool['name'])
            self.tools[tool['name']] = (subset.Validator(tool['inputSchema']), subset.Validator(tool['outputSchema']))
        return listed

    def call(self, tool, arguments, *, modern, expect_ok=None):
        """One tools/call: its result validated, its structured content against the tool's output schema."""
        answer = self.send('tools/call', {'name': tool, 'arguments': arguments}, 'CallToolRequest', modern=modern)
        result = self.result(answer, 'CallToolResult')
        inputs, outputs = self.tools[tool]
        structured = result['structuredContent']
        problems = outputs.errors(structured)
        require(not problems, '%s structuredContent does not fit its outputSchema: %s' % (tool, problems[:5]))
        require(len(result['content']) == 1 and result['content'][0]['type'] == 'text', 'one text content block')
        require(json.loads(result['content'][0]['text']) == structured, 'the text content is the structured result')
        require(result['isError'] == (structured['ok'] is not True), 'isError follows the report\'s ok')
        # The server's own argument refusal: the three-member report naming an argument (a verb's or the diag
        # service's refusal is a report of its own).
        refused = set(structured) == {'ok', 'verb', 'reason'} and structured['reason'].startswith(
            ('argument ', 'unknown argument '))
        require(inputs.valid(arguments) == (not refused),
                '%s: the input schema %s %r but the server %s it' %
                (tool, 'accepts' if inputs.valid(arguments) else 'refuses', arguments,
                 'refused' if refused else 'ran'))
        if expect_ok is not None:
            require(structured['ok'] is expect_ok, '%s %r answered %r' % (tool, arguments, structured))
        return structured


def drive_tools(session, modern, scratch, tag):
    """Every tool, run and refused, on files under `scratch`."""
    source = scratch / ('src-%s' % tag)
    source.mkdir()
    binary_stl(source / 'tri.stl')
    (source / 'tri.stl.meta').write_text('[id]\nuuid = "00112233445566770011223344556677"\n', encoding='utf-8')
    pack, scene, timl, otio = ('%s.pack.crdr' % tag, '%s.scen' % tag, '%s.timl.crdr' % tag, '%s.otio' % tag)
    call = lambda tool, arguments, ok=None: session.call(tool, arguments, modern=modern, expect_ok=ok)
    call('import', {'path': 'src-%s/tri.stl' % tag}, True)
    call('import', {'path': 'src-%s/none.stl' % tag}, False)
    call('import', {'path': ''}, False)
    call('cook', {'root': 'src-%s' % tag, 'out': pack}, True)
    call('cook', {'root': 'src-%s' % tag}, False)
    call('query', {'pack': pack}, True)
    call('query', {'pack': pack, 'extra': 1}, False)
    call('instantiate', {'pack': pack, 'asset': 'tri.stl', 'x': 1, 'out': scene, 'dry_run': True}, True)
    call('instantiate', {'pack': pack, 'asset': 'tri.stl', 'x': 1, 'y': 2.5, 'z': -3, 'out': scene}, True)
    call('instantiate', {'pack': pack, 'asset': 'no-such', 'out': scene}, False)
    call('instantiate', {'pack': pack, 'asset': 'tri.stl', 'out': scene, 'dry_run': 'true'}, False)
    call('instantiate', {'pack': pack, 'asset': 'tri.stl', 'out': scene, 'x': 1e39}, False)
    call('sequence', {'name': 'cut', 'clip_a': 'a', 'frames_a': 24, 'clip_b': 'b', 'frames_b': 24, 'transition': 6,
                      'out_timl': timl, 'out_otio': otio}, True)
    call('sequence', {'name': 'cut', 'clip_a': 'a', 'frames_a': 1.5, 'clip_b': 'b', 'frames_b': 24,
                      'out_timl': timl, 'out_otio': otio}, False)
    call('render', {'otio': otio, 'dir': 'frames-%s' % tag, 'max_frames': 2}, True)
    call('render', {'otio': 'none.otio', 'dir': 'frames-%s' % tag}, False)
    call('export', {'timl': timl, 'out': 'export-%s.otio' % tag}, True)
    call('export', {'timl': timl}, False)
    call('diag', {'command': 'diag.commands', 'page_items': 3}, True)
    call('diag', {'command': 'diag.capabilities'}, True)
    call('diag', {'command': 'capture.start'}, False)  # the read grant refuses it in the service
    call('diag', {'command': 'diag.commands', 'cursor': -1}, False)
    call('diag', {'command': 'diag.commands', 'grant': 'record'}, False)
    call('diag', {'command': 'diag.commands', 'args': {'a': 1}}, False)
    # A headless host without a GPU: ceridc opens no context, and the command says so instead of failing.
    gpu = call('diag', {'command': 'gpu.resources'}, True)
    require('unavailable' in json.dumps(gpu['items']), 'gpu.resources names the GPU evidence it cannot have')


def modern_session(ceridc, scratch, schema):
    session = Session(ceridc, scratch, schema, 'modern')
    try:
        discover = session.result(session.send('server/discover', None, 'DiscoverRequest', modern=True),
                                  'DiscoverResult')
        require(discover['supportedVersions'] == [MODERN, LEGACY], 'discovery names both revisions')
        require(discover['capabilities'] == {'tools': {'listChanged': False}}, 'discovery names the tools capability')
        require(discover['_meta']['io.modelcontextprotocol/serverInfo']['name'] == 'ceridc', 'the server names itself')
        first = session.list_tools(modern=True)
        second = session.list_tools(modern=True)
        require(first == second, 'tools/list is deterministic')
        # The version error names the supported revisions; a malformed _meta is invalid params.
        line = {'jsonrpc': '2.0', 'id': 900, 'method': 'tools/list',
                'params': {'_meta': dict(META, **{'io.modelcontextprotocol/protocolVersion': '2099-01-01'})}}
        session.process.stdin.write(json.dumps(line).encode('utf-8') + b'\n')
        session.process.stdin.flush()
        error = session.error(json.loads(session.process.stdout.readline()), -32022, 'UnsupportedProtocolVersionError')
        require(error['data'] == {'supported': [MODERN, LEGACY], 'requested': '2099-01-01'}, 'the version error data')
        session.process.stdin.write(b'{"jsonrpc":"2.0","id":901,"method":"tools/list","params":{}}\n')
        session.process.stdin.flush()
        session.error(json.loads(session.process.stdout.readline()), -32602)
        session.error(session.send('tools/call', {'name': 'no-such-tool', 'arguments': {}}, 'CallToolRequest',
                                   modern=True), -32602)
        drive_tools(session, True, scratch, 'modern')
    finally:
        session.close()
    return session.checked


def legacy_session(ceridc, scratch, schema, modern_schema):
    session = Session(ceridc, scratch, schema, 'legacy')
    try:
        init = session.result(session.send('initialize', {'protocolVersion': LEGACY, 'capabilities': {},
                                                          'clientInfo': {'name': 'mcp-conformance', 'version': '1'}},
                                           'InitializeRequest', modern=False), 'InitializeResult')
        require(init['protocolVersion'] == LEGACY, 'the handshake answers 2025-11-25')
        session.process.stdin.write(b'{"jsonrpc":"2.0","method":"notifications/initialized"}\n')
        session.process.stdin.flush()
        session.result(session.send('ping', None, 'PingRequest', modern=False), 'EmptyResult')
        session.list_tools(modern=False)
        drive_tools(session, False, scratch, 'legacy')
        # Dual-era: a 2026-07-28 request on the handshake's connection is served by 2026-07-28.
        session.schema = modern_schema
        session.result(session.send('server/discover', None, 'DiscoverRequest', modern=True), 'DiscoverResult')
    finally:
        session.close()
    return session.checked


def main(argv=None):
    parser = argparse.ArgumentParser(prog='mcp_conformance.py', description=__doc__.splitlines()[0])
    parser.add_argument('--ceridc', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        modern_schema = load_schema(MODERN)
        legacy_schema = load_schema(LEGACY)
        with tempfile.TemporaryDirectory(prefix='mcp-conformance-') as scratch:
            checked = modern_session(str(args.ceridc), Path(scratch), modern_schema)
            checked += legacy_session(str(args.ceridc), Path(scratch), legacy_schema, modern_schema)
    except (Failure, subset.SchemaError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print('mcp-conformance: FAILED: %s' % error, file=sys.stderr)
        return 1
    print('mcp-conformance: %d replies validated against the published %s and %s schemas' % (checked, MODERN, LEGACY))
    return 0


if __name__ == '__main__':
    sys.exit(main())
