#!/usr/bin/env python3
"""Validate the pin registry and the files that must agree with it.

Contract: docs/design/pinned-inputs.md. `cmake/pins.json` is the only place a version, URL or digest is written.
This guard fails when an entry is incomplete, when the workflow references an action by anything but its pinned
commit, runs on an unpinned runner label, checks out with persisted credentials, or names a tool version the
registry does not, when the root build file acquires a package outside the registry, when a helper carries a
literal digest, or when a declared patch spec is missing.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PINS = ROOT / 'cmake/pins.json'
DEFAULT_WORKFLOW = ROOT / '.github/workflows/ci.yml'
DEFAULT_CMAKE = ROOT / 'CMakeLists.txt'
DEFAULT_TIERS = ROOT / '.github/ci-tiers.json'
HELPERS = ('cmake/CPM.cmake', 'scripts/install-vulkan-sdk.py', 'scripts/install-vulkan-headers.py',
           'scripts/install-vulkan-validation.py', 'scripts/install-warp.py', 'scripts/install-sccache.py',
           'scripts/pins.py')
SCHEMA = 'cerid-pins/1'
HEX64 = re.compile(r'^[0-9a-f]{64}$')
HEX40 = re.compile(r'^[0-9a-f]{40}$')
LITERAL_DIGEST = re.compile(r'\b[0-9a-f]{64}\b')
FORBIDDEN_CMAKE = ('CPMAddPackage(', 'GIT_TAG', 'GITHUB_REPOSITORY', 'GITLAB_REPOSITORY', '"gh:', '"gl:')


def load(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def validate_registry(pins):
    problems = []
    if pins.get('schema') != SCHEMA:
        problems.append('schema must be %s' % SCHEMA)
    for section in ('packages', 'tools', 'actions', 'runners'):
        if not isinstance(pins.get(section), dict) or not pins[section]:
            problems.append('section %s is missing or empty' % section)
    for name, entry in pins.get('packages', {}).items():
        for field in ('version', 'url', 'file', 'sha256', 'license'):
            if not entry.get(field):
                problems.append('package %s lacks %s' % (name, field))
        if entry.get('url', '').startswith('https://github.com/') and not entry.get('commit'):
            problems.append('package %s from GitHub must name its commit' % name)
        if entry.get('commit') and (not HEX40.match(entry['commit']) or entry['commit'] not in entry.get('url', '')):
            problems.append('package %s: url must be addressed by its commit' % name)
        if entry.get('sha256') and not HEX64.match(entry['sha256']):
            problems.append('package %s: sha256 must be 64 hex digits' % name)
        for patch in entry.get('patches', []):
            if not (ROOT / 'cmake/patches' / (patch + '.cmake')).is_file():
                problems.append('package %s declares patch %s but cmake/patches/%s.cmake is missing' % (name, patch, patch))
    for name, entry in pins.get('tools', {}).items():
        if not entry.get('version') or not entry.get('license'):
            problems.append('tool %s lacks version or license' % name)
        if 'files' in entry:
            for file_name, spec in entry['files'].items():
                if not spec.get('url') or not HEX64.match(spec.get('sha256', '')):
                    problems.append('tool %s file %s lacks url or a 64-hex sha256' % (name, file_name))
            if entry.get('commit') and not HEX40.match(entry['commit']):
                problems.append('tool %s: commit must be 40 hex digits' % name)
        elif 'acquired_by' in entry:
            if entry['acquired_by'] not in pins.get('actions', {}):
                problems.append('tool %s is acquired by an action outside the registry' % name)
        else:
            for field in ('url', 'file', 'sha256'):
                if not entry.get(field):
                    problems.append('tool %s lacks %s' % (name, field))
            if entry.get('sha256') and not HEX64.match(entry['sha256']):
                problems.append('tool %s: sha256 must be 64 hex digits' % name)
    for name, entry in pins.get('actions', {}).items():
        if not entry.get('ref') or not HEX40.match(entry.get('sha', '')):
            problems.append('action %s needs a ref and a 40-hex sha' % name)
    for name, label in pins.get('runners', {}).items():
        if label.endswith('-latest'):
            problems.append('runner %s uses the floating label %s' % (name, label))
    return problems


def validate_workflow(pins, text):
    problems = []
    actions = pins.get('actions', {})
    for match in re.finditer(r'^\s*(?:- )?uses:\s*(\S+)@(\S+)(.*)$', text, re.M):
        action, ref, rest = match.group(1), match.group(2), match.group(3)
        pin = actions.get(action)
        if pin is None:
            problems.append('workflow uses %s, which is not in the registry' % action)
        elif ref != pin['sha']:
            problems.append('workflow uses %s@%s; the registry pins %s (%s)' % (action, ref, pin['sha'], pin['ref']))
        elif ('# ' + pin['ref']) not in rest:
            problems.append('workflow uses %s@%s without the "# %s" comment' % (action, ref, pin['ref']))
    runners = set(pins.get('runners', {}).values())
    for match in re.finditer(r'^\s*runs-on:\s*(\S+)\s*$', text, re.M):
        label = match.group(1)
        if label.startswith('${{'):
            continue
        if label not in runners:
            problems.append('workflow runs on %s, which is not a pinned runner' % label)
    for match in re.finditer(r'^\s*os:\s*\[([^\]]*)\]', text, re.M):
        for label in (item.strip() for item in match.group(1).split(',')):
            if label and label not in runners:
                problems.append('workflow matrix runs on %s, which is not a pinned runner' % label)
    checkouts = len(re.findall(r'uses:\s*actions/checkout@', text))
    persisted = len(re.findall(r'persist-credentials:\s*false', text))
    if checkouts != persisted:
        problems.append('%d checkouts but %d persist-credentials: false' % (checkouts, persisted))
    tools = pins.get('tools', {})
    llvm = tools.get('llvm', {}).get('version')
    if llvm and ("version: '%s'" % llvm) not in text:
        problems.append('workflow does not install LLVM %s' % llvm)
    actionlint = tools.get('actionlint', {})
    if actionlint and (actionlint.get('file', '') not in text or ('actionlint %s' % actionlint.get('version')) not in text):
        problems.append('workflow does not run actionlint %s from the pinned archive' % actionlint.get('version'))
    sdk = tools.get('vulkan-sdk-windows', {})
    if sdk:
        version = sdk['version']
        if ('vulkan-sdk-%s-windows' % version) not in text or ('C:\\VulkanSDK\\%s' % version) not in text:
            problems.append('workflow does not name Vulkan SDK %s' % version)
        for other in re.findall(r'VulkanSDK\\(\d+\.\d+\.\d+\.\d+)', text):
            if other != version:
                problems.append('workflow names Vulkan SDK %s, registry pins %s' % (other, version))
    for literal in ('sdk.lunarg.com', 'raw.githubusercontent.com', 'git clone', 'mozilla/sccache/releases'):
        if literal in text:
            problems.append('workflow acquires %s directly; use the pinned helper' % literal)
    if LITERAL_DIGEST.search(text):
        problems.append('workflow carries a literal digest; digests live only in cmake/pins.json')
    return problems


def validate_cmake(pins, text):
    problems = []
    for token in FORBIDDEN_CMAKE:
        if token in text:
            problems.append('CMakeLists.txt acquires a package outside the registry (%s)' % token)
    used = set(re.findall(r'crd_add_pinned_package\((\w+)', text))
    packages = set(pins.get('packages', {}))
    for name in sorted(used - packages):
        problems.append('CMakeLists.txt adds unpinned package %s' % name)
    for name in sorted(packages - used):
        problems.append('registry package %s is never added' % name)
    return problems


def validate_helpers(root=ROOT):
    problems = []
    for relative in HELPERS:
        path = root / relative
        if not path.is_file():
            problems.append('%s is missing' % relative)
            continue
        if LITERAL_DIGEST.search(path.read_text(encoding='utf-8')):
            problems.append('%s carries a literal digest; digests live only in cmake/pins.json' % relative)
    return problems


def validate_tiers(pins, tiers):
    runners = set(pins.get('runners', {}).values())
    return ['ci-tiers.json job %s runs on %s, which is not a pinned runner' % (job, spec.get('runner'))
            for job, spec in tiers.get('jobs', {}).items() if spec.get('runner') not in runners]


def validate(pins, workflow_text, cmake_text, tiers, root=ROOT):
    return (validate_registry(pins) + validate_workflow(pins, workflow_text) + validate_cmake(pins, cmake_text)
            + validate_helpers(root) + validate_tiers(pins, tiers))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--pins', default=str(DEFAULT_PINS))
    parser.add_argument('--workflow', default=str(DEFAULT_WORKFLOW))
    parser.add_argument('--cmake', default=str(DEFAULT_CMAKE))
    parser.add_argument('--tiers', default=str(DEFAULT_TIERS))
    args = parser.parse_args(argv)
    pins = load(args.pins)
    problems = validate(pins, Path(args.workflow).read_text(encoding='utf-8'),
                        Path(args.cmake).read_text(encoding='utf-8-sig'), load(args.tiers))
    for problem in problems:
        print('FAIL: ' + problem)
    if problems:
        return 1
    print('Checked %d packages, %d tools, %d actions and %d runners against the workflow, the build and the helpers.'
          % (len(pins['packages']), len(pins['tools']), len(pins['actions']), len(pins['runners'])))
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
