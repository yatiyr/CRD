//! crd-quiche-peer: the HTTP/3 exchange of http_exchange.h over Cloudflare's quiche, real UDP on the loopback, the
//! counterpart of crd-quic-peer --http3 1 (crd-net). http3_interop.py runs one of each and reads both reports.
//!
//! quiche's QPACK has no dynamic table, so this end always advertises none, whatever --table says, and its reports show
//! what Cerid advertised. Its report of each connection, one `name=value` per line from `connection=N`, goes to
//! standard output when the connection has ended. The certificate is self-signed from the seed; a client pins its key.

use std::collections::VecDeque;
use std::fs;
use std::io::{ErrorKind, Write};
use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, Instant};

use boring::ssl::{SslContextBuilder, SslMethod, SslSignatureAlgorithm};
use quiche::h3::{self, Event, Header, NameValue};
use rcgen::{CertificateParams, KeyPair, PKCS_ED25519};
use rustls_pki_types::PrivatePkcs8KeyDer;

const SERVER_NAME: &str = "cerid.test";
const ALPN: &[u8] = b"h3";
const TUNNEL_PROTOCOL: &[u8] = b"cerid-tunnel";
const REPEAT_VALUE: &[u8] = b"a field value every message of the exchange repeats";
const CANCEL_CODE: u64 = 0x10C;
const REJECTED_CODE: u64 = 0x10B;
const NO_ERROR: u64 = 0x100;
const LARGEST_GOAWAY: u64 = (1 << 62) - 4;
const MAX_REQUESTS: usize = 512;
const PATTERN_BYTES: usize = 65536;
const DATAGRAM_INDEX_BYTES: usize = 4;
const WRITE_CHUNK: usize = 16384;
const PACKET_BYTES: usize = 65535;
const SEND_PAYLOAD_BYTES: usize = 1350;
const CONNECTION_ID_BYTES: usize = 16;
const RUN: Duration = Duration::from_secs(60);
const SERVICE_WAIT: Duration = Duration::from_millis(5);
const LINGER: Duration = Duration::from_millis(100);
const DATAGRAM_SETTLE: Duration = Duration::from_secs(1);
const SETTING_TABLE: u64 = 0x1;
const SETTING_FIELD_LIMIT: u64 = 0x6;
const SETTING_BLOCKED: u64 = 0x7;
const SETTING_CONNECT: u64 = 0x8;
const SETTING_DATAGRAM: u64 = 0x33;
/// ssl_early_data_accepted of BoringSSL's ssl_early_data_reason_t.
const EARLY_DATA_ACCEPTED: u32 = 2;
const TICKET_KEY: [u8; 48] = [0x5A; 48];
const ED25519_PKCS8_PREFIX: [u8; 16] = [
    0x30, 0x2e, 0x02, 0x01, 0x00, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x04, 0x22, 0x04, 0x20,
];

#[derive(Clone, Copy, Default, PartialEq)]
enum Goaway {
    #[default]
    None,
    Notice,
    Reject,
    Client,
}

#[derive(Clone, Copy, Default, PartialEq)]
enum Cancel {
    #[default]
    None,
    Client,
    Server,
}

#[derive(Default)]
struct Options {
    client: bool,
    port: u16,
    seed: Vec<u8>,
    group: String,
    version: u32,
    code: u64,
    max_streams: u64,
    requests: u64,
    request_bytes: u64,
    response_bytes: u64,
    tunnels: u64,
    field_bytes: u64,
    field_limit: u64,
    trailers: bool,
    interim: bool,
    goaway: Goaway,
    cancel: Cancel,
    over_limit: bool,
    datagrams: u64,
    datagram_bytes: u64,
    connections: u64,
    early: bool,
    reject_early: bool,
}

impl Options {
    fn fill_bytes(&self, index: u64) -> u64 {
        if self.over_limit && index + 1 == self.requests {
            self.field_limit + (self.field_limit / 2)
        } else {
            self.field_bytes
        }
    }

    fn is_tunnel(&self, index: u64) -> bool {
        index < self.tunnels
    }

    fn has_trailers(&self, index: u64) -> bool {
        self.trailers && !self.is_tunnel(index)
    }

    fn is_last(&self, index: u64) -> bool {
        index + 1 == self.requests
    }

    fn body_bytes(&self, index: u64, response: bool) -> u64 {
        if response && self.cancel == Cancel::Client && self.is_last(index) {
            u64::MAX
        } else if response {
            self.response_bytes
        } else {
            self.request_bytes
        }
    }

    fn has_length(&self, index: u64, body: u64) -> bool {
        index.is_multiple_of(2) && !self.is_tunnel(index) && body != u64::MAX
    }

    fn per_tunnel(&self) -> u64 {
        (self.datagrams.checked_div(self.tunnels).unwrap_or(1)).max(1)
    }
}

/// What one received section held.
#[derive(Default)]
struct Section {
    status: u64,
    index_seen: bool,
    repeat_seen: bool,
    fill_seen: bool,
    protocol_seen: bool,
    connect: bool,
    get: bool,
    post: bool,
    wrong: bool,
}

fn decimal(bytes: &[u8]) -> u64 {
    std::str::from_utf8(bytes).ok().and_then(|text| text.parse().ok()).unwrap_or(u64::MAX)
}

fn fill_byte(at: usize) -> u8 {
    b'a' + (at % 26) as u8
}

impl Section {
    fn take(&mut self, options: &Options, index: u64, name: &[u8], value: &[u8]) {
        match name {
            b":status" => self.status = decimal(value),
            b":method" => {
                self.connect = value == b"CONNECT";
                self.get = value == b"GET";
                self.post = value == b"POST";
            }
            b":path" => {
                let start = value.iter().rposition(|byte| *byte == b'/').map_or(0, |slash| slash + 1);
                self.wrong = self.wrong || decimal(&value[start..]) != index;
            }
            b":protocol" => self.protocol_seen = value == TUNNEL_PROTOCOL,
            b"x-cerid-repeat" => self.repeat_seen = value == REPEAT_VALUE,
            b"x-cerid-index" => self.index_seen = decimal(value) == index,
            b"x-cerid-fill" => {
                self.fill_seen = value.len() as u64 == options.fill_bytes(index)
                    && value.iter().enumerate().all(|(at, byte)| *byte == fill_byte(at));
            }
            _ => {}
        }
    }

