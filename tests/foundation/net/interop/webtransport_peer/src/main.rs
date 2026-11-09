//! crd-webtransport-quinn-peer: a WebTransport endpoint over web-transport-quinn and rustls, real UDP on the loopback,
//! the counterpart of crd-quic-peer's `--webtransport` mode. webtransport_interop.py runs one of each and reads both
//! reports.
//!
//! The exchange is interop_webtransport.hpp's, byte for byte, and so is the report. The certificate is self-signed from
//! the seed; a client pins its key. This crate speaks only the legacy `webtransport` spelling, with no flow control.

use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use bytes::Bytes;
use rcgen::{CertificateParams, KeyPair, PKCS_ED25519};
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::crypto::{CryptoProvider, aws_lc_rs, verify_tls13_signature};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer, ServerName, UnixTime};
use rustls::{ClientConfig, DigitallySignedStruct, ServerConfig, SignatureScheme};
use tokio::sync::{Notify, Semaphore};
use url::Url;
use web_transport_quinn::http::{HeaderName, HeaderValue, StatusCode};
use web_transport_quinn::proto::{self, ConnectRequest, ConnectResponse};
use web_transport_quinn::quinn;
use web_transport_quinn::quinn::crypto::rustls::{QuicClientConfig, QuicServerConfig};
use web_transport_quinn::{
    ALPN, ClientError, ConnectError, ReadError, RecvStream, Request, SendStream, Session, SessionError,
    WebTransportError, WriteError,
};

const SERVER_NAME: &str = "cerid.test";
const URL: &str = "https://cerid.test/wt";
const RUN: Duration = Duration::from_secs(60);
const CLOSE_GRACE: Duration = Duration::from_millis(500);
const DATAGRAM_SETTLE: Duration = Duration::from_secs(1);
const LABEL_BYTES: usize = 8;
const DATAGRAM_INDEX_BYTES: usize = 4;
const CHUNK_BYTES: usize = 4096;
const KIND_CONTROL: u8 = 0;
const KIND_ECHO: u8 = 1;
const KIND_UNI: u8 = 2;
const KIND_RESET: u8 = 3;
const KIND_STOP: u8 = 4;
const KIND_REPLY: u8 = 0x80;
const ACK: u8 = 0xA5;
const RESET_BASE: u32 = 0x10000;
const STOP_BASE: u32 = 0x20000;
const CLOSE_CODE: u32 = 0x0C1_D05E;
const CLOSE_REASON: &str = "cerid-interop done";
const OFFERED: [&str; 2] = ["cerid-a", "cerid-b"];
const CLIENT_DATAGRAM_TAG: u64 = 16;
const SERVER_DATAGRAM_TAG: u64 = 17;
const MAX_STREAMS: u32 = 4096;
const ED25519_PKCS8_PREFIX: [u8; 16] = [
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
];
const ED25519_SPKI_PREFIX: [u8; 12] = [0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00];

#[derive(Default)]
struct Options {
    client: bool,
    client_closes: bool,
    port: u16,
    seed: Vec<u8>,
    group: String,
    cipher: String,
    streams: u64,
    bytes: u64,
    code: u32,
    uni: u64,
    uni_bytes: u64,
    resets: u64,
    stops: u64,
    datagrams: u64,
    datagram_bytes: usize,
    reject: u16,
    protocols: bool,
    origin: String,
}

#[derive(Default)]
struct Counters {
    echo: AtomicU64,
    uni: AtomicU64,
    resets_sent: AtomicU64,
    resets_seen: AtomicU64,
    stops_sent: AtomicU64,
    stops_seen: AtomicU64,
    bytes_received: AtomicU64,
    bytes_sent: AtomicU64,
    datagrams_sent: AtomicU64,
    datagrams_received: AtomicU64,
    failed: AtomicBool,
}

impl Counters {
    fn check(&self, good: bool) {
        if !good {
            self.failed.store(true, Ordering::Relaxed);
        }
    }
}

