"""The four executors of the Cerid CI platform (ADR-0135 §4, amended by §12): every one ends with the Cerid agent
(`ci/agent/agent.py`) running exactly one job, or polling its queue on an owned machine.

- gce_vm: a Google Compute Engine spot VM created for the job and deleted after it (cloud CPUs, GPUs, Windows); its
  agent proves the VM's Google identity, so the machine is given no credential at all.
- owned_agent: an owned machine whose long-running agent polls the owned queue (no launch; the operator's hardware).
- firebase_test_lab: a one-job agent process on the launcher that drives Firebase Test Lab devices.
- ssh_host: an existing host reached over SSH (AMD Developer Cloud, a rented Mac, a Windows ARM64 host), given one
  job token that expires with the job.

Every launched machine fetches the agent from the coordinator and refuses it unless its SHA-256 matches the digest
the coordinator computed from its own checkout (passed in the launch, never stored in a source file). A console or
platform pack plugs in through these same interfaces and the device registry; it never adds a fifth executor kind
(ADR-0135 §8).
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
NAMES = ('gce_vm', 'owned_agent', 'firebase_test_lab', 'ssh_host')
AGENT = ROOT / 'ci/agent/agent.py'


def os_family(device):
    name = device['os']['name'].lower()
    if name.startswith('windows'):
        return 'windows'
    if name.startswith('macos') or name.startswith('ios') or name.startswith('ipados'):
        return 'macos'
    return 'linux'


def agent_tags(device):
    """The tags an agent of this cell offers (its queue comes from its registration or its launch)."""
    return {'device': device['id'], 'os': os_family(device), 'arch': device['cpu']['arch']}


def tags_text(device):
    return ','.join('%s=%s' % item for item in sorted(agent_tags(device).items()))


from . import firebase_test_lab, gce_vm, owned_agent, ssh_host  # noqa: E402  (they use the helpers above)


def registry(environ):
    return {'gce_vm': gce_vm.GceVm(environ), 'owned_agent': owned_agent.OwnedAgent(environ),
            'firebase_test_lab': firebase_test_lab.FirebaseTestLab(environ), 'ssh_host': ssh_host.SshHost(environ)}
