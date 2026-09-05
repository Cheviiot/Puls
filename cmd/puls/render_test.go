package main

import (
	"bytes"
	"context"
	"strings"
	"testing"
	"time"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

func TestHelpUsesServiceTerminologyAndEnglishFlags(t *testing.T) {
	var output bytes.Buffer
	printHelp(&output, ui.NewStyle(false))
	text := output.String()
	for _, expected := range []string{"СЕРВИСЫ", "сервер измерения", "интернет-провайдер", "--show-ip", "puls ip speedtest"} {
		if !strings.Contains(strings.ToLower(text), strings.ToLower(expected)) {
			t.Errorf("help does not contain %q:\n%s", expected, text)
		}
	}
	for _, forbidden := range []string{"ИСТОЧНИКИ", "--ip", "--режим", "--длительность"} {
		if strings.Contains(text, forbidden) {
			t.Errorf("help contains obsolete term %q:\n%s", forbidden, text)
		}
	}
}

func TestHumanMeasurementIsCompactAndUnframed(t *testing.T) {
	var output bytes.Buffer
	app := testApplication(&output, &bytes.Buffer{}, &fakeBackend{id: service.Yandex}, &fakeBackend{id: service.Speedtest})
	if code := app.Run(context.Background(), []string{"yandex"}); code != 0 {
		t.Fatalf("Run() = %d", code)
	}
	text := output.String()
	for _, expected := range []string{"Puls · Яндекс.Интернетометр", "Сервер", "Задержка", "Загрузка", "Отдача", "готово"} {
		if !strings.Contains(text, expected) {
			t.Errorf("output missing %q:\n%s", expected, text)
		}
	}
	if strings.Contains(text, "────") {
		t.Errorf("output contains a frame:\n%s", text)
	}
}

func TestGoldenHumanYandex(t *testing.T) {
	var output bytes.Buffer
	app := testApplication(&output, &bytes.Buffer{}, &fakeBackend{id: service.Yandex}, &fakeBackend{id: service.Speedtest})
	if code := app.Run(context.Background(), []string{"yandex"}); code != 0 {
		t.Fatalf("Run() = %d", code)
	}
	want := "Puls · Яндекс.Интернетометр\n" +
		"  Сервер                mock.example · Владивосток\n" +
		"  Задержка              11.0 мс  ·  джиттер 1.5 мс\n" +
		"  Загрузка              100.00 Мбит/с\n" +
		"  Отдача                50.00 Мбит/с\n" +
		"  ✓ готово\n\n"
	if output.String() != want {
		t.Fatalf("human output changed:\n--- got ---\n%s--- want ---\n%s", output.String(), want)
	}
}

func TestTTYProgressIsAdaptiveAndPipeHasOnlyFinalLine(t *testing.T) {
	var terminal bytes.Buffer
	terminalRenderer := newTerminalObserver(&terminal, ui.NewStyle(false), true, 10, 10*time.Second)
	terminalRenderer.Observe(appcore.RunEvent{Kind: appcore.EventPhaseStarted, Phase: service.PhaseDownload})
	terminalRenderer.Observe(appcore.RunEvent{Kind: appcore.EventThroughputProgress, Phase: service.PhaseDownload, Throughput: &service.ThroughputProgress{Mbps: 80, Elapsed: 5 * time.Second}})
	terminalRenderer.Observe(appcore.RunEvent{Kind: appcore.EventPhaseCompleted, Phase: service.PhaseDownload, PhaseResult: &appcore.PhaseResult{Status: service.StatusOK, Mbps: floatPointer(80)}})
	if !strings.Contains(terminal.String(), "[█████░░░░░]") || !strings.Contains(terminal.String(), "\r\x1b[K") {
		t.Fatalf("TTY progress = %q", terminal.String())
	}
	var pipe bytes.Buffer
	pipeRenderer := newTerminalObserver(&pipe, ui.NewStyle(false), false, 0, 10*time.Second)
	pipeRenderer.Observe(appcore.RunEvent{Kind: appcore.EventPhaseStarted, Phase: service.PhaseDownload})
	pipeRenderer.Observe(appcore.RunEvent{Kind: appcore.EventThroughputProgress, Phase: service.PhaseDownload, Throughput: &service.ThroughputProgress{Mbps: 80, Elapsed: 5 * time.Second}})
	pipeRenderer.Observe(appcore.RunEvent{Kind: appcore.EventPhaseCompleted, Phase: service.PhaseDownload, PhaseResult: &appcore.PhaseResult{Status: service.StatusOK, Mbps: floatPointer(80)}})
	if strings.Count(pipe.String(), "\n") != 1 || strings.Contains(pipe.String(), "[") {
		t.Fatalf("pipe output = %q", pipe.String())
	}
}

func floatPointer(value float64) *float64 { return &value }

func TestFormatServer(t *testing.T) {
	got := formatServer(service.Server{Name: "host", City: "Москва", Region: "Москва"})
	if got != "host · Москва, Москва" {
		t.Fatalf("formatServer = %q", got)
	}
}
