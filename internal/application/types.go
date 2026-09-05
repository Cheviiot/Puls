// Package application coordinates measurement services independently from a
// concrete user interface. Both the terminal and graphical frontends use the
// types and runner defined here.
package application

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

const SchemaVersion = 1

type Command string

const (
	CommandMeasure Command = "measure"
	CommandIP      Command = "ip"
)

type Profile string

const (
	ProfileQuick    Profile = "quick"
	ProfileBalanced Profile = "balanced"
	ProfileAccurate Profile = "accurate"
)

func (p Profile) Duration() time.Duration {
	switch p {
	case ProfileQuick:
		return 5 * time.Second
	case ProfileAccurate:
		return 15 * time.Second
	default:
		return 10 * time.Second
	}
}

type PhaseSelection string

const (
	PhaseAll      PhaseSelection = "all"
	PhasePing     PhaseSelection = "ping"
	PhaseDownload PhaseSelection = "download"
	PhaseUpload   PhaseSelection = "upload"
)

type MeasureRequest struct {
	Service        service.ServiceID
	Profile        Profile
	Duration       time.Duration
	Connections    int
	Only           PhaseSelection
	Server         string
	ShowConnection bool
}

type ConnectionRequest struct {
	Service  service.ServiceID
	Explicit bool
}

type EventKind string

const (
	EventRunStarted          EventKind = "run_started"
	EventConnectionStarted   EventKind = "connection_started"
	EventConnectionCompleted EventKind = "connection_completed"
	EventServiceStarted      EventKind = "service_started"
	EventPhaseStarted        EventKind = "phase_started"
	EventServerSelected      EventKind = "server_selected"
	EventPingCompleted       EventKind = "ping_completed"
	EventThroughputProgress  EventKind = "throughput_progress"
	EventPhaseCompleted      EventKind = "phase_completed"
	EventServiceCompleted    EventKind = "service_completed"
	EventRunCompleted        EventKind = "run_completed"
)

// RunEvent is an immutable snapshot of a runner transition. Pointer fields
// refer to copies owned by the event and may be retained by observers.
type RunEvent struct {
	Kind        EventKind
	Service     service.ServiceID
	Phase       service.Phase
	Server      *service.Server
	Ping        *service.PingResult
	Throughput  *service.ThroughputProgress
	PhaseResult *PhaseResult
	Measurement *MeasurementResult
	Connection  *ConnectionResult
	Envelope    *Envelope
}

// Observer must return promptly. GUI observers should enqueue the snapshot and
// update widgets on the Fyne event thread.
type Observer func(RunEvent)

type Envelope struct {
	SchemaVersion int                 `json:"schema_version"`
	Command       Command             `json:"command"`
	Status        service.Status      `json:"status"`
	Connection    *ConnectionResult   `json:"connection"`
	Results       []MeasurementResult `json:"results"`
}

type ConnectionResult struct {
	Status     service.Status     `json:"status"`
	ExternalIP *string            `json:"external_ip"`
	ISP        *string            `json:"isp"`
	DetectedBy *service.ServiceID `json:"detected_by"`
	Warnings   []string           `json:"warnings"`
	Error      *ErrorResult       `json:"error"`
}

type MeasurementResult struct {
	Service  service.ServiceID `json:"service"`
	Status   service.Status    `json:"status"`
	Server   *ServerResult     `json:"server"`
	Phases   MeasurementPhases `json:"phases"`
	Error    *ErrorResult      `json:"error"`
	Warnings []string          `json:"warnings"`
}

type ServerResult struct {
	Name   string  `json:"name"`
	City   *string `json:"city"`
	Region *string `json:"region"`
}

type MeasurementPhases struct {
	Select   PhaseResult `json:"select"`
	Ping     PhaseResult `json:"ping"`
	Download PhaseResult `json:"download"`
	Upload   PhaseResult `json:"upload"`
}

type PhaseResult struct {
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
	Error             *ErrorResult   `json:"error"`
}

type ErrorResult struct {
	Code      service.ErrorCode `json:"code"`
	Phase     service.Phase     `json:"phase"`
	Message   string            `json:"message"`
	Retryable bool              `json:"retryable"`
}

