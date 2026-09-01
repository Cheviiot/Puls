// Package speedtestru implements the first-party QMS protocol used by
// speedtest.ru. Server discovery and short-lived JWT issuance use the public
// browser API; measurements themselves go directly to the selected QMS hosts.
package speedtestru

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"sort"
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
	defaultAPIBase     = "https://speedtest.ru"
	defaultBrowserKey  = "5f3287b55fbcd8076919114885f8f3f7"
	userAgent          = "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)"
	uploadBlockSize    = 2 * 1024 * 1024
	maxDiscoveryBytes  = 4 << 20
	maxPageBytes       = 8 << 20
	maxJWTBytes        = 1 << 20
	apiRequestTimeout  = 15 * time.Second
	websocketReadLimit = 64 << 10
)

var errPingOnlyFallback = errors.New("резервный список speedtest.ru поддерживает только проверку задержки")

var knownServers = []struct{ host, city string }{
	{"vladivostok.qms.ru:20000", "Владивосток"},
	{"vladivostok10.qms.ru:20000", "Владивосток"},
	{"khabarovsk1.qms.ru:20000", "Хабаровск"},
	{"blagoveshchensk.qms.ru:20000", "Благовещенск"},
	{"yuzhno-sahalinsk.qms.ru:20000", "Южно-Сахалинск"},
	{"chita.qms.ru:20000", "Чита"},
	{"ulan-ude.qms.ru:20000", "Улан-Удэ"},
	{"yakutsk.qms.ru:20000", "Якутск"},
	{"magadan.qms.ru:20000", "Магадан"},
}

type qmsServer struct {
	ID       int
	Host     string
	Name     string
	City     string
	Region   string
	Provider string
	RTT      time.Duration
	Ping     provider.PingResult
}

func (s qmsServer) wsURL() string   { return "wss://" + s.Host + "/" }
func (s qmsServer) httpURL() string { return "https://" + s.Host + "/" }

type nearestResponse struct {
	Algorithm string `json:"algorithm"`
	Data      []struct {
		ID         int    `json:"id"`
		Name       string `json:"name"`
		City       string `json:"city"`
		Src        string `json:"src"`
		Source     string `json:"source"`
		Port       int    `json:"port"`
		RegionName string `json:"region_name"`
	} `json:"data"`
}

// Provider speaks QMS to either a caller-selected host or dynamically
// discovered speedtest.ru hosts.
type Provider struct {
	host string

	client  *http.Client
	dialer  *websocket.Dialer
	apiBase string

	mu           sync.RWMutex
	servers      []qmsServer
	selected     qmsServer
	browserKey   string
	jwt          string
	authMu       sync.Mutex
	throughput   bool
	discoveryErr error
	verbose      func(string, ...any)
}

func New(host string) *Provider {
	transport := &http.Transport{
		DialContext:           (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
		TLSClientConfig:       &tls.Config{MinVersion: tls.VersionTLS12},
		MaxIdleConns:          32,
		MaxIdleConnsPerHost:   16,
		MaxConnsPerHost:       16,
		DisableCompression:    true,
		ForceAttemptHTTP2:     true,
		IdleConnTimeout:       30 * time.Second,
		TLSHandshakeTimeout:   8 * time.Second,
		ExpectContinueTimeout: time.Second,
		ResponseHeaderTimeout: 12 * time.Second,
	}
	return &Provider{
		host:   host,
		client: &http.Client{Transport: transport},
		dialer: &websocket.Dialer{
			HandshakeTimeout: 8 * time.Second,
			NetDialContext:   (&net.Dialer{Timeout: 5 * time.Second, KeepAlive: 30 * time.Second}).DialContext,
			TLSClientConfig:  &tls.Config{MinVersion: tls.VersionTLS12},
		},
		apiBase:    defaultAPIBase,
		browserKey: defaultBrowserKey,
	}
}

func (p *Provider) Name() string { return "speedtest.ru" }

func (p *Provider) SetVerbose(logger func(string, ...any)) {
	p.mu.Lock()
	p.verbose = logger
	p.mu.Unlock()
}

func (p *Provider) logf(format string, values ...any) {
	p.mu.RLock()
	logger := p.verbose
	p.mu.RUnlock()
	if logger != nil {
		logger(format, values...)
	}
}

func (p *Provider) Capabilities() provider.Capability {
	return provider.CapPing | provider.CapDownload | provider.CapUpload
}

func (p *Provider) SelectServer(ctx context.Context) (provider.Server, error) {
	if err := ctx.Err(); err != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", errorCode(err), false, err)
	}
	if p.host != "" {
		host, err := normalizeHost(p.host, "20000")
		if err != nil {
			return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeInternal, false, err)
		}
		server := qmsServer{Host: host, Name: host}
		p.mu.Lock()
		p.selected = server
		p.servers = []qmsServer{server}
		p.throughput = true
		p.discoveryErr = nil
		p.mu.Unlock()
		return provider.Server{Name: host}, nil
	}

	servers, err := p.discover(ctx)
	if err == nil && len(servers) > 0 {
		responsive := p.selectResponsive(ctx, servers)
		if len(responsive) > 0 {
			// Limit the native multi-host mode to four hosts; two streams per
			// host still leaves ample room under the global 16-stream cap.
			if len(responsive) > 4 {
				responsive = responsive[:4]
			}
			p.mu.Lock()
			p.selected = responsive[0]
			p.servers = responsive
			p.throughput = true
			p.discoveryErr = nil
			p.mu.Unlock()
			s := responsive[0]
			p.logf("speedtest.ru · поиск серверов: выбран=%s, доступных вариантов=%d", s.Host, len(responsive))
			return provider.Server{Name: s.Host, City: s.City, Region: s.Region}, nil
		}
		err = errors.New("ни один найденный сервер QMS не завершил согласование WebSocket")
	}
	p.logf("speedtest.ru · поиск серверов недоступен, используется встроенный список только для задержки: %v", err)
	if contextErr := ctx.Err(); contextErr != nil {
		return provider.Server{}, provider.NewError(p.Name(), "select", errorCode(contextErr), false, contextErr)
	}

	// The public API is an optimization, not a single point of failure for
	// ping. A small built-in list keeps latency measurement available.
	fallback, fallbackErr := autoSelect(ctx)
	if fallbackErr != nil {
		if err != nil {
			fallbackErr = errors.Join(err, fallbackErr)
		}
		return provider.Server{}, provider.NewError(p.Name(), "select", provider.CodeUnavailable, true,
			fmt.Errorf("не удалось автоматически выбрать сервер (%w); укажите адрес через --server", fallbackErr))
	}
	server := fallback
	p.mu.Lock()
	p.selected = server
	p.servers = []qmsServer{server}
	p.throughput = false
	p.discoveryErr = err
	p.mu.Unlock()
	return provider.Server{Name: server.Host, City: server.City}, nil
}

