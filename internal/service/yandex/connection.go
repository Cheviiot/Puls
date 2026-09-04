package yandex

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/netip"

	"github.com/Cheviiot/Puls/internal/service"
)

// DetectConnection reports the visitor's public IP address exactly as the
// official Яндекс.Интернетометр page (https://yandex.ru/internet/) detects
// and embeds it in its own bootstrap state for display. It performs a
// single GET of that page and does not touch get-probes or any measurement
// state, so it can be called independently of SelectServer.
func (p *Backend) DetectConnection(ctx context.Context) (service.ConnectionInfo, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.internetPageURL, nil)
	if err != nil {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeInternal, false, fmt.Errorf("запрос страницы интернетометра: %w", err))
	}
	req.Header.Set("User-Agent", userAgent)
	req.Header.Set("Accept", "text/html")

	resp, err := p.client.Do(req)
	if err != nil {
		code := service.ClassifyError(err)
		retryable := service.RetryableCode(code)
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, code, retryable, fmt.Errorf("запрос страницы интернетометра: %w", err))
	}
	defer resp.Body.Close()
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		code, retryable := service.ClassifyHTTPStatus(resp.StatusCode)
		statusErr := &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "страница интернетометра"}
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, code, retryable, statusErr)
	}
	if err := service.ValidateContentType(resp.Header, "text/html", "application/xhtml+xml"); err != nil {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false, fmt.Errorf("страница интернетометра: %w", err))
	}

	limited := &io.LimitedReader{R: resp.Body, N: maxInternetPageSize + 1}
	body, err := io.ReadAll(limited)
	if err != nil {
		code := service.ClassifyError(err)
		retryable := service.RetryableCode(code)
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, code, retryable, fmt.Errorf("чтение страницы интернетометра: %w", err))
	}
	if limited.N <= 0 {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false, errors.New("страница интернетометра превышает безопасный предел"))
	}

	object, err := extractBalancedJSONObject(body, clientStateMarker)
	if err != nil {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false, fmt.Errorf("поиск состояния клиента на странице интернетометра: %w", err))
	}
	var state struct {
		IP struct {
			V4 string `json:"v4"`
			V6 string `json:"v6"`
		} `json:"ip"`
	}
	if err := json.Unmarshal(object, &state); err != nil {
		return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false, fmt.Errorf("разбор состояния клиента интернетометра: %w", err))
	}
	for _, candidate := range []string{state.IP.V4, state.IP.V6} {
		if ip, parseErr := netip.ParseAddr(candidate); parseErr == nil && ip.Zone() == "" {
			return service.ConnectionInfo{ExternalIP: ip.Unmap()}, nil
		}
	}
	return service.ConnectionInfo{}, service.NewError(p.ID(), service.PhaseConnection, service.CodeProtocol, false, errors.New("страница интернетометра не сообщила действительный IP-адрес"))
}

// extractBalancedJSONObject returns the first "{...}" object literal that
// follows marker in body, matching braces while respecting quoted strings
// so nested objects and escaped quotes don't break the scan.
func extractBalancedJSONObject(body []byte, marker string) ([]byte, error) {
	idx := bytes.Index(body, []byte(marker))
	if idx < 0 {
		return nil, fmt.Errorf("маркер %q не найден", marker)
	}
	start := idx + len(marker)
	for start < len(body) && isJSONSpace(body[start]) {
		start++
	}
	if start >= len(body) || body[start] != '{' {
		return nil, errors.New("после маркера нет JSON-объекта")
	}

	depth := 0
	inString, escaped := false, false
	for i := start; i < len(body); i++ {
		c := body[i]
		if inString {
			switch {
			case escaped:
				escaped = false
			case c == '\\':
				escaped = true
			case c == '"':
				inString = false
			}
			continue
		}
		switch c {
		case '"':
			inString = true
		case '{':
			depth++
		case '}':
			depth--
			if depth == 0 {
				return body[start : i+1], nil
			}
		}
	}
	return nil, errors.New("не удалось найти конец JSON-объекта")
}

func isJSONSpace(c byte) bool {
	return c == ' ' || c == '\t' || c == '\n' || c == '\r'
}
