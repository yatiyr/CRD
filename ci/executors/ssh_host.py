"""ssh_host: an existing host reached over SSH (AMD Developer Cloud, a rented Mac, a Windows ARM64 host).

The coordinator connects with a key the operator stored (by name: a key file named by `CERID_SSH_<HOST>_KEY_FILE` or
the registry's Secret Manager secret), with strict host-key checking against a pinned known_hosts entry, optionally
powers the host on, and starts the Cerid agent there in one-shot mode. The agent is downloaded from the coordinator
and refused unless its SHA-256 matches; the one job token travels inside standard input (never on a command line)
into a file only that user reads, which the agent deletes as it starts. The job then runs natively on the host and
streams its own log. Used only when the host's secrets exist.
"""
from __future__ import annotations

import shlex
import subprocess

from . import os_family

POSIX_BOOTSTRAP = r'''read -r token
set -eu
dir="$HOME/.cerid-ci"
mkdir -p "$dir/builds"
umask 077
curl -sSLf -o "$dir/cerid-agent-{machine}.py" '{agent_url}'
echo '{sha256}  '"$dir/cerid-agent-{machine}.py" | (command -v sha256sum >/dev/null && sha256sum -c - || shasum -a 256 -c -)
printf '%s' "$token" > "$dir/token-{machine}"
nohup python3 "$dir/cerid-agent-{machine}.py" start --mode one-shot --token-file "$dir/token-{machine}" \
  --coordinator '{coordinator}' --build-path "$dir/builds" --name '{machine}' > "$dir/agent-{machine}.log" 2>&1 &
'''

POWERSHELL_BOOTSTRAP = r'''$ErrorActionPreference = 'Stop'
$token = [Console]::In.ReadLine()
$dir = "$env:USERPROFILE\.cerid-ci"
New-Item -ItemType Directory -Force -Path "$dir\builds" | Out-Null
$agent = "$dir\cerid-agent-{machine}.py"
Invoke-WebRequest -UseBasicParsing -Uri '{agent_url}' -OutFile $agent
if ((Get-FileHash -Algorithm SHA256 $agent).Hash.ToLower() -ne '{sha256}') {{ throw 'agent checksum mismatch' }}
$tokenFile = "$dir\token-{machine}"
$token | Out-File -Encoding ascii -NoNewline $tokenFile
icacls $tokenFile /inheritance:r /grant:r "$($env:USERNAME):(R,D)" | Out-Null
Start-Process -WindowStyle Hidden -FilePath 'py' -ArgumentList '-3.12', $agent, 'start', '--mode', 'one-shot', `
  '--token-file', $tokenFile, '--coordinator', '{coordinator}', '--build-path', "$dir\builds", '--name', '{machine}'
'''


def env_name(device):
    return device['executor_config']['host_env']


class SshHost:
    name = 'ssh_host'

    def __init__(self, environ, run=subprocess.run):
        self.environ = environ
        self.run = run

    def missing(self, device):
        host = env_name(device)
        missing = [] if self.environ.get(host) else [host]
        for suffix in ('_KEY_FILE', '_KNOWN_HOSTS'):
            name = host.replace('_HOST', suffix)
            if not self.environ.get(name):
                missing.append(name)
        return missing

    def bootstrap(self, machine, device, agent):
        """The script sent on standard input. An SSH host is outside the CI project's network: it reaches the
        coordinator by its external HTTPS URL."""
        base = agent.get('external') or agent['coordinator']
        values = {'machine': machine, 'agent_url': base + '/agent/cerid-agent.py', 'sha256': agent['agent_sha256'],
                  'coordinator': base}
        template = POWERSHELL_BOOTSTRAP if os_family(device) == 'windows' else POSIX_BOOTSTRAP
        return template.format(**values)

    def command(self, device, key_file, known_hosts):
        host = self.environ[env_name(device)]
        shell = device['executor_config'].get('shell', 'bash')
        remote = 'powershell -NoProfile -NonInteractive -Command -' if shell == 'powershell' else 'sh -s'
        return ['ssh', '-i', str(key_file), '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
                '-o', 'UserKnownHostsFile=%s' % known_hosts, '-o', 'ConnectTimeout=30', host, remote]

    def launch(self, machine, device, agent):
        host_env = env_name(device)
        key_file = self.environ.get(host_env.replace('_HOST', '_KEY_FILE'))
        known = self.environ.get(host_env.replace('_HOST', '_KNOWN_HOSTS'))
        if not key_file or not known:
            raise RuntimeError('%s needs its key file and known_hosts named by %s and %s' % (
                device['id'], host_env.replace('_HOST', '_KEY_FILE'), host_env.replace('_HOST', '_KNOWN_HOSTS')))
        if not agent.get('token'):
            raise RuntimeError('an SSH host needs the one job token of its launch')
        power_on = device['executor_config'].get('power_on_command_env')
        if power_on and self.environ.get(power_on):
            self.run(shlex.split(self.environ[power_on]), check=True)
        script = self.bootstrap(machine, device, agent)
        # `sh -s` reads the script from standard input line by line: its first line, `read -r token`, consumes the
        # line after it. PowerShell reads the whole script first, so the token is a literal inside it. Either way the
        # token travels inside standard input and never appears in a process listing.
        if device['executor_config'].get('shell') == 'powershell':
            payload = script.replace('[Console]::In.ReadLine()', "'" + agent['token'].replace("'", "''") + "'")
        else:
            first, rest = script.split('\n', 1)
            payload = first + '\n' + agent['token'] + '\n' + rest
        completed = self.run(self.command(device, key_file, known), input=payload, text=True, capture_output=True,
                             check=False)
        if completed.returncode != 0:
            raise RuntimeError('ssh to %s exited %d: %s' % (device['id'], completed.returncode,
                                                             (completed.stderr or '')[-400:]))
        return {'device': device['id'], 'machine': machine}

    def teardown(self, handle):
        return None
