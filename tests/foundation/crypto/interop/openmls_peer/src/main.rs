//! crd-openmls-peer -- MLS group members through OpenMLS (the version Cargo.lock pins), an independent peer of
//! crd-mls-peer in tests/foundation/crypto/interop/mls_interop.py (NET.2, decision M4;
//! docs/design/network-foundation.md#mls). It speaks crd-mls-peer's protocol (cerid_mls_peer.cpp's header states it)
//! through OpenMLS's public interface over its RustCrypto provider, and links neither crd-crypto nor AWS-LC: the
//! implementations meet on the wire.
//!
//! OpenMLS specifics the protocol leaves to each peer:
//!   members       each member has its own provider, so its own storage: OpenMLS keys a group's state by its id, and
//!                 one process holds several members of a group. A member's capabilities list mls10, the provider's
//!                 suites and basic credentials, each list with a GREASE value (RFC 9420 section 13.5); its leaf and
//!                 key package carry an extension of the listed GREASE type as the request asks (section 10 has a
//!                 package's extensions listed too), which OpenMLS then refuses itself (TP-10,
//!                 docs/third-party-defects.md).
//!   wire formats  a group's outgoing handshake format is its configuration, set before each proposal and commit as
//!                 the request names it; incoming messages may be either (OpenMLS's mixed policies).
//!   commits       the GroupInfo of every commit is created, with the tree when INLINE is 1; the commit is merged at
//!                 once (the driver never hands a member its own commit).
//!   resync        OpenMLS's external commit removes the leaf whose signature key is the joiner's, so a joiner naming
//!                 resync=LEAF takes over the signer and credential of the member of this process at that leaf.
//! A refusal is "error refused:" and OpenMLS's error, spaces replaced; "error" and a word of the peer's own otherwise.

use std::io::{self, BufRead, Read, Write};

use openmls::prelude::*;
use openmls::schedule::{ExternalPsk, PreSharedKeyId, Psk};
use openmls_basic_credential::SignatureKeyPair;
use openmls_rust_crypto::OpenMlsRustCrypto;

const MAX_MEMBERS: usize = 16;
const MAX_LINE: u64 = 1 << 24;
const EXPORT_LABEL: &str = "cerid interop";
const EXPORT_CONTEXT: &[u8] = b"context";
const EXPORT_BYTES: usize = 32;

/// A failed request: its reply after "error ".
struct Refusal(String);

type Outcome<T> = Result<T, Refusal>;

fn refused(error: impl std::fmt::Debug) -> Refusal {
    let detail: String = format!("{error:?}").chars().take(4096).collect();
    Refusal(format!("refused:{}", detail.replace([' ', '\n', '\r'], "_")))
}

fn word(text: &str) -> Refusal {
    Refusal(text.to_owned())
}

struct Member {
    name: String,
    suite: Ciphersuite,
    provider: OpenMlsRustCrypto,
    signer: SignatureKeyPair,
    credential: CredentialWithKey,
    capabilities: Capabilities,
    leaf_extensions: Extensions<LeafNode>,
    lifetime: Lifetime,
    group: Option<MlsGroup>,
    psks: Vec<Vec<u8>>,
}

fn hex(bytes: &[u8]) -> String {
    if bytes.is_empty() {
        return "-".to_owned();
    }
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(DIGITS[usize::from(byte >> 4)] as char);
        out.push(DIGITS[usize::from(byte & 0x0F)] as char);
    }
    out
}

fn bytes(token: &str) -> Outcome<Vec<u8>> {
    if token == "-" {
        return Ok(Vec::new());
    }
    let nibble = |c: u8| match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        _ => None,
    };
    let digits = token.as_bytes();
    if !digits.len().is_multiple_of(2) {
        return Err(word("usage"));
    }
    digits
        .chunks(2)
        .map(|pair| match (nibble(pair[0]), nibble(pair[1])) {
            (Some(high), Some(low)) => Ok(high << 4 | low),
            _ => Err(word("usage")),
        })
        .collect()
}

fn number(token: &str) -> Outcome<u64> {
    if token.is_empty() || !token.bytes().all(|c| c.is_ascii_digit()) {
        return Err(word("usage"));
    }
    token.parse().map_err(|_| word("usage"))
}

