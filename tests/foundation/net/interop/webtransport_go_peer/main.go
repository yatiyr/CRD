// Command crd-webtransport-go-peer is a WebTransport endpoint over webtransport-go and quic-go, real UDP on the
// loopback, the counterpart of crd-quic-peer's --webtransport mode. webtransport_interop.py runs one of each and reads
// both reports.
//
// The exchange is interop_webtransport.hpp's, byte for byte, and so is the report. The certificate is self-signed from
// the seed; a client pins its key. This module speaks `webtransport-h3` at draft -16, with flow control.
package main

import (
	"context"
	"crypto/ed25519"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"math/big"
	"net"
	"net/http"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/quic-go/quic-go"
	"github.com/quic-go/quic-go/http3"
	"github.com/quic-go/webtransport-go"
)

const (
	serverName         = "cerid.test"
	sessionURL         = "https://cerid.test/wt"
	runTime            = 60 * time.Second
	closeGrace         = 500 * time.Millisecond
	datagramSettle     = time.Second
	labelBytes         = 8
	datagramIndexBytes = 4
	chunkBytes         = 4096
	kindControl        = 0
	kindEcho           = 1
	kindUni            = 2
	kindReset          = 3
	kindStop           = 4
	kindReply          = 0x80
	ack                = 0xA5
	resetBase          = 0x10000
	stopBase           = 0x20000
	closeCode          = 0x0C1D05E
	closeReason        = "cerid-interop done"
	chosenProtocol     = "cerid-b"
	clientDatagramTag  = 16
	serverDatagramTag  = 17
	maxStreams         = 4096
	flowStreams        = 2048
)

var offeredProtocols = []string{"cerid-a", chosenProtocol}

type options struct {
	client        bool
	clientCloses  bool
	port          int
	seed          []byte
	group         string
	streams       uint32
	bytes         uint64
	code          uint64
	uni           uint32
	uniBytes      uint64
	resets        uint32
	stops         uint32
	datagrams     uint32
	datagramBytes int
	reject        int
	protocols     bool
	origin        string
	window        int64
}

type counters struct {
	echo, uni                        atomic.Uint64
	resetsSent, resetsSeen           atomic.Uint64
	stopsSent, stopsSeen             atomic.Uint64
	bytesReceived, bytesSent         atomic.Uint64
	datagramsSent, datagramsReceived atomic.Uint64
	failed                           atomic.Bool
}

func (c *counters) check(good bool) {
	if !good {
		c.failed.Store(true)
	}
}

// outcome is what a report says of how the session began and ended.
type outcome struct {
	opened      bool
	status      int
	protocol    string
	origin      string
	controlDone bool
	closeSeen   bool
	closeCode   uint64
	closeReason string
}

// exchange is one session's exchange: what its goroutines share.
type exchange struct {
	options       *options
	session       *webtransport.Session
	counters      counters
	finished      chan struct{}
	control       chan *webtransport.Stream
	datagramsLock sync.Mutex
	datagramsSeen []bool
	datagramsDone chan struct{}
}

func pattern(tag, index, offset uint64) byte {
	at := (offset + index*37) & 0xFFFF
	return byte(at*31 + at>>8 + tag*7)
}

func label(kind byte, index uint32) []byte {
	bytes := make([]byte, labelBytes)
	bytes[0] = kind
	binary.BigEndian.PutUint32(bytes[4:], index)
	return bytes
}

func parseLabel(bytes []byte) (byte, uint32) {
	return bytes[0], binary.BigEndian.Uint32(bytes[4:])
}

func (e *exchange) writeBody(writer io.Writer, tag byte, index uint32, total uint64) error {
	chunk := make([]byte, chunkBytes)
	for offset := uint64(0); offset < total; {
		size := min(total-offset, chunkBytes)
		for at := range size {
			chunk[at] = pattern(uint64(tag), uint64(index), offset+at)
		}
		if _, err := writer.Write(chunk[:size]); err != nil {
			return err
		}
		offset += size
	}
	e.counters.bytesSent.Add(total)
	return nil
}

