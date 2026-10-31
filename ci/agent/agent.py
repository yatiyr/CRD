#!/usr/bin/env python3
"""The Cerid CI agent: one standard-library Python file for Windows, Linux and macOS on x64 and ARM64 (ADR-0135 §12).

  agent.py start --coordinator URL --mode long-running --registration-token-file FILE [--name N] [--tags k=v,...]
  agent.py start --coordinator URL --mode one-shot --identity gce      (a cloud VM: its Google identity, no secret)
  agent.py start --coordinator URL --mode one-shot --token-stdin       (an SSH host or a launcher job: one job token;
                                                                       or --token-file, deleted once read)

Inside a running step (the agent sets CERID_AGENT_ENDPOINT, CERID_AGENT_JOB_ID, CERID_AGENT_JOB_TOKEN and
CERID_AGENT_SCRIPT):

  agent.py pipeline upload [FILE|-]        add steps to the build
  agent.py annotate --context C --style S [--append] [--body-file F] < markdown
  agent.py artifact upload 'glob;glob'     agent.py artifact download PATH [DEST]
  agent.py meta-data set KEY VALUE         agent.py meta-data get KEY
  agent.py step outcome KEY

A job: check out the exact commit (`git fetch` of that commit, detached, cleaned), run the step's command in a
clean environment (an allow-list of the host's variables, the build's identity, the step's env and its declared
secrets), stream its output live in chunks with every secret value redacted (also across chunk boundaries), send
heartbeats, stop the whole process tree on cancellation, timeout or a preemption notice, upload the artifacts, report
the exit status and clean up. The agent never holds a long-lived secret: a cloud VM proves its Google identity, an
owned machine exchanges its registration token (a file only the agent's user reads) for a short-lived session, and
every job runs on its own job token, valid for that attempt only.
"""
from __future__ import annotations

from pathlib import Path
import argparse
import glob
import hashlib
import ipaddress
import json
import os
import platform
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib

VERSION = '1.0.0'
PROTOCOL = 1
METADATA = 'http://metadata.google.internal/computeMetadata/v1/instance/'
HOST_ENV = ('PATH', 'PATHEXT', 'SYSTEMROOT', 'SYSTEMDRIVE', 'WINDIR', 'COMSPEC', 'TEMP', 'TMP', 'TMPDIR', 'HOME',
            'USERPROFILE', 'HOMEDRIVE', 'HOMEPATH', 'APPDATA', 'LOCALAPPDATA', 'PROGRAMDATA', 'PROGRAMFILES',
            'PROGRAMFILES(X86)', 'PROGRAMW6432', 'COMMONPROGRAMFILES', 'COMMONPROGRAMFILES(X86)', 'ALLUSERSPROFILE',
            'PUBLIC', 'NUMBER_OF_PROCESSORS', 'PROCESSOR_ARCHITECTURE', 'PROCESSOR_IDENTIFIER', 'OS', 'USERNAME',
            'USER', 'LOGNAME', 'SHELL', 'LANG', 'LC_ALL', 'LC_CTYPE', 'TZ', 'XDG_RUNTIME_DIR', 'DBUS_SESSION_BUS_ADDRESS',
            'DISPLAY', 'WAYLAND_DISPLAY', 'VULKAN_SDK', 'VK_ICD_FILENAMES', 'CPM_SOURCE_CACHE', 'SSL_CERT_FILE',
            'SSL_CERT_DIR', 'REQUESTS_CA_BUNDLE', 'DEVELOPER_DIR', 'ANDROID_HOME', 'ANDROID_SDK_ROOT', 'JAVA_HOME')
REDACTED = b'[REDACTED]'
CHUNK = 128 * 1024


class CoordinatorError(RuntimeError):
    def __init__(self, status, message):
        super().__init__('coordinator answered %s: %s' % (status, message))
        self.status = status


def normalized_arch():
    machine = platform.machine().lower()
    return {'amd64': 'x86_64', 'x64': 'x86_64', 'arm64': 'aarch64'}.get(machine, machine)


def safe_url(url, allow_insecure=False):
    """HTTPS, or plain HTTP only to a loopback or private address (inside the CI project's VPC, whose traffic Google
    encrypts) unless explicitly allowed: a job token must never cross the Internet in the clear."""
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme == 'https':
        return url.rstrip('/')
    if parsed.scheme != 'http':
        raise SystemExit('coordinator URL must be https:// (got %s)' % url)
    host = parsed.hostname or ''
    try:
        private = host == 'localhost' or ipaddress.ip_address(host).is_private
    except ValueError:
        private = False
    if not private and not allow_insecure:
        raise SystemExit('plain http is allowed only to loopback or private addresses; use https for %s' % host)
    return url.rstrip('/')


