"""The orchestrator's pipeline document (`cerid-ci-pipeline/1`): what `ci/pipeline.py upload` and any step that
uploads more steps send to the coordinator.

  {"schema": "cerid-ci-pipeline/1", "env": {...}, "timeout_in_minutes": N, "steps": [step | "wait" | {"wait": ...}]}

A command step:

  key                 unique in the build ([A-Za-z0-9_:@.-], at most 100); generated when absent
  label, command      command is a string or a list of lines, run by the agent's shell in the checkout
  agents              {"queue": q, tag: value | "*"}: the queue and the agent tags it needs
  depends_on          a key, or a list of keys and {"step": key, "allow_failure": bool}
  priority            integer, higher first (default 0)
  concurrency_group   with `concurrency`: at most that many of the group's jobs assigned or running at once,
                      oldest first
  timeout_in_minutes  the job's running time limit (the agent and the coordinator's watchdog both enforce it)
  soft_fail           true, or [{"exit_status": n | "*"}]: such a failure does not fail the build or its dependents
  retry               {"automatic": [{"reason": "agent_lost"|"preempted"|"vm_lost"|"launch_failed"|
                      "infrastructure"|"*", "limit": n} | {"exit_status": n | "*", "limit": n}],
                      "manual": {"allowed": bool}}
  skip                {"state": "cached"|"budget", "reason": text}: created terminal, never run
  env                 environment for this step (merged over the document's)
  artifact_paths      globs (a list, or one string separated by `;`) uploaded after the command
  secrets             secret names, or {"name": n, "env": VAR}: delivered only for trusted builds, redacted from logs
  allow_dependency_failure   shorthand for allow_failure on every dependency

A `wait` step makes every later step of the same document depend on every earlier one (`continue_on_failure` lets
them run after failures). Unknown fields are refused, so a typo can never silently drop a constraint.
"""
from __future__ import annotations

import re

SCHEMA = 'cerid-ci-pipeline/1'
KEY = re.compile(r'^[A-Za-z0-9_:@.\-]{1,100}$')
NAME = re.compile(r'^[A-Za-z0-9_\-]{1,100}$')
ENV_NAME = re.compile(r'^[A-Za-z_][A-Za-z0-9_]{0,100}$')
STEP_FIELDS = {'key', 'label', 'command', 'agents', 'depends_on', 'priority', 'concurrency_group', 'concurrency',
               'timeout_in_minutes', 'soft_fail', 'retry', 'skip', 'env', 'artifact_paths', 'secrets',
               'allow_dependency_failure'}
DOCUMENT_FIELDS = {'schema', 'env', 'steps', 'timeout_in_minutes'}
INFRA_REASONS = ('agent_lost', 'preempted', 'vm_lost', 'launch_failed', 'infrastructure', '*')
MAX_STEPS = 500


class PipelineError(ValueError):
    def __init__(self, problems):
        super().__init__('; '.join(problems))
        self.problems = problems


def _dependencies(value, problems, where):
    if value is None:
        return []
    items = [value] if isinstance(value, (str, dict)) else value
    if not isinstance(items, list):
        problems.append('%s: depends_on must be a key or a list' % where)
        return []
    out = []
    for item in items:
        if isinstance(item, str):
            out.append({'step': item, 'allow_failure': False})
        elif isinstance(item, dict) and isinstance(item.get('step'), str) and set(item) <= {'step', 'allow_failure'}:
            out.append({'step': item['step'], 'allow_failure': bool(item.get('allow_failure', False))})
        else:
            problems.append('%s: bad dependency %r' % (where, item))
    return out


