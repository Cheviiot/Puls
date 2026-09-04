package main

import (
	"bytes"
	"strings"
	"testing"

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
	result := runMeasurement(
		t.Context(),
		&fakeBackend{id: service.Yandex},
		&output,
		ui.NewStyle(false),
		false,
		0,
		service.MeasurementConfig{Duration: 10, MaxConnections: 16},
		phaseAll,
		nil,
	)
	if result.Status != service.StatusOK {
		t.Fatalf("result = %+v", result)
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
	runMeasurement(
		t.Context(), &fakeBackend{id: service.Yandex}, &output, ui.NewStyle(false),
		false, 0, service.MeasurementConfig{Duration: 10, MaxConnections: 16}, phaseAll, nil,
	)
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
	cfg := service.MeasurementConfig{Duration: 10}
	run := func(progress func(service.ThroughputProgress)) (service.ThroughputResult, error) {
		if progress != nil {
			progress(service.ThroughputProgress{Mbps: 80, Elapsed: 5})
		}
		return service.ThroughputResult{Mbps: 80}, nil
	}
	var terminal bytes.Buffer
	_, _ = measureThroughput(t.Context(), &terminal, ui.NewStyle(false), true, 10, "Загрузка", cfg, run)
	if !strings.Contains(terminal.String(), "[█████░░░░░]") || !strings.Contains(terminal.String(), "\r\x1b[K") {
		t.Fatalf("TTY progress = %q", terminal.String())
	}
	var pipe bytes.Buffer
	_, _ = measureThroughput(t.Context(), &pipe, ui.NewStyle(false), false, 0, "Загрузка", cfg, run)
	if strings.Count(pipe.String(), "\n") != 1 || strings.Contains(pipe.String(), "[") {
		t.Fatalf("pipe output = %q", pipe.String())
	}
}

func TestFormatServer(t *testing.T) {
	got := formatServer(service.Server{Name: "host", City: "Москва", Region: "Москва"})
	if got != "host · Москва, Москва" {
		t.Fatalf("formatServer = %q", got)
	}
}