/// What a report says of how the session began and ended.
#[derive(Default)]
struct Outcome {
    opened: bool,
    status: u64,
    protocol: String,
    origin: String,
    control_done: bool,
    close_seen: bool,
    close_code: u64,
    close_reason: String,
}

/// One session's exchange: what its tasks share.
struct Exchange {
    options: Arc<Options>,
    counters: Counters,
    session: Session,
    finished: Semaphore,
    control: Mutex<Option<(SendStream, RecvStream)>>,
    control_arrived: Notify,
    datagrams_seen: Mutex<Vec<bool>>,
    datagrams_done: Notify,
}

fn pattern(tag: u64, index: u64, offset: u64) -> u8 {
    let at = offset.wrapping_add(index.wrapping_mul(37)) & 0xFFFF;
    at.wrapping_mul(31).wrapping_add(at >> 8).wrapping_add(tag.wrapping_mul(7)) as u8
}

fn label(kind: u8, index: u32) -> [u8; LABEL_BYTES] {
    let mut bytes = [0u8; LABEL_BYTES];
    bytes[0] = kind;
    bytes[4..].copy_from_slice(&index.to_be_bytes());
    bytes
}

fn parse_label(bytes: &[u8; LABEL_BYTES]) -> (u8, u32) {
    (bytes[0], u32::from_be_bytes([bytes[4], bytes[5], bytes[6], bytes[7]]))
}

impl Exchange {
    async fn write_body(&self, send: &mut SendStream, tag: u8, index: u32, total: u64) -> Result<(), WriteError> {
        let mut chunk = [0u8; CHUNK_BYTES];
        let mut offset = 0;
        while offset < total {
            let size = usize::try_from(total - offset).unwrap_or(CHUNK_BYTES).min(CHUNK_BYTES);
            for (at, byte) in chunk[..size].iter_mut().enumerate() {
                *byte = pattern(u64::from(tag), u64::from(index), offset + at as u64);
            }
            send.write_all(&chunk[..size]).await?;
            offset += size as u64;
        }
        self.counters.bytes_sent.fetch_add(total, Ordering::Relaxed);
        Ok(())
    }

    /// Everything up to the stream's FIN, each byte checked.
    async fn read_body(&self, recv: &mut RecvStream, tag: u8, index: u32, total: u64) -> Result<(), ReadError> {
        let mut buffer = [0u8; CHUNK_BYTES];
        let mut offset = 0u64;
        while let Some(size) = recv.read(&mut buffer).await? {
            let good = buffer[..size]
                .iter()
                .zip(offset..)
                .all(|(byte, at)| *byte == pattern(u64::from(tag), u64::from(index), at));
            self.counters.check(good);
            offset += size as u64;
        }
        self.counters.check(offset == total);
        self.counters.bytes_received.fetch_add(offset, Ordering::Relaxed);
        Ok(())
    }

    fn finish_item(&self, good: bool) {
        self.counters.check(good);
        self.finished.add_permits(1);
    }

    async fn open_echo(self: Arc<Self>, index: u32) {
        let total = self.options.bytes;
        let result = async {
            let (mut send, mut recv) = self.session.open_bi().await.ok()?;
            let writing = async {
                send.write_all(&label(KIND_ECHO, index)).await?;
                self.write_body(&mut send, KIND_ECHO, index, total).await?;
                send.finish().map_err(|_| WriteError::ClosedStream)
            };
            let reading = async {
                let mut head = [0u8; LABEL_BYTES];
                recv.read_exact(&mut head).await.ok()?;
                self.counters.check(parse_label(&head) == (KIND_ECHO | KIND_REPLY, index));
                self.read_body(&mut recv, KIND_ECHO | KIND_REPLY, index, total).await.ok()
            };
            let (wrote, read) = tokio::join!(writing, reading);
            wrote.ok()?;
            read
        }
        .await;
        self.counters.echo.fetch_add(u64::from(result.is_some()), Ordering::Relaxed);
        self.finish_item(result.is_some());
    }