// readBody reads everything up to the stream's FIN, each byte checked.
func (e *exchange) readBody(reader io.Reader, tag byte, index uint32, total uint64) error {
	buffer := make([]byte, chunkBytes)
	offset := uint64(0)
	for {
		size, err := reader.Read(buffer)
		for at := range uint64(size) {
			e.counters.check(buffer[at] == pattern(uint64(tag), uint64(index), offset+at))
		}
		offset += uint64(size)
		if errors.Is(err, io.EOF) {
			break
		}
		if err != nil {
			return err
		}
	}
	e.counters.check(offset == total)
	e.counters.bytesReceived.Add(offset)
	return nil
}

func (e *exchange) finishItem(good bool) {
	e.counters.check(good)
	e.finished <- struct{}{}
}

func count(counter *atomic.Uint64, good bool) {
	if good {
		counter.Add(1)
	}
}

func readHead(reader io.Reader) ([]byte, error) {
	head := make([]byte, labelBytes)
	_, err := io.ReadFull(reader, head)
	return head, err
}

func (e *exchange) openEcho(ctx context.Context, index uint32) {
	good := func() bool {
		stream, err := e.session.OpenStreamSync(ctx)
		if err != nil {
			return false
		}
		written := make(chan error, 1)
		go func() {
			written <- e.writeLabelled(stream, kindEcho, index, e.options.bytes)
		}()
		head, err := readHead(stream)
		if err != nil {
			return false
		}
		kind, at := parseLabel(head)
		e.counters.check(kind == kindEcho|kindReply && at == index)
		if e.readBody(stream, kindEcho|kindReply, index, e.options.bytes) != nil {
			return false
		}
		return <-written == nil
	}()
	count(&e.counters.echo, good)
	e.finishItem(good)
}

// writeLabelled sends a stream's label, its body and a FIN.
func (e *exchange) writeLabelled(stream io.WriteCloser, kind byte, index uint32, total uint64) error {
	if _, err := stream.Write(label(kind, index)); err != nil {
		return err
	}
	if err := e.writeBody(stream, kind, index, total); err != nil {
		return err
	}
	return stream.Close()
}

func (e *exchange) openUni(ctx context.Context, index uint32) {
	good := func() bool {
		stream, err := e.session.OpenUniStreamSync(ctx)
		if err != nil {
			return false
		}
		return e.writeLabelled(stream, kindUni, index, e.options.uniBytes) == nil
	}()
	count(&e.counters.uni, good)
	e.finishItem(good)
}

func readToEnd(reader io.Reader, limit int64) ([]byte, error) {
	return io.ReadAll(io.LimitReader(reader, limit))
}

func (e *exchange) openReset(ctx context.Context, index uint32) {
	good := func() bool {
		stream, err := e.session.OpenStreamSync(ctx)
		if err != nil {
			return false
		}
		if _, err := stream.Write(label(kindReset, index)); err != nil {
			return false
		}
		answer, err := readToEnd(stream, 2)
		if err != nil {
			return false
		}
		e.counters.check(len(answer) == 1 && answer[0] == ack)
		stream.CancelWrite(webtransport.StreamErrorCode(resetBase + index))
		return true
	}()
	count(&e.counters.resetsSent, good)
	e.finishItem(good)
}

func (e *exchange) openStop(ctx context.Context, index uint32) {
	good := func() bool {
		stream, err := e.session.OpenStreamSync(ctx)
		if err != nil {
			return false
		}
		if _, err := stream.Write(label(kindStop, index)); err != nil {
			return false
		}
		select {
		case <-stream.Context().Done():
		case <-ctx.Done():
			return false
		}
		// The context says that the write side ended; a write says with which code.
		_, err = stream.Write([]byte{0})
		var stopped *webtransport.StreamError
		e.counters.check(errors.As(err, &stopped) && stopped.Remote &&
			uint32(stopped.ErrorCode) == stopBase+index)
		rest, err := readToEnd(stream, 1)
		e.counters.check(len(rest) == 0)
		return err == nil
	}()
	count(&e.counters.stopsSeen, good)
	e.finishItem(good)
}

