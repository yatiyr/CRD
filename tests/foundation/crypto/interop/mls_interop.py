#!/usr/bin/env python3
"""MLS interoperability of crd-crypto's groups with Cisco's MLSpp (NET.2, decision M4).

Runs crd-mls-peer (crd-crypto over AWS-LC) and crd-mlspp-peer (MLSpp over the pinned OpenSSL 4.0), each in its own
process holding several members, and relays MLS messages between them as a delivery service would: the members of one
group live in both processes, so every message one implementation makes the other processes (the protocol is in
cerid_mls_peer.cpp's header). After every epoch every member reports its epoch, epoch authenticator, an exporter value,
its tree hash and its roster, and all must be equal; every member then sends application data that every other member
reads.

Each of RFC 9420's suites both libraries implement (0x0001, 0x0002, 0x0003 and 0x0007; the post-quantum suites have
no codepoint in common: decision M4) runs one group script twice, Cerid creating it and MLSpp creating it, with
handshake messages as PublicMessages and as PrivateMessages. The roles alternate between the two implementations, so
each operation runs both ways: creating a group and adding members by value with the tree in the Welcome; an Update
and an Add proposed by members and committed by reference by a third with a path, the joiner fetching the tree; an
Update and a Remove committed by reference, the removed member told so; an external PSK and an Add by value committed
without a path; an empty commit; external joins from a GroupInfo carrying the tree and from one without it; and a
member rejoining by external commit with the Remove of its former leaf (resync). Hostile input crosses the
implementations too: every commit is first delivered with one byte flipped (refused, the group unchanged), an
application message delivered again is refused, as is one from the previous epoch, and a key package claiming an
endless lifetime or of another suite is refused by a committer.

Every message is also read off the wire independently of both libraries: the version and wire format of each
MLSMessage (handshake messages in the format asked for, application data always private), each key package's suite,
key lengths, identity, lifetime and GREASE values (RFC 9420 section 13.5: in its capabilities' lists, a leaf extension
of a listed GREASE type and a key package extension), each Welcome's suite and one EncryptedGroupSecrets per new member
addressed by the KeyPackageRef of its key package (RefHash, section 5.2, computed here) with an encapsulation of the
suite's length, and each GroupInfo's GroupContext (suite, group, epoch and the tree hash every member reports), its
ratchet_tree extension exactly when asked for, its external_pub where a joiner needs it and Cerid's GREASE extension.

Usage: mls_interop.py --cerid <crd-mls-peer> --mlspp <crd-mlspp-peer>
"""

import argparse
import hashlib
import os
import subprocess
import sys
import time

# Suite -> (hash, Npk = Nenc): the suites both libraries implement.
SUITES = {1: ('sha256', 32), 2: ('sha256', 65), 3: ('sha256', 32), 7: ('sha384', 97)}
PUBLIC = 1
PRIVATE = 2
WELCOME = 3
GROUP_INFO = 4
KEY_PACKAGE = 5
APPLICATION = 1
COMMIT = 3
RATCHET_TREE = 2
EXTERNAL_PUB = 4
BASIC = 1
TWELVE_WEEKS = 12 * 7 * 24 * 3600
EXPORTER_BYTES = 32


class Failure(Exception):
    pass


class Refused(Failure):
    def __init__(self, kind, request, word):
        super().__init__('%s refused %s: %s' % (kind, request, word))
        self.word = word


def expect(condition, message):
    if not condition:
        raise Failure(message)


def is_grease(value):
    return value & 0x0F0F == 0x0A0A and value != 0xFAFA


# ---- The wire, read independently ----

