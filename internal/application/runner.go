package application

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/service/speedtestru"
	"github.com/Cheviiot/Puls/internal/service/yandex"
)

const (
	phaseAttemptTimeout = 15 * time.Second
	connectionTimeout   = 15 * time.Second
)

type BackendFactory func(server string, log service.LogFunc) service.Backend

type Options struct {
	YandexFactory    BackendFactory
	SpeedtestFactory BackendFactory
	Log              service.LogFunc
}

type Runner struct {
	yandexFactory    BackendFactory
	speedtestFactory BackendFactory
	log              service.LogFunc
}

func NewRunner(options Options) *Runner {
	yandexFactory := options.YandexFactory
	if yandexFactory == nil {
		yandexFactory = func(_ string, log service.LogFunc) service.Backend {
			return yandex.New(yandex.Options{Log: log})
		}
	}
	speedtestFactory := options.SpeedtestFactory
	if speedtestFactory == nil {
		speedtestFactory = func(server string, log service.LogFunc) service.Backend {
			return speedtestru.New(speedtestru.Options{Server: server, Log: log})
		}
	}
	return &Runner{yandexFactory: yandexFactory, speedtestFactory: speedtestFactory, log: options.Log}
}

func ValidateMeasureRequest(request MeasureRequest) error {
	switch request.Service {
	case service.Yandex, service.Speedtest, service.All:
	default:
		return fmt.Errorf("неизвестный сервис %q", request.Service)
	}
	switch request.Profile {
	case ProfileQuick, ProfileBalanced, ProfileAccurate:
	default:
		return fmt.Errorf("неизвестный профиль %q", request.Profile)
	}
	if request.Duration == 0 {
		request.Duration = request.Profile.Duration()
	}
	if request.Duration < 3*time.Second || request.Duration > 60*time.Second {
		return errors.New("длительность должна быть от 3 до 60 секунд")
	}
	if request.Connections < 0 || request.Connections > 16 {
		return errors.New("число соединений должно быть 0 (авто) или от 1 до 16")
	}
	switch request.Only {
	case PhaseAll, PhasePing, PhaseDownload, PhaseUpload:
	default:
		return fmt.Errorf("неизвестный этап %q", request.Only)
	}
	if strings.TrimSpace(request.Server) != "" && request.Service != service.Speedtest {
		return errors.New("ручной сервер можно использовать только с сервисом speedtest")
	}
	return nil
}

func (r *Runner) Measure(ctx context.Context, request MeasureRequest, observer Observer) Envelope {
	result := NewEnvelope(CommandMeasure)
	if request.Duration == 0 {
		request.Duration = request.Profile.Duration()
	}
	emit(observer, RunEvent{Kind: EventRunStarted})

	if request.ShowConnection {
		connectionRequest := ConnectionRequest{Service: request.Service, Explicit: request.Service != service.All}
		if request.Service == service.All {
			connectionRequest.Service = ""
		}
		connection := r.DetectConnection(ctx, connectionRequest, observer)
		result.Connection = &connection
	}

	for _, backend := range r.backends(request) {
		measured := r.measureService(ctx, backend, request, observer)
		result.Results = append(result.Results, measured)
		if ctx.Err() != nil {
			break
		}
	}
	result.Status = AggregateStatus(result.Results)
	if errors.Is(ctx.Err(), context.Canceled) {
		result.Status = service.StatusCanceled
	}
	completed := result
	emit(observer, RunEvent{Kind: EventRunCompleted, Envelope: &completed})
	return result
}

