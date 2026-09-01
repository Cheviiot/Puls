// Package yandex implements the public protocol used by
// Яндекс.Интернетометр (https://yandex.ru/internet/).
package yandex

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"mime"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/measure"
	"github.com/Cheviiot/Puls/internal/provider"
)

const (
	defaultProbesURL     = "https://yandex.ru/internet/api/v0/get-probes?flag_ws-conn-timeout=2000"
	userAgent            = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)"
	uploadChunkSize      = 2 * 1024 * 1024
	websocketChunkSize   = 64 << 10
	expectedDownloadSize = 50 << 20
	maxDiscoveryBodySize = 2 << 20
	maxPingBodySize      = 64 << 10
	maxDownloadBodySize  = 1 << 30
	maxUploadResponse    = 1 << 20
	maxWebsocketAckSize  = 4 << 10
)

var websocketUploadChunk = make([]byte, websocketChunkSize)

var errInvalidWebsocketAcknowledgement = errors.New("неверное подтверждение WebSocket")

type probesResponse struct {
	MID     string `json:"mid"`
	Latency struct {
		Probes []struct {
			URL string `json:"url"`
		} `json:"probes"`
	} `json:"latency"`
	Download struct {
		Probes []struct {
			URL        string `json:"url"`
			Timeout    int    `json:"timeout,omitempty"`
			StartDelay int    `json:"startDelay,omitempty"`
		} `json:"probes"`
	} `json:"download"`
	Upload struct {
		Probes []struct {
			Size                       int    `json:"size"`
			URL                        string `json:"url"`
			PostURL                    string `json:"postUrl"`
			StatsURL                   string `json:"statsUrl"`
			WebsocketURL               string `json:"websocketUrl"`
			WebsocketConnectionTimeout int    `json:"websocketConnectionTimeout"`
			Timeout                    int    `json:"timeout,omitempty"`
		} `json:"probes"`
	} `json:"upload"`
}

type uploadProbe struct {
	postURL                    string
	websocketURL               string
	websocketConnectionTimeout time.Duration
}

// Provider implements provider.Provider for Яндекс.Интернетометр.
type Provider struct {
	client    *http.Client
	dialer    *websocket.Dialer
	probesURL string

	mu           sync.RWMutex
	latencyURLs  []string
	downloadURLs []string
	uploadProbes []uploadProbe
}

// New creates a Yandex Internetometer provider.
func New() *Provider {
	transport := &http.Transport{
		TLSClientConfig:       &tls.Config{MinVersion: tls.VersionTLS12},
		MaxIdleConns:          32,
		MaxIdleConnsPerHost:   16,
		MaxConnsPerHost:       16,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: time.Second,
		DisableCompression:    true,
		ForceAttemptHTTP2:     true,
		ResponseHeaderTimeout: 10 * time.Second,
	}
	return &Provider{
		client: &http.Client{
			Transport: transport,
			CheckRedirect: func(*http.Request, []*http.Request) error {
				return http.ErrUseLastResponse
			},
		},
		dialer: &websocket.Dialer{
			HandshakeTimeout:  10 * time.Second,
			TLSClientConfig:   &tls.Config{MinVersion: tls.VersionTLS12},
			Proxy:             http.ProxyFromEnvironment,
			EnableCompression: false,
		},
		probesURL: defaultProbesURL,
	}
}

func (p *Provider) Name() string { return "yandex" }

func (p *Provider) Capabilities() provider.Capability {
	return provider.CapPing | provider.CapDownload | provider.CapUpload
}

