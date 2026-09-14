#!/usr/bin/env python3
"""Render the dependency and license manifest from the pinned-input registry (REPO.DEV.10).

  python scripts/gen_license_manifest.py          # (re)write docs/generated/dependency-licenses.md
  python scripts/gen_license_manifest.py --check  # byte-compare the committed document with a fresh rendering

The registry (cmake/pins.json) is the single source: every external input of the build and the hosted workflow
with its version, source, digest and license. This document is its human-readable view; a change to the registry
without a re-render fails `--check` (a CTest, a repository step and a tooling test). No third-party dependencies.
"""
from pathlib import Path
import json
import sys

ROOT = Path(__file__).resolve().parents[1]
REGISTRY = ROOT / 'cmake' / 'pins.json'
OUT = ROOT / 'docs' / 'generated' / 'dependency-licenses.md'


def cell(value):
    return str(value).replace('|', '\\|').replace('\n', ' ')


def render(registry):
    packages = registry['packages']
    tools = registry['tools']
    actions = registry['actions']
    runners = registry['runners']
    unpinned = registry.get('unpinned', {})
    lines = [
        '# Dependency and license manifest (GENERATED -- do not edit)',
        '',
        '> Emitted by `scripts/gen_license_manifest.py` from [cmake/pins.json](../../cmake/pins.json), the registry',
        '> that names every external input once with its version, source, SHA-256 and license',
        '> ([pinned inputs](../design/pinned-inputs.md)). Regenerate with `python scripts/gen_license_manifest.py`;',
        '> `--check` fails on drift. A manifest supplements review, it does not replace it: a new dependency is a',
        '> registry edit, a license reading and a reviewed commit ([contribution routes](../CONTRIBUTING.md)).',
        '',
        f'- packages: **{len(packages)}** · tools: **{len(tools)}** · workflow actions: **{len(actions)}** · '
        f'runner images: **{len(runners)}** · unpinned apt packages: **{len(unpinned.get("apt", []))}**',
        '- Demo assets keep their own terms: [assets/source/LICENSES.md](../../assets/source/LICENSES.md).',
        '- `external/` is git-ignored and never shipped: locally built peer oracles for benchmarks, each under its',
        '  upstream license, outside this registry and outside every build of the engine.',
        '',
        '## Packages (CPM, commit-addressed archives, verified before extraction)',
        '',
        '| Package | Version | License | Source | Archive |',
        '|---|---|---|---|---|',
    ]
    for name, entry in packages.items():
        parts = [entry.get('repository') or entry.get('url', '')]
        if entry.get('ref'):
            parts.append(f'@ `{entry["ref"]}`')
        if entry.get('commit'):
            parts.append(f'({entry["commit"][:12]})')
        lines.append(f'| {cell(name)} | {cell(entry.get("version", ""))} | {cell(entry.get("license", ""))} | '
                     f'{cell(" ".join(parts))} | `{cell(entry.get("file", ""))}` |')
    lines += ['', '## Tools (installers, SDK members and helpers, verified by digest)', '',
              '| Tool | Version | License | Source | File |', '|---|---|---|---|---|']
    for name, entry in tools.items():
        if entry.get('repository'):
            source = f'{entry["repository"]} ({entry.get("commit", "")[:12]})'
        elif entry.get('acquired_by'):
            source = f'pinned action {entry["acquired_by"]}'
        else:
            url = entry.get('url') or entry.get('provenance') or ''
            source = url.split('/')[2] if url.startswith('http') and url.count('/') >= 2 else url
        if entry.get('file'):
            payload = f'`{cell(entry["file"])}`'
        elif entry.get('files'):
            payload = ', '.join(f'`{cell(member)}`' for member in entry['files'])
        else:
            payload = 'installed by the action'
        lines.append(f'| {cell(name)} | {cell(entry.get("version", ""))} | {cell(entry.get("license", ""))} | '
                     f'{cell(source)} | {payload} |')
    lines += ['', '## Workflow actions (pinned to a reviewed commit)', '',
              '| Action | Tag | Commit |', '|---|---|---|']
    for name, entry in actions.items():
        lines.append(f'| {cell(name)} | `{cell(entry["ref"])}` | `{cell(entry["sha"])}` |')
    lines += ['', '## Runner images', '', '| Lane | Image |', '|---|---|']
    for name, image in runners.items():
        lines.append(f'| {cell(name)} | `{cell(image)}` |')
    apt = unpinned.get('apt', [])
    lines += ['', '## Unpinned inputs', '',
              'OS packages of the Linux image, recorded with their installed versions in every Linux lane census: '
              + ', '.join(f'`{cell(p)}`' for p in apt) + '.']
    if unpinned.get('note'):
        lines.append('')
        lines.append(cell(unpinned['note']))
    lines.append('')
    return '\n'.join(lines)


def main(argv):
    registry = json.loads(REGISTRY.read_text(encoding='utf-8'))
    text = render(registry)
    if '--check' in argv:
        committed = OUT.read_bytes().replace(b'\r\n', b'\n') if OUT.is_file() else b''
        if committed != text.encode('utf-8'):
            sys.stderr.write(f'gen_license_manifest: DRIFT -- {OUT.relative_to(ROOT).as_posix()} is stale; '
                             'run scripts/gen_license_manifest.py to refresh\n')
            return 1
        print(f'gen_license_manifest: {OUT.relative_to(ROOT).as_posix()} matches the registry')
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(text.encode('utf-8'))
    print(f'gen_license_manifest: wrote {OUT.relative_to(ROOT).as_posix()} ({len(registry["packages"])} packages, '
          f'{len(registry["tools"])} tools, {len(registry["actions"])} actions)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