func normalizeHost(host, defaultPort string) (string, error) {
	host = strings.TrimSpace(host)
	if host == "" {
		return "", errors.New("адрес сервера не может быть пустым")
	}
	if strings.ContainsAny(host, "/?#@") || strings.Contains(host, "://") {
		return "", fmt.Errorf("неверный адрес сервера %q: укажите host или host:port", host)
	}
	if ip := net.ParseIP(strings.Trim(host, "[]")); ip != nil {
		return net.JoinHostPort(ip.String(), defaultPort), nil
	}
	hostname, port, err := net.SplitHostPort(host)
	if err != nil {
		if strings.Contains(host, ":") {
			return "", fmt.Errorf("неверный адрес сервера %q: %w", host, err)
		}
		hostname, port = host, defaultPort
	}
	if hostname == "" || strings.ContainsAny(hostname, " \t\r\n") {
		return "", fmt.Errorf("неверное имя сервера %q", hostname)
	}
	portNumber, err := strconv.Atoi(port)
	if err != nil || portNumber < 1 || portNumber > 65535 {
		return "", fmt.Errorf("неверный порт сервера %q", port)
	}
	return net.JoinHostPort(hostname, strconv.Itoa(portNumber)), nil
}

func (p *Provider) discover(ctx context.Context) ([]qmsServer, error) {
	key := p.currentBrowserKey()
	servers, status, err := p.fetchNearest(ctx, key)
	if err == nil {
		return servers, nil
	}
	if status != http.StatusUnauthorized && status != http.StatusForbidden {
		return nil, err
	}
	refreshed, refreshErr := p.extractBrowserKey(ctx)
	if refreshErr != nil {
		return nil, errors.Join(err, refreshErr)
	}
	p.mu.Lock()
	p.browserKey = refreshed
	p.jwt = ""
	p.mu.Unlock()
	p.logf("speedtest.ru · ключ браузерного клиента обновлён из текущего JS-файла сервиса")
	servers, _, err = p.fetchNearest(ctx, refreshed)
	return servers, err
}

