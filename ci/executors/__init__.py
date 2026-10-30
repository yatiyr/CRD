"""The four executors of the Cerid CI platform (ADR-0135 §4): every one ends with the same pinned `buildkite-agent`
acquiring exactly one job on the machine that runs it.

- gce_vm: a Google Compute Engine spot VM created for the job and deleted after it (cloud CPUs, GPUs, Windows).
- owned_agent: an owned machine whose agent listens on the owned queue (no launch; the operator's hardware).
- firebase_test_lab: a job driven from the launcher that runs the test on Firebase Test Lab devices.
- ssh_host: an existing host reached over SSH (AMD Developer Cloud, a rented Mac, a Windows ARM64 host).

A console or platform pack plugs in through these same interfaces and the device registry; it never adds a fifth
executor kind (ADR-0135 §8).
"""
from __future__ import annotations

from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('gce_vm', 'owned_agent', 'firebase_test_lab', 'ssh_host')
AGENT_PINS = {('linux', 'x86_64'): 'buildkite-agent-linux-amd64', ('linux', 'aarch64'): 'buildkite-agent-linux-arm64',
              ('windows', 'x86_64'): 'buildkite-agent-windows-amd64', ('windows', 'aarch64'): 'buildkite-agent-windows-amd64',
              ('macos', 'aarch64'): 'buildkite-agent-darwin-arm64'}


def os_family(device):
    name = device['os']['name'].lower()
    if name.startswith('windows'):
        return 'windows'
    if name.startswith('macos') or name.startswith('ios') or name.startswith('ipados'):
        return 'macos'
    return 'linux'


def agent_pin(device, pins=None):
    """The pinned agent archive for a device's OS and architecture (cmake/pins.json; the only place a digest lives).
    Windows on ARM runs the x64 agent under emulation: Buildkite publishes no windows-arm64 build."""
    pins = pins or json.loads((ROOT / 'cmake/pins.json').read_text(encoding='utf-8'))
    key = (os_family(device), device['cpu']['arch'])
    name = AGENT_PINS[key]
    return name, pins['tools'][name]


def agent_tags(device):
    return 'device=%s,queue=%s,os=%s,arch=%s' % (device['id'], 'cerid-owned' if device['executor'] == 'owned_agent'
                                                 else 'cerid-cloud', os_family(device), device['cpu']['arch'])


from . import firebase_test_lab, gce_vm, owned_agent, ssh_host  # noqa: E402  (they use the helpers above)


def registry(environ):
    return {'gce_vm': gce_vm.GceVm(environ), 'owned_agent': owned_agent.OwnedAgent(environ),
            'firebase_test_lab': firebase_test_lab.FirebaseTestLab(environ), 'ssh_host': ssh_host.SshHost(environ)}
