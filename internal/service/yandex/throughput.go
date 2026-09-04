package yandex

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/measure"
	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) Download(ctx context.Context, cfg service.MeasurementConfig, progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	p.mu.RLock()
	urls := append([]string(nil), p.downloadURLs...)
	p.mu.RUnlock()
	if len(urls) == 0 {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseDownload, service.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}
	initial, maximum, err := service.ConnectionLimits(cfg, len(urls))
	if err != nil {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseDownload, service.CodeInternal, false, err)
	}

	runResult, err := measure.Run(ctx, measure.RunConfig{
		Duration:       cfg.Duration,
		Warmup:         750 * time.Millisecond,
		StartupTimeout: 8 * time.Second,
		ReadyGrace:     400 * time.Millisecond,
		InitialWorkers: initial,
		MaxWorkers:     maximum,
		Reconnects:     1,
		Adaptive:       service.AdaptiveConnections,
		OnWorkerError:  service.WorkerErrorLogger(p.log, p.ID(), service.PhaseDownload),
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
	}, service.AdaptProgress(progress))
	result := service.ConvertRunResult(runResult)
	if err != nil {
		code := service.ClassifyError(err)
		return result, service.NewError(p.ID(), service.PhaseDownload, code, code != service.CodeCanceled, err)
	}
	return result, nil
}

func (p *Backend) Upload(ctx context.Context, cfg service.MeasurementConfig, progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	p.mu.RLock()
	probes := append([]uploadProbe(nil), p.uploadProbes...)
	p.mu.RUnlock()
	if len(probes) == 0 {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseUpload, service.CodeInternal, false, errors.New("сначала необходимо выбрать сервер"))
	}
	initial, maximum, err := service.ConnectionLimits(cfg, len(probes))
	if err != nil {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseUpload, service.CodeInternal, false, err)
	}

	payload := make([]byte, uploadChunkSize)
	runResult, err := measure.Run(ctx, measure.RunConfig{
		Duration:       cfg.Duration,
		Warmup:         750 * time.Millisecond,
		StartupTimeout: 10 * time.Second,
		ReadyGrace:     400 * time.Millisecond,
		InitialWorkers: initial,
		MaxWorkers:     maximum,
		Reconnects:     1,
		Adaptive:       service.AdaptiveConnections,
		OnWorkerError:  service.WorkerErrorLogger(p.log, p.ID(), service.PhaseUpload),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		probe := probes[index%len(probes)]
		return p.uploadWorker(workerCtx, probe, payload, cfg, ready, record)
	}, service.AdaptProgress(progress))
	result := service.ConvertRunResult(runResult)
	if err != nil {
		code := service.ClassifyError(err)
		return result, service.NewError(p.ID(), service.PhaseUpload, code, code != service.CodeCanceled, err)
	}
	return result, nil
}

func (p *Backend) downloadProbe(ctx context.Context, probeURL string, buf []byte, ready func(), record func(int64)) (int64, error) {
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
		return 0, &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "проба скачивания"}
	}
	expectedBytes := resp.ContentLength
	if expectedBytes <= 0 {
		return 0, service.ProtocolError(errors.New("проба скачивания не содержит положительный Content-Length"))
	}
	if expectedBytes > maxDownloadBodySize {
		return 0, service.ProtocolError(fmt.Errorf("Content-Length пробы скачивания (%d) превышает безопасный предел", expectedBytes))
	}
	if isLargeDownloadProbe(probeURL) && expectedBytes != expectedDownloadSize {
		return 0, service.ProtocolError(fmt.Errorf("неожиданный размер пробы скачивания: %d байт вместо %d", expectedBytes, expectedDownloadSize))
	}
	if encoding := strings.TrimSpace(resp.Header.Get("Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
		return 0, service.ProtocolError(fmt.Errorf("проба скачивания использует неожиданное Content-Encoding %q", encoding))
	}
	if err := service.ValidateContentType(resp.Header, "application/octet-stream"); err != nil {
		return 0, service.ProtocolError(fmt.Errorf("проба скачивания: %w", err))
	}
	ready()
	var readBytes int64
	for {
		n, readErr := resp.Body.Read(buf)
		if n > 0 {
			if readBytes+int64(n) > expectedBytes {
				return readBytes, service.ProtocolError(fmt.Errorf("проба скачивания превысила ожидаемый размер %d байт", expectedBytes))
			}
			readBytes += int64(n)

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
		return 0, service.ProtocolError(errors.New("проба скачивания вернула пустой ответ"))
	}
	if expectedBytes >= 0 && readBytes != expectedBytes {
		return readBytes, service.ProtocolError(fmt.Errorf("проба скачивания вернула %d байт, ожидалось %d", readBytes, expectedBytes))
	}
	return readBytes, nil
}

func (p *Backend) uploadWorker(ctx context.Context, probe uploadProbe, payload []byte, cfg service.MeasurementConfig, ready func(), record func(int64)) error {
	if probe.websocketURL != "" {
		if wsErr := p.websocketUploadWithTimeout(ctx, probe.websocketURL, probe.websocketConnectionTimeout, cfg.Duration, ready, record); wsErr == nil || ctx.Err() != nil {
			return wsErr
		} else if p.log != nil {
			p.log("Яндекс · отдача: WebSocket недоступен, переход на резервный HTTP-запрос: %v", wsErr)
		}
	}
	return p.httpUpload(ctx, probe.postURL, payload, ready, record)
}