func (p *Provider) fetchNearest(ctx context.Context, key string) ([]qmsServer, int, error) {
	requestCtx, cancel := context.WithTimeout(ctx, apiRequestTimeout)
	defer cancel()
	endpoint := p.apiBase + "/api/nearest_servers?t=" + strconv.FormatInt(time.Now().UnixMilli(), 10)
	req, err := http.NewRequestWithContext(requestCtx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, 0, err
	}
	req.Header.Set("x-api-key", key)
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")
	resp, err := p.client.Do(req)
	if err != nil {
		return nil, 0, fmt.Errorf("запрос nearest_servers: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, resp.StatusCode, fmt.Errorf("nearest_servers вернул состояние %s", resp.Status)
	}
	var payload nearestResponse
	if err := decodeJSONLimited(resp.Body, maxDiscoveryBytes, &payload); err != nil {
		return nil, resp.StatusCode, fmt.Errorf("разбор ответа nearest_servers: %w", err)
	}
	if strings.TrimSpace(payload.Algorithm) == "" {
		return nil, resp.StatusCode, errors.New("nearest_servers не вернул алгоритм выбора серверов")
	}
	servers := make([]qmsServer, 0, len(payload.Data))
	seen := make(map[string]struct{}, len(payload.Data))
	for _, item := range payload.Data {
		u, parseErr := url.Parse(item.Src)
		if parseErr != nil || u.Scheme != "https" || u.Hostname() == "" || u.User != nil ||
			u.RawQuery != "" || u.Fragment != "" || (u.Path != "" && u.Path != "/") ||
			item.ID <= 0 || strings.TrimSpace(item.Name) == "" || strings.TrimSpace(item.City) == "" ||
			strings.TrimSpace(item.Source) == "" || item.Port < 1 || item.Port > 65535 {
			continue
		}
		host := net.JoinHostPort(u.Hostname(), strconv.Itoa(item.Port))
		if _, duplicate := seen[host]; duplicate {
			continue
		}
		seen[host] = struct{}{}
		servers = append(servers, qmsServer{
			ID: item.ID, Host: host, Name: item.Name, City: item.City,
			Region: item.RegionName, Provider: item.Source,
		})
	}
	if len(servers) == 0 {
		return nil, resp.StatusCode, errors.New("nearest_servers не вернул корректных адресов")
	}
	return servers, resp.StatusCode, nil
}

func (p *Provider) extractBrowserKey(ctx context.Context) (string, error) {
	requestCtx, cancel := context.WithTimeout(ctx, apiRequestTimeout)
	defer cancel()
	req, err := http.NewRequestWithContext(requestCtx, http.MethodGet, p.apiBase+"/", nil)
	if err != nil {
		return "", err
	}
	req.Header.Set("User-Agent", userAgent)
	resp, err := p.client.Do(req)
	if err != nil {
		return "", fmt.Errorf("загрузка страницы speedtest.ru: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", fmt.Errorf("страница speedtest.ru вернула состояние %s", resp.Status)
	}
	html, err := readLimited(resp.Body, maxPageBytes)
	if err != nil {
		return "", fmt.Errorf("чтение страницы speedtest.ru: %w", err)
	}
	assetPattern := regexp.MustCompile(`src=["']([^"']+\.js[^"']*)["']`)
	assets := assetPattern.FindAllSubmatch(html, 32)
	if len(assets) == 0 {
		return "", errors.New("не удалось найти адрес JS-файла страницы")
	}
	keyPattern := regexp.MustCompile(`QMSLibrary\(\{\s*id\s*:\s*["']([A-Za-z0-9_-]{16,128})["']`)
	for _, match := range assets {
		assetURL, resolveErr := url.Parse(string(match[1]))
		if resolveErr != nil {
			continue
		}
		base, _ := url.Parse(p.apiBase)
		assetURL = base.ResolveReference(assetURL)
		if !sameOrigin(base, assetURL) {
			continue
		}
		assetReq, reqErr := http.NewRequestWithContext(requestCtx, http.MethodGet, assetURL.String(), nil)
		if reqErr != nil {
			continue
		}
		assetReq.Header.Set("User-Agent", userAgent)
		assetResp, reqErr := p.client.Do(assetReq)
		if reqErr != nil {
			continue
		}
		body, readErr := readLimited(assetResp.Body, maxPageBytes)
		finalURL := assetResp.Request.URL
		assetResp.Body.Close()
		if readErr != nil || assetResp.StatusCode < 200 || assetResp.StatusCode >= 300 || !sameOrigin(base, finalURL) {
			continue
		}
		if keyMatch := keyPattern.FindSubmatch(body); len(keyMatch) == 2 {
			return string(keyMatch[1]), nil
		}
	}
	return "", errors.New("в текущем JS-файле страницы не найден ключ браузерного клиента QMS")
}

func (p *Provider) currentBrowserKey() string {
	p.mu.RLock()
	defer p.mu.RUnlock()
	return p.browserKey
}

func (p *Provider) selectResponsive(ctx context.Context, servers []qmsServer) []qmsServer {
	if len(servers) > 8 {
		servers = servers[:8]
	}
	probeCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()
	type result struct {
		server qmsServer
		err    error
	}
	results := make(chan result, len(servers))
	jobs := make(chan qmsServer)
	workerCount := min(4, len(servers))
	for range workerCount {
		go func() {
			for candidate := range jobs {
				ping, err := p.probePing(probeCtx, candidate, 10)
				candidate.Ping = ping
				candidate.RTT = time.Duration(ping.MedianMs * float64(time.Millisecond))
				results <- result{server: candidate, err: err}
			}
		}()
	}
	go func() {
		defer close(jobs)
		for _, candidate := range servers {
			select {
			case jobs <- candidate:
			case <-probeCtx.Done():
				return
			}
		}
	}()
	responsive := make([]qmsServer, 0, len(servers))
	for range servers {
		select {
		case result := <-results:
			if result.err == nil {
				responsive = append(responsive, result.server)
			}
		case <-probeCtx.Done():
			return sortServers(responsive)
		}
	}
	return sortServers(responsive)
}

func sortServers(servers []qmsServer) []qmsServer {
	// Discovery is already ordered by geographic proximity. Keeping that
	// order for equal medians avoids arbitrary server changes between runs.
	sort.SliceStable(servers, func(i, j int) bool { return servers[i].RTT < servers[j].RTT })
	return servers
}

func autoSelect(ctx context.Context) (qmsServer, error) {
	probeCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
	defer cancel()
	type result struct {
		server qmsServer
		err    error
	}
	results := make(chan result, len(knownServers))
	p := New("")
	jobs := make(chan struct{ host, city string })
	for range min(3, len(knownServers)) {
		go func() {
			for candidate := range jobs {
				server := qmsServer{Host: candidate.host, Name: candidate.host, City: candidate.city}
				ping, err := p.probePing(probeCtx, server, 10)
				server.Ping = ping
				server.RTT = time.Duration(ping.MedianMs * float64(time.Millisecond))
				results <- result{server: server, err: err}
			}
		}()
	}
	go func() {
		defer close(jobs)
		for _, candidate := range knownServers {
			select {
			case jobs <- candidate:
			case <-probeCtx.Done():
				return
			}
		}
	}()
	var best result
	have := false
	for range knownServers {
		select {
		case candidate := <-results:
			if candidate.err == nil && (!have || candidate.server.RTT < best.server.RTT) {
				best, have = candidate, true
			}
		case <-probeCtx.Done():
			if !have {
				return qmsServer{}, errors.New("ни один известный сервер не ответил вовремя")
			}
			return best.server, nil
		}
	}
	if !have {
		return qmsServer{}, errors.New("ни один известный сервер не ответил вовремя")
	}
	return best.server, nil
}

func (p *Provider) probePing(ctx context.Context, server qmsServer, sampleCount int) (provider.PingResult, error) {
	conn, err := p.dial(ctx, server.wsURL())
	if err != nil {
		return provider.PingResult{}, err
	}
	defer conn.Close()
	stop := interruptOnDone(ctx, conn)
	defer stop()
	if err := handshake(conn); err != nil {
		return provider.PingResult{}, err
	}
	samples := make([]float64, 0, sampleCount)
	for range sampleCount {
		duration, pingErr := pingOnce(conn)
		if pingErr != nil {
			return provider.PingResult{}, pingErr
		}
		samples = append(samples, duration.Seconds()*1000)
	}
	result := provider.StatsWithMethod(samples, "median")
	result.JitterMs = qmsJitter(samples, result.MedianMs)
	return result, nil
}

func (p *Provider) dial(ctx context.Context, wsURL string) (*websocket.Conn, error) {
	conn, resp, err := p.dialer.DialContext(ctx, wsURL, http.Header{"User-Agent": []string{userAgent}})
	if err != nil {
		if resp != nil && resp.Body != nil {
			_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 64<<10))
			_ = resp.Body.Close()
		}
		return nil, fmt.Errorf("подключение WebSocket к %s: %w", wsURL, err)
	}
	conn.SetReadLimit(websocketReadLimit)
	if deadline, ok := ctx.Deadline(); ok {
		if err := conn.SetReadDeadline(deadline); err != nil {
			conn.Close()
			return nil, err
		}
	}
	return conn, nil
}