    fn good(&self, options: &Options, index: u64, response: bool) -> bool {
        let fill = options.fill_bytes(index) == 0 || self.fill_seen;
        if self.wrong || !self.repeat_seen || !self.index_seen || !fill {
            return false;
        }
        if response {
            return self.status == 200;
        }
        if options.is_tunnel(index) {
            return self.connect && self.protocol_seen;
        }
        if options.body_bytes(index, false) != 0 { self.post } else { self.get }
    }
}

/// What is left to write of a message, in order.
#[derive(Clone, Copy, Default, PartialEq)]
enum Send {
    #[default]
    Nothing,
    Interim,
    Final,
    Body,
    Trailers,
}

#[derive(Default)]
struct Stream {
    used: bool,
    section: Section,
    sections: u32,
    received: u64,
    offered: u64,
    send: Send,
    trailers_seen: bool,
    tunnel_open: bool,
    ended: bool,
    responded: bool,
    counted: bool,
}

#[derive(Default)]
struct Counters {
    opened: u64,
    seen: u64,
    request_ended: u64,
    completed: u64,
    rejected: u64,
    aborted: u64,
    abort_code: u64,
    cancelled: u64,
    status_431: u64,
    refused_after_goaway: u64,
    goaway_seen: u64,
    goaway_id: u64,
    goaway_sent: u64,
    /// The stream a server's GOAWAY of `reject` named: requests from there on are refused.
    reject_from: Option<u64>,
    body_received: u64,
    body_sent: u64,
    interims: u64,
    trailers: u64,
    tunnels: u64,
    early_requests: u64,
    unverified: bool,
    datagrams_sent: u64,
    datagrams_received: u64,
    datagrams_unverified: bool,
}

/// One end's exchange over one connection.
struct App<'a> {
    options: &'a Options,
    pattern: &'a [Vec<u8>; 4],
    fill_text: Vec<u8>,
    streams: Vec<Stream>,
    h3: Option<h3::Connection>,
    settings: Option<Vec<(u64, u64)>>,
    cancel_due: Option<u64>,
    counters: Counters,
    seen_datagrams: Vec<bool>,
    echoes: VecDeque<u32>,
    last_datagram: Instant,
    done_at: Option<Instant>,
}

fn pattern_index(stream: u64, offset: u64) -> usize {
    (offset.wrapping_add(stream.wrapping_mul(37)) as usize) & (PATTERN_BYTES - 1)
}

fn field(name: &[u8], value: &[u8]) -> Header {
    Header::new(name, value)
}

fn is_blocked(error: &h3::Error) -> bool {
    matches!(
        error,
        h3::Error::StreamBlocked
            | h3::Error::Done
            | h3::Error::TransportError(quiche::Error::StreamLimit | quiche::Error::Done)
    )
}

fn stopped_code(error: &h3::Error) -> Option<u64> {
    match error {
        h3::Error::TransportError(quiche::Error::StreamStopped(code) | quiche::Error::StreamReset(code)) => Some(*code),
        _ => None,
    }
}

impl<'a> App<'a> {
    fn new(options: &'a Options, pattern: &'a [Vec<u8>; 4]) -> Self {
        App {
            options,
            pattern,
            fill_text: (0..(options.field_limit * 2) as usize).map(fill_byte).collect(),
            streams: (0..MAX_REQUESTS).map(|_| Stream::default()).collect(),
            h3: None,
            settings: None,
            cancel_due: None,
            counters: Counters::default(),
            seen_datagrams: vec![false; options.datagrams as usize],
            echoes: VecDeque::new(),
            last_datagram: Instant::now(),
            done_at: None,
        }
    }

    fn common_fields(&self, index: u64, fields: &mut Vec<Header>) {
        fields.push(field(b"x-cerid-repeat", REPEAT_VALUE));
        fields.push(field(b"x-cerid-index", index.to_string().as_bytes()));
        let fill = self.options.fill_bytes(index) as usize;
        if fill != 0 {
            fields.push(field(b"x-cerid-fill", &self.fill_text[..fill]));
        }
    }

    fn request_fields(&self, index: u64) -> Vec<Header> {
        let body = self.options.body_bytes(index, false);
        let tunnel = self.options.is_tunnel(index);
        let method: &[u8] = if tunnel {
            b"CONNECT"
        } else if body != 0 {
            b"POST"
        } else {
            b"GET"
        };
        let path = format!("/{}/{index}", if tunnel { "tunnel" } else { "cerid" });
        let mut fields = vec![
            field(b":method", method),
            field(b":scheme", b"https"),
            field(b":authority", SERVER_NAME.as_bytes()),
            field(b":path", path.as_bytes()),
        ];
        if tunnel {
            fields.push(field(b":protocol", TUNNEL_PROTOCOL));
        }
        self.common_fields(index, &mut fields);
        if self.options.has_length(index, body) && body != 0 {
            fields.push(field(b"content-length", body.to_string().as_bytes()));
        }
        fields
    }

    fn response_fields(&self, index: u64) -> Vec<Header> {
        let body = self.options.body_bytes(index, true);
        let mut fields = vec![field(b":status", b"200")];
        self.common_fields(index, &mut fields);
        if self.options.has_length(index, body) {
            fields.push(field(b"content-length", body.to_string().as_bytes()));
        }
        fields
    }

    fn body_follows(&self, index: u64, body: u64) -> bool {
        body != 0 || self.options.has_trailers(index) || self.options.is_tunnel(index)
    }

