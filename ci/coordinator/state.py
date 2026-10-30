"""Job and build state machines of the Cerid orchestrator (ADR-0135 §12).

A job is one attempt-carrying unit of work: a step of a build on one agent. Its states are exactly

  scheduled  waiting for its dependencies or for an agent (its `ready` time says which)
  assigned   handed to an agent or reserved for a launched machine, not yet started
  running    the agent started the command
  passed, failed, canceled, timed_out, expired, budget, cached   terminal

`cached` and `budget` are terminal at creation (the planner's skip states). Every allowed move is a row of
TRANSITIONS; `transition()` refuses anything else, so a race or a stale request can never push a job backwards
silently. Automatic retry after an infrastructure loss (agent lost, VM preempted or lost, launch failure) is the
move back to `scheduled` with the attempt counter raised; a manual retry is the same move from a terminal failure.
"""
from __future__ import annotations

JOB_STATES = ('scheduled', 'assigned', 'running', 'passed', 'failed', 'canceled', 'timed_out', 'expired', 'budget',
              'cached')
TERMINAL = frozenset(('passed', 'failed', 'canceled', 'timed_out', 'expired', 'budget', 'cached'))
ACTIVE = frozenset(('assigned', 'running'))
INITIAL = frozenset(('scheduled', 'cached', 'budget'))
# A dependency in one of these states lets its dependents run; any other terminal state breaks them unless the
# dependency allows failure. A soft-failed job counts as passed for its dependents (and for the build).
SATISFIED = frozenset(('passed', 'cached'))

# (from, to) -> the events that may cause it.
TRANSITIONS = {
    ('scheduled', 'assigned'): ('assign', 'reserve'),
    ('scheduled', 'canceled'): ('cancel', 'dependency_failed'),
    ('scheduled', 'expired'): ('expire',),
    ('scheduled', 'budget'): ('budget',),
    ('scheduled', 'failed'): ('launch_failed', 'no_device'),
    ('assigned', 'running'): ('start',),
    ('assigned', 'scheduled'): ('requeue',),
    ('assigned', 'canceled'): ('cancel',),
    ('assigned', 'failed'): ('infrastructure',),
    ('assigned', 'timed_out'): ('timeout',),
    ('running', 'passed'): ('finish',),
    ('running', 'failed'): ('finish', 'infrastructure'),
    ('running', 'canceled'): ('finish', 'cancel'),
    ('running', 'timed_out'): ('finish', 'timeout'),
    ('running', 'scheduled'): ('requeue', 'retry_exit'),
    ('failed', 'scheduled'): ('retry',),
    ('canceled', 'scheduled'): ('retry',),
    ('timed_out', 'scheduled'): ('retry',),
    ('expired', 'scheduled'): ('retry',),
    ('budget', 'scheduled'): ('retry',),
}

BUILD_STATES = ('scheduled', 'running', 'passed', 'failed', 'canceled')
BUILD_TERMINAL = frozenset(('passed', 'failed', 'canceled'))
BUILD_TRANSITIONS = {
    ('scheduled', 'running'): ('start',),
    ('scheduled', 'canceled'): ('finish',),
    ('scheduled', 'passed'): ('finish',),
    ('scheduled', 'failed'): ('finish',),
    ('running', 'passed'): ('finish',),
    ('running', 'failed'): ('finish',),
    ('running', 'canceled'): ('finish',),
    ('passed', 'running'): ('retry',),
    ('failed', 'running'): ('retry',),
    ('canceled', 'running'): ('retry',),
}


class TransitionError(ValueError):
    """A move the state machine does not allow (a stale or racing request)."""


def check(table, current, target, event):
    if event not in table.get((current, target), ()):
        raise TransitionError('%s -> %s by %s is not a transition' % (current, target, event))


def transition(job, target, event, now, reason=''):
    """Move `job` (a record, changed in place and returned) to `target` by `event`, with its history."""
    check(TRANSITIONS, job['state'], target, event)
    job.setdefault('history', []).append({'at': round(now, 3), 'from': job['state'], 'to': target, 'event': event,
                                          'reason': reason[:500]})
    job['state'] = target
    if reason:
        job['reason'] = reason[:500]
    if target in TERMINAL:
        job['finished_at'] = round(now, 3)
    return job


def build_transition(build, target, event, now):
    check(BUILD_TRANSITIONS, build['state'], target, event)
    build.setdefault('history', []).append({'at': round(now, 3), 'from': build['state'], 'to': target,
                                            'event': event})
    build['state'] = target
    if target in BUILD_TERMINAL:
        build['finished_at'] = round(now, 3)
    elif target == 'running':
        build.setdefault('started_at', round(now, 3))
        build.pop('finished_at', None)
    return build


def soft_failed(job):
    """A failed job whose step allows that failure: `soft_fail: true`, or a list of allowed exit statuses."""
    if job['state'] != 'failed':
        return False
    rule = job.get('soft_fail')
    if rule is True:
        return True
    if isinstance(rule, list):
        return any(r.get('exit_status') in ('*', job.get('exit_status')) for r in rule)
    return False


def build_outcome(build, jobs):
    """The terminal state a build reaches once every job is terminal, else None. Cached and budget jobs never fail a
    build by themselves: the planner's `required` step turns a budget step into a failed required result."""
    if any(job['state'] not in TERMINAL for job in jobs):
        return None
    if build.get('cancel_requested'):
        return 'canceled'
    for job in jobs:
        if job['state'] in ('failed', 'timed_out', 'expired') and not soft_failed(job):
            return 'failed'
        if job['state'] == 'canceled':
            return 'canceled' if not job.get('reason', '').startswith('dependency') else 'failed'
    return 'passed'


def dependency_state(job, by_key):
    """'ready', 'waiting' or 'broken: <why>' for a scheduled job against the jobs of its build."""
    for dep in job.get('depends_on', []):
        other = by_key.get(dep['step'])
        if other is None:
            return 'waiting'
        if other['state'] not in TERMINAL:
            return 'waiting'
        if other['state'] in SATISFIED or soft_failed(other) or dep.get('allow_failure'):
            continue
        return 'broken: dependency %s %s' % (dep['step'], other['state'])
    return 'ready'


def tags_match(wanted, offered):
    """Agent matching: every tag the step asks for (except its queue) has the agent's value, `*` meaning present."""
    for name, value in wanted.items():
        if name == 'queue':
            continue
        have = offered.get(name)
        if have is None:
            return False
        if value != '*' and str(value) != str(have):
            return False
    return True


def retry_limit(job, reason=None, exit_status=None):
    """How many automatic retries the step allows for an infrastructure `reason` or an `exit_status`."""
    rules = (job.get('retry') or {}).get('automatic') or []
    for rule in rules:
        if reason is not None and rule.get('reason') in (reason, '*', 'infrastructure'):
            return int(rule.get('limit', 2))
        if exit_status is not None and 'exit_status' in rule and rule['exit_status'] in ('*', exit_status):
            return int(rule.get('limit', 2))
    return 0