class Client:
    """The agent side of the coordinator's agent API (JSON over HTTPS, bearer tokens)."""

    def __init__(self, base, token=None, opener=None, retries=6):
        self.base = base
        self.token = token
        self.opener = opener or urllib.request.urlopen
        self.retries = retries

    def call(self, method, path, body=None, data=None, headers=None, raw=False, retry=True, token=None):
        sent = {'X-Cerid-Agent-Protocol': str(PROTOCOL), 'User-Agent': 'cerid-agent/%s' % VERSION}
        sent.update(headers or {})
        token = token or self.token
        if token:
            sent['Authorization'] = 'Bearer %s' % token
        if body is not None:
            data = json.dumps(body).encode('utf-8')
            sent['Content-Type'] = 'application/json'
        attempts = self.retries if retry else 1
        for attempt in range(attempts):
            request = urllib.request.Request(self.base + path, data=data, method=method, headers=sent)
            try:
                with self.opener(request, timeout=60) as response:
                    payload = response.read()
                return payload if raw else (json.loads(payload.decode('utf-8')) if payload.strip() else {})
            except urllib.error.HTTPError as error:
                text = error.read().decode('utf-8', errors='replace')
                error.close()
                if error.code >= 500 and attempt < attempts - 1:
                    time.sleep(min(2 ** attempt, 30))
                    continue
                try:
                    message = json.loads(text).get('error', text)
                except ValueError:
                    message = text
                raise CoordinatorError(error.code, message[:500]) from None
            except (urllib.error.URLError, OSError, TimeoutError) as error:
                if attempt == attempts - 1:
                    raise CoordinatorError(0, 'unreachable: %s' % error) from None
                time.sleep(min(2 ** attempt, 30))
        raise CoordinatorError(0, 'unreachable')


class Redactor:
    """Replaces every secret value in a byte stream, including a value split across two writes: the last
    (longest secret - 1) bytes are held back until the next write or the end."""

    def __init__(self, values):
        self.values = sorted({v.encode('utf-8') for v in values if v and len(v) >= 4}, key=len, reverse=True)
        self.keep = max((len(v) for v in self.values), default=1) - 1
        self.tail = b''

    def scrub(self, data):
        for value in self.values:
            data = data.replace(value, REDACTED)
        return data

    def feed(self, data):
        if not self.values:
            return data
        buffer = self.scrub(self.tail + data)
        if len(buffer) <= self.keep:
            self.tail = buffer
            return b''
        self.tail = buffer[len(buffer) - self.keep:] if self.keep else b''
        return buffer[:len(buffer) - self.keep] if self.keep else buffer

    def close(self):
        rest, self.tail = self.scrub(self.tail), b''
        return rest

    def text(self, value):
        return self.scrub(value.encode('utf-8')).decode('utf-8', errors='replace')


class LogStream:
    """Chunks of the job's output to the coordinator: at most CHUNK bytes or one second per chunk, numbered so a
    retried upload is idempotent and the viewer reads them in order."""

    def __init__(self, client, job, redactor, interval=1.0, echo=None):
        self.client = client
        self.job = job
        self.redactor = redactor
        self.interval = interval
        self.echo = echo
        self.buffer = bytearray()
        self.seq = 0
        self.lock = threading.Lock()
        self.last = time.monotonic()
        self.failed = 0

    def write(self, data):
        clean = self.redactor.feed(data)
        if self.echo is not None and clean:
            self.echo.write(clean)
            self.echo.flush()
        with self.lock:
            self.buffer += clean
            due = len(self.buffer) >= CHUNK or time.monotonic() - self.last >= self.interval
        if due:
            self.flush()

    def line(self, text):
        self.write(('~~~ %s\n' % text).encode('utf-8'))

    def flush(self, final=False):
        with self.lock:
            if final:
                rest = self.redactor.close()
                if self.echo is not None and rest:
                    self.echo.write(rest)
                self.buffer += rest
            while self.buffer:
                piece = bytes(self.buffer[:CHUNK])
                try:
                    self.client.call('PUT', '/api/agent/jobs/%s/log?attempt=%d&seq=%d' % (
                        self.job['id'], self.job['attempt'], self.seq), data=piece,
                        headers={'Content-Type': 'application/octet-stream'})
                except CoordinatorError as error:
                    self.failed += 1
                    if error.status in (401, 403, 409) or final:
                        del self.buffer[:len(piece)]  # the job is gone or the chunk refused: never block on it
                        self.seq += 1
                    break
                del self.buffer[:len(piece)]
                self.seq += 1
            self.last = time.monotonic()


def kill_tree(process, grace=10.0):
    """Stop a command and everything it started."""
    if process.poll() is not None:
        return
    if os.name == 'nt':
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(process.pid)], capture_output=True, check=False)
    else:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            return
        deadline = time.monotonic() + grace
        while time.monotonic() < deadline and process.poll() is None:
            time.sleep(0.1)
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=30)
    except subprocess.TimeoutExpired:
        pass


CHECKOUT_STRATEGIES = ('direct', 'mirror')


