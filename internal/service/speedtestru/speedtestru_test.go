package speedtestru

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestSelectServerDefaultsPort(t *testing.T) {
	p := New(Options{Server: "vladivostok.qms.ru"})
	server, err := p.SelectServer(context.Background())
	if err != nil {
		t.Fatalf("SelectServer() error = %v", err)
	}
	if server.Name != "vladivostok.qms.ru:20000" {
		t.Errorf("Server.Name = %q, want default port appended", server.Name)
	}
	if p.selected.wsURL() != "wss://vladivostok.qms.ru:20000/" {
		t.Errorf("wsURL = %q", p.selected.wsURL())
	}
}

func TestSelectServerKeepsExplicitPort(t *testing.T) {
	p := New(Options{Server: "vladivostok.qms.ru:12345"})
	server, err := p.SelectServer(context.Background())
	if err != nil {
		t.Fatalf("SelectServer() error = %v", err)
	}
	if server.Name != "vladivostok.qms.ru:12345" {
		t.Errorf("Server.Name = %q, explicit port should not be overridden", server.Name)
	}
}

// With no host given, SelectServer auto-selects by pinging knownServers —
// which needs real network access, so it's not something to exercise in a
// unit test. What *is* hermetic: an already-canceled context makes every
// dial attempt fail immediately, so auto-select fails fast and
// deterministically, without depending on network conditions.
func TestSelectServerAutoSelectFailsFastWithCanceledContext(t *testing.T) {
	p := New(Options{Server: ""})
	ctx, cancel := context.WithCancel(context.Background())
	cancel()

	_, err := p.SelectServer(ctx)
	if err == nil {
		t.Fatal("SelectServer with no host and a canceled context = nil error, want one")
	}
	if !errors.Is(err, context.Canceled) {
		t.Errorf("error = %v, want context.Canceled", err)
	}
	var opErr *service.OpError
	if !errors.As(err, &opErr) || opErr.Code != service.CodeCanceled || opErr.Retryable {
		t.Errorf("error = %#v, want non-retryable canceled OpError", err)
	}
}

func TestPingRequiresSelectServerFirst(t *testing.T) {
	p := New(Options{Server: "example.com"})
	if _, err := p.Ping(context.Background()); err == nil {
		t.Error("Ping before SelectServer = nil error, want one")
	}
}

func TestDownloadUploadRequireSelectServer(t *testing.T) {
	p := New(Options{Server: "example.com"})
	ctx := context.Background()
	cfg := service.MeasurementConfig{Duration: 3 * time.Second}
	if _, err := p.Download(ctx, cfg, nil); err == nil {
		t.Error("Download() = nil error, want SelectServer error")
	}
	if _, err := p.Upload(ctx, cfg, nil); err == nil {
		t.Error("Upload() = nil error, want SelectServer error")
	}
}

func TestCapabilities(t *testing.T) {
	p := New(Options{Server: "example.com"})
	caps := p.Capabilities()
	for _, capability := range []service.Capability{service.CapPing, service.CapDownload, service.CapUpload} {
		if !caps.Has(capability) {
			t.Errorf("speedtest.ru service should support %v", capability)
		}
	}
}

func TestKnownServersNonEmpty(t *testing.T) {
	if len(knownServers) == 0 {
		t.Fatal("knownServers is empty — auto-select has nothing to try")
	}
	for _, s := range knownServers {
		if s.host == "" {
			t.Errorf("knownServers entry has an empty host (city %q)", s.city)
		}
	}
}

