"""gce_vm: one Google Compute Engine spot VM per job, deleted after it (ADR-0135 §4, amended by §12).

The coordinator creates the instance through the Compute Engine REST API with its own service-account credentials
(the metadata server on GCE; `gcloud auth print-access-token` elsewhere). The startup script downloads the Cerid
agent from the coordinator, refuses it unless its SHA-256 matches the digest passed in the instance metadata, runs it
as an unprivileged user in one-shot mode (the agent proves the VM's Google identity and takes the one job reserved
for this instance: the machine is given no credential) and deletes its own instance. Three independent bounds keep a
machine from outliving its job: the self-delete, the coordinator's reaper and the instance's `maxRunDuration` with
the DELETE termination action. Images come from per-device image families baked by `ci/ops.py bake`.
"""
from __future__ import annotations

import base64
import datetime as dt
import json
import os
import re
import subprocess
import urllib.error
import urllib.parse
import urllib.request

from . import os_family, tags_text

METADATA = 'http://metadata.google.internal/computeMetadata/v1'
COMPUTE = 'https://compute.googleapis.com/compute/v1'
SECRETS = 'https://secretmanager.googleapis.com/v1'
NAME = re.compile(r'[^a-z0-9-]')


# The provisioning scripts print these on serial port 1; an image is made only after the first and without the second.
BAKE_MARKER = 'cerid-ci-bake-complete'
BAKE_FAILED_MARKER = 'cerid-ci-bake-failed'
# The guest agent's line for a startup script that exited non-zero (Linux and Windows agents, old and new).
SCRIPT_FAILED = re.compile(r'Script "[\w-]*startup-script[\w-]*" failed with error|startup-script exit status [1-9]')


class BakeError(RuntimeError):
    pass


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
# Cerid CI one-job runner (ci/executors/gce_vm.py). Values come from instance metadata; the agent's digest from the
# coordinator that launched this machine. The machine holds no credential: its agent proves the VM's identity.
set -euo pipefail
md() { curl -sf -H 'Metadata-Flavor: Google' "http://metadata.google.internal/computeMetadata/v1/instance/$1"; }
name=$(md name); zone=$(md zone | sed 's#.*/##')
url=$(md attributes/cerid-agent-url); sha=$(md attributes/cerid-agent-sha256); coordinator=$(md attributes/cerid-coordinator)
finish() { gcloud compute instances delete "$name" --zone "$zone" --quiet || shutdown -h now; }
trap finish EXIT
id cerid >/dev/null 2>&1 || useradd --create-home --shell /bin/bash cerid
install -d -o cerid -g cerid /var/lib/cerid-ci /home/cerid/builds
curl -sSLf -o /var/lib/cerid-ci/cerid-agent.py "$url"
echo "$sha  /var/lib/cerid-ci/cerid-agent.py" | sha256sum -c -
sudo -u cerid -H python3 /var/lib/cerid-ci/cerid-agent.py start --mode one-shot --identity gce \
  --coordinator "$coordinator" --build-path /home/cerid/builds --name "$name" || true
