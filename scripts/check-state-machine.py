#!/usr/bin/env python3
"""Protocol state learning: the implementation's state machine against the reviewed one and the model's properties.

NET.2 binds the Tamarin models (tests/foundation/crypto/model) to the code by active automata learning, as protocol
state fuzzing does (de Ruiter and Poll, USENIX Security 2015; Fiterau-Brostean et al. for SSH and DTLS). A system
under learning (a SUL: a program that maps abstract inputs onto real sessions and their outcomes onto abstract
outputs) answers queries; L* for Mealy machines (Shahbaz and Groz) with Rivest and Schapire's counterexample analysis
learns its machine, and the W-method (Chow) with `--extra-states` k proves the hypothesis against every machine of at
most k more states, with seeded random words beside it. The learned machine must then

  - equal the committed, reviewed machine of its system (any change in behaviour shows a shortest input word that
    tells the two apart, for review and a deliberate `--write`), and
  - satisfy every property of its system on every path: the trace properties the models' lemmas state, as monitors
    run over the product of the machine and the monitor, so a violation comes with its shortest witness.

Each answer is checked for determinism (`--recheck` queries asked again outside the cache). Contract:
docs/design/network-foundation.md#state-learning. The crd-crypto-state-learning CTest runs it on every desktop lane.

  python3 scripts/check-state-machine.py --sul build/<preset>/bin/crd-crypto-tls-sul [--system tls-server] [--write]
"""
from collections import deque
from pathlib import Path
import argparse
import itertools
import random
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
STATE_DIR = ROOT / 'tests' / 'foundation' / 'crypto' / 'state'
BATCH = 64  # queries written before their answers are read (each answer stays far below a pipe's buffer)


class Machine:
    """A deterministic, complete Mealy machine: states 0..n-1, state 0 initial."""

    def __init__(self, inputs, transitions):
        self.inputs = tuple(inputs)
        self.transitions = dict(transitions)  # (state, input) -> (output, next state)
        self.states = sorted({state for state, _ in self.transitions})

    def run(self, word, state=0):
        outputs = []
        for symbol in word:
            output, state = self.transitions[(state, symbol)]
            outputs.append(output)
        return tuple(outputs)

    def canonical(self):
        """The same machine with its states numbered in breadth-first order over the inputs as listed."""
        order = {0: 0}
        pending = deque([0])
        while pending:
            state = pending.popleft()
            for symbol in self.inputs:
                target = self.transitions[(state, symbol)][1]
                if target not in order:
                    order[target] = len(order)
                    pending.append(target)
        return Machine(self.inputs, {(order[state], symbol): (output, order[target])
                                     for (state, symbol), (output, target) in self.transitions.items()
                                     if state in order})

    def write(self, path, header):
        lines = [f'# {line}' if line else '#' for line in header]
        lines.append('inputs ' + ' '.join(self.inputs))
        for state in self.states:
            for symbol in self.inputs:
                output, target = self.transitions[(state, symbol)]
                lines.append(f's{state} {symbol} / {output} -> s{target}')
        path.write_bytes(('\n'.join(lines) + '\n').encode('utf-8'))

    @staticmethod
    def read(path):
        inputs, transitions = None, {}
        for number, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
            if not line.strip() or line.startswith('#'):
                continue
            parts = line.split()
            if parts[0] == 'inputs':
                inputs = parts[1:]
            elif len(parts) == 6 and parts[2] == '/' and parts[4] == '->':
                transitions[(int(parts[0][1:]), parts[1])] = (parts[3], int(parts[5][1:]))
            else:
                raise ValueError(f'{path}:{number}: not a transition: {line}')
        if inputs is None:
            raise ValueError(f'{path}: no inputs line')
        machine = Machine(inputs, transitions)
        missing = [(state, symbol) for state in machine.states for symbol in inputs
                   if (state, symbol) not in transitions]
        targets = {target for _, target in transitions.values()}
        if missing or not targets <= set(machine.states) or 0 not in machine.states:
            raise ValueError(f'{path}: an incomplete machine ({len(missing)} transitions missing)')
        return machine