fn leaf_index(token: &str) -> Outcome<LeafNodeIndex> {
    let value = u32::try_from(number(token)?).map_err(|_| word("usage"))?;
    Ok(LeafNodeIndex::new(value))
}

fn flag(token: &str) -> Outcome<bool> {
    match token {
        "0" => Ok(false),
        "1" => Ok(true),
        _ => Err(word("usage")),
    }
}

fn message(token: &str) -> Outcome<MlsMessageIn> {
    MlsMessageIn::tls_deserialize_exact_bytes(&bytes(token)?).map_err(refused)
}

fn key_package(token: &str, provider: &OpenMlsRustCrypto) -> Outcome<KeyPackage> {
    match message(token)?.extract() {
        MlsMessageBodyIn::KeyPackage(package) => {
            package.validate(provider.crypto(), ProtocolVersion::Mls10).map_err(refused)
        }
        _ => Err(word("wire-format")),
    }
}

fn tree(token: &str) -> Outcome<Option<RatchetTreeIn>> {
    if token == "-" {
        return Ok(None);
    }
    RatchetTreeIn::tls_deserialize_exact_bytes(&bytes(token)?).map(Some).map_err(refused)
}

fn serialized(value: &impl tls_codec::Serialize) -> Outcome<Vec<u8>> {
    value.tls_serialize_detached().map_err(refused)
}

/// The group configuration whose outgoing handshake messages are PublicMessages (`public`) or PrivateMessages; both
/// are read.
fn join_config(public: bool) -> MlsGroupJoinConfig {
    let policy = if public { MIXED_PLAINTEXT_WIRE_FORMAT_POLICY } else { MIXED_CIPHERTEXT_WIRE_FORMAT_POLICY };
    MlsGroupJoinConfig::builder().wire_format_policy(policy).use_ratchet_tree_extension(true).build()
}

fn wire(token: &str) -> Outcome<bool> {
    match token {
        "public" => Ok(true),
        "private" => Ok(false),
        _ => Err(word("usage")),
    }
}

/// A reference to an external PSK the member holds, with a fresh nonce.
fn external_psk(member: &Member, id: &[u8]) -> Outcome<PreSharedKeyId> {
    if !member.psks.iter().any(|held| held == id) {
        return Err(word("no-psk"));
    }
    let psk = Psk::External(ExternalPsk::new(id.to_vec()));
    PreSharedKeyId::new(member.suite, member.provider.rand(), psk).map_err(refused)
}

struct Peer {
    members: Vec<Member>,
    reply: String,
}

impl Peer {
    fn say(&mut self, key: &str, value: &[u8]) {
        self.reply.push(' ');
        self.reply.push_str(key);
        self.reply.push('=');
        self.reply.push_str(&hex(value));
    }

    fn say_number(&mut self, key: &str, value: u64) {
        self.reply.push_str(&format!(" {key}={value}"));
    }

    fn slot(&self, name: &str) -> Option<usize> {
        self.members.iter().position(|member| member.name == name)
    }

    fn member(&mut self, name: &str) -> Outcome<&mut Member> {
        let slot = self.slot(name).ok_or_else(|| word("no-member"))?;
        Ok(&mut self.members[slot])
    }

