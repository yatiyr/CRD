"""Generate the crypto policy bootstrap headers (NET.2; docs/design/network-foundation.md#policy-state).

Each output embeds one directory's trust set and newest signed policy generation as constexpr data:

  trust-set.toml          the installation's trust set as `ceridc policy key` reports each entry: [[key]] tables of
                          `algorithm` (the standards register's name) and `public_key` (lower-case hex)
  generation-<N>.policy   a signed generation as `ceridc policy assemble` writes it; the highest N is embedded

A directory with neither yields the empty bootstrap; a trust set without a generation embeds the trust set alone (the
keys are made, nothing is signed yet); a generation without a trust set is refused. The bytes are copied, not
judged: crd-crypto's decoder is the one reader (decision S22), and crd-crypto-tests verifies each embedded generation
under its trust set.

  python scripts/gen-policy-bootstrap.py           # write the outputs
  python scripts/gen-policy-bootstrap.py --check   # compare with the committed outputs (the drift CTest)
"""
from pathlib import Path
import argparse
import hashlib
import re
import sys
import textwrap
import tomllib

ROOT = Path(__file__).resolve().parents[1]
GENERATOR = 'scripts/gen-policy-bootstrap.py'
REGISTER_IDS = 'engine/foundation/crypto/include/crd/crypto/register_ids.hpp'
TRUST_FILE = 'trust-set.toml'
GENERATION_FILE = re.compile(r'^generation-([1-9][0-9]*)\.policy$')
MAX_TRUST_KEYS = 4          # kMaxTrustKeys (policy.hpp)
MAX_POLICY_BYTES = 32768    # kMaxPolicyBytes (policy.hpp)
BYTES_PER_LINE = 16

# output header -> (source directory, namespace, what it is)
OUTPUTS = {
    'engine/foundation/crypto/src/policy_bootstrap.hpp':
        ('assets/crypto-policy', 'crd::crypto::bootstrap', "The build's bootstrap"),
    'tests/foundation/crypto/policy_bootstrap_fixture.hpp':
        ('tests/foundation/crypto/data/policy-bootstrap', 'crd::crypto::test::bootstrap',
         "A test trust set and generation 1 signed by it (made with ceridc policy key, sign and assemble): the "
         "populated shape, compiled and verified on every lane"),
}


class BootstrapError(Exception):
    pass


def algorithm_names(root):
    """Register name -> AlgorithmId enumerator, from the generated register identifiers."""
    text = (root / REGISTER_IDS).read_text(encoding='utf-8')
    names = dict((name, member) for member, name in
                 re.findall(r'case AlgorithmId::(\w+):\n\s+return "([a-z0-9-]+)";', text))
    if not names:
        raise BootstrapError(f'{REGISTER_IDS} names no algorithm')
    return names


def read_trust(path, names):
    """[(enumerator, public key bytes)] of a trust set, every key and value checked."""
    try:
        document = tomllib.loads(path.read_text(encoding='utf-8'))
    except tomllib.TOMLDecodeError as error:
        raise BootstrapError(f'{path.name}: {error}') from error
    if set(document) - {'key'}:
        raise BootstrapError(f'{path.name}: unknown key {sorted(set(document) - {"key"})[0]}')
    keys = document.get('key', [])
    if not isinstance(keys, list) or not 2 <= len(keys) <= MAX_TRUST_KEYS:
        raise BootstrapError(f'{path.name}: a trust set holds 2 to {MAX_TRUST_KEYS} [[key]] tables')
    trust = []
    for index, key in enumerate(keys):
        where = f'{path.name}: key {index + 1}'
        if not isinstance(key, dict) or set(key) != {'algorithm', 'public_key'}:
            raise BootstrapError(f'{where}: exactly algorithm and public_key')
        if key['algorithm'] not in names:
            raise BootstrapError(f'{where}: unknown algorithm {key["algorithm"]!r}')
        if not isinstance(key['public_key'], str) or not re.fullmatch(r'(?:[0-9a-f]{2})+', key['public_key']):
            raise BootstrapError(f'{where}: public_key is not lower-case hex')
        trust.append((names[key['algorithm']], bytes.fromhex(key['public_key'])))
    if len({public for _, public in trust}) != len(trust):
        raise BootstrapError(f'{path.name}: a key appears twice')
    return trust