class Cache:
    """Every answered word and, through the trie, every prefix of one."""

    def __init__(self):
        self.root = {}

    def get(self, word):
        node, outputs = self.root, []
        for symbol in word:
            if symbol not in node:
                return None
            output, node = node[symbol]
            outputs.append(output)
        return tuple(outputs)

    def put(self, word, outputs):
        node = self.root
        for symbol, output in zip(word, outputs):
            known = node.get(symbol)
            if known is None:
                node[symbol] = known = (output, {})
            elif known[0] != output:
                raise RuntimeError(f'nondeterministic: {" ".join(word)} answered {output} where it answered {known[0]}')
            node = known[1]


class Oracle:
    """Membership queries through the cache; `answer` is a function from a list of words to their outputs."""

    def __init__(self, answer):
        self.answer = answer
        self.cache = Cache()
        self.queries = 0
        self.symbols = 0

    def ask(self, words):
        missing = list(dict.fromkeys(word for word in words if word and self.cache.get(word) is None))
        for start in range(0, len(missing), BATCH):
            batch = missing[start:start + BATCH]
            for word, outputs in zip(batch, self.answer(batch)):
                if len(outputs) != len(word):
                    raise RuntimeError(f'{" ".join(word)}: {len(outputs)} outputs for {len(word)} inputs')
                self.cache.put(word, outputs)
            self.queries += len(batch)
            self.symbols += sum(len(word) for word in batch)
        return [self.cache.get(word) if word else () for word in words]

    def query(self, word):
        return self.ask([word])[0]


class Sul:
    """A SUL program over its standard streams: one query a line, one answer a line."""

    def __init__(self, command):
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True,
                                        encoding='utf-8', bufsize=1)

    def answer(self, words):
        self.process.stdin.write(''.join(' '.join(word) + '\n' for word in words))
        self.process.stdin.flush()
        answers = []
        for word in words:
            line = self.process.stdout.readline()
            if not line:
                raise RuntimeError(f'the SUL ended (exit {self.process.wait()}) on: {" ".join(word)}')
            answers.append(tuple(line.split()))
        return answers

    def close(self):
        self.process.stdin.close()
        code = self.process.wait()
        if code != 0:
            raise RuntimeError(f'the SUL exited {code}')


def learn(oracle, inputs, extra_states=1, random_words=1000, seed=1):
    """L* with Rivest-Schapire counterexamples; the hypothesis once the W-method and the random words find none."""
    access = [()]
    suffixes = [(symbol,) for symbol in inputs]
    generator = random.Random(seed)

    def rows(words):
        queries = [word + suffix for word in words for suffix in suffixes]
        answers = iter(oracle.ask(queries))
        return [tuple(next(answers)[len(word):] for suffix in suffixes) for word in words]

    while True:
        # Close the table: every one-input extension of an access word has the row of an access word.
        while True:
            known = rows(access)
            index = {row: i for i, row in enumerate(known)}
            extensions = [word + (symbol,) for word in access for symbol in inputs]
            fresh = [word for word, row in zip(extensions, rows(extensions)) if row not in index]
            if not fresh:
                break
            seen = set()
            for word, row in zip(fresh, rows(fresh)):
                if row not in seen and row not in index:
                    seen.add(row)
                    access.append(word)
        extension_rows = dict(zip(extensions, rows(extensions)))
        transitions = {}
        for (i, word), symbol in itertools.product(enumerate(access), inputs):
            extension = word + (symbol,)
            transitions[(i, symbol)] = (oracle.query(extension)[-1], index[extension_rows[extension]])
        hypothesis = Machine(inputs, transitions)
        counterexample = find_counterexample(oracle, hypothesis, access, suffixes, extra_states, random_words,
                                             generator)
        if counterexample is None:
            return hypothesis.canonical()
        suffix = distinguishing_suffix(oracle, hypothesis, access, counterexample)
        if suffix in suffixes:
            raise RuntimeError(f'counterexample {" ".join(counterexample)} adds no suffix: the SUL is not a '
                               'deterministic finite machine over these inputs')
        suffixes.append(suffix)


