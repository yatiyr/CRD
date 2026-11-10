//! crd-qmux-crate-peer: a QMux endpoint over the `qmux` crate and rustls, real TCP on the loopback, the counterpart of
//! crd-qmux-peer (crd-net). qmux_interop.py runs one of each and reads both reports.
//!
//! `--transport` picks what carries QMux: `tls` (the default), `ws` (WebSocket over HTTP/1.1 inside TLS, RFC 6455),
//! `h2` (WebSocket over HTTP/2 inside TLS with ALPN `h2`, RFC 8441: the `h2` crate's extended CONNECT, tokio-tungstenite
//! on the stream, `connections` tunnels at once on one connection), `h2c` (the same with prior knowledge over TCP) or
//! `h3` (the same over HTTP/3, RFC 9220: the `h3` crate over Quinn, real UDP).
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
use h2::ext::Protocol;
use h3::ConnectionState as _;
use qmux::tls;
use qmux::ws;
use qmux::ws::tokio_tungstenite::tungstenite::client::IntoClientRequest;
use qmux::ws::tokio_tungstenite::tungstenite::http::header::{SEC_WEBSOCKET_PROTOCOL, SEC_WEBSOCKET_VERSION};
use qmux::ws::tokio_tungstenite::tungstenite::http::{HeaderValue, Method, Request, Response, StatusCode};
use qmux::ws::tokio_tungstenite::tungstenite::protocol::Role;
use qmux::ws::tokio_tungstenite::{WebSocketStream, client_async};
use qmux::{Config, Error, RecvStream, SendStream, Session, Version};
use quinn::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use rcgen::{CertificateParams, KeyPair, PKCS_ED25519};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, aws_lc_rs, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, DigitallySignedStruct, ServerConfig, SignatureScheme};
use tokio::io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt, DuplexStream};
use tokio::net::{TcpListener, TcpStream};
use tokio::task::JoinSet;
use tokio_rustls::{TlsAcceptor, TlsConnector};
use web_transport_trait::{RecvStream as _, SendStream as _, Session as _, Stats as _};

const SERVER_NAME: &str = "cerid.test";
const PROTOCOL: &str = "cerid-interop";
const WEBSOCKET_PATH: &str = "/interop";
const HTTP_11: &[u8] = b"http/1.1";
const H2_ALPN: &[u8] = b"h2";
const CLOSE_REASON: &str = "cerid-interop done";
const RUN: Duration = Duration::from_secs(60);
const CLOSE_GRACE: Duration = Duration::from_millis(300);
const DATAGRAM_SETTLE: Duration = Duration::from_secs(1);
const DATAGRAM_INDEX_BYTES: usize = 4;
const RESET_STREAM_BYTES: u64 = 4096;
const RESET_CODE: u32 = 7;
const STOP_CODE: u32 = 9;
const HEAD_BYTES: usize = 8;
const WEBSOCKET: &str = "websocket";
const WEBSOCKET_VERSION: &str = "13";
const BRIDGE_BYTES: usize = 64 << 10;
const SETTINGS_POLL: Duration = Duration::from_millis(1);
// hyper's HTTP/2 windows, which a production endpoint on this crate runs with (the crate's own default is RFC 9113's
// 65,535 bytes for both).
const H2_STREAM_WINDOW: u32 = 2 << 20;
const H2_CONNECTION_WINDOW: u32 = 5 << 20;
const H3_ALPN: &[u8] = b"h3";
const H3_NO_ERROR: u32 = 0x100;
/// How long an HTTP/3 end whose tunnels are done waits for the other: the server for its client to close, the client
/// for the server's GOAWAY.
const H3_LINGER: Duration = Duration::from_secs(2);
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
    transport: String,
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