'''

WINDOWS_STARTUP = r'''# Cerid CI one-job runner (ci/executors/gce_vm.py). Values come from instance metadata; the agent's digest from the
# coordinator that launched this machine. The machine holds no credential: its agent proves the VM's identity.
$ErrorActionPreference = 'Stop'
function Md($path) { Invoke-RestMethod -Headers @{'Metadata-Flavor'='Google'} -Uri "http://metadata.google.internal/computeMetadata/v1/instance/$path" }
$name = Md 'name'; $zone = (Md 'zone').Split('/')[-1]
try {
  $url = Md 'attributes/cerid-agent-url'; $sha = Md 'attributes/cerid-agent-sha256'; $coordinator = Md 'attributes/cerid-coordinator'
  $work = 'C:\cerid-ci'; New-Item -ItemType Directory -Force -Path $work, 'C:\b' | Out-Null
  Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile "$work\cerid-agent.py"
  if ((Get-FileHash -Algorithm SHA256 "$work\cerid-agent.py").Hash.ToLower() -ne $sha) { throw 'agent checksum mismatch' }
  $py = (Get-Command python -ErrorAction SilentlyContinue).Source
  if (-not $py) { $py = 'C:\Program Files\Python312\python.exe' }
  & $py "$work\cerid-agent.py" start --mode one-shot --identity gce --coordinator $coordinator --build-path 'C:\b' --name $name
} finally {
  & gcloud compute instances delete $name --zone $zone --quiet
}
'''


class GceVm:
    name = 'gce_vm'

    def __init__(self, environ, http=http_request, token=access_token):
        self.environ = environ
        self.http = http
        self.token = token

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

    def body(self, machine, device, agent):
        """The instances.insert request for one job (pure: tests read it without a cloud). `agent` carries the
        coordinator's URL and the agent's URL and SHA-256; it never carries a token for a VM."""
        if agent.get('token'):
            raise ValueError('a GCE machine proves its identity; it must never be given a job token')
        config = device['executor_config']
        zone = config.get('zone', self.zone)
        startup_key, startup = self.startup(device)
        scheduling = {'automaticRestart': False, 'onHostMaintenance': 'TERMINATE',
                      'maxRunDuration': {'seconds': int(config['max_run_hours'] * 3600)},
                      'instanceTerminationAction': 'DELETE'}
        if config.get('spot', True):
            scheduling['provisioningModel'] = 'SPOT'
        body = {
            'name': instance_name(machine),
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
                {'key': 'cerid-machine', 'value': instance_name(machine)},
                {'key': 'cerid-coordinator', 'value': agent['coordinator']},
                {'key': 'cerid-agent-url', 'value': agent['agent_url']},
                {'key': 'cerid-agent-sha256', 'value': agent['agent_sha256']},
                {'key': 'cerid-agent-tags', 'value': tags_text(device)},
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

    def launch(self, machine, device, agent):
        zone, body = self.body(machine, device, agent)
        self.http('POST', '%s/projects/%s/zones/%s/instances' % (COMPUTE, self.project, zone), token=self.token(),
                  body=body)
        return {'project': self.project, 'zone': zone, 'instance': body['name']}

    def alive(self, handle):
        """False once the instance is gone or stopped (a preempted spot VM is deleted by its termination action);
        True while it exists and runs."""
        status, payload = self.http('GET', '%s/projects/%s/zones/%s/instances/%s' % (
            COMPUTE, handle['project'], handle['zone'], handle['instance']), token=self.token(), allow=(404,))
        if status == 404:
            return False
        return payload.get('status') not in ('STOPPING', 'STOPPED', 'TERMINATED', 'SUSPENDING', 'SUSPENDED')

    def machines(self):
        """Every job machine of the project (label cerid-ci=job), for the orphan sweep."""
        out = []
        page = None
        while True:
            query = {'filter': 'labels.cerid-ci=job', 'maxResults': 500}
            if page:
                query['pageToken'] = page
            _, listing = self.http('GET', '%s/projects/%s/aggregated/instances?%s' % (
                COMPUTE, self.project, urllib.parse.urlencode(query)), token=self.token())
            for zone_path, scoped in (listing.get('items') or {}).items():
                for item in scoped.get('instances', []):
                    created = item.get('creationTimestamp', '')
                    try:
                        stamp = dt.datetime.fromisoformat(created).timestamp()
                    except ValueError:
                        stamp = 0.0
                    zone = zone_path.rsplit('/', 1)[-1]
                    out.append({'name': item['name'], 'created': stamp, 'handle': {
                        'project': self.project, 'zone': zone, 'instance': item['name']}})
            page = listing.get('nextPageToken')
            if not page:
                return out

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

    def serial_since(self, zone, instance, start):
        """(text, next offset): serial port 1 of a running instance from `start`, every page; the guest agent logs
        each startup-script line there. Compute Engine answers 400 for a stopped instance, so the bake reads it
        while the instance runs and keeps its own copy."""
        url = '%s/projects/%s/zones/%s/instances/%s/serialPort?port=1' % (COMPUTE, self.project, zone, instance)
        text = ''
        while True:
            _, page = self.http('GET', '%s&start=%d' % (url, start), token=self.token())
            text += page.get('contents', '')
            following = int(page.get('next', start))
            if following <= start or not page.get('contents'):
                return text, start
            start = following

    def bake(self, device, provision, stamp, log, sleep, clock, hours=4):
        """Provision a one-off instance, image its disk and delete it; the image record, or BakeError.

        The image is made only after the provisioning script printed BAKE_MARKER (a disk whose provisioning failed
        must never become an image). The serial console that carries it is readable only while the instance runs,
        so it is read on every poll; the script pauses after the marker for longer than a poll. A failure (the
        script's BAKE_FAILED_MARKER or the guest agent's failed-script line) stops the instance at once instead of
        leaving it idle until maxRunDuration. The instance and its auto-deleted disk are removed only after the image
        operation is DONE without an error: deleting earlier removes the image's source disk and the insert fails
        with RESOURCE_NOT_FOUND."""
        zone, body = self.bake_body(device, provision)
        base = '%s/projects/%s/zones/%s/instances' % (COMPUTE, self.project, zone)
        instance = '%s/%s' % (base, body['name'])
        self.http('POST', base, token=self.token(), body=body)
        log('baking %s on %s; waiting for it to power off (up to %d hours)' % (device['id'], body['name'], hours))
        deadline = clock() + hours * 3600
        serial, offset = '', 0
        while True:
            _, state = self.http('GET', instance, token=self.token())
            if state.get('status') == 'RUNNING':
                try:
                    text, offset = self.serial_since(zone, body['name'], offset)
                    serial += text
                except HttpError as error:
                    if error.status != 400:  # 400: it stopped between the two reads; the next poll sees that
                        raise
            failed = BAKE_FAILED_MARKER in serial or SCRIPT_FAILED.search(serial)
            if failed:
                self.http('POST', instance + '/stop', token=self.token(), allow=(404,))
                line = next((x for x in serial.splitlines() if BAKE_FAILED_MARKER in x or SCRIPT_FAILED.search(x)))
                raise BakeError('provisioning failed on %s (%s); it is stopped and stays for inspection'
                                % (body['name'], line.strip()[:300]))
            if state.get('status') == 'TERMINATED':
                break
            if clock() >= deadline:
                raise BakeError('the bake did not finish in %d hours; instance %s stays for inspection'
                                % (hours, body['name']))
            sleep(30)
        if BAKE_MARKER not in serial:
            raise BakeError('instance %s stopped without printing %s while it ran; it stays for inspection'
                            % (body['name'], BAKE_MARKER))
        image = self.image_body(device, zone, body['name'], stamp)
        _, operation = self.http('POST', '%s/projects/%s/global/images' % (COMPUTE, self.project),
                                 token=self.token(), body=image)
        operation = self.wait_global_operation(operation, sleep, clock, deadline=clock() + 3600)
        if operation.get('error'):
            raise BakeError('creating image %s failed (%s); instance %s stays for inspection' % (
                image['name'], json.dumps(operation['error'])[:300], body['name']))
        self.http('DELETE', '%s/%s' % (base, body['name']), token=self.token(), allow=(404,))
        return image

    def wait_global_operation(self, operation, sleep, clock, deadline):
        url = '%s/projects/%s/global/operations/%s' % (COMPUTE, self.project, operation['name'])
        while operation.get('status') != 'DONE':
            if clock() >= deadline:
                raise BakeError('operation %s did not finish in time' % operation['name'])
            sleep(10)
            _, operation = self.http('GET', url, token=self.token())
        return operation


PROVISION = {
    'linux': r'''#!/bin/bash