func NewEnvelope(command Command) Envelope {
	return Envelope{
		SchemaVersion: SchemaVersion,
		Command:       command,
		Status:        service.StatusPending,
		Results:       []MeasurementResult{},
	}
}

func NewMeasurementResult(id service.ServiceID, selection PhaseSelection) MeasurementResult {
	result := MeasurementResult{
		Service:  id,
		Status:   service.StatusPending,
		Warnings: []string{},
		Phases: MeasurementPhases{
			Select:   EmptyPhase(service.StatusPending),
			Ping:     EmptyPhase(service.StatusSkipped),
			Download: EmptyPhase(service.StatusSkipped),
			Upload:   EmptyPhase(service.StatusSkipped),
		},
	}
	switch selection {
	case PhaseAll:
		result.Phases.Ping.Status = service.StatusPending
		result.Phases.Download.Status = service.StatusPending
		result.Phases.Upload.Status = service.StatusPending
	case PhasePing:
		result.Phases.Ping.Status = service.StatusPending
	case PhaseDownload:
		result.Phases.Download.Status = service.StatusPending
	case PhaseUpload:
		result.Phases.Upload.Status = service.StatusPending
	}
	return result
}

func EmptyPhase(status service.Status) PhaseResult {
	return PhaseResult{Status: status, Warnings: []string{}}
}

func PingPhase(value service.PingResult) PhaseResult {
	primary, minimum := Round2(value.ValueMs), Round2(value.MinMs)
	median, average, jitter := Round2(value.MedianMs), Round2(value.AvgMs), Round2(value.JitterMs)
	samples, method := value.Samples, value.Method
	return PhaseResult{
		Status: service.StatusOK, ValueMs: &primary, MinMs: &minimum,
		MedianMs: &median, AverageMs: &average, JitterMs: &jitter,
		Samples: &samples, Method: &method, Warnings: []string{},
	}
}

func ThroughputPhase(value service.ThroughputResult, err error) PhaseResult {
	result := EmptyPhase(service.StatusError)
	if value.Bytes > 0 || value.Elapsed > 0 || value.SuccessfulConnections > 0 || value.FailedConnections > 0 {
		bytes := value.Bytes
		successful, failed := value.SuccessfulConnections, value.FailedConnections
		result.Bytes = &bytes
		result.SuccessfulStreams = &successful
		result.FailedStreams = &failed
	}
	result.Warnings = AppendUnique(result.Warnings, value.Warnings...)
	if value.Elapsed > 0 {
		elapsed := Round2(float64(value.Elapsed) / float64(time.Millisecond))
		result.ElapsedMs = &elapsed
	}
	if err == nil {
		mbps := Round2(value.Mbps)
		result.Status = service.StatusOK
		result.Mbps = &mbps
	}
	return result
}

func PhaseError(phase service.Phase, err error) *ErrorResult {
	if err == nil {
		return nil
	}
	result := ErrorResult{Code: service.CodeUnavailable, Phase: phase, Message: HumanError(err)}
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

func SetPhaseError(result *MeasurementResult, phase *PhaseResult, phaseID service.Phase, err error) {
	phase.Status = service.StatusError
	if errors.Is(err, context.Canceled) {
		phase.Status = service.StatusCanceled
	}
	phase.Error = PhaseError(phaseID, err)
	if result.Error == nil {
		result.Error = phase.Error
	}
}

func MeasurementStatus(result MeasurementResult) service.Status {
	if result.Phases.Select.Status == service.StatusCanceled {
		return service.StatusCanceled
	}
	if result.Phases.Select.Status == service.StatusError {
		return service.StatusError
	}
	succeeded, failed := 0, 0
	for _, phase := range []PhaseResult{result.Phases.Ping, result.Phases.Download, result.Phases.Upload} {
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

func AggregateStatus(results []MeasurementResult) service.Status {
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

func WriteJSON(w io.Writer, result Envelope) error {
	encoder := json.NewEncoder(w)
	encoder.SetIndent("", "  ")
	return encoder.Encode(result)
}

func HumanError(err error) string {
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

func AppendUnique(destination []string, values ...string) []string {
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

func StringPointer(value string) *string {
	if value == "" {
		return nil
	}
	return &value
}

func Round2(value float64) float64 { return math.Round(value*100) / 100 }