func interruptOnDone(ctx context.Context, conn *websocket.Conn) func() {
	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = conn.Close()
		case <-done:
		}
	}()
	return func() { close(done) }
}

func (p *Provider) Ping(ctx context.Context) (provider.PingResult, error) {
	if err := ctx.Err(); err != nil {
		return provider.PingResult{}, provider.NewError(p.Name(), "ping", provider.CodeCanceled, false, err)
	}
	p.mu.RLock()
	server := p.selected
	servers := append([]qmsServer(nil), p.servers...)
	p.mu.RUnlock()
	if server.Host == "" {
		return provider.PingResult{}, provider.NewError(p.Name(), "ping", provider.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}
	if server.Ping.Samples == 10 && server.Ping.ValueMs > 0 {
		return server.Ping, nil
	}
	if len(servers) == 0 {
		servers = []qmsServer{server}
	}
	ordered := make([]qmsServer, 0, len(servers))
	ordered = append(ordered, server)
	for _, candidate := range servers {
		if candidate.Host != server.Host {
			ordered = append(ordered, candidate)
		}
	}
	var pingErrors []error
	failedHosts := make(map[string]struct{}, len(ordered))
	for _, candidate := range ordered {
		result, err := p.probePing(ctx, candidate, 10)
		if err == nil {
			if candidate.Host != server.Host {
				p.logf("speedtest.ru · задержка: переход с недоступного сервера %s на %s", server.Host, candidate.Host)
			}
			candidate.Ping = result
			candidate.RTT = time.Duration(result.MedianMs * float64(time.Millisecond))
			p.mu.Lock()
			p.selected = candidate
			healthy := make([]qmsServer, 0, len(servers)-len(failedHosts))
			healthy = append(healthy, candidate)
			for _, remaining := range servers {
				if remaining.Host == candidate.Host {
					continue
				}
				if _, failed := failedHosts[remaining.Host]; !failed {
					healthy = append(healthy, remaining)
				}
			}
			p.servers = healthy
			p.mu.Unlock()
			return result, nil
		}
		failedHosts[candidate.Host] = struct{}{}
		pingErrors = append(pingErrors, fmt.Errorf("%s: %w", candidate.Host, err))
		if ctx.Err() != nil {
			break
		}
	}
	err := errors.Join(pingErrors...)
	code := errorCode(err)
	return provider.PingResult{}, provider.NewError(p.Name(), "ping", code, retryableForCode(code), err)
}

func handshake(conn *websocket.Conn) error {
	if err := conn.WriteMessage(websocket.TextMessage, []byte("HI")); err != nil {
		return fmt.Errorf("отправка HI: %w", err)
	}
	if err := expectExact(conn, "HELLO"); err != nil {
		return err
	}
	if err := conn.WriteMessage(websocket.TextMessage, []byte("GETINFO")); err != nil {
		return fmt.Errorf("отправка GETINFO: %w", err)
	}
	if err := expectServerInfo(conn); err != nil {
		return err
	}
	if err := conn.WriteMessage(websocket.TextMessage, []byte("AUTH")); err != nil {
		return fmt.Errorf("отправка AUTH: %w", err)
	}
	return expectExact(conn, "READY_TO_TEST")
}

func pingOnce(conn *websocket.Conn) (time.Duration, error) {
	start := time.Now()
	if err := conn.WriteMessage(websocket.TextMessage, []byte("PING")); err != nil {
		return 0, err
	}
	messageType, message, err := conn.ReadMessage()
	if err != nil {
		return 0, err
	}
	if messageType != websocket.TextMessage {
		return 0, fmt.Errorf("неожиданный тип ответа PONG: %d", messageType)
	}
	fields := strings.Fields(string(message))
	if len(fields) != 2 || fields[0] != "PONG" {
		return 0, fmt.Errorf("неожиданный ответ на проверку задержки: %q", message)
	}
	stamp, err := strconv.ParseInt(fields[1], 10, 64)
	if err != nil || stamp < 0 {
		return 0, fmt.Errorf("неверная временная метка PONG: %q", fields[1])
	}
	return time.Since(start), nil
}

func expectExact(conn *websocket.Conn, expected string) error {
	messageType, message, err := conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("получение %s: %w", expected, err)
	}
	if messageType != websocket.TextMessage || strings.TrimSpace(string(message)) != expected {
		return fmt.Errorf("неожиданный ответ %q, ожидался %q", message, expected)
	}
	return nil
}