def find_counterexample(oracle, hypothesis, access, suffixes, extra_states, random_words, generator):
    """The W-method's words (transition cover, up to `extra_states` inputs more, the characterizing suffixes), then
    seeded random words; the shortest mismatching prefix of the first word the SUL answers differently."""
    middles = [()] + [word for length in range(1, extra_states + 1)
                      for word in itertools.product(hypothesis.inputs, repeat=length)]
    cover = list(access) + [word + (symbol,) for word in access for symbol in hypothesis.inputs]
    words = [prefix + middle + suffix for prefix in cover for middle in middles for suffix in suffixes]
    longest = 2 * len(access) + 2
    words += [tuple(generator.choice(hypothesis.inputs) for _ in range(generator.randint(1, longest)))
              for _ in range(random_words)]
    for start in range(0, len(words), 4096):
        chunk = words[start:start + 4096]
        for word, outputs in zip(chunk, oracle.ask(chunk)):
            expected = hypothesis.run(word)
            if outputs != expected:
                first = next(i for i, (a, b) in enumerate(zip(outputs, expected)) if a != b)
                return word[:first + 1]
    return None


def distinguishing_suffix(oracle, hypothesis, access, word):
    """Rivest and Schapire: the index where replacing the prefix by its state's access word stops changing the
    answer; the rest of the word after that input tells two access words apart that the table did not."""

    def agrees(i):
        state = 0
        for symbol in word[:i]:
            state = hypothesis.transitions[(state, symbol)][1]
        prefix = access[state]
        return oracle.query(prefix + word[i:])[len(prefix):] == hypothesis.run(word[i:], state)

    low, high = 0, len(word)  # agrees(low) is false (a counterexample), agrees(high) is true (nothing left)
    while high - low > 1:
        middle = (low + high) // 2
        if agrees(middle):
            high = middle
        else:
            low = middle
    return word[low + 1:]


def difference(a, b):
    """The shortest input word on which two machines answer differently; None when they are equivalent."""
    if a.inputs != b.inputs:
        raise ValueError('machines over different inputs')
    seen = {(0, 0): ()}
    pending = deque([(0, 0)])
    while pending:
        pair = pending.popleft()
        for symbol in a.inputs:
            (out_a, next_a), (out_b, next_b) = a.transitions[(pair[0], symbol)], b.transitions[(pair[1], symbol)]
            word = seen[pair] + (symbol,)
            if out_a != out_b:
                return word
            if (next_a, next_b) not in seen:
                seen[(next_a, next_b)] = word
                pending.append((next_a, next_b))
    return None


def violations(machine, properties):
    """Every property's shortest violating word: a breadth-first search of the machine times each monitor. A monitor
    is (name, initial state, step): step(state, input, output) is the next monitor state (hashable, never a string),
    or a string naming the violation."""
    found = []
    for name, initial, step in properties:
        seen = {(0, initial): ()}
        pending = deque([(0, initial)])
        failure = None
        while pending and failure is None:
            state, monitor = pending.popleft()
            for symbol in machine.inputs:
                output, target = machine.transitions[(state, symbol)]
                verdict = step(monitor, symbol, output)
                word = seen[(state, monitor)] + (symbol,)
                if isinstance(verdict, str):
                    failure = (name, verdict, word, machine.run(word))
                    break
                if (target, verdict) not in seen:
                    seen[(target, verdict)] = word
                    pending.append((target, verdict))
        if failure is not None:
            found.append(failure)
    return found


# The TLS sessions' outputs (crd-crypto-tls-sul): result.subject.peer and the flags d D k K u U.
def tls_parts(output):
    head, _, rest = output.partition('.')
    subject, _, peer = rest.partition('.')
    letters = ''.join(c for c in peer if c in 'dDkKuU')
    return head, subject, peer.rstrip('dDkKuU'), letters


