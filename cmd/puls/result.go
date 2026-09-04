package main

import (
	"context"
	"encoding/json"
	"errors"
	"io"
	"math"
	"net"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

const schemaVersion = 1

type envelope struct {
	SchemaVersion int                 `json:"schema_version"`
	Command       command             `json:"command"`
	Status        service.Status      `json:"status"`
	Connection    *connectionResult   `json:"connection"`
	Results       []measurementResult `json:"results"`
}

type connectionResult struct {
	Status     service.Status     `json:"status"`
	ExternalIP *string            `json:"external_ip"`
	ISP        *string            `json:"isp"`
	DetectedBy *service.ServiceID `json:"detected_by"`
	Warnings   []string           `json:"warnings"`
	Error      *errorResult       `json:"error"`
}

type measurementResult struct {
	Service  service.ServiceID `json:"service"`
	Status   service.Status    `json:"status"`
	Server   *serverResult     `json:"server"`
	Phases   measurementPhases `json:"phases"`
	Error    *errorResult      `json:"error"`
	Warnings []string          `json:"warnings"`
}

type serverResult struct {
	Name   string  `json:"name"`
	City   *string `json:"city"`
	Region *string `json:"region"`
}

type measurementPhases struct {
	Select   phaseResult `json:"select"`
	Ping     phaseResult `json:"ping"`
	Download phaseResult `json:"download"`
	Upload   phaseResult `json:"upload"`
}

type phaseResult struct {
	Status            service.Status `json:"status"`
	ValueMs           *float64       `json:"value_ms"`
	MinMs             *float64       `json:"min_ms"`
	MedianMs          *float64       `json:"median_ms"`
	AverageMs         *float64       `json:"average_ms"`
	JitterMs          *float64       `json:"jitter_ms"`
	Samples           *int           `json:"samples"`
	Method            *string        `json:"method"`
	Mbps              *float64       `json:"mbps"`
	Bytes             *int64         `json:"bytes"`
	ElapsedMs         *float64       `json:"elapsed_ms"`
	SuccessfulStreams *int           `json:"successful_streams"`
	FailedStreams     *int           `json:"failed_streams"`
	Warnings          []string       `json:"warnings"`
	Error             *errorResult   `json:"error"`
}

type errorResult struct {
	Code      service.ErrorCode `json:"code"`
	Phase     service.Phase     `json:"phase"`
	Message   string            `json:"message"`
	Retryable bool              `json:"retryable"`
}

func newEnvelope(cmd command) envelope {
	return envelope{
		SchemaVersion: schemaVersion,
		Command:       cmd,
		Status:        service.StatusPending,
		Results:       []measurementResult{},
	}
}

func newMeasurementResult(id service.ServiceID, selection phaseSelection) measurementResult {
	result := measurementResult{
		Service:  id,
		Status:   service.StatusPending,
		Warnings: []string{},
		Phases: measurementPhases{
			Select:   emptyPhase(service.StatusPending),
			Ping:     emptyPhase(service.StatusSkipped),
			Download: emptyPhase(service.StatusSkipped),
			Upload:   emptyPhase(service.StatusSkipped),
		},
	}
	switch selection {
	case phaseAll:
		result.Phases.Ping.Status = service.StatusPending
		result.Phases.Download.Status = service.StatusPending
		result.Phases.Upload.Status = service.StatusPending
	case phasePing:
		result.Phases.Ping.Status = service.StatusPending
	case phaseDownload:
		result.Phases.Download.Status = service.StatusPending
	case phaseUpload:
		result.Phases.Upload.Status = service.StatusPending
	}
	return result
}

func emptyPhase(status service.Status) phaseResult {
	return phaseResult{Status: status, Warnings: []string{}}
}

func pingPhase(value service.PingResult) phaseResult {
	primary, minimum := round2(value.ValueMs), round2(value.MinMs)
	median, average, jitter := round2(value.MedianMs), round2(value.AvgMs), round2(value.JitterMs)
	samples, method := value.Samples, value.Method
	return phaseResult{
		Status: service.StatusOK, ValueMs: &primary, MinMs: &minimum,
		MedianMs: &median, AverageMs: &average, JitterMs: &jitter,
		Samples: &samples, Method: &method, Warnings: []string{},
	}
}

func throughputPhase(value service.ThroughputResult, err error) phaseResult {
	result := emptyPhase(service.StatusError)
	if value.Bytes > 0 || value.Elapsed > 0 || value.SuccessfulConnections > 0 || value.FailedConnections > 0 {
		bytes := value.Bytes
		successful, failed := value.SuccessfulConnections, value.FailedConnections
		result.Bytes = &bytes
		result.SuccessfulStreams = &successful
		result.FailedStreams = &failed
	}
	result.Warnings = appendUnique(result.Warnings, value.Warnings...)
	if value.Elapsed > 0 {
		elapsed := round2(float64(value.Elapsed) / float64(time.Millisecond))
		result.ElapsedMs = &elapsed
	}
	if err == nil {
		mbps := round2(value.Mbps)
		result.Status = service.StatusOK
		result.Mbps = &mbps
	}
	return result
}

func phaseError(phase service.Phase, err error) *errorResult {
	if err == nil {
		return nil
	}
	result := errorResult{
		Code:    service.CodeUnavailable,
		Phase:   phase,
		Message: humanError(err),
	}
	var operationError *service.OpError
	if errors.As(err, &operationError) {
		result.Code = operationError.Code
		result.Retryable = operationError.Retryable
		if operationError.Phase != "" {
			result.Phase = operationError.Phase
		}
	} else if errors.Is(err, context.Canceled) {
		result.Code = service.CodeCanceled
	} else if errors.Is(err, context.DeadlineExceeded) {
		result.Code = service.CodeTimeout
		result.Retryable = true
	} else {
		var networkError net.Error
		if errors.As(err, &networkError) {
			result.Retryable = true
			if networkError.Timeout() {
				result.Code = service.CodeTimeout
			}
		}
	}
	return &result
}

func setPhaseError(result *measurementResult, phase *phaseResult, phaseID service.Phase, err error) {
	phase.Status = service.StatusError
	if errors.Is(err, context.Canceled) {
		phase.Status = service.StatusCanceled
	}
	phase.Error = phaseError(phaseID, err)
	if result.Error == nil {
		result.Error = phase.Error
	}
}

func measurementStatus(result measurementResult) service.Status {
	if result.Phases.Select.Status == service.StatusCanceled {
		return service.StatusCanceled
	}
	if result.Phases.Select.Status == service.StatusError {
		return service.StatusError
	}
	succeeded, failed := 0, 0
	for _, phase := range []phaseResult{result.Phases.Ping, result.Phases.Download, result.Phases.Upload} {
		switch phase.Status {
		case service.StatusOK:
			succeeded++
		case service.StatusError:
			failed++
		case service.StatusCanceled:
			return service.StatusCanceled
		}
	}
	if failed == 0 {
		return service.StatusOK
	}
	if succeeded > 0 {
		return service.StatusPartial
	}
	return service.StatusError
}

func aggregateStatus(results []measurementResult) service.Status {
	if len(results) == 0 {
		return service.StatusError
	}
	ok, failed := 0, 0
	for _, result := range results {
		switch result.Status {
		case service.StatusCanceled:
			return service.StatusCanceled
		case service.StatusOK:
			ok++
		case service.StatusPartial:
			ok++
			failed++
		default:
			failed++
		}
	}
	switch {
	case failed == 0:
		return service.StatusOK
	case ok > 0:
		return service.StatusPartial
	default:
		return service.StatusError
	}
}

func writeJSON(w io.Writer, result envelope) error {
	encoder := json.NewEncoder(w)
	encoder.SetIndent("", "  ")
	return encoder.Encode(result)
}

func humanError(err error) string {
	switch {
	case err == nil:
		return "неизвестная ошибка"
	case errors.Is(err, context.Canceled):
		return "операция отменена"
	case errors.Is(err, context.DeadlineExceeded):
		return "истекло время ожидания"
	}
	var operationError *service.OpError
	if errors.As(err, &operationError) {
		switch operationError.Code {
		case service.CodeCanceled:
			return "операция отменена"
		case service.CodeTimeout:
			return "истекло время ожидания"
		case service.CodeProtocol:
			return "ответ сервиса не соответствует протоколу"
		case service.CodeAuth:
			return "сервис отклонил авторизацию"
		case service.CodeUnavailable:
			return "сервис временно недоступен"
		case service.CodeInternal:
			return "внутренняя ошибка"
		}
	}
	var networkError net.Error
	if errors.As(err, &networkError) && networkError.Timeout() {
		return "истекло время ожидания"
	}
	return err.Error()
}

func appendUnique(destination []string, values ...string) []string {
	for _, value := range values {
		if value == "" {
			continue
		}
		seen := false
		for _, existing := range destination {
			if existing == value {
				seen = true
				break
			}
		}
		if !seen {
			destination = append(destination, value)
		}
	}
	return destination
}

func stringPointer(value string) *string {
	if value == "" {
		return nil
	}
	return &value
}

func round2(value float64) float64 { return math.Round(value*100) / 100 }