def _retry(value, problems, where):
    if value is None:
        return {'automatic': [], 'manual': {'allowed': True}}
    if not isinstance(value, dict) or not set(value) <= {'automatic', 'manual'}:
        problems.append('%s: retry must be {"automatic": [...], "manual": {...}}' % where)
        return {'automatic': [], 'manual': {'allowed': True}}
    rules = value.get('automatic') or []
    if rules is True:
        rules = [{'reason': 'infrastructure', 'limit': 2}]
    out = []
    for rule in rules if isinstance(rules, list) else []:
        if not isinstance(rule, dict) or not set(rule) <= {'reason', 'exit_status', 'limit'}:
            problems.append('%s: bad retry rule %r' % (where, rule))
            continue
        if ('reason' in rule) == ('exit_status' in rule):
            problems.append('%s: a retry rule names a reason or an exit status' % where)
            continue
        if 'reason' in rule and rule['reason'] not in INFRA_REASONS:
            problems.append('%s: unknown retry reason %r' % (where, rule['reason']))
            continue
        limit = rule.get('limit', 2)
        if not isinstance(limit, int) or not 0 <= limit <= 10:
            problems.append('%s: retry limit must be 0 to 10' % where)
            continue
        out.append(dict(rule, limit=limit))
    manual = value.get('manual') or {'allowed': True}
    return {'automatic': out, 'manual': {'allowed': bool(manual.get('allowed', True))}}


def _secrets(value, problems, where):
    out = []
    for item in value or []:
        if isinstance(item, str):
            item = {'name': item, 'env': re.sub(r'[^A-Za-z0-9_]', '_', item).upper()}
        if not isinstance(item, dict) or not NAME.match(str(item.get('name', ''))) \
                or not ENV_NAME.match(str(item.get('env', ''))):
            problems.append('%s: bad secret %r' % (where, item))
            continue
        out.append({'name': item['name'], 'env': item['env']})
    return out


def _env(value, problems, where):
    if value is None:
        return {}
    if not isinstance(value, dict) or not all(ENV_NAME.match(str(k)) for k in value):
        problems.append('%s: env must map variable names to strings' % where)
        return {}
    return {k: str(v) for k, v in value.items()}


