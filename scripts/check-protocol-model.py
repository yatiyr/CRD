#!/usr/bin/env python3
"""Prove Cerid's protocol models with the pinned Tamarin and hold every lemma to its expected verdict (NET.2's model
clause).

Each model in tests/foundation/crypto/model is proven once (`tamarin-prover --prove`, the pinned Maude beside the
binary). The check fails when Tamarin fails, reports a wellformedness warning, leaves a lemma unproven ("analysis
incomplete"), falsifies one, or when the lemmas a model holds differ from the ones MODELS names: a lemma removed,
renamed or added without this list is a failure, so a property cannot vanish from CI unseen. A model also cites the
design sections it formalizes (`Design: docs/...#anchor` lines in its comments); each anchor must exist, so the model
and the contract it proves move together. Linux only (Tamarin's pinned build); the crd-crypto-protocol-model CTest
(CRD_BUILD_PROTOCOL_MODEL, CRD_TAMARIN from scripts/install-tamarin.py). Contract:
docs/design/network-foundation.md#protocol-model.

    python3 scripts/check-protocol-model.py --tamarin ~/.cerid/tools/tamarin/1.12.0/tamarin-prover
"""
from pathlib import Path
import argparse
import re
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
MODEL_DIR = ROOT / 'tests' / 'foundation' / 'crypto' / 'model'
# Every model and the lemmas it must prove; Tamarin reports an exists-trace lemma "verified" when it found the trace.
MODELS = {
    'tls_handshake.spthy': ('handshake_completes', 'no_downgrade', 'client_authenticates_server',
                            'server_authenticates_client', 'handshake_secret', 'client_traffic_secret',
                            'server_traffic_secret'),
    'tls_key_update.spthy': ('updates_complete', 'sender_epoch', 'receiver_epoch', 'root_secrecy', 'epoch_secrecy',
                             'record_authentic', 'update_authentic', 'update_does_not_heal'),
    'tls_resumption.spthy': ('ticket_sources', 'resumption_completes', 'psk_secrecy', 'opened_issued',
                             'early_data_once', 'early_data_authentic', 'early_data_secrecy',
                             'early_data_not_forward_secret', 'client_authenticates_server',
                             'server_authenticates_client', 'client_traffic_secret', 'server_traffic_secret'),
}
DERIVCHECK_SECONDS = 120
RESULT = re.compile(r'^\s+(\w+) \((all-traces|exists-trace)\): (verified|falsified|analysis incomplete)\b', re.M)
DESIGN = re.compile(r'Design: (\S+?)#([\w.-]+)')


def verdicts(output):
    """{lemma: verdict} from the summary Tamarin prints after its proofs."""
    summary = output[output.rfind('summary of summaries:'):] if 'summary of summaries:' in output else ''
    return {match.group(1): match.group(3) for match in RESULT.finditer(summary)}


def judge(name, output, returncode, expected):
    """The failures of one model's run."""
    if returncode != 0:
        return [f'{name}: tamarin-prover exited {returncode}']
    failures = []
    if 'wellformedness check failed' in output or 'WARNING' in output:
        failures.append(f'{name}: Tamarin reports a wellformedness warning')
    found = verdicts(output)
    if not found:
        return failures + [f'{name}: no proof summary in the output']
    for lemma in sorted(set(expected) - set(found)):
        failures.append(f'{name}: the expected lemma {lemma} is missing')
    for lemma in sorted(set(found) - set(expected)):
        failures.append(f'{name}: the lemma {lemma} is not listed in check-protocol-model.py MODELS')
    for lemma, verdict in sorted(found.items()):
        if lemma in expected and verdict != 'verified':
            failures.append(f'{name}: {lemma} is {verdict}')
    return failures


def design_failures(name, text, root=ROOT):
    """The model's design citations whose file or anchor does not exist; a model must cite at least one."""
    cited = DESIGN.findall(text)
    if not cited:
        return [f'{name}: cites no design section (a "Design: docs/...#anchor" line)']
    failures = []
    for path, anchor in cited:
        target = root / path
        if not target.is_file():
            failures.append(f'{name}: cites {path}, which does not exist')
        elif f'<a id="{anchor}"></a>' not in target.read_text(encoding='utf-8'):
            failures.append(f'{name}: cites {path}#{anchor}, which has no such anchor')
    return failures


def prove(tamarin, model, rts):
    """Run Tamarin over one model: (exit code, combined output)."""
    maude = Path(tamarin).parent / 'maude'
    # The derivation checks are part of wellformedness; Tamarin's default 5 s budget times them out on a busy host.
    command = [str(tamarin), '--prove', f'--derivcheck-timeout={DERIVCHECK_SECONDS}', f'--with-maude={maude}',
               str(model), '+RTS', *rts.split(), '-RTS']
    result = subprocess.run(command, capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--tamarin', required=True, type=Path, help='the pinned tamarin-prover (CRD_TAMARIN)')
    parser.add_argument('--rts', default='-N4 -M8g', help="GHC runtime options: threads and heap bound")
    args = parser.parse_args(argv)
    present = sorted(path.name for path in MODEL_DIR.glob('*.spthy'))
    failures = [f'{name}: a model outside check-protocol-model.py MODELS' for name in present if name not in MODELS]
    failures += [f'{name}: listed in MODELS but missing' for name in MODELS if name not in present]
    for name, expected in MODELS.items():
        model = MODEL_DIR / name
        if not model.is_file():
            continue
        failures += design_failures(name, model.read_text(encoding='utf-8'))
        start = time.monotonic()
        returncode, output = prove(args.tamarin, model, args.rts)
        found = judge(name, output, returncode, expected)
        failures += found
        state = 'FAIL' if found else 'ok'
        print(f'[protocol-model] {name}: {len(verdicts(output))} lemmas, {time.monotonic() - start:.0f} s, {state}')
        if found:
            print(output[-4000:])
    for failure in failures:
        print(f'[protocol-model] FAIL {failure}')
    print('[protocol-model] ' + ('FAIL' if failures else 'PASS'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
