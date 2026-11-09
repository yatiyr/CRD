//! crd-qmux-crate-peer: a QMux endpoint over the `qmux` crate and rustls, real TCP on the loopback, the counterpart of
//! crd-qmux-peer (crd-net). qmux_interop.py runs one of each and reads both reports.
//!
//! The application is interop_echo.hpp's, byte for byte. This crate does not expose a stream's ID, so a stream a peer
//! opened is known by its pattern: its first bytes name exactly one of the IDs the run can use.
//!
//! The report of each connection, one `name=value` per line from `connection=N`, goes to standard output when it has
//! ended. The certificate is self-signed from the seed; a client pins its key.

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::Duration;

use bytes::Bytes;
use qmux::tls;
use qmux::{Config, Error, RecvStream, SendStream, Session, Version};
use rcgen::{CertificateParams, KeyPair, PKCS_ED25519};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, aws_lc_rs, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, DigitallySignedStruct, ServerConfig, SignatureScheme};
use tokio::net::TcpListener;
use tokio::task::JoinSet;
use web_transport_trait::{RecvStream as _, SendStream as _, Session as _, Stats as _};

const SERVER_NAME: &str = "cerid.test";
const PROTOCOL: &str = "cerid-interop";
const CLOSE_REASON: &str = "cerid-interop done";
const RUN: Duration = Duration::from_secs(60);
const CLOSE_GRACE: Duration = Duration::from_millis(300);
const DATAGRAM_SETTLE: Duration = Duration::from_secs(1);
const DATAGRAM_INDEX_BYTES: usize = 4;
const RESET_STREAM_BYTES: u64 = 4096;
const RESET_CODE: u32 = 7;
const STOP_CODE: u32 = 9;
const HEAD_BYTES: usize = 8;
const ED25519_PKCS8_PREFIX: [u8; 16] = [
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
];
const ED25519_SPKI_PREFIX: [u8; 12] = [0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00];

#[derive(Clone, Default)]
struct Options {
    client: bool,
    closer: String,
    port: u16,
    seed: Vec<u8>,
    group: String,
    cipher: String,
    streams: u64,
    bytes: u64,
    code: u32,
    max_streams: u64,
    uni: u64,
    uni_bytes: u64,
    resets: u64,
    stops: u64,
    datagrams: u64,
    datagram_bytes: usize,
    datagram_limit: u64,
    datagram_probe: bool,
    connections: u64,
    idle_ms: u64,
    hold_ms: u64,
    record_size: u64,
    alpn_version: String,
}

#[derive(Default)]
struct Counters {
    streams: AtomicU64,
    bytes_received: AtomicU64,
    bytes_sent: AtomicU64,
    failed: AtomicBool,
    uni_received: AtomicU64,
    uni_finished: AtomicU64,
    resets_seen: AtomicU64,
    reset_code: AtomicU64,
    stops_seen: AtomicU64,
    stop_code: AtomicU64,
    datagrams_sent: AtomicU64,
    datagrams_received: AtomicU64,
    datagrams_failed: AtomicBool,
    datagram_refused: AtomicBool,
}

impl Counters {
    fn check(&self, good: bool) {
        if !good {
            self.failed.store(true, Ordering::Relaxed);
        }
    }

    fn report(&self, number: u64) {
        let value = |counter: &AtomicU64| counter.load(Ordering::Relaxed);
        println!("connection={number}\nestablished=1\nstreams={}", value(&self.streams));
        println!("bytes_received={}\nbytes_sent={}", value(&self.bytes_received), value(&self.bytes_sent));
        println!("verified={}", u8::from(!self.failed.load(Ordering::Relaxed)));
        println!("uni_received={}\nuni_finished={}", value(&self.uni_received), value(&self.uni_finished));
        println!("resets_seen={}\nreset_code={}", value(&self.resets_seen), value(&self.reset_code));
        println!("stops_seen={}\nstop_code={}", value(&self.stops_seen), value(&self.stop_code));
        println!(
            "datagrams_sent={}\ndatagrams_received={}",
            value(&self.datagrams_sent),
            value(&self.datagrams_received)
        );
        println!("datagrams_verified={}", u8::from(!self.datagrams_failed.load(Ordering::Relaxed)));
        println!("datagrams_refused={}", u8::from(self.datagram_refused.load(Ordering::Relaxed)));
    }
}

