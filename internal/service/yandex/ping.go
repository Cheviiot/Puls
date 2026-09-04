package yandex

import (
	"context"
	"errors"
	"io"
	"math"
	"net/http"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) Ping(ctx context.Context) (service.PingResult, error) {
	p.mu.RLock()
	urls := append([]string(nil), p.latencyURLs...)
	p.mu.RUnlock()
	if len(urls) == 0 {
		return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, service.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}

	const samplesPerURL = 4
	type latencyResult struct {
		index   int
		samples []float64
		err     error
	}
	results := make(chan latencyResult, len(urls))
	for index, probeURL := range urls {
		go func(index int, probeURL string) {
			values := make([]float64, 0, samplesPerURL)
			failures := make([]error, 0, samplesPerURL)
			for range samplesPerURL {
				probeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
				duration, err := p.pingOnce(probeCtx, probeURL)
				cancel()
				if err != nil {
					failures = append(failures, err)
					continue
				}
				values = append(values, duration.Seconds()*1000)
			}
			results <- latencyResult{index: index, samples: values, err: errors.Join(failures...)}
		}(index, probeURL)
	}
	ordered := make([]latencyResult, len(urls))
	for range urls {
		select {
		case result := <-results:
			ordered[result.index] = result
		case <-ctx.Done():
			return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, service.CodeCanceled, false, ctx.Err())
		}
	}
	samples := make([]float64, 0, len(urls)*samplesPerURL)
	var winningCDN []float64
	winningLatency := math.MaxFloat64
	var failures []error
	for _, probe := range ordered {
		values := probe.samples
		if probe.err != nil {
			failures = append(failures, probe.err)
		}
		samples = append(samples, values...)
		for _, value := range values {
			if value < winningLatency {
				winningLatency = value
				winningCDN = values
			}
		}
	}
	if len(samples) == 0 {
		cause := errors.Join(failures...)
		if cause == nil {
			cause = errors.New("все запросы задержки завершились с ошибкой")
		}
		code := service.ClassifyError(cause)
		return service.PingResult{}, service.NewError(p.ID(), service.PhasePing, code, service.RetryableCode(code), cause)
	}
	result := service.StatsWithMethod(samples, "minimum")

	if len(winningCDN) > 2 {
		result.JitterMs = service.MedianAbsoluteDeviation(winningCDN[1:])
	} else {
		result.JitterMs = 0
	}
	return result, nil
}

func (p *Backend) pingOnce(ctx context.Context, probeURL string) (time.Duration, error) {
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
		return 0, &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "запрос задержки"}
	}
	responseBytes, readErr := io.Copy(io.Discard, io.LimitReader(resp.Body, maxPingBodySize+1))
	if readErr != nil {
		return 0, readErr
	}
	if responseBytes > maxPingBodySize {
		return 0, service.ProtocolError(errors.New("ответ на запрос задержки превышает безопасный предел"))
	}
	return latency, nil
}