def resolve_checkout(base, checkout):
    """The checkout to fetch. The strategy alone decides where from: `mirror` is the coordinator's mirror, whose
    path the agent puts after the coordinator address it already uses; `direct` is the repository URL exactly as
    configured, whatever its shape (a POSIX path such as /srv/git/cerid also starts with '/'). An unknown strategy
    is refused rather than guessed."""
    checkout = dict(checkout)
    strategy = checkout.get('strategy', 'direct')
    if strategy not in CHECKOUT_STRATEGIES:
        raise ValueError('unknown checkout strategy %r' % strategy)
    if strategy == 'mirror':
        if not checkout['url'].startswith('/git/'):
            raise ValueError('a mirror checkout names the coordinator path /git/<repository>.git, got %r'
                             % checkout['url'])
        checkout['url'] = base + checkout['url']
    return checkout


def clean_environment(job, extra, host=None):
    host = os.environ if host is None else host
    env = {}
    for name in HOST_ENV:
        for key in host:
            if key.upper() == name:
                env[key] = host[key]
    env.update(job['env'])
    env.update(extra)
    env.update(job.get('secrets', {}))
    # Names only, never values: in-job tools (artifact upload) read the values from their own environment to scrub.
    env['CERID_CI_SECRET_NAMES'] = ','.join(sorted(job.get('secrets', {})))
    env['PYTHONUTF8'] = '1'
    env['CERID_CI'] = '1'
    return env


def script_for(command, directory):
    """The command as a script file outside the checkout: bash with errexit on POSIX, a batch file that stops at the
    first failing line on Windows (both shells' defaults would run on past a failure)."""
    if os.name == 'nt':
        path = Path(directory) / 'step.cmd'
        lines = ['@echo off', 'setlocal']
        for line in command.splitlines():
            if line.strip():
                lines += [line, 'if errorlevel 1 exit /b %errorlevel%']
        path.write_text('\r\n'.join(lines) + '\r\n', encoding='utf-8')
        return ['cmd.exe', '/d', '/s', '/c', str(path)]
    path = Path(directory) / 'step.sh'
    path.write_text('set -eo pipefail\n' + command + '\n', encoding='utf-8')
    shell = shutil.which('bash') or '/bin/sh'
    return [shell, str(path)] if shell.endswith('bash') else [shell, '-e', str(path)]


