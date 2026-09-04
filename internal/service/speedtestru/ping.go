package speedtestru

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/gorilla/websocket"

	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) probePing(ctx context.Context, server qmsServer, sampleCount int) (service.PingResult, error) {
	conn, err := p.dial(ctx, server.wsURL())
	if err != nil {
		return service.PingResult{}, err
	}
	defer conn.Close()
	stop := interruptOnDone(ctx, conn)
	defer stop()
	if err := handshake(conn); err != nil {
		return service.PingResult{}, err
	}
	samples := make([]float64, 0, sampleCount)
	for range sampleCount {
		duration, pingErr := pingOnce(conn)
		if pingErr != nil {
			return service.PingResult{}, pingErr
		}
		samples = append(samples, duration.Seconds()*1000)
	}
	result := service.StatsWithMethod(samples, "median")
	result.JitterMs = qmsJitter(samples, result.MedianMs)
	return result, nil
}

func (p *Backend) dial(ctx context.Context, wsURL string) (*websocket.Conn, error) {
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

func (p *Backend) Ping(ctx context.Context) (service.PingResult, error) {
	if err := ctx.Err(); err != nil {
		return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, service.CodeCanceled, false, err)
	}
	p.mu.RLock()
	server := p.selected
	servers := append([]qmsServer(nil), p.servers...)
	p.mu.RUnlock()
	if server.Host == "" {
		return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, service.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
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
	code := service.ClassifyError(err)
	return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, code, service.RetryableCode(code), err)
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
		return 0, service.ProtocolError(fmt.Errorf("неожиданный тип ответа PONG: %d", messageType))
	}
	fields := strings.Fields(string(message))
	if len(fields) != 2 || fields[0] != "PONG" {
		return 0, service.ProtocolError(fmt.Errorf("неожиданный ответ на проверку задержки: %q", message))
	}
	stamp, err := strconv.ParseInt(fields[1], 10, 64)
	if err != nil || stamp < 0 {
		return 0, service.ProtocolError(fmt.Errorf("неверная временная метка PONG: %q", fields[1]))
	}
	return time.Since(start), nil
}

func expectExact(conn *websocket.Conn, expected string) error {
	messageType, message, err := conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("получение %s: %w", expected, err)
	}
	if messageType != websocket.TextMessage || strings.TrimSpace(string(message)) != expected {
		return service.ProtocolError(fmt.Errorf("неожиданный ответ %q, ожидался %q", message, expected))
	}
	return nil
}

func expectServerInfo(conn *websocket.Conn) error {
	messageType, message, err := conn.ReadMessage()
	if err != nil {
		return fmt.Errorf("получение qms_testing: %w", err)
	}
	if messageType != websocket.TextMessage {
		return service.ProtocolError(fmt.Errorf("неожиданный тип кадра со сведениями о сервере: %d", messageType))
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
	return service.ProtocolError(fmt.Errorf("неожиданный ответ со сведениями о сервере: %q", message))
}

func qmsJitter(samples []float64, median float64) float64 {

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