fn pattern(tag: u64, stream: u64, offset: u64) -> u8 {
    let at = offset.wrapping_add(stream.wrapping_mul(37)) & 0xFFFF;
    at.wrapping_mul(31).wrapping_add(at >> 8).wrapping_add(tag.wrapping_mul(7)) as u8
}

fn pattern_matches(data: &[u8], tag: u64, stream: u64, from: u64) -> bool {
    data.iter().zip(from..).all(|(byte, offset)| *byte == pattern(tag, stream, offset))
}

/// The one ID in `candidates` whose pattern `head` shows.
fn identify(head: &[u8], tag: u64, candidates: impl Iterator<Item = u64>) -> Option<u64> {
    candidates.into_iter().find(|id| pattern_matches(head, tag, *id, 0))
}

async fn write_pattern(
    send: &mut SendStream,
    tag: u64,
    id: u64,
    total: u64,
    from: u64,
    counters: &Counters,
) -> Result<(), Error> {
    let mut chunk = [0u8; 4096];
    let mut offset = from;
    while offset < total {
        let size = usize::try_from(total - offset).unwrap_or(chunk.len()).min(chunk.len());
        for (at, byte) in chunk[..size].iter_mut().enumerate() {
            *byte = pattern(tag, id, offset + at as u64);
        }
        send.write_all(&chunk[..size]).await?;
        offset += size as u64;
        counters.bytes_sent.fetch_add(size as u64, Ordering::Relaxed);
    }
    Ok(())
}

/// At least `want` bytes from the stream's start, fewer only when it ended first (the second value: it ended).
async fn read_head(recv: &mut RecvStream, want: usize) -> Result<(Vec<u8>, bool), Error> {
    let mut head = Vec::new();
    while head.len() < want {
        match recv.read_chunk(want - head.len()).await? {
            Some(chunk) => head.extend_from_slice(&chunk),
            None => return Ok((head, true)),
        }
    }
    Ok((head, false))
}

async fn read_pattern(recv: &mut RecvStream, tag: u64, id: u64, from: u64, counters: &Counters) -> Result<u64, Error> {
    let mut offset = from;
    while let Some(chunk) = recv.read_chunk(65536).await? {
        counters.check(pattern_matches(&chunk, tag, id, offset));
        offset += chunk.len() as u64;
        counters.bytes_received.fetch_add(chunk.len() as u64, Ordering::Relaxed);
    }
    Ok(offset)
}