func (p *Provider) SelectServer(ctx context.Context) (provider.Server, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, cacheBust(p.probesURL), nil)
	if err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeInternal, false, err)
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")

	resp, err := p.client.Do(req)
	if err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeUnavailable, true, fmt.Errorf("запрос get-probes: %w", err))
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		code := provider.CodeProtocol
		retryable := resp.StatusCode == http.StatusRequestTimeout || resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode >= 500
		if resp.StatusCode >= 500 {
			code = provider.CodeUnavailable
		}
		return provider.Server{}, provider.NewError(p.Name(), "select", code, retryable, fmt.Errorf("get-probes вернул состояние %s", resp.Status))
	}
	contentType, _, contentTypeErr := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if contentTypeErr != nil || contentType != "application/json" {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true,
			fmt.Errorf("get-probes вернул неожиданный Content-Type %q", resp.Header.Get("Content-Type")))
	}

	var pr probesResponse
	limited := &io.LimitedReader{R: resp.Body, N: maxDiscoveryBodySize + 1}
	decoder := json.NewDecoder(limited)
	if err := decoder.Decode(&pr); err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true, fmt.Errorf("разбор ответа get-probes: %w", err))
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true, fmt.Errorf("разбор ответа get-probes: %w", err))
	}
	if limited.N <= 0 {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true, errors.New("ответ get-probes превышает безопасный предел"))
	}
	if strings.TrimSpace(pr.MID) == "" {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true, errors.New("get-probes вернул пустой идентификатор mid"))
	}

	latencyURLs := make([]string, 0, len(pr.Latency.Probes))
	latencyHosts := make(map[string]struct{}, len(pr.Latency.Probes))
	for index, probe := range pr.Latency.Probes {
		if !validHTTPProbeURL(probe.URL) {
			return provider.Server{}, p.discoverySchemaError("неверный URL latency-пробы %d", index+1)
		}
		host := endpointHost(probe.URL)
		if _, exists := latencyHosts[host]; exists {
			return provider.Server{}, p.discoverySchemaError("повторяющийся CDN latency-пробы %d", index+1)
		}
		latencyHosts[host] = struct{}{}
		latencyURLs = append(latencyURLs, probe.URL)
	}
	downloadURLs := make([]string, 0, len(pr.Download.Probes))
	downloadHosts := make(map[string]struct{}, len(pr.Download.Probes))
	for index, probe := range pr.Download.Probes {
		if !validHTTPProbeURL(probe.URL) {
			return provider.Server{}, p.discoverySchemaError("неверный URL download-пробы %d", index+1)
		}
		if probe.Timeout != 0 {
			continue
		}
		if !isLargeDownloadProbe(probe.URL) {
			return provider.Server{}, p.discoverySchemaError("неожиданный URL основной download-пробы %d", index+1)
		}
		host := endpointHost(probe.URL)
		if _, exists := downloadHosts[host]; exists {
			return provider.Server{}, p.discoverySchemaError("повторяющийся CDN download-пробы %d", index+1)
		}
		downloadHosts[host] = struct{}{}
		downloadURLs = append(downloadURLs, probe.URL)
	}
	uploadProbes := make([]uploadProbe, 0, len(pr.Upload.Probes))
	uploadHosts := make(map[string]struct{}, len(pr.Upload.Probes))
	for index, probe := range pr.Upload.Probes {
		if probe.Size <= 0 || !validHTTPProbeURL(probe.URL) || !validHTTPProbeURL(probe.PostURL) || !validHTTPProbeURL(probe.StatsURL) {
			return provider.Server{}, p.discoverySchemaError("неверная upload-проба %d", index+1)
		}
		probeHost := endpointHost(probe.URL)
		if endpointHost(probe.PostURL) != probeHost || endpointHost(probe.StatsURL) != probeHost {
			return provider.Server{}, p.discoverySchemaError("endpoint upload-пробы %d относятся к разным CDN", index+1)
		}
		if probe.WebsocketURL != "" {
			if !validWebSocketProbeURL(probe.WebsocketURL) || endpointHost(probe.WebsocketURL) != probeHost {
				return provider.Server{}, p.discoverySchemaError("неверный WebSocket URL upload-пробы %d", index+1)
			}
			if probe.WebsocketConnectionTimeout <= 0 || probe.WebsocketConnectionTimeout > 30_000 {
				return provider.Server{}, p.discoverySchemaError("неверный WebSocket timeout upload-пробы %d", index+1)
			}
		}
		if probe.Timeout != 0 {
			continue
		}
		if _, exists := uploadHosts[probeHost]; exists {
			return provider.Server{}, p.discoverySchemaError("повторяющийся CDN upload-пробы %d", index+1)
		}
		uploadHosts[probeHost] = struct{}{}
		uploadProbes = append(uploadProbes, uploadProbe{
			postURL:                    probe.PostURL,
			websocketURL:               probe.WebsocketURL,
			websocketConnectionTimeout: time.Duration(probe.WebsocketConnectionTimeout) * time.Millisecond,
		})
	}
	if len(latencyURLs) == 0 || len(downloadURLs) == 0 || len(uploadProbes) == 0 {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true,
			fmt.Errorf("неполный набор проб (задержка=%d, скачивание=%d, отдача=%d)", len(latencyURLs), len(downloadURLs), len(uploadProbes)))
	}
	if err := validateCDNSets(latencyHosts, downloadHosts, uploadHosts); err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeProtocol, true, err)
	}

	p.mu.Lock()
	p.latencyURLs = latencyURLs
	p.downloadURLs = downloadURLs
	p.uploadProbes = uploadProbes
	p.mu.Unlock()

	return provider.Server{Name: hostOf(downloadURLs[0])}, nil
}