func (e *exchange) serveBidirectional(stream *webtransport.Stream) {
	head, err := readHead(stream)
	if err != nil {
		e.counters.check(false)
		return
	}
	kind, index := parseLabel(head)
	switch kind {
	case kindControl:
		e.control <- stream
	case kindEcho:
		good := e.readBody(stream, kindEcho, index, e.options.bytes) == nil &&
			e.writeLabelled(stream, kindEcho|kindReply, index, e.options.bytes) == nil
		count(&e.counters.echo, good)
		e.finishItem(good)
	case kindReset:
		good := func() bool {
			if _, err := stream.Write([]byte{ack}); err != nil {
				return false
			}
			if stream.Close() != nil {
				return false
			}
			_, err := readToEnd(stream, 16)
			var reset *webtransport.StreamError
			return errors.As(err, &reset) && reset.Remote && uint32(reset.ErrorCode) == resetBase+index
		}()
		count(&e.counters.resetsSeen, good)
		e.finishItem(good)
	case kindStop:
		stream.CancelRead(webtransport.StreamErrorCode(stopBase + index))
		good := stream.Close() == nil
		count(&e.counters.stopsSent, good)
		e.finishItem(good)
	default:
		e.finishItem(false)
	}
}

func (e *exchange) serveUnidirectional(stream *webtransport.ReceiveStream) {
	good := func() bool {
		head, err := readHead(stream)
		if err != nil {
			return false
		}
		kind, index := parseLabel(head)
		e.counters.check(kind == kindUni)
		return e.readBody(stream, kindUni, index, e.options.uniBytes) == nil
	}()
	count(&e.counters.uni, good)
	e.finishItem(good)
}

func (e *exchange) acceptBidirectional() {
	for {
		stream, err := e.session.AcceptStream(context.Background())
		if err != nil {
			return
		}
		go e.serveBidirectional(stream)
	}
}

func (e *exchange) acceptUnidirectional() {
	for {
		stream, err := e.session.AcceptUniStream(context.Background())
		if err != nil {
			return
		}
		go e.serveUnidirectional(stream)
	}
}

func (e *exchange) sendDatagrams() {
	tag := uint64(serverDatagramTag)
	if e.options.client {
		tag = clientDatagramTag
	}
	for index := range e.options.datagrams {
		payload := make([]byte, e.options.datagramBytes)
		binary.BigEndian.PutUint32(payload, index)
		for at := datagramIndexBytes; at < len(payload); at++ {
			payload[at] = pattern(tag, uint64(index), uint64(at))
		}
		sent := e.session.SendDatagram(payload) == nil
		e.counters.check(sent)
		count(&e.counters.datagramsSent, sent)
	}
}

func (e *exchange) readDatagrams() {
	tag := uint64(clientDatagramTag)
	if e.options.client {
		tag = serverDatagramTag
	}
	for {
		payload, err := e.session.ReceiveDatagram(context.Background())
		if err != nil {
			return
		}
		e.readDatagram(payload, tag)
	}
}

func (e *exchange) readDatagram(payload []byte, tag uint64) {
	if len(payload) != e.options.datagramBytes || len(payload) < datagramIndexBytes {
		e.counters.check(false)
		return
	}
	index := binary.BigEndian.Uint32(payload)
	if index >= e.options.datagrams {
		e.counters.check(false)
		return
	}
	for at := datagramIndexBytes; at < len(payload); at++ {
		e.counters.check(payload[at] == pattern(tag, uint64(index), uint64(at)))
	}
	e.datagramsLock.Lock()
	defer e.datagramsLock.Unlock()
	if !e.datagramsSeen[index] {
		e.datagramsSeen[index] = true
		if e.counters.datagramsReceived.Add(1) == uint64(e.options.datagrams) {
			close(e.datagramsDone)
		}
	}
}

// exchangeControl writes this end's acknowledgement on the control stream and reads the other's: then nothing is unread
// at either.
func (e *exchange) exchangeControl(ctx context.Context, stream *webtransport.Stream) bool {
	if stream == nil {
		select {
		case stream = <-e.control:
		case <-ctx.Done():
			return false
		}
	}
	if _, err := stream.Write([]byte{ack}); err != nil {
		return false
	}
	if stream.Close() != nil {
		return false
	}
	answer, err := readToEnd(stream, 2)
	return err == nil && len(answer) == 1 && answer[0] == ack
}

