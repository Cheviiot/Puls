package yandex

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestHostOf(t *testing.T) {
	tests := map[string]string{
		"https://cdn.example.com/path?query=1": "cdn.example.com",
		"http://cdn.example.com/path":          "cdn.example.com",
		"https://cdn.example.com":              "cdn.example.com",
	}
	for in, want := range tests {
		if got := hostOf(in); got != want {
			t.Errorf("hostOf(%q) = %q, want %q", in, got, want)
		}
	}
}

func TestCacheBustAddsQueryParam(t *testing.T) {
	withoutQuery := cacheBust("https://example.com/probe")
	if !strings.Contains(withoutQuery, "?cb=") {
		t.Errorf("cacheBust(no query) = %q, want a %q param", withoutQuery, "?cb=")
	}
	withQuery := cacheBust("https://example.com/probe?lid=1")
	if !strings.Contains(withQuery, "&cb=") {
		t.Errorf("cacheBust(with query) = %q, want a %q param", withQuery, "&cb=")
	}
	// Two calls must not collide, or the CDN could serve a cached response.
	first := cacheBust("https://example.com/probe")
	second := cacheBust("https://example.com/probe")
	if first == second {
		t.Error("cacheBust should produce a different value on each call")
	}
}

func TestProbeURLValidation(t *testing.T) {
	if !validHTTPProbeURL("https://cdn.example/probe") {
		t.Error("valid HTTPS probe rejected")
	}
	if validHTTPProbeURL("http://cdn.example/probe") {
		t.Error("insecure HTTP probe accepted")
	}
	if !validWebSocketProbeURL("wss://cdn.example/upload") {
		t.Error("valid WSS probe rejected")
	}
	if validWebSocketProbeURL("ws://cdn.example/upload") {
		t.Error("insecure WS probe accepted")
	}
}

func TestMedianAbsoluteDeviationIgnoresOneQueuedResponse(t *testing.T) {
	got := service.MedianAbsoluteDeviation([]float64{10, 10.2, 80})
	if got < 0.19 || got > 0.21 {
		t.Errorf("medianAbsoluteDeviation() = %v, want about 0.2", got)
	}
}

func TestDiscoveryPingAndDownloadProtocol(t *testing.T) {
	var pingRequests atomic.Int32
	var server *httptest.Server
	mux := http.NewServeMux()
	mux.HandleFunc("/get-probes", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"mid":      "test",
			"latency":  map[string]any{"probes": []map[string]any{{"url": server.URL + "/latency"}}},
			"download": map[string]any{"probes": []map[string]any{{"url": server.URL + "/probes/50mb", "timeout": 0}}},
			"upload": map[string]any{"probes": []map[string]any{{
				"size": 30720, "url": server.URL + "/upload-probe", "postUrl": server.URL + "/upload",
				"statsUrl": server.URL + "/stats", "websocketUrl": "wss" + strings.TrimPrefix(server.URL, "https") + "/ws",
				"websocketConnectionTimeout": 2000, "timeout": 0,
			}}},
		})
	})
	mux.HandleFunc("/latency", func(w http.ResponseWriter, _ *http.Request) {
		pingRequests.Add(1)
		w.WriteHeader(http.StatusNoContent)
	})
	downloadHandler := func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Length", fmt.Sprint(32<<10))
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(make([]byte, 32<<10))
	}
	mux.HandleFunc("/probes/50mb", downloadHandler)
	mux.HandleFunc("/download", downloadHandler)
	server = httptest.NewTLSServer(mux)
	defer server.Close()

	p := New(Options{})
	p.client = server.Client()
	p.probesURL = server.URL + "/get-probes"
	selected, err := p.SelectServer(context.Background())
	if err != nil {
		t.Fatalf("SelectServer() error = %v", err)
	}
	if selected.Name == "" {
		t.Fatal("SelectServer() returned an empty endpoint")
	}
	ping, err := p.Ping(context.Background())
	if err != nil {
		t.Fatalf("Ping() error = %v", err)
	}
	if ping.Samples != 4 || ping.Method != "minimum" || pingRequests.Load() != 4 {
		t.Errorf("ping = %+v, HTTP requests = %d; want four samples and minimum method", ping, pingRequests.Load())
	}
	ready := false
	var recorded atomic.Int64
	bytes, err := p.downloadProbe(context.Background(), server.URL+"/download", make([]byte, 4096), func() { ready = true }, func(n int64) { recorded.Add(n) })
	if err != nil || bytes != 32<<10 || recorded.Load() != bytes || !ready {
		t.Errorf("downloadProbe() = (%d, %v), recorded=%d ready=%t", bytes, err, recorded.Load(), ready)
	}
}