async fn client_bidirectional(
    mut send: SendStream,
    mut recv: RecvStream,
    id: u64,
    bytes: u64,
    counters: Arc<Counters>,
) -> Result<(), Error> {
    let writing = async {
        write_pattern(&mut send, 0, id, bytes, 0, &counters).await?;
        send.finish()
    };
    let (written, read) = tokio::join!(writing, read_pattern(&mut recv, 1, id, 0, &counters));
    written?;
    counters.check(read? == bytes);
    counters.streams.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

async fn server_bidirectional(
    mut send: SendStream,
    mut recv: RecvStream,
    options: Arc<Options>,
    counters: Arc<Counters>,
) -> Result<(), Error> {
    let (head, ended) = read_head(&mut recv, HEAD_BYTES).await?;
    let id = identify(&head, 0, (0..options.streams).map(|index| index * 4));
    counters.check(id.is_some());
    let id = id.unwrap_or(0);
    counters.bytes_received.fetch_add(head.len() as u64, Ordering::Relaxed);
    let mut offset = 0;
    let mut chunk = Some(Bytes::from(head));
    let mut ended = ended;
    while let Some(data) = chunk {
        counters.check(pattern_matches(&data, 0, id, offset));
        let echoed = offset + data.len() as u64;
        write_pattern(&mut send, 1, id, echoed, offset, &counters).await?;
        offset = echoed;
        if ended {
            break;
        }
        chunk = recv.read_chunk(65536).await?;
        ended = chunk.is_none();
        if let Some(next) = &chunk {
            counters.bytes_received.fetch_add(next.len() as u64, Ordering::Relaxed);
        }
    }
    send.finish()?;
    send.closed().await?;
    counters.check(offset == options.bytes);
    counters.streams.fetch_add(1, Ordering::Relaxed);
    Ok(())
}

async fn write_unidirectional(
    mut send: SendStream,
    id: u64,
    index: u64,
    options: Arc<Options>,
    counters: Arc<Counters>,
) -> Result<(), Error> {
    let tag = if options.client { 4 } else { 5 };
    if index < options.uni {
        write_pattern(&mut send, tag, id, options.uni_bytes, 0, &counters).await?;
        send.finish()?;
        send.closed().await?;
        counters.uni_finished.fetch_add(1, Ordering::Relaxed);
    } else if index < options.uni + options.resets {
        write_pattern(&mut send, tag, id, RESET_STREAM_BYTES, 0, &counters).await?;
        send.reset(RESET_CODE);
    } else {
        // Writes until the peer's STOP_SENDING.
        let mut offset = 0;
        loop {
            let goal = offset + RESET_STREAM_BYTES;
            match write_pattern(&mut send, tag, id, goal, offset, &counters).await {
                Ok(()) => offset = goal,
                Err(Error::StreamStop(code)) => {
                    counters.stops_seen.fetch_add(1, Ordering::Relaxed);
                    counters.stop_code.store(code.into_inner(), Ordering::Relaxed);
                    return Ok(());
                }
                Err(error) => return Err(error),
            }
        }
    }
    Ok(())
}

fn note_reset(code: u64, counters: &Counters) {
    counters.resets_seen.fetch_add(1, Ordering::Relaxed);
    counters.reset_code.store(code, Ordering::Relaxed);
}

async fn read_unidirectional(
    mut recv: RecvStream,
    options: Arc<Options>,
    counters: Arc<Counters>,
) -> Result<(), Error> {
    let (tag, base) = if options.client { (5, 3) } else { (4, 2) };
    let total = options.uni + options.resets + options.stops;
    let (head, ended) = match read_head(&mut recv, HEAD_BYTES).await {
        Ok(read) => read,
        Err(Error::StreamReset(code)) => {
            note_reset(code.into_inner(), &counters);
            return Ok(());
        }
        Err(error) => return Err(error),
    };
    let id = identify(&head, tag, (0..total).map(|index| (index * 4) + base));
    counters.check(id.is_some());
    let id = id.unwrap_or(base);
    let index = id / 4;
    counters.bytes_received.fetch_add(head.len() as u64, Ordering::Relaxed);
    if index >= options.uni + options.resets {
        recv.stop(STOP_CODE);
        return Ok(());
    }
    let result = if ended {
        Ok(head.len() as u64)
    } else {
        read_pattern(&mut recv, tag, id, head.len() as u64, &counters).await
    };
    match result {
        Ok(size) => {
            counters.check(index < options.uni && size == options.uni_bytes);
            counters.uni_received.fetch_add(1, Ordering::Relaxed);
            Ok(())
        }
        Err(Error::StreamReset(code)) => {
            counters.check(index >= options.uni);
            note_reset(code.into_inner(), &counters);
            Ok(())
        }
        Err(error) => Err(error),
    }
}

fn datagram_of(index: u64, tag: u64, size: usize) -> Bytes {
    let mut payload = vec![0u8; size];
    for (at, byte) in payload.iter_mut().enumerate() {
        *byte = if at < DATAGRAM_INDEX_BYTES { (index >> (at * 8)) as u8 } else { pattern(tag, index, at as u64) };
    }
    Bytes::from(payload)
}

fn check_datagram(payload: &[u8], tag: u64, options: &Options, seen: &mut [bool], counters: &Counters) -> u64 {
    let bytes = payload.iter().take(DATAGRAM_INDEX_BYTES).enumerate();
    let index = bytes.fold(0u64, |value, (at, byte)| value | (u64::from(*byte) << (at * 8)));
    let usable = usize::try_from(index).ok().filter(|at| *at < seen.len());
    let fresh = usable.is_some_and(|at| !std::mem::replace(&mut seen[at], true));
    let mut body = payload.iter().enumerate().skip(DATAGRAM_INDEX_BYTES);
    let intact = body.all(|(at, byte)| *byte == pattern(tag, index, at as u64));
    if payload.len() != options.datagram_bytes || !fresh || !intact {
        counters.datagrams_failed.store(true, Ordering::Relaxed);
    }
    counters.datagrams_received.fetch_add(1, Ordering::Relaxed);
    index
}

async fn run_datagrams(session: Session, options: Arc<Options>, counters: Arc<Counters>) -> Result<(), Error> {
    if options.datagram_probe {
        let over = Bytes::from(vec![0u8; options.datagram_bytes + 1]);
        counters.datagram_refused.store(session.send_datagram(over).is_err(), Ordering::Relaxed);
    }
    if options.client {
        for index in 0..options.datagrams {
            session.send_datagram(datagram_of(index, 2, options.datagram_bytes))?;
            counters.datagrams_sent.fetch_add(1, Ordering::Relaxed);
            tokio::task::yield_now().await;
        }
    }
    let tag = if options.client { 3 } else { 2 };
    let mut seen = vec![false; usize::try_from(options.datagrams).unwrap_or(0)];
    let mut received = 0;
    while received < options.datagrams {
        let Ok(payload) = tokio::time::timeout(DATAGRAM_SETTLE, session.recv_datagram()).await else {
            break;
        };
        let payload = payload?;
        let index = check_datagram(&payload, tag, &options, &mut seen, &counters);
        received += 1;
        if !options.client {
            session.send_datagram(datagram_of(index, 3, options.datagram_bytes))?;
            counters.datagrams_sent.fetch_add(1, Ordering::Relaxed);
        }
    }
    Ok(())
}

/// Everything this end has to do on the connection, every task to its end.
async fn run_application(session: Session, options: Arc<Options>, counters: Arc<Counters>) -> Result<(), Error> {
    let mut tasks = JoinSet::new();
    if options.client {
        for index in 0..options.streams {
            let (send, recv) = session.open_bi().await?;
            tasks.spawn(client_bidirectional(send, recv, index * 4, options.bytes, counters.clone()));
        }
    } else {
        for _ in 0..options.streams {
            let (send, recv) = session.accept_bi().await?;
            tasks.spawn(server_bidirectional(send, recv, options.clone(), counters.clone()));
        }
    }
    let total = options.uni + options.resets + options.stops;
    let base = if options.client { 2 } else { 3 };
    for index in 0..total {
        let send = session.open_uni().await?;
        tasks.spawn(write_unidirectional(send, (index * 4) + base, index, options.clone(), counters.clone()));
    }
    for _ in 0..total {
        let recv = session.accept_uni().await?;
        tasks.spawn(read_unidirectional(recv, options.clone(), counters.clone()));
    }
    if options.datagrams != 0 {
        tasks.spawn(run_datagrams(session.clone(), options.clone(), counters.clone()));
    }
    while let Some(result) = tasks.join_next().await {
        match result {
            Ok(outcome) => outcome?,
            Err(_) => counters.check(false),
        }
    }
    Ok(())
}

fn report_close(error: &Error) {
    let (kind, code, reason) = match error {
        Error::ConnectionClosed { code, reason } => ("application", code.into_inner(), reason.clone()),
        Error::ConnectionReset { code, reason } => ("peer-transport", code.into_inner(), reason.clone()),
        Error::IdleTimeout => ("idle", 0, String::new()),
        Error::Closed => ("closed", 0, String::new()),
        other => ("error", 0, other.to_string()),
    };
    println!("close={kind}\nclose_code={code}\nclose_reason={reason}");
}

async fn run_connection(session: Session, options: Arc<Options>, number: u64) {
    let counters = Arc::new(Counters::default());
    let protocol = session.protocol().unwrap_or("").to_string();
    let closes = options.closer == if options.client { "client" } else { "server" };
    let application = run_application(session.clone(), options.clone(), counters.clone());
    let finished = tokio::time::timeout(RUN, application).await;
    let application_error = match finished {
        Ok(Ok(())) => None,
        Ok(Err(error)) => Some(error.to_string()),
        Err(_) => Some("the application outlasted its deadline".to_string()),
    };
    if closes && application_error.is_none() {
        tokio::time::sleep(Duration::from_millis(options.hold_ms)).await;
        session.close(options.code, CLOSE_REASON);
    }
    let closed = tokio::time::timeout(RUN, session.closed()).await;
    if closes {
        tokio::time::sleep(CLOSE_GRACE).await;
    }
    counters.report(number);
    println!("protocol={protocol}\nrtt={}", u8::from(session.stats().rtt().is_some()));
    match closed {
        Ok(error) if closes && application_error.is_none() => {
            let (code, reason) = match &error {
                Error::ConnectionClosed { code, reason } => (code.into_inner(), reason.clone()),
                other => (0, other.to_string()),
            };
            println!("close=local\nclose_code={code}\nclose_reason={reason}");
        }
        Ok(error) => report_close(&error),
        Err(_) => println!("close=timeout\nclose_code=0\nclose_reason="),
    }
    if let Some(error) = application_error {
        println!("application_error={error}");
    }
}

fn report_refused(number: u64, error: &Error) {
    println!("connection={number}\nestablished=0\nverified=1\nerror={error:?}");
}

#[derive(Debug)]
struct PinnedKey {
    spki: Vec<u8>,
    provider: Arc<CryptoProvider>,
}

impl ServerCertVerifier for PinnedKey {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        if end_entity.as_ref().windows(self.spki.len()).any(|window| window == self.spki) {
            Ok(ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::General("the server's key is not the pinned one".into()))
        }
    }

    fn verify_tls12_signature(
        &self,
        _: &[u8],
        _: &CertificateDer<'_>,
        _: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        Err(rustls::Error::General("TLS 1.2 is not used".into()))
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        signature: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        verify_tls13_signature(message, cert, signature, &self.provider.signature_verification_algorithms)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }
}