func (p *Provider) discoverySchemaError(format string, args ...any) error {
	return provider.NewError(p.Name(), "select", provider.CodeProtocol, true, fmt.Errorf(format, args...))
}

func validateCDNSets(latency, download, upload map[string]struct{}) error {
	for host := range latency {
		if _, ok := download[host]; !ok {
			return fmt.Errorf("CDN %s не содержит основной download-пробы", host)
		}
		if _, ok := upload[host]; !ok {
			return fmt.Errorf("CDN %s не содержит основной upload-пробы", host)
		}
	}
	if len(download) != len(latency) || len(upload) != len(latency) {
		return errors.New("наборы latency, download и upload относятся к разным CDN")
	}
	return nil
}

func ensureJSONEOF(decoder *json.Decoder) error {
	var trailing any
	if err := decoder.Decode(&trailing); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("ответ содержит несколько значений JSON")
		}
		return err
	}
	return nil
}

func validHTTPProbeURL(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && u.Scheme == "https" && u.Hostname() != "" && u.User == nil && u.Fragment == "" && u.Opaque == ""
}

func validWebSocketProbeURL(raw string) bool {
	u, err := url.Parse(raw)
	return err == nil && u.Scheme == "wss" && u.Hostname() != "" && u.User == nil && u.Fragment == "" && u.Opaque == ""
}

func endpointHost(rawURL string) string {
	u, err := url.Parse(rawURL)
	if err != nil {
		return ""
	}
	return strings.ToLower(u.Host)
}

func isLargeDownloadProbe(rawURL string) bool {
	u, err := url.Parse(rawURL)
	return err == nil && strings.HasSuffix(strings.TrimRight(u.Path, "/"), "/probes/50mb")
}

func hostOf(rawURL string) string {
	u, err := url.Parse(rawURL)
	if err != nil {
		return ""
	}
	return u.Host
}

func (p *Provider) Ping(ctx context.Context) (provider.PingResult, error) {
	p.mu.RLock()
	urls := append([]string(nil), p.latencyURLs...)
	p.mu.RUnlock()
	if len(urls) == 0 {
		return provider.PingResult{}, provider.NewError(p.Name(), "ping", provider.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}

	const samplesPerURL = 4
	type latencyResult struct {
		index   int
		samples []float64
	}
	results := make(chan latencyResult, len(urls))
	for index, probeURL := range urls {
		// The first-party client probes CDN groups concurrently, but performs
		// four requests to each individual CDN sequentially. This is important:
		// requests 2–4 reuse the HTTP/2/TLS connection and represent network RTT
		// instead of connection setup and CDN queueing time.
		go func(index int, probeURL string) {
			values := make([]float64, 0, samplesPerURL)
			for range samplesPerURL {
				probeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
				duration, err := p.pingOnce(probeCtx, probeURL)
				cancel()
				if err != nil {
					// The browser client discards the whole CDN group when any
					// of its four samples exceeds the five-second timeout.
					values = nil
					break
				}
				values = append(values, duration.Seconds()*1000)
			}
			results <- latencyResult{index: index, samples: values}
		}(index, probeURL)
	}
	ordered := make([][]float64, len(urls))
	for range urls {
		select {
		case result := <-results:
			ordered[result.index] = result.samples
		case <-ctx.Done():
			return provider.PingResult{}, provider.NewError(p.Name(), "ping", provider.CodeCanceled, false, ctx.Err())
		}
	}
	samples := make([]float64, 0, len(urls)*samplesPerURL)
	var winningCDN []float64
	winningLatency := math.MaxFloat64
	for _, values := range ordered {
		samples = append(samples, values...)
		for _, value := range values {
			if value < winningLatency {
				winningLatency = value
				winningCDN = values
			}
		}
	}
	if len(samples) == 0 {
		return provider.PingResult{}, provider.NewError(p.Name(), "ping", provider.CodeUnavailable, true, errors.New("все запросы задержки завершились с ошибкой"))
	}
	result := provider.StatsWithMethod(samples, "minimum")
	// Jitter across different CDN locations (and their first TLS request) is
	// meaningless. Report variation of the reused-connection samples from the
	// CDN that produced the primary minimum RTT.
	if len(winningCDN) > 2 {
		result.JitterMs = provider.MedianAbsoluteDeviation(winningCDN[1:])
	} else {
		result.JitterMs = 0
	}
	return result, nil
}

func (p *Provider) pingOnce(ctx context.Context, probeURL string) (time.Duration, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, cacheBust(probeURL), nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept-Encoding", "identity")
	start := time.Now()
	resp, err := p.client.Do(req)
	if err != nil {
		return 0, err
	}
	latency := time.Since(start)
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return 0, fmt.Errorf("запрос задержки вернул состояние %s", resp.Status)
	}
	responseBytes, readErr := io.Copy(io.Discard, io.LimitReader(resp.Body, maxPingBodySize+1))
	if readErr != nil {
		return 0, readErr
	}
	if responseBytes > maxPingBodySize {
		return 0, errors.New("ответ на запрос задержки превышает безопасный предел")
	}
	return latency, nil
}

