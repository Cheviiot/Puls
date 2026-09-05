package main

import (
	"fmt"
	"io"
	"time"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

type terminalObserver struct {
	output        io.Writer
	style         *ui.Style
	live          bool
	progressWidth int
	duration      time.Duration
	lines         map[service.Phase]*ui.Line
	rendered      map[service.Phase]bool
}

func newTerminalObserver(output io.Writer, style *ui.Style, live bool, progressWidth int, duration time.Duration) *terminalObserver {
	return &terminalObserver{
		output: output, style: style, live: live, progressWidth: progressWidth, duration: duration,
		lines: make(map[service.Phase]*ui.Line), rendered: make(map[service.Phase]bool),
	}
}

func (t *terminalObserver) Observe(event appcore.RunEvent) {
	switch event.Kind {
	case appcore.EventConnectionStarted:
		line := ui.NewLine(t.output, t.live)
		line.Update("определение подключения…")
		t.lines[service.PhaseConnection] = line
	case appcore.EventConnectionCompleted:
		if line := t.lines[service.PhaseConnection]; line != nil {
			line.Update("")
		}
		if event.Connection != nil {
			renderConnection(t.output, t.style, *event.Connection)
		}
	case appcore.EventServiceStarted:
		t.lines = make(map[service.Phase]*ui.Line)
		t.rendered = make(map[service.Phase]bool)
		renderServiceHeader(t.output, t.style, event.Service)
	case appcore.EventPhaseStarted:
		line := ui.NewLine(t.output, t.live)
		line.Update(metricLabel(t.style, phaseLabel(event.Phase)) + t.style.Dim(phaseActivity(event.Phase)))
		t.lines[event.Phase] = line
	case appcore.EventServerSelected:
		if event.Server != nil {
			t.final(event.Phase, metricLabel(t.style, "Сервер")+formatServer(*event.Server))
		}
	case appcore.EventPingCompleted:
		if event.Ping != nil {
			t.final(service.PhasePing, renderPingMetric(t.style, *event.Ping))
		}
	case appcore.EventThroughputProgress:
		if event.Throughput != nil {
			t.progress(event.Phase, *event.Throughput)
		}
	case appcore.EventPhaseCompleted:
		if event.PhaseResult != nil {
			t.completePhase(event.Phase, *event.PhaseResult)
		}
	case appcore.EventServiceCompleted:
		if event.Measurement != nil {
			renderMeasurementFooter(t.output, t.style, *event.Measurement)
		}
	}
}

func (t *terminalObserver) progress(phase service.Phase, value service.ThroughputProgress) {
	line := t.line(phase)
	fraction := 0.0
	if t.duration > 0 {
		fraction = max(0, min(1, float64(value.Elapsed)/float64(t.duration)))
	}
	bar := ""
	if t.progressWidth > 0 {
		bar = t.style.Cyan(ui.Bar(t.progressWidth, fraction)) + "  "
	}
	line.Update(fmt.Sprintf("%s%s%3.0f%%  %s", metricLabel(t.style, phaseLabel(phase)), bar, fraction*100, t.style.Speed(value.Mbps)))
}

func (t *terminalObserver) completePhase(phase service.Phase, result appcore.PhaseResult) {
	if t.rendered[phase] {
		return
	}
	label := phaseLabel(phase)
	switch result.Status {
	case service.StatusOK:
		if result.Mbps != nil {
			t.final(phase, metricLabel(t.style, label)+t.style.Speed(*result.Mbps))
		} else if phase == service.PhaseSelect {
			return
		}
	case service.StatusSkipped:
		t.final(phase, metricLabel(t.style, label)+t.style.Dim("пропущено"))
	case service.StatusCanceled:
		t.final(phase, metricLabel(t.style, label)+t.style.Dim("остановлено"))
	case service.StatusError:
		message := ""
		if result.Error != nil {
			message = " · " + result.Error.Message
		}
		t.final(phase, metricLabel(t.style, label)+t.style.Red("не удалось")+t.style.Dim(message))
	}
}

func (t *terminalObserver) final(phase service.Phase, text string) {
	t.line(phase).Final(text)
	t.rendered[phase] = true
}

func (t *terminalObserver) line(phase service.Phase) *ui.Line {
	line := t.lines[phase]
	if line == nil {
		line = ui.NewLine(t.output, t.live)
		t.lines[phase] = line
	}
	return line
}

func phaseLabel(phase service.Phase) string {
	switch phase {
	case service.PhaseSelect:
		return "Сервер"
	case service.PhasePing:
		return "Задержка"
	case service.PhaseDownload:
		return "Загрузка"
	case service.PhaseUpload:
		return "Отдача"
	case service.PhaseConnection:
		return "Подключение"
	default:
		return string(phase)
	}
}

func phaseActivity(phase service.Phase) string {
	switch phase {
	case service.PhaseSelect:
		return "выбор сервера…"
	case service.PhasePing:
		return "измерение…"
	case service.PhaseDownload, service.PhaseUpload:
		return "подготовка…"
	default:
		return "выполнение…"
	}
}