func flag(value bool) int {
	if value {
		return 1
	}
	return 0
}

func (e *exchange) report(result *outcome) {
	c := &e.counters
	fmt.Printf("connection=1\nestablished=1\nwt_opened=%d\n", flag(result.opened))
	fmt.Printf("wt_status=%d\nwt_refused_error=0\nwt_legacy=0\n", result.status)
	fmt.Printf("wt_protocol=%s\nwt_origin=%s\n", result.protocol, result.origin)
	fmt.Printf("wt_echo=%d\nwt_uni=%d\n", c.echo.Load(), c.uni.Load())
	fmt.Printf("wt_resets_sent=%d\nwt_resets_seen=%d\n", c.resetsSent.Load(), c.resetsSeen.Load())
	fmt.Printf("wt_stops_sent=%d\nwt_stops_seen=%d\n", c.stopsSent.Load(), c.stopsSeen.Load())
	fmt.Printf("wt_bytes_received=%d\nwt_bytes_sent=%d\n", c.bytesReceived.Load(), c.bytesSent.Load())
	fmt.Printf("wt_datagrams_sent=%d\nwt_datagrams_received=%d\n", c.datagramsSent.Load(), c.datagramsReceived.Load())
	fmt.Printf("wt_control_done=%d\nwt_verified=%d\n", flag(result.controlDone), flag(!c.failed.Load()))
	fmt.Printf("wt_close_seen=%d\nwt_close_code=%d\n", flag(result.closeSeen), result.closeCode)
	fmt.Printf("wt_close_reason=%s\nwt_close_error=0\n", result.closeReason)
}

// waitForItems is true when every item of both ends finished within the run.
func (e *exchange) waitForItems(ctx context.Context, total uint64) bool {
	for done := uint64(0); done < total; done++ {
		select {
		case <-e.finished:
		case <-ctx.Done():
			return false
		}
	}
	return true
}

// readEnd reads why the session ended into `result`, once it has.
func (e *exchange) readEnd(ctx context.Context, result *outcome) {
	select {
	case <-e.session.Context().Done():
	case <-ctx.Done():
		e.counters.check(false)
		return
	}
	_, err := e.session.AcceptStream(context.Background())
	var ended *webtransport.SessionError
	if !errors.As(err, &ended) {
		fmt.Fprintf(os.Stderr, "crd-webtransport-go-peer: the session ended with %v\n", err)
		e.counters.check(false)
		return
	}
	if ended.Remote {
		result.closeSeen = true
		result.closeCode = uint64(ended.ErrorCode)
		result.closeReason = ended.Message
	}
}

func runSession(session *webtransport.Session, connection *quic.Conn, o *options, result outcome) {
	items := uint64(o.streams) + uint64(o.uni) + uint64(o.resets) + uint64(o.stops)
	e := &exchange{
		options:       o,
		session:       session,
		finished:      make(chan struct{}, 2*items),
		control:       make(chan *webtransport.Stream, 1),
		datagramsSeen: make([]bool, o.datagrams),
		datagramsDone: make(chan struct{}),
	}
	ctx, cancel := context.WithTimeout(context.Background(), runTime)
	defer cancel()
	go e.acceptBidirectional()
	go e.acceptUnidirectional()
	var opened *webtransport.Stream
	if o.client {
		stream, err := session.OpenStreamSync(ctx)
		e.counters.check(err == nil)
		if err == nil {
			_, err = stream.Write(label(kindControl, 0))
			e.counters.check(err == nil)
			opened = stream
		}
	}
	for index := range o.streams {
		go e.openEcho(ctx, index)
	}
	for index := range o.uni {
		go e.openUni(ctx, index)
	}
	for index := range o.resets {
		go e.openReset(ctx, index)
	}
	for index := range o.stops {
		go e.openStop(ctx, index)
	}
	go e.readDatagrams()
	go e.sendDatagrams()

	e.counters.check(e.waitForItems(ctx, 2*items))
	if o.datagrams != 0 {
		select {
		case <-e.datagramsDone:
		case <-time.After(datagramSettle):
		}
	}
	result.controlDone = e.exchangeControl(ctx, opened)

	closes := o.clientCloses == o.client
	if closes {
		e.counters.check(session.CloseWithError(closeCode, closeReason) == nil)
		result.closeCode = closeCode
	}
	e.readEnd(ctx, &result)
	if closes {
		select {
		case <-connection.Context().Done():
		case <-time.After(closeGrace):
		}
	} else {
		e.counters.check(connection.CloseWithError(quic.ApplicationErrorCode(o.code), "") == nil)
	}
	e.report(&result)
}