func (p *Provider) Download(ctx context.Context, cfg provider.MeasurementConfig, progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	p.mu.RLock()
	urls := append([]string(nil), p.downloadURLs...)
	p.mu.RUnlock()
	if len(urls) == 0 {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "download", provider.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}
	initial, maximum, err := connectionLimits(cfg, len(urls))
	if err != nil {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "download", provider.CodeInternal, false, err)
	}

	runResult, err := measure.Run(ctx, measure.RunConfig{
		Duration:       cfg.Duration,
		Warmup:         750 * time.Millisecond,
		StartupTimeout: 8 * time.Second,
		ReadyGrace:     400 * time.Millisecond,
		InitialWorkers: initial,
		MaxWorkers:     maximum,
		Reconnects:     1,
		Adaptive:       adaptiveConnections,
		OnWorkerError:  verboseWorkerError(cfg, "download"),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		probeURL := urls[index%len(urls)]
		buf := make([]byte, 64<<10)
		for workerCtx.Err() == nil {
			_, reqErr := p.downloadProbe(workerCtx, probeURL, buf, ready, record)
			if reqErr != nil {
				return reqErr
			}
		}
		return workerCtx.Err()
	}, adaptProgress(progress))
	result := convertRunResult(runResult)
	if err != nil {
		code := errorCode(err)
		return result, provider.NewError(p.Name(), "download", code, code != provider.CodeCanceled, err)
	}
	return result, nil
}

func (p *Provider) Upload(ctx context.Context, cfg provider.MeasurementConfig, progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	p.mu.RLock()
	probes := append([]uploadProbe(nil), p.uploadProbes...)
	p.mu.RUnlock()
	if len(probes) == 0 {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "upload", provider.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}
	initial, maximum, err := connectionLimits(cfg, len(probes))
	if err != nil {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "upload", provider.CodeInternal, false, err)
	}

	// The browser client uses zero-filled ArrayBuffer payloads. Reusing one
	// immutable slice across workers avoids needless CPU work and allocations.
	payload := make([]byte, uploadChunkSize)
	runResult, err := measure.Run(ctx, measure.RunConfig{
		Duration:       cfg.Duration,
		Warmup:         750 * time.Millisecond,
		StartupTimeout: 10 * time.Second,
		ReadyGrace:     400 * time.Millisecond,
		InitialWorkers: initial,
		MaxWorkers:     maximum,
		Reconnects:     1,
		Adaptive:       adaptiveConnections,
		OnWorkerError:  verboseWorkerError(cfg, "upload"),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		probe := probes[index%len(probes)]
		return p.uploadWorker(workerCtx, probe, payload, cfg, ready, record)
	}, adaptProgress(progress))
	result := convertRunResult(runResult)
	if err != nil {
		code := errorCode(err)
		return result, provider.NewError(p.Name(), "upload", code, code != provider.CodeCanceled, err)
	}
	return result, nil
}

