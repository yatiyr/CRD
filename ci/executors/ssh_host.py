"""ssh_host: an existing host reached over SSH (AMD Developer Cloud, a rented Mac, a Windows ARM64 host).

The launcher connects with a key the operator stored (by name: a key file named by `CERID_SSH_<HOST>_KEY_FILE` or
the registry's Secret Manager secret), with strict host-key checking against a pinned known_hosts entry, optionally
powers the host on, and starts the pinned buildkite-agent there with the one-job acquisition token sent on standard
input, never on a command line. The job then runs natively on the host and streams its own log. Used only when the
host's secrets exist.
"""
from __future__ import annotations

from pathlib import Path
import os
import shlex
import subprocess
import tempfile

from . import agent_pin, agent_tags, os_family

POSIX_BOOTSTRAP = r'''read -r token
set -eu
dir="$HOME/.cerid-ci/agent-{version}"
mkdir -p "$dir" "$HOME/.cerid-ci/builds"
if [ ! -x "$dir/buildkite-agent" ]; then
  curl -sSLf -o "$dir/agent.tar.gz" '{url}'
  echo '{sha256}  '"$dir/agent.tar.gz" | (command -v sha256sum >/dev/null && sha256sum -c - || shasum -a 256 -c -)
  tar -xzf "$dir/agent.tar.gz" -C "$dir" '{member}'
fi
umask 077
printf '%s' "$token" > "$HOME/.cerid-ci/token-{job}"
nohup env BUILDKITE_AGENT_ACQUIRE_JOB='{job}' BUILDKITE_AGENT_TOKEN="file://$HOME/.cerid-ci/token-{job}" \
  BUILDKITE_BUILD_PATH="$HOME/.cerid-ci/builds" BUILDKITE_AGENT_TAGS='{tags}' \
  sh -c '"$0/buildkite-agent" start --reflect-exit-status; rm -f "$HOME/.cerid-ci/token-{job}"' "$dir" \
  > "$HOME/.cerid-ci/agent-{job}.log" 2>&1 &
'''

POWERSHELL_BOOTSTRAP = r'''$ErrorActionPreference = 'Stop'
$token = [Console]::In.ReadLine()
$dir = "$env:USERPROFILE\.cerid-ci\agent-{version}"
New-Item -ItemType Directory -Force -Path $dir, "$env:USERPROFILE\.cerid-ci\builds" | Out-Null
if (-not (Test-Path "$dir\buildkite-agent.exe")) {{
  Invoke-WebRequest -UseBasicParsing -Uri '{url}' -OutFile "$dir\agent.zip"
  if ((Get-FileHash -Algorithm SHA256 "$dir\agent.zip").Hash.ToLower() -ne '{sha256}') {{ throw 'agent checksum mismatch' }}
  Expand-Archive -Force -Path "$dir\agent.zip" -DestinationPath $dir
}}
$tokenFile = "$env:USERPROFILE\.cerid-ci\token-{job}"
$token | Out-File -Encoding ascii -NoNewline $tokenFile
$env:BUILDKITE_AGENT_ACQUIRE_JOB = '{job}'; $env:BUILDKITE_AGENT_TOKEN = "file://$tokenFile"
$env:BUILDKITE_BUILD_PATH = "$env:USERPROFILE\.cerid-ci\builds"; $env:BUILDKITE_AGENT_TAGS = '{tags}'
Start-Process -WindowStyle Hidden -FilePath "$dir\buildkite-agent.exe" -ArgumentList 'start', '--reflect-exit-status'
'''


def env_name(device):
    return device['executor_config']['host_env']


class SshHost:
    name = 'ssh_host'

    def __init__(self, environ, run=subprocess.run, pins=None, secret=None):
        self.environ = environ
        self.run = run
        self.pins = pins
        self.secret = secret

    def missing(self, device):
        host = env_name(device)
        missing = [] if self.environ.get(host) else [host]
        for suffix in ('_KEY_FILE', '_KNOWN_HOSTS'):
            name = host.replace('_HOST', suffix)
            if not self.environ.get(name):
                missing.append(name)
        return missing

    def bootstrap(self, job_id, device):
        name, pin = agent_pin(device, self.pins)
        values = {'version': pin['version'], 'url': pin['url'], 'sha256': pin['sha256'], 'member': pin['member'],
                  'job': job_id, 'tags': agent_tags(device)}
        template = POWERSHELL_BOOTSTRAP if os_family(device) == 'windows' else POSIX_BOOTSTRAP
        return template.format(**values)

    def command(self, device, key_file, known_hosts):
        host = self.environ[env_name(device)]
        shell = device['executor_config'].get('shell', 'bash')
        remote = 'powershell -NoProfile -NonInteractive -Command -' if shell == 'powershell' else 'sh -s'
        return ['ssh', '-i', str(key_file), '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=yes',
                '-o', 'UserKnownHostsFile=%s' % known_hosts, '-o', 'ConnectTimeout=30', host, remote]

    def launch(self, job_id, device, token):
        host_env = env_name(device)
        key_file = self.environ.get(host_env.replace('_HOST', '_KEY_FILE'))
        known = self.environ.get(host_env.replace('_HOST', '_KNOWN_HOSTS'))
        if not key_file or not known:
            raise RuntimeError('%s needs its key file and known_hosts named by %s and %s' % (
                device['id'], host_env.replace('_HOST', '_KEY_FILE'), host_env.replace('_HOST', '_KNOWN_HOSTS')))
        power_on = device['executor_config'].get('power_on_command_env')
        if power_on and self.environ.get(power_on):
            self.run(shlex.split(self.environ[power_on]), check=True)
        script = self.bootstrap(job_id, device)
        # `sh -s` reads the script from standard input line by line: its first line, `read -r token`, consumes the
        # line after it, so the token travels inside standard input and never appears in a process listing.
        first, rest = script.split('\n', 1)
        payload = first + '\n' + token + '\n' + rest
        if device['executor_config'].get('shell') == 'powershell':
            payload = script.replace('[Console]::In.ReadLine()', "'" + token.replace("'", "''") + "'")
        completed = self.run(self.command(device, key_file, known), input=payload, text=True, capture_output=True,
                             check=False)
        if completed.returncode != 0:
            raise RuntimeError('ssh to %s exited %d: %s' % (device['id'], completed.returncode,
                                                             (completed.stderr or '')[-400:]))
        return {'device': device['id'], 'job': job_id}

    def teardown(self, handle):
        return None
