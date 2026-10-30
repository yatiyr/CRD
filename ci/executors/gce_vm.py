"""gce_vm: one Google Compute Engine spot VM per job, deleted after it (ADR-0135 §4).

The launcher creates the instance through the Compute Engine REST API with its own service-account credentials (the
metadata server on GCE; `gcloud auth print-access-token` elsewhere). The startup script downloads the pinned
buildkite-agent, verifies its SHA-256 from cmake/pins.json (passed as metadata at launch; no digest lives in this
file), runs it with the one-job acquisition token and deletes its own instance. Three independent bounds keep a
machine from outliving its job: the self-delete, the launcher's reaper and the instance's `maxRunDuration` with the
DELETE termination action. Images come from per-device image families baked by `ci/ops.py bake`.
"""
from __future__ import annotations

import base64
import json
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request

from . import agent_pin, agent_tags, os_family

METADATA = 'http://metadata.google.internal/computeMetadata/v1'
COMPUTE = 'https://compute.googleapis.com/compute/v1'
SECRETS = 'https://secretmanager.googleapis.com/v1'
NAME = re.compile(r'[^a-z0-9-]')


class HttpError(RuntimeError):
    def __init__(self, status, body):
        super().__init__('HTTP %s: %s' % (status, body[:400]))
        self.status = status


def http_request(method, url, token=None, scheme='Bearer', body=None, data=None, raw=False, allow=(),
                 content_type=None, timeout=60, headers=None, with_headers=False):
    """(status, payload): JSON in and out unless `raw`; a status outside 2xx and `allow` raises HttpError. With
    `with_headers` the response headers (lower-case names) come third: Cloud Storage returns an object's generation
    there, which the coordinator's compare-and-swap needs."""
    sent = dict(headers or {})
    if token:
        sent['Authorization'] = '%s %s' % (scheme, token)
    if body is not None:
        data = json.dumps(body).encode('utf-8')
        sent['Content-Type'] = 'application/json'
    elif content_type:
        sent['Content-Type'] = content_type
    request = urllib.request.Request(url, data=data, method=method, headers=sent)
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            status, payload = response.status, response.read()
            received = {k.lower(): v for k, v in response.headers.items()}
    except urllib.error.HTTPError as error:
        status, payload = error.code, error.read()
        received = {k.lower(): v for k, v in (error.headers or {}).items()}
        error.close()
        if status not in allow:
            raise HttpError(status, payload.decode('utf-8', errors='replace')) from None
    if not raw:
        payload = json.loads(payload.decode('utf-8')) if payload.strip() else {}
    return (status, payload, received) if with_headers else (status, payload)


def access_token(http=None):
    """An OAuth access token for the machine's service account, never written anywhere."""
    http = http or urllib.request.urlopen
    request = urllib.request.Request(METADATA + '/instance/service-accounts/default/token',
                                     headers={'Metadata-Flavor': 'Google'})
    try:
        with http(request, timeout=5) as response:
            return json.loads(response.read().decode('utf-8'))['access_token']
    except (OSError, ValueError, KeyError):
        completed = subprocess.run(['gcloud', 'auth', 'print-access-token'], capture_output=True, text=True,
                                   check=False)
        if completed.returncode != 0:
            raise RuntimeError('no Google Cloud credentials: run on the launcher VM or `gcloud auth login`')
        return completed.stdout.strip()


def secret_names(project, http=http_request, token=access_token):
    """Secret Manager secret names of the project (names only; values are never listed)."""
    _, listing = http('GET', '%s/projects/%s/secrets?fields=secrets.name' % (SECRETS, project), token=token())
    return sorted(item['name'].rsplit('/', 1)[-1] for item in listing.get('secrets', []))


def secret_value(project, name, http=http_request, token=access_token):
    _, payload = http('GET', '%s/projects/%s/secrets/%s/versions/latest:access' % (SECRETS, project, name),
                      token=token())
    return base64.b64decode(payload['payload']['data']).decode('utf-8')


def instance_name(job_id):
    return NAME.sub('-', ('cerid-ci-' + job_id.lower()))[:62].rstrip('-')


