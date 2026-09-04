package speedtestru

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) ensureJWT(ctx context.Context, refresh bool, previous string) (string, error) {
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

func (p *Backend) fetchJWT(ctx context.Context, key string) (string, int, error) {
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
		return "", resp.StatusCode, &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "gentoken"}
	}
	if err := service.ValidateContentType(resp.Header, "application/json"); err != nil {
		return "", resp.StatusCode, service.ProtocolError(fmt.Errorf("gentoken: %w", err))
	}
	var payload struct {
		Token string `json:"token"`
	}
	if err := service.DecodeJSONLimited(resp.Body, maxJWTBytes, &payload); err != nil {
		return "", resp.StatusCode, service.ProtocolError(fmt.Errorf("разбор ответа gentoken: %w", err))
	}
	parts := strings.Split(payload.Token, ".")
	if len(payload.Token) < 16 || len(parts) != 3 || parts[0] == "" || parts[1] == "" || parts[2] == "" {
		return "", resp.StatusCode, service.AuthorizationError(errors.New("gentoken вернул пустой или неверный токен"))
	}
	return payload.Token, resp.StatusCode, nil
}