    /// What ends a server's or client's turn with this stream: the peer stopped it, or its send part is done with.
    fn stopped(&mut self, index: usize, code: u64) {
        self.streams[index].send = Send::Nothing;
        self.count_abort(index, code);
    }

    fn count_abort(&mut self, index: usize, code: u64) {
        if self.streams[index].counted || code == NO_ERROR {
            return;
        }
        self.streams[index].counted = true;
        if code == REJECTED_CODE {
            self.counters.rejected += 1;
        } else {
            self.counters.aborted += 1;
            self.counters.abort_code = code;
        }
    }

    /// May the body of this stream go yet? A tunnel's waits: a client's FIN for its datagrams, a server's for the
    /// request's end.
    fn body_may_go(&self, index: usize) -> bool {
        if !self.options.is_tunnel(index as u64) {
            return true;
        }
        let stream = &self.streams[index];
        if self.options.client { stream.tunnel_open && self.datagrams_settled() } else { stream.ended }
    }

    /// Writes what it can of `index`'s message; false when the connection cannot go on.
    fn pump(&mut self, conn: &mut quiche::Connection, index: usize) -> bool {
        loop {
            let send = self.streams[index].send;
            let result = match send {
                Send::Nothing => return true,
                Send::Interim => self.write_interim(conn, index),
                Send::Final => self.write_final(conn, index),
                Send::Body => match self.write_body(conn, index) {
                    Ok(true) => Ok(()),
                    Ok(false) => return true,
                    Err(error) => Err(error),
                },
                Send::Trailers => self.write_trailers(conn, index),
            };
            match result {
                Ok(()) => {}
                Err(error) if is_blocked(&error) => return true,
                Err(error) => {
                    if let Some(code) = stopped_code(&error) {
                        self.stopped(index, code);
                        return true;
                    }
                    eprintln!("stream {index}: {error}");
                    return false;
                }
            }
        }
    }

    fn write_interim(&mut self, conn: &mut quiche::Connection, index: usize) -> h3::Result<()> {
        let fields = [field(b":status", b"103"), field(b"link", b"</cerid.css>; rel=preload")];
        let id = (index as u64) * 4;
        self.h3.as_mut().expect("HTTP/3 has started").send_response(conn, id, &fields, false)?;
        self.streams[index].send = Send::Final;
        Ok(())
    }

    fn write_final(&mut self, conn: &mut quiche::Connection, index: usize) -> h3::Result<()> {
        let at = index as u64;
        let fields = self.response_fields(at);
        let id = at * 4;
        let follows = self.body_follows(at, self.options.body_bytes(at, true));
        let h3 = self.h3.as_mut().expect("HTTP/3 has started");
        if self.options.interim {
            h3.send_additional_headers(conn, id, &fields, false, !follows)?;
        } else {
            h3.send_response(conn, id, &fields, !follows)?;
        }
        self.streams[index].send = if follows { Send::Body } else { Send::Nothing };
        Ok(())
    }

    /// Ok(true) when the body has all gone.
    fn write_body(&mut self, conn: &mut quiche::Connection, index: usize) -> h3::Result<bool> {
        if !self.body_may_go(index) {
            return Ok(false);
        }
        let at = index as u64;
        let id = at * 4;
        let goal = self.options.body_bytes(at, !self.options.client);
        let trailers = self.options.has_trailers(at);
        let table = &self.pattern[usize::from(!self.options.client)];
        let h3 = self.h3.as_mut().expect("HTTP/3 has started");
        let stream = &mut self.streams[index];
        loop {
            let start = pattern_index(id, stream.offered);
            let amount = (goal - stream.offered).min((PATTERN_BYTES - start).min(WRITE_CHUNK) as u64) as usize;
            let last = stream.offered + amount as u64 == goal;
            if amount == 0 && trailers {
                stream.send = Send::Trailers;
                return Ok(true);
            }
            let wrote = match h3.send_body(conn, id, &table[start..start + amount], last && !trailers) {
                Ok(wrote) => wrote,
                Err(h3::Error::Done) => return Ok(false),
                Err(error) => return Err(error),
            };
            stream.offered += wrote as u64;
            self.counters.body_sent += wrote as u64;
            if wrote < amount {
                return Ok(false);
            }
            if last {
                stream.send = if trailers { Send::Trailers } else { Send::Nothing };
                return Ok(true);
            }
        }
    }

    fn write_trailers(&mut self, conn: &mut quiche::Connection, index: usize) -> h3::Result<()> {
        let at = index as u64;
        let fields = [field(b"x-cerid-trailer", at.to_string().as_bytes()), field(b"x-cerid-repeat", REPEAT_VALUE)];
        self.h3.as_mut().expect("HTTP/3 has started").send_additional_headers(conn, at * 4, &fields, true, true)?;
        self.streams[index].send = Send::Nothing;
        Ok(())
    }

    fn respond(&mut self, index: usize) {
        self.streams[index].responded = true;
        self.streams[index].send = if self.options.interim { Send::Interim } else { Send::Final };
        self.counters.completed += 1;
    }

    fn respond_waiting(&mut self) {
        for index in 0..MAX_REQUESTS {
            let stream = &self.streams[index];
            let waiting = stream.used && stream.ended && !stream.responded && !stream.counted;
            if waiting && !self.options.is_tunnel(index as u64) {
                self.respond(index);
            }
        }
    }

    fn cancel(&mut self, conn: &mut quiche::Connection) {
        let Some(index) = self.cancel_due.take() else { return };
        let stream = &mut self.streams[index as usize];
        if !stream.counted {
            stream.counted = true;
            stream.send = Send::Nothing;
            self.counters.cancelled += 1;
            let id = index * 4;
            // Either half may already be over; the other still has to go.
            let _ = conn.stream_shutdown(id, quiche::Shutdown::Read, CANCEL_CODE);
            let _ = conn.stream_shutdown(id, quiche::Shutdown::Write, CANCEL_CODE);
        }
    }