fn provider_of(options: &Options) -> Arc<CryptoProvider> {
    use aws_lc_rs::cipher_suite;
    use aws_lc_rs::kx_group;
    let group = match options.group.as_str() {
        "x25519mlkem768" => kx_group::X25519MLKEM768,
        "secp256r1mlkem768" => kx_group::SECP256R1MLKEM768,
        "x25519" => kx_group::X25519,
        other => panic!("unknown group {other}"),
    };
    let cipher = match options.cipher.as_str() {
        "aes128" => cipher_suite::TLS13_AES_128_GCM_SHA256,
        "aes256" => cipher_suite::TLS13_AES_256_GCM_SHA384,
        "chacha20" => cipher_suite::TLS13_CHACHA20_POLY1305_SHA256,
        other => panic!("unknown cipher {other}"),
    };
    Arc::new(CryptoProvider { cipher_suites: vec![cipher], kx_groups: vec![group], ..aws_lc_rs::default_provider() })
}

fn key_of(seed: &[u8]) -> (KeyPair, Vec<u8>) {
    let mut pkcs8 = ED25519_PKCS8_PREFIX.to_vec();
    pkcs8.extend_from_slice(seed);
    let der = PrivatePkcs8KeyDer::from(pkcs8.as_slice());
    (KeyPair::from_pkcs8_der_and_sign_algo(&der, &PKCS_ED25519).expect("an Ed25519 seed"), pkcs8)
}