func TestQMSPingDownloadUploadProtocol(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	const token = "test-jwt-token-long-enough"
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if !websocket.IsWebSocketUpgrade(r) {
			http.NotFound(w, r)
			return
		}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for _, exchange := range []struct{ request, response string }{
			{"HI", "HELLO"}, {"GETINFO", `{"name":"qms_testing"}`}, {"AUTH", "READY_TO_TEST"}, {"PING", "PONG 123"},
		} {
			messageType, message, err := conn.ReadMessage()
			if err != nil || messageType != websocket.TextMessage || string(message) != exchange.request {
				return
			}
			_ = conn.WriteMessage(websocket.TextMessage, []byte(exchange.response))
		}
	})
	mux.HandleFunc("/download.php", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("jwt") != token || r.Header.Get("Accept-Encoding") != "identity" {
			http.Error(w, "bad auth or compression", http.StatusUnauthorized)
			return
		}
		_, _ = w.Write(make([]byte, 1_000_000))
	})
	mux.HandleFunc("/upload.php", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("jwt") != token {
			http.Error(w, "bad auth", http.StatusUnauthorized)
			return
		}
		body, _ := io.ReadAll(r.Body)
		if len(body) != uploadBlockSize {
			http.Error(w, "bad block size", http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	})
	server := httptest.NewTLSServer(mux)
	defer server.Close()

	p := New(Options{Server: ""})
	p.client = server.Client()
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	p.jwt = token
	host := strings.TrimPrefix(server.URL, "https://")
	qms := qmsServer{Host: host}
	conn, err := p.dial(context.Background(), qms.wsURL())
	if err != nil {
		t.Fatal(err)
	}
	if err := handshake(conn); err != nil {
		conn.Close()
		t.Fatalf("handshake() error = %v", err)
	}
	if _, err := pingOnce(conn); err != nil {
		conn.Close()
		t.Fatalf("pingOnce() error = %v", err)
	}
	conn.Close()

	var downloaded atomic.Int64
	ready := false
	bytesRead, err := p.downloadRequest(context.Background(), qms, 1, make([]byte, 64<<10), func() { ready = true }, func(n int64) { downloaded.Add(n) })
	if err != nil || bytesRead != 1_000_000 || downloaded.Load() != bytesRead || !ready {
		t.Fatalf("downloadRequest() bytes=%d recorded=%d ready=%t err=%v", bytesRead, downloaded.Load(), ready, err)
	}
	uploadReady := false
	if err := p.uploadRequest(context.Background(), qms, make([]byte, uploadBlockSize), func() { uploadReady = true }); err != nil || !uploadReady {
		t.Fatalf("uploadRequest() ready=%t err=%v", uploadReady, err)
	}
}

func TestPingUsesTenCachedDiscoverySamples(t *testing.T) {
	p := New(Options{Server: ""})
	samples := []float64{10, 11, 9, 10, 12, 10, 11, 9, 10, 10}
	cached := service.StatsWithMethod(samples, "median")
	cached.JitterMs = qmsJitter(samples, cached.MedianMs)
	p.selected = qmsServer{Host: "unreachable.invalid:20000", Ping: cached}
	p.servers = []qmsServer{p.selected}
	result, err := p.Ping(context.Background())
	if err != nil {
		t.Fatalf("Ping() error = %v; cached result should avoid a second WebSocket dial", err)
	}
	if result != cached || result.Samples != 10 || result.Method != "median" {
		t.Errorf("Ping() = %+v, want cached native result %+v", result, cached)
	}
}

func TestPingFailsOverToNextResponsiveServer(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		for {
			messageType, message, err := conn.ReadMessage()
			if err != nil || messageType != websocket.TextMessage {
				return
			}
			switch string(message) {
			case "HI":
				_ = conn.WriteMessage(websocket.TextMessage, []byte("HELLO"))
			case "GETINFO":
				_ = conn.WriteMessage(websocket.TextMessage, []byte("qms_testing/1.0"))
			case "AUTH":
				_ = conn.WriteMessage(websocket.TextMessage, []byte("READY_TO_TEST"))
			case "PING":
				_ = conn.WriteMessage(websocket.TextMessage, []byte("PONG 123"))
			}
		}
	}))
	defer server.Close()
	p := New(Options{Server: ""})
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	failed := qmsServer{Host: "127.0.0.1:1"}
	responsive := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
	p.selected = failed
	p.servers = []qmsServer{failed, responsive}
	result, err := p.Ping(context.Background())
	if err != nil {
		t.Fatalf("Ping() failover error = %v", err)
	}
	if result.Samples != 10 || p.selected.Host != responsive.Host {
		t.Errorf("result=%+v selected=%q, want 10 samples from %q", result, p.selected.Host, responsive.Host)
	}
}