func TestPingIsSequentialPerCDNAndParallelAcrossCDNs(t *testing.T) {
	var activeA, activeB, maxA, maxB, activeTotal, maxTotal atomic.Int32
	updateMax := func(maximum *atomic.Int32, value int32) {
		for {
			previous := maximum.Load()
			if value <= previous || maximum.CompareAndSwap(previous, value) {
				return
			}
		}
	}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var active, maximum *atomic.Int32
		switch r.URL.Path {
		case "/a":
			active, maximum = &activeA, &maxA
		case "/b":
			active, maximum = &activeB, &maxB
		default:
			http.NotFound(w, r)
			return
		}
		current := active.Add(1)
		updateMax(maximum, current)
		total := activeTotal.Add(1)
		updateMax(&maxTotal, total)
		time.Sleep(15 * time.Millisecond)
		active.Add(-1)
		activeTotal.Add(-1)
		w.WriteHeader(http.StatusNoContent)
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	p.latencyURLs = []string{server.URL + "/a", server.URL + "/b"}
	result, err := p.Ping(context.Background())
	if err != nil {
		t.Fatal(err)
	}
	if result.Samples != 8 {
		t.Errorf("samples = %d, want 8", result.Samples)
	}
	if maxA.Load() != 1 || maxB.Load() != 1 {
		t.Errorf("per-CDN concurrency a=%d b=%d, want 1", maxA.Load(), maxB.Load())
	}
	if maxTotal.Load() < 2 {
		t.Errorf("cross-CDN concurrency = %d, want at least 2", maxTotal.Load())
	}
}

func TestPingKeepsValidSamplesAfterOneRequestFails(t *testing.T) {
	var requests atomic.Int32
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		if requests.Add(1) == 1 {
			http.Error(w, "temporary", http.StatusServiceUnavailable)
			return
		}
		w.WriteHeader(http.StatusNoContent)
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	p.latencyURLs = []string{server.URL}

	result, err := p.Ping(context.Background())
	if err != nil {
		t.Fatalf("Ping() error = %v", err)
	}
	if requests.Load() != 4 || result.Samples != 3 {
		t.Fatalf("requests=%d samples=%d, want all four attempts and three valid samples", requests.Load(), result.Samples)
	}
}

func TestDiscoveryRejectsMalformedProbeSchemaWithoutReplacingState(t *testing.T) {
	var malformed atomic.Bool
	var server *httptest.Server
	server = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		document := discoveryDocument(server.URL)
		if malformed.Load() {
			upload := document["upload"].(map[string]any)["probes"].([]map[string]any)
			upload[0]["postUrl"] = "https://different-cdn.example/upload"
		}
		_ = json.NewEncoder(w).Encode(document)
	}))
	defer server.Close()

	p := New(Options{})
	p.client = server.Client()
	p.probesURL = server.URL
	if _, err := p.SelectServer(context.Background()); err != nil {
		t.Fatalf("initial SelectServer() error = %v", err)
	}
	p.mu.RLock()
	oldLatency := append([]string(nil), p.latencyURLs...)
	oldDownload := append([]string(nil), p.downloadURLs...)
	p.mu.RUnlock()

	malformed.Store(true)
	if _, err := p.SelectServer(context.Background()); err == nil {
		t.Fatal("SelectServer() accepted upload endpoints from different CDN hosts")
	}
	p.mu.RLock()
	defer p.mu.RUnlock()
	if strings.Join(p.latencyURLs, "|") != strings.Join(oldLatency, "|") || strings.Join(p.downloadURLs, "|") != strings.Join(oldDownload, "|") {
		t.Fatal("failed discovery replaced the last known-good endpoint set")
	}
}