# Cerid CI image bake (ci/ops.py bake): the toolchain the hosted ubuntu-24.04 image provided, recorded in the census.
# The failure trap is set before tracing starts: a traced definition would print the failure marker on every run.
trap 'status=$?; echo "cerid-ci-bake-failed: line $LINENO exited $status"; shutdown -h now' ERR
set -Eeuxo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -q
apt-get install -y ca-certificates curl git python3 python-is-python3 python3-venv python3-pip build-essential gcc-14 g++-14 clang \
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
echo cerid-ci-bake-complete
sleep 90  # the bake reads the serial console every 30 seconds, and only while the instance runs
shutdown -h now
''',
    'windows': r'''# Cerid CI image bake (ci/ops.py bake): the toolchain the hosted windows-2025-vs2026 image provided.
$ErrorActionPreference = 'Stop'
winget install --silent --accept-package-agreements --accept-source-agreements --scope machine --id Python.Python.3.12
winget install --silent --accept-package-agreements --accept-source-agreements --id Git.Git
winget install --silent --accept-package-agreements --accept-source-agreements --id Kitware.CMake
winget install --silent --accept-package-agreements --accept-source-agreements --id Microsoft.VisualStudio.2026.BuildTools `
  --override '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --includeRecommended'
{extra}
New-Item -ItemType Directory -Force -Path 'C:\cerid-ci' | Out-Null
(Get-ComputerInfo | Select-Object OsName, OsVersion | Out-String) + (winget list | Out-String) | Out-File 'C:\cerid-ci\image-census.txt'
Write-Output 'cerid-ci-bake-complete'
& "$env:SystemRoot\System32\Sysprep\sysprep.exe" /generalize /oobe /shutdown /quiet
if ($LASTEXITCODE -ne 0) { Write-Output "cerid-ci-bake-failed: sysprep exited $LASTEXITCODE" }
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