func (p *Provider) downloadProbe(ctx context.Context, probeURL string, buf []byte, ready func(), record func(int64)) (int64, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, cacheBust(probeURL), nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept-Encoding", "identity")
	resp, err := p.client.Do(req)
	if err != nil {
		return 0, err
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return 0, fmt.Errorf("проба скачивания вернула состояние %s", resp.Status)
	}
	expectedBytes := resp.ContentLength
	if expectedBytes <= 0 {
		return 0, errors.New("проба скачивания не содержит положительный Content-Length")
	}
	if expectedBytes > maxDownloadBodySize {
		return 0, fmt.Errorf("Content-Length пробы скачивания (%d) превышает безопасный предел", expectedBytes)
	}
	if isLargeDownloadProbe(probeURL) && expectedBytes != expectedDownloadSize {
		return 0, fmt.Errorf("неожиданный размер пробы скачивания: %d байт вместо %d", expectedBytes, expectedDownloadSize)
	}
	if encoding := strings.TrimSpace(resp.Header.Get("Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
		return 0, fmt.Errorf("проба скачивания использует неожиданное Content-Encoding %q", encoding)
	}
	contentType, _, contentTypeErr := mime.ParseMediaType(resp.Header.Get("Content-Type"))
	if contentTypeErr != nil || contentType != "application/octet-stream" {
		return 0, fmt.Errorf("проба скачивания вернула неожиданный Content-Type %q", resp.Header.Get("Content-Type"))
	}
	ready()
	var readBytes int64
	for {
		n, readErr := resp.Body.Read(buf)
		if n > 0 {
			if readBytes+int64(n) > expectedBytes {
				return readBytes, fmt.Errorf("проба скачивания превысила ожидаемый размер %d байт", expectedBytes)
			}
			readBytes += int64(n)
			// A validated 2xx response confirms bytes as they arrive. Recording
			// them per read keeps a timed test accurate even when its context
			// intentionally stops a large probe before EOF.
			record(int64(n))
		}
		if readErr != nil {
			if errors.Is(readErr, io.EOF) {
				break
			}
			if ctx.Err() != nil {
				return readBytes, ctx.Err()
			}
			return readBytes, readErr
		}
	}
	if readBytes == 0 {
		return 0, errors.New("проба скачивания вернула пустой ответ")
	}
	if expectedBytes >= 0 && readBytes != expectedBytes {
		return readBytes, fmt.Errorf("проба скачивания вернула %d байт, ожидалось %d", readBytes, expectedBytes)
	}
	return readBytes, nil
}

func (p *Provider) uploadWorker(ctx context.Context, probe uploadProbe, payload []byte, cfg provider.MeasurementConfig, ready func(), record func(int64)) error {
	if probe.websocketURL != "" {
		if wsErr := p.websocketUploadWithTimeout(ctx, probe.websocketURL, probe.websocketConnectionTimeout, cfg.Duration, ready, record); wsErr == nil || ctx.Err() != nil {
			return wsErr
		} else if cfg.Verbose != nil {
			cfg.Verbose("Яндекс · отдача: WebSocket недоступен, переход на резервный HTTP-запрос: %v", wsErr)
		}
	}
	return p.httpUpload(ctx, probe.postURL, payload, ready, record)
}

func (p *Provider) websocketUpload(ctx context.Context, rawURL string, duration time.Duration, ready func(), record func(int64)) error {
	return p.websocketUploadWithTimeout(ctx, rawURL, 0, duration, ready, record)
}

func (p *Provider) websocketUploadWithTimeout(ctx context.Context, rawURL string, connectionTimeout, duration time.Duration, ready func(), record func(int64)) error {
	u, err := url.Parse(rawURL)
	if err != nil {
		return err
	}
	if !validWebSocketProbeURL(rawURL) {
		return errors.New("неверный URL WebSocket upload-пробы")
	}
	query := u.Query()
	query.Set("type", "upload")
	query.Set("duration", strconv.Itoa(max(1, int(math.Ceil(duration.Seconds()+2)))))
	u.RawQuery = query.Encode()
	header := http.Header{"User-Agent": []string{userAgent}, "Origin": []string{"https://yandex.ru"}}
	dialCtx := ctx
	cancelDial := func() {}
	if connectionTimeout > 0 {
		dialCtx, cancelDial = context.WithTimeout(ctx, connectionTimeout)
	}
	conn, response, err := p.dialer.DialContext(dialCtx, u.String(), header)
	cancelDial()
	if err != nil {
		if response != nil {
			_ = response.Body.Close()
		}
		return fmt.Errorf("подключение WebSocket: %w", err)
	}
	defer conn.Close()
	conn.SetReadLimit(maxWebsocketAckSize)
	closeDone := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-closeDone:
		}
	}()
	defer close(closeDone)
	writeErr := make(chan error, 1)
	writeDone := make(chan struct{})
	var sent atomic.Int64
	go func() {
		defer close(writeDone)
		for ctx.Err() == nil {
			if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
				writeErr <- err
				_ = conn.Close()
				return
			}
			// Reserve before WriteMessage: a fast local/server peer may deliver
			// its acknowledgement before this goroutine is scheduled again.
			sent.Add(websocketChunkSize)
			if err := conn.WriteMessage(websocket.BinaryMessage, websocketUploadChunk); err != nil {
				sent.Add(-websocketChunkSize)
				writeErr <- err
				_ = conn.Close()
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
	defer func() {
		_ = conn.Close()
		<-writeDone
	}()

	acked := int64(0)
	for {
		ackBytes, readErr := readWebsocketAcknowledgement(conn)
		if readErr != nil {
			if ctx.Err() != nil {
				return ctx.Err()
			}
			select {
			case err := <-writeErr:
				return fmt.Errorf("отправка через WebSocket: %w", err)
			default:
			}
			return fmt.Errorf("получение через WebSocket: %w", readErr)
		}
		available := sent.Load() - acked
		if available < 0 || ackBytes > available {
			return fmt.Errorf("%w: объём превышает отправленные данные", errInvalidWebsocketAcknowledgement)
		}
		// The first-party server sends a schema-valid b=0 control frame when
		// the stream starts. It confirms no payload and must neither count as
		// traffic nor trigger the HTTP fallback.
		if ackBytes == 0 {
			continue
		}
		ready()
		acked += ackBytes
		record(ackBytes)
	}
}

func readWebsocketAcknowledgement(conn *websocket.Conn) (int64, error) {
	messageType, reader, err := conn.NextReader()
	if err != nil {
		return 0, err
	}
	if messageType != websocket.TextMessage {
		return 0, fmt.Errorf("%w: неожиданный тип сообщения %d", errInvalidWebsocketAcknowledgement, messageType)
	}
	type acknowledgement struct {
		Kind  string `json:"k"`
		Bytes *int64 `json:"b"`
	}
	limited := &io.LimitedReader{R: reader, N: maxWebsocketAckSize + 1}
	decoder := json.NewDecoder(limited)
	var ack acknowledgement
	if err := decoder.Decode(&ack); err != nil {
		return 0, fmt.Errorf("%w: ошибка JSON: %v", errInvalidWebsocketAcknowledgement, err)
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return 0, fmt.Errorf("%w: ошибка JSON: %v", errInvalidWebsocketAcknowledgement, err)
	}
	if limited.N <= 0 {
		return 0, fmt.Errorf("%w: сообщение превышает безопасный предел", errInvalidWebsocketAcknowledgement)
	}
	if ack.Kind != "u" || ack.Bytes == nil || *ack.Bytes < 0 {
		return 0, fmt.Errorf("%w: неверная схема", errInvalidWebsocketAcknowledgement)
	}
	return *ack.Bytes, nil
}

func (p *Provider) httpUpload(ctx context.Context, postURL string, payload []byte, ready func(), record func(int64)) error {
	for ctx.Err() == nil {
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, cacheBust(postURL), bytes.NewReader(payload))
		if err != nil {
			return err
		}
		req.Header.Set("User-Agent", userAgent)
		req.Header.Set("Content-Type", "application/octet-stream")
		resp, err := p.client.Do(req)
		if err != nil {
			return err
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			_ = resp.Body.Close()
			return fmt.Errorf("HTTP-запрос отдачи вернул состояние %s", resp.Status)
		}
		if resp.ContentLength > maxUploadResponse {
			_ = resp.Body.Close()
			return errors.New("неожиданный размер ответа HTTP при отдаче: превышен безопасный предел")
		}
		responseBytes, copyErr := io.Copy(io.Discard, io.LimitReader(resp.Body, maxUploadResponse+1))
		closeErr := resp.Body.Close()
		if copyErr != nil {
			return copyErr
		}
		if responseBytes > 1<<20 {
			return errors.New("ответ HTTP при отдаче превышает безопасный предел")
		}
		if closeErr != nil {
			return closeErr
		}
		ready()
		record(int64(len(payload)))
	}
	return ctx.Err()
}

