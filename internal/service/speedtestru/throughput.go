package speedtestru

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"github.com/Cheviiot/Puls/internal/measure"
	"github.com/Cheviiot/Puls/internal/service"
)

func (p *Backend) Download(ctx context.Context, cfg service.MeasurementConfig, progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	servers, err := p.measurementServers()
	if err != nil {
		code, retryable := measurementServerError(err)
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseDownload, code, retryable, err)
	}
	initial, maximum, err := service.ConnectionLimits(cfg, min(16, len(servers)*2))
	if err != nil {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseDownload, service.CodeInternal, false, err)
	}
	if _, err := p.ensureJWT(ctx, false, ""); err != nil {
		code := service.ClassifyError(err)
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseDownload, code, service.RetryableCode(code), err)
	}
	serverAttempts := make([]atomic.Uint32, maximum)
	runResult, runErr := measure.Run(ctx, measure.RunConfig{
		Duration: cfg.Duration, Warmup: 2 * time.Second, StartupTimeout: 10 * time.Second,
		ReadyGrace: 500 * time.Millisecond, InitialWorkers: initial, MaxWorkers: maximum, Reconnects: 1,
		Adaptive: service.AdaptiveConnections, OnWorkerError: service.WorkerErrorLogger(p.log, p.ID(), service.PhaseDownload),
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
	}, service.AdaptProgress(progress))
	result := service.ConvertRunResult(runResult)
	if runErr != nil {
		code := service.ClassifyError(runErr)
		return result, service.NewError(p.ID(), service.PhaseDownload, code, service.RetryableCode(code), runErr)
	}
	return result, nil
}

func (p *Backend) downloadRequest(ctx context.Context, server qmsServer, chunkMB int, buf []byte, ready func(), record func(int64)) (int64, error) {
	if chunkMB < 1 || chunkMB > 250 {
		return 0, service.ProtocolError(fmt.Errorf("неверный размер блока скачивания: %d МБ", chunkMB))
	}
	if len(buf) == 0 {
		return 0, service.ProtocolError(errors.New("буфер скачивания не может быть пустым"))
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
			service.DrainAndClose(resp.Body)
			if authAttempt > 0 {
				return 0, service.AuthorizationError(fmt.Errorf("авторизация скачивания отклонена после обновления JWT: %s", resp.Status))
			}
			if _, err := p.ensureJWT(ctx, true, token); err != nil {
				return 0, err
			}
			continue
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			service.DrainAndClose(resp.Body)
			return 0, &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "сервер " + server.Host + " при скачивании"}
		}
		if encoding := strings.TrimSpace(resp.Header.Get("Content-Encoding")); encoding != "" && !strings.EqualFold(encoding, "identity") {
			service.DrainAndClose(resp.Body)
			return 0, service.ProtocolError(fmt.Errorf("сервер %s применил недопустимое сжатие %q", server.Host, encoding))
		}
		if resp.ContentLength >= 0 && resp.ContentLength != expectedBytes {
			service.DrainAndClose(resp.Body)
			return 0, service.ProtocolError(fmt.Errorf("Content-Length при скачивании равен %d, ожидалось %d", resp.ContentLength, expectedBytes))
		}
		ready()
		var total int64
		for {
			n, readErr := resp.Body.Read(buf)
			if n > 0 {
				if total+int64(n) > expectedBytes {
					_ = resp.Body.Close()
					return total, service.ProtocolError(fmt.Errorf("при скачивании получено больше ожидаемых %d байт", expectedBytes))
				}
				total += int64(n)

				record(int64(n))
			}
			if readErr != nil {
				resp.Body.Close()
				if errors.Is(readErr, io.EOF) {
					if total != expectedBytes {
						return total, service.ProtocolError(fmt.Errorf("при скачивании получено %d байт, ожидалось %d", total, expectedBytes))
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
	return 0, service.AuthorizationError(errors.New("авторизация скачивания не удалась после обновления токена"))
}

func (p *Backend) Upload(ctx context.Context, cfg service.MeasurementConfig, progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	servers, err := p.measurementServers()
	if err != nil {
		code, retryable := measurementServerError(err)
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseUpload, code, retryable, err)
	}
	initial, maximum, err := service.ConnectionLimits(cfg, min(16, len(servers)*2))
	if err != nil {
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseUpload, service.CodeInternal, false, err)
	}
	if _, err := p.ensureJWT(ctx, false, ""); err != nil {
		code := service.ClassifyError(err)
		return service.ThroughputResult{}, service.NewError(p.ID(), service.PhaseUpload, code, service.RetryableCode(code), err)
	}
	payload := make([]byte, uploadBlockSize)
	serverAttempts := make([]atomic.Uint32, maximum)
	runResult, runErr := measure.Run(ctx, measure.RunConfig{
		Duration: cfg.Duration, Warmup: 4 * time.Second, StartupTimeout: 12 * time.Second,
		ReadyGrace: 500 * time.Millisecond, InitialWorkers: initial, MaxWorkers: maximum, Reconnects: 1,
		Adaptive: service.AdaptiveConnections, OnWorkerError: service.WorkerErrorLogger(p.log, p.ID(), service.PhaseUpload),
	}, func(workerCtx context.Context, index int, ready func(), record func(int64)) error {
		server := nextServerForWorker(servers, index, &serverAttempts[index])
		for workerCtx.Err() == nil {
			if err := p.uploadRequest(workerCtx, server, payload, ready); err != nil {
				return err
			}
			record(int64(len(payload)))
		}
		return workerCtx.Err()
	}, service.AdaptProgress(progress))
	result := service.ConvertRunResult(runResult)
	if runErr != nil {
		code := service.ClassifyError(runErr)
		return result, service.NewError(p.ID(), service.PhaseUpload, code, service.RetryableCode(code), runErr)
	}
	return result, nil
}

func (p *Backend) uploadRequest(ctx context.Context, server qmsServer, payload []byte, ready func()) error {
	if len(payload) != uploadBlockSize {
		return service.ProtocolError(fmt.Errorf("неверный размер блока отдачи: %d байт, ожидалось %d", len(payload), uploadBlockSize))
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
				return service.AuthorizationError(fmt.Errorf("авторизация отдачи отклонена после обновления JWT: %s", resp.Status))
			}
			if _, err := p.ensureJWT(ctx, true, token); err != nil {
				return err
			}
			continue
		}
		if resp.StatusCode < 200 || resp.StatusCode >= 300 {
			return &service.HTTPStatusError{StatusCode: resp.StatusCode, Status: resp.Status, Operation: "сервер " + server.Host + " при отдаче"}
		}
		if copyErr != nil {
			return copyErr
		}
		if responseBytes > 1<<20 {
			return service.ProtocolError(errors.New("ответ сервера при отдаче превышает безопасный предел"))
		}
		if closeErr != nil {
			return closeErr
		}
		ready()
		return nil
	}
	return service.AuthorizationError(errors.New("авторизация отдачи не удалась после обновления токена"))
}

func (p *Backend) measurementServers() ([]qmsServer, error) {
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

func measurementServerError(err error) (service.ErrorCode, bool) {
	if errors.Is(err, errPingOnlyFallback) {
		return service.CodeAuth, true
	}
	return service.CodeInternal, false
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
