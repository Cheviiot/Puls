package yandex

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

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

func (p *Backend) SelectServer(ctx context.Context) (service.Server, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, cacheBust(p.probesURL), nil)
	if err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeInternal, false, err)
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")

	resp, err := p.client.Do(req)
	if err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeUnavailable, true, fmt.Errorf("запрос get-probes: %w", err))
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		code := service.CodeProtocol
		retryable := resp.StatusCode == http.StatusRequestTimeout || resp.StatusCode == http.StatusTooManyRequests || resp.StatusCode >= 500
		if resp.StatusCode >= 500 {
			code = service.CodeUnavailable
		}
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, code, retryable, fmt.Errorf("get-probes вернул состояние %s", resp.Status))
	}
	if err := service.ValidateContentType(resp.Header, "application/json"); err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true,
			fmt.Errorf("get-probes: %w", err))
	}

	var pr probesResponse
	if err := service.DecodeJSONLimited(resp.Body, maxDiscoveryBodySize, &pr); err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true, fmt.Errorf("разбор ответа get-probes: %w", err))
	}
	if strings.TrimSpace(pr.MID) == "" {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true, errors.New("get-probes вернул пустой идентификатор mid"))
	}

	latencyURLs := make([]string, 0, len(pr.Latency.Probes))
	latencyHosts := make(map[string]struct{}, len(pr.Latency.Probes))
	for index, probe := range pr.Latency.Probes {
		if !validHTTPProbeURL(probe.URL) {
			return service.Server{}, p.discoverySchemaError("неверный URL latency-пробы %d", index+1)
		}
		host := endpointHost(probe.URL)
		if _, exists := latencyHosts[host]; exists {
			return service.Server{}, p.discoverySchemaError("повторяющийся CDN latency-пробы %d", index+1)
		}
		latencyHosts[host] = struct{}{}
		latencyURLs = append(latencyURLs, probe.URL)
	}
	downloadURLs := make([]string, 0, len(pr.Download.Probes))
	downloadHosts := make(map[string]struct{}, len(pr.Download.Probes))
	for index, probe := range pr.Download.Probes {
		if !validHTTPProbeURL(probe.URL) {
			return service.Server{}, p.discoverySchemaError("неверный URL download-пробы %d", index+1)
		}
		if probe.Timeout != 0 {
			continue
		}
		if !isLargeDownloadProbe(probe.URL) {
			return service.Server{}, p.discoverySchemaError("неожиданный URL основной download-пробы %d", index+1)
		}
		host := endpointHost(probe.URL)
		if _, exists := downloadHosts[host]; exists {
			return service.Server{}, p.discoverySchemaError("повторяющийся CDN download-пробы %d", index+1)
		}
		downloadHosts[host] = struct{}{}
		downloadURLs = append(downloadURLs, probe.URL)
	}
	uploadProbes := make([]uploadProbe, 0, len(pr.Upload.Probes))
	uploadHosts := make(map[string]struct{}, len(pr.Upload.Probes))
	for index, probe := range pr.Upload.Probes {
		if probe.Size <= 0 || !validHTTPProbeURL(probe.URL) || !validHTTPProbeURL(probe.PostURL) || !validHTTPProbeURL(probe.StatsURL) {
			return service.Server{}, p.discoverySchemaError("неверная upload-проба %d", index+1)
		}
		probeHost := endpointHost(probe.URL)
		if endpointHost(probe.PostURL) != probeHost || endpointHost(probe.StatsURL) != probeHost {
			return service.Server{}, p.discoverySchemaError("endpoint upload-пробы %d относятся к разным CDN", index+1)
		}
		if probe.WebsocketURL != "" {
			if !validWebSocketProbeURL(probe.WebsocketURL) || endpointHost(probe.WebsocketURL) != probeHost {
				return service.Server{}, p.discoverySchemaError("неверный WebSocket URL upload-пробы %d", index+1)
			}
			if probe.WebsocketConnectionTimeout <= 0 || probe.WebsocketConnectionTimeout > 30_000 {
				return service.Server{}, p.discoverySchemaError("неверный WebSocket timeout upload-пробы %d", index+1)
			}
		}
		if probe.Timeout != 0 {
			continue
		}
		if _, exists := uploadHosts[probeHost]; exists {
			return service.Server{}, p.discoverySchemaError("повторяющийся CDN upload-пробы %d", index+1)
		}
		uploadHosts[probeHost] = struct{}{}
		uploadProbes = append(uploadProbes, uploadProbe{
			postURL:                    probe.PostURL,
			websocketURL:               probe.WebsocketURL,
			websocketConnectionTimeout: time.Duration(probe.WebsocketConnectionTimeout) * time.Millisecond,
		})
	}
	if len(latencyURLs) == 0 || len(downloadURLs) == 0 || len(uploadProbes) == 0 {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true,
			fmt.Errorf("неполный набор проб (задержка=%d, скачивание=%d, отдача=%d)", len(latencyURLs), len(downloadURLs), len(uploadProbes)))
	}
	if err := validateCDNSets(latencyHosts, downloadHosts, uploadHosts); err != nil {
		return service.Server{}, service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true, err)
	}

	p.mu.Lock()
	p.latencyURLs = latencyURLs
	p.downloadURLs = downloadURLs
	p.uploadProbes = uploadProbes
	p.mu.Unlock()

	return service.Server{Name: hostOf(downloadURLs[0])}, nil
}

func (p *Backend) discoverySchemaError(format string, args ...any) error {
	return service.NewError(p.ID(), service.PhaseSelect, service.CodeProtocol, true, fmt.Errorf(format, args...))
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
