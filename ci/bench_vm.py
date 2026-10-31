#!/usr/bin/env python3
"""ci/bench_vm.py: the benchmark machine of the optimization track (SIMD-PERF, user order 2026-10-31).

Usage: python ci/bench_vm.py [--name MACHINE] <command> [options]

  create                 make the machine (once), then leave it stopped
  start | stop | status  power it on (and wait for SSH) | power it off | its state and this run's hours
  sync [--ref REF]       put REF (default HEAD) on the machine as a detached checkout of ~/cerid
  run [--detach NAME] -- COMMAND
                         run COMMAND in ~/cerid; with --detach it keeps running after this call returns, its output
                         in ~/jobs/NAME.log and its exit status in ~/jobs/NAME.exit
  wait NAME [--timeout S]
                         wait up to S seconds (default 540) for a detached command; prints its log tail and exits
                         with its status, or 124 while it still runs (call wait again)
  fetch REMOTE LOCAL     copy a file or directory from the machine (paths under ~ are relative to it)

--name picks the machine (default cerid-bench, the optimization track's; each development track has its own, such as
cerid-track-b). One spot c4-standard-8 (Intel Emerald Rapids, AVX-512) in the cerid-ci project, booted from the CI's Linux image
family, with the guest's hardware performance counters on (the standard PMU: perf stat, top-down
analysis and sampling profiles see the core counters), with no service account (it holds no Google credential) and reached only through IAP (firewall rule
cerid-bench-iap, tag cerid-bench). The code reaches it as git bundles over that tunnel, never with a token: the first
sync clones the public GitHub repository and every sync sends only the commits the machine lacks. It runs only while
the track needs it: `stop` after every batch, and the instance stops itself after MAX_RUN_HOURS of one run (a spot
preemption stops it too; its disk stays). Never compare its numbers with another machine's boards.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
PROJECT = 'cerid-ci'
ZONE = 'us-central1-a'
NAME = 'cerid-bench'
MACHINE = 'c4-standard-8'  # the smallest Intel shape with a guest PMU (c3-standard-8 has none)
PMU = 'standard'  # core counters; 'enhanced' needs a larger shape
DISK_TYPE = 'hyperdisk-balanced'  # c4 takes no Persistent Disk
IMAGE_FAMILY = 'cerid-ci-ubuntu-2404-x64'
DISK_GB = 200
TAG = 'cerid-bench'
MAX_RUN_HOURS = 10
PUBLIC_REPOSITORY = 'https://github.com/yatiyr/CRD.git'
CHECKOUT = 'cerid'
JOBS = 'jobs'
SYNC_REF = 'refs/bench/sync'  # a bundle carries refs, not bare commits


class BenchVmError(RuntimeError):
    pass


def gcloud(which=shutil.which):
    for name in ('gcloud', 'gcloud.cmd'):
        found = which(name)
        if found:
            return found
    raise BenchVmError('gcloud is not installed or not on PATH')


def where():
    return ['--project', PROJECT, '--zone', ZONE]


def create_command():
    return ['compute', 'instances', 'create', NAME, *where(), '--machine-type', MACHINE,
            '--provisioning-model', 'SPOT', '--instance-termination-action', 'STOP',
            '--max-run-duration', '%dh' % MAX_RUN_HOURS, '--image-family', IMAGE_FAMILY, '--image-project', PROJECT,
            '--boot-disk-size', '%dGB' % DISK_GB, '--boot-disk-type', DISK_TYPE, '--tags', TAG,
            '--no-service-account', '--no-scopes', '--network-interface', 'nic-type=GVNIC',
            '--labels', 'cerid-track=' + NAME, '--performance-monitoring-unit', PMU]


def ssh_command(remote):
    return ['compute', 'ssh', NAME, *where(), '--tunnel-through-iap', '--quiet', '--command', remote]


def detach_script(name, command):
    """The remote shell text that runs `command` in the checkout under nohup, its output and exit status in files."""
    log, status = '~/%s/%s.log' % (JOBS, name), '~/%s/%s.exit' % (JOBS, name)
    # A subshell, so an `exit` in the command still leaves its status behind.
    inner = 'cd ~/%s && ( %s ); echo $? > %s' % (CHECKOUT, command, status)
    return 'mkdir -p ~/%s && rm -f %s && nohup bash -lc %s > %s 2>&1 < /dev/null &' % (
        JOBS, status, shlex.quote(inner), log)


def wait_script(name, tail=40):
    log, status = '~/%s/%s.log' % (JOBS, name), '~/%s/%s.exit' % (JOBS, name)
    return 'tail -n %d %s 2>/dev/null; if [ -f %s ]; then echo "@@exit $(cat %s)"; else echo "@@running"; fi' % (
        tail, log, status, status)


def parse_wait(output):
    """(finished, exit status or None, the log text without the marker line)."""
    lines = output.rstrip('\n').split('\n')
    marker = lines[-1] if lines else ''
    text = '\n'.join(lines[:-1])
    if marker.startswith('@@exit '):
        return True, int(marker.split()[1]), text
    return False, None, text


def sync_script(bundle, ref_commit, base):
    """Remote text that brings ~/cerid to `ref_commit` from an uploaded bundle (cloning the public repository first
    when the checkout does not exist yet)."""
    clone = '[ -d ~/%s/.git ] || git clone --quiet %s ~/%s' % (CHECKOUT, PUBLIC_REPOSITORY, CHECKOUT)
    fetch = ('git -C ~/%s fetch --quiet %s %s:%s' % (CHECKOUT, bundle, SYNC_REF, SYNC_REF)
             if base != ref_commit else 'true')
    return ' && '.join([clone, fetch, 'git -C ~/%s checkout --quiet --detach %s' % (CHECKOUT, ref_commit),
                        'rm -f %s' % bundle, 'git -C ~/%s log --oneline -1' % CHECKOUT])


class BenchVm:
    def __init__(self, runner=subprocess.run, which=shutil.which, sleep=time.sleep, clock=time.time):
        self.runner = runner
        self.which = which
        self.sleep = sleep
        self.clock = clock

    def gcloud(self, args, capture=True, check=True):
        result = self.runner([gcloud(self.which)] + list(args), capture_output=capture, text=True)
        if check and result.returncode != 0:
            raise BenchVmError('gcloud %s failed (%d): %s' % (' '.join(args[:3]), result.returncode,
                                                              (result.stderr or '').strip()[-800:]))
        return result

    def describe(self):
        result = self.gcloud(['compute', 'instances', 'describe', NAME, *where(), '--format', 'json'], check=False)
        if result.returncode != 0:
            return None
        return json.loads(result.stdout)

    def status(self):
        instance = self.describe()
        if instance is None:
            return {'exists': False}
        return {'exists': True, 'status': instance['status'], 'machine': instance['machineType'].rsplit('/', 1)[-1],
                'last_start': instance.get('lastStartTimestamp'), 'last_stop': instance.get('lastStopTimestamp')}

    def create(self):
        if self.describe() is not None:
            return 'exists'
        self.gcloud(create_command())
        self.wait_ssh()
        self.gcloud(['compute', 'instances', 'stop', NAME, *where(), '--quiet'])
        return 'created and stopped'

    def start(self):
        state = self.status()
        if not state['exists']:
            raise BenchVmError('%s does not exist: run `python ci/bench_vm.py create` first' % NAME)
        if state['status'] != 'RUNNING':
            self.gcloud(['compute', 'instances', 'start', NAME, *where(), '--quiet'])
        self.wait_ssh()
        return 'running'

    def stop(self):
        state = self.status()
        if state.get('exists') and state['status'] not in ('TERMINATED', 'STOPPED', 'STOPPING'):
            self.gcloud(['compute', 'instances', 'stop', NAME, *where(), '--quiet'])
        return 'stopped'

    def wait_ssh(self, timeout=300):
        deadline = self.clock() + timeout
        while True:
            if self.gcloud(ssh_command('true'), check=False).returncode == 0:
                return
            if self.clock() > deadline:
                raise BenchVmError('%s did not answer SSH through IAP in %d s' % (NAME, timeout))
            self.sleep(10)

    def run(self, command, detach=None):
        if detach:
            self.gcloud(ssh_command(detach_script(detach, command)))
            return 0
        return self.gcloud(ssh_command('cd ~/%s && %s' % (CHECKOUT, command)), capture=False, check=False).returncode

    def wait(self, name, timeout=540):
        deadline = self.clock() + timeout
        while True:
            finished, code, text = parse_wait(self.gcloud(ssh_command(wait_script(name))).stdout)
            if finished or self.clock() > deadline:
                print(text)
                return code if finished else 124
            self.sleep(30)

    def sync(self, ref='HEAD', git='git'):
        commit = subprocess.run([git, 'rev-parse', '%s^{commit}' % ref], capture_output=True, text=True,
                                check=True, cwd=ROOT).stdout.strip()
        result = self.gcloud(ssh_command('git -C ~/%s rev-parse HEAD 2>/dev/null || true' % CHECKOUT))
        base = result.stdout.strip() or None
        if base and subprocess.run([git, 'cat-file', '-e', base + '^{commit}'], cwd=ROOT).returncode != 0:
            base = None
        if base is None:
            # A new checkout clones the public repository; the bundle then carries what it lacks from there.
            base = self.public_head(git)
        with tempfile.TemporaryDirectory() as scratch:
            bundle = Path(scratch) / 'sync.bundle'
            if base != commit:
                subprocess.run([git, 'update-ref', SYNC_REF, commit], check=True, cwd=ROOT)
                try:
                    subprocess.run([git, 'bundle', 'create', str(bundle), SYNC_REF, '--not', base], check=True,
                                   cwd=ROOT, capture_output=True)
                finally:
                    subprocess.run([git, 'update-ref', '-d', SYNC_REF], cwd=ROOT)
                self.gcloud(['compute', 'scp', str(bundle), '%s:sync.bundle' % NAME, *where(),
                             '--tunnel-through-iap', '--quiet'])
            out = self.gcloud(ssh_command(sync_script('~/sync.bundle', commit, base))).stdout.strip()
        return out

    def public_head(self, git='git'):
        heads = subprocess.run([git, 'ls-remote', PUBLIC_REPOSITORY, 'refs/heads/main'], capture_output=True,
                               text=True, check=True).stdout.split()
        if not heads or subprocess.run([git, 'cat-file', '-e', heads[0] + '^{commit}'], cwd=ROOT).returncode != 0:
            raise BenchVmError('the public repository\'s main is unknown here: fetch it first')
        return heads[0]

    def fetch(self, remote, local):
        Path(local).mkdir(parents=True, exist_ok=True)
        self.gcloud(['compute', 'scp', '--recurse', '%s:%s' % (NAME, remote), str(local), *where(),
                     '--tunnel-through-iap', '--quiet'])
        return str(local)


def main(argv=None):
    global NAME
    parser = argparse.ArgumentParser(prog='bench_vm.py', description=__doc__.splitlines()[0])
    parser.add_argument('--name', default=NAME, help='the machine (default %(default)s)')
    sub = parser.add_subparsers(dest='command', required=True)
    for name in ('create', 'start', 'stop', 'status'):
        sub.add_parser(name)
    sync = sub.add_parser('sync')
    sync.add_argument('--ref', default='HEAD')
    run = sub.add_parser('run')
    run.add_argument('--detach')
    run.add_argument('remote', nargs=argparse.REMAINDER)
    wait = sub.add_parser('wait')
    wait.add_argument('name')
    wait.add_argument('--timeout', type=int, default=540)
    fetch = sub.add_parser('fetch')
    fetch.add_argument('remote')
    fetch.add_argument('local')
    args = parser.parse_args(argv)
    if not args.name.startswith(('cerid-bench', 'cerid-track-')):
        parser.error('--name must start with cerid-bench or cerid-track- (the firewall tag and the CI sweep rely on it)')
    NAME = args.name
    vm = BenchVm()
    try:
        if args.command == 'status':
            print(json.dumps(vm.status(), indent=1))
        elif args.command in ('create', 'start', 'stop'):
            print(getattr(vm, args.command)())
        elif args.command == 'sync':
            print(vm.sync(args.ref))
        elif args.command == 'run':
            remote = args.remote[1:] if args.remote[:1] == ['--'] else args.remote
            if not remote:
                parser.error('run needs a command after --')
            return vm.run(' '.join(remote), detach=args.detach)
        elif args.command == 'wait':
            return vm.wait(args.name, args.timeout)
        elif args.command == 'fetch':
            print(vm.fetch(args.remote, args.local))
    except BenchVmError as error:
        print('error: %s' % error, file=sys.stderr)
        return 2
    return 0


if __name__ == '__main__':
    sys.exit(main())