func TestDiscoveryValidatesEveryProbe(t *testing.T) {
	tests := []struct {
		name   string
		mutate func(map[string]any)
	}{
		{
			name: "latency URL",
			mutate: func(document map[string]any) {
				document["latency"].(map[string]any)["probes"].([]map[string]any)[0]["url"] = "http://insecure.example/ping"
			},
		},
		{
			name: "optional download URL",
			mutate: func(document map[string]any) {
				download := document["download"].(map[string]any)
				download["probes"] = append(
					download["probes"].([]map[string]any),
					map[string]any{"url": "http://insecure.example/probes/100kb", "timeout": 100},
				)
			},
		},
		{
			name: "main download path",
			mutate: func(document map[string]any) {
				document["download"].(map[string]any)["probes"].([]map[string]any)[0]["url"] = "https://cdn.example/probes/50mb-copy"
			},
		},
		{
			name: "websocket timeout",
			mutate: func(document map[string]any) {
				document["upload"].(map[string]any)["probes"].([]map[string]any)[0]["websocketConnectionTimeout"] = 0
			},
		},
		{
			name: "optional upload URL",
			mutate: func(document map[string]any) {
				probes := document["upload"].(map[string]any)["probes"].([]map[string]any)
				invalid := cloneMap(probes[0])
				invalid["url"] = "http://insecure.example/upload"
				invalid["timeout"] = 100
				document["upload"].(map[string]any)["probes"] = append(probes, invalid)
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			var server *httptest.Server
			server = httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				document := discoveryDocument(server.URL)
				tt.mutate(document)
				_ = json.NewEncoder(w).Encode(document)
			}))
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			p.probesURL = server.URL
			if _, err := p.SelectServer(context.Background()); err == nil {
				t.Fatal("SelectServer() accepted malformed discovery schema")
			}
		})
	}
}

func TestDiscoveryRejectsUnexpectedContentTypeAndOversizedBody(t *testing.T) {
	tests := []struct {
		name    string
		handler http.Handler
	}{
		{
			name: "content type",
			handler: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "text/html")
				_, _ = io.WriteString(w, `{}`)
			}),
		},
		{
			name: "body limit",
			handler: http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/json")
				_, _ = fmt.Fprintf(w, `{"mid":"test","padding":"%s"}`, strings.Repeat("x", maxDiscoveryBodySize))
			}),
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := httptest.NewTLSServer(tt.handler)
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			p.probesURL = server.URL
			if _, err := p.SelectServer(context.Background()); err == nil {
				t.Fatal("SelectServer() accepted invalid discovery response")
			}
		})
	}
}

func TestDiscoveryClassifiesHTTPStatus(t *testing.T) {
	tests := []struct {
		status    int
		code      service.ErrorCode
		retryable bool
	}{
		{status: http.StatusForbidden, code: service.CodeProtocol, retryable: false},
		{status: http.StatusTooManyRequests, code: service.CodeProtocol, retryable: true},
		{status: http.StatusServiceUnavailable, code: service.CodeUnavailable, retryable: true},
	}
	for _, tt := range tests {
		t.Run(http.StatusText(tt.status), func(t *testing.T) {
			server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(tt.status)
			}))
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			p.probesURL = server.URL
			_, err := p.SelectServer(context.Background())
			var opErr *service.OpError
			if !errors.As(err, &opErr) {
				t.Fatalf("SelectServer() error = %v, want *service.OpError", err)
			}
			if opErr.Code != tt.code || opErr.Retryable != tt.retryable {
				t.Fatalf("SelectServer() error = %+v, want code=%s retryable=%t", opErr, tt.code, tt.retryable)
			}
		})
	}
}

func TestDiscoveryDoesNotFollowRedirects(t *testing.T) {
	var redirected atomic.Bool
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/target" {
			redirected.Store(true)
			w.Header().Set("Content-Type", "application/json")
			_, _ = io.WriteString(w, `{}`)
			return
		}
		http.Redirect(w, r, "/target", http.StatusFound)
	}))
	defer server.Close()
	p := New(Options{})
	p.client.Transport = server.Client().Transport
	p.probesURL = server.URL
	if _, err := p.SelectServer(context.Background()); err == nil {
		t.Fatal("SelectServer() accepted an HTTP redirect")
	}
	if redirected.Load() {
		t.Fatal("SelectServer() followed a discovery redirect")
	}
}

