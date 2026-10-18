#!/usr/bin/env python3
"""Validate the CI tier mapping against the presets and the workflow.

Contract: docs/design/ci-tiers.md. Every visible configure preset has exactly one owner in
`.github/ci-tiers.json`; every owning job reads its preset list from the preflight job, is gated on a non-empty
list, and is a dependency of the required aggregate; a preset step (a non-CMake check riding on preset matrix
entries, such as the GenMC model on linux-clang-tsan or the seeded detector failures on the five lanes they were
qualified on) exists in each listed preset's job, is gated on exactly the listed presets and inherits each one's
tier; every compiler-cached lane (CRD_CACHED_PRESETS) is a hosted preset the local cache rule covers, never a
cold-acquisition preset, built in a job with the cache steps and statistics (docs/design/build-performance.md#hosted-lanes);
the class table ends with a catch-all so every changed path resolves to a tier; cancellation stays limited to
superseded pull-request runs. Every other workflow file is an experiment triggered only by workflow_dispatch, so it
never joins a tier; only there may a diagnostic preset run hosted.
"""
from pathlib import Path
import argparse
import json
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_TIERS = ROOT / '.github/ci-tiers.json'
DEFAULT_PRESETS = ROOT / 'CMakePresets.json'
DEFAULT_WORKFLOW = ROOT / '.github/workflows/ci.yml'
TIER_ORDER = ('preflight', 'change', 'complete')
PRESET_TIERS = ('change', 'complete', 'diagnostic')
FIXED_JOBS = ('preflight', 'repository', 'required')


def output_key(job):
    """GITHUB_OUTPUT key carrying a job's preset list."""
    return 'presets_' + job.replace('-', '_')


def load_json(path):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def visible_presets(presets_document):
    return [p['name'] for p in presets_document['configurePresets'] if not p.get('hidden')]


def workflow_jobs(text):
    """Split the workflow text into {job id: block text}; job ids sit at two-space indentation under `jobs:`."""
    start = text.find('\njobs:\n')
    if start < 0:
        return {}
    body = text[start + len('\njobs:\n'):]
    blocks = {}
    current = None
    for line in body.splitlines(keepends=True):
        match = re.match(r'^  ([A-Za-z0-9_-]+):\s*$', line)
        if match:
            current = match.group(1)
            blocks[current] = ''
        elif current is not None:
            blocks[current] += line
    return blocks


def validate_preset_step(step, spec, entries, blocks):
    """Problems of one preset step.

    A step rides on one preset (`preset` with the `tier` it inherits, e.g. the GenMC model on linux-clang-tsan) or
    on several (`presets`, each leg in its own preset's tier, e.g. the seeded detector failures). In every owning job
    the step exists, is gated on each listed preset of that job, and names no preset the mapping does not list.
    """
    if 'presets' in spec:
        presets = spec.get('presets')
        listed_once = isinstance(presets, list) and presets and len(set(presets)) == len(presets)
        if set(spec) != {'presets'} or not listed_once:
            return ['step %r lists its presets once each and declares no tier of its own (each leg inherits its '
                    "preset's)" % step]
    elif set(spec) == {'preset', 'tier'}:
        presets = [spec['preset']]
    else:
        return ['step %r declares preset and tier, or a presets list' % step]
    problems = []
    gated = {}
    for preset in presets:
        entry = entries.get(preset)
        if entry is None or entry.get('tier') == 'diagnostic':
            problems.append('step %r rides on %r, which is not a hosted preset' % (step, preset))
            continue
        if 'tier' in spec and entry.get('tier') != spec['tier']:
            problems.append('step %r is declared %s-tier but its preset %s is %s-tier'
                            % (step, spec['tier'], preset, entry.get('tier')))
        job = entry.get('job', '')
        step_block = re.search(r'^ +- name: %s\n((?: {8,}\S.*\n)+)' % re.escape(step), blocks.get(job, ''), re.M)
        if step_block is None:
            problems.append('job %s lacks the step %r' % (job, step))
            continue
        gated.setdefault(job, set(re.findall(r"matrix\.preset == '([^']+)'", step_block.group(1))))
        if preset not in gated[job]:
            problems.append("step %r must be gated on matrix.preset == '%s'" % (step, preset))
    for job, names in sorted(gated.items()):
        for name in sorted(names - set(presets)):
            problems.append("step %r in job %s is gated on %s, which the mapping does not list" % (step, job, name))
    return problems


CACHED_LANE_STEPS = ('Install pinned sccache', 'Compiler cache service', 'Configure with the compiler cache',
                     'Compiler cache statistics')


def env_preset_list(workflow_text, name):
    """The preset names of a workflow-level list variable such as CRD_CACHED_PRESETS (a quoted scalar that may be
    folded over several lines), or None when the workflow does not define it."""
    match = re.search(r"^  %s: '([^']*)'" % re.escape(name), workflow_text, re.M)
    return None if match is None else match.group(1).split()