class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def take(self, count):
        expect(self.at + count <= len(self.data), 'a structure runs past its bytes')
        out = self.data[self.at:self.at + count]
        self.at += count
        return out

    def number(self, size):
        return int.from_bytes(self.take(size), 'big')

    def varint(self):
        """RFC 9420 section 2.1.2: a 2-bit prefix, then 6, 14 or 30 bits, minimally encoded."""
        first = self.data[self.at] if self.at < len(self.data) else None
        expect(first is not None and first >> 6 != 3, 'a variable-length header with the prefix 0b11')
        size = 1 << (first >> 6)
        value = self.number(size) & ((1 << (8 * size - 2)) - 1)
        expect(size == 1 or value >= 1 << (8 * (size // 2) - 2), 'a variable-length header longer than needed')
        return value

    def vector(self):
        return self.take(self.varint())

    def done(self):
        expect(self.at == len(self.data), '%d bytes left over' % (len(self.data) - self.at))


def encode_vector(data):
    size = len(data)
    if size < 1 << 6:
        head = size.to_bytes(1, 'big')
    elif size < 1 << 14:
        head = (size | 0x4000).to_bytes(2, 'big')
    else:
        head = (size | 0x80000000).to_bytes(4, 'big')
    return head + data


def ref_hash(suite, label, value):
    """RefHash(label, value) = Hash(RefHashInput{label, value}) (section 5.2)."""
    return hashlib.new(SUITES[suite][0], encode_vector(label) + encode_vector(value)).digest()


def codepoints(body):
    expect(len(body) % 2 == 0, 'a list of 16-bit codepoints of odd length')
    return [int.from_bytes(body[i:i + 2], 'big') for i in range(0, len(body), 2)]


def extensions(body):
    reader = Reader(body)
    out = []
    while reader.at < len(body):
        out.append((reader.number(2), reader.vector()))
    return out


def header(message, wire_format):
    reader = Reader(message)
    expect(reader.number(2) == 1, 'an MLSMessage of another version')
    found = reader.number(2)
    expect(found == wire_format, 'an MLSMessage of wire format %d, expected %d' % (found, wire_format))
    return reader


def read_leaf(reader):
    leaf = {'encryption_key': reader.vector(), 'signature_key': reader.vector()}
    leaf['credential_type'] = reader.number(2)
    expect(leaf['credential_type'] == BASIC, 'a credential of type %d' % leaf['credential_type'])
    leaf['identity'] = reader.vector()
    for field in ('versions', 'suites', 'extension_types', 'proposals', 'credentials'):
        leaf[field] = codepoints(reader.vector())
    leaf['source'] = reader.number(1)
    if leaf['source'] == 1:
        leaf['lifetime'] = (reader.number(8), reader.number(8))
    elif leaf['source'] == 3:
        leaf['parent_hash'] = reader.vector()
    leaf['extensions'] = extensions(reader.vector())
    leaf['signature'] = reader.vector()
    return leaf


def read_key_package(message):
    reader = header(message, KEY_PACKAGE)
    package = {'version': reader.number(2), 'suite': reader.number(2), 'init_key': reader.vector()}
    package['leaf'] = read_leaf(reader)
    package['extensions'] = extensions(reader.vector())
    package['signature'] = reader.vector()
    reader.done()
    return package


def read_welcome(message):
    reader = header(message, WELCOME)
    suite = reader.number(2)
    secrets = Reader(reader.vector())
    entries = []
    while secrets.at < len(secrets.data):
        entries.append((secrets.vector(), secrets.vector(), secrets.vector()))
    reader.vector()  # the sealed GroupInfo
    reader.done()
    return suite, entries


def read_group_info(message):
    reader = header(message, GROUP_INFO)
    expect(reader.number(2) == 1, 'a GroupContext of another version')
    info = {'suite': reader.number(2), 'group_id': reader.vector(), 'epoch': reader.number(8),
            'tree_hash': reader.vector(), 'confirmed': reader.vector(),
            'context_extensions': extensions(reader.vector())}
    info['extensions'] = extensions(reader.vector())
    info['tag'] = reader.vector()
    info['signer'] = reader.number(4)
    info['signature'] = reader.vector()
    reader.done()
    return info


def forgeries(message, suite):
    """Each authenticated field of a public or private message with one bit flipped, by name. A PublicMessage ends
    with its signature, a commit's confirmation tag and a member's membership tag (section 6.2), each tag Nh bytes
    behind a one-byte length; a PrivateMessage's sender data and content are each sealed (section 6.3)."""
    reader = Reader(message)
    reader.number(4)  # version and wire format
    wire_format = int.from_bytes(message[2:4], 'big')
    reader.vector()   # group_id
    reader.number(8)  # epoch
    hash_bytes = hashlib.new(SUITES[suite][0]).digest_size
    out = []

    def flipped(at):
        forged = bytearray(message)
        forged[at] ^= 0x01
        return bytes(forged)

    if wire_format == PRIVATE:
        reader.number(1)  # content_type
        reader.vector()   # authenticated_data
        start = reader.at
        sender_data = reader.vector()
        expect(len(sender_data) == 28, 'sealed sender data of %d bytes' % len(sender_data))
        out.append(('sender data', flipped(start + 1)))
        reader.vector()
        reader.done()
        out.append(('content', flipped(len(message) - 1)))
        return out
    sender_type = reader.number(1)
    end = len(message)
    if sender_type == 1:
        expect(message[end - hash_bytes - 1] == hash_bytes, 'no membership tag ends a member PublicMessage')
        out.append(('membership tag', flipped(end - 1)))
        end -= hash_bytes + 1
    content_type = read_framing(message)[3]
    if content_type == COMMIT:
        expect(message[end - hash_bytes - 1] == hash_bytes, 'no confirmation tag ends a commit')
        out.append(('confirmation tag', flipped(end - 1)))
        end -= hash_bytes + 1
    out.append(('signature', flipped(end - 1)))
    return out


def read_framing(message):
    """A public or private message's wire format, group, epoch and content type."""
    reader = Reader(message)
    expect(reader.number(2) == 1, 'an MLSMessage of another version')
    wire_format = reader.number(2)
    expect(wire_format in (PUBLIC, PRIVATE), 'a handshake or application message of wire format %d' % wire_format)
    group_id = reader.vector()
    epoch = reader.number(8)
    if wire_format == PUBLIC:
        sender_type = reader.number(1)
        if sender_type in (1, 2):
            reader.number(4)
        reader.vector()  # authenticated_data
    return wire_format, group_id, epoch, reader.number(1)


# ---- The peers ----

class Peer:
    trace = False  # --trace: each request's operation and member, and the reply's start, on standard error

    def __init__(self, kind, path):
        self.kind = kind
        self.process = subprocess.Popen([path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)

    def ask(self, *tokens):
        self.process.stdin.write(' '.join(str(token) for token in tokens) + '\n')
        self.process.stdin.flush()
        reply = self.process.stdout.readline()
        if Peer.trace:
            print('%-5s %s -> %s' % (self.kind, ' '.join(str(token)[:24] for token in tokens[:4]), reply[:120]),
                  file=sys.stderr)
        expect(reply.endswith('\n'), 'the %s peer exited (%s)' % (self.kind, self.process.poll()))
        words = reply.rstrip('\n').split(' ')
        if words[0] == 'error':
            raise Refused(self.kind, ' '.join(str(token) for token in tokens[:2]), ' '.join(words[1:]))
        expect(words[0] == 'ok', 'the %s peer answered %r' % (self.kind, reply[:200]))
        return dict(word.split('=', 1) for word in words[1:])

    def refuses(self, *tokens):
        try:
            self.ask(*tokens)
        except Refused as refusal:
            return refusal.word
        raise Failure('the %s peer accepted %s' % (self.kind, tokens[0]))

    def close(self):
        self.process.stdin.close()
        code = self.process.wait(timeout=60)
        expect(code == 0, 'the %s peer exited %d' % (self.kind, code))


def as_bytes(value):
    return b'' if value == '-' else bytes.fromhex(value)


class Group:
    """One group whose members live in two peer processes, its expected state kept beside them."""

    def __init__(self, peers, suite, handshake):
        self.peers = peers
        self.suite = suite
        self.handshake = handshake
        self.wire = PUBLIC if handshake == 'public' else PRIVATE
        self.group_id = os.urandom(16)
        self.owner = {}       # member name -> its peer
        self.identity = {}    # member name -> its identity
        self.packages = {}    # member name -> its KeyPackage MLSMessage
        self.active = []      # the members in the group, in no particular order
        self.epoch = None
        self.last_info = None  # the GroupInfo of the current epoch, as read off the wire

    def member(self, name, kind, identity=None, lifetime=None, suite=None):
        now = int(time.time())
        not_before, not_after = lifetime or (now - 3600, now - 3600 + TWELVE_WEEKS)
        identity = identity or name.encode()
        suite = suite or self.suite
        reply = self.peers[kind].ask('member', name, identity.hex(), suite, not_before, not_after)
        package = bytes.fromhex(reply['key_package'])
        read = read_key_package(package)
        leaf = read['leaf']
        expect(read['version'] == 1 and read['suite'] == suite, 'a key package of suite %d' % read['suite'])
        expect(len(read['init_key']) == SUITES[suite][1] and len(leaf['encryption_key']) == SUITES[suite][1],
               'key package keys of %d and %d bytes' % (len(read['init_key']), len(leaf['encryption_key'])))
        expect(leaf['identity'] == identity and leaf['source'] == 1 and leaf['lifetime'] == (not_before, not_after),
               'the key package leaf of %s: %s, source %d, lifetime %s' % (name, leaf['identity'], leaf['source'],
                                                                          leaf.get('lifetime')))
        expect(1 in leaf['versions'] and suite in leaf['suites'] and BASIC in leaf['credentials'],
               'capabilities without mls10, the suite or basic credentials: %s' % leaf)
        for field in ('suites', 'extension_types', 'credentials'):
            expect(any(is_grease(value) for value in leaf[field]), '%s lists no GREASE %s' % (name, field))
        expect(any(is_grease(kind_of) and kind_of in leaf['extension_types'] for kind_of, _ in leaf['extensions']),
               '%s has no GREASE leaf extension of a listed type' % name)
        expect(any(is_grease(kind_of) for kind_of, _ in read['extensions']), '%s has no GREASE package extension' % name)
        self.owner[name] = self.peers[kind]
        self.identity[name] = identity
        self.packages[name] = package
        return package

    def ask(self, name, *tokens):
        return self.owner[name].ask(tokens[0], name, *tokens[1:])

    def refuses(self, name, *tokens):
        return self.owner[name].refuses(tokens[0], name, *tokens[1:])

    def create(self, name):
        reply = self.ask(name, 'create', self.group_id.hex())
        expect(reply['epoch'] == '0', 'a new group at epoch %s' % reply['epoch'])
        self.active = [name]
        self.epoch = 0
        self.agree()

    def check_info(self, info, maker, inline, external_pub):
        read = read_group_info(info)
        types = [kind_of for kind_of, _ in read['extensions']]
        expect(read['suite'] == self.suite and read['group_id'] == self.group_id and read['epoch'] == self.epoch,
               'a GroupInfo of suite %d, epoch %d' % (read['suite'], read['epoch']))
        expect((RATCHET_TREE in types) == inline, 'a GroupInfo whose tree is %s, asked %s' % (
            RATCHET_TREE in types, inline))
        if external_pub:
            expect(EXTERNAL_PUB in types, 'a GroupInfo for an external joiner without external_pub')
        if self.owner[maker].kind == 'cerid':
            expect(sum(1 for kind_of in types if is_grease(kind_of)) == 1, 'a Cerid GroupInfo without one GREASE '
                   'extension: %s' % types)
        self.last_info = read

    def deliver(self, sender, message, receivers, content_type, epoch, removed=()):
        """The message to every receiver, each first refusing a forgery of every authenticated field and the group
        unchanged by it; the replies. A member the commit removes is not handed a forged confirmation tag: the path
        secret that keys the new epoch is not encrypted to it, so no implementation can check the tag (section
        12.4.2). An MLSpp receiver is not handed a PrivateMessage of forged content: MLSpp spends the generation
        before the seal authenticates (TP-9, which forged_content_spends reproduces)."""
        wire_format, group_id, at, kind = read_framing(message)
        expect(group_id == self.group_id and at == epoch and kind == content_type,
               'a message of %s at epoch %d of content type %d' % (sender, at, kind))
        replies = {}
        for field, forged in forgeries(message, self.suite):
            for name in receivers:
                mlspp = self.owner[name].kind == 'mlspp'
                if (field == 'confirmation tag' and name in removed) or (mlspp and field == 'content'):
                    continue
                word = self.refuses(name, 'process', forged.hex())
                expect(mlspp or word == 'forged', 'cerid refused a forged %s as %s' % (field, word))
        for name in receivers:
            replies[name] = self.ask(name, 'process', message.hex())
        return wire_format, replies

    def propose(self, name, *tokens):
        message = bytes.fromhex(self.ask(name, 'propose', self.handshake, *tokens)['message'])
        wire_format, replies = self.deliver(name, message, self.others(name), 2, self.epoch)
        expect(wire_format == self.wire, 'a proposal of wire format %d' % wire_format)
        expect(all(reply['type'] == 'proposal' for reply in replies.values()), 'proposal replies %s' % replies)

    def others(self, name):
        return [other for other in self.active if other != name]

    def commit(self, name, path=1, inline=1, adds=(), removes=(), psks=(), joining=(), removed=()):
        """`name` commits the cached proposals and these by value: Adds, Removes and external PSKs. `joining` and
        `removed` name the members the cached proposals add and remove besides."""
        tokens = ['commit', self.handshake, path, inline]
        tokens += ['add=' + self.packages[added].hex() for added in adds]
        tokens += ['remove=%d' % self.leaf_of(member) for member in removes]
        tokens += ['psk=' + psk.hex() for psk in psks]
        reply = self.ask(name, *tokens)
        commit = bytes.fromhex(reply['commit'])
        gone = set(removed) | set(removes)
        wire_format, replies = self.deliver(name, commit, self.others(name), COMMIT, self.epoch, gone)
        expect(wire_format == self.wire, 'a commit of wire format %d' % wire_format)
        self.epoch += 1
        expect(int(reply['epoch']) == self.epoch, 'the committer is at epoch %s' % reply['epoch'])
        for member, answer in replies.items():
            expect(answer['type'] == 'commit' and int(answer['epoch']) == self.epoch and
                   answer['removed'] == ('1' if member in gone else '0'), '%s processed the commit as %s' % (
                       member, answer))
        self.active = [member for member in self.active if member not in gone]
        self.check_info(bytes.fromhex(reply['group_info']), name, inline == 1, False)
        adds = tuple(adds) + tuple(joining)
        if adds:
            welcome = bytes.fromhex(reply['welcome'])
            suite, entries = read_welcome(welcome)
            expect(suite == self.suite and len(entries) == len(adds), 'a Welcome of suite %d for %d members' % (
                suite, len(entries)))
            refs = {ref_hash(self.suite, b'MLS 1.0 KeyPackage Reference', self.packages[added][4:]): added
                    for added in adds}
            for ref, kem_output, _ in entries:
                expect(ref in refs and len(kem_output) == SUITES[self.suite][1],
                       'a Welcome entry for %s with a %d-byte encapsulation' % (ref.hex(), len(kem_output)))
            expect(len({ref for ref, _, _ in entries}) == len(adds), 'a Welcome addressing a member twice')
            tree = '-' if inline else reply['tree']
            for added in adds:
                joined = self.ask(added, 'join', welcome.hex(), tree)
                expect(int(joined['epoch']) == self.epoch, '%s joined at epoch %s' % (added, joined['epoch']))
                self.active.append(added)
        else:
            expect(reply['welcome'] == '-', 'a Welcome for nobody')
        self.agree()

    def external_join(self, joiner, publisher, inline=1, resync=None, psks=()):
        reply = self.ask(publisher, 'group_info', inline)
        info = bytes.fromhex(reply['group_info'])
        self.check_info(info, publisher, inline == 1, True)
        tokens = ['external_join', info.hex(), '-' if inline else reply['tree']]
        if resync is not None:
            tokens.append('resync=%d' % self.leaf_of(resync))
        tokens += ['psk=' + psk.hex() for psk in psks]
        joined = self.ask(joiner, *tokens)
        commit = bytes.fromhex(joined['commit'])
        wire_format, replies = self.deliver(joiner, commit, list(self.active), COMMIT, self.epoch,
                                            () if resync is None else (resync,))
        expect(wire_format == PUBLIC, 'an external commit of wire format %d' % wire_format)
        self.epoch += 1
        expect(int(joined['epoch']) == self.epoch, 'the joiner is at epoch %s' % joined['epoch'])
        for member, answer in replies.items():
            expect(answer['type'] == 'commit' and answer['removed'] == ('1' if member == resync else '0'),
                   '%s processed the external commit as %s' % (member, answer))
        self.active = [member for member in self.active if member != resync] + [joiner]
        self.agree()

    def leaf_of(self, name):
        return int(self.ask(name, 'state')['leaf'])

    def agree(self):
        """Every member at the group's epoch with the same secrets, tree and roster, each in its own leaf."""
        states = {name: self.ask(name, 'state') for name in self.active}
        first = states[self.active[0]]
        roster = dict(entry.split(':') for entry in first['roster'].split(','))
        expect(sorted(roster.values()) == sorted(self.identity[name].hex() for name in self.active),
               'the roster %s for members %s' % (roster, self.active))
        for name, state in states.items():
            for field in ('epoch', 'authenticator', 'exporter', 'tree_hash', 'roster'):
                expect(state[field] == first[field], '%s differs on %s' % (name, field))
            expect(roster.get(state['leaf']) == self.identity[name].hex(), '%s is not in its own leaf' % name)
        expect(int(first['epoch']) == self.epoch and len(as_bytes(first['exporter'])) == EXPORTER_BYTES,
               'the members are at epoch %s' % first['epoch'])
        if self.last_info is not None and self.last_info['epoch'] == self.epoch:
            expect(self.last_info['tree_hash'] == as_bytes(first['tree_hash']), 'the GroupInfo tree hash differs')

    def exchange(self):
        """Every member sends application data every other member reads; one is delivered again (refused)."""
        sent = None
        for name in self.active:
            data = ('%s in epoch %d' % (name, self.epoch)).encode()
            message = bytes.fromhex(self.ask(name, 'protect', data.hex())['message'])
            wire_format, replies = self.deliver(name, message, self.others(name), APPLICATION, self.epoch)
            expect(wire_format == PRIVATE, 'application data of wire format %d' % wire_format)
            for member, reply in replies.items():
                expect(reply['type'] == 'application' and as_bytes(reply['data']) == data,
                       '%s read %s' % (member, reply))
            sent = (name, message)
        if sent is not None and len(self.active) > 1:
            for member in self.others(sent[0]):
                word = self.refuses(member, 'process', sent[1].hex())
                if self.owner[member].kind == 'cerid':
                    expect(word == 'replayed', 'cerid refused a replayed message as %s' % word)
        return sent

    def refuse_stale(self, sent):
        """An application message of the previous epoch, delivered now: every member but its sender refuses it."""
        for member in self.others(sent[0]):
            if member in self.active:
                word = self.refuses(member, 'process', sent[1].hex())
                if self.owner[member].kind == 'cerid':
                    expect(word == 'out-of-window', 'cerid refused a stale message as %s' % word)




def group_script(peers, suite, handshake, creator):
    """The group script, each role played by the implementation the pattern gives it from the creator's."""
    other = 'mlspp' if creator == 'cerid' else 'cerid'
    kinds = {'alice': creator, 'bob': other, 'carol': other, 'dave': creator, 'erin': creator, 'frank': other,
             'gina': creator}
    group = Group(peers, suite, handshake)
    tag = '%s-%s-%d' % (creator, handshake, suite)
    name = {role: '%s-%s' % (role, tag) for role in kinds}
    name['bob again'] = 'bob-again-' + tag

    def member(role, kind=None):
        return group.member(name[role], kind or kinds[role], identity=role.split(' ')[0].encode())

    for role in ('alice', 'bob', 'dave'):
        member(role)
    group.create(name['alice'])
    # Adds by value, the tree in the Welcome.
    group.commit(name['alice'], adds=(name['bob'], name['dave']))
    sent = group.exchange()
    # An Update and an Add proposed, committed by reference with a path by a third member; the joiner fetches the
    # tree. The last epoch's application data is refused.
    member('carol')
    group.propose(name['bob'], 'update')
    group.propose(name['dave'], 'add', group.packages[name['carol']].hex())
    group.commit(name['alice'], inline=0, joining=(name['carol'],))
    group.refuse_stale(sent)
    group.exchange()
    # An Update and a Remove proposed, committed by reference: the removed member is told.
    group.propose(name['alice'], 'update')
    group.propose(name['carol'], 'remove', group.leaf_of(name['dave']))
    group.commit(name['bob'], removed=(name['dave'],))
    group.exchange()
    # An external PSK every member holds and an Add by value, without a path; the joiner fetches the tree.
    member('erin')
    psk_id = os.urandom(16)
    secret = os.urandom(32)
    for holder in group.active + [name['erin']]:
        group.ask(holder, 'psk', psk_id.hex(), secret.hex())
    group.commit(name['carol'], path=0, inline=0, adds=(name['erin'],), psks=(psk_id,))
    group.exchange()
    # An empty commit: the committer's path alone.
    group.commit(name['erin'])
    group.exchange()
    # External joins: from a GroupInfo carrying the tree, and from one without it (the tree fetched beside it).
    member('frank')
    group.external_join(name['frank'], name['alice'])
    group.exchange()
    member('gina')
    group.external_join(name['gina'], name['frank'], inline=0)
    group.exchange()
    # Bob rejoins by an external commit that removes his former leaf (resync), then commits.
    member('bob again', kinds['bob'])
    group.external_join(name['bob again'], name['gina'], resync=name['bob'])
    group.exchange()
    group.commit(name['bob again'], removes=(name['erin'],))
    group.exchange()


def refused_packages(peers, suite):
    """A committer refuses a key package claiming an endless lifetime (Cerid's default policy, decision M8) and one
    of another suite (both libraries)."""
    group = Group(peers, suite, 'public')
    tag = 'refusals-%d' % suite
    other_suite = 3 if suite != 3 else 1
    group.member('creator-c-' + tag, 'cerid', identity=b'creator')
    group.create('creator-c-' + tag)
    endless = group.member('endless-' + tag, 'mlspp', identity=b'endless', lifetime=(0, (1 << 64) - 1))
    word = group.refuses('creator-c-' + tag, 'commit', 'public', 1, 1, 'add=' + endless.hex())
    expect(word == 'expired', 'cerid refused an endless key package as %s' % word)
    foreign = group.member('foreign-m-' + tag, 'mlspp', identity=b'foreign', suite=other_suite)
    word = group.refuses('creator-c-' + tag, 'commit', 'public', 1, 1, 'add=' + foreign.hex())
    expect(word == 'unsupported', 'cerid refused a key package of another suite as %s' % word)
    group.agree()

    group = Group(peers, suite, 'public')
    group.member('creator-m-' + tag, 'mlspp', identity=b'creator')
    group.create('creator-m-' + tag)
    foreign = group.member('foreign-c-' + tag, 'cerid', identity=b'foreign', suite=other_suite)
    group.refuses('creator-m-' + tag, 'commit', 'public', 1, 1, 'add=' + foreign.hex())
    group.agree()


def forged_content_spends(peers, suite):
    """TP-9's reproduction, MLSpp's members alone: a member refuses a PrivateMessage whose content was forged, then
    refuses the genuine message too ("Request for expired key"), because MLSpp spent the generation before the seal
    authenticated. A Cerid member of the same group refuses the forgery and reads the genuine message: Group::process
    spends a generation only once both seals authenticate. When MLSpp stops spending it, this case fails and the
    entry is retired."""
    group = Group(peers, suite, 'private')
    tag = 'tp9-%d' % suite
    for role, kind in (('sender', 'mlspp'), ('reader', 'mlspp'), ('cerid', 'cerid')):
        group.member('%s-%s' % (role, tag), kind, identity=role.encode())
    group.create('sender-' + tag)
    group.commit('sender-' + tag, adds=('reader-' + tag, 'cerid-' + tag))
    data = b'one generation'
    message = bytes.fromhex(group.ask('sender-' + tag, 'protect', data.hex())['message'])
    forged = dict(forgeries(message, suite))['content']
    group.refuses('reader-' + tag, 'process', forged.hex())
    word = group.refuses('reader-' + tag, 'process', message.hex())
    expect('expired_key' in word, 'MLSpp refused the genuine message after the forgery as %s' % word)
    word = group.refuses('cerid-' + tag, 'process', forged.hex())
    expect(word == 'forged', 'cerid refused the forged content as %s' % word)
    reply = group.ask('cerid-' + tag, 'process', message.hex())
    expect(reply['type'] == 'application' and as_bytes(reply['data']) == data, 'cerid read %s' % reply)


def cases(cerid, mlspp):
    def with_peers(body):
        def run():
            peers = {'cerid': Peer('cerid', cerid), 'mlspp': Peer('mlspp', mlspp)}
            try:
                body(peers)
            finally:
                for peer in peers.values():
                    peer.close()
        return run

    for suite in SUITES:
        for creator in ('cerid', 'mlspp'):
            for handshake in ('public', 'private'):
                yield ('suite 0x%04x, %s creates, %s handshake: adds, proposals by reference, a removal, a PSK, an '
                       'empty commit, external joins and a resync agree' % (suite, creator, handshake),
                       with_peers(lambda peers, s=suite, h=handshake, c=creator: group_script(peers, s, h, c)))
        yield ('suite 0x%04x: endless and foreign key packages refused' % suite,
               with_peers(lambda peers, s=suite: refused_packages(peers, s)))
        yield ('suite 0x%04x: TP-9, MLSpp spends a generation on forged content, Cerid does not' % suite,
               with_peers(lambda peers, s=suite: forged_content_spends(peers, s)))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cerid', required=True)
    parser.add_argument('--mlspp', required=True)
    parser.add_argument('--trace', action='store_true', help='print every request and the start of its reply')
    parser.add_argument('--only', help='run the cases whose name holds this text')
    args = parser.parse_args()
    Peer.trace = args.trace
    failures = 0
    count = 0
    for name, case in cases(args.cerid, args.mlspp):
        if args.only and args.only not in name:
            continue
        count += 1
        try:
            case()
            print('ok      %s' % name)
        except Failure as failure:
            failures += 1
            print('FAILED  %s: %s' % (name, failure))
    print('%d of %d cases passed' % (count - failures, count))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
