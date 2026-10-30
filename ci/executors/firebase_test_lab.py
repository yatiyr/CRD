"""firebase_test_lab: device jobs driven from the launcher against Firebase Test Lab (Tier B, the Spark plan's free
daily quota) in the existing Firebase project (`CERID_FTL_PROJECT`, the project the FTL_GCP_PROJECT repository
variable names).

A job is taken by a one-shot Cerid agent process on the launcher itself (FTL does the device work; the launcher only
waits), so no VM is paid for. Its one job token reaches it on standard input. The job's command builds the
`gcloud firebase test` invocation below; the daily quota (physical and virtual device tests) is kept in the result
store and a test past it is reported as budget, never run.
"""
from __future__ import annotations

from pathlib import Path
import datetime as dt
import subprocess
import sys

from . import AGENT

# Spark plan quotas (Firebase Test Lab documentation, read 2026-10-30): tests per day.
DAILY_QUOTA = {'physical': 5, 'virtual': 10}


class FirebaseTestLab:
    name = 'firebase_test_lab'

    def __init__(self, environ, popen=subprocess.Popen):
        self.environ = environ
        self.popen = popen

    def missing(self, device):
        return [] if self.environ.get('CERID_FTL_PROJECT') else ['CERID_FTL_PROJECT']

    def test_command(self, device, artifact, kind='game-loop', results_dir=None, timeout='15m'):
        config = device['executor_config']
        project = self.environ.get('CERID_FTL_PROJECT', '')
        spec = 'model=%s,version=%s,locale=en,orientation=portrait' % (config['model'], config['version'])
        if config.get('platform') == 'ios':
            command = ['gcloud', 'firebase', 'test', 'ios', 'run', '--project', project, '--type', kind,
                       '--app' if kind == 'game-loop' else '--test', artifact, '--device', spec]
        else:
            command = ['gcloud', 'firebase', 'test', 'android', 'run', '--project', project, '--type', kind,
                       '--app', artifact, '--device', spec]
        command += ['--timeout', timeout, '--format', 'json']
        if results_dir:
            command += ['--results-dir', results_dir]
        return command

    def quota_left(self, store, device, today=None):
        today = today or dt.date.today().isoformat()
        kind = 'physical' if device['executor_config'].get('physical', True) else 'virtual'
        used = len(store.list('quota/ftl/%s/%s/' % (today, kind))) if store else 0
        return DAILY_QUOTA[kind] - used

    def take_quota(self, store, device, job_id, today=None):
        today = today or dt.date.today().isoformat()
        kind = 'physical' if device['executor_config'].get('physical', True) else 'virtual'
        store.put_json('quota/ftl/%s/%s/%s.json' % (today, kind, job_id), {'device': device['id']})

    def argv(self, machine, agent):
        builds = Path(self.environ.get('CERID_CI_LAUNCHER_BUILDS', '/var/lib/cerid-ci/ftl-builds'))
        return [sys.executable, str(AGENT), 'start', '--mode', 'one-shot', '--token-stdin', '--coordinator',
                agent['coordinator'], '--build-path', str(builds), '--name', machine]

    def launch(self, machine, device, agent):
        """Start a one-shot agent process on the launcher; its job token goes in on standard input."""
        if not agent.get('token'):
            raise RuntimeError('a Test Lab job needs the one job token of its launch')
        process = self.popen(self.argv(machine, agent), stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
        process.stdin.write((agent['token'] + '\n').encode('utf-8'))
        process.stdin.close()
        return {'pid': process.pid, 'machine': machine}

    def teardown(self, handle):
        return None