def normalize(document, existing_keys=(), order_start=0):
    """The command steps of an uploaded document, each a complete job specification, or PipelineError with every
    problem. `existing_keys` are the keys already in the build (dependencies may name them; new keys must differ)."""
    problems = []
    if isinstance(document, list):
        document = {'steps': document}
    if not isinstance(document, dict) or not isinstance(document.get('steps'), list):
        raise PipelineError(['a pipeline is {"steps": [...]}'])
    unknown = set(document) - DOCUMENT_FIELDS
    if unknown:
        problems.append('unknown pipeline fields: %s' % ', '.join(sorted(unknown)))
    if document.get('schema', SCHEMA) != SCHEMA:
        problems.append('schema %r is not %s' % (document.get('schema'), SCHEMA))
    if len(document['steps']) > MAX_STEPS:
        problems.append('at most %d steps per upload' % MAX_STEPS)
    base_env = _env(document.get('env'), problems, 'pipeline')
    existing = set(existing_keys)
    seen = set()
    steps = []
    before_wait = []
    barrier = []  # the dependencies a wait step imposes on every later step
    for index, raw in enumerate(document['steps']):
        where = 'step %d' % (index + 1)
        if raw == 'wait' or (isinstance(raw, dict) and 'wait' in raw):
            allow = bool(raw.get('continue_on_failure', False)) if isinstance(raw, dict) else False
            if isinstance(raw, dict) and not set(raw) <= {'wait', 'continue_on_failure', 'key'}:
                problems.append('%s: a wait step takes only continue_on_failure' % where)
            barrier = barrier + [{'step': key, 'allow_failure': allow} for key in before_wait]
            before_wait = []
            continue
        if not isinstance(raw, dict):
            problems.append('%s: a step is an object or "wait"' % where)
            continue
        unknown = set(raw) - STEP_FIELDS
        if unknown:
            problems.append('%s: unknown fields %s' % (where, ', '.join(sorted(unknown))))
        key = raw.get('key') or 'step-%d' % (order_start + index + 1)
        where = 'step %s' % key
        if not KEY.match(str(key)):
            problems.append('%s: bad key' % where)
        if key in seen or key in existing:
            problems.append('%s: duplicate key' % where)
        seen.add(key)
        command = raw.get('command')
        if isinstance(command, list):
            command = '\n'.join(str(line) for line in command)
        skip = raw.get('skip')
        if isinstance(skip, str):
            state = skip.split(':', 1)[0].strip()
            skip = {'state': state, 'reason': skip}
        if skip is not None and (not isinstance(skip, dict) or skip.get('state') not in ('cached', 'budget')):
            problems.append('%s: skip is {"state": "cached"|"budget", "reason": ...}' % where)
            skip = None
        if not command and skip is None:
            problems.append('%s: a command is required' % where)
        agents = raw.get('agents') or {}
        if not isinstance(agents, dict) or not isinstance(agents.get('queue', 'default'), str):
            problems.append('%s: agents must be an object with a queue' % where)
            agents = {}
        agents = {str(k): str(v) for k, v in agents.items()}
        agents.setdefault('queue', 'default')
        deps = _dependencies(raw.get('depends_on'), problems, where)
        if raw.get('allow_dependency_failure'):
            deps = [dict(d, allow_failure=True) for d in deps]
        deps = barrier + [d for d in deps if d['step'] not in {b['step'] for b in barrier}]
        priority = raw.get('priority', 0)
        if not isinstance(priority, int):
            problems.append('%s: priority must be an integer' % where)
            priority = 0
        group = raw.get('concurrency_group')
        limit = raw.get('concurrency')
        if (group is None) != (limit is None):
            problems.append('%s: concurrency_group and concurrency go together' % where)
        if limit is not None and (not isinstance(limit, int) or limit < 1):
            problems.append('%s: concurrency must be a positive integer' % where)
        timeout = raw.get('timeout_in_minutes')
        if timeout is not None and (not isinstance(timeout, (int, float)) or not 0 < timeout <= 7 * 24 * 60):
            problems.append('%s: timeout_in_minutes must be positive and at most a week' % where)
        soft = raw.get('soft_fail', False)
        if not isinstance(soft, (bool, list)):
            problems.append('%s: soft_fail is a boolean or a list of exit statuses' % where)
            soft = False
        paths = raw.get('artifact_paths') or []
        if isinstance(paths, str):
            paths = [p.strip() for p in paths.split(';') if p.strip()]
        for path in paths:
            if '..' in path.replace('\\', '/').split('/') or path.startswith(('/', '\\')) or ':' in path:
                problems.append('%s: artifact path %r leaves the checkout' % (where, path))
        env = dict(base_env)
        env.update(_env(raw.get('env'), problems, where))
        steps.append({'key': key, 'label': str(raw.get('label') or key)[:200], 'command': command or '',
                      'agents': agents, 'depends_on': deps, 'priority': priority, 'concurrency_group': group,
                      'concurrency': limit, 'timeout_minutes': timeout, 'soft_fail': soft,
                      'retry': _retry(raw.get('retry'), problems, where), 'skip': skip, 'env': env,
                      'artifact_paths': list(paths), 'secrets': _secrets(raw.get('secrets'), problems, where),
                      'order': order_start + index})
        before_wait.append(key)
    known = existing | seen
    for step in steps:
        for dep in step['depends_on']:
            if dep['step'] not in known:
                problems.append('step %s: depends on unknown step %s' % (step['key'], dep['step']))
            if dep['step'] == step['key']:
                problems.append('step %s: depends on itself' % step['key'])
    problems += cycles(steps)
    if problems:
        raise PipelineError(problems)
    return steps, document.get('timeout_in_minutes')


def cycles(steps):
    graph = {s['key']: [d['step'] for d in s['depends_on']] for s in steps}
    state = {}
    found = []

    def visit(key, trail):
        if state.get(key) == 'done' or key not in graph:
            return
        if state.get(key) == 'open':
            found.append('dependency cycle: %s' % ' -> '.join(trail + [key]))
            return
        state[key] = 'open'
        for nxt in graph[key]:
            visit(nxt, trail + [key])
        state[key] = 'done'

    for key in graph:
        visit(key, [])
    return found[:1]