    async fn open_uni(self: Arc<Self>, index: u32) {
        let result = async {
            let mut send = self.session.open_uni().await.ok()?;
            send.write_all(&label(KIND_UNI, index)).await.ok()?;
            self.write_body(&mut send, KIND_UNI, index, self.options.uni_bytes).await.ok()?;
            send.finish().ok()
        }
        .await;
        self.counters.uni.fetch_add(u64::from(result.is_some()), Ordering::Relaxed);
        self.finish_item(result.is_some());
    }

    async fn open_reset(self: Arc<Self>, index: u32) {
        let result = async {
            let (mut send, mut recv) = self.session.open_bi().await.ok()?;
            send.write_all(&label(KIND_RESET, index)).await.ok()?;
            let answer = recv.read_to_end(1).await.ok()?;
            self.counters.check(answer == [ACK]);
            send.reset(RESET_BASE + index).ok()
        }
        .await;
        self.counters.resets_sent.fetch_add(u64::from(result.is_some()), Ordering::Relaxed);
        self.finish_item(result.is_some());
    }

    async fn open_stop(self: Arc<Self>, index: u32) {
        let result = async {
            let (mut send, mut recv) = self.session.open_bi().await.ok()?;
            send.write_all(&label(KIND_STOP, index)).await.ok()?;
            let stopped = send.stopped().await.ok()?;
            self.counters.check(stopped == Some(STOP_BASE + index));
            let rest = recv.read_to_end(1).await.ok()?;
            self.counters.check(rest.is_empty());
            Some(())
        }
        .await;
        self.counters.stops_seen.fetch_add(u64::from(result.is_some()), Ordering::Relaxed);
        self.finish_item(result.is_some());
    }

    async fn serve_bidirectional(self: Arc<Self>, mut send: SendStream, mut recv: RecvStream) {
        let mut head = [0u8; LABEL_BYTES];
        if recv.read_exact(&mut head).await.is_err() {
            self.counters.check(false);
            return;
        }
        let (kind, index) = parse_label(&head);
        match kind {
            KIND_CONTROL => {
                *self.control.lock().expect("the control slot") = Some((send, recv));
                self.control_arrived.notify_one();
            }
            KIND_ECHO => {
                let answered = async {
                    self.read_body(&mut recv, KIND_ECHO, index, self.options.bytes).await.ok()?;
                    send.write_all(&label(KIND_ECHO | KIND_REPLY, index)).await.ok()?;
                    self.write_body(&mut send, KIND_ECHO | KIND_REPLY, index, self.options.bytes).await.ok()?;
                    send.finish().ok()
                }
                .await;
                self.counters.echo.fetch_add(u64::from(answered.is_some()), Ordering::Relaxed);
                self.finish_item(answered.is_some());
            }
            KIND_RESET => {
                let answered = async {
                    send.write_all(&[ACK]).await.ok()?;
                    send.finish().ok()?;
                    let mut buffer = [0u8; 16];
                    match recv.read(&mut buffer).await {
                        Err(ReadError::Reset(code)) if code == RESET_BASE + index => Some(()),
                        _ => None,
                    }
                }
                .await;
                self.counters.resets_seen.fetch_add(u64::from(answered.is_some()), Ordering::Relaxed);
                self.finish_item(answered.is_some());
            }
            KIND_STOP => {
                let answered = recv.stop(STOP_BASE + index).is_ok() && send.finish().is_ok();
                self.counters.stops_sent.fetch_add(u64::from(answered), Ordering::Relaxed);
                self.finish_item(answered);
            }
            _ => self.finish_item(false),
        }
    }

    async fn serve_unidirectional(self: Arc<Self>, mut recv: RecvStream) {
        let mut head = [0u8; LABEL_BYTES];
        let read = async {
            recv.read_exact(&mut head).await.ok()?;
            let (kind, index) = parse_label(&head);
            self.counters.check(kind == KIND_UNI);
            self.read_body(&mut recv, KIND_UNI, index, self.options.uni_bytes).await.ok()
        }
        .await;
        self.counters.uni.fetch_add(u64::from(read.is_some()), Ordering::Relaxed);
        self.finish_item(read.is_some());
    }