    fn goaway(&mut self, conn: &mut quiche::Connection, id: u64) -> bool {
        match self.h3.as_mut().expect("HTTP/3 has started").send_goaway(conn, id) {
            Ok(()) => {
                self.counters.goaway_sent += 1;
                true
            }
            Err(error) => {
                eprintln!("GOAWAY: {error}");
                false
            }
        }
    }

    fn reject(&mut self, conn: &mut quiche::Connection, index: usize) {
        let id = (index as u64) * 4;
        let _ = conn.stream_shutdown(id, quiche::Shutdown::Read, REJECTED_CODE);
        let _ = conn.stream_shutdown(id, quiche::Shutdown::Write, REJECTED_CODE);
        self.streams[index].counted = true;
    }

    fn on_request(&mut self, conn: &mut quiche::Connection, index: usize, early: bool) -> bool {
        let at = index as u64;
        if let Some(first) = self.counters.reject_from
            && at * 4 >= first
        {
            self.reject(conn, index);
            return true;
        }
        if !self.streams[index].section.good(self.options, at, false) {
            self.counters.unverified = true;
        }
        self.counters.seen += 1;
        self.counters.early_requests += u64::from(early);
        if self.counters.seen == 1 && self.options.goaway == Goaway::Notice && !self.goaway(conn, LARGEST_GOAWAY) {
            return false;
        }
        if self.counters.seen == 1 && self.options.goaway == Goaway::Reject {
            let id = (at + 1) * 4;
            self.counters.reject_from = Some(id);
            if !self.goaway(conn, id) {
                return false;
            }
        }
        if self.options.cancel == Cancel::Server && self.options.is_last(at) {
            self.cancel_due = Some(at);
            return true;
        }
        if self.options.is_tunnel(at) {
            self.counters.tunnels += 1;
            self.respond(index);
        }
        true
    }

    fn on_headers(&mut self, conn: &mut quiche::Connection, index: usize, list: &[Header]) -> bool {
        let at = index as u64;
        let early = conn.is_in_early_data();
        let trailer = if self.options.client {
            self.streams[index].section.status >= 200
        } else {
            self.streams[index].sections != 0
        };
        self.streams[index].used = true;
        self.streams[index].sections += 1;
        if trailer {
            for header in list {
                if header.name() == b"x-cerid-trailer" {
                    self.streams[index].trailers_seen = decimal(header.value()) == at;
                }
            }
            return true;
        }
        let mut section = Section::default();
        for header in list {
            section.take(self.options, at, header.name(), header.value());
        }
        if self.options.client && (100..200).contains(&section.status) {
            self.counters.interims += 1;
            return true;
        }
        self.streams[index].section = section;
        if !self.options.client {
            return self.on_request(conn, index, early);
        }
        if self.options.is_tunnel(at) && self.streams[index].section.status == 200 {
            self.streams[index].tunnel_open = true;
            self.counters.tunnels += 1;
        }
        true
    }

    fn on_data(&mut self, conn: &mut quiche::Connection, index: usize, buffer: &mut [u8]) {
        let id = (index as u64) * 4;
        let expected = &self.pattern[usize::from(self.options.client)];
        let h3 = self.h3.as_mut().expect("HTTP/3 has started");
        while let Ok(read) = h3.recv_body(conn, id, buffer) {
            let stream = &mut self.streams[index];
            for (at, byte) in buffer[..read].iter().enumerate() {
                if *byte != expected[pattern_index(id, stream.received + at as u64)] {
                    self.counters.unverified = true;
                }
            }
            stream.received += read as u64;
            self.counters.body_received += read as u64;
            if self.options.client
                && self.options.cancel == Cancel::Client
                && self.options.is_last(index as u64)
                && read != 0
                && !stream.counted
            {
                self.cancel_due = Some(index as u64);
            }
        }
    }

    fn response_ended(&mut self, index: usize) {
        let at = index as u64;
        self.streams[index].counted = true;
        self.counters.completed += 1;
        let stream = &self.streams[index];
        if self.options.over_limit && self.options.is_last(at) && stream.section.status == 431 {
            self.counters.status_431 += 1;
            return;
        }
        if !stream.section.good(self.options, at, true)
            || stream.received != self.options.body_bytes(at, true)
            || (self.options.has_trailers(at) && !stream.trailers_seen)
        {
            self.counters.unverified = true;
        }
        self.counters.trailers += u64::from(stream.trailers_seen);
    }

    fn request_ended(&mut self, conn: &mut quiche::Connection, index: usize) -> bool {
        let at = index as u64;
        let stream = &self.streams[index];
        if stream.received != self.options.body_bytes(at, false)
            || (self.options.has_trailers(at) && !stream.trailers_seen)
        {
            self.counters.unverified = true;
        }
        self.counters.trailers += u64::from(stream.trailers_seen);
        self.counters.request_ended += 1;
        if self.options.goaway == Goaway::Notice
            && self.counters.request_ended == self.options.requests
            && !self.goaway(conn, self.options.requests * 4)
        {
            return false;
        }
        if self.options.is_tunnel(at) || self.streams[index].counted {
            return true;
        }
        if self.options.goaway == Goaway::Client && self.counters.goaway_seen < 2 {
            return true;
        }
        self.respond(index);
        true
    }

    fn poll(&mut self, conn: &mut quiche::Connection, buffer: &mut [u8]) -> bool {
        loop {
            let event = self.h3.as_mut().expect("HTTP/3 has started").poll(conn);
            let (id, event) = match event {
                Ok(pair) => pair,
                Err(h3::Error::Done) => return true,
                Err(error) => {
                    eprintln!("poll: {error}");
                    return false;
                }
            };
            let index = (id / 4) as usize;
            let in_range = id.is_multiple_of(4) && index < MAX_REQUESTS;
            let going = match event {
                Event::Headers { list, .. } if in_range => self.on_headers(conn, index, &list),
                Event::Data if in_range => {
                    self.on_data(conn, index, buffer);
                    true
                }
                Event::Finished if in_range => {
                    self.streams[index].ended = true;
                    if self.options.client {
                        if !self.streams[index].counted {
                            self.response_ended(index);
                        }
                        true
                    } else {
                        self.request_ended(conn, index)
                    }
                }
                Event::Reset(code) if in_range => {
                    self.streams[index].send = Send::Nothing;
                    self.count_abort(index, code);
                    true
                }
                Event::GoAway => {
                    self.counters.goaway_seen += 1;
                    self.counters.goaway_id = id;
                    if !self.options.client && self.options.goaway == Goaway::Client && self.counters.goaway_seen == 2
                    {
                        self.respond_waiting();
                    }
                    true
                }
                _ => true,
            };
            if !going {
                return false;
            }
        }
    }