fn report_refused(number: u64, error: &dyn std::fmt::Debug) {
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

/// TLS 1.3 with the run's group and cipher suite, the server's key pinned.
fn client_tls(options: &Options) -> ClientConfig {
    let provider = provider_of(options);
    let (key, _) = key_of(&options.seed);
    let mut spki = ED25519_SPKI_PREFIX.to_vec();
    spki.extend_from_slice(key.public_key_raw());
    let verifier = Arc::new(PinnedKey { spki, provider: provider.clone() });
    ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth()
}

/// TLS 1.3 with the run's group and cipher suite, and the certificate self-signed from the seed.
fn server_tls(options: &Options) -> ServerConfig {
    let (key, pkcs8) = key_of(&options.seed);
    let params = CertificateParams::new(vec![SERVER_NAME.to_string()]).expect("a server name");
    let certificate = params.self_signed(&key).expect("a certificate");
    let chain = vec![certificate.der().clone()];
    ServerConfig::builder_with_provider(provider_of(options))
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .with_no_client_auth()
        .with_single_cert(chain, PrivateKeyDer::Pkcs8(pkcs8.into()))
        .expect("a certificate and its key")
}

async fn run_client(options: Arc<Options>) {
    let config = client_tls(&options);
    if options.transport == "ws" {
        run_websocket_client(config, options).await;
        return;
    }
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

/// The opening handshake by hand, over a TLS session of this peer's own: the crate's `wss` feature would bring the
/// platform's certificate store in, and the pin is what a test needs.
async fn run_websocket_client(mut config: ClientConfig, options: Arc<Options>) {
    config.alpn_protocols = vec![HTTP_11.to_vec()];
    let connector = TlsConnector::from(Arc::new(config));
    let offered = offered_protocol(&options);
    for number in 1..=options.connections {
        let tcp = TcpStream::connect(("127.0.0.1", options.port)).await.expect("a connection");
        let name = ServerName::try_from(SERVER_NAME).expect("a server name");
        let stream = connector.connect(name, tcp).await.expect("a TLS session");
        let mut request = format!("wss://{SERVER_NAME}{WEBSOCKET_PATH}").into_client_request().expect("a request");
        let protocol = HeaderValue::from_str(&offered).expect("a subprotocol");
        request.headers_mut().insert(SEC_WEBSOCKET_PROTOCOL, protocol);
        match client_async(request, stream).await {
            Ok((socket, response)) => {
                let agreed = response.headers().get(SEC_WEBSOCKET_PROTOCOL).expect("the server's choice");
                let session = ws::Upgraded::new(socket)
                    .with_alpn(agreed.to_str().expect("a token"))
                    .with_session_config(session_config(&options))
                    .connect();
                run_connection(session, options.clone(), number).await;
            }
            Err(error) => report_refused(number, &Error::from(error)),
        }
    }
}

async fn run_websocket_server(mut config: ServerConfig, options: Arc<Options>) {
    config.alpn_protocols = vec![HTTP_11.to_vec()];
    let acceptor = TlsAcceptor::from(Arc::new(config));
    let versions = [version_of(&options)];
    let server = ws::Server::new()
        .with_protocol(PROTOCOL, &versions)
        .require_protocol()
        .with_session_config(session_config(&options));
    let listener = TcpListener::bind(("127.0.0.1", options.port)).await.expect("a listening socket");
    println!("port={}", listener.local_addr().expect("its address").port());
    for number in 1..=options.connections {
        let (stream, _) = listener.accept().await.expect("a connection");
        let stream = acceptor.accept(stream).await.expect("a TLS session");
        match server.accept(stream).await {
            Ok(session) => run_connection(session, options.clone(), number).await,
            Err(error) => report_refused(number, &error),
        }
    }
}

async fn run_server(options: Arc<Options>) {
    let mut config = server_tls(&options);
    if options.transport == "ws" {
        run_websocket_server(config, options).await;
        return;
    }
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

/// The tunnel's byte stream for tokio-tungstenite: a pipe whose far end two tasks join to the HTTP/2 stream, handing
/// the receive window back as bytes are taken and writing as the send window allows. The pipe's end is END_STREAM, and
/// the stream's end or reset is the pipe's.
fn bridge(mut send: h2::SendStream<Bytes>, mut recv: h2::RecvStream) -> DuplexStream {
    let (near, far) = tokio::io::duplex(BRIDGE_BYTES);
    let (mut far_read, mut far_write) = tokio::io::split(far);
    tokio::spawn(async move {
        while let Some(Ok(chunk)) = recv.data().await {
            let _ = recv.flow_control().release_capacity(chunk.len());
            if far_write.write_all(&chunk).await.is_err() {
                return;
            }
        }
        let _ = far_write.shutdown().await;
    });
    tokio::spawn(async move {
        let mut buffer = vec![0u8; BRIDGE_BYTES];
        loop {
            match far_read.read(&mut buffer).await {
                Ok(0) | Err(_) => {
                    let _ = send.send_data(Bytes::new(), true);
                    return;
                }
                Ok(count) => {
                    if send_all(&mut send, Bytes::copy_from_slice(&buffer[..count])).await.is_err() {
                        return;
                    }
                }
            }
        }
    });
    near
}

async fn send_all(send: &mut h2::SendStream<Bytes>, mut data: Bytes) -> Result<(), h2::Error> {
    while !data.is_empty() {
        send.reserve_capacity(data.len());
        let room = std::future::poll_fn(|context| send.poll_capacity(context))
            .await
            .ok_or_else(|| h2::Error::from(h2::Reason::CANCEL))??;
        let piece = data.split_to(room.min(data.len()));
        send.send_data(piece, false)?;
    }
    Ok(())
}

fn offered_protocol(options: &Options) -> String {
    format!("{}{PROTOCOL}", version_of(options).prefix())
}

/// The WebSocket and the QMux session on a tunnel whose CONNECT was answered 200.
async fn run_tunnel(io: DuplexStream, role: Role, agreed: &str, options: Arc<Options>, number: u64) {
    let socket = WebSocketStream::from_raw_socket(io, role, None).await;
    let upgraded = ws::Upgraded::new(socket).with_alpn(agreed).with_session_config(session_config(&options));
    let session = if role == Role::Client { upgraded.connect() } else { upgraded.accept() };
    run_connection(session, options, number).await;
}

async fn open_tunnel(
    response: h2::client::ResponseFuture,
    send: h2::SendStream<Bytes>,
    options: Arc<Options>,
    number: u64,
) {
    let response = match response.await {
        Ok(response) => response,
        Err(error) => return report_refused(number, &error),
    };
    if response.status() != StatusCode::OK {
        return report_refused(number, &format!("status {}", response.status().as_u16()));
    }
    let agreed = response.headers().get(SEC_WEBSOCKET_PROTOCOL).and_then(|value| value.to_str().ok());
    let agreed = agreed.unwrap_or_default().to_string();
    let recv = response.into_body();
    run_tunnel(bridge(send, recv), Role::Client, &agreed, options, number).await;
}

/// The HTTP/2 connection's byte stream: TCP with prior knowledge (`h2c`), or TLS that agreed on `h2` (RFC 9113
/// section 3.2); `None` when TLS agreed on anything else.
async fn h2_client_io(options: &Options) -> Option<Box<dyn H2Io>> {
    let tcp = TcpStream::connect(("127.0.0.1", options.port)).await.expect("a connection");
    tcp.set_nodelay(true).expect("no delay");
    if options.transport == "h2c" {
        return Some(Box::new(tcp));
    }
    let mut config = client_tls(options);
    config.alpn_protocols = vec![H2_ALPN.to_vec()];
    let name = ServerName::try_from(SERVER_NAME).expect("a server name");
    let stream = TlsConnector::from(Arc::new(config)).connect(name, tcp).await.expect("a TLS session");
    agreed_h2(stream.get_ref().1.alpn_protocol()).then(|| Box::new(stream) as Box<dyn H2Io>)
}

/// The server's side of `h2_client_io`.
async fn h2_server_io(options: &Options, tcp: TcpStream) -> Option<Box<dyn H2Io>> {
    tcp.set_nodelay(true).expect("no delay");
    if options.transport == "h2c" {
        return Some(Box::new(tcp));
    }
    let mut config = server_tls(options);
    config.alpn_protocols = vec![H2_ALPN.to_vec()];
    let stream = TlsAcceptor::from(Arc::new(config)).accept(tcp).await.expect("a TLS session");
    agreed_h2(stream.get_ref().1.alpn_protocol()).then(|| Box::new(stream) as Box<dyn H2Io>)
}

/// Whether TLS agreed on `h2`, said as tunnel 1's refusal when it did not.
fn agreed_h2(agreed: Option<&[u8]>) -> bool {
    if agreed == Some(H2_ALPN) {
        return true;
    }
    report_refused(1, &format!("TLS agreed on {agreed:?}, not h2"));
    false
}

/// What HTTP/2 runs on: TCP, or TLS over it.
trait H2Io: AsyncRead + AsyncWrite + Unpin + Send {}
impl<T: AsyncRead + AsyncWrite + Unpin + Send> H2Io for T {}

async fn run_h2_client(options: Arc<Options>) {
    let Some(io) = h2_client_io(&options).await else { return };
    let (send, connection) = h2::client::Builder::new()
        .initial_window_size(H2_STREAM_WINDOW)
        .initial_connection_window_size(H2_CONNECTION_WINDOW)
        .handshake::<_, Bytes>(io)
        .await
        .expect("an HTTP/2 connection");
    let driver = tokio::spawn(connection);
    let mut send = send.ready().await.expect("an HTTP/2 connection");
    // RFC 8441 section 3: no extended CONNECT before the server's SETTINGS_ENABLE_CONNECT_PROTOCOL.
    let enabled = tokio::time::timeout(RUN, async {
        while !send.is_extended_connect_protocol_enabled() {
            tokio::time::sleep(SETTINGS_POLL).await;
        }
    });
    if enabled.await.is_err() {
        return report_refused(1, &"the server never enabled extended CONNECT");
    }
    let offered = offered_protocol(&options);
    let mut tunnels = JoinSet::new();
    for number in 1..=options.connections {
        let request = Request::builder()
            .method(Method::CONNECT)
            .uri(format!("https://{SERVER_NAME}{WEBSOCKET_PATH}"))
            .header(SEC_WEBSOCKET_VERSION, WEBSOCKET_VERSION)
            .header(SEC_WEBSOCKET_PROTOCOL, offered.as_str())
            .extension(Protocol::from(WEBSOCKET))
            .body(())
            .expect("a request");
        match send.send_request(request, false) {
            Ok((response, stream)) => {
                tunnels.spawn(open_tunnel(response, stream, options.clone(), number));
            }
            Err(error) => report_refused(number, &error),
        }
    }
    while tunnels.join_next().await.is_some() {}
    // With every handle gone the connection sends what it holds and ends.
    drop(send);
    let _ = tokio::time::timeout(CLOSE_GRACE, driver).await;
}

/// Whether a server takes an extended CONNECT whose `:protocol` is `protocol`: RFC 8441's and RFC 9220's request,
/// RFC 6455's version and the subprotocol this end takes (`offered`).
fn tunnel_acceptable<T>(request: &Request<T>, protocol: Option<&str>, offered: &str) -> bool {
    let headers = request.headers();
    let well_formed = request.method() == Method::CONNECT
        && protocol == Some(WEBSOCKET)
        && request.uri().path() == WEBSOCKET_PATH
        && headers.get(SEC_WEBSOCKET_VERSION).is_some_and(|value| value == WEBSOCKET_VERSION);
    well_formed
        && headers
            .get_all(SEC_WEBSOCKET_PROTOCOL)
            .iter()
            .filter_map(|value| value.to_str().ok())
            .flat_map(|value| value.split(','))
            .any(|token| token.trim() == offered)
}

/// The server's answer to one extended CONNECT on HTTP/2: the tunnel, or 400.
async fn serve_tunnel(
    request: Request<h2::RecvStream>,
    mut respond: h2::server::SendResponse<Bytes>,
    options: Arc<Options>,
    number: u64,
) {
    let offered = offered_protocol(&options);
    let protocol = request.extensions().get::<Protocol>().map(Protocol::as_str);
    if !tunnel_acceptable(&request, protocol, &offered) {
        let refusal = Response::builder().status(StatusCode::BAD_REQUEST).body(()).expect("a response");
        let _ = respond.send_response(refusal, true);
        return report_refused(number, &"refused with status 400");
    }
    let accepted = Response::builder()
        .status(StatusCode::OK)
        .header(SEC_WEBSOCKET_PROTOCOL, offered.as_str())
        .body(())
        .expect("a response");
    let send = match respond.send_response(accepted, false) {
        Ok(send) => send,
        Err(error) => return report_refused(number, &error),
    };
    run_tunnel(bridge(send, request.into_body()), Role::Server, &offered, options, number).await;
}

/// One HTTP/2 connection's tunnels: `connections` of them, refused ones among them, then GOAWAY and what the
/// connection still holds. The client may never hang up (a silent path), so their end is the run's.
async fn run_h2_server(options: Arc<Options>) {
    let listener = TcpListener::bind(("127.0.0.1", options.port)).await.expect("a listening socket");
    println!("port={}", listener.local_addr().expect("its address").port());
    let (tcp, _) = listener.accept().await.expect("a connection");
    let Some(io) = h2_server_io(&options, tcp).await else { return };
    let mut connection = h2::server::Builder::new()
        .enable_connect_protocol()
        .initial_window_size(H2_STREAM_WINDOW)
        .initial_connection_window_size(H2_CONNECTION_WINDOW)
        .handshake::<_, Bytes>(io)
        .await
        .expect("an HTTP/2 connection");
    let mut tunnels = JoinSet::new();
    let mut number = 0;
    let mut ended = 0;
    while ended < options.connections {
        tokio::select! {
            accepted = connection.accept() => {
                let Some(Ok((request, respond))) = accepted else { break };
                number += 1;
                tunnels.spawn(serve_tunnel(request, respond, options.clone(), number));
            }
            Some(_) = tunnels.join_next(), if !tunnels.is_empty() => ended += 1,
        }
    }
    connection.graceful_shutdown();
    let _ = tokio::time::timeout(CLOSE_GRACE, std::future::poll_fn(|context| connection.poll_closed(context))).await;
    while tunnels.join_next().await.is_some() {}
}

/// The tunnel's byte stream for tokio-tungstenite over an HTTP/3 request stream's halves, as `bridge` does for HTTP/2:
/// QUIC's own flow control paces both tasks. h3 gives each end its own stream types, so this makes one function per
/// end.
macro_rules! h3_bridge {
    ($name:ident, $end:ident) => {
        fn $name(
            mut send: h3::$end::RequestStream<h3_quinn::SendStream<Bytes>, Bytes>,
            mut recv: h3::$end::RequestStream<h3_quinn::RecvStream, Bytes>,
        ) -> DuplexStream {
            let (near, far) = tokio::io::duplex(BRIDGE_BYTES);
            let (mut far_read, mut far_write) = tokio::io::split(far);
            tokio::spawn(async move {
                while let Ok(Some(mut chunk)) = recv.recv_data().await {
                    if far_write.write_all_buf(&mut chunk).await.is_err() {
                        return;
                    }
                }
                let _ = far_write.shutdown().await;
            });
            tokio::spawn(async move {
                let mut buffer = vec![0u8; BRIDGE_BYTES];
                loop {
                    match far_read.read(&mut buffer).await {
                        Ok(0) | Err(_) => {
                            let _ = send.finish().await;
                            return;
                        }
                        Ok(count) => {
                            if send.send_data(Bytes::copy_from_slice(&buffer[..count])).await.is_err() {
                                return;
                            }
                        }
                    }
                }
            });
            near
        }
    };
}

h3_bridge!(bridge_h3_client, client);
h3_bridge!(bridge_h3_server, server);

/// QUIC's Initial packets use AES-128-GCM whatever suite the run negotiates (RFC 9001 section 5.2).
fn initial_suite() -> rustls::quic::Suite {
    let suite = aws_lc_rs::cipher_suite::TLS13_AES_128_GCM_SHA256.tls13().expect("a TLS 1.3 suite");
    suite.quic_suite().expect("a QUIC suite")
}

fn loopback(port: u16) -> std::net::SocketAddr {
    std::net::SocketAddr::from(([127, 0, 0, 1], port))
}

async fn open_h3_tunnel(
    mut stream: h3::client::RequestStream<h3_quinn::BidiStream<Bytes>, Bytes>,
    options: Arc<Options>,
    number: u64,
) {
    let response = match stream.recv_response().await {
        Ok(response) => response,
        Err(error) => return report_refused(number, &error),
    };
    if response.status() != StatusCode::OK {
        return report_refused(number, &format!("status {}", response.status().as_u16()));
    }
    let agreed = response.headers().get(SEC_WEBSOCKET_PROTOCOL).and_then(|value| value.to_str().ok());
    let agreed = agreed.unwrap_or_default().to_string();
    let (send, recv) = stream.split();
    run_tunnel(bridge_h3_client(send, recv), Role::Client, &agreed, options, number).await;
}

/// `connections` tunnels at once on one HTTP/3 connection over Quinn, once the server's SETTINGS allow them; then
/// H3_NO_ERROR, once the server's GOAWAY says its tunnels are done too.
async fn run_h3_client(options: Arc<Options>) {
    let mut tls = client_tls(&options);
    tls.alpn_protocols = vec![H3_ALPN.to_vec()];
    let quic = QuicClientConfig::with_initial(Arc::new(tls), initial_suite()).expect("a QUIC client config");
    let endpoint = quinn::Endpoint::client(loopback(0)).expect("a client endpoint");
    let connecting = endpoint
        .connect_with(quinn::ClientConfig::new(Arc::new(quic)), loopback(options.port), SERVER_NAME)
        .expect("a connection");
    let connection = connecting.await.expect("a handshake");
    let (mut driver, mut send) =
        h3::client::new(h3_quinn::Connection::new(connection.clone())).await.expect("an HTTP/3 connection");
    let driver = tokio::spawn(async move { std::future::poll_fn(|context| driver.poll_close(context)).await });
    // RFC 9220 section 3: no extended CONNECT before the server's SETTINGS_ENABLE_CONNECT_PROTOCOL.
    let enabled = tokio::time::timeout(RUN, async {
        while !send.settings().enable_extended_connect() {
            tokio::time::sleep(SETTINGS_POLL).await;
        }
    });
    if enabled.await.is_err() {
        return report_refused(1, &"the server never enabled extended CONNECT");
    }
    let offered = offered_protocol(&options);
    let mut tunnels = JoinSet::new();
    for number in 1..=options.connections {
        let request = Request::builder()
            .method(Method::CONNECT)
            .uri(format!("https://{SERVER_NAME}{WEBSOCKET_PATH}"))
            .header(SEC_WEBSOCKET_VERSION, WEBSOCKET_VERSION)
            .header(SEC_WEBSOCKET_PROTOCOL, offered.as_str())
            .extension(h3::ext::Protocol::WEBSOCKET)
            .body(())
            .expect("a request");
        match send.send_request(request).await {
            Ok(stream) => {
                tunnels.spawn(open_h3_tunnel(stream, options.clone(), number));
            }
            Err(error) => report_refused(number, &error),
        }
    }
    while tunnels.join_next().await.is_some() {}
    // With no request handle left, the driver ends at the server's GOAWAY.
    drop(send);
    let _ = tokio::time::timeout(H3_LINGER, driver).await;
    connection.close(H3_NO_ERROR.into(), b"");
    let _ = tokio::time::timeout(CLOSE_GRACE, endpoint.wait_idle()).await;
}

/// The server's answer to one extended CONNECT on HTTP/3: the tunnel, or 400.
async fn serve_h3_tunnel(
    resolver: h3::server::RequestResolver<h3_quinn::Connection, Bytes>,
    options: Arc<Options>,
    number: u64,
) {
    let (request, mut stream) = match resolver.resolve_request().await {
        Ok(resolved) => resolved,
        Err(error) => return report_refused(number, &error),
    };
    let offered = offered_protocol(&options);
    let protocol = request.extensions().get::<h3::ext::Protocol>().map(h3::ext::Protocol::as_str);
    if !tunnel_acceptable(&request, protocol, &offered) {
        let refusal = Response::builder().status(StatusCode::BAD_REQUEST).body(()).expect("a response");
        if stream.send_response(refusal).await.is_ok() {
            let _ = stream.finish().await;
        }
        return report_refused(number, &"refused with status 400");
    }
    let accepted = Response::builder()
        .status(StatusCode::OK)
        .header(SEC_WEBSOCKET_PROTOCOL, offered.as_str())
        .body(())
        .expect("a response");
    if let Err(error) = stream.send_response(accepted).await {
        return report_refused(number, &error);
    }
    let (send, recv) = stream.split();
    run_tunnel(bridge_h3_server(send, recv), Role::Server, &offered, options, number).await;
}

/// One HTTP/3 connection's tunnels, refused ones among them, then GOAWAY; the client closes the connection, or the
/// grace ends it.
async fn run_h3_server(options: Arc<Options>) {
    let mut tls = server_tls(&options);
    tls.alpn_protocols = vec![H3_ALPN.to_vec()];
    let quic = QuicServerConfig::with_initial(Arc::new(tls), initial_suite()).expect("a QUIC server config");
    let config = quinn::ServerConfig::with_crypto(Arc::new(quic));
    let endpoint = quinn::Endpoint::server(config, loopback(options.port)).expect("a server endpoint");
    println!("port={}", endpoint.local_addr().expect("its address").port());
    let incoming = endpoint.accept().await.expect("a connection");
    let connection = incoming.await.expect("a handshake");
    let mut http = h3::server::builder()
        .enable_extended_connect(true)
        .build::<_, Bytes>(h3_quinn::Connection::new(connection.clone()))
        .await
        .expect("an HTTP/3 connection");
    let mut tunnels = JoinSet::new();
    for number in 1..=options.connections {
        match http.accept().await {
            Ok(Some(resolver)) => {
                tunnels.spawn(serve_h3_tunnel(resolver, options.clone(), number));
            }
            Ok(None) | Err(_) => break,
        }
    }
    while tunnels.join_next().await.is_some() {}
    let _ = tokio::time::timeout(CLOSE_GRACE, http.shutdown(0)).await;
    let _ = tokio::time::timeout(H3_LINGER, connection.closed()).await;
    connection.close(H3_NO_ERROR.into(), b"");
    let _ = tokio::time::timeout(CLOSE_GRACE, endpoint.wait_idle()).await;
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
        transport: "tls".to_string(),
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
            "--transport" => options.transport.clone_from(value),
            other => panic!("unknown option {other}"),
        }
    }
    options
}

#[tokio::main(flavor = "current_thread")]
async fn main() {
    let options = Arc::new(parse_options());
    match (options.transport.as_str(), options.client) {
        ("h2" | "h2c", true) => run_h2_client(options).await,
        ("h2" | "h2c", false) => run_h2_server(options).await,
        ("h3", true) => run_h3_client(options).await,
        ("h3", false) => run_h3_server(options).await,
        (_, true) => run_client(options).await,
        (_, false) => run_server(options).await,
    }
}
