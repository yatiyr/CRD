#!/usr/bin/env python3
"""Run crd-crypto-tests' TPM cases against the pinned swtpm, a TPM 2.0 simulator of their own.

The simulator (scripts/install-swtpm.py) keeps its state in a temporary directory and listens on two free loopback
ports; the tests reach it through tss2's swtpm TCTI (CRD_TPM_TCTI) and load the prefix's tss2 libraries
(LD_LIBRARY_PATH), so no system TPM, resource manager or tss2 install is touched and nothing survives the run.

  1. "[key-store]": every key store case, the TPM's among them (protector and device), against the fresh simulator
  2. "[.tpm-bus]" through tss2's pcap TCTI, which records every command and response: the canary master key the case
     protects and recovers must appear nowhere in the recording (the salted session's parameter encryption), while
     the recording holds the NV_Write and NV_Read commands that carried it
  3. the owner hierarchy is given an authorization over tss2's ESYS (Esys_HierarchyChangeAuth, through ctypes), then
     "[.tpm-owner-auth]": a TPM whose owner hierarchy has one is Unavailable, before any command that would use it

The simulator is stopped before the script returns; the result is the first failing run's exit code.
Contract: docs/design/network-foundation.md#key-store (decision K13).
"""
from pathlib import Path
import argparse
import ctypes
import os
import socket
import subprocess
import sys
import tempfile
import time

SECONDS = 600
OWNER_AUTH = b'cerid-test-owner'  # a test value: the simulator's, gone with its state directory
CANARY = b'cerid-tpm-bus-canary-0123456789!'  # the master key "[.tpm-bus]" protects
NV_WRITE = (0x137).to_bytes(4, 'big')  # TPM_CC_NV_Write
NV_READ = (0x14E).to_bytes(4, 'big')  # TPM_CC_NV_Read
ESYS_TR_NONE = 0xFFF
ESYS_TR_PASSWORD = 0x0FF
ESYS_TR_RH_OWNER = 0x101


class Tpm2bAuth(ctypes.Structure):
    _fields_ = [('size', ctypes.c_uint16), ('buffer', ctypes.c_uint8 * 64)]


def free_ports():
    """Two consecutive loopback ports nothing listens on now: tss2's swtpm TCTI reaches the simulator's control channel
    on the port after its command port."""
    for _ in range(64):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as first:
            first.bind(('127.0.0.1', 0))
            port = first.getsockname()[1]
            if port >= 65535:
                continue
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as second:
                try:
                    second.bind(('127.0.0.1', port + 1))
                except OSError:
                    continue
                return port, port + 1
    raise RuntimeError('no two consecutive free loopback ports')


def library_path(swtpm):
    libraries = Path(swtpm).parents[2] / 'usr' / 'lib' / 'x86_64-linux-gnu'
    return f'{libraries}:{libraries / "swtpm"}'


def wait_listening(port, seconds=30):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=1):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def set_owner_auth(tcti):
    """Gives the simulator's owner hierarchy an authorization, as an administrator's tpm2_changeauth would; run in a
    process started with the prefix's LD_LIBRARY_PATH, which the libraries' own dependencies are found through."""
    loader = ctypes.CDLL('libtss2-tctildr.so.0')
    esys = ctypes.CDLL('libtss2-esys.so.0')
    context = ctypes.c_void_p()
    transport = ctypes.c_void_p()
    if loader.Tss2_TctiLdr_Initialize(tcti.encode(), ctypes.byref(transport)) != 0:
        return False
    try:
        if esys.Esys_Initialize(ctypes.byref(context), transport, None) != 0:
            return False
        auth = Tpm2bAuth(len(OWNER_AUTH))
        ctypes.memmove(auth.buffer, OWNER_AUTH, len(OWNER_AUTH))
        changed = esys.Esys_HierarchyChangeAuth(context, ctypes.c_uint32(ESYS_TR_RH_OWNER),
                                                ctypes.c_uint32(ESYS_TR_PASSWORD), ctypes.c_uint32(ESYS_TR_NONE),
                                                ctypes.c_uint32(ESYS_TR_NONE), ctypes.byref(auth))
        esys.Esys_Finalize(ctypes.byref(context))
        return changed == 0
    finally:
        loader.Tss2_TctiLdr_Finalize(ctypes.byref(transport))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--swtpm', required=True, help='the pinned swtpm (scripts/install-swtpm.py)')
    parser.add_argument('--set-owner-auth', metavar='TCTI', help=argparse.SUPPRESS)
    parser.add_argument('tests', nargs=argparse.REMAINDER, help='-- crd-crypto-tests [arguments]')
    args = parser.parse_args(argv)
    if args.set_owner_auth:
        return 0 if set_owner_auth(args.set_owner_auth) else 1
    tests = args.tests[1:] if args.tests[:1] == ['--'] else args.tests
    if not tests:
        parser.error('name the test program after --')
    swtpm = str(Path(args.swtpm).resolve())
    libraries = library_path(swtpm)
    with tempfile.TemporaryDirectory(prefix='crd-tpm-') as state:
        server, control = free_ports()
        env = dict(os.environ)
        env['LD_LIBRARY_PATH'] = os.pathsep.join(filter(None, (libraries, env.get('LD_LIBRARY_PATH'))))
        simulator = subprocess.Popen(
            [swtpm, 'socket', '--tpm2', '--tpmstate', f'dir={state}',
             '--server', f'type=tcp,port={server},bindaddr=127.0.0.1',
             '--ctrl', f'type=tcp,port={control},bindaddr=127.0.0.1', '--flags', 'not-need-init,startup-clear'],
            env=env)
        try:
            if not wait_listening(server):
                print('tpm-simulator-session: swtpm did not start listening', file=sys.stderr)
                return 1
            tcti = f'swtpm:host=127.0.0.1,port={server}'
            env['CRD_TPM_TCTI'] = tcti
            result = subprocess.run(tests + ['[key-store]'], env=env, timeout=SECONDS).returncode
            if result != 0:
                return result
            recording = Path(state) / 'bus.pcap'
            recorded = dict(env, CRD_TPM_TCTI='pcap:' + tcti, TCTI_PCAP_FILE=str(recording))
            result = subprocess.run(tests + ['[.tpm-bus]'], env=recorded, timeout=SECONDS).returncode
            if result != 0:
                return result
            traffic = recording.read_bytes() if recording.is_file() else b''
            if NV_WRITE not in traffic or NV_READ not in traffic:
                print('tpm-simulator-session: the recording holds no NV_Write and NV_Read', file=sys.stderr)
                return 1
            if CANARY in traffic:
                print('tpm-simulator-session: the master key crossed the bus in the clear', file=sys.stderr)
                return 1
            print(f'tpm-simulator-session: {len(traffic)} bytes of TPM traffic recorded, the master key in none')
            changer = [sys.executable, str(Path(__file__).resolve()), '--swtpm', swtpm, '--set-owner-auth', tcti]
            if subprocess.run(changer, env=env, timeout=60).returncode != 0:
                print('tpm-simulator-session: the owner hierarchy took no authorization', file=sys.stderr)
                return 1
            return subprocess.run(tests + ['[.tpm-owner-auth]'], env=env, timeout=SECONDS).returncode
        finally:
            simulator.terminate()
            try:
                simulator.wait(timeout=30)
            except subprocess.TimeoutExpired:
                simulator.kill()
                simulator.wait()


if __name__ == '__main__':
    sys.exit(main())