    fn datagrams_settled(&self) -> bool {
        let options = self.options;
        if options.datagrams == 0 {
            return true;
        }
        if !self.echoes.is_empty() || (options.client && self.counters.datagrams_sent < options.datagrams) {
            return false;
        }
        self.counters.datagrams_received >= options.datagrams || self.last_datagram.elapsed() >= DATAGRAM_SETTLE
    }

    /// Prefixes datagram `index`'s payload with the quarter stream ID of its tunnel (RFC 9297 section 2).
    fn datagram(&self, index: u32, tag: usize) -> Vec<u8> {
        let tunnel = u64::from(index) / self.options.per_tunnel();
        let mut payload = if tunnel < 64 {
            vec![tunnel as u8]
        } else {
            vec![(0x40 | (tunnel >> 8)) as u8, tunnel as u8]
        };
        payload.extend_from_slice(&index.to_le_bytes());
        for at in DATAGRAM_INDEX_BYTES..self.options.datagram_bytes as usize {
            payload.push(self.pattern[tag][pattern_index(u64::from(index), at as u64)]);
        }
        payload
    }

    /// Holds a received HTTP datagram to its tunnel, size, pattern and one delivery; a server sends it back.
    fn take_datagram(&mut self, data: &[u8]) {
        let per_tunnel = self.options.per_tunnel();
        let prefix = 1usize << (data.first().copied().unwrap_or(0) >> 6);
        if data.len() < prefix + DATAGRAM_INDEX_BYTES {
            self.counters.datagrams_unverified = true;
            return;
        }
        let first = u64::from(data[0] & 0x3F);
        let quarter = data[1..prefix].iter().fold(first, |value, byte| (value << 8) | u64::from(*byte));
        let payload = &data[prefix..];
        let index = u32::from_le_bytes([payload[0], payload[1], payload[2], payload[3]]);
        let tag = if self.options.client { 3 } else { 2 };
        let fresh = u64::from(index) < self.options.datagrams && !self.seen_datagrams[index as usize];
        let matches = payload.iter().enumerate().skip(DATAGRAM_INDEX_BYTES).all(|(at, byte)| {
            *byte == self.pattern[tag][pattern_index(u64::from(index), at as u64)]
        });
        if quarter != u64::from(index) / per_tunnel
            || payload.len() as u64 != self.options.datagram_bytes
            || !fresh
            || !matches
        {
            self.counters.datagrams_unverified = true;
            return;
        }
        self.seen_datagrams[index as usize] = true;
        self.counters.datagrams_received += 1;
        self.last_datagram = Instant::now();
        if !self.options.client {
            self.echoes.push_back(index);
        }
    }

    fn exchange_datagrams(&mut self, conn: &mut quiche::Connection, buffer: &mut [u8]) {
        while let Ok(size) = conn.dgram_recv(buffer) {
            self.take_datagram(&buffer[..size]);
        }
        let options = self.options;
        loop {
            let index = if options.client {
                let index = self.counters.datagrams_sent;
                let tunnel = (index / options.per_tunnel()) as usize;
                if index >= options.datagrams || !self.streams[tunnel].tunnel_open {
                    return;
                }
                index as u32
            } else {
                match self.echoes.front() {
                    Some(index) => *index,
                    None => return,
                }
            };
            let payload = self.datagram(index, if options.client { 2 } else { 3 });
            if conn.dgram_send(&payload).is_err() {
                return;
            }
            self.counters.datagrams_sent += 1;
            self.last_datagram = Instant::now();
            self.echoes.pop_front();
        }
    }

    fn open_requests(&mut self, conn: &mut quiche::Connection) -> bool {
        while self.counters.opened < self.options.requests {
            let index = self.counters.opened;
            let fields = self.request_fields(index);
            let follows = self.body_follows(index, self.options.body_bytes(index, false));
            match self.h3.as_mut().expect("HTTP/3 has started").send_request(conn, &fields, !follows) {
                Ok(id) if id == index * 4 => {
                    self.streams[index as usize].used = true;
                    self.streams[index as usize].send = if follows { Send::Body } else { Send::Nothing };
                    self.counters.opened += 1;
                }
                Ok(id) => {
                    eprintln!("request {index} opened stream {id}");
                    return false;
                }
                Err(error) if is_blocked(&error) => return true,
                Err(error) => {
                    eprintln!("request {index}: {error}");
                    return false;
                }
            }
        }
        true
    }

    /// One service turn; false when the connection cannot go on.
    fn turn(&mut self, conn: &mut quiche::Connection, config: &mut h3::Config, buffer: &mut [u8]) -> bool {
        if !conn.is_established() && !conn.is_in_early_data() {
            return true;
        }
        if self.h3.is_none() {
            match h3::Connection::with_transport(conn, config) {
                Ok(connection) => self.h3 = Some(connection),
                Err(error) => {
                    eprintln!("HTTP/3: {error}");
                    return false;
                }
            }
        }
        if !self.poll(conn, buffer) {
            return false;
        }
        if self.settings.is_none() {
            self.settings = self.h3.as_ref().and_then(|h3| h3.peer_settings_raw().map(<[_]>::to_vec));
        }
        self.exchange_datagrams(conn, buffer);
        if self.options.client && (self.settings.is_some() || conn.is_in_early_data()) && !self.open_requests(conn) {
            return false;
        }
        for index in 0..self.options.requests.min(MAX_REQUESTS as u64) as usize {
            if !self.pump(conn, index) {
                return false;
            }
        }
        self.cancel(conn);
        self.after_goaway(conn)
    }