    // member NAME IDENTITY SUITE NOT_BEFORE NOT_AFTER [GREASE]
    fn make_member(&mut self, tokens: &[&str]) -> Outcome<()> {
        if self.slot(tokens[1]).is_some() {
            return Err(word("exists"));
        }
        if self.members.len() == MAX_MEMBERS {
            return Err(word("full"));
        }
        let suite = u16::try_from(number(tokens[3])?).map_err(|_| word("usage"))?;
        let suite = Ciphersuite::try_from(suite).map_err(|_| word("unsupported"))?;
        let provider = OpenMlsRustCrypto::default();
        provider.crypto().supports(suite).map_err(|_| word("unsupported"))?;
        let signer = SignatureKeyPair::new(suite.signature_algorithm()).map_err(refused)?;
        let credential = CredentialWithKey {
            credential: BasicCredential::new(bytes(tokens[2])?).into(),
            signature_key: signer.public().into(),
        };
        let grease = openmls::grease::random_grease_value(provider.rand());
        let mut extension_types = Capabilities::for_provider(provider.crypto()).extensions().to_vec();
        extension_types.push(ExtensionType::Grease(grease));
        let capabilities = Capabilities::builder()
            .ciphersuites(provider.crypto().supported_ciphersuites())
            .extensions(extension_types)
            .credentials(vec![CredentialType::Basic])
            .build()
            .with_grease(provider.rand());
        let unknown = |value| Extension::Unknown(value, UnknownExtension(Vec::new()));
        let (leaf_extensions, package_extensions) = if tokens.get(6).map_or(Ok(true), |token| flag(token))? {
            let package = Extensions::single(unknown(grease)).map_err(refused)?;
            (Extensions::single(unknown(grease)).map_err(refused)?, package)
        } else {
            (Extensions::empty(), Extensions::empty())
        };
        let lifetime = Lifetime::init(number(tokens[4])?, number(tokens[5])?);
        let bundle = KeyPackage::builder()
            .leaf_node_capabilities(capabilities.clone())
            .leaf_node_extensions(leaf_extensions.clone())
            .key_package_extensions(package_extensions)
            .key_package_lifetime(lifetime)
            .build(suite, &provider, &signer, credential.clone())
            .map_err(refused)?;
        self.reply.push_str("ok");
        self.say("key_package", &serialized(&MlsMessageOut::from(bundle.key_package().clone()))?);
        self.members.push(Member {
            name: tokens[1].to_owned(),
            suite,
            provider,
            signer,
            credential,
            capabilities,
            leaf_extensions,
            lifetime,
            group: None,
            psks: Vec::new(),
        });
        Ok(())
    }

    // create NAME GROUP_ID
    fn create(&mut self, tokens: &[&str]) -> Outcome<()> {
        let member = self.member(tokens[1])?;
        let group = MlsGroup::builder()
            .with_group_id(GroupId::from_slice(&bytes(tokens[2])?))
            .ciphersuite(member.suite)
            .with_wire_format_policy(MIXED_PLAINTEXT_WIRE_FORMAT_POLICY)
            .use_ratchet_tree_extension(true)
            .with_capabilities(member.capabilities.clone())
            .with_leaf_node_extensions(member.leaf_extensions.clone())
            .map_err(refused)?
            .lifetime(member.lifetime)
            .build(&member.provider, &member.signer, member.credential.clone())
            .map_err(refused)?;
        let epoch = group.epoch().as_u64();
        member.group = Some(group);
        self.reply.push_str("ok");
        self.say_number("epoch", epoch);
        Ok(())
    }

    // join NAME WELCOME TREE
    fn join(&mut self, tokens: &[&str]) -> Outcome<()> {
        let member = self.member(tokens[1])?;
        let welcome = match message(tokens[2])?.extract() {
            MlsMessageBodyIn::Welcome(welcome) => welcome,
            _ => return Err(word("wire-format")),
        };
        let group = StagedWelcome::new_from_welcome(&member.provider, &join_config(true), welcome, tree(tokens[3])?)
            .map_err(refused)?
            .into_group(&member.provider)
            .map_err(refused)?;
        let epoch = group.epoch().as_u64();
        member.group = Some(group);
        self.reply.push_str("ok");
        self.say_number("epoch", epoch);
        Ok(())
    }

    // psk NAME ID SECRET
    fn hold_psk(&mut self, tokens: &[&str]) -> Outcome<()> {
        let member = self.member(tokens[1])?;
        let id = bytes(tokens[2])?;
        PreSharedKeyId::external(id.clone(), Vec::new()).store(&member.provider, &bytes(tokens[3])?).map_err(refused)?;
        if !member.psks.contains(&id) {
            member.psks.push(id);
        }
        self.reply.push_str("ok");
        Ok(())
    }