LINUX_STARTUP = r'''#!/bin/bash
# Cerid CI one-job runner (ci/executors/gce_vm.py). Values come from instance metadata; the agent digest from pins.
set -euo pipefail
md() { curl -sf -H 'Metadata-Flavor: Google' "http://metadata.google.internal/computeMetadata/v1/instance/$1"; }
name=$(md name); zone=$(md zone | sed 's#.*/##')
job=$(md attributes/cerid-job); url=$(md attributes/cerid-agent-url); sha=$(md attributes/cerid-agent-sha256)
member=$(md attributes/cerid-agent-member); tags=$(md attributes/cerid-agent-tags)
finish() { gcloud compute instances delete "$name" --zone "$zone" --quiet || shutdown -h now; }
trap finish EXIT
id cerid >/dev/null 2>&1 || useradd --create-home --shell /bin/bash cerid
install -d -o cerid -g cerid /var/lib/cerid-ci /home/cerid/builds
cd /var/lib/cerid-ci
curl -sSLf -o agent.tar.gz "$url"
echo "$sha  agent.tar.gz" | sha256sum -c -
tar -xzf agent.tar.gz "$member"
install -m 0755 "$member" /usr/local/bin/buildkite-agent
md attributes/cerid-agent-token > token && chown cerid token && chmod 0400 token
sudo -u cerid -H env BUILDKITE_AGENT_ACQUIRE_JOB="$job" BUILDKITE_AGENT_TOKEN="file:///var/lib/cerid-ci/token" \
  BUILDKITE_BUILD_PATH=/home/cerid/builds BUILDKITE_AGENT_TAGS="$tags" BUILDKITE_AGENT_NAME="$name" \
  /usr/local/bin/buildkite-agent start --reflect-exit-status || true
rm -f token
'''

WINDOWS_STARTUP = r'''# Cerid CI one-job runner (ci/executors/gce_vm.py). Values come from instance metadata; the agent digest from pins.
$ErrorActionPreference = 'Stop'
function Md($path) { Invoke-RestMethod -Headers @{'Metadata-Flavor'='Google'} -Uri "http://metadata.google.internal/computeMetadata/v1/instance/$path" }
$name = Md 'name'; $zone = (Md 'zone').Split('/')[-1]
try {
  $job = Md 'attributes/cerid-job'; $url = Md 'attributes/cerid-agent-url'; $sha = Md 'attributes/cerid-agent-sha256'
  $member = Md 'attributes/cerid-agent-member'; $tags = Md 'attributes/cerid-agent-tags'
  $work = 'C:\cerid-ci'; New-Item -ItemType Directory -Force -Path $work, 'C:\b' | Out-Null
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$work\agent.zip"
  if ((Get-FileHash -Algorithm SHA256 "$work\agent.zip").Hash.ToLower() -ne $sha) { throw 'agent checksum mismatch' }
  Expand-Archive -Force -Path "$work\agent.zip" -DestinationPath "$work\agent"
  (Md 'attributes/cerid-agent-token') | Out-File -Encoding ascii -NoNewline "$work\token"
  $env:BUILDKITE_AGENT_ACQUIRE_JOB = $job; $env:BUILDKITE_AGENT_TOKEN = "file://$work/token"
  $env:BUILDKITE_BUILD_PATH = 'C:\b'; $env:BUILDKITE_AGENT_TAGS = $tags; $env:BUILDKITE_AGENT_NAME = $name
  & "$work\agent\$member" start --reflect-exit-status
} finally {
  Remove-Item -Force -ErrorAction SilentlyContinue 'C:\cerid-ci\token'
  & gcloud compute instances delete $name --zone $zone --quiet
}
'''