func expectServerInfo(conn *websocket.Conn) error {
	messageType, message, err := conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("получение qms_testing: %w", err)
	}
	if messageType != websocket.TextMessage {
		return fmt.Errorf("неожиданный тип кадра со сведениями о сервере: %d", messageType)
	}
	serverInfo := strings.TrimSpace(strings.ToLower(string(message)))
	if serverInfo == "qms_testing" || strings.HasPrefix(serverInfo, "qms_testing/") {
		return nil
	}
	var payload struct {
		Name string `json:"name"`
	}
	if json.Unmarshal(message, &payload) == nil && strings.EqualFold(payload.Name, "qms_testing") {
		return nil
	}
	return fmt.Errorf("неожиданный ответ со сведениями о сервере: %q", message)
}

func qmsJitter(samples []float64, median float64) float64 {
	// Keep this in sync with speedtest.ru's Sr() calculation: it sorts the
	// samples, trims one quarter on either side and averages adjacent deltas.
	// The unusual lower bound and high-jitter fallback are provider-native.
	if len(samples) < 2 {
		return 0
	}
	values := append([]float64(nil), samples...)
	sort.Float64s(values)
	quarter := len(values) / 4
	trimmed := values[quarter : len(values)-quarter]
	if len(trimmed) < 2 {
		trimmed = values
	}
	total := 0.0
	for i := 1; i < len(trimmed); i++ {
		total += math.Abs(trimmed[i] - trimmed[i-1])
	}
	jitter := math.Round(total / float64(len(trimmed)-1))
	if jitter == 0 {
		jitter = 1
	}
	if jitter >= median {
		jitter = values[0]
	}
	return jitter
}