func TestDownloadRejectsTruncatedBody(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Length", "100")
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(make([]byte, 10))
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	var recorded atomic.Int64
	if bytes, err := p.downloadProbe(context.Background(), server.URL, make([]byte, 32), func() {}, func(n int64) { recorded.Add(n) }); err == nil || bytes == 0 {
		t.Fatalf("truncated response = (%d, %v), want partial byte count and error", bytes, err)
	}
	if recorded.Load() != 10 {
		t.Errorf("truncated response recorded %d bytes, want the 10 bytes received after valid HTTP headers", recorded.Load())
	}
}

func TestDownloadRejectsHTTP5xxWithoutBytes(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "failed", http.StatusServiceUnavailable)
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	var recorded atomic.Int64
	if bytes, err := p.downloadProbe(context.Background(), server.URL, make([]byte, 32), func() {}, func(n int64) { recorded.Add(n) }); err == nil || bytes != 0 || recorded.Load() != 0 {
		t.Fatalf("HTTP 503 response bytes=%d recorded=%d err=%v", bytes, recorded.Load(), err)
	}
}

func TestDownloadValidatesHeadersBeforeBecomingReady(t *testing.T) {
	tests := []struct {
		name    string
		handler http.HandlerFunc
	}{
		{
			name: "unknown length",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/octet-stream")
				w.WriteHeader(http.StatusOK)
				w.(http.Flusher).Flush()
				_, _ = w.Write([]byte("data"))
			},
		},
		{
			name: "content encoding",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/octet-stream")
				w.Header().Set("Content-Encoding", "gzip")
				w.Header().Set("Content-Length", "4")
				_, _ = w.Write([]byte("data"))
			},
		},
		{
			name: "content type",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "text/plain")
				w.Header().Set("Content-Length", "4")
				_, _ = w.Write([]byte("data"))
			},
		},
		{
			name: "unsafe size",
			handler: func(w http.ResponseWriter, _ *http.Request) {
				w.Header().Set("Content-Type", "application/octet-stream")
				w.Header().Set("Content-Length", fmt.Sprint(maxDownloadBodySize+1))
				w.WriteHeader(http.StatusOK)
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			server := httptest.NewTLSServer(tt.handler)
			defer server.Close()
			p := New(Options{})
			p.client = server.Client()
			var ready atomic.Bool
			var recorded atomic.Int64
			if _, err := p.downloadProbe(context.Background(), server.URL, make([]byte, 32), func() { ready.Store(true) }, func(n int64) { recorded.Add(n) }); err == nil {
				t.Fatal("downloadProbe() accepted invalid response headers")
			}
			if ready.Load() || recorded.Load() != 0 {
				t.Fatalf("invalid response became ready=%t and recorded=%d bytes", ready.Load(), recorded.Load())
			}
		})
	}
}

func TestDownloadValidatesNative50MiBProbeSize(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		w.Header().Set("Content-Length", "1024")
		_, _ = w.Write(make([]byte, 1024))
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	var ready atomic.Bool
	var recorded atomic.Int64
	_, err := p.downloadProbe(context.Background(), server.URL+"/probes/50mb", make([]byte, 1024), func() { ready.Store(true) }, func(n int64) { recorded.Add(n) })
	if err == nil {
		t.Fatal("downloadProbe() accepted a /probes/50mb response with the wrong size")
	}
	if ready.Load() || recorded.Load() != 0 {
		t.Fatalf("wrong-sized native probe became ready=%t and recorded=%d", ready.Load(), recorded.Load())
	}
}

