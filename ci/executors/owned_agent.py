"""owned_agent: a machine the operator owns, running the same pinned buildkite-agent on the owned queue.

Nothing is launched: the agent pulls jobs whose `device=` tag names it. Adding owned hardware later is one registry
entry (`ci/ops.py add-device --executor owned_agent`) and this configuration on the machine; no pipeline change. A
console dev kit is an owned agent too: its private platform pack supplies the commands that talk to the kit
(ADR-0135 §8).
"""
from __future__ import annotations

from . import agent_pin, agent_tags, os_family


class OwnedAgent:
    name = 'owned_agent'

    def __init__(self, environ, pins=None):
        self.environ = environ
        self.pins = pins

    def missing(self, device):
        return []

    def launch(self, job_id, device, token):
        raise RuntimeError('owned agents pull their own jobs from the owned queue; the dispatcher never launches one')

    def teardown(self, handle):
        return None

    def config(self, device):
        """buildkite-agent.cfg for the machine. The token line points at a file the operator writes; it is never
        printed or stored here."""
        family = os_family(device)
        token = 'C:\\cerid-ci\\agent-token' if family == 'windows' else '/etc/cerid-ci/agent-token'
        builds = 'C:\\b' if family == 'windows' else '/var/lib/cerid-ci/builds'
        return '\n'.join([
            '# Cerid CI owned agent for %s (ci/executors/owned_agent.py)' % device['id'],
            'token="file://%s"' % token.replace('\\', '/'),
            'name="%s-%%spawn"' % device['id'],
            'tags="%s"' % agent_tags(device),
            'build-path="%s"' % builds,
            'disconnect-after-idle-timeout=0',
            'spawn=1',
            '',
        ])

    def install(self, device):
        """The operator's steps on the machine: the pinned agent archive and its digest, then the service."""
        name, pin = agent_pin(device, self.pins)
        family = os_family(device)
        steps = ['download %s (%s %s)' % (pin['url'], name, pin['version']),
                 'verify SHA-256 %s before extracting' % pin['sha256'],
                 'extract %s and write the configuration above' % pin['member']]
        if family == 'windows':
            steps.append('register it as a service: nssm install buildkite-agent <path>\\buildkite-agent.exe start '
                         '(or a scheduled task at logon for a workstation that only runs between quiet sessions)')
        elif family == 'macos':
            steps.append('install it as a launchd agent (buildkite-agent start under ~/Library/LaunchAgents)')
        else:
            steps.append('install it as a systemd unit running `buildkite-agent start` as an unprivileged user')
        return steps