    // propose NAME WIRE add KEY_PACKAGE | update | remove LEAF | psk ID
    fn propose(&mut self, tokens: &[&str]) -> Outcome<()> {
        let public = wire(tokens[2])?;
        let member = self.member(tokens[1])?;
        let psk = match (tokens[3], tokens.len()) {
            ("psk", 5) => Some(external_psk(member, &bytes(tokens[4])?)?),
            _ => None,
        };
        let added = match (tokens[3], tokens.len()) {
            ("add", 5) => Some(key_package(tokens[4], &member.provider)?),
            _ => None,
        };
        let Member { provider, signer, group, .. } = member;
        let group = group.as_mut().ok_or_else(|| word("no-group"))?;
        group.set_configuration(provider.storage(), &join_config(public)).map_err(refused)?;
        let (message, _) = match (tokens[3], tokens.len()) {
            ("update", 4) => {
                group.propose_self_update(provider, signer, LeafNodeParameters::default()).map_err(refused)?
            }
            ("add", 5) => group.propose_add_member(provider, signer, &added.expect("parsed above")).map_err(refused)?,
            ("remove", 5) => group.propose_remove_member(provider, signer, leaf_index(tokens[4])?).map_err(refused)?,
            ("psk", 5) => group
                .propose_pre_shared_key(provider, signer, psk.expect("parsed above"))
                .map_err(refused)?,
            _ => return Err(word("usage")),
        };
        let message = serialized(&message)?;
        self.reply.push_str("ok");
        self.say("message", &message);
        Ok(())
    }

    // commit NAME WIRE PATH INLINE [add=KEY_PACKAGE] [remove=LEAF] [psk=ID] ...
    fn commit(&mut self, tokens: &[&str]) -> Outcome<()> {
        let public = wire(tokens[2])?;
        let path = flag(tokens[3])?;
        let inline = flag(tokens[4])?;
        let member = self.member(tokens[1])?;
        let mut adds = Vec::new();
        let mut removes = Vec::new();
        let mut psks = Vec::new();
        for token in &tokens[5..] {
            if let Some(package) = token.strip_prefix("add=") {
                adds.push(key_package(package, &member.provider)?);
            } else if let Some(leaf) = token.strip_prefix("remove=") {
                removes.push(leaf_index(leaf)?);
            } else if let Some(id) = token.strip_prefix("psk=") {
                let psk = PreSharedKeyProposal::new(external_psk(member, &bytes(id)?)?);
                psks.push(Proposal::PreSharedKey(Box::new(psk)));
            } else {
                return Err(word("usage"));
            }
        }
        let Member { provider, signer, group, .. } = member;
        let group = group.as_mut().ok_or_else(|| word("no-group"))?;
        group.set_configuration(provider.storage(), &join_config(public)).map_err(refused)?;
        let bundle = group
            .commit_builder()
            .consume_proposal_store(true)
            .force_self_update(path)
            .propose_adds(adds)
            .propose_removals(removes)
            .add_proposals(psks)
            .load_psks(provider.storage())
            .map_err(refused)?
            .create_group_info(true)
            .use_ratchet_tree_extension(inline)
            .build(provider.rand(), provider.crypto(), signer, |_| true)
            .map_err(refused)?
            .stage_commit(provider)
            .map_err(refused)?;
        group.merge_pending_commit(provider).map_err(refused)?;
        let (commit, welcome, group_info) = bundle.into_messages();
        let commit = serialized(&commit)?;
        let welcome = welcome.map(|welcome| serialized(&welcome)).transpose()?.unwrap_or_default();
        let group_info = serialized(&group_info.ok_or_else(|| word("no-group-info"))?)?;
        let tree = serialized(&group.export_ratchet_tree())?;
        let epoch = group.epoch().as_u64();
        self.reply.push_str("ok");
        self.say("commit", &commit);
        self.say("welcome", &welcome);
        self.say("group_info", &group_info);
        self.say("tree", &tree);
        self.say_number("epoch", epoch);
        Ok(())
    }