func (p *Provider) Download(ctx context.Context, cfg provider.MeasurementConfig, progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	servers, err := p.measurementServers()
	if err != nil {
		code, retryable := measurementServerError(err)
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "download", code, retryable, err)
	}
	initial, maximum, err := connectionLimits(cfg, min(16, len(servers)*2))
	if err != nil {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "download", provider.CodeInternal, false, err)
	}
	if _, err := p.ensureJWT(ctx, false, ""); err != nil {
		code := errorCode(err)
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "download", code, retryableForCode(code), err)
	}
	serverAttempts := make([]atomic.Uint32, maximum)
	runResult, runErr := measure.Run(ctx, measure.RunConfig{
		Duration: cfg.Duration, Warmup: 2 * time.Second, StartupTimeout: 10 * time.Second,
		ReadyGrace: 500 * time.Millisecond, InitialWorkers: initial, MaxWorkers: maximum, Reconnects: 1,
		Adaptive: adaptiveConnections, OnWorkerError: verboseWorkerError(cfg, "download"),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		server := nextServerForWorker(servers, index, &serverAttempts[index])
		chunkMB := 25
		buf := make([]byte, 64<<10)
		for workerCtx.Err() == nil {
			started := time.Now()
			bytesRead, requestErr := p.downloadRequest(workerCtx, server, chunkMB, buf, ready, record)
			if requestErr != nil {
				return requestErr
			}
			chunkMB = nextDownloadChunkMB(bytesRead, time.Since(started))
		}
		return workerCtx.Err()
	}, adaptProgress(progress))
	result := convertRunResult(runResult)
	if runErr != nil {
		code := errorCode(runErr)
		return result, provider.NewError(p.Name(), "download", code, retryableForCode(code), runErr)
	}
	return result, nil
}

func (p *Provider) downloadRequest(ctx context.Context, server qmsServer, chunkMB int, buf []byte, ready func(), record func(int64)) (int64, error) {
	if chunkMB < 1 || chunkMB > 250 {
		return 0, fmt.Errorf("неверный размер блока скачивания: %d МБ", chunkMB)
	}
	if len(buf) == 0 {
		return 0, errors.New("буфер скачивания не может быть пустым")
	}
	expectedBytes := int64(chunkMB) * 1_000_000
	endpoint := server.httpURL() + "download.php?ckSize=" + strconv.Itoa(chunkMB) + "&r=" + strconv.FormatInt(time.Now().UnixNano(), 36)
	for authAttempt := 0; authAttempt < 2; authAttempt++ {
		token, err := p.ensureJWT(ctx, false, "")
		if err != nil {
			return 0, err
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
		if err != nil {
			return 0, err
		}
		req.Header.Set("jwt", token)
		req.Header.Set("User-Agent", userAgent)
		req.Header.Set("Accept-Encoding", "identity")
		resp, err := p.client.Do(req)
		if err != nil {
			return 0, err
		}
		if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
			drainAndClose(resp.Body)
			if authAttempt > 0 {
				return 0, fmt.Errorf("авторизация скачивания отклонена после обновления JWT: %s", resp.Status)
			}
			if _, err := p.ensureJWT(ctx, true, token); err != nil {
				return 0, err
			}
			continue
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			drainAndClose(resp.Body)
			return 0, fmt.Errorf("сервер %s вернул состояние %s при скачивании", server.Host, resp.Status)
		}
		if encoding := strings.TrimSpace(resp.Header.Get("Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
			drainAndClose(resp.Body)
			return 0, fmt.Errorf("сервер %s применил недопустимое сжатие %q", server.Host, encoding)
		}
		if resp.ContentLength >= 0 && resp.ContentLength != expectedBytes {
			drainAndClose(resp.Body)
			return 0, fmt.Errorf("Content-Length при скачивании равен %d, ожидалось %d", resp.ContentLength, expectedBytes)
		}
		ready()
		var total int64
		for {
			n, readErr := resp.Body.Read(buf)
			if n > 0 {
				if total+int64(n) > expectedBytes {
					_ = resp.Body.Close()
					return total, fmt.Errorf("при скачивании получено больше ожидаемых %d байт", expectedBytes)
				}
				total += int64(n)
				// The response status, encoding and declared size are validated
				// before ready. Count received bytes incrementally so a timed phase
				// does not discard the unfinished tail of a large native chunk.
				record(int64(n))
			}
			if readErr != nil {
				resp.Body.Close()
				if errors.Is(readErr, io.EOF) {
					if total != expectedBytes {
						return total, fmt.Errorf("при скачивании получено %d байт, ожидалось %d", total, expectedBytes)
					}
					return total, nil
				}
				if ctx.Err() != nil {
					return total, ctx.Err()
				}
				return total, readErr
			}
		}
	}
	return 0, errors.New("авторизация скачивания не удалась после обновления токена")
}

func (p *Provider) Upload(ctx context.Context, cfg provider.MeasurementConfig, progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	servers, err := p.measurementServers()
	if err != nil {
		code, retryable := measurementServerError(err)
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "upload", code, retryable, err)
	}
	initial, maximum, err := connectionLimits(cfg, min(16, len(servers)*2))
	if err != nil {
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "upload", provider.CodeInternal, false, err)
	}
	if _, err := p.ensureJWT(ctx, false, ""); err != nil {
		code := errorCode(err)
		return provider.ThroughputResult{}, provider.NewError(p.Name(), "upload", code, retryableForCode(code), err)
	}
	payload := make([]byte, uploadBlockSize)
	serverAttempts := make([]atomic.Uint32, maximum)
	runResult, runErr := measure.Run(ctx, measure.RunConfig{
		Duration: cfg.Duration, Warmup: 4 * time.Second, StartupTimeout: 12 * time.Second,
		ReadyGrace: 500 * time.Millisecond, InitialWorkers: initial, MaxWorkers: maximum, Reconnects: 1,
		Adaptive: adaptiveConnections, OnWorkerError: verboseWorkerError(cfg, "upload"),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		server := nextServerForWorker(servers, index, &serverAttempts[index])
		for workerCtx.Err() == nil {
			if err := p.uploadRequest(workerCtx, server, payload, ready); err != nil {
				return err
			}
			record(int64(len(payload)))
		}
		return workerCtx.Err()
	}, adaptProgress(progress))
	result := convertRunResult(runResult)
	if runErr != nil {
		code := errorCode(runErr)
		return result, provider.NewError(p.Name(), "upload", code, retryableForCode(code), runErr)
	}
	return result, nil
}