    /// A client's probe past a GOAWAY, which its own end refuses, and a client's own two GOAWAYs.
    fn after_goaway(&mut self, conn: &mut quiche::Connection) -> bool {
        let options = self.options;
        if !options.client || self.counters.opened != options.requests {
            return true;
        }
        if options.goaway == Goaway::Notice && self.counters.goaway_seen >= 2 && self.counters.refused_after_goaway == 0
        {
            let fields = self.request_fields(0);
            let refused = self.h3.as_mut().expect("HTTP/3 has started").send_request(conn, &fields, true).is_err();
            self.counters.refused_after_goaway = u64::from(refused);
        }
        if options.goaway == Goaway::Client && self.counters.goaway_sent == 0 {
            return self.goaway(conn, 0) && self.goaway(conn, 0);
        }
        true
    }

    fn exchange_over(&self) -> bool {
        let options = self.options;
        let counters = &self.counters;
        if !options.client
            || self.settings.is_none()
            || counters.opened < options.requests
            || counters.completed + counters.rejected + counters.aborted + counters.cancelled < options.requests
        {
            return false;
        }
        match options.goaway {
            Goaway::Notice => counters.goaway_seen >= 2 && counters.refused_after_goaway != 0,
            Goaway::Reject => counters.goaway_seen >= 1,
            _ => true,
        }
    }

    fn done(&mut self, wait_for_session: bool) -> bool {
        if !self.exchange_over() || wait_for_session {
            return false;
        }
        self.done_at.get_or_insert_with(Instant::now).elapsed() >= LINGER
    }

    fn setting(&self, id: u64) -> u64 {
        self.settings.iter().flatten().find(|(name, _)| *name == id).map_or(0, |(_, value)| *value)
    }

    fn report(&self, out: &mut impl Write) -> std::io::Result<()> {
        let c = &self.counters;
        writeln!(out, "h3_requests={}\nh3_seen={}\nh3_completed={}", c.opened, c.seen, c.completed)?;
        writeln!(out, "h3_rejected={}\nh3_aborted={}\nh3_abort_code={}", c.rejected, c.aborted, c.abort_code)?;
        writeln!(out, "h3_cancelled={}\nh3_status_431={}", c.cancelled, c.status_431)?;
        writeln!(out, "h3_refused_too_large=0\nh3_refused_after_goaway={}", c.refused_after_goaway)?;
        writeln!(out, "h3_goaway_seen={}\nh3_goaway_id={}", c.goaway_seen, c.goaway_id)?;
        writeln!(out, "h3_body_received={}\nh3_body_sent={}", c.body_received, c.body_sent)?;
        writeln!(out, "h3_interims={}", c.interims)?;
        writeln!(out, "h3_trailers={}\nh3_tunnels={}\nh3_early_requests={}", c.trailers, c.tunnels, c.early_requests)?;
        writeln!(out, "h3_verified={}\nh3_peer_known={}", u8::from(!c.unverified), u8::from(self.settings.is_some()))?;
        writeln!(out, "h3_peer_table={}", self.setting(SETTING_TABLE))?;
        writeln!(out, "h3_peer_blocked={}", self.setting(SETTING_BLOCKED))?;
        writeln!(out, "h3_peer_field_limit={}", self.setting(SETTING_FIELD_LIMIT))?;
        writeln!(out, "h3_peer_connect={}", self.setting(SETTING_CONNECT))?;
        writeln!(out, "h3_peer_datagram={}", self.setting(SETTING_DATAGRAM))?;
        writeln!(out, "datagrams_sent={}\ndatagrams_received={}", c.datagrams_sent, c.datagrams_received)?;
        writeln!(out, "datagrams_verified={}", u8::from(!c.datagrams_unverified))
    }
}

fn flush(conn: &mut quiche::Connection, socket: &UdpSocket, packet: &mut [u8]) {
    loop {
        match conn.send(packet) {
            Ok((size, info)) => {
                // Loopback buffers fill during a burst; a dropped packet is recovery's to resend.
                let _ = socket.send_to(&packet[..size], info.to);
            }
            Err(quiche::Error::Done) => return,
            Err(error) => {
                eprintln!("send: {error}");
                let _ = conn.close(false, 0x1, b"send failed");
                return;
            }
        }
    }
}

/// Runs one connection until it has ended. `first` is what a server has already read of it.
fn drive(
    conn: &mut quiche::Connection,
    socket: &UdpSocket,
    app: &mut App,
    h3_config: &mut h3::Config,
    keep_session: bool,
    first: Option<(Vec<u8>, SocketAddr)>,
) -> std::io::Result<()> {
    let local = socket.local_addr()?;
    let mut packet = vec![0u8; PACKET_BYTES];
    let mut buffer = vec![0u8; PACKET_BYTES];
    let deadline = Instant::now() + RUN;
    let code = app.options.code;
    let mut closing = false;
    if let Some((mut datagram, from)) = first {
        let _ = conn.recv(&mut datagram, quiche::RecvInfo { to: local, from });
    }
    while !conn.is_closed() && Instant::now() < deadline {
        if !closing && !app.turn(conn, h3_config, &mut buffer) {
            let _ = conn.close(true, 0x102, b"the exchange failed");
            closing = true;
        }
        if !closing && app.done(keep_session && conn.session().is_none()) {
            let _ = conn.close(true, code, b"");
            closing = true;
        }
        flush(conn, socket, &mut packet);
        let wait = conn.timeout().unwrap_or(SERVICE_WAIT).min(SERVICE_WAIT);
        socket.set_read_timeout(Some(wait.max(Duration::from_micros(100))))?;
        match socket.recv_from(&mut packet) {
            Ok((size, from)) => {
                let _ = conn.recv(&mut packet[..size], quiche::RecvInfo { to: local, from });
            }
            Err(error) if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(error) => return Err(error),
        }
        if conn.timeout().is_some_and(|left| left.is_zero()) {
            conn.on_timeout();
        }
    }
    Ok(())
}