    // process NAME MESSAGE
    fn process(&mut self, tokens: &[&str]) -> Outcome<()> {
        let message = message(tokens[2])?.try_into_protocol_message().map_err(refused)?;
        let member = self.member(tokens[1])?;
        let Member { provider, group: slot, .. } = member;
        let group = slot.as_mut().ok_or_else(|| word("no-group"))?;
        let epoch = group.epoch().as_u64();
        match group.process_message(provider, message).map_err(refused)?.into_content() {
            ProcessedMessageContent::ApplicationMessage(application) => {
                self.reply.push_str("ok type=application");
                self.say("data", &application.into_bytes());
            }
            ProcessedMessageContent::ProposalMessage(proposal) => {
                group.store_pending_proposal(provider.storage(), *proposal).map_err(refused)?;
                self.reply.push_str("ok type=proposal");
            }
            ProcessedMessageContent::StagedCommitMessage(staged) => {
                let removed = staged.self_removed();
                group.merge_staged_commit(provider, *staged).map_err(refused)?;
                if removed {
                    *slot = None;
                }
                self.reply.push_str("ok type=commit");
                self.say_number("epoch", epoch + 1);
                self.reply.push_str(if removed { " removed=1" } else { " removed=0" });
            }
            _ => return Err(word("content")),
        }
        Ok(())
    }

    // protect NAME DATA
    fn protect(&mut self, tokens: &[&str]) -> Outcome<()> {
        let data = bytes(tokens[2])?;
        let Member { provider, signer, group, .. } = self.member(tokens[1])?;
        let group = group.as_mut().ok_or_else(|| word("no-group"))?;
        let message = serialized(&group.create_message(provider, signer, &data).map_err(refused)?)?;
        self.reply.push_str("ok");
        self.say("message", &message);
        Ok(())
    }

    // group_info NAME INLINE
    fn group_info(&mut self, tokens: &[&str]) -> Outcome<()> {
        let inline = flag(tokens[2])?;
        let Member { provider, signer, group, .. } = self.member(tokens[1])?;
        let group = group.as_ref().ok_or_else(|| word("no-group"))?;
        let info = serialized(&group.export_group_info(provider.crypto(), signer, inline).map_err(refused)?)?;
        let tree = serialized(&group.export_ratchet_tree())?;
        self.reply.push_str("ok");
        self.say("group_info", &info);
        self.say("tree", &tree);
        Ok(())
    }

    /// The signer and credential of the member of this process at `leaf` of the group `group_id`: a resync joiner's.
    fn resync_identity(
        &self,
        group_id: &GroupId,
        leaf: LeafNodeIndex,
    ) -> Outcome<(SignatureKeyPair, CredentialWithKey)> {
        let at_leaf = |group: &MlsGroup| group.group_id() == group_id && group.own_leaf_index() == leaf;
        self.members
            .iter()
            .find(|member| member.group.as_ref().is_some_and(at_leaf))
            .map(|member| (member.signer.clone(), member.credential.clone()))
            .ok_or_else(|| word("no-resync-member"))
    }

    // external_join NAME GROUP_INFO TREE [resync=LEAF] [psk=ID] ...
    fn external_join(&mut self, tokens: &[&str]) -> Outcome<()> {
        let info = match message(tokens[2])?.extract() {
            MlsMessageBodyIn::GroupInfo(info) => info,
            _ => return Err(word("wire-format")),
        };
        let mut resync = None;
        let mut psk_ids = Vec::new();
        for token in &tokens[4..] {
            if let Some(leaf) = token.strip_prefix("resync=") {
                resync = Some(leaf_index(leaf)?);
            } else if let Some(id) = token.strip_prefix("psk=") {
                psk_ids.push(bytes(id)?);
            } else {
                return Err(word("usage"));
            }
        }
        let identity = match resync {
            Some(leaf) => Some(self.resync_identity(info.group_context().group_id(), leaf)?),
            None => None,
        };
        let member = self.member(tokens[1])?;
        if let Some((signer, credential)) = identity {
            member.signer = signer;
            member.credential = credential;
        }
        let psks = psk_ids
            .iter()
            .map(|id| external_psk(member, id).map(PreSharedKeyProposal::new))
            .collect::<Outcome<Vec<_>>>()?;
        let mut builder = MlsGroup::external_commit_builder().with_config(join_config(true));
        if let Some(tree) = tree(tokens[3])? {
            builder = builder.with_ratchet_tree(tree);
        }
        let leaf = LeafNodeParameters::builder()
            .with_capabilities(member.capabilities.clone())
            .with_extensions(member.leaf_extensions.clone())
            .build();
        let (group, bundle) = builder
            .build_group(&member.provider, info, member.credential.clone())
            .map_err(refused)?
            .leaf_node_parameters(leaf)
            .add_psk_proposals(psks)
            .load_psks(member.provider.storage())
            .map_err(refused)?
            .build(member.provider.rand(), member.provider.crypto(), &member.signer, |_| true)
            .map_err(refused)?
            .finalize(&member.provider)
            .map_err(refused)?;
        let commit = serialized(bundle.commit())?;
        let epoch = group.epoch().as_u64();
        member.group = Some(group);
        self.reply.push_str("ok");
        self.say("commit", &commit);
        self.say_number("epoch", epoch);
        Ok(())
    }