func (p *Provider) uploadRequest(ctx context.Context, server qmsServer, payload []byte, ready func()) error {
	if len(payload) != uploadBlockSize {
		return fmt.Errorf("неверный размер блока отдачи: %d байт, ожидалось %d", len(payload), uploadBlockSize)
	}
	endpoint := server.httpURL() + "upload.php?r=" + strconv.FormatInt(time.Now().UnixNano(), 36)
	for authAttempt := 0; authAttempt < 2; authAttempt++ {
		token, err := p.ensureJWT(ctx, false, "")
		if err != nil {
			return err
		}
		req, err := http.NewRequestWithContext(ctx, http.MethodPost, endpoint, bytes.NewReader(payload))
		if err != nil {
			return err
		}
		req.Header.Set("jwt", token)
		req.Header.Set("User-Agent", userAgent)
		req.Header.Set("Content-Type", "application/octet-stream")
		resp, err := p.client.Do(req)
		if err != nil {
			return err
		}
		responseBytes, copyErr := io.Copy(io.Discard, io.LimitReader(resp.Body, (1<<20)+1))
		closeErr := resp.Body.Close()
		if resp.StatusCode == http.StatusUnauthorized || resp.StatusCode == http.StatusForbidden {
			if authAttempt > 0 {
				return fmt.Errorf("авторизация отдачи отклонена после обновления JWT: %s", resp.Status)
			}
			if _, err := p.ensureJWT(ctx, true, token); err != nil {
				return err
			}
			continue
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return fmt.Errorf("сервер %s вернул состояние %s при отдаче", server.Host, resp.Status)
		}
		if copyErr != nil {
			return copyErr
		}
		if responseBytes > 1<<20 {
			return errors.New("ответ сервера при отдаче превышает безопасный предел")
		}
		if closeErr != nil {
			return closeErr
		}
		ready()
		return nil
	}
	return errors.New("авторизация отдачи не удалась после обновления токена")
}

func (p *Provider) measurementServers() ([]qmsServer, error) {
	p.mu.RLock()
	defer p.mu.RUnlock()
	if len(p.servers) == 0 {
		return nil, errors.New("сначала необходимо выбрать сервер")
	}
	if !p.throughput {
		return nil, errors.Join(errPingOnlyFallback, p.discoveryErr)
	}
	return append([]qmsServer(nil), p.servers...), nil
}

func measurementServerError(err error) (provider.ErrorCode, bool) {
	if errors.Is(err, errPingOnlyFallback) {
		return provider.CodeAuth, true
	}
	return provider.CodeInternal, false
}

