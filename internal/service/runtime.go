package service

import (
	"context"
	"errors"
	"fmt"
	"net"
	"time"

	"github.com/Cheviiot/Puls/internal/measure"
)

var (
	// ErrProtocol marks validation failures in first-party service protocols.
	ErrProtocol = errors.New("ошибка протокола")
	// ErrAuthorization marks rejected or invalid service authorization.
	ErrAuthorization = errors.New("ошибка авторизации")
)

func ProtocolError(err error) error {
	if err == nil {
		return ErrProtocol
	}
	return fmt.Errorf("%w: %w", ErrProtocol, err)
}

func AuthorizationError(err error) error {
	if err == nil {
		return ErrAuthorization
	}
	return fmt.Errorf("%w: %w", ErrAuthorization, err)
}

// ClassifyError maps typed and standard-library errors to stable API codes.
func ClassifyError(err error) ErrorCode {
	switch {
	case errors.Is(err, context.Canceled):
		return CodeCanceled
	case errors.Is(err, context.DeadlineExceeded):
		return CodeTimeout
	case errors.Is(err, ErrAuthorization):
		return CodeAuth
	case errors.Is(err, ErrProtocol):
		return CodeProtocol
	}
	var statusError *HTTPStatusError
	if errors.As(err, &statusError) {
		code, _ := ClassifyHTTPStatus(statusError.StatusCode)
		return code
	}
	var networkError net.Error
	if errors.As(err, &networkError) && networkError.Timeout() {
		return CodeTimeout
	}
	return CodeUnavailable
}

func RetryableCode(code ErrorCode) bool {
	return code == CodeUnavailable || code == CodeTimeout || code == CodeAuth
}

func ConnectionLimits(cfg MeasurementConfig, native int) (int, int, error) {
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

func AdaptiveConnections(mbps float64) int {
	switch {
	case mbps > 300:
		return 16
	case mbps > 50:
		return 12
	default:
		return 8
	}
}

func AdaptProgress(progress func(ThroughputProgress)) func(measure.RunProgress) {
	if progress == nil {
		return nil
	}
	return func(value measure.RunProgress) {
		progress(ThroughputProgress{
			Mbps: value.Mbps, Bytes: value.Bytes, Elapsed: value.Elapsed, ActiveConnections: value.Active,
		})
	}
}

func ConvertRunResult(result measure.RunResult) ThroughputResult {
	warnings := []string{}
	workerErrors := 0
	for _, err := range result.WorkerErrors {
		if err != nil {
			workerErrors++
		}
	}
	if workerErrors > 0 {
		warnings = append(warnings, fmt.Sprintf("сетевые сбои отдельных потоков: %d", workerErrors))
	}
	return ThroughputResult{
		Mbps: measure.Mbps(result.Bytes, result.Elapsed), Bytes: result.Bytes, Elapsed: result.Elapsed,
		SuccessfulConnections: result.WorkersOK, FailedConnections: result.WorkersFailed, Warnings: warnings,
	}
}

func WorkerErrorLogger(log LogFunc, serviceID ServiceID, phase Phase) func(int, int, error) {
	if log == nil {
		return nil
	}
	return func(index, attempt int, err error) {
		reconnect := "нет"
		if attempt == 0 {
			reconnect = "да"
		}
		log("%s · %s: поток=%d, попытка=%d, переподключение=%s: %v", DisplayName(serviceID), PhaseDisplayName(phase), index+1, attempt+1, reconnect, err)
	}
}

func PhaseDisplayName(phase Phase) string {
	switch phase {
	case PhaseSelect:
		return "выбор сервера"
	case PhaseConnection:
		return "данные подключения"
	case PhasePing:
		return "задержка"
	case PhaseDownload:
		return "скачивание"
	case PhaseUpload:
		return "отдача"
	default:
		return string(phase)
	}
}
