package speedtestru

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/netip"
	"strconv"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

// DetectConnection reads the public IP and Internet provider from the same
// first-party JSON endpoints used by the speedtest.ru browser client. The IP
// is required; ISP metadata is best effort and never invalidates a valid IP.
func (p *Backend) DetectConnection(ctx context.Context) (service.ConnectionInfo, error) {
	ipPayload, err := withBrowserKey(ctx, p, p.fetchExternalIP)
	if err != nil {
		code := service.ClassifyError(err)
		retryable := service.RetryableCode(code)
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, code, retryable, err)
	}
	ip, err := netip.ParseAddr(strings.TrimSpace(ipPayload.IP))
	if err != nil || ip.Zone() != "" {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false,
			fmt.Errorf("speedtest.ru вернул неверный IP-адрес %q", ipPayload.IP))
	}
	info := service.ConnectionInfo{ExternalIP: ip.Unmap()}

	asnPayload, asnErr := withBrowserKey(ctx, p, p.fetchASN)
	if asnErr != nil {
		info.Warnings = append(info.Warnings, fmt.Sprintf("интернет-провайдер недоступен: %v", asnErr))
		return info, nil
	}
	if asnIP, parseErr := netip.ParseAddr(strings.TrimSpace(asnPayload.IP)); parseErr != nil || asnIP.Zone() != "" || asnIP.Unmap() != info.ExternalIP {
		info.Warnings = append(info.Warnings, "speedtest.ru вернул несовпадающий IP при определении интернет-провайдера")
		return info, nil
	}
	isp := strings.TrimSpace(asnPayload.ProviderName)
	if isp == "" || len(isp) > maxISPNameBytes || strings.ContainsAny(isp, "\r\n\x00") {
		info.Warnings = append(info.Warnings, "speedtest.ru не сообщил корректное название интернет-провайдера")
		return info, nil
	}
	info.ISP = isp
	return info, nil
}

type externalIPResponse struct {
	IP string `json:"ip"`
}

type asnResponse struct {
	IP           string `json:"ip"`
	ProviderName string `json:"provider_name"`
}

func (p *Backend) fetchExternalIP(ctx context.Context, key string) (externalIPResponse, int, error) {
	var payload externalIPResponse
	status, err := p.fetchAPIJSON(ctx, "/api/asn_provider/ip", key, &payload)
	return payload, status, err
}

func (p *Backend) fetchASN(ctx context.Context, key string) (asnResponse, int, error) {
	var payload asnResponse
	status, err := p.fetchAPIJSON(ctx, "/api/asn_provider/asn", key, &payload)
	return payload, status, err
}

func (p *Backend) fetchAPIJSON(ctx context.Context, path, key string, destination any) (int, error) {
	requestCtx, cancel := context.WithTimeout(ctx, apiRequestTimeout)
	defer cancel()
	endpoint := p.apiBase + path + "?t=" + strconv.FormatInt(time.Now().UnixMilli(), 10)
	req, err := http.NewRequestWithContext(requestCtx, http.MethodGet, endpoint, nil)
	if err != nil {
		return 0, err
	}
	req.Header.Set("x-api-key", key)
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "application/json")
	resp, err := p.client.Do(req)
	if err != nil {
		return 0, fmt.Errorf("запрос %s: %w", path, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return resp.StatusCode, &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: path}
	}
	if err := service.ValidateContentType(resp.Header, "application/json"); err != nil {
		return resp.StatusCode, service.ProtocolError(fmt.Errorf("%s: %w", path, err))
	}
	if err := service.DecodeJSONLimited(resp.Body, maxConnectionBytes, destination); err != nil {
		return resp.StatusCode, service.ProtocolError(fmt.Errorf("разбор ответа %s: %w", path, err))
	}
	return resp.StatusCode, nil
}

func withBrowserKey[T any](ctx context.Context, p *Backend, fetch func(context.Context, string) (T, int, error)) (T, error) {
	key := p.currentBrowserKey()
	value, status, err := fetch(ctx, key)
	if err == nil {
		return value, nil
	}
	if status != http.StatusUnauthorized && status != http.StatusForbidden {
		return value, err
	}
	refreshed, refreshErr := p.refreshBrowserKey(ctx, key)
	if refreshErr != nil {
		return value, errors.Join(err, refreshErr)
	}
	value, _, err = fetch(ctx, refreshed)
	return value, err
}

func (p *Backend) refreshBrowserKey(ctx context.Context, previous string) (string, error) {
	p.keyMu.Lock()
	defer p.keyMu.Unlock()
	if current := p.currentBrowserKey(); current != "" && current != previous {
		return current, nil
	}
	refreshed, err := p.extractBrowserKey(ctx)
	if err != nil {
		return "", err
	}
	p.mu.Lock()
	p.browserKey = refreshed
	p.jwt = ""
	p.mu.Unlock()
	p.logf("speedtest.ru · ключ браузерного клиента обновлён из текущего JS-файла сервиса")
	return refreshed, nil
}