def cache_eligible(preset):
    """The local helpers' rule (scripts/compiler-cache.py): a Ninja preset that is neither a clang-tidy nor an MSVC
    LTCG preset."""
    import importlib.util
    spec = importlib.util.spec_from_file_location('compiler_cache', ROOT / 'scripts/compiler-cache.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return bool(module.configure_args(preset, {'CRD_SCCACHE': 'sccache'}))


def validate_cached_presets(entries, workflow_text, blocks, eligible=cache_eligible):
    """Every lane in CRD_CACHED_PRESETS is a hosted preset the local rule caches, is not a cold-acquisition preset
    (those stay the uncached reference build), and builds in a job that installs the pinned sccache, starts it on the
    cache service, configures with it, prints its statistics and hands them to the evidence bundle."""
    cached = env_preset_list(workflow_text, 'CRD_CACHED_PRESETS')
    if cached is None:
        return ['the workflow defines no CRD_CACHED_PRESETS']
    problems = []
    cold = set(env_preset_list(workflow_text, 'CRD_COLD_PRESETS') or ())
    for preset in cached:
        entry = entries.get(preset)
        if entry is None or entry.get('tier') == 'diagnostic' or 'job' not in entry:
            problems.append('cached preset %s is not a hosted preset' % preset)
            continue
        if preset in cold:
            problems.append('cached preset %s is a cold-acquisition preset, which stays uncached' % preset)
        if not eligible(preset):
            problems.append('cached preset %s is one the compiler-cache rule leaves uncached (tidy, Visual Studio or '
                            'MSVC LTCG)' % preset)
        block = blocks.get(entry['job'], '')
        for step in CACHED_LANE_STEPS:
            if '- name: %s\n' % step not in block:
                problems.append('job %s builds cached preset %s but lacks the step %r' % (entry['job'], preset, step))
        if '--sccache-stats' not in block:
            problems.append('job %s builds cached preset %s but its evidence bundle gets no --sccache-stats'
                            % (entry['job'], preset))
    if len(set(cached)) != len(cached):
        problems.append('CRD_CACHED_PRESETS names a preset twice')
    return problems


def validate(tiers, presets_document, workflow_text):
    problems = []
    if tiers.get('schema') != 'cerid-ci-tiers/1':
        problems.append('unknown schema %r' % tiers.get('schema'))
    if tuple(tiers.get('tiers', ())) != TIER_ORDER:
        problems.append('tiers must be %s' % list(TIER_ORDER))
    visible = visible_presets(presets_document)
    entries = tiers.get('presets', {})
    jobs = tiers.get('jobs', {})
    for name in visible:
        if name not in entries:
            problems.append('visible preset %s has no tier owner' % name)
    for name, entry in entries.items():
        if name not in visible:
            problems.append('tier entry %s is not a visible configure preset' % name)
        tier = entry.get('tier')
        if tier not in PRESET_TIERS:
            problems.append('preset %s has tier %r; expected one of %s' % (name, tier, list(PRESET_TIERS)))
        if tier == 'diagnostic':
            if 'job' in entry:
                problems.append('diagnostic preset %s must not name a job' % name)
            if re.search(r'--preset\s+%s\b' % re.escape(name), workflow_text):
                problems.append('diagnostic preset %s is invoked by the workflow' % name)
        elif entry.get('job') not in jobs:
            problems.append('preset %s names unknown job %r' % (name, entry.get('job')))
    for job in FIXED_JOBS:
        if job in jobs:
            problems.append('job %s is fixed by the workflow and cannot own presets' % job)
    for job, spec in jobs.items():
        if not spec.get('runner'):
            problems.append('job %s has no runner' % job)
        if not any(entry.get('job') == job for entry in entries.values()):
            problems.append('job %s owns no preset' % job)

    blocks = workflow_jobs(workflow_text)
    for job in FIXED_JOBS:
        if job not in blocks:
            problems.append('workflow lacks the %s job' % job)
    required = blocks.get('required', '')
    needs = re.search(r'^\s+needs:\s*\[([^\]]*)\]', required, re.M)
    required_needs = {item.strip() for item in needs.group(1).split(',')} if needs else set()
    if 'if: always()' not in required:
        problems.append('required job must run with if: always()')
    for job in ('preflight', 'repository'):
        if job not in required_needs:
            problems.append('required job must need %s' % job)
    for job in jobs:
        if job not in required_needs:
            problems.append('required job must need %s' % job)
        block = blocks.get(job)
        if block is None:
            problems.append('workflow lacks job %s' % job)
            continue
        key = output_key(job)
        if not re.search(r'^\s+needs:\s*preflight\s*$', block, re.M):
            problems.append('job %s must need preflight' % job)
        if ("if: needs.preflight.outputs.%s != '[]'" % key) not in block:
            problems.append("job %s must be gated on needs.preflight.outputs.%s != '[]'" % (job, key))
        if ('fromJSON(needs.preflight.outputs.%s)' % key) not in block:
            problems.append('job %s must take its matrix from fromJSON(needs.preflight.outputs.%s)' % (job, key))
        if 'runs-on: %s' % jobs[job]['runner'] not in block:
            problems.append('job %s must run on %s' % (job, jobs[job]['runner']))
    for step, spec in tiers.get('preset_steps', {}).items():
        problems.extend(validate_preset_step(step, spec, entries, blocks))
    problems.extend(validate_cached_presets(entries, workflow_text, blocks))
    for name, entry in entries.items():
        test = entry.get('test')
        if test and not re.search(r'--preset\s+%s\b' % re.escape(test), blocks.get(entry.get('job', ''), '')):
            problems.append('preset %s declares test preset %s, which its job never invokes' % (name, test))
    preflight = blocks.get('preflight', '')
    for job in jobs:
        if ('%s: ${{ steps.tier.outputs.%s }}' % (output_key(job), output_key(job))) not in preflight:
            problems.append('preflight must publish output %s' % output_key(job))
    for key in ('tier', 'expected'):
        if ('%s: ${{ steps.tier.outputs.%s }}' % (key, key)) not in preflight:
            problems.append('preflight must publish output %s' % key)

    classes = tiers.get('classes', [])
    if not classes:
        problems.append('no path classes')
    for cls in classes:
        if cls.get('tier') not in TIER_ORDER:
            problems.append('class %r has tier %r' % (cls.get('name'), cls.get('tier')))
        if not cls.get('patterns'):
            problems.append('class %r has no patterns' % cls.get('name'))
    if classes and '**' not in classes[-1].get('patterns', []):
        problems.append('the last class must be the catch-all (**)')

    triggers = re.search(r'^on:\n((?:  .*\n|\n)+)', workflow_text, re.M)
    trigger_text = triggers.group(1) if triggers else ''
    for trigger in ('push:', 'pull_request:', 'schedule:', 'workflow_dispatch:'):
        if ('  ' + trigger) not in trigger_text:
            problems.append('workflow must define the %s trigger' % trigger[:-1])
    cancel = re.search(r'^\s+cancel-in-progress:\s*(.*)$', workflow_text, re.M)
    if not re.search(r'^concurrency:\s*$', workflow_text, re.M) or cancel is None:
        problems.append('workflow must declare a concurrency group with cancel-in-progress')
    elif "github.event_name == 'pull_request'" not in cancel.group(1) or cancel.group(1).strip() == 'true':
        problems.append('cancel-in-progress must be limited to pull_request runs')
    return problems


def validate_experiment(name, text):
    """Any workflow file besides the CI workflow is an experiment: it runs only when dispatched, so it can never join
    a tier, and only there may a diagnostic preset run on a hosted machine."""
    problems = []
    triggers = re.search(r'^on:\n((?:  .*\n|\n)+)', text, re.M)
    names = re.findall(r'^  ([\w-]+):', triggers.group(1), re.M) if triggers else []
    if names != ['workflow_dispatch']:
        problems.append('%s must be triggered only by workflow_dispatch (found %s)'
                        % (name, ', '.join(names) or 'none'))
    if re.search(r'^\s+cancel-in-progress:\s*true\s*$', text, re.M):
        problems.append('%s must not cancel a running experiment' % name)
    return problems


def validate_experiments(workflow_dir, main_workflow):
    problems = []
    for path in sorted(Path(workflow_dir).glob('*.y*ml')):
        if path.resolve() != Path(main_workflow).resolve():
            problems += validate_experiment(path.name, path.read_text(encoding='utf-8'))
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tiers', default=str(DEFAULT_TIERS))
    parser.add_argument('--presets', default=str(DEFAULT_PRESETS))
    parser.add_argument('--workflow', default=str(DEFAULT_WORKFLOW))
    parser.add_argument('--workflow-dir', default=str(DEFAULT_WORKFLOW.parent),
                        help='the directory whose other workflow files must be dispatch-only experiments')
    args = parser.parse_args(argv)
    tiers = load_json(args.tiers)
    presets = load_json(args.presets)
    workflow = Path(args.workflow).read_text(encoding='utf-8')
    problems = validate(tiers, presets, workflow) + validate_experiments(args.workflow_dir, args.workflow)
    for problem in problems:
        print('FAIL: ' + problem)
    if problems:
        return 1
    owned = [n for n, e in tiers['presets'].items() if e['tier'] != 'diagnostic']
    print('Checked %d visible presets: %d owned by %d jobs, %d diagnostic; %d path classes.'
          % (len(visible_presets(presets)), len(owned), len(tiers['jobs']),
             len(tiers['presets']) - len(owned), len(tiers['classes'])))
    print('PASS')
    return 0


if __name__ == '__main__':
    sys.exit(main())