    // state NAME
    fn report(&mut self, tokens: &[&str]) -> Outcome<()> {
        let Member { provider, group, .. } = self.member(tokens[1])?;
        let group = group.as_ref().ok_or_else(|| word("no-group"))?;
        let exporter =
            group.export_secret(provider.crypto(), EXPORT_LABEL, EXPORT_CONTEXT, EXPORT_BYTES).map_err(refused)?;
        let mut roster = Vec::new();
        for leaf in group.members() {
            let identity = BasicCredential::try_from(leaf.credential).map_err(refused)?;
            roster.push(format!("{}:{}", leaf.index.u32(), hex(identity.identity())));
        }
        let (epoch, own) = (group.epoch().as_u64(), u64::from(group.own_leaf_index().u32()));
        let authenticator = group.epoch_authenticator().as_slice().to_vec();
        let tree_hash = group.public_group().group_context().tree_hash().to_vec();
        self.reply.push_str("ok");
        self.say_number("epoch", epoch);
        self.say_number("leaf", own);
        self.say("authenticator", &authenticator);
        self.say("exporter", &exporter);
        self.say("tree_hash", &tree_hash);
        self.reply.push_str(" roster=");
        self.reply.push_str(&roster.join(","));
        Ok(())
    }

    fn dispatch(&mut self, tokens: &[&str]) -> Outcome<()> {
        match (tokens[0], tokens.len()) {
            ("member", 6 | 7) => self.make_member(tokens),
            ("create", 3) => self.create(tokens),
            ("join", 4) => self.join(tokens),
            ("psk", 4) => self.hold_psk(tokens),
            ("propose", 4..) => self.propose(tokens),
            ("commit", 5..) => self.commit(tokens),
            ("process", 3) => self.process(tokens),
            ("protect", 3) => self.protect(tokens),
            ("group_info", 3) => self.group_info(tokens),
            ("external_join", 4..) => self.external_join(tokens),
            ("state", 2) => self.report(tokens),
            _ => Err(word("usage")),
        }
    }
}

fn main() {
    let mut peer = Peer { members: Vec::new(), reply: String::new() };
    let stdin = io::stdin();
    let mut input = stdin.lock();
    let mut stdout = io::stdout().lock();
    let mut line = Vec::new();
    loop {
        line.clear();
        let read = match (&mut input).take(MAX_LINE + 1).read_until(b'\n', &mut line) {
            Ok(read) => read,
            Err(_) => std::process::exit(1),
        };
        if read == 0 {
            break;
        }
        if line.last() != Some(&b'\n') {
            std::process::exit(2); // a line longer than the buffer, or input cut short
        }
        line.pop();
        peer.reply.clear();
        let outcome = match std::str::from_utf8(&line) {
            Ok(text) if !text.is_empty() => {
                let tokens: Vec<&str> = text.split(' ').collect();
                if tokens.iter().any(|token| token.is_empty()) {
                    Err(word("usage"))
                } else {
                    peer.dispatch(&tokens)
                }
            }
            _ => Err(word("usage")),
        };
        if let Err(Refusal(refusal)) = outcome {
            peer.reply.clear();
            peer.reply.push_str("error ");
            peer.reply.push_str(&refusal);
        }
        peer.reply.push('\n');
        if stdout.write_all(peer.reply.as_bytes()).and_then(|()| stdout.flush()).is_err() {
            std::process::exit(1);
        }
    }
}