    async fn accept_bidirectional(self: Arc<Self>) {
        while let Ok((send, recv)) = self.session.accept_bi().await {
            tokio::spawn(self.clone().serve_bidirectional(send, recv));
        }
    }

    async fn accept_unidirectional(self: Arc<Self>) {
        while let Ok(recv) = self.session.accept_uni().await {
            tokio::spawn(self.clone().serve_unidirectional(recv));
        }
    }

    async fn send_datagrams(self: Arc<Self>) {
        let tag = if self.options.client { CLIENT_DATAGRAM_TAG } else { SERVER_DATAGRAM_TAG };
        let size = self.options.datagram_bytes;
        for index in 0..self.options.datagrams {
            let mut payload = vec![0u8; size];
            payload[..DATAGRAM_INDEX_BYTES].copy_from_slice(&(index as u32).to_be_bytes());
            for (at, byte) in payload.iter_mut().enumerate().skip(DATAGRAM_INDEX_BYTES) {
                *byte = pattern(tag, index, at as u64);
            }
            let sent = self.session.send_datagram_wait(Bytes::from(payload)).await.is_ok();
            self.counters.check(sent);
            self.counters.datagrams_sent.fetch_add(u64::from(sent), Ordering::Relaxed);
        }
    }

    async fn read_datagrams(self: Arc<Self>) {
        let tag = if self.options.client { SERVER_DATAGRAM_TAG } else { CLIENT_DATAGRAM_TAG };
        while let Ok(payload) = self.session.read_datagram().await {
            let Some(head) = payload.first_chunk::<DATAGRAM_INDEX_BYTES>() else {
                self.counters.check(false);
                continue;
            };
            let index = u64::from(u32::from_be_bytes(*head));
            let good = payload.len() == self.options.datagram_bytes
                && index < self.options.datagrams
                && payload.iter().enumerate().skip(DATAGRAM_INDEX_BYTES).all(|(at, byte)| {
                    *byte == pattern(tag, index, at as u64)
                });
            self.counters.check(good);
            if !good {
                continue;
            }
            let mut seen = self.datagrams_seen.lock().expect("the datagram table");
            if !seen[index as usize] {
                seen[index as usize] = true;
                let count = self.counters.datagrams_received.fetch_add(1, Ordering::Relaxed) + 1;
                if count == self.options.datagrams {
                    self.datagrams_done.notify_one();
                }
            }
        }
    }

    /// Writes this end's acknowledgement on the control stream and reads the other's: then nothing is unread at either.
    async fn exchange_control(&self, opened: Option<(SendStream, RecvStream)>) -> Option<()> {
        let (mut send, mut recv) = match opened {
            Some(pair) => pair,
            None => loop {
                if let Some(pair) = self.control.lock().expect("the control slot").take() {
                    break pair;
                }
                self.control_arrived.notified().await;
            },
        };
        send.write_all(&[ACK]).await.ok()?;
        send.finish().ok()?;
        let answer = recv.read_to_end(1).await.ok()?;
        (answer == [ACK]).then_some(())
    }

    fn report(&self, outcome: &Outcome) {
        let value = |counter: &AtomicU64| counter.load(Ordering::Relaxed);
        let counters = &self.counters;
        println!("connection=1\nestablished=1\nwt_opened={}", u8::from(outcome.opened));
        println!("wt_status={}\nwt_refused_error=0\nwt_legacy=1", outcome.status);
        println!("wt_protocol={}\nwt_origin={}", outcome.protocol, outcome.origin);
        println!("wt_echo={}\nwt_uni={}", value(&counters.echo), value(&counters.uni));
        println!("wt_resets_sent={}\nwt_resets_seen={}", value(&counters.resets_sent), value(&counters.resets_seen));
        println!("wt_stops_sent={}\nwt_stops_seen={}", value(&counters.stops_sent), value(&counters.stops_seen));
        println!("wt_bytes_received={}", value(&counters.bytes_received));
        println!("wt_bytes_sent={}", value(&counters.bytes_sent));
        println!("wt_datagrams_sent={}", value(&counters.datagrams_sent));
        println!("wt_datagrams_received={}", value(&counters.datagrams_received));
        println!("wt_control_done={}", u8::from(outcome.control_done));
        println!("wt_verified={}", u8::from(!counters.failed.load(Ordering::Relaxed)));
        println!("wt_close_seen={}\nwt_close_code={}", u8::from(outcome.close_seen), outcome.close_code);
        println!("wt_close_reason={}\nwt_close_error=0", outcome.close_reason);
    }
}