// reportRefused has a session that never opened still report, so the driver reads why.
func reportRefused(status int) {
	fmt.Printf("connection=1\nestablished=1\nwt_opened=0\nwt_status=%d\nwt_verified=1\n", status)
}

func curvesOf(group string) []tls.CurveID {
	switch group {
	case "x25519mlkem768":
		return []tls.CurveID{tls.X25519MLKEM768}
	case "x25519":
		return []tls.CurveID{tls.X25519}
	}
	log.Fatalf("unknown group %s", group)
	return nil
}

// tlsConfig is one side's TLS 1.3: the cipher is the Cerid end's, since crypto/tls does not let TLS 1.3's be chosen.
func tlsConfig(o *options) *tls.Config {
	return &tls.Config{
		MinVersion:       tls.VersionTLS13,
		NextProtos:       []string{http3.NextProtoH3},
		CurvePreferences: curvesOf(o.group),
		ServerName:       serverName,
	}
}

func quicConfig() *quic.Config {
	return &quic.Config{
		EnableDatagrams:                  true,
		EnableStreamResetPartialDelivery: true,
		MaxIncomingStreams:               maxStreams,
		MaxIncomingUniStreams:            maxStreams,
	}
}

// sessionConfig turns WebTransport's flow control on when the run has a window, with stream limits that do not bind.
func sessionConfig(o *options) *webtransport.Config {
	if o.window == 0 {
		return nil
	}
	return &webtransport.Config{
		MaxIncomingData:       o.window,
		MaxIncomingStreams:    flowStreams,
		MaxIncomingUniStreams: flowStreams,
	}
}

func runClient(o *options) {
	key := ed25519.NewKeyFromSeed(o.seed)
	config := tlsConfig(o)
	config.InsecureSkipVerify = true
	config.VerifyConnection = func(state tls.ConnectionState) error {
		public, isEd25519 := state.PeerCertificates[0].PublicKey.(ed25519.PublicKey)
		if !isEd25519 || !public.Equal(key.Public()) {
			return errors.New("the server's key is not the pinned one")
		}
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), runTime)
	defer cancel()
	connection, err := quic.DialAddr(ctx, net.JoinHostPort("127.0.0.1", strconv.Itoa(o.port)), config, quicConfig())
	if err != nil {
		log.Fatalf("a handshake: %v", err)
	}
	transport := &webtransport.Transport{Config: sessionConfig(o)}
	if o.protocols {
		transport.ApplicationProtocols = offeredProtocols
	}
	client, err := transport.NewClientConn(connection)
	if err != nil {
		log.Fatalf("a client connection: %v", err)
	}
	header := http.Header{}
	if o.origin != "" {
		header.Set("Origin", o.origin)
	}
	response, session, err := client.Dial(ctx, sessionURL, header)
	if err != nil {
		if response == nil {
			log.Fatalf("the session did not open: %v", err)
		}
		reportRefused(response.StatusCode)
		if err := connection.CloseWithError(quic.ApplicationErrorCode(o.code), ""); err != nil {
			log.Fatalf("closing: %v", err)
		}
		return
	}
	protocol := session.SessionState().ApplicationProtocol
	runSession(session, connection, o, outcome{opened: true, status: http.StatusOK, protocol: protocol})
}

func selfSigned(key ed25519.PrivateKey) tls.Certificate {
	now := time.Now()
	template := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: serverName},
		DNSNames:     []string{serverName},
		NotBefore:    now.Add(-time.Hour),
		NotAfter:     now.Add(24 * time.Hour),
	}
	der, err := x509.CreateCertificate(rand.Reader, template, template, key.Public(), key)
	if err != nil {
		log.Fatalf("a certificate: %v", err)
	}
	return tls.Certificate{Certificate: [][]byte{der}, PrivateKey: key}
}