func (r *Runner) DetectConnection(ctx context.Context, request ConnectionRequest, observer Observer) ConnectionResult {
	emit(observer, RunEvent{Kind: EventConnectionStarted})
	result := ConnectionResult{Status: service.StatusPending, Warnings: []string{}}
	services := []service.ServiceID{request.Service}
	if !request.Explicit {
		services = []service.ServiceID{service.Speedtest, service.Yandex}
	}

	var lookupErrors []error
	for index, id := range services {
		backend := r.connectionBackend(id)
		detector, ok := backend.(service.ConnectionInfoBackend)
		if !ok {
			err := service.NewError(id, service.PhaseConnection, service.CodeInternal, false, errors.New("сервис не поддерживает определение подключения"))
			lookupErrors = append(lookupErrors, err)
			continue
		}
		attemptCtx, cancel := context.WithTimeout(ctx, connectionTimeout)
		info, err := detector.DetectConnection(attemptCtx)
		cancel()
		if err == nil && !info.ExternalIP.IsValid() {
			err = service.NewError(id, service.PhaseConnection, service.CodeProtocol, false, errors.New("сервис не вернул действительный IP-адрес"))
		}
		if err != nil {
			lookupErrors = append(lookupErrors, err)
			r.logf("данные подключения через %s: %v", service.DisplayName(id), err)
			if errors.Is(err, context.Canceled) || errors.Is(ctx.Err(), context.Canceled) {
				result.Status = service.StatusCanceled
				result.Error = PhaseError(service.PhaseConnection, context.Canceled)
				completed := result
				emit(observer, RunEvent{Kind: EventConnectionCompleted, Connection: &completed})
				return result
			}
			continue
		}

		ip := info.ExternalIP.String()
		result.Status = service.StatusOK
		result.ExternalIP = &ip
		result.ISP = StringPointer(info.ISP)
		result.DetectedBy = &id
		result.Warnings = AppendUnique(result.Warnings, info.Warnings...)
		for _, warning := range info.Warnings {
			r.logf("данные подключения через %s: %s", service.DisplayName(id), warning)
		}
		if index > 0 {
			warning := fmt.Sprintf("использован резервный сервис %s", service.DisplayName(id))
			result.Warnings = AppendUnique(result.Warnings, warning)
			r.logf("fallback определения подключения: %s", service.DisplayName(id))
		}
		completed := result
		emit(observer, RunEvent{Kind: EventConnectionCompleted, Service: id, Connection: &completed})
		return result
	}

	err := errors.Join(lookupErrors...)
	if err == nil {
		err = errors.New("нет доступного сервиса определения подключения")
	}
	result.Status = service.StatusError
	result.Error = PhaseError(service.PhaseConnection, err)
	completed := result
	emit(observer, RunEvent{Kind: EventConnectionCompleted, Connection: &completed})
	return result
}

func (r *Runner) measureService(ctx context.Context, backend service.Backend, request MeasureRequest, observer Observer) MeasurementResult {
	result := NewMeasurementResult(backend.ID(), request.Only)
	emit(observer, RunEvent{Kind: EventServiceStarted, Service: backend.ID()})

	emit(observer, RunEvent{Kind: EventPhaseStarted, Service: backend.ID(), Phase: service.PhaseSelect})
	var server service.Server
	err := withRetry(ctx, 2, func() error {
		attemptCtx, cancel := context.WithTimeout(ctx, phaseAttemptTimeout)
		defer cancel()
		var selectErr error
		server, selectErr = backend.SelectServer(attemptCtx)
		return selectErr
	})
	if err != nil {
		r.logPhaseError(backend.ID(), service.PhaseSelect, err)
		SetPhaseError(&result, &result.Phases.Select, service.PhaseSelect, err)
		skipPendingPhases(&result)
		result.Status = MeasurementStatus(result)
		phaseCopy, resultCopy := result.Phases.Select, result
		emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: service.PhaseSelect, PhaseResult: &phaseCopy})
		emit(observer, RunEvent{Kind: EventServiceCompleted, Service: backend.ID(), Measurement: &resultCopy})
		return result
	}
	result.Phases.Select.Status = service.StatusOK
	result.Server = &ServerResult{Name: server.Name, City: StringPointer(server.City), Region: StringPointer(server.Region)}
	r.logf("сервис=%s сервер измерения=%s", backend.ID(), server.Name)
	serverCopy, phaseCopy := server, result.Phases.Select
	emit(observer, RunEvent{Kind: EventServerSelected, Service: backend.ID(), Phase: service.PhaseSelect, Server: &serverCopy})
	emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: service.PhaseSelect, PhaseResult: &phaseCopy})

	if result.Phases.Ping.Status == service.StatusPending {
		emit(observer, RunEvent{Kind: EventPhaseStarted, Service: backend.ID(), Phase: service.PhasePing})
		var ping service.PingResult
		pingErr := withRetry(ctx, 2, func() error {
			attemptCtx, cancel := context.WithTimeout(ctx, phaseAttemptTimeout)
			defer cancel()
			var attemptErr error
			ping, attemptErr = backend.Ping(attemptCtx)
			return attemptErr
		})
		if pingErr != nil {
			r.logPhaseError(backend.ID(), service.PhasePing, pingErr)
			SetPhaseError(&result, &result.Phases.Ping, service.PhasePing, pingErr)
		} else {
			result.Phases.Ping = PingPhase(ping)
			pingCopy := ping
			emit(observer, RunEvent{Kind: EventPingCompleted, Service: backend.ID(), Phase: service.PhasePing, Ping: &pingCopy})
		}
		phaseCopy := result.Phases.Ping
		emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: service.PhasePing, PhaseResult: &phaseCopy})
	}

	r.runThroughput(ctx, backend, request, service.PhaseDownload, &result, observer)
	r.runThroughput(ctx, backend, request, service.PhaseUpload, &result, observer)

	result.Status = MeasurementStatus(result)
	resultCopy := result
	emit(observer, RunEvent{Kind: EventServiceCompleted, Service: backend.ID(), Measurement: &resultCopy})
	return result
}