class Agent:
    def __init__(self, coordinator, build_path, name=None, tags=None, mode='long-running', heartbeat=10.0,
                 poll=5.0, echo=False, git='git', allow_insecure=False, opener=None, script=None):
        self.base = safe_url(coordinator, allow_insecure)
        self.build_path = Path(build_path)
        self.name = name or platform.node() or 'agent'
        self.tags = dict(tags or {})
        self.mode = mode
        self.heartbeat_seconds = heartbeat
        self.poll_seconds = poll
        self.echo = sys.stdout.buffer if echo else None
        self.git = git
        self.opener = opener
        self.client = Client(self.base, opener=opener)
        self.stopping = threading.Event()
        self.script = script or os.path.abspath(__file__)

    def info(self):
        return {'name': self.name, 'os': platform.system().lower(), 'arch': normalized_arch(), 'tags': self.tags,
                'version': VERSION, 'protocol': PROTOCOL, 'mode': self.mode}

    # Acquiring work ------------------------------------------------------------------------------------------
    def register(self, registration_token):
        answer = self.client.call('POST', '/api/agent/register', dict(self.info(),
                                                                       registration_token=registration_token))
        self.client.token = answer['session_token']
        return answer['agent']

    def gce_identity(self):
        audience = urllib.parse.quote(self.base, safe='')
        request = urllib.request.Request(METADATA + 'service-accounts/default/identity?audience=%s&format=full' %
                                         audience, headers={'Metadata-Flavor': 'Google'})
        with (self.opener or urllib.request.urlopen)(request, timeout=10) as response:
            return response.read().decode('ascii').strip()

    def acquire_identity(self):
        return self.client.call('POST', '/api/agent/acquire', {'identity_token': self.gce_identity()})

    def claim(self, token):
        return self.client.call('POST', '/api/agent/claim', {}, token=token)

    def long_running(self, registration_token, max_jobs=None, exit_when_idle=None):
        self.register(registration_token)
        done = 0
        idle_since = time.monotonic()
        while not self.stopping.is_set():
            try:
                answer = self.client.call('POST', '/api/agent/poll', dict(self.info()))
            except CoordinatorError as error:
                if error.status == 401:
                    self.register(registration_token)
                    continue
                if error.status == 426:
                    raise SystemExit('the coordinator speaks another agent protocol: %s' % error)
                print('poll failed: %s' % error, file=sys.stderr)
                answer = {}
            if answer.get('job'):
                self.run(answer)
                done += 1
                idle_since = time.monotonic()
                if max_jobs and done >= max_jobs:
                    return 0
                continue
            if exit_when_idle is not None and time.monotonic() - idle_since > exit_when_idle:
                return 0
            self.stopping.wait(answer.get('wait', self.poll_seconds))
        return 0

    # Running one job ------------------------------------------------------------------------------------------
    def workdir(self, assignment):
        if self.mode == 'one-shot':
            return self.build_path / ('job-' + assignment['job']['id'])
        safe = ''.join(c if c.isalnum() or c in '-_' else '-' for c in self.name)
        return self.build_path / safe / assignment['job']['env'].get('CERID_CI_REPOSITORY', 'repository')

    def git_env(self, checkout, token):
        """`checkout` is resolved (resolve_checkout): its mirror URL is absolute."""
        env = clean_environment({'env': {}}, {'GIT_TERMINAL_PROMPT': '0'})
        if checkout.get('strategy') == 'mirror':
            # The mirror authenticates with the job token, passed through the environment: never on a command
            # line (process listings) and never in .git/config (the checkout outlives the token). The header is
            # scoped to the mirror's URL: the job's own git requests to other hosts (a FetchContent clone from
            # GitHub) must not carry it, and GitHub answers a foreign bearer token with 401.
            env.update({'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'http.%s.extraHeader' % checkout['url'],
                        'GIT_CONFIG_VALUE_0': 'Authorization: Bearer %s' % token})
        return env

    def checkout(self, assignment, log):
        checkout = resolve_checkout(self.base, assignment['checkout'])
        work = self.workdir(assignment)
        work.mkdir(parents=True, exist_ok=True)
        env = self.git_env(checkout, assignment['token'])

        def git(*args, check=True):
            completed = subprocess.run([self.git] + list(args), cwd=str(work), env=env, capture_output=True,
                                       check=False)
            output = completed.stdout + completed.stderr
            # Plumbing output (remote URLs, object ids) only when it explains a failure or a transfer.
            if output and (completed.returncode != 0 or args[0] in ('fetch', 'checkout')):
                log.write(output if output.endswith(b'\n') else output + b'\n')
            if check and completed.returncode != 0:
                raise RuntimeError('git %s exited %d' % (args[0], completed.returncode))
            return completed

        log.line('checkout %s at %s (%s)' % (checkout['url'] if checkout.get('strategy') != 'mirror' else 'mirror',
                                            checkout['commit'], checkout.get('branch', '')))
        if not (work / '.git').exists():
            git('init', '--quiet')
        if git('config', '--get', 'remote.origin.url', check=False).stdout:  # quiet when there is none yet
            git('remote', 'set-url', 'origin', checkout['url'])
        else:
            git('remote', 'add', 'origin', checkout['url'])
        depth = ['--depth=%d' % checkout['depth']] if checkout.get('depth') else []
        wanted = checkout.get('ref') or checkout['commit']
        if git('fetch', '--no-tags', '--quiet', *depth, 'origin', wanted, check=False).returncode != 0:
            branch = checkout.get('branch')
            if not branch:
                raise RuntimeError('the remote refused to send commit %s' % checkout['commit'])
            git('fetch', '--no-tags', '--quiet', *depth, 'origin',
                '+refs/heads/%s:refs/remotes/origin/%s' % (branch, branch))
        git('checkout', '--quiet', '--force', '--detach', checkout['commit'])
        git('clean', '-ffdxq')
        head = git('rev-parse', 'HEAD').stdout.decode('ascii').strip()
        if head != checkout['commit'] and not head.startswith(checkout['commit']):
            raise RuntimeError('checked out %s, wanted %s' % (head, checkout['commit']))
        return work

    def run(self, assignment):
        job = assignment['job']
        token = assignment['token']
        client = Client(self.base, token=token, opener=self.opener)
        redactor = Redactor(job.get('secrets', {}).values())
        log = LogStream(client, job, redactor, echo=self.echo)
        scratch = Path(tempfile.mkdtemp(prefix='cerid-job-'))
        cancel = {'reason': None}
        process_box = {}
        finished = threading.Event()

        def heartbeat():
            while not finished.wait(self.heartbeat_seconds):
                try:
                    answer = client.call('POST', '/api/agent/jobs/%s/heartbeat' % job['id'], {}, retry=False)
                except CoordinatorError as error:
                    if error.status in (401, 403, 404, 409):
                        answer = {'cancel': True, 'reason': 'the coordinator ended the job'}
                    else:
                        continue
                if answer.get('cancel') and not cancel['reason']:
                    cancel['reason'] = answer.get('reason') or 'canceled'
                    log.line('cancellation requested: %s' % cancel['reason'])
                    if 'process' in process_box:
                        kill_tree(process_box['process'])
                log.flush()

        def preemption():
            request = urllib.request.Request(METADATA + 'preempted?wait_for_change=true',
                                             headers={'Metadata-Flavor': 'Google'})
            while not finished.is_set():
                try:
                    with urllib.request.urlopen(request, timeout=3600) as response:
                        if response.read().strip().upper() == b'TRUE':
                            cancel['infra'] = 'preempted'
                            cancel['reason'] = 'the spot VM is being preempted'
                            if 'process' in process_box:
                                kill_tree(process_box['process'], grace=5)
                            return
                except OSError:
                    finished.wait(10)

        exit_status, infra, detail, timed_out = -1, None, '', False
        try:
            client.call('POST', '/api/agent/jobs/%s/start' % job['id'], {})
            beat = threading.Thread(target=heartbeat, daemon=True)
            beat.start()
            if self.mode == 'one-shot' and os.environ.get('CERID_AGENT_GCE') == '1':
                threading.Thread(target=preemption, daemon=True).start()
            log.line('cerid-agent %s on %s (%s %s), job %s attempt %d: %s' % (
                VERSION, self.name, platform.system(), normalized_arch(), job['key'], job['attempt'], job['label']))
            work = self.checkout(assignment, log)
            extra = {'CERID_AGENT_ENDPOINT': self.base, 'CERID_AGENT_JOB_ID': job['id'],
                     'CERID_AGENT_JOB_TOKEN': token, 'CERID_AGENT_SCRIPT': self.script,
                     'CERID_CI_CHECKOUT': str(work)}
            if assignment['checkout'].get('strategy') == 'mirror':
                extra.update({k: v for k, v in self.git_env(resolve_checkout(self.base, assignment['checkout']),
                                                            token).items() if k.startswith('GIT_CONFIG_')})
            env = clean_environment(job, extra)
            try:
                client.call('POST', '/api/agent/jobs/%s/environment' % job['id'],
                            reproducibility(job, env, assignment['checkout'], self.name, gce=os.environ.get(
                                'CERID_AGENT_GCE') == '1', opener=self.opener), retry=False)
            except CoordinatorError as error:  # the record is evidence, never a reason to fail the job
                log.line('reproducibility record not stored: %s' % error)
            argv = script_for(job['command'], scratch)
            log.line('running %s' % job['key'])
            flags = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {
                'start_new_session': True}
            process = subprocess.Popen(argv, cwd=str(work), env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       stdin=subprocess.DEVNULL, **flags)
            process_box['process'] = process
            step_started = time.time()
            deadline = time.monotonic() + float(job['timeout_minutes']) * 60.0

            def watch():
                while process.poll() is None:
                    if time.monotonic() > deadline:
                        cancel['timeout'] = True
                        cancel['reason'] = cancel['reason'] or 'timeout after %s minutes' % job['timeout_minutes']
                        log.line(cancel['reason'])
                        kill_tree(process)
                        return
                    time.sleep(0.5)
            threading.Thread(target=watch, daemon=True).start()
            while True:
                data = process.stdout.read1(CHUNK) if hasattr(process.stdout, 'read1') else process.stdout.read(4096)
                if not data:
                    break
                log.write(data)
            exit_status = process.wait()
            timed_out = bool(cancel.get('timeout'))
            infra = cancel.get('infra')
            log.line('exit status %d%s' % (exit_status, ' (%s)' % cancel['reason'] if cancel['reason'] else ''))
            if exit_status != 0 and not infra:
                collect_diagnostics(work, env, step_started, log.line)
            if job.get('artifact_paths') and not infra:
                upload_artifacts(client, job['id'], work, ';'.join(job['artifact_paths']), log.line,
                                 secrets=job.get('secrets', {}).values())
        except Exception as error:  # checkout or start failures are the job's failure, reported with the reason
            detail = redactor.text(str(error))[:500]
            log.line('agent error: %s' % detail)
            exit_status = -1
        finally:
            finished.set()
            log.flush(final=True)
            try:
                client.call('POST', '/api/agent/jobs/%s/finish' % job['id'], {
                    'exit_status': exit_status, 'infra': infra, 'detail': detail, 'timed_out': timed_out})
            except CoordinatorError as error:
                print('finish was not accepted: %s' % redactor.text(str(error)), file=sys.stderr)
            shutil.rmtree(scratch, ignore_errors=True)
            if self.mode == 'one-shot':
                shutil.rmtree(self.workdir(assignment), ignore_errors=True)
        return exit_status