fn session_config(options: &Options) -> Config {
    let mut config = Config::default();
    config.max_streams_bidi = options.max_streams;
    config.max_streams_uni = (options.uni + options.resets + options.stops).max(1);
    config.max_idle_timeout = options.idle_ms;
    config.max_record_size = options.record_size;
    if options.datagrams != 0 {
        config.max_datagram_frame_size = options.datagram_limit;
    }
    config
}

fn version_of(options: &Options) -> Version {
    if options.alpn_version == "01" { Version::QMux01 } else { Version::QMux02 }
}

async fn run_client(options: Arc<Options>) {
    let provider = provider_of(&options);
    let (key, _) = key_of(&options.seed);
    let mut spki = ED25519_SPKI_PREFIX.to_vec();
    spki.extend_from_slice(key.public_key_raw());
    let verifier = Arc::new(PinnedKey { spki, provider: provider.clone() });
    let config = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    let versions = [version_of(&options)];
    let client = tls::Client::new(Arc::new(config))
        .with_protocol(PROTOCOL, &versions)
        .require_protocol()
        .with_session_config(session_config(&options));
    for number in 1..=options.connections {
        match client.connect(("127.0.0.1", options.port), SERVER_NAME).await {
            Ok(session) => run_connection(session, options.clone(), number).await,
            Err(error) => report_refused(number, &error),
        }
    }
}