def newest_generation(directory):
    """(number, bytes) of the highest generation-<N>.policy, or None."""
    found = [(int(match.group(1)), path) for path in directory.iterdir()
             if (match := GENERATION_FILE.match(path.name))] if directory.is_dir() else []
    if not found:
        return None
    number, path = max(found)
    data = path.read_bytes()
    if not 0 < len(data) <= MAX_POLICY_BYTES:
        raise BootstrapError(f'{path.name}: {len(data)} bytes, not 1 to {MAX_POLICY_BYTES}')
    return number, data


def byte_lines(data):
    hexes = [f'0x{byte:02X}U' for byte in data]
    return ['    ' + ', '.join(hexes[at:at + BYTES_PER_LINE]) + ',' for at in range(0, len(hexes), BYTES_PER_LINE)]


def render(root, source, namespace, what):
    directory = root / source
    trust_path = directory / TRUST_FILE
    trust = read_trust(trust_path, algorithm_names(root)) if trust_path.is_file() else []
    generation = newest_generation(directory)
    if generation is not None and not trust:
        raise BootstrapError(f'{source}: generation-{generation[0]}.policy has no {TRUST_FILE} beside it')
    embedded = f'generation-{generation[0]}.policy' if generation else 'no generation'
    def comment(text):
        return ['// ' + line for line in textwrap.wrap(text, 117, break_on_hyphens=False)]

    lines = (['#pragma once', '']
             + comment(f'GENERATED by {GENERATOR} from {source} - DO NOT EDIT. Regenerate with `python {GENERATOR}`; '
                       'the crd-crypto-policy-bootstrap-drift CTest fails on drift.')
             + ['//'] + comment(f'{what} ({len(trust)} trust keys, {embedded}).')
             + ['', '#include <crd/crypto/policy_state.hpp>', '', f'namespace {namespace}', '{'])
    if not trust:
        lines += ['inline constexpr PolicyBootstrap kBootstrap{};']
    else:
        lines += ['// clang-format off']
        for index, (_, public) in enumerate(trust):
            lines += [f'inline constexpr u8 kTrustKey{index}[] = {{'] + byte_lines(public) + ['};', '']
        lines += ['inline constexpr TrustKey kTrust[] = {']
        for index, (member, public) in enumerate(trust):
            digest = [f'0x{byte:02X}U' for byte in hashlib.sha256(public).digest()]
            rows = [', '.join(digest[at:at + 12]) for at in range(0, len(digest), 12)]
            lines += ([f'    {{KeyId{{{{{rows[0]},'] + [f'            {row},' for row in rows[1:-1]]
                      + [f'            {rows[-1]}}}}},', f'     AlgorithmId::{member},'])
            lines += [f'     cont::ConstSpan<u8>(kTrustKey{index})}},']
        lines += ['};', '']
        if generation is None:
            lines += ['inline constexpr PolicyBootstrap kBootstrap{cont::ConstSpan<TrustKey>(kTrust), {}, 0U};']
        else:
            number, data = generation
            lines += ['inline constexpr u8 kGeneration[] = {'] + byte_lines(data) + ['};', '',
                      'inline constexpr PolicyBootstrap kBootstrap{cont::ConstSpan<TrustKey>(kTrust),',
                      f'                                             cont::ConstSpan<u8>(kGeneration), {number}U}};']
        lines += ['// clang-format on']
    lines += [f'}} // namespace {namespace}', '']
    return '\n'.join(lines)


def outputs(root):
    return {header: render(root, *spec) for header, spec in OUTPUTS.items()}


def main(argv=None):
    parser = argparse.ArgumentParser(description='Generate the crypto policy bootstrap headers')
    parser.add_argument('--check', action='store_true', help='compare with the committed outputs instead of writing')
    parser.add_argument('--root', default=str(ROOT), help='repository root (tests use a fixture)')
    args = parser.parse_args(argv)
    root = Path(args.root).resolve()
    try:
        generated = outputs(root)
    except (BootstrapError, OSError, UnicodeDecodeError) as error:
        print(f'FAIL: {error}')
        return 1
    drift = False
    for relative, text in generated.items():
        path = root / relative
        data = text.encode('utf-8')
        if args.check:
            current = path.read_bytes().replace(b'\r\n', b'\n') if path.exists() else None
            if current != data:
                print(f'FAIL: {relative} differs from {GENERATOR}\'s output; run python {GENERATOR}')
                drift = True
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            print(f'wrote {relative}')
    if args.check and not drift:
        print(f'policy bootstrap current: {len(generated)} header(s)')
    return 1 if drift else 0


if __name__ == '__main__':
    sys.exit(main())