func (r *Runner) runThroughput(ctx context.Context, backend service.Backend, request MeasureRequest, phase service.Phase, result *MeasurementResult, observer Observer) {
	var destination *PhaseResult
	var capability service.Capability
	var run func(func(service.ThroughputProgress)) (service.ThroughputResult, error)
	switch phase {
	case service.PhaseDownload:
		destination = &result.Phases.Download
		capability = service.CapDownload
		run = func(progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
			return backend.Download(ctx, service.MeasurementConfig{Duration: request.Duration, Connections: request.Connections, MaxConnections: 16}, progress)
		}
	case service.PhaseUpload:
		destination = &result.Phases.Upload
		capability = service.CapUpload
		run = func(progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
			return backend.Upload(ctx, service.MeasurementConfig{Duration: request.Duration, Connections: request.Connections, MaxConnections: 16}, progress)
		}
	default:
		return
	}
	if destination.Status != service.StatusPending {
		return
	}
	if ctx.Err() != nil {
		SetPhaseError(result, destination, phase, ctx.Err())
		phaseCopy := *destination
		emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: phase, PhaseResult: &phaseCopy})
		return
	}
	if !backend.Capabilities().Has(capability) {
		destination.Status = service.StatusSkipped
		phaseCopy := *destination
		emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: phase, PhaseResult: &phaseCopy})
		return
	}

	emit(observer, RunEvent{Kind: EventPhaseStarted, Service: backend.ID(), Phase: phase})
	measured, phaseErr := run(func(value service.ThroughputProgress) {
		progressCopy := value
		emit(observer, RunEvent{Kind: EventThroughputProgress, Service: backend.ID(), Phase: phase, Throughput: &progressCopy})
	})
	*destination = ThroughputPhase(measured, phaseErr)
	result.Warnings = AppendUnique(result.Warnings, measured.Warnings...)
	if phaseErr != nil {
		r.logPhaseError(backend.ID(), phase, phaseErr)
		SetPhaseError(result, destination, phase, phaseErr)
	}
	phaseCopy := *destination
	emit(observer, RunEvent{Kind: EventPhaseCompleted, Service: backend.ID(), Phase: phase, PhaseResult: &phaseCopy})
}

func (r *Runner) backends(request MeasureRequest) []service.Backend {
	switch request.Service {
	case service.Speedtest:
		return []service.Backend{r.speedtestFactory(request.Server, r.log)}
	case service.All:
		return []service.Backend{r.yandexFactory("", r.log), r.speedtestFactory("", r.log)}
	default:
		return []service.Backend{r.yandexFactory("", r.log)}
	}
}

func (r *Runner) connectionBackend(id service.ServiceID) service.Backend {
	if id == service.Yandex {
		return r.yandexFactory("", r.log)
	}
	return r.speedtestFactory("", r.log)
}

func (r *Runner) logf(format string, arguments ...any) {
	if r.log != nil {
		r.log(format, arguments...)
	}
}

func (r *Runner) logPhaseError(id service.ServiceID, phase service.Phase, err error) {
	r.logf("сервис=%s этап=%s: %v", id, phase, err)
}

func skipPendingPhases(result *MeasurementResult) {
	for _, phase := range []*PhaseResult{&result.Phases.Ping, &result.Phases.Download, &result.Phases.Upload} {
		if phase.Status == service.StatusPending {
			phase.Status = service.StatusSkipped
		}
	}
}

func emit(observer Observer, event RunEvent) {
	if observer == nil {
		return
	}
	defer func() { _ = recover() }()
	observer(event)
}