fn close_kind(conn: &quiche::Connection) -> (&'static str, u64) {
    if let Some(error) = conn.local_error() {
        return ("local", error.error_code);
    }
    if let Some(error) = conn.peer_error() {
        return (if error.is_app { "application" } else { "transport" }, error.error_code);
    }
    (if conn.is_timed_out() { "idle" } else { "none" }, 0)
}

fn report(number: u64, conn: &quiche::Connection, app: &App, offered_early: bool) -> std::io::Result<()> {
    let mut out = std::io::stdout().lock();
    let (close, close_code) = close_kind(conn);
    let accepted = conn.early_data_reason() == EARLY_DATA_ACCEPTED;
    let early = if offered_early && accepted {
        "accepted"
    } else if offered_early {
        "rejected"
    } else {
        "none"
    };
    let alpn = String::from_utf8_lossy(conn.application_proto()).into_owned();
    let established = u8::from(conn.is_established() || app.h3.is_some());
    writeln!(out, "connection={number}\nestablished={established}\nversion={}", app.options.version)?;
    writeln!(out, "resumed={}\nearly={early}", u8::from(conn.is_resumed()))?;
    writeln!(out, "close={close}\nclose_code={close_code}\nalpn={alpn}")?;
    app.report(&mut out)?;
    out.flush()
}

fn key_of(seed: &[u8]) -> KeyPair {
    let mut pkcs8 = ED25519_PKCS8_PREFIX.to_vec();
    pkcs8.extend_from_slice(seed);
    KeyPair::from_pkcs8_der_and_sign_algo(&PrivatePkcs8KeyDer::from(pkcs8), &PKCS_ED25519).expect("an Ed25519 seed")
}

/// quiche reads its identity from PEM files: a certificate self-signed from the seed, for as long as the loading takes.
fn load_identity(config: &mut quiche::Config, seed: &[u8]) {
    let key = key_of(seed);
    let certificate = CertificateParams::new(vec![SERVER_NAME.to_owned()])
        .expect("the server's name")
        .self_signed(&key)
        .expect("a self-signed certificate");
    let directory = std::env::temp_dir().join(format!("crd-quiche-peer-{}", std::process::id()));
    fs::create_dir_all(&directory).expect("a directory for the identity");
    let certificate_path = directory.join("certificate.pem");
    let key_path = directory.join("key.pem");
    fs::write(&certificate_path, certificate.pem()).expect("the certificate written");
    fs::write(&key_path, key.serialize_pem()).expect("the key written");
    config.load_cert_chain_from_pem_file(certificate_path.to_str().expect("a UTF-8 path")).expect("the certificate");
    config.load_priv_key_from_pem_file(key_path.to_str().expect("a UTF-8 path")).expect("the key");
    let _ = fs::remove_dir_all(&directory);
}

fn make_config(options: &Options, early: bool) -> quiche::Config {
    // BoringSSL leaves Ed25519 out of what a client verifies by default, and the peers of this test sign with it.
    let mut tls = SslContextBuilder::new(SslMethod::tls()).expect("a TLS context");
    tls.set_verify_algorithm_prefs(&[SslSignatureAlgorithm::ED25519]).expect("the signature algorithm");
    let mut config =
        quiche::Config::with_boring_ssl_ctx_builder(options.version, tls).expect("a QUIC configuration");
    config.set_application_protos(&[ALPN]).expect("the ALPN");
    config.set_curves_list(&options.group).expect("the key exchange group");
    config.set_max_idle_timeout(RUN.as_millis() as u64);
    config.set_max_recv_udp_payload_size(PACKET_BYTES);
    config.set_max_send_udp_payload_size(SEND_PAYLOAD_BYTES);
    config.set_initial_max_data(1 << 22);
    config.set_initial_max_stream_data_bidi_local(1 << 20);
    config.set_initial_max_stream_data_bidi_remote(1 << 20);
    config.set_initial_max_stream_data_uni(1 << 20);
    config.set_initial_max_streams_bidi(options.max_streams);
    config.set_initial_max_streams_uni(16);
    config.set_disable_active_migration(true);
    config.verify_peer(false);
    if options.datagrams != 0 {
        config.enable_dgram(true, MAX_REQUESTS * 4, MAX_REQUESTS * 4);
    }
    if early {
        config.enable_early_data();
    }
    config.set_ticket_key(&TICKET_KEY).expect("the ticket key");
    if !options.client {
        load_identity(&mut config, &options.seed);
    }
    config
}

fn h3_config(options: &Options) -> h3::Config {
    let mut config = h3::Config::new().expect("an HTTP/3 configuration");
    config.set_max_field_section_size(options.field_limit);
    config.enable_extended_connect(!options.client);
    config
}

fn pin_holds(conn: &quiche::Connection, seed: &[u8]) -> bool {
    let public = key_of(seed).public_key_raw().to_vec();
    conn.peer_cert().is_some_and(|der| der.windows(public.len()).any(|window| window == public))
}

fn connection_id(role: u8, number: u64) -> [u8; CONNECTION_ID_BYTES] {
    [role | number as u8; CONNECTION_ID_BYTES]
}