func TestDownloadRefreshesJWTAfterUnauthorized(t *testing.T) {
	const freshToken = "fresh.payload.signature"
	var tokenCalls atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/api/server/gentoken", func(w http.ResponseWriter, _ *http.Request) {
		tokenCalls.Add(1)
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"token": freshToken})
	})
	mux.HandleFunc("/download.php", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("jwt") != freshToken {
			w.WriteHeader(http.StatusUnauthorized)
			return
		}
		_, _ = w.Write(make([]byte, 1_000_000))
	})
	server := httptest.NewTLSServer(mux)
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.apiBase = server.URL
	p.jwt = "stale-jwt-token-long-enough"
	p.browserKey = "browser-key-long-enough"
	qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
	var confirmed atomic.Int64
	bytesRead, err := p.downloadRequest(context.Background(), qms, 1, make([]byte, 64<<10), func() {}, func(n int64) { confirmed.Add(n) })
	if err != nil || bytesRead != 1_000_000 || confirmed.Load() != bytesRead {
		t.Fatalf("downloadRequest after 401 bytes=%d confirmed=%d err=%v", bytesRead, confirmed.Load(), err)
	}
	if tokenCalls.Load() != 1 || p.jwt != freshToken {
		t.Errorf("token refresh calls=%d stored token=%q", tokenCalls.Load(), p.jwt)
	}
}

func TestEnsureJWTRotatesBrowserKey(t *testing.T) {
	const newKey = "new-browser-key-123456789"
	const token = "rotated.payload.signature"
	var oldKeyCalls atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/api/server/gentoken", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("x-api-key") != newKey {
			oldKeyCalls.Add(1)
			w.WriteHeader(http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]string{"token": token})
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, `<script src="/app/page-test.js"></script>`)
	})
	mux.HandleFunc("/app/page-test.js", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, `new QMSLibrary({id:"`+newKey+`"})`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.apiBase = server.URL
	p.browserKey = "obsolete-browser-key-123"
	got, err := p.ensureJWT(context.Background(), false, "")
	if err != nil || got != token {
		t.Fatalf("ensureJWT() = (%q, %v), want rotated token", got, err)
	}
	if p.browserKey != newKey || oldKeyCalls.Load() != 1 {
		t.Errorf("browser key=%q old-key calls=%d", p.browserKey, oldKeyCalls.Load())
	}
}

func TestFetchNearestValidatesCandidates(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("x-api-key") != "key" {
			w.WriteHeader(http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.Copy(w, bytes.NewBufferString(`{"algorithm":"nearest","data":[{"id":1,"name":"moscow.speedtest.ru","city":"Москва","src":"https://moscow.qms.ru","source":"provider","port":20000},{"id":2,"name":"bad","city":"Москва","src":"http://insecure.invalid","source":"provider","port":20000}]}`))
	}))
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.apiBase = server.URL
	servers, _, err := p.fetchNearest(context.Background(), "key")
	if err != nil || len(servers) != 1 || servers[0].Host != "moscow.qms.ru:20000" {
		t.Fatalf("fetchNearest() = (%+v, %v)", servers, err)
	}
}

func TestNormalizeHost(t *testing.T) {
	tests := []struct {
		name    string
		input   string
		want    string
		wantErr bool
	}{
		{name: "hostname default port", input: "qms.example", want: "qms.example:20000"},
		{name: "hostname explicit port", input: "qms.example:443", want: "qms.example:443"},
		{name: "IPv4", input: "127.0.0.1", want: "127.0.0.1:20000"},
		{name: "IPv6", input: "::1", want: "[::1]:20000"},
		{name: "IPv6 explicit port", input: "[::1]:443", want: "[::1]:443"},
		{name: "URL is not a host", input: "https://qms.example", wantErr: true},
		{name: "path is rejected", input: "qms.example/path", wantErr: true},
		{name: "zero port", input: "qms.example:0", wantErr: true},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			got, err := normalizeHost(test.input, "20000")
			if test.wantErr {
				if err == nil {
					t.Fatalf("normalizeHost(%q) = %q, want error", test.input, got)
				}
				return
			}
			if err != nil || got != test.want {
				t.Fatalf("normalizeHost(%q) = (%q, %v), want %q", test.input, got, err, test.want)
			}
		})
	}
}

