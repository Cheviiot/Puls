package speedtestru

import (
	"context"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) SelectServer(ctx context.Context) (service.Server, error) {
	if err := ctx.Err(); err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.ClassifyError(err), false, err)
	}
	if p.host != "" {
		host, err := normalizeHost(p.host, "20000")
		if err != nil {
			return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeInternal, false, err)
		}
		server := qmsServer{Host: host, Name: host}
		p.mu.Lock()
		p.selected = server
		p.servers = []qmsServer{server}
		p.throughput = true
		p.discoveryErr = nil
		p.mu.Unlock()
		return service.Server{Name: host}, nil
	}

	servers, err := p.discover(ctx)
	if err == nil && len(servers) > 0 {
		responsive := p.selectResponsive(ctx, servers)
		if len(responsive) > 0 {

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
			return service.Server{Name: s.Host, City: s.City, Region: s.Region}, nil
		}
		err = errors.New("ни один найденный сервер QMS не завершил согласование WebSocket")
	}
	p.logf("speedtest.ru · поиск серверов недоступен, используется встроенный список только для задержки: %v", err)
	if contextErr := ctx.Err(); contextErr != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.ClassifyError(contextErr), false, contextErr)
	}

	fallback, fallbackErr := autoSelect(ctx)
	if fallbackErr != nil {
		if err != nil {
			fallbackErr = errors.Join(err, fallbackErr)
		}
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeUnavailable, true,
			fmt.Errorf("не удалось автоматически выбрать сервер (%w); укажите адрес через --server", fallbackErr))
	}
	server := fallback
	p.mu.Lock()
	p.selected = server
	p.servers = []qmsServer{server}
	p.throughput = false
	p.discoveryErr = err
	p.mu.Unlock()
	return service.Server{Name: server.Host, City: server.City}, nil
}

func (p *Backend) discover(ctx context.Context) ([]qmsServer, error) {
	return withBrowserKey(ctx, p, p.fetchNearest)
}

func (p *Backend) fetchNearest(ctx context.Context, key string) ([]qmsServer, int, error) {
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
	if err := service.ValidateContentType(resp.Header, "application/json"); err != nil {
		return nil, resp.StatusCode, service.ProtocolError(fmt.Errorf("nearest_servers: %w", err))
	}
	var payload nearestResponse
	if err := service.DecodeJSONLimited(resp.Body, maxDiscoveryBytes, &payload); err != nil {
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
			Region: item.RegionName,
		})
	}
	if len(servers) == 0 {
		return nil, resp.StatusCode, errors.New("nearest_servers не вернул корректных адресов")
	}
	return servers, resp.StatusCode, nil
}

func (p *Backend) extractBrowserKey(ctx context.Context) (string, error) {
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
	if err := service.ValidateContentType(resp.Header, "text/html", "application/xhtml+xml"); err != nil {
		return "", service.ProtocolError(fmt.Errorf("страница speedtest.ru: %w", err))
	}
	html, err := service.ReadLimited(resp.Body, maxPageBytes)
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
		body, readErr := service.ReadLimited(assetResp.Body, maxPageBytes)
		finalURL := assetResp.Request.URL
		contentTypeErr := service.ValidateContentType(assetResp.Header, "application/javascript", "text/javascript", "text/plain")
		assetResp.Body.Close()
		if readErr != nil || contentTypeErr != nil || assetResp.StatusCode < 200 || assetResp.StatusCode >= 300 || !sameOrigin(base, finalURL) {
			continue
		}
		if keyMatch := keyPattern.FindSubmatch(body); len(keyMatch) == 2 {
			return string(keyMatch[1]), nil
		}
	}
	return "", errors.New("в текущем JS-файле страницы не найден ключ браузерного клиента QMS")
}

func (p *Backend) currentBrowserKey() string {
	p.mu.RLock()
	defer p.mu.RUnlock()
	return p.browserKey
}

func (p *Backend) selectResponsive(ctx context.Context, servers []qmsServer) []qmsServer {
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
	p := New(Options{})
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

func sameOrigin(left, right *url.URL) bool {
	if left == nil || right == nil {
		return false
	}
	return strings.EqualFold(left.Scheme, right.Scheme) && strings.EqualFold(left.Host, right.Host)
}
