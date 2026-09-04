package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

const phaseAttemptTimeout = 15 * time.Second

func runMeasurement(
	ctx context.Context,
	backend service.Backend,
	output io.Writer,
	style *ui.Style,
	live bool,
	progressWidth int,
	cfg service.MeasurementConfig,
	selection phaseSelection,
	log service.LogFunc,
) measurementResult {
	result := newMeasurementResult(backend.ID(), selection)
	renderServiceHeader(output, style, backend.ID())

	selectLine := ui.NewLine(output, live)
	server, err := ui.Spin(selectLine, "выбор сервера измерения…", func() (service.Server, error) {
		var selected service.Server
		err := withRetry(ctx, 2, func() error {
			attemptCtx, cancel := context.WithTimeout(ctx, phaseAttemptTimeout)
			defer cancel()
			var selectErr error
			selected, selectErr = backend.SelectServer(attemptCtx)
			return selectErr
		})
		return selected, err
	})
	if err != nil {
		logPhaseError(log, backend.ID(), service.PhaseSelect, err)
		setPhaseError(&result, &result.Phases.Select, service.PhaseSelect, err)
		skipPendingPhases(&result)
		selectLine.Final(metricLabel(style, "Сервер") + phaseFailure(style, err))
		result.Status = measurementStatus(result)
		renderMeasurementFooter(output, style, result)
		return result
	}
	result.Phases.Select.Status = service.StatusOK
	result.Server = &serverResult{Name: server.Name, City: stringPointer(server.City), Region: stringPointer(server.Region)}
	if log != nil {
		log("сервис=%s сервер измерения=%s", backend.ID(), server.Name)
	}
	selectLine.Final(metricLabel(style, "Сервер") + formatServer(server))

	if result.Phases.Ping.Status == service.StatusPending {
		pingLine := ui.NewLine(output, live)
		ping, pingErr := ui.Spin(pingLine, "измерение задержки…", func() (service.PingResult, error) {
			var measured service.PingResult
			err := withRetry(ctx, 2, func() error {
				attemptCtx, cancel := context.WithTimeout(ctx, phaseAttemptTimeout)
				defer cancel()
				var attemptErr error
				measured, attemptErr = backend.Ping(attemptCtx)
				return attemptErr
			})
			return measured, err
		})
		if pingErr != nil {
			logPhaseError(log, backend.ID(), service.PhasePing, pingErr)
			setPhaseError(&result, &result.Phases.Ping, service.PhasePing, pingErr)
			pingLine.Final(metricLabel(style, "Задержка") + phaseFailure(style, pingErr))
		} else {
			result.Phases.Ping = pingPhase(ping)
			pingLine.Final(renderPingMetric(style, ping))
		}
	}

	runThroughputPhase(ctx, backend, output, style, live, progressWidth, cfg, service.PhaseDownload, &result, log)
	runThroughputPhase(ctx, backend, output, style, live, progressWidth, cfg, service.PhaseUpload, &result, log)

	result.Status = measurementStatus(result)
	renderMeasurementFooter(output, style, result)
	return result
}

func runThroughputPhase(
	ctx context.Context,
	backend service.Backend,
	output io.Writer,
	style *ui.Style,
	live bool,
	progressWidth int,
	cfg service.MeasurementConfig,
	phase service.Phase,
	result *measurementResult,
	log service.LogFunc,
) {
	var destination *phaseResult
	var capability service.Capability
	var run func(func(service.ThroughputProgress)) (service.ThroughputResult, error)
	label := "Загрузка"
	switch phase {
	case service.PhaseDownload:
		destination = &result.Phases.Download
		capability = service.CapDownload
		run = func(progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
			return backend.Download(ctx, cfg, progress)
		}
	case service.PhaseUpload:
		destination = &result.Phases.Upload
		capability = service.CapUpload
		label = "Отдача"
		run = func(progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
			return backend.Upload(ctx, cfg, progress)
		}
	default:
		return
	}
	if destination.Status != service.StatusPending {
		return
	}
	if ctx.Err() != nil {
		setPhaseError(result, destination, phase, ctx.Err())
		return
	}
	if !backend.Capabilities().Has(capability) {
		destination.Status = service.StatusSkipped
		fmt.Fprintln(output, metricLabel(style, label)+style.Dim("не поддерживается"))
		return
	}

	measured, phaseErr := measureThroughput(ctx, output, style, live, progressWidth, label, cfg, run)
	*destination = throughputPhase(measured, phaseErr)
	result.Warnings = appendUnique(result.Warnings, measured.Warnings...)
	if phaseErr != nil {
		logPhaseError(log, backend.ID(), phase, phaseErr)
		setPhaseError(result, destination, phase, phaseErr)
	}
}

func logPhaseError(log service.LogFunc, id service.ServiceID, phase service.Phase, err error) {
	if log != nil {
		log("сервис=%s этап=%s: %v", id, phase, err)
	}
}

func measureThroughput(
	ctx context.Context,
	output io.Writer,
	style *ui.Style,
	live bool,
	progressWidth int,
	label string,
	cfg service.MeasurementConfig,
	run func(func(service.ThroughputProgress)) (service.ThroughputResult, error),
) (service.ThroughputResult, error) {
	line := ui.NewLine(output, live)
	var progress func(service.ThroughputProgress)
	if live {
		progress = func(value service.ThroughputProgress) {
			fraction := max(0, min(1, float64(value.Elapsed)/float64(cfg.Duration)))
			bar := ""
			if progressWidth > 0 {
				bar = style.Cyan(ui.Bar(progressWidth, fraction)) + "  "
			}
			line.Update(fmt.Sprintf("%s%s%3.0f%%  %s", metricLabel(style, label), bar, fraction*100, style.Speed(value.Mbps)))
		}
	}
	measured, err := run(progress)
	if err != nil {
		if errors.Is(err, context.Canceled) {
			line.Final(metricLabel(style, label) + style.Dim("остановлено"))
		} else {
			line.Final(metricLabel(style, label) + phaseFailure(style, err))
		}
		return measured, err
	}
	line.Final(metricLabel(style, label) + style.Speed(measured.Mbps))
	return measured, nil
}

func skipPendingPhases(result *measurementResult) {
	for _, phase := range []*phaseResult{&result.Phases.Ping, &result.Phases.Download, &result.Phases.Upload} {
		if phase.Status == service.StatusPending {
			phase.Status = service.StatusSkipped
		}
	}
}