func TestFallbackServerIsPingOnly(t *testing.T) {
	p := New(Options{Server: ""})
	p.selected = qmsServer{Host: "fallback.qms.ru:20000"}
	p.servers = []qmsServer{p.selected}
	p.throughput = false
	p.discoveryErr = errors.New("discovery unavailable")

	_, err := p.Download(context.Background(), service.MeasurementConfig{Duration: 3 * time.Second}, nil)
	var opErr *service.OpError
	if !errors.As(err, &opErr) || opErr.Code != service.CodeAuth || !opErr.Retryable {
		t.Fatalf("Download() error = %#v, want retryable auth OpError", err)
	}
	if !errors.Is(err, errPingOnlyFallback) {
		t.Fatalf("Download() error = %v, want ping-only fallback cause", err)
	}
}

func TestDownloadValidatesResponseBeforeCounting(t *testing.T) {
	tests := []struct {
		name    string
		handler http.HandlerFunc
	}{
		{
			name: "wrong content length",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Length", "7")
				_, _ = io.WriteString(w, "1234567")
			},
		},
		{
			name: "compressed response",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Encoding", "gzip")
				_, _ = io.WriteString(w, "compressed")
			},
		},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			server := httptest.NewTLSServer(test.handler)
			defer server.Close()
			p := New(Options{Server: ""})
			p.client = server.Client()
			p.jwt = "cached.payload.signature"
			qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
			var recorded atomic.Int64
			ready := false
			_, err := p.downloadRequest(context.Background(), qms, 1, make([]byte, 4096), func() { ready = true }, func(n int64) { recorded.Add(n) })
			if err == nil {
				t.Fatal("downloadRequest() error = nil, want protocol validation error")
			}
			if ready || recorded.Load() != 0 {
				t.Fatalf("ready=%t recorded=%d, invalid response must not be counted", ready, recorded.Load())
			}
		})
	}
}

func TestDownloadRejectsTruncatedPayload(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Length", "1000000")
		_, _ = w.Write(make([]byte, 1024))
	}))
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.jwt = "cached.payload.signature"
	qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
	var recorded atomic.Int64
	bytesRead, err := p.downloadRequest(context.Background(), qms, 1, make([]byte, 4096), func() {}, func(n int64) { recorded.Add(n) })
	if err == nil || bytesRead != 1024 || recorded.Load() != 1024 {
		t.Fatalf("downloadRequest() = bytes %d, recorded %d, error %v; want explicit truncated-payload error", bytesRead, recorded.Load(), err)
	}
}

func TestDownloadRejectsPayloadLargerThanRequested(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.(http.Flusher).Flush()
		_, _ = w.Write(make([]byte, 1_000_001))
	}))
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.jwt = "cached.payload.signature"
	qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
	if _, err := p.downloadRequest(context.Background(), qms, 1, make([]byte, 64<<10), func() {}, func(int64) {}); err == nil {
		t.Fatal("downloadRequest() error = nil, want oversized-payload error")
	}
}