def tls_properties(max_updates):
    """The tls_handshake.spthy and tls_key_update.spthy lemmas as monitors over one end's sessions, plus RFC 9846's
    key update rules (sections 4.7.3 and 5.5) that the models assume."""

    def desynced(monitor, symbol, output):
        result = tls_parts(output)[0]
        return monitor or (symbol in ('drop', 'replay') and result != 'empty')

    def records_authentic(monitor, symbol, output):
        # record_authentic, update_authentic: data and KeyUpdates are accepted only as sent, in order: from a
        # delivery, never from a replay or after the peer's records were dropped or replayed.
        _, subject, _, flags = tls_parts(output)
        if ('d' in flags or 'k' in flags) and (monitor or symbol != 'deliver'):
            return 'accepted a record that was not delivered in order'
        if 'd' in flags and subject[0] in 'IH':
            return 'read application data before the handshake ended'
        return desynced(monitor, symbol, output)

    def handshake_in_order(monitor, symbol, output):
        # handshake_completes and the agreement lemmas: an end is established only by the peer's flights, delivered
        # in order and unaltered.
        established, was = monitor
        subject = tls_parts(output)[1]
        if subject == 'E' and not was and (symbol != 'deliver' or established):
            return 'established without the peer flights in order'
        return (desynced(established, symbol, output), subject[0] in 'ECF')

    def terminal(monitor, symbol, output):
        # A failed session stays failed with its alert, and a closed one never reopens or reads.
        _, subject, _, flags = tls_parts(output)
        if monitor is not None:
            if monitor[0].startswith('F') and subject != monitor[0]:
                return f'left the failed state {monitor[0]} for {subject}'
            if monitor[0] == 'C' and (subject[0] in 'IHE' or 'd' in flags or 'k' in flags):
                return 'a closed session reopened or read'
        return (subject,) if subject[0] in 'CF' else None

    def update_bound(monitor, symbol, output):
        # max_key_updates (decision S7): an update of the session's own that would reach the bound closes the session
        # instead, the last update kept for the peer's request, which the provider answers unasked; an answer that
        # reaches the bound closes the session after it. The count never passes the bound.
        result, subject, _, flags = tls_parts(output)
        own = symbol in ('s_update', 's_update_req') and result == 'ok'
        count = monitor + ('u' in flags) + own
        if count > max_updates or (own and count == max_updates):
            return f'{count} key updates under a bound of {max_updates}'
        if count == max_updates and subject[0] not in 'CF':
            return 'open after the update that reached its bound'
        return count

    def requests_answered(monitor, symbol, output):
        # updates_complete: a KeyUpdate that requests an update is answered by one (RFC 9846 section 4.7.3), unless
        # the bound closed the session; nothing else makes an end update by itself; and an end asks again only once
        # its earlier request was answered (the answer delivered).
        queued, outstanding = monitor
        result, subject, _, flags = tls_parts(output)
        if 'u' in flags and not (symbol == 'deliver' and queued and 'k' in flags):
            return 'updated its key without a request'
        if symbol == 'deliver' and queued and 'k' in flags and 'u' not in flags and subject != 'C':
            return 'left a requested key update unanswered'
        if 'U' in flags and (symbol != 's_update_req' or outstanding):
            return 'asked for a key update while one was outstanding'
        if symbol == 'p_update_req' and result == 'ok':
            queued = True
        elif symbol in ('deliver', 'drop') and result != 'empty':
            queued = False
        if symbol == 's_update_req' and 'U' in flags:
            outstanding = True
        elif symbol == 'deliver' and 'k' in flags:
            outstanding = False
        return (queued, outstanding)

    return [('records_authentic', False, records_authentic), ('handshake_in_order', (False, False), handshake_in_order),
            ('terminal', None, terminal), ('update_bound', 0, update_bound),
            ('requests_answered', (False, False), requests_answered)]


TLS_MAX_UPDATES = 3  # crd-crypto-tls-sul's kSubjectMaxUpdates
SYSTEMS = {
    'tls-server': (('--role', 'server'), 'tls_server.mealy', tls_properties(TLS_MAX_UPDATES)),
    'tls-client': (('--role', 'client'), 'tls_client.mealy', tls_properties(TLS_MAX_UPDATES)),
}