class GceVm:
    name = 'gce_vm'

    def __init__(self, environ, http=http_request, token=access_token, pins=None):
        self.environ = environ
        self.http = http
        self.token = token
        self.pins = pins

    @property
    def project(self):
        return self.environ.get('CERID_CI_GCP_PROJECT', 'cerid-ci')

    @property
    def zone(self):
        return self.environ.get('CERID_CI_GCP_ZONE', 'us-central1-a')

    def missing(self, device):
        return [name for name in ('CERID_CI_GCP_PROJECT', 'CERID_CI_RUNNER_SERVICE_ACCOUNT') if not self.environ.get(name)]

    def startup(self, device):
        family = os_family(device)
        return ('windows-startup-script-ps1', WINDOWS_STARTUP) if family == 'windows' else ('startup-script',
                                                                                           LINUX_STARTUP)

    def body(self, job_id, device, token):
        """The instances.insert request for one job (pure: tests read it without a cloud)."""
        config = device['executor_config']
        zone = config.get('zone', self.zone)
        pin_name, pin = agent_pin(device, self.pins)
        member = pin['member']
        startup_key, startup = self.startup(device)
        scheduling = {'automaticRestart': False, 'onHostMaintenance': 'TERMINATE',
                      'maxRunDuration': {'seconds': int(config['max_run_hours'] * 3600)},
                      'instanceTerminationAction': 'DELETE'}
        if config.get('spot', True):
            scheduling['provisioningModel'] = 'SPOT'
        body = {
            'name': instance_name(job_id),
            'machineType': 'zones/%s/machineTypes/%s' % (zone, config['machine_type']),
            'scheduling': scheduling,
            'disks': [{'boot': True, 'autoDelete': True, 'initializeParams': {
                'sourceImage': 'projects/%s/global/images/family/%s' % (self.project, config['image_family']),
                'diskSizeGb': str(config['disk_gb']), 'diskType': 'zones/%s/diskTypes/pd-balanced' % zone}}],
            'networkInterfaces': [{'network': 'global/networks/default',
                                   'accessConfigs': [{'type': 'ONE_TO_ONE_NAT', 'name': 'External NAT'}]}],
            'serviceAccounts': [{'email': self.environ.get('CERID_CI_RUNNER_SERVICE_ACCOUNT', ''),
                                 'scopes': ['https://www.googleapis.com/auth/cloud-platform']}],
            'labels': {'cerid-ci': 'job', 'cerid-device': device['id'][:63]},
            'metadata': {'items': [
                {'key': 'cerid-job', 'value': job_id},
                {'key': 'cerid-agent-url', 'value': pin['url']},
                {'key': 'cerid-agent-sha256', 'value': pin['sha256']},
                {'key': 'cerid-agent-member', 'value': member},
                {'key': 'cerid-agent-pin', 'value': pin_name},
                {'key': 'cerid-agent-tags', 'value': agent_tags(device)},
                {'key': 'cerid-agent-token', 'value': token},
                {'key': startup_key, 'value': startup}]},
        }
        if config.get('min_cpu_platform'):
            body['minCpuPlatform'] = config['min_cpu_platform']
        accelerator = config.get('accelerator')
        if accelerator:
            body['guestAccelerators'] = [{'acceleratorType': 'zones/%s/acceleratorTypes/%s' % (zone, accelerator['type']),
                                          'acceleratorCount': accelerator['count']}]
        features = {}
        if config.get('nested_virtualization'):
            features['enableNestedVirtualization'] = True
        if config.get('threads_per_core'):
            features['threadsPerCore'] = config['threads_per_core']
        if features:
            body['advancedMachineFeatures'] = features
        return zone, body

    def launch(self, job_id, device, token):
        zone, body = self.body(job_id, device, token)
        self.http('POST', '%s/projects/%s/zones/%s/instances' % (COMPUTE, self.project, zone), token=self.token(),
                  body=body)
        return {'project': self.project, 'zone': zone, 'instance': body['name']}

    def teardown(self, handle):
        self.http('DELETE', '%s/projects/%s/zones/%s/instances/%s' % (COMPUTE, handle['project'], handle['zone'],
                                                                     handle['instance']),
                  token=self.token(), allow=(404,))

    # Image baking ---------------------------------------------------------------------------------------------
    def bake_body(self, device, provision):
        """A one-off instance from the base image that runs the device's provisioning and powers off; ops.py then
        creates the image family member from its disk and deletes it."""
        config = device['executor_config']
        zone = config.get('zone', self.zone)
        key = 'windows-startup-script-ps1' if os_family(device) == 'windows' else 'startup-script'
        body = {
            'name': instance_name('bake-' + device['id']),
            'machineType': 'zones/%s/machineTypes/%s' % (zone, config['machine_type']),
            'scheduling': {'automaticRestart': False, 'onHostMaintenance': 'TERMINATE',
                           'maxRunDuration': {'seconds': 4 * 3600}, 'instanceTerminationAction': 'STOP'},
            'disks': [{'boot': True, 'autoDelete': True, 'initializeParams': {
                'sourceImage': config['base_image'], 'diskSizeGb': str(config['disk_gb'])}}],
            'networkInterfaces': [{'network': 'global/networks/default',
                                   'accessConfigs': [{'type': 'ONE_TO_ONE_NAT', 'name': 'External NAT'}]}],
            'serviceAccounts': [{'email': self.environ.get('CERID_CI_RUNNER_SERVICE_ACCOUNT', ''),
                                 'scopes': ['https://www.googleapis.com/auth/cloud-platform']}],
            'labels': {'cerid-ci': 'bake', 'cerid-device': device['id'][:63]},
            'metadata': {'items': [{'key': key, 'value': provision}]},
        }
        accelerator = config.get('accelerator')
        if accelerator:
            body['guestAccelerators'] = [{'acceleratorType': 'zones/%s/acceleratorTypes/%s' % (zone, accelerator['type']),
                                          'acceleratorCount': accelerator['count']}]
        if config.get('nested_virtualization'):
            body['advancedMachineFeatures'] = {'enableNestedVirtualization': True}
        return zone, body

    def image_body(self, device, zone, instance, stamp):
        family = device['executor_config']['image_family']
        return {'name': '%s-%s' % (family, stamp), 'family': family,
                'sourceDisk': 'zones/%s/disks/%s' % (zone, instance), 'labels': {'cerid-ci': 'image'}}