func TestThroughputAuthIsRefreshedOnlyOnce(t *testing.T) {
	const freshToken = "fresh.payload.signature"
	for _, phase := range []string{"download", "upload"} {
		t.Run(phase, func(t *testing.T) {
			var tokenCalls atomic.Int32
			var transferCalls atomic.Int32
			mux := http.NewServeMux()
			mux.HandleFunc("/api/server/gentoken", func(w http.ResponseWriter, _ *http.Request) {
				tokenCalls.Add(1)
				w.Header().Set("Content-Type", "application/json")
				_ = json.NewEncoder(w).Encode(map[string]string{"token": freshToken})
			})
			mux.HandleFunc("/download.php", func(w http.ResponseWriter, _ *http.Request) {
				transferCalls.Add(1)
				w.WriteHeader(http.StatusUnauthorized)
			})
			mux.HandleFunc("/upload.php", func(w http.ResponseWriter, r *http.Request) {
				transferCalls.Add(1)
				_, _ = io.Copy(io.Discard, r.Body)
				w.WriteHeader(http.StatusUnauthorized)
			})
			server := httptest.NewTLSServer(mux)
			defer server.Close()
			p := New(Options{Server: ""})
			p.client = server.Client()
			p.apiBase = server.URL
			p.browserKey = "browser-key-long-enough"
			p.jwt = "stale.payload.signature"
			qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
			var err error
			if phase == "download" {
				_, err = p.downloadRequest(context.Background(), qms, 1, make([]byte, 4096), func() {}, func(int64) {})
			} else {
				err = p.uploadRequest(context.Background(), qms, make([]byte, uploadBlockSize), func() {})
			}
			if err == nil {
				t.Fatal("request error = nil, want authorization failure")
			}
			if tokenCalls.Load() != 1 || transferCalls.Load() != 2 {
				t.Fatalf("gentoken calls=%d transfer calls=%d, want one refresh and two attempts", tokenCalls.Load(), transferCalls.Load())
			}
		})
	}
}

func TestUploadDoesNotConfirmFailedResponse(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.Copy(io.Discard, r.Body)
		http.Error(w, "failed", http.StatusInternalServerError)
	}))
	defer server.Close()
	p := New(Options{Server: ""})
	p.client = server.Client()
	p.jwt = "cached.payload.signature"
	qms := qmsServer{Host: strings.TrimPrefix(server.URL, "https://")}
	ready := false
	if err := p.uploadRequest(context.Background(), qms, make([]byte, uploadBlockSize), func() { ready = true }); err == nil {
		t.Fatal("uploadRequest() error = nil, want HTTP error")
	}
	if ready {
		t.Fatal("failed upload response was incorrectly confirmed")
	}
}

func TestDecodeJSONLimitedRejectsOversizedAndTrailingResponses(t *testing.T) {
	var destination map[string]any
	if err := service.DecodeJSONLimited(strings.NewReader(`{"ok":true}`+strings.Repeat(" ", 64)), 32, &destination); err == nil {
		t.Fatal("oversized JSON response was accepted")
	}
	if err := service.DecodeJSONLimited(strings.NewReader(`{"ok":true} {"second":true}`), 64, &destination); err == nil {
		t.Fatal("multiple JSON values were accepted")
	}
}

func TestExtractBrowserKeyRejectsCrossOriginAssets(t *testing.T) {
	var externalCalls atomic.Int32
	external := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		externalCalls.Add(1)
		_, _ = io.WriteString(w, `new QMSLibrary({id:"external-browser-key-123"})`)
	}))
	defer external.Close()
	page := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, `<script src="`+external.URL+`/bundle.js"></script>`)
	}))
	defer page.Close()
	p := New(Options{Server: ""})
	p.client = page.Client()
	p.apiBase = page.URL
	if _, err := p.extractBrowserKey(context.Background()); err == nil {
		t.Fatal("extractBrowserKey() error = nil, want cross-origin rejection")
	}
	if externalCalls.Load() != 0 {
		t.Fatalf("cross-origin asset fetched %d times, want 0", externalCalls.Load())
	}
}

func TestNativeCalculationsAndReconnectServerRotation(t *testing.T) {
	if got := qmsJitter([]float64{10, 40, 20, 30, 50, 60, 70, 80, 90, 100}, 45); got != 10 {
		t.Fatalf("qmsJitter() = %v, want browser-native 10", got)
	}
	chunkTests := []struct {
		bytes   int64
		elapsed time.Duration
		want    int
	}{
		{bytes: 0, elapsed: time.Second, want: 25},
		{bytes: 25_000_000, elapsed: 6 * time.Second, want: 25},
		{bytes: 100_000_000, elapsed: 6 * time.Second, want: 100},
		{bytes: 1_000_000_000, elapsed: time.Second, want: 250},
	}
	for _, test := range chunkTests {
		if got := nextDownloadChunkMB(test.bytes, test.elapsed); got != test.want {
			t.Errorf("nextDownloadChunkMB(%d, %s) = %d, want %d", test.bytes, test.elapsed, got, test.want)
		}
	}
	servers := []qmsServer{{Host: "one"}, {Host: "two"}, {Host: "three"}}
	var attempts atomic.Uint32
	if first := nextServerForWorker(servers, 1, &attempts); first.Host != "two" {
		t.Fatalf("first server = %q, want two", first.Host)
	}
	if reconnect := nextServerForWorker(servers, 1, &attempts); reconnect.Host != "three" {
		t.Fatalf("reconnect server = %q, want three", reconnect.Host)
	}
}