TOOLS = (('git', ['--version']), ('cmake', ['--version']), ('ninja', ['--version']), ('ctest', ['--version']),
         ('gcc', ['--version']), ('g++', ['--version']), ('clang', ['--version']), ('clang-cl', ['--version']),
         ('python3', ['--version']), ('python', ['--version']), ('sccache', ['--version']), ('xcrun', ['--version']))
HIDDEN_ENV = ('CERID_AGENT_JOB_TOKEN', 'GIT_CONFIG_VALUE_0')


def tool_versions(env, run=subprocess.run, which=shutil.which):
    """First line of `<tool> --version` for every tool on the step's PATH (bounded, never fatal)."""
    out = {}
    for name, args in TOOLS:
        path = which(name, path=env.get('PATH') or env.get('Path'))
        if not path:
            continue
        try:
            completed = run([path] + args, capture_output=True, text=True, timeout=10, check=False, env=env)
            line = (completed.stdout or completed.stderr).strip().splitlines()
            out[name] = line[0][:200] if line else ''
        except (OSError, subprocess.TimeoutExpired):
            out[name] = 'unavailable'
    return out


def reproducibility(job, env, checkout, name, gce=False, opener=None, run=subprocess.run, which=shutil.which):
    """What it takes to reproduce this attempt: the commit, the machine and image, the toolchain versions and the
    step's environment with every secret value and token left out."""
    secret_names = set(job.get('secrets', {})) | set(HIDDEN_ENV)
    image = None
    if gce:
        try:
            request = urllib.request.Request(METADATA + 'image', headers={'Metadata-Flavor': 'Google'})
            with (opener or urllib.request.urlopen)(request, timeout=5) as response:
                image = response.read().decode('utf-8', 'replace')[:300]
        except OSError:
            image = 'unknown'
    return {'commit': checkout.get('commit'), 'checkout': checkout.get('strategy', 'direct'),
            'device': job['env'].get('CERID_CI_DEVICE'), 'machine': name, 'image': image,
            'os': '%s %s' % (platform.system(), platform.release()), 'platform': platform.platform(),
            'arch': normalized_arch(), 'cpu': platform.processor()[:200], 'cpus': os.cpu_count(),
            'python': platform.python_version(), 'agent_version': VERSION,
            'tools': tool_versions(env, run=run, which=which),
            'env': {k: ('[secret]' if k in secret_names else v[:500]) for k, v in sorted(env.items())
                    if k not in HIDDEN_ENV}}