func runServer(o *options) {
	udp, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: o.port})
	if err != nil {
		log.Fatalf("a socket: %v", err)
	}
	fmt.Printf("port=%d\n", udp.LocalAddr().(*net.UDPAddr).Port)
	config := tlsConfig(o)
	config.Certificates = []tls.Certificate{selfSigned(ed25519.NewKeyFromSeed(o.seed))}
	listener, err := quic.Listen(udp, config, quicConfig())
	if err != nil {
		log.Fatalf("a listener: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), runTime)
	defer cancel()
	connection, err := listener.Accept(ctx)
	if err != nil {
		log.Fatalf("a connection: %v", err)
	}

	finished := make(chan struct{})
	refused := make(chan string, 1)
	server := &webtransport.Server{
		H3:          &http3.Server{},
		Config:      sessionConfig(o),
		CheckOrigin: func(*http.Request) bool { return true },
	}
	if o.protocols {
		server.ApplicationProtocols = []string{chosenProtocol}
	}
	server.H3.Handler = http.HandlerFunc(func(writer http.ResponseWriter, request *http.Request) {
		origin := request.Header.Get("Origin")
		if o.reject != 0 {
			writer.WriteHeader(o.reject)
			refused <- origin
			return
		}
		defer close(finished)
		session, err := server.Upgrade(writer, request)
		if err != nil {
			log.Fatalf("an upgrade: %v", err)
		}
		protocol := session.SessionState().ApplicationProtocol
		runSession(session, connection, o, outcome{opened: true, status: http.StatusOK, protocol: protocol, origin: origin})
	})
	go func() {
		if err := server.ServeQUICConn(connection); err != nil {
			log.Fatalf("serving: %v", err)
		}
	}()
	select {
	case <-finished:
	case origin := <-refused:
		// The handler returns for the answer to leave, and the client that read it closes the connection.
		select {
		case <-connection.Context().Done():
		case <-ctx.Done():
			log.Fatalf("the client never closed the refused connection")
		}
		reportRefused(o.reject)
		fmt.Printf("wt_origin=%s\n", origin)
	case <-ctx.Done():
		log.Fatalf("no session ended within the run")
	}
}

func parseOptions() *options {
	o := &options{}
	arguments := os.Args[1:]
	if len(arguments)%2 != 0 {
		log.Fatalf("an option has no value")
	}
	for at := 0; at < len(arguments); at += 2 {
		name, value := arguments[at], arguments[at+1]
		number := func() uint64 {
			parsed, err := strconv.ParseUint(value, 10, 64)
			if err != nil {
				log.Fatalf("%s: %v", name, err)
			}
			return parsed
		}
		switch name {
		case "--role":
			o.client = value == "client"
		case "--closer":
			o.clientCloses = value == "client"
		case "--port":
			o.port = int(number())
		case "--seed":
			seed, err := hex.DecodeString(value)
			if err != nil {
				log.Fatalf("a hexadecimal seed: %v", err)
			}
			o.seed = seed
		case "--group":
			o.group = value
		case "--streams":
			o.streams = uint32(number())
		case "--bytes":
			o.bytes = number()
		case "--code":
			o.code = number()
		case "--uni":
			o.uni = uint32(number())
		case "--uni-bytes":
			o.uniBytes = number()
		case "--resets":
			o.resets = uint32(number())
		case "--stops":
			o.stops = uint32(number())
		case "--datagrams":
			o.datagrams = uint32(number())
		case "--datagram-bytes":
			o.datagramBytes = int(number())
		case "--wt-reject":
			o.reject = int(number())
		case "--wt-protocols":
			o.protocols = value != "0"
		case "--wt-origin":
			o.origin = value
		case "--wt-window":
			o.window = int64(number())
		case "--cipher", "--version", "--max-streams", "--webtransport", "--wt-allow":
		default:
			log.Fatalf("unknown option %s", name)
		}
	}
	return o
}

func main() {
	o := parseOptions()
	if o.client {
		runClient(o)
	} else {
		runServer(o)
	}
}