func TestUploadWebSocketFallsBackToConfirmedHTTP(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	var httpRequests atomic.Int32
	var server *httptest.Server
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		_, _, _ = conn.ReadMessage()
		_ = conn.WriteMessage(websocket.TextMessage, []byte(`{"k":"u","b":1000000000000}`))
	})
	mux.HandleFunc("/upload", func(w http.ResponseWriter, r *http.Request) {
		defer r.Body.Close()
		if _, err := io.Copy(io.Discard, r.Body); err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		httpRequests.Add(1)
		w.WriteHeader(http.StatusNoContent)
	})
	server = httptest.NewTLSServer(mux)
	defer server.Close()

	var fallbackLogged atomic.Bool
	p := New(Options{Log: func(format string, _ ...any) {
		if strings.Contains(format, "резервный") {
			fallbackLogged.Store(true)
		}
	}})
	p.client = server.Client()
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	ctx, cancel := context.WithTimeout(context.Background(), 80*time.Millisecond)
	defer cancel()
	var confirmed atomic.Int64
	err := p.uploadWorker(ctx, uploadProbe{
		postURL: server.URL + "/upload", websocketURL: "wss" + strings.TrimPrefix(server.URL, "https") + "/ws",
	}, make([]byte, 1024), service.MeasurementConfig{Duration: 3 * time.Second}, func() {}, func(n int64) { confirmed.Add(n) })
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("uploadWorker() error = %v, want deadline after fallback loop", err)
	}
	if confirmed.Load() == 0 || httpRequests.Load() == 0 {
		t.Fatalf("HTTP fallback confirmed bytes=%d requests=%d", confirmed.Load(), httpRequests.Load())
	}
	if !fallbackLogged.Load() {
		t.Error("verbose fallback diagnostic was not emitted")
	}
}

func TestWebSocketUploadAcceptsZeroControlAck(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		first := true
		for {
			messageType, message, err := conn.ReadMessage()
			if err != nil {
				return
			}
			if messageType != websocket.BinaryMessage {
				return
			}
			if first {
				_ = conn.WriteMessage(websocket.TextMessage, []byte(`{"k":"u","b":0,"i":0}`))
				first = false
			}
			ack, _ := json.Marshal(map[string]any{"k": "u", "b": len(message)})
			_ = conn.WriteMessage(websocket.TextMessage, ack)
		}
	}))
	defer server.Close()
	p := New(Options{})
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()
	var confirmed atomic.Int64
	err := p.websocketUpload(ctx, "wss"+strings.TrimPrefix(server.URL, "https"), 3*time.Second, func() {}, func(n int64) { confirmed.Add(n) })
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("websocketUpload() error = %v, want deadline", err)
	}
	if confirmed.Load() == 0 {
		t.Error("positive acknowledgement after b=0 control frame was not counted")
	}
}

func TestWebSocketUploadValidatesAcknowledgementSchema(t *testing.T) {
	tests := []struct {
		name        string
		messageType int
		message     []byte
	}{
		{name: "missing bytes", messageType: websocket.TextMessage, message: []byte(`{"k":"u"}`)},
		{name: "wrong command", messageType: websocket.TextMessage, message: []byte(`{"k":"download","b":1}`)},
		{name: "negative bytes", messageType: websocket.TextMessage, message: []byte(`{"k":"u","b":-1}`)},
		{name: "wrong bytes type", messageType: websocket.TextMessage, message: []byte(`{"k":"u","b":"1"}`)},
		{name: "multiple JSON values", messageType: websocket.TextMessage, message: []byte(`{"k":"u","b":1} {}`)},
		{name: "binary acknowledgement", messageType: websocket.BinaryMessage, message: []byte(`{"k":"u","b":1}`)},
		{name: "oversized acknowledgement", messageType: websocket.TextMessage, message: []byte(`{"k":"u","b":1,"padding":"` + strings.Repeat("x", maxWebsocketAckSize) + `"}`)},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := websocketUploadWithReply(t, tt.messageType, tt.message)
			if err == nil {
				t.Fatal("websocketUpload() accepted malformed acknowledgement")
			}
			if strings.Contains(err.Error(), "padding") {
				t.Fatal("protocol error included untrusted acknowledgement payload")
			}
		})
	}
}