def header(name, machine, oracle, seconds, extra_states):
    return [f'{name}: the machine crd-crypto-tls-sul {" ".join(SYSTEMS[name][0])} learned and the review accepted',
            '(scripts/check-state-machine.py; docs/design/network-foundation.md#state-learning). Regenerate with',
            '--write only after reviewing the change the check reports.',
            f'{len(machine.states)} states, {len(machine.inputs)} inputs; W-method with {extra_states} extra state(s); '
            f'{oracle.queries} queries, {oracle.symbols} inputs, {seconds:.0f} s on the writing host.']


def check_system(name, sul_path, extra_states, random_words, recheck, write):
    arguments, machine_file, properties = SYSTEMS[name]
    start = time.monotonic()
    sul = Sul([str(sul_path), *arguments])
    alphabet = subprocess.run([str(sul_path), '--alphabet'], stdout=subprocess.PIPE, text=True, check=True)
    inputs = tuple(alphabet.stdout.split())
    oracle = Oracle(sul.answer)
    failures = []
    try:
        learned = learn(oracle, inputs, extra_states, random_words)
        # Determinism: a sample of answered words asked again, outside the cache.
        leaves = sorted(words_of(oracle.cache))
        sample = random.Random(2).sample(leaves, min(recheck, len(leaves)))
        for word, answer in zip(sample, sul.answer(sample)):
            if answer != oracle.cache.get(word):
                failures.append(f'{name}: nondeterministic: {" ".join(word)}')
    finally:
        sul.close()
    seconds = time.monotonic() - start
    print(f'[state-learning] {name}: {len(learned.states)} states, {oracle.queries} queries, '
          f'{oracle.symbols} inputs, {seconds:.0f} s')
    for prop, verdict, word, outputs in violations(learned, properties):
        failures.append(f'{name}: property {prop}: {verdict}\n  inputs:  {" ".join(word)}\n  outputs: '
                        f'{" ".join(outputs)}')
    path = STATE_DIR / machine_file
    if write:
        learned.write(path, header(name, learned, oracle, seconds, extra_states))
        print(f'[state-learning] wrote {path.relative_to(ROOT).as_posix()}')
    elif not path.is_file():
        failures.append(f'{name}: no reviewed machine {path.relative_to(ROOT).as_posix()} (--write one)')
    else:
        reviewed = Machine.read(path)
        if reviewed.inputs != learned.inputs:
            failures.append(f'{name}: {machine_file} has the inputs {" ".join(reviewed.inputs)}, the SUL '
                            f'{" ".join(learned.inputs)}')
        elif (word := difference(learned, reviewed)) is not None:
            failures.append(f'{name}: the learned machine differs from {machine_file}\n  inputs:   {" ".join(word)}'
                            f'\n  learned:  {" ".join(learned.run(word))}'
                            f'\n  reviewed: {" ".join(reviewed.run(word))}')
    return failures


def words_of(cache):
    """The longest words the cache holds (its leaves)."""
    pending = [((), cache.root)]
    while pending:
        word, node = pending.pop()
        if not node and word:
            yield word
        for symbol, (_, child) in node.items():
            pending.append((word + (symbol,), child))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--sul', required=True, type=Path, help='crd-crypto-tls-sul')
    parser.add_argument('--system', action='append', choices=sorted(SYSTEMS), help='default: every system')
    parser.add_argument('--extra-states', type=int, default=1, help="the W-method's bound beyond the hypothesis")
    parser.add_argument('--random-words', type=int, default=2000, help='seeded random words beside the W-method')
    parser.add_argument('--recheck', type=int, default=200, help='answers asked again for determinism')
    parser.add_argument('--write', action='store_true', help='write the learned machines (after review)')
    args = parser.parse_args(argv)
    failures = []
    for name in args.system or sorted(SYSTEMS):
        failures += check_system(name, args.sul, args.extra_states, args.random_words, args.recheck, args.write)
    for failure in failures:
        print(f'[state-learning] FAIL {failure}')
    print('[state-learning] ' + ('FAIL' if failures else 'PASS'))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