func TestErrorClassification(t *testing.T) {
	tests := []struct {
		err       error
		code      service.ErrorCode
		retryable bool
	}{
		{err: context.Canceled, code: service.CodeCanceled, retryable: false},
		{err: context.DeadlineExceeded, code: service.CodeTimeout, retryable: true},
		{err: service.ProtocolError(errors.New("Content-Length при скачивании равен 1, ожидалось 2")), code: service.CodeProtocol, retryable: false},
		{err: &service.HTTPStatusError{StatusCode: http.StatusForbidden, Status: "403 Forbidden", Operation: "gentoken"}, code: service.CodeAuth, retryable: true},
		{err: errors.New("connection reset"), code: service.CodeUnavailable, retryable: true},
	}
	for _, test := range tests {
		code := service.ClassifyError(test.err)
		if code != test.code || service.RetryableCode(code) != test.retryable {
			t.Errorf("error %q: code=%s retryable=%t, want %s/%t", test.err, code, service.RetryableCode(code), test.code, test.retryable)
		}
	}
}

func TestDetectConnection_ReturnsIPAndISP(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("x-api-key") != "test-browser-key" {
			http.Error(w, "missing key", http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json; charset=utf-8")
		_, _ = io.WriteString(w, `{"ip":"2001:db8::7"}`)
	})
	mux.HandleFunc("/api/asn_provider/asn", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"2001:db8::7","provider_name":"Тест Телеком"}`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	p.browserKey = "test-browser-key"
	info, err := p.DetectConnection(context.Background())
	if err != nil {
		t.Fatalf("DetectConnection() error = %v", err)
	}
	if got := info.ExternalIP.String(); got != "2001:db8::7" {
		t.Errorf("ExternalIP = %q, want 2001:db8::7", got)
	}
	if info.ISP != "Тест Телеком" || len(info.Warnings) != 0 {
		t.Errorf("ConnectionInfo = %+v, want ISP without warnings", info)
	}
}

func TestDetectConnection_KeepsIPWhenISPIsUnavailable(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.9"}`)
	})
	mux.HandleFunc("/api/asn_provider/asn", func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	info, err := p.DetectConnection(context.Background())
	if err != nil {
		t.Fatalf("DetectConnection() error = %v", err)
	}
	if got := info.ExternalIP.String(); got != "203.0.113.9" || info.ISP != "" || len(info.Warnings) != 1 {
		t.Fatalf("ConnectionInfo = %+v, want valid IP and one ISP warning", info)
	}
}

func TestDetectConnection_RejectsInvalidIPResponse(t *testing.T) {
	tests := map[string]func(http.ResponseWriter){
		"malformed JSON": func(w http.ResponseWriter) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"ip":`)
		},
		"wrong content type": func(w http.ResponseWriter) {
			w.Header().Set("Content-Type", "text/plain")
			_, _ = io.WriteString(w, `{"ip":"203.0.113.9"}`)
		},
		"invalid IP": func(w http.ResponseWriter) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{"ip":"not-an-ip"}`)
		},
		"oversized": func(w http.ResponseWriter) {
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, strings.Repeat(" ", maxConnectionBytes+1))
		},
	}
	for name, response := range tests {
		t.Run(name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				response(w)
			}))
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			p.apiBase = server.URL
			if _, err := p.DetectConnection(context.Background()); err == nil {
				t.Fatal("DetectConnection() error = nil, want protocol error")
			}
		})
	}
}