func TestWebSocketUploadCountsOnlyAcknowledgedBinaryFrame(t *testing.T) {
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	var queryOK atomic.Bool
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Query().Get("type") == "upload" && r.URL.Query().Get("duration") == "5" {
			queryOK.Store(true)
		}
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		messageType, message, err := conn.ReadMessage()
		if err != nil || messageType != websocket.BinaryMessage {
			return
		}
		ack, _ := json.Marshal(map[string]any{"k": "u", "b": len(message), "i": 1})
		_ = conn.WriteMessage(websocket.TextMessage, ack)
		_ = conn.WriteControl(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""), time.Now().Add(time.Second))
	}))
	defer server.Close()

	p := New(Options{})
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	var ready atomic.Bool
	var confirmed atomic.Int64
	err := p.websocketUpload(context.Background(), "wss"+strings.TrimPrefix(server.URL, "https"), 3*time.Second, func() { ready.Store(true) }, func(n int64) { confirmed.Add(n) })
	if err == nil {
		t.Fatal("websocketUpload() unexpectedly remained successful after server closed before the phase deadline")
	}
	if !queryOK.Load() {
		t.Fatal("websocketUpload() did not send native type/duration query parameters")
	}
	if !ready.Load() || confirmed.Load() != websocketChunkSize {
		t.Fatalf("ready=%t confirmed=%d, want exactly one acknowledged %d-byte frame", ready.Load(), confirmed.Load(), websocketChunkSize)
	}
}

func TestWebSocketUploadHonorsDiscoveryConnectionTimeout(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(http.ResponseWriter, *http.Request) {
		time.Sleep(300 * time.Millisecond)
	}))
	defer server.Close()
	p := New(Options{})
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	start := time.Now()
	err := p.websocketUploadWithTimeout(ctx, "wss"+strings.TrimPrefix(server.URL, "https"), 30*time.Millisecond, 3*time.Second, func() {}, func(int64) {})
	if err == nil {
		t.Fatal("websocketUploadWithTimeout() accepted a stalled handshake")
	}
	if elapsed := time.Since(start); elapsed > 200*time.Millisecond {
		t.Fatalf("stalled handshake stopped after %s, want discovery timeout near 30ms", elapsed)
	}
}

func TestHTTPUploadRejectsOversizedResponseWithoutConfirmation(t *testing.T) {
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = io.Copy(io.Discard, r.Body)
		_ = r.Body.Close()
		w.Header().Set("Content-Length", fmt.Sprint(maxUploadResponse+1))
		w.WriteHeader(http.StatusOK)
	}))
	defer server.Close()
	p := New(Options{})
	p.client = server.Client()
	var ready atomic.Bool
	var confirmed atomic.Int64
	err := p.httpUpload(context.Background(), server.URL, make([]byte, 1024), func() { ready.Store(true) }, func(n int64) { confirmed.Add(n) })
	if err == nil {
		t.Fatal("httpUpload() accepted an oversized response")
	}
	if ready.Load() || confirmed.Load() != 0 {
		t.Fatalf("oversized response became ready=%t and confirmed=%d", ready.Load(), confirmed.Load())
	}
}

func TestErrorCodeClassifiesWebSocketSchemaViolation(t *testing.T) {
	err := fmt.Errorf("upload stream: %w", errInvalidWebsocketAcknowledgement)
	if got := service.ClassifyError(err); got != service.CodeProtocol {
		t.Fatalf("service.ClassifyError(%v) = %s, want %s", err, got, service.CodeProtocol)
	}
}

func websocketUploadWithReply(t *testing.T, messageType int, message []byte) error {
	t.Helper()
	upgrader := websocket.Upgrader{CheckOrigin: func(*http.Request) bool { return true }}
	server := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		defer conn.Close()
		if _, _, err := conn.ReadMessage(); err != nil {
			return
		}
		_ = conn.WriteMessage(messageType, message)
	}))
	defer server.Close()
	p := New(Options{})
	p.dialer.TLSClientConfig = server.Client().Transport.(*http.Transport).TLSClientConfig
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	return p.websocketUpload(ctx, "wss"+strings.TrimPrefix(server.URL, "https"), 3*time.Second, func() {}, func(int64) {})
}

func discoveryDocument(baseURL string) map[string]any {
	websocketURL := "wss" + strings.TrimPrefix(baseURL, "https") + "/upload-ws"
	return map[string]any{
		"mid": "test",
		"latency": map[string]any{"probes": []map[string]any{
			{"url": baseURL + "/ping"},
		}},
		"download": map[string]any{"probes": []map[string]any{
			{"url": baseURL + "/probes/50mb", "timeout": 0},
			{"url": baseURL + "/probes/100kb", "timeout": 100},
		}},
		"upload": map[string]any{"probes": []map[string]any{
			{
				"size": 30_720, "url": baseURL + "/upload", "postUrl": baseURL + "/upload-http",
				"statsUrl": baseURL + "/upload-stats", "websocketUrl": websocketURL,
				"websocketConnectionTimeout": 2000, "timeout": 0,
			},
		}},
	}
}