# cerid-diagnostics/1 bundles (DIAG.5d, engine/foundation/perf/include/crd/perf/bundle.hpp): a 40-byte header
# (magic, schema, format, flags, created_at_ns, total_len, section_count, crc32 of the first 36 bytes), then per
# section a 32-byte header (tag, flags, payload_length, original_length, payload crc32, spare) and the payload. The
# CRC is IEEE 802.3, the same as zlib.crc32.
BUNDLE_MAGIC = b'CDB1'
BUNDLE_HEADER = struct.Struct('<IIIIQQI')
SECTION_HEADER = struct.Struct('<IIQQII')
SECTION_RAW_DUMP = 3
SECTION_ABSENT = 1
SECTION_TRUNCATED = 2
TEXT_SUFFIXES = ('.txt', '.log', '.md', '.json', '.xml', '.csv', '.html', '.toml', '.yaml', '.yml', '.ini', '.cfg')


def bundle_sections(data):
    """(header, [(tag, flags, payload, original_length)], complete) of a CDB1 bundle, recovering the complete prefix
    of a truncated or partly damaged one the way the engine's reader does; None when it is not a bundle at all."""
    if len(data) < 40 or data[:4] != BUNDLE_MAGIC:
        return None
    _, schema, version, flags, created, _, count, crc = struct.unpack_from('<IIIIQQII', data, 0)
    if zlib.crc32(data[:36]) != crc or version != 1:
        return None
    sections = []
    offset = 40
    for _ in range(count):
        if offset + SECTION_HEADER.size > len(data):
            break
        tag, section_flags, length, original, payload_crc, _ = SECTION_HEADER.unpack_from(data, offset)
        payload = data[offset + SECTION_HEADER.size:offset + SECTION_HEADER.size + length]
        if len(payload) != length or (length and zlib.crc32(payload) != payload_crc):
            break
        sections.append((tag, section_flags, payload, original))
        offset += SECTION_HEADER.size + length
    return {'schema': schema, 'flags': flags, 'created': created}, sections, len(sections) == count


def bundle_bytes(header, sections):
    """A well-formed CDB1 bundle of these sections: lengths, total and every CRC recomputed."""
    body = b''.join(SECTION_HEADER.pack(tag, flags, len(payload), original, zlib.crc32(payload) if payload else 0, 0)
                    + payload for tag, flags, payload, original in sections)
    head = BUNDLE_HEADER.pack(struct.unpack('<I', BUNDLE_MAGIC)[0], header['schema'], 1, header['flags'],
                              header['created'], 40 + len(body), len(sections))
    return head + struct.pack('<I', zlib.crc32(head)) + body


def secret_forms(values):
    """Every byte spelling of each secret a file can carry: UTF-8 and the UTF-16 a Windows process block holds."""
    forms = []
    for value in values:
        if value and len(value) >= 4:
            forms += [value.encode('utf-8'), value.encode('utf-16-le')]
    return sorted(set(forms), key=len, reverse=True)