func connectionLimits(cfg provider.MeasurementConfig, native int) (int, int, error) {
	if cfg.Duration < 3*time.Second || cfg.Duration > 60*time.Second {
		return 0, 0, errors.New("длительность должна быть от 3 до 60 секунд")
	}
	maximum := cfg.MaxConnections
	if maximum == 0 {
		maximum = 16
	}
	if maximum < 1 || maximum > 16 {
		return 0, 0, errors.New("предельное число потоков должно быть от 1 до 16")
	}
	initial := cfg.Connections
	if initial == 0 {
		initial = native
	}
	if initial < 1 || initial > maximum {
		return 0, 0, fmt.Errorf("число потоков должно быть от 1 до %d", maximum)
	}
	return initial, maximum, nil
}

func adaptiveConnections(mbps float64) int {
	switch {
	case mbps > 300:
		return 16
	case mbps > 50:
		return 12
	default:
		return 8
	}
}

func verboseWorkerError(cfg provider.MeasurementConfig, phase string) func(int, int, error) {
	if cfg.Verbose == nil {
		return nil
	}
	return func(index, attempt int, err error) {
		reconnect := "нет"
		if attempt == 0 {
			reconnect = "да"
		}
		cfg.Verbose("Яндекс · %s: поток=%d, попытка=%d, переподключение=%s: %v", phaseDisplayName(phase), index+1, attempt+1, reconnect, err)
	}
}