async fn run_session(session: Session, connection: quinn::Connection, options: Arc<Options>, mut outcome: Outcome) {
    let items = options.streams + options.uni + options.resets + options.stops;
    let exchange = Arc::new(Exchange {
        options: options.clone(),
        counters: Counters::default(),
        session,
        finished: Semaphore::new(0),
        control: Mutex::new(None),
        control_arrived: Notify::new(),
        datagrams_seen: Mutex::new(vec![false; usize::try_from(options.datagrams).expect("a count")]),
        datagrams_done: Notify::new(),
    });
    tokio::spawn(exchange.clone().accept_bidirectional());
    tokio::spawn(exchange.clone().accept_unidirectional());
    let mut opened_control = None;
    if options.client {
        let opened = exchange.session.open_bi().await;
        if let Ok((mut send, recv)) = opened {
            exchange.counters.check(send.write_all(&label(KIND_CONTROL, 0)).await.is_ok());
            opened_control = Some((send, recv));
        }
    }
    let kinds = (0..options.streams, 0..options.uni, 0..options.resets, 0..options.stops);
    for index in kinds.0 {
        tokio::spawn(exchange.clone().open_echo(index as u32));
    }
    for index in kinds.1 {
        tokio::spawn(exchange.clone().open_uni(index as u32));
    }
    for index in kinds.2 {
        tokio::spawn(exchange.clone().open_reset(index as u32));
    }
    for index in kinds.3 {
        tokio::spawn(exchange.clone().open_stop(index as u32));
    }
    tokio::spawn(exchange.clone().read_datagrams());
    tokio::spawn(exchange.clone().send_datagrams());

    let total = u32::try_from(2 * items).expect("a count");
    let finished = tokio::time::timeout(RUN, exchange.finished.acquire_many(total)).await;
    exchange.counters.check(finished.is_ok());
    if options.datagrams != 0 && exchange.counters.datagrams_received.load(Ordering::Relaxed) < options.datagrams {
        // A wake raised before this wait stays stored: notify_one is what makes it safe to arrive late.
        let _ = tokio::time::timeout(DATAGRAM_SETTLE, exchange.datagrams_done.notified()).await;
    }
    let settled = tokio::time::timeout(RUN, exchange.exchange_control(opened_control)).await;
    outcome.control_done = matches!(settled, Ok(Some(())));

    if options.client_closes == options.client {
        exchange.session.close(CLOSE_CODE, CLOSE_REASON.as_bytes());
        outcome.close_code = u64::from(CLOSE_CODE);
    }
    let ended = tokio::time::timeout(RUN, exchange.session.closed()).await;
    match ended {
        Ok(SessionError::WebTransportError(WebTransportError::Closed(code, reason))) => {
            outcome.close_seen = true;
            outcome.close_code = u64::from(code);
            outcome.close_reason = reason;
        }
        Ok(SessionError::ConnectionError(quinn::ConnectionError::LocallyClosed)) => {}
        other => {
            eprintln!("crd-webtransport-quinn-peer: the session ended with {other:?}");
            exchange.counters.check(false);
        }
    }
    if options.client_closes == options.client {
        let _ = tokio::time::timeout(CLOSE_GRACE, connection.closed()).await;
    } else {
        connection.close(options.code.into(), b"");
    }
    exchange.report(&outcome);
}