def scrub_artifact(path, data, secrets):
    """(bytes to upload or None, note): no secret value ever leaves the machine. Text has every value replaced; a
    diagnostics bundle has every section scrubbed and its checksums rewritten; any other binary that carries a
    value is withheld, with a note saying so."""
    forms = secret_forms(secrets)
    if not any(form in data for form in forms):
        return data, None
    parsed = bundle_sections(data)
    if parsed is not None:
        header, sections, _ = parsed
        clean = []
        for tag, flags, payload, original in sections:
            for form in forms:
                payload = payload.replace(form, REDACTED)
            # A truncated section keeps the size it was cut from; any other one stays whole at its new length.
            clean.append((tag, flags, payload, original if flags & SECTION_TRUNCATED else len(payload)))
        cleaned = bundle_bytes(header, clean)
        if not any(form in cleaned for form in forms):
            return cleaned, 'secret values redacted in the bundle sections'
        return None, 'withheld: a secret value survives in the bundle'
    text = str(path).lower().endswith(TEXT_SUFFIXES)
    if not text and b'\x00' not in data:  # NUL bytes mark binary (and UTF-16) data even when it decodes as UTF-8
        try:
            data.decode('utf-8')
            text = True
        except UnicodeDecodeError:
            text = False
    if text:
        for form in forms:
            data = data.replace(form, REDACTED)
        return data, 'secret values redacted'
    return None, 'withheld: a secret value appears in this binary file'


def upload_artifacts(client, job_id, work, patterns, say, secrets=(), kind=None):
    """Upload every file matching the `;`-separated globs (relative to the checkout, `**` recursive), each scrubbed of
    the job's secret values first (scrub_artifact). `kind` types them (crash-dump, sanitizer-report, ...); without
    it the coordinator types each by its path."""
    count = 0
    secrets = list(secrets)
    work = Path(work).resolve()
    for pattern in [p.strip() for p in patterns.split(';') if p.strip()]:
        for match in sorted(glob.glob(str(work / pattern), recursive=True)):
            path = Path(match)
            if not path.is_file():
                continue
            relative = path.resolve().relative_to(work).as_posix()
            data, note = scrub_artifact(relative, path.read_bytes(), secrets)
            if note:
                say('artifact %s: %s' % (relative, note))
            if data is None:
                continue
            query = 'path=%s' % urllib.parse.quote(relative, safe='')
            if kind:
                query += '&kind=%s' % urllib.parse.quote(kind, safe='')
            try:
                client.call('PUT', '/api/agent/jobs/%s/artifacts?%s' % (job_id, query), data=data,
                            headers={'Content-Type': 'application/octet-stream',
                                     'X-Content-SHA256': hashlib.sha256(data).hexdigest()})
                count += 1
            except CoordinatorError as error:
                say('artifact %s not uploaded: %s' % (relative, error))
    say('%d artifacts uploaded' % count)
    return count