func adaptProgress(progress func(provider.ThroughputProgress)) func(measure.RunProgress) {
	if progress == nil {
		return nil
	}
	return func(p measure.RunProgress) {
		progress(provider.ThroughputProgress{Mbps: p.Mbps, Bytes: p.Bytes, Elapsed: p.Elapsed, ActiveConnections: p.Active})
	}
}

func convertRunResult(result measure.RunResult) provider.ThroughputResult {
	warnings := make([]string, 0, len(result.WorkerErrors))
	seen := make(map[string]struct{})
	for _, err := range result.WorkerErrors {
		if err == nil {
			continue
		}
		text := err.Error()
		if _, ok := seen[text]; ok {
			continue
		}
		seen[text] = struct{}{}
		warnings = append(warnings, text)
	}
	return provider.ThroughputResult{
		Mbps:                  measure.Mbps(result.Bytes, result.Elapsed),
		Bytes:                 result.Bytes,
		Elapsed:               result.Elapsed,
		SuccessfulConnections: result.WorkersOK,
		FailedConnections:     result.WorkersFailed,
		Warnings:              warnings,
	}
}

func errorCode(err error) provider.ErrorCode {
	if errors.Is(err, context.Canceled) {
		return provider.CodeCanceled
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return provider.CodeTimeout
	}
	if errors.Is(err, errInvalidWebsocketAcknowledgement) {
		return provider.CodeProtocol
	}
	message := strings.ToLower(err.Error())
	if strings.Contains(message, "неожидан") || strings.Contains(message, "неверн") || strings.Contains(message, "пуст") || strings.Contains(message, "content-length") || strings.Contains(message, "размер кадра") {
		return provider.CodeProtocol
	}
	return provider.CodeUnavailable
}

func phaseDisplayName(phase string) string {
	if phase == "download" {
		return "скачивание"
	}
	if phase == "upload" {
		return "отдача"
	}
	return phase
}

var cacheBustCounter atomic.Uint64

func cacheBust(rawURL string) string {
	sep := "&"
	if !strings.Contains(rawURL, "?") {
		sep = "?"
	}
	value := strconv.FormatInt(time.Now().UnixNano(), 36) + "-" + strconv.FormatUint(cacheBustCounter.Add(1), 36)
	return rawURL + sep + "cb=" + value
}
