"""Bounded build-tool process trees; a waiting child is contained before native tools may start."""
from __future__ import annotations

import ctypes
import json
import math
import os
from pathlib import Path
import signal
import subprocess
import sys
import time


class ProcessError(RuntimeError):
    pass


class WindowsJob:
    def __init__(self):
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel, self.handle = kernel, None
        kernel.CreateJobObjectW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
        kernel.CreateJobObjectW.restype = ctypes.c_void_p
        kernel.SetInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
        kernel.SetInformationJobObject.restype = ctypes.c_int
        kernel.AssignProcessToJobObject.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        kernel.AssignProcessToJobObject.restype = ctypes.c_int
        kernel.TerminateJobObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
        kernel.TerminateJobObject.restype = ctypes.c_int
        kernel.QueryInformationJobObject.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p,
                                                     ctypes.c_uint32, ctypes.c_void_p]
        kernel.QueryInformationJobObject.restype = ctypes.c_int
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        kernel.CloseHandle.restype = ctypes.c_int

        class BasicLimits(ctypes.Structure):
            _fields_ = [('process_time', ctypes.c_int64), ('job_time', ctypes.c_int64),
                        ('flags', ctypes.c_uint32), ('minimum_working_set', ctypes.c_size_t),
                        ('maximum_working_set', ctypes.c_size_t), ('active_process_limit', ctypes.c_uint32),
                        ('affinity', ctypes.c_size_t), ('priority', ctypes.c_uint32), ('scheduling', ctypes.c_uint32)]

        class ExtendedLimits(ctypes.Structure):
            _fields_ = [('basic', BasicLimits), ('io_counters', ctypes.c_uint64 * 6),
                        ('process_memory', ctypes.c_size_t), ('job_memory', ctypes.c_size_t),
                        ('peak_process_memory', ctypes.c_size_t), ('peak_job_memory', ctypes.c_size_t)]

        self.handle = kernel.CreateJobObjectW(None, None)
        if not self.handle:
            raise ProcessError(f'CreateJobObject failed: {ctypes.get_last_error()}')
        limits = ExtendedLimits()
        limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; no breakaway permissions.
        if not kernel.SetInformationJobObject(self.handle, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
            error = ctypes.get_last_error()
            kernel.CloseHandle(self.handle)
            self.handle = None
            raise ProcessError(f'SetInformationJobObject failed: {error}')

    def attach(self, process):
        if not self.kernel.AssignProcessToJobObject(self.handle, int(process._handle)):
            raise ProcessError(f'AssignProcessToJobObject failed before command start: {ctypes.get_last_error()}')

    def stop(self):
        if self.handle is None:
            return

        class Accounting(ctypes.Structure):
            _fields_ = [('times', ctypes.c_int64 * 4), ('faults', ctypes.c_uint32), ('total', ctypes.c_uint32),
                        ('active', ctypes.c_uint32), ('terminated', ctypes.c_uint32)]

        try:
            if not self.kernel.TerminateJobObject(self.handle, 125):
                raise ProcessError(f'TerminateJobObject failed: {ctypes.get_last_error()}')
            deadline = time.monotonic() + 10
            while True:
                accounting = Accounting()
                if not self.kernel.QueryInformationJobObject(self.handle, 1, ctypes.byref(accounting),
                                                             ctypes.sizeof(accounting), None):
                    raise ProcessError(f'Job accounting query failed: {ctypes.get_last_error()}')
                if accounting.active == 0:
                    break
                if time.monotonic() >= deadline:
                    raise ProcessError('Process-tree termination was not confirmed within the cleanup budget')
                time.sleep(0.01)
        finally:
            self.kernel.CloseHandle(self.handle)
            self.handle = None


def stop_tree(process, job):
    if job:
        job.stop()
    elif os.name != 'nt':
        # Keep the supervisor unreaped until the final kill, so its process-group ID cannot be recycled.
        for sig in (signal.SIGTERM, signal.SIGKILL):
            try:
                os.killpg(process.pid, sig)
            except ProcessLookupError:
                pass
            if sig == signal.SIGTERM:
                time.sleep(0.1)
    if os.name == 'nt' and process.poll() is None:
        # Assignment failed while the supervisor was still waiting for authorization; no child tool exists.
        process.kill()
    process.wait(timeout=10)


def supervisor_exit(process):
    if os.name == 'nt':
        return process.poll()
    # WNOWAIT leaves the process-group leader reserved until stop_tree has addressed its descendants.
    if hasattr(os, 'waitid') and hasattr(os, 'WNOWAIT'):
        value = os.waitid(os.P_PID, process.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
        return value.si_status if value else None
    return None


def run_command(argv, cwd, environment, directory, timeout):
    """Return exact native exit/status and durable logs. Timeout is a budget, never a diagnosis of a hang."""
    if not argv or not math.isfinite(timeout) or timeout <= 0:
        raise ProcessError('A nonempty command and a positive execution budget are required')
    directory = Path(directory).resolve()
    directory.mkdir(parents=True, exist_ok=False)
    log_path, status_path = directory / 'output.log', directory / 'native-status.json'
    started = time.time()
    start = time.monotonic()
    result = {'argv': [str(arg) for arg in argv], 'cwd': str(Path(cwd).resolve()), 'timeout_seconds': timeout,
              'started_unix': started, 'status': 'starting', 'exit_code': None, 'log': str(log_path)}
    process, job = None, None
    try:
        with log_path.open('xb') as output:
            options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {'start_new_session': True}
            if os.name == 'nt':
                job = WindowsJob()
            process = subprocess.Popen([sys.executable, '-I', '-S', str(Path(__file__).resolve()), '--child'],
                                       stdin=subprocess.PIPE, stdout=output, stderr=subprocess.STDOUT,
                                       cwd=cwd, env=environment, **options)
            result['supervisor_pid'] = process.pid
            if job:
                job.attach(process)
            request = {'argv': result['argv'], 'cwd': result['cwd'], 'status': str(status_path)}
            process.stdin.write((json.dumps(request) + '\n').encode('utf-8'))
            process.stdin.flush()
            result['status'] = 'running'
            status_denied_since = None
            while True:
                try:
                    # The child atomically renames this completed document before waiting for cleanup. A transient
                    # sharing/access denial can prevent reopening it; retry the read, never the native command.
                    native = json.loads(status_path.read_text(encoding='utf-8'))
                except FileNotFoundError:
                    native = None
                except PermissionError as error:
                    now = time.monotonic()
                    if status_denied_since is None:
                        status_denied_since = now
                    result['status_read_retries'] = result.get('status_read_retries', 0) + 1
                    result['status_read_last_error'] = str(error)
                    if now - status_denied_since >= 1:
                        raise
                    native = None
                if native is not None:
                    if type(native['exit_code']) is not int:
                        raise ProcessError('Native exit evidence is not an integer')
                    result['exit_code'] = native['exit_code']
                    result['status'] = 'passed' if native['exit_code'] == 0 else 'failed'
                    break
                exited = supervisor_exit(process)
                if exited is not None:
                    raise ProcessError(f'Supervisor exited {exited} without native exit evidence')
                if time.monotonic() - start >= timeout:
                    result['status'], result['exit_code'] = 'budget_exhausted', 124
                    break
                time.sleep(0.025)
    except KeyboardInterrupt:
        result['status'], result['exit_code'] = 'interrupted', 130
    except (ProcessError, OSError, ValueError, KeyError) as error:
        result['status'], result['exit_code'], result['error'] = 'instrument_failure', 125, str(error)
    finally:
        try:
            if process:
                stop_tree(process, job)
                process.stdin.close()
            elif job:
                job.stop()
        except (ProcessError, OSError, subprocess.TimeoutExpired) as error:
            result['native_outcome'] = {'status': result['status'], 'exit_code': result['exit_code']}
            result['status'], result['exit_code'], result['error'] = 'instrument_failure', 125, str(error)
        result['duration_seconds'] = time.monotonic() - start
        result['ended_unix'] = time.time()
        (directory / 'process.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def child_main():
    request = json.loads(sys.stdin.buffer.readline())
    child = subprocess.Popen(request['argv'], cwd=request['cwd'], stdin=subprocess.DEVNULL)
    while child.poll() is None:
        if os.name != 'nt':
            import select
            if select.select([sys.stdin.buffer], [], [], 0.05)[0] and not os.read(sys.stdin.fileno(), 1):
                # Parent died during a tool run. Terminate this supervised group, including ourselves.
                os.killpg(os.getpgrp(), signal.SIGKILL)
        else:
            time.sleep(0.025)
    path = Path(request['status'])
    temporary = path.with_suffix('.pending')
    temporary.write_text(json.dumps({'exit_code': child.returncode}), encoding='utf-8')
    os.replace(temporary, path)
    # Preserve the group leader until the parent confirms the result and stops the complete process tree.
    sys.stdin.buffer.read(1)
    if os.name != 'nt':
        os.killpg(os.getpgrp(), signal.SIGKILL)


if __name__ == '__main__':
    if sys.argv[1:] != ['--child']:
        raise SystemExit('Internal supervisor; use scripts/dev.py')
    child_main()