def collect_diagnostics(work, env, since, say, run=subprocess.run, timeout=600):
    """After a failed, timed-out or killed step: the checkout's own collector (ci/coordinator/diagnostics.py) gathers
    what the step left in every build tree it did not summarise itself, typed and scrubbed, for the end-of-job upload.
    A checkout without the collector, or a collector that fails, never changes the job's result."""
    script = Path(work) / 'ci' / 'coordinator' / 'diagnostics.py'
    if not script.is_file():
        return False
    try:
        completed = run([sys.executable, str(script), 'collect', '--checkout', str(work), '--since', repr(since)],
                        cwd=str(work), env=env, capture_output=True, text=True, timeout=timeout, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        say('diagnostics not collected: %s' % error)
        return False
    for line in (completed.stdout or '').splitlines()[:200]:
        say(line)
    if completed.returncode:
        say('diagnostics collector exited %d: %s' % (completed.returncode, (completed.stderr or '')[-300:]))
    return completed.returncode == 0


def job_secret_values(environ=None):
    """The secret values of the running job, read from the step's environment by the names the agent passed."""
    environ = os.environ if environ is None else environ
    names = [n for n in environ.get('CERID_CI_SECRET_NAMES', '').split(',') if n]
    return [environ[n] for n in names if environ.get(n)]


# --------------------------------------------------------------------------------------------- in-job tools

def job_client():
    endpoint = os.environ.get('CERID_AGENT_ENDPOINT')
    token = os.environ.get('CERID_AGENT_JOB_TOKEN')
    job = os.environ.get('CERID_AGENT_JOB_ID')
    if not (endpoint and token and job):
        raise SystemExit('not inside a Cerid CI job (CERID_AGENT_ENDPOINT, _JOB_ID and _JOB_TOKEN are unset)')
    return Client(endpoint, token=token), job


def tool(args):
    client, job = job_client()
    if args.tool == 'pipeline':
        text = sys.stdin.read() if args.file in (None, '-') else Path(args.file).read_text(encoding='utf-8')
        answer = client.call('POST', '/api/agent/jobs/%s/pipeline' % job, json.loads(text))
        print('uploaded %d steps' % len(answer.get('steps', [])))
        return 0
    if args.tool == 'annotate':
        if args.body_file:
            body = Path(args.body_file).read_text(encoding='utf-8')
        else:
            body = sys.stdin.read() if args.body is None else args.body
        client.call('POST', '/api/agent/jobs/%s/annotations' % job, {'context': args.context, 'style': args.style,
                                                                      'body': body, 'append': args.append})
        return 0
    if args.tool == 'artifact':
        if args.action == 'upload':
            work = Path(os.environ.get('CERID_CI_CHECKOUT', os.getcwd()))
            return 0 if upload_artifacts(client, job, work, args.path, print, secrets=job_secret_values(),
                                         kind=args.kind) else 1
        data = client.call('GET', '/api/agent/jobs/%s/artifacts?path=%s' % (job, urllib.parse.quote(
            args.path, safe='')), raw=True)
        destination = Path(args.destination or '.')
        target = destination / Path(args.path).name if destination.is_dir() else destination
        target.write_bytes(data)
        return 0
    if args.tool == 'meta-data':
        path = '/api/agent/jobs/%s/meta-data/%s' % (job, urllib.parse.quote(args.key, safe=''))
        if args.action == 'set':
            client.call('PUT', path, {'value': args.value})
        else:
            print(client.call('GET', path)['value'])
        return 0
    if args.tool == 'step':
        answer = client.call('GET', '/api/agent/jobs/%s/steps/%s' % (job, urllib.parse.quote(args.key, safe='')))
        print(answer['outcome'])
        return 0
    return 2


def parse_tags(text):
    tags = {}
    for part in (text or '').split(','):
        if '=' in part:
            key, value = part.split('=', 1)
            tags[key.strip()] = value.strip()
    return tags


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest='tool', required=True)
    start = sub.add_parser('start', help='run the agent')
    start.add_argument('--coordinator', required=True)
    start.add_argument('--mode', choices=('long-running', 'one-shot'), default='long-running')
    start.add_argument('--registration-token-file')
    start.add_argument('--identity', choices=('gce',))
    start.add_argument('--token-stdin', action='store_true')
    start.add_argument('--token-file', help='a one-job token file, read and deleted at start')
    start.add_argument('--name')
    start.add_argument('--tags')
    start.add_argument('--build-path', default=os.path.join(tempfile.gettempdir(), 'cerid-ci-builds'))
    start.add_argument('--heartbeat-seconds', type=float, default=10.0)
    start.add_argument('--poll-seconds', type=float, default=5.0)
    start.add_argument('--max-jobs', type=int)
    start.add_argument('--exit-when-idle', type=float)
    start.add_argument('--echo', action='store_true', help='also copy job output to standard output')
    start.add_argument('--allow-insecure-http', action='store_true')
    pipe = sub.add_parser('pipeline')
    pipe.add_argument('action', choices=('upload',))
    pipe.add_argument('file', nargs='?')
    note = sub.add_parser('annotate')
    note.add_argument('--context', default='default')
    note.add_argument('--style', default='info')
    note.add_argument('--append', action='store_true')
    note.add_argument('--body-file')
    note.add_argument('body', nargs='?')
    art = sub.add_parser('artifact')
    art.add_argument('action', choices=('upload', 'download'))
    art.add_argument('path')
    art.add_argument('destination', nargs='?')
    art.add_argument('--kind', help='type the uploaded files (crash-dump, sanitizer-report, gold-image, ...)')
    meta = sub.add_parser('meta-data')
    meta.add_argument('action', choices=('set', 'get'))
    meta.add_argument('key')
    meta.add_argument('value', nargs='?')
    step = sub.add_parser('step')
    step.add_argument('action', choices=('outcome',))
    step.add_argument('key')
    args = parser.parse_args(argv)
    if args.tool != 'start':
        return tool(args)
    agent = Agent(args.coordinator, args.build_path, name=args.name, tags=parse_tags(args.tags), mode=args.mode,
                  heartbeat=args.heartbeat_seconds, poll=args.poll_seconds, echo=args.echo,
                  allow_insecure=args.allow_insecure_http)

    def stop(signum, frame):
        agent.stopping.set()
    for name in ('SIGTERM', 'SIGINT'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), stop)
    if args.mode == 'long-running':
        if not args.registration_token_file:
            raise SystemExit('a long-running agent needs --registration-token-file')
        token = Path(args.registration_token_file).read_text(encoding='utf-8').strip()
        return agent.long_running(token, max_jobs=args.max_jobs, exit_when_idle=args.exit_when_idle)
    if args.identity == 'gce':
        os.environ['CERID_AGENT_GCE'] = '1'
        assignment = agent.acquire_identity()
    elif args.token_stdin:
        assignment = agent.claim(sys.stdin.readline().strip())
    elif args.token_file:
        path = Path(args.token_file)
        token = path.read_text(encoding='utf-8').strip()
        path.unlink()
        assignment = agent.claim(token)
    else:
        raise SystemExit('a one-shot agent needs --identity gce, --token-stdin or --token-file')
    status = agent.run(assignment)
    return 0 if status == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