func TestDetectConnection_RotatesBrowserKeyOnce(t *testing.T) {
	const newKey = "new-connection-key-123456"
	var rejected atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("x-api-key") != newKey {
			rejected.Add(1)
			w.WriteHeader(http.StatusForbidden)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.10"}`)
	})
	mux.HandleFunc("/api/asn_provider/asn", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.10","provider_name":"ISP"}`)
	})
	mux.HandleFunc("/", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, `<script src="/bundle.js"></script>`)
	})
	mux.HandleFunc("/bundle.js", func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, `new QMSLibrary({id:"`+newKey+`"})`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	p.browserKey = "obsolete-key-123456789"
	info, err := p.DetectConnection(context.Background())
	if err != nil || !info.ExternalIP.IsValid() {
		t.Fatalf("DetectConnection() = (%+v, %v), want success", info, err)
	}
	if rejected.Load() != 1 || p.currentBrowserKey() != newKey {
		t.Fatalf("rejected=%d key=%q, want one rejection and rotated key", rejected.Load(), p.currentBrowserKey())
	}
}

func TestDetectConnection_DoesNotUseISPFromMismatchedIP(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.1"}`)
	})
	mux.HandleFunc("/api/asn_provider/asn", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.2","provider_name":"Wrong ISP"}`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	info, err := p.DetectConnection(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if info.ISP != "" || len(info.Warnings) != 1 {
		t.Fatalf("ConnectionInfo = %+v, want ignored ISP and warning", info)
	}
}

func TestDetectConnection_RejectsIPEndpoint5xx(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	_, err := p.DetectConnection(context.Background())
	var operationError *service.OpError
	if !errors.As(err, &operationError) || operationError.Code != service.CodeUnavailable || !operationError.Retryable {
		t.Fatalf("DetectConnection() error = %#v, want retryable unavailable", err)
	}
}

func TestDetectConnection_MissingISPIsWarning(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.20"}`)
	})
	mux.HandleFunc("/api/asn_provider/asn", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = io.WriteString(w, `{"ip":"203.0.113.20","provider_name":"  "}`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	p.apiBase = server.URL
	info, err := p.DetectConnection(context.Background())
	if err != nil || !info.ExternalIP.IsValid() || info.ISP != "" || len(info.Warnings) != 1 {
		t.Fatalf("DetectConnection() = (%+v, %v)", info, err)
	}
}

func TestDetectConnection_Cancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	p := New(Options{})
	_, err := p.DetectConnection(ctx)
	var operationError *service.OpError
	if !errors.Is(err, context.Canceled) || !errors.As(err, &operationError) || operationError.Code != service.CodeCanceled {
		t.Fatalf("DetectConnection() error = %#v, want canceled OpError", err)
	}
}

func TestDetectConnection_RotatesBrowserKeyOnlyOnceAfterRepeatedRejection(t *testing.T) {
	for _, status := range []int{http.StatusUnauthorized, http.StatusForbidden} {
		t.Run(strconv.Itoa(status), func(t *testing.T) {
			var apiCalls, pageCalls atomic.Int32
			mux := http.NewServeMux()
			mux.HandleFunc("/api/asn_provider/ip", func(w http.ResponseWriter, _ *http.Request) {
				apiCalls.Add(1)
				w.WriteHeader(status)
			})
			mux.HandleFunc("/", func(w http.ResponseWriter, _ *http.Request) {
				pageCalls.Add(1)
				_, _ = io.WriteString(w, `<script src="/bundle.js"></script>`)
			})
			mux.HandleFunc("/bundle.js", func(w http.ResponseWriter, _ *http.Request) {
				_, _ = io.WriteString(w, `new QMSLibrary({id:"still-rejected-key"})`)
			})
			server := httptest.NewServer(mux)
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			p.apiBase = server.URL
			if _, err := p.DetectConnection(context.Background()); err == nil {
				t.Fatal("DetectConnection() error = nil")
			}
			if apiCalls.Load() != 2 || pageCalls.Load() != 1 {
				t.Fatalf("api calls=%d page calls=%d, want 2/1", apiCalls.Load(), pageCalls.Load())
			}
		})
	}
}