func (p *Provider) ensureJWT(ctx context.Context, refresh bool, previous string) (string, error) {
	p.authMu.Lock()
	defer p.authMu.Unlock()
	p.mu.RLock()
	current := p.jwt
	key := p.browserKey
	p.mu.RUnlock()
	if current != "" && (!refresh || (previous != "" && current != previous)) {
		return current, nil
	}
	token, status, err := p.fetchJWT(ctx, key)
	if err != nil && (status == http.StatusUnauthorized || status == http.StatusForbidden) {
		refreshedKey, keyErr := p.extractBrowserKey(ctx)
		if keyErr == nil {
			key = refreshedKey
			p.logf("speedtest.ru · авторизация JWT обновила ключ браузерного клиента")
			token, _, err = p.fetchJWT(ctx, key)
		} else {
			err = errors.Join(err, keyErr)
		}
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if err != nil {
		p.jwt = ""
		return "", err
	}
	p.browserKey = key
	p.jwt = token
	return token, nil
}

func (p *Provider) fetchJWT(ctx context.Context, key string) (string, int, error) {
	requestCtx, cancel := context.WithTimeout(ctx, apiRequestTimeout)
	defer cancel()
	endpoint := p.apiBase + "/api/server/gentoken?t=" + strconv.FormatInt(time.Now().UnixMilli(), 10)
	req, err := http.NewRequestWithContext(requestCtx, http.MethodPost, endpoint, nil)
	if err != nil {
		return "", 0, err
	}
	req.Header.Set("x-api-key", key)
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")
	resp, err := p.client.Do(req)
	if err != nil {
		return "", 0, fmt.Errorf("запрос gentoken: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return "", resp.StatusCode, fmt.Errorf("gentoken вернул состояние %s", resp.Status)
	}
	var payload struct {
		Token string `json:"token"`
	}
	if err := decodeJSONLimited(resp.Body, maxJWTBytes, &payload); err != nil {
		return "", resp.StatusCode, fmt.Errorf("разбор ответа gentoken: %w", err)
	}
	parts := strings.Split(payload.Token, ".")
	if len(payload.Token) < 16 || len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
		return "", resp.StatusCode, errors.New("gentoken вернул пустой или неверный токен")
	}
	return payload.Token, resp.StatusCode, nil
}

func readLimited(reader io.Reader, limit int64) ([]byte, error) {
	if limit < 1 {
		return nil, errors.New("предел ответа должен быть положительным")
	}
	data, err := io.ReadAll(io.LimitReader(reader, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("ответ превышает безопасный предел %d байт", limit)
	}
	return data, nil
}

func decodeJSONLimited(reader io.Reader, limit int64, destination any) error {
	data, err := readLimited(reader, limit)
	if err != nil {
		return err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	return ensureJSONEOF(decoder)
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

func sameOrigin(left, right *url.URL) bool {
	if left == nil || right == nil {
		return false
	}
	return strings.EqualFold(left.Scheme, right.Scheme) && strings.EqualFold(left.Host, right.Host)
}

func drainAndClose(body io.ReadCloser) {
	if body == nil {
		return
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(body, 64<<10))
	_ = body.Close()
}

func nextDownloadChunkMB(bytesRead int64, elapsed time.Duration) int {
	if bytesRead <= 0 || elapsed <= 0 {
		return 25
	}
	target := float64(bytesRead) / elapsed.Seconds() * 6 / 1e6
	if math.IsNaN(target) || target <= 25 {
		return 25
	}
	if math.IsInf(target, 1) || target >= 250 {
		return 250
	}
	return int(math.Round(target))
}

func nextServerForWorker(servers []qmsServer, workerIndex int, attempts *atomic.Uint32) qmsServer {
	attempt := int(attempts.Add(1) - 1)
	return servers[(workerIndex+attempt)%len(servers)]
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
		initial = max(1, min(native, maximum))
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
		cfg.Verbose("speedtest.ru · %s: поток=%d, попытка=%d, переподключение=%s: %v", phaseDisplayName(phase), index+1, attempt+1, reconnect, err)
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
		if err != nil {
			if _, ok := seen[err.Error()]; !ok {
				seen[err.Error()] = struct{}{}
				warnings = append(warnings, err.Error())
			}
		}
	}
	return provider.ThroughputResult{
		Mbps: measure.Mbps(result.Bytes, result.Elapsed), Bytes: result.Bytes, Elapsed: result.Elapsed,
		SuccessfulConnections: result.WorkersOK, FailedConnections: result.WorkersFailed, Warnings: warnings,
	}
}

func errorCode(err error) provider.ErrorCode {
	if errors.Is(err, context.Canceled) {
		return provider.CodeCanceled
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return provider.CodeTimeout
	}
	message := strings.ToLower(err.Error())
	if strings.Contains(message, "неожидан") || strings.Contains(message, "неверн") ||
		strings.Contains(message, "пустой ответ") || strings.Contains(message, "content-length") ||
		strings.Contains(message, "ожидалось") || strings.Contains(message, "недопустимое сжатие") ||
		strings.Contains(message, "больше ожидаемых") || strings.Contains(message, "несколько значений json") ||
		strings.Contains(message, "разбор ответа") {
		return provider.CodeProtocol
	}
	if strings.Contains(message, "авторизац") || strings.Contains(message, "jwt") || strings.Contains(message, "gentoken") || strings.Contains(message, "unauthorized") || strings.Contains(message, "forbidden") {
		return provider.CodeAuth
	}
	return provider.CodeUnavailable
}

func retryableForCode(code provider.ErrorCode) bool {
	return code == provider.CodeUnavailable || code == provider.CodeTimeout || code == provider.CodeAuth
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
