"""owned_agent: a machine the operator owns, running the Cerid agent in long-running mode on the owned queue.

Nothing is launched: the agent polls the coordinator and takes the jobs whose `device` tag names it. Adding owned
hardware later is one registry entry (`ci/ops.py add-device --executor owned_agent`), one registration token
(`ci/ops.py agent-token create`, written to a file only the agent's user reads; its tags pin the agent to its cell)
and the service below; no pipeline change. A console dev kit is an owned agent too: its private platform pack
supplies the commands that talk to the kit (ADR-0135 §8).
"""
from __future__ import annotations

from . import os_family, tags_text


class OwnedAgent:
    name = 'owned_agent'

    def __init__(self, environ):
        self.environ = environ

    def missing(self, device):
        return []

    def launch(self, machine, device, agent):
        raise RuntimeError('owned agents poll the owned queue; the dispatcher never launches one')

    def teardown(self, handle):
        return None

    def paths(self, device):
        if os_family(device) == 'windows':
            return {'checkout': 'C:\\cerid-ci\\cerid', 'token': 'C:\\cerid-ci\\registration-token',
                    'builds': 'C:\\b', 'python': 'py -3.12'}
        return {'checkout': '/opt/cerid-ci/cerid', 'token': '/etc/cerid-ci/registration-token',
                'builds': '/var/lib/cerid-ci/builds', 'python': 'python3'}

    def command(self, device, coordinator='https://<coordinator>'):
        """The agent's command line. The token is a file named here, never a value."""
        paths = self.paths(device)
        sep = '\\' if os_family(device) == 'windows' else '/'
        return ('%s %s%sci%sagent%sagent.py start --coordinator %s --mode long-running --registration-token-file %s '
                '--name %s --tags %s --build-path %s' % (paths['python'], paths['checkout'], sep, sep, sep, coordinator,
                                                         paths['token'], device['id'], tags_text(device),
                                                         paths['builds']))

    def config(self, device, coordinator='https://<coordinator>'):
        return '\n'.join(['# Cerid CI owned agent for %s (ci/executors/owned_agent.py)' % device['id'],
                          self.command(device, coordinator), ''])

    def install(self, device):
        """The operator's steps on the machine: a checkout pinned to a reviewed revision, the token file, the
        service."""
        paths = self.paths(device)
        family = os_family(device)
        steps = ['clone the repository to %s at the revision the coordinator runs (git checkout <tag>); the agent is '
                 'one standard-library Python file there, ci/agent/agent.py' % paths['checkout'],
                 'on the launcher: python ci/ops.py agent-token create --name %s --queue cerid-owned --tags device=%s '
                 '--out <file>, then move that file to %s on this machine, readable only by the agent user' % (
                     device['id'], device['id'], paths['token'])]
        if family == 'windows':
            steps.append('register the command above as a service (nssm install cerid-agent <command>) or as a '
                         'scheduled task at logon for a workstation that only runs between quiet sessions')
        elif family == 'macos':
            steps.append('install the command above as a launchd agent under ~/Library/LaunchAgents (KeepAlive true)')
        else:
            steps.append('install the command above as a systemd unit (Restart=always) running as an unprivileged '
                         'user')
        return steps