fn run_client(options: &Options, pattern: &[Vec<u8>; 4]) -> std::io::Result<bool> {
    let socket = UdpSocket::bind("127.0.0.1:0")?;
    let peer: SocketAddr = format!("127.0.0.1:{}", options.port).parse().expect("a loopback address");
    let local = socket.local_addr()?;
    let mut config = make_config(options, options.early);
    let mut h3_settings = h3_config(options);
    let mut session: Option<Vec<u8>> = None;
    let mut all_good = true;
    for number in 1..=options.connections {
        let id = connection_id(0xA0, number);
        let scid = quiche::ConnectionId::from_ref(&id);
        let mut conn = quiche::connect(Some(SERVER_NAME), &scid, local, peer, &mut config).expect("a QUIC connection");
        if let Some(saved) = &session {
            conn.set_session(saved).expect("the saved session");
        }
        let offered_early = options.early && session.is_some();
        let mut app = App::new(options, pattern);
        let keep_session = number < options.connections;
        drive(&mut conn, &socket, &mut app, &mut h3_settings, keep_session, None)?;
        all_good = all_good && pin_holds(&conn, &options.seed);
        session = conn.session().map(<[u8]>::to_vec);
        report(number, &conn, &app, offered_early)?;
    }
    Ok(all_good)
}

/// The next Initial of a new connection; what a connection left behind is dropped.
fn wait_for_initial(socket: &UdpSocket, deadline: Instant) -> std::io::Result<Option<(Vec<u8>, SocketAddr)>> {
    let mut buffer = vec![0u8; PACKET_BYTES];
    socket.set_read_timeout(Some(SERVICE_WAIT))?;
    while Instant::now() < deadline {
        match socket.recv_from(&mut buffer) {
            Ok((size, from)) => {
                let header = quiche::Header::from_slice(&mut buffer[..size], CONNECTION_ID_BYTES);
                if header.is_ok_and(|header| header.ty == quiche::Type::Initial) {
                    return Ok(Some((buffer[..size].to_vec(), from)));
                }
            }
            Err(error) if matches!(error.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {}
            Err(error) => return Err(error),
        }
    }
    Ok(None)
}

fn run_server(options: &Options, pattern: &[Vec<u8>; 4]) -> std::io::Result<bool> {
    let socket = UdpSocket::bind(("127.0.0.1", options.port))?;
    let local = socket.local_addr()?;
    println!("port={}", local.port());
    std::io::stdout().flush()?;
    let mut accepting = make_config(options, options.early);
    let mut refusing = make_config(options, false);
    let mut h3_settings = h3_config(options);
    let deadline = Instant::now() + RUN;
    for number in 1..=options.connections {
        let Some((datagram, from)) = wait_for_initial(&socket, deadline)? else {
            return Ok(false);
        };
        let id = connection_id(0xC0, number);
        let scid = quiche::ConnectionId::from_ref(&id);
        let config = if options.reject_early && number > 1 { &mut refusing } else { &mut accepting };
        let mut conn = quiche::accept(&scid, None, local, from, config).expect("a QUIC connection");
        let mut app = App::new(options, pattern);
        drive(&mut conn, &socket, &mut app, &mut h3_settings, false, Some((datagram, from)))?;
        report(number, &conn, &app, false)?;
    }
    Ok(true)
}

fn fill_pattern() -> [Vec<u8>; 4] {
    std::array::from_fn(|tag| {
        (0..PATTERN_BYTES)
            .map(|at| at.wrapping_mul(31).wrapping_add(at >> 8).wrapping_add(tag.wrapping_mul(7)) as u8)
            .collect()
    })
}

fn parse_hex(text: &str) -> Vec<u8> {
    let byte = |at: usize| u8::from_str_radix(&text[at * 2..at * 2 + 2], 16).expect("a hexadecimal seed");
    (0..text.len() / 2).map(byte).collect()
}

fn parse_options() -> Options {
    let mut options =
        Options { version: 1, field_limit: 16384, connections: 1, datagram_bytes: 900, ..Options::default() };
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    for pair in arguments.chunks(2) {
        let [name, value] = pair else { panic!("{pair:?} has no value") };
        let number = || value.parse::<u64>().expect("a number");
        match name.as_str() {
            "--role" => options.client = value == "client",
            "--port" => options.port = value.parse().expect("a port"),
            "--seed" => options.seed = parse_hex(value),
            "--group" => options.group.clone_from(value),
            "--version" => options.version = value.parse().expect("a version"),
            "--code" => options.code = number(),
            "--max-streams" => options.max_streams = number(),
            "--requests" => options.requests = number(),
            "--request-bytes" => options.request_bytes = number(),
            "--response-bytes" => options.response_bytes = number(),
            "--tunnels" => options.tunnels = number(),
            "--field-bytes" => options.field_bytes = number(),
            "--field-limit" => options.field_limit = number(),
            "--trailers" => options.trailers = number() != 0,
            "--interim" => options.interim = number() != 0,
            "--over-limit" => options.over_limit = number() != 0,
            "--datagrams" => options.datagrams = number(),
            "--datagram-bytes" => options.datagram_bytes = number(),
            "--connections" => options.connections = number(),
            "--early" => options.early = number() != 0,
            "--reject-early" => options.reject_early = number() != 0,
            "--goaway" => {
                options.goaway = match value.as_str() {
                    "notice" => Goaway::Notice,
                    "reject" => Goaway::Reject,
                    "client" => Goaway::Client,
                    _ => Goaway::None,
                };
            }
            "--cancel" => {
                options.cancel = match value.as_str() {
                    "client" => Cancel::Client,
                    "server" => Cancel::Server,
                    _ => Cancel::None,
                };
            }
            // QPACK's limits (this end has no dynamic table), and what the shared harness passes for other peers.
            "--table" | "--blocked" | "--cipher" | "--streams" | "--bytes" | "--closer" | "--http3" => {}
            other => panic!("unknown option {other}"),
        }
    }
    options
}

fn main() -> std::process::ExitCode {
    let options = parse_options();
    let pattern = fill_pattern();
    let result = if options.client { run_client(&options, &pattern) } else { run_server(&options, &pattern) };
    match result {
        Ok(true) => std::process::ExitCode::SUCCESS,
        Ok(false) => {
            eprintln!("a connection failed its pin or never arrived");
            std::process::ExitCode::FAILURE
        }
        Err(error) => {
            eprintln!("{error}");
            std::process::ExitCode::FAILURE
        }
    }
}