async fn run_server(options: Arc<Options>) {
    let provider = provider_of(&options);
    let (key, pkcs8) = key_of(&options.seed);
    let params = CertificateParams::new(vec![SERVER_NAME.to_string()]).expect("a server name");
    let certificate = params.self_signed(&key).expect("a certificate");
    let chain = vec![certificate.der().clone()];
    let mut config = ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .with_no_client_auth()
        .with_single_cert(chain, PrivateKeyDer::Pkcs8(pkcs8.into()))
        .expect("a certificate and its key");
    config.alpn_protocols = vec![format!("{}{PROTOCOL}", version_of(&options).prefix()).into_bytes()];
    config.ticketer = aws_lc_rs::Ticketer::new().expect("a ticketer");
    let server = tls::Server::new(Arc::new(config)).with_session_config(session_config(&options));
    let listener = TcpListener::bind(("127.0.0.1", options.port)).await.expect("a listening socket");
    println!("port={}", listener.local_addr().expect("its address").port());
    for number in 1..=options.connections {
        let (stream, _) = listener.accept().await.expect("a connection");
        match server.accept(stream).await {
            Ok(session) => run_connection(session, options.clone(), number).await,
            Err(error) => report_refused(number, &error),
        }
    }
}

fn parse_hex(text: &str) -> Vec<u8> {
    let byte = |at: usize| u8::from_str_radix(&text[at * 2..at * 2 + 2], 16).expect("a hexadecimal seed");
    (0..text.len() / 2).map(byte).collect()
}

fn parse_options() -> Options {
    let mut options = Options {
        max_streams: 16,
        connections: 1,
        datagram_limit: 1400,
        idle_ms: 30_000,
        record_size: 16_382,
        alpn_version: "02".to_string(),
        ..Options::default()
    };
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    for pair in arguments.chunks(2) {
        let [name, value] = pair else { panic!("{} has no value", pair[0]) };
        let number = || value.parse::<u64>().expect("a number");
        match name.as_str() {
            "--role" => options.client = value == "client",
            "--closer" => options.closer.clone_from(value),
            "--port" => options.port = u16::try_from(number()).expect("a port"),
            "--seed" => options.seed = parse_hex(value),
            "--group" => options.group.clone_from(value),
            "--cipher" => options.cipher.clone_from(value),
            "--streams" => options.streams = number(),
            "--bytes" => options.bytes = number(),
            "--code" => options.code = u32::try_from(number()).expect("a code"),
            "--max-streams" => options.max_streams = number(),
            "--uni" => options.uni = number(),
            "--uni-bytes" => options.uni_bytes = number(),
            "--resets" => options.resets = number(),
            "--stops" => options.stops = number(),
            "--datagrams" => options.datagrams = number(),
            "--datagram-bytes" => options.datagram_bytes = usize::try_from(number()).expect("a size"),
            "--datagram-limit" => options.datagram_limit = number(),
            "--datagram-probe" => options.datagram_probe = value != "0",
            "--connections" => options.connections = number(),
            "--idle-ms" => options.idle_ms = number(),
            "--hold-ms" => options.hold_ms = number(),
            "--record-size" => options.record_size = number(),
            "--alpn-version" => options.alpn_version.clone_from(value),
            other => panic!("unknown option {other}"),
        }
    }
    options
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let options = Arc::new(parse_options());
    if options.client {
        run_client(options).await;
    } else {
        run_server(options).await;
    }
}