/// A session that never opened still reports, so the driver reads why.
fn report_refused(status: u64) {
    println!("connection=1\nestablished=1\nwt_opened=0\nwt_status={status}\nwt_verified=1");
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
        "x25519" => kx_group::X25519,
        other => panic!("unknown group {other}"),
    };
    let cipher = match options.cipher.as_str() {
        "aes128" => cipher_suite::TLS13_AES_128_GCM_SHA256,
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

fn transport() -> Arc<quinn::TransportConfig> {
    let mut config = quinn::TransportConfig::default();
    config.max_concurrent_bidi_streams(MAX_STREAMS.into());
    config.max_concurrent_uni_streams(MAX_STREAMS.into());
    config.datagram_receive_buffer_size(Some(1 << 22));
    config.datagram_send_buffer_size(1 << 22);
    Arc::new(config)
}

fn loopback() -> std::net::SocketAddr {
    std::net::SocketAddr::from(([127, 0, 0, 1], 0))
}

async fn wait_for_end(endpoint: quinn::Endpoint) {
    let _ = tokio::time::timeout(CLOSE_GRACE, endpoint.wait_idle()).await;
}

fn refused_status(error: &ClientError) -> Option<u64> {
    match error {
        ClientError::HttpError(ConnectError::ProtoError(proto::ConnectError::WrongStatus(Some(status)))) => {
            Some(u64::from(status.as_u16()))
        }
        _ => None,
    }
}

async fn run_client(options: Arc<Options>) {
    let provider = provider_of(&options);
    let (key, _) = key_of(&options.seed);
    let mut spki = ED25519_SPKI_PREFIX.to_vec();
    spki.extend_from_slice(key.public_key_raw());
    let verifier = Arc::new(PinnedKey { spki, provider: provider.clone() });
    let mut tls = ClientConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .dangerous()
        .with_custom_certificate_verifier(verifier)
        .with_no_client_auth();
    tls.alpn_protocols = vec![ALPN.as_bytes().to_vec()];
    let mut config = quinn::ClientConfig::new(Arc::new(QuicClientConfig::try_from(tls).expect("a QUIC client config")));
    config.transport_config(transport());
    let endpoint = quinn::Endpoint::client(loopback()).expect("a client endpoint");
    let server = std::net::SocketAddr::from(([127, 0, 0, 1], options.port));
    let connecting = endpoint.connect_with(config, server, SERVER_NAME).expect("a connection");
    let connection = connecting.await.expect("a handshake");

    let mut request = ConnectRequest::new(Url::parse(URL).expect("a URL"));
    if options.protocols {
        request = request.with_protocols(OFFERED.iter().map(|name| (*name).to_string()));
    }
    if !options.origin.is_empty() {
        let value = HeaderValue::from_str(&options.origin).expect("an Origin");
        request = request.with_header(HeaderName::from_static("origin"), value);
    }
    match Session::connect(connection.clone(), request).await {
        Ok(session) => {
            let protocol = session.protocol().unwrap_or_default().to_string();
            let outcome = Outcome { opened: true, status: 200, protocol, ..Outcome::default() };
            run_session(session, connection, options, outcome).await;
        }
        Err(error) => {
            match refused_status(&error) {
                Some(status) => report_refused(status),
                None => panic!("the session did not open: {error:?}"),
            }
            connection.close(options.code.into(), b"");
        }
    }
    wait_for_end(endpoint).await;
}

async fn run_server(options: Arc<Options>) {
    let provider = provider_of(&options);
    let (key, pkcs8) = key_of(&options.seed);
    let params = CertificateParams::new(vec![SERVER_NAME.to_string()]).expect("a server name");
    let certificate = params.self_signed(&key).expect("a certificate");
    let chain = vec![certificate.der().clone()];
    let mut tls = ServerConfig::builder_with_provider(provider)
        .with_protocol_versions(&[&rustls::version::TLS13])
        .expect("TLS 1.3")
        .with_no_client_auth()
        .with_single_cert(chain, PrivateKeyDer::Pkcs8(pkcs8.into()))
        .expect("a certificate and its key");
    tls.alpn_protocols = vec![ALPN.as_bytes().to_vec()];
    let mut config =
        quinn::ServerConfig::with_crypto(Arc::new(QuicServerConfig::try_from(tls).expect("a QUIC server config")));
    config.transport_config(transport());
    let bound = std::net::SocketAddr::from(([127, 0, 0, 1], options.port));
    let endpoint = quinn::Endpoint::server(config, bound).expect("a server endpoint");
    println!("port={}", endpoint.local_addr().expect("its address").port());
    use std::io::Write as _;
    std::io::stdout().flush().expect("standard output");

    let incoming = endpoint.accept().await.expect("a connection");
    let connection = incoming.await.expect("a handshake");
    let request = Request::accept(connection.clone()).await.expect("a CONNECT request");
    let origin = request
        .connect()
        .headers
        .get("origin")
        .and_then(|value| value.to_str().ok())
        .unwrap_or_default()
        .to_string();
    if options.reject != 0 {
        // Not request.reject(): it drops the HTTP/3 control stream with the request, which a client must then treat as
        // a connection error (RFC 9114 section 6.2.1) however the refusal raced it.
        let status = StatusCode::from_u16(options.reject).expect("a status");
        let held = request.respond(ConnectResponse::new(status)).await.expect("a refusal");
        let _ = tokio::time::timeout(RUN, connection.closed()).await;
        drop(held);
        println!("connection=1\nestablished=1\nwt_opened=0\nwt_status={}\nwt_verified=1", options.reject);
        println!("wt_origin={origin}");
        wait_for_end(endpoint).await;
        return;
    }
    let mut response = ConnectResponse::OK;
    let mut protocol = String::new();
    if options.protocols {
        let offered: Vec<String> = request.connect().protocols.clone();
        if offered.len() == OFFERED.len() && offered.iter().zip(OFFERED).all(|(left, right)| left == right) {
            protocol = OFFERED[1].to_string();
            response = response.with_protocol(protocol.clone());
        }
    }
    let session = request.respond(response).await.expect("a response");
    let outcome = Outcome { opened: true, status: 200, protocol, origin, ..Outcome::default() };
    run_session(session, connection, options, outcome).await;
    wait_for_end(endpoint).await;
}

fn parse_hex(text: &str) -> Vec<u8> {
    let byte = |at: usize| u8::from_str_radix(&text[at * 2..at * 2 + 2], 16).expect("a hexadecimal seed");
    (0..text.len() / 2).map(byte).collect()
}

fn parse_options() -> Options {
    let mut options = Options::default();
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    for pair in arguments.chunks(2) {
        let [name, value] = pair else { panic!("{} has no value", pair[0]) };
        let number = || value.parse::<u64>().expect("a number");
        match name.as_str() {
            "--role" => options.client = value == "client",
            "--closer" => options.client_closes = value == "client",
            "--port" => options.port = u16::try_from(number()).expect("a port"),
            "--seed" => options.seed = parse_hex(value),
            "--group" => options.group.clone_from(value),
            "--cipher" => options.cipher.clone_from(value),
            "--streams" => options.streams = number(),
            "--bytes" => options.bytes = number(),
            "--code" => options.code = u32::try_from(number()).expect("a code"),
            "--uni" => options.uni = number(),
            "--uni-bytes" => options.uni_bytes = number(),
            "--resets" => options.resets = number(),
            "--stops" => options.stops = number(),
            "--datagrams" => options.datagrams = number(),
            "--datagram-bytes" => options.datagram_bytes = usize::try_from(number()).expect("a size"),
            "--wt-reject" => options.reject = u16::try_from(number()).expect("a status"),
            "--wt-protocols" => options.protocols = value != "0",
            "--wt-origin" => options.origin.clone_from(value),
            "--version" | "--max-streams" | "--webtransport" | "--wt-window" | "--wt-allow" => {}
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