PROVISION = {
    'linux': r'''#!/bin/bash
# Cerid CI image bake (ci/ops.py bake): the toolchain the hosted ubuntu-24.04 image provided, recorded in the census.
set -euxo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y ca-certificates curl git python3 python3-venv python3-pip build-essential gcc-14 g++-14 clang \
  clang-tools lld llvm ninja-build gdb rsync unzip xz-utils {packages}
curl -sSLf https://apt.kitware.com/kitware-archive.sh | bash
apt-get install -y cmake
{extra}
id cerid >/dev/null 2>&1 || useradd --create-home --shell /bin/bash cerid
echo 'cerid ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/cerid && chmod 0440 /etc/sudoers.d/cerid
mkdir -p /var/lib/cerid-ci
(lsb_release -ds; uname -r; cmake --version | head -1; gcc --version | head -1; clang --version | head -1;
 python3 --version; dpkg-query -W -f='${{Package}} ${{Version}}\n') > /var/lib/cerid-ci/image-census.txt
rm -f /etc/machine-id && touch /etc/machine-id
shutdown -h now
''',
    'windows': r'''# Cerid CI image bake (ci/ops.py bake): the toolchain the hosted windows-2025-vs2026 image provided.
$ErrorActionPreference = 'Stop'
winget install --silent --accept-package-agreements --accept-source-agreements --id Python.Python.3.12
winget install --silent --accept-package-agreements --accept-source-agreements --id Git.Git
winget install --silent --accept-package-agreements --accept-source-agreements --id Kitware.CMake
winget install --silent --accept-package-agreements --accept-source-agreements --id Microsoft.VisualStudio.2026.BuildTools `
  --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended'
{extra}
New-Item -ItemType Directory -Force -Path 'C:\cerid-ci' | Out-Null
(Get-ComputerInfo | Select-Object OsName, OsVersion | Out-String) + (winget list | Out-String) | Out-File 'C:\cerid-ci\image-census.txt'
& "$env:SystemRoot\System32\Sysprep\sysprep.exe" /generalize /oobe /shutdown /quiet
''',
}
PROVISION_EXTRA = {
    'nvidia-linux': 'ubuntu-drivers install --gpgpu\napt-get install -y nvidia-utils-570-server libnvidia-gl-570-server '
                    'nvidia-cuda-toolkit',
    'nvidia-windows': "Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/GoogleCloudPlatform/compute-gpu-"
                      "installation/raw/main/windows/install_gpu_driver.ps1' -OutFile C:\\install_gpu_driver.ps1\n"
                      "& C:\\install_gpu_driver.ps1",
    'android': 'apt-get install -y openjdk-17-jdk-headless qemu-kvm\nusermod -aG kvm cerid || true',
}


def provision_script(device, apt_packages):
    family = os_family(device)
    config = device['executor_config']
    extra = []
    if config.get('accelerator'):
        extra.append(PROVISION_EXTRA['nvidia-' + family])
    if config.get('nested_virtualization'):
        extra.append(PROVISION_EXTRA['android'])
    template = PROVISION[family]
    if family == 'windows':
        return template.replace('{extra}', '\n'.join(extra))
    return template.format(packages=' '.join(apt_packages), extra='\n'.join(extra))