func cloneMap(source map[string]any) map[string]any {
	result := make(map[string]any, len(source))
	for key, value := range source {
		result[key] = value
	}
	return result
}

func internetPageHTML(body string) string {
	return "<html><head></head><body><script>" + body + "</script></body></html>"
}

func TestDetectConnection_ParsesIPv4(t *testing.T) {
	html := internetPageHTML(`Client.default({"blackbox":{"isValid":false},"ip":{"v4":"203.0.113.7","v6":null},"other":{"nested":{"a":1}}})`)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, html)
	}))
	defer server.Close()

	p := New(Options{})
	p.internetPageURL = server.URL
	info, err := p.DetectConnection(context.Background())
	if err != nil {
		t.Fatalf("DetectConnection() error = %v", err)
	}
	if got := info.ExternalIP.String(); got != "203.0.113.7" {
		t.Fatalf("ExternalIP = %q, want %q", got, "203.0.113.7")
	}
}

func TestDetectConnection_FallsBackToIPv6(t *testing.T) {
	html := internetPageHTML(`Client.default({"ip":{"v4":"","v6":"2001:db8::1"}})`)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, html)
	}))
	defer server.Close()

	p := New(Options{})
	p.internetPageURL = server.URL
	info, err := p.DetectConnection(context.Background())
	if err != nil {
		t.Fatalf("DetectConnection() error = %v", err)
	}
	if got := info.ExternalIP.String(); got != "2001:db8::1" {
		t.Fatalf("ExternalIP = %q, want %q", got, "2001:db8::1")
	}
}

func TestDetectConnection_RejectsInvalidResponses(t *testing.T) {
	tests := map[string]struct {
		status int
		body   string
	}{
		"no marker":        {http.StatusOK, internetPageHTML(`SomethingElse({"ip":{"v4":"203.0.113.7"}})`)},
		"malformed json":   {http.StatusOK, internetPageHTML(`Client.default({"ip":{"v4":"203.0.113.7"`)},
		"missing ip":       {http.StatusOK, internetPageHTML(`Client.default({"blackbox":{"isValid":false}})`)},
		"invalid ip value": {http.StatusOK, internetPageHTML(`Client.default({"ip":{"v4":"not-an-ip","v6":""}})`)},
		"server error":     {http.StatusInternalServerError, internetPageHTML(`Client.default({"ip":{"v4":"203.0.113.7"}})`)},
	}
	for name, tt := range tests {
		t.Run(name, func(t *testing.T) {
			server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				w.WriteHeader(tt.status)
				fmt.Fprint(w, tt.body)
			}))
			defer server.Close()

			p := New(Options{})
			p.internetPageURL = server.URL
			if _, err := p.DetectConnection(context.Background()); err == nil {
				t.Fatal("DetectConnection() error = nil, want error")
			}
		})
	}
}

func TestDetectConnection_RejectsOversizedPage(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		_, _ = w.Write(make([]byte, maxInternetPageSize+1))
	}))
	defer server.Close()

	p := New(Options{})
	p.internetPageURL = server.URL
	if _, err := p.DetectConnection(context.Background()); err == nil {
		t.Fatal("DetectConnection() error = nil, want error for oversized page")
	}
}

func TestExtractBalancedJSONObjectHandlesEscapedQuotesAndNesting(t *testing.T) {
	body := []byte(`prefix Client.default({"a":"va\"lue","b":{"c":1}}) suffix`)
	object, err := extractBalancedJSONObject(body, clientStateMarker)
	if err != nil {
		t.Fatalf("extractBalancedJSONObject() error = %v", err)
	}
	want := `{"a":"va\"lue","b":{"c":1}}`
	if string(object) != want {
		t.Fatalf("extractBalancedJSONObject() = %q, want %q", object, want)
	}
}
