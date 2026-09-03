package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"strings"
	"testing"
	"time"

	"github.com/Cheviiot/Puls/internal/provider"
	"github.com/Cheviiot/Puls/internal/ui"
)

func TestRound2(t *testing.T) {
	tests := map[float64]float64{
		1.006:   1.01,
		1.004:   1.0,
		87.5678: 87.57,
		0:       0,
	}
	for in, want := range tests {
		if got := round2(in); got != want {
			t.Errorf("round2(%v) = %v, want %v", in, got, want)
		}
	}
}

func TestFormatServer(t *testing.T) {
	got := formatServer(provider.Server{Name: "host.example"})
	if got != "host.example" {
		t.Errorf("formatServer(no city) = %q, want %q", got, "host.example")
	}
	got = formatServer(provider.Server{Name: "host.example", City: "Москва", Region: "Московская область"})
	want := "host.example (Москва, Московская область)"
	if got != want {
		t.Errorf("formatServer(with city+region) = %q, want %q", got, want)
	}

	got = formatServer(provider.Server{Name: "host.example", City: "Рига"})
	want = "host.example (Рига)"
	if got != want {
		t.Errorf("formatServer(city, no region) = %q, want %q", got, want)
	}
}

func TestWithRetrySucceedsAfterFailures(t *testing.T) {
	calls := 0
	err := withRetryBackoff(context.Background(), 3, 0, func() error {
		calls++
		if calls < 3 {
			return errors.New("transient")
		}
		return nil
	})
	if err != nil {
		t.Errorf("withRetry() = %v, want nil", err)
	}
	if calls != 3 {
		t.Errorf("fn called %d times, want 3", calls)
	}
}

func TestWithRetryGivesUpAfterAttempts(t *testing.T) {
	calls := 0
	wantErr := errors.New("persistent")
	err := withRetryBackoff(context.Background(), 2, 0, func() error {
		calls++
		return wantErr
	})
	if !errors.Is(err, wantErr) {
		t.Errorf("withRetry() = %v, want %v", err, wantErr)
	}
	if calls != 2 {
		t.Errorf("fn called %d times, want 2 (attempts limit)", calls)
	}
}

func TestWithRetryStopsOnCanceledContext(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel() // already canceled before the first attempt

	calls := 0
	err := withRetry(ctx, 5, func() error {
		calls++
		return errors.New("would retry forever otherwise")
	})
	if err == nil {
		t.Error("withRetry() = nil, want an error")
	}
	if calls != 0 {
		t.Errorf("fn called %d times, want 0 (do not start work once ctx is canceled)", calls)
	}
}

func TestWithRetryDoesNotRepeatPermanentProviderError(t *testing.T) {
	calls := 0
	wantErr := provider.NewError("fake", "ping", provider.CodeProtocol, false, errors.New("broken response"))
	err := withRetry(context.Background(), 5, func() error {
		calls++
		return wantErr
	})
	if !errors.Is(err, wantErr) {
		t.Fatalf("withRetry() = %v, want %v", err, wantErr)
	}
	if calls != 1 {
		t.Fatalf("permanent error was attempted %d times, want 1", calls)
	}
}

func TestWithRetryReturnsCancellationDuringBackoff(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	go func() {
		time.Sleep(10 * time.Millisecond)
		cancel()
	}()

	err := withRetry(ctx, 5, func() error { return errors.New("temporary failure") })
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("withRetry() = %v, want context.Canceled", err)
	}
}

func TestRunVersionFlag(t *testing.T) {
	if code := run([]string{"--version"}); code != 0 {
		t.Errorf("run([--version]) = %d, want 0", code)
	}
}

func TestRunHelpFlagExitsZero(t *testing.T) {
	if code := run([]string{"--help"}); code != 0 {
		t.Errorf("run([--help]) = %d, want 0", code)
	}
}

func TestSplitCommand(t *testing.T) {
	providerKey, args := splitCommand([]string{"speedtest", "--profile", "quick"})
	if providerKey != "speedtest" || strings.Join(args, " ") != "--profile quick" {
		t.Fatalf("splitCommand() = (%q, %v)", providerKey, args)
	}

	providerKey, args = splitCommand([]string{"help"})
	if providerKey != "" || len(args) != 1 || args[0] != "--help" {
		t.Fatalf("splitCommand(help) = (%q, %v)", providerKey, args)
	}
}

func TestHelpHasClearStructureAndExamples(t *testing.T) {
	var output bytes.Buffer
	printHelp(&output, ui.NewStyle(false))
	text := output.String()
	for _, want := range []string{
		"Puls  точная проверка скорости интернета",
		"ИСПОЛЬЗОВАНИЕ",
		"ИСТОЧНИКИ",
		"РЕЖИМЫ",
		"ПАРАМЕТРЫ ПРОВЕРКИ",
		"ВЫВОД И ДИАГНОСТИКА",
		"ПРИМЕРЫ",
		"puls all --profile quick",
	} {
		if !strings.Contains(text, want) {
			t.Errorf("help does not contain %q:\n%s", want, text)
		}
	}
	for _, russianFlag := range []string{"--режим", "--длительность", "--потоки", "--только", "--сервер", "--подробно", "--без-цвета", "--версия", "--справка"} {
		if strings.Contains(text, russianFlag) {
			t.Errorf("справка содержит нестандартный русскоязычный параметр %q:\n%s", russianFlag, text)
		}
	}
}

func TestRunRejectsInvalidFlags(t *testing.T) {
	cases := [][]string{
		{"--duration=0"},
		{"--duration=2"},
		{"--duration=-1"},
		{"--duration=61"},
		{"--connections=-1"},
		{"--connections=17"},
		{"--profile=unknown"},
		{"--only=unknown"},
		{"unknown"},
		{"yandex", "--server=example.com"},
	}
	for _, args := range cases {
		if code := run(args); code != 2 {
			t.Errorf("run(%v) = %d, want 2", args, code)
		}
	}
}

type fakeProvider struct {
	pingErr     error
	downloadErr error
	uploadErr   error
}

type errorWriter struct{ err error }

func (w errorWriter) Write([]byte) (int, error) { return 0, w.err }

func (f fakeProvider) Name() string { return "fake" }
func (f fakeProvider) Capabilities() provider.Capability {
	return provider.CapPing | provider.CapDownload | provider.CapUpload
}
func (f fakeProvider) SelectServer(context.Context) (provider.Server, error) {
	return provider.Server{Name: "mock.example", City: "Тест"}, nil
}
func (f fakeProvider) Ping(context.Context) (provider.PingResult, error) {
	if f.pingErr != nil {
		return provider.PingResult{}, f.pingErr
	}
	return provider.StatsWithMethod([]float64{10, 12, 11}, "median"), nil
}
func (f fakeProvider) Download(context.Context, provider.MeasurementConfig, func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	return provider.ThroughputResult{Mbps: 100, Bytes: 12_500_000, Elapsed: time.Second, SuccessfulConnections: 2}, f.downloadErr
}
func (f fakeProvider) Upload(context.Context, provider.MeasurementConfig, func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
	return provider.ThroughputResult{Mbps: 50, Bytes: 6_250_000, Elapsed: time.Second, SuccessfulConnections: 1}, f.uploadErr
}

func TestRunProviderPartialResultAndHumanOutput(t *testing.T) {
	var output bytes.Buffer
	result := runProvider(context.Background(), fakeProvider{pingErr: errors.New("ping failed")}, &output, ui.NewStyle(false), false,
		provider.MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}, true, true, true, false)
	if result.Status != "partial" || result.PingMs != nil || result.Download == nil || result.Upload == nil {
		t.Fatalf("result = %+v, want partial with null ping and successful throughput", result)
	}
	for _, text := range []string{"● fake", "mock.example", "не удалось", "100.00", "50.00", "Частичный результат"} {
		if !strings.Contains(output.String(), text) {
			t.Errorf("human output does not contain %q:\n%s", text, output.String())
		}
	}
}

type fakeIPProvider struct {
	fakeProvider
	ip    string
	ipErr error
}

func (f fakeIPProvider) DetectExternalIP(context.Context) (string, error) {
	if f.ipErr != nil {
		return "", f.ipErr
	}
	return f.ip, nil
}

func TestRunProviderShowIPDetectsAddress(t *testing.T) {
	var output bytes.Buffer
	result := runProvider(context.Background(), fakeIPProvider{ip: "203.0.113.7"}, &output, ui.NewStyle(false), false,
		provider.MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}, true, true, true, true)
	if result.ExternalIP == nil || *result.ExternalIP != "203.0.113.7" {
		t.Fatalf("result.ExternalIP = %v, want \"203.0.113.7\"", result.ExternalIP)
	}
	if !strings.Contains(output.String(), "203.0.113.7") {
		t.Errorf("human output does not contain detected IP:\n%s", output.String())
	}
}

func TestRunProviderShowIPHandlesDetectionFailure(t *testing.T) {
	var output bytes.Buffer
	result := runProvider(context.Background(), fakeIPProvider{ipErr: errors.New("boom")}, &output, ui.NewStyle(false), false,
		provider.MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}, true, true, true, true)
	if result.ExternalIP != nil {
		t.Fatalf("result.ExternalIP = %v, want nil after detection failure", result.ExternalIP)
	}
	if result.Status != "ok" {
		t.Errorf("result.Status = %q, want \"ok\" — IP detection failure must not affect measurement status", result.Status)
	}
	if !strings.Contains(output.String(), "недоступно") {
		t.Errorf("human output does not report unavailable IP:\n%s", output.String())
	}
}

func TestRunProviderShowIPUnsupportedByProvider(t *testing.T) {
	var output bytes.Buffer
	result := runProvider(context.Background(), fakeProvider{}, &output, ui.NewStyle(false), false,
		provider.MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}, true, true, true, true)
	if result.ExternalIP != nil {
		t.Fatalf("result.ExternalIP = %v, want nil for a provider without IP detection", result.ExternalIP)
	}
	if !strings.Contains(output.String(), "не поддерживается") {
		t.Errorf("human output does not report unsupported IP detection:\n%s", output.String())
	}
}

func TestJSONKeepsMissingMeasurementsAsNull(t *testing.T) {
	result := newRunResult("fake", true, true, true)
	result.Status = "error"
	encoded, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	text := string(encoded)
	for _, field := range []string{`"ping_ms":null`, `"jitter_ms":null`, `"download_mbps":null`, `"upload_mbps":null`, `"external_ip":null`} {
		if !strings.Contains(text, field) {
			t.Errorf("JSON missing %s: %s", field, text)
		}
	}
	for _, field := range []string{`"value_ms":null`, `"jitter_ms":null`, `"mbps":null`, `"elapsed_ms":null`} {
		if !strings.Contains(text, field) {
			t.Errorf("JSON phase missing %s: %s", field, text)
		}
	}
}

func TestWriteJSONUsesObjectForOneResultAndArrayForMany(t *testing.T) {
	result := newRunResult("fake", true, true, true)
	var output bytes.Buffer
	if err := writeJSON(&output, []runResult{result}); err != nil {
		t.Fatal(err)
	}
	if first := strings.TrimSpace(output.String())[0]; first != '{' {
		t.Fatalf("single result starts with %q, want object", first)
	}

	output.Reset()
	if err := writeJSON(&output, []runResult{result, result}); err != nil {
		t.Fatal(err)
	}
	if first := strings.TrimSpace(output.String())[0]; first != '[' {
		t.Fatalf("multiple results start with %q, want array", first)
	}
}

func TestWriteJSONReturnsWriterError(t *testing.T) {
	wantErr := errors.New("broken pipe")
	err := writeJSON(errorWriter{err: wantErr}, []runResult{newRunResult("fake", true, true, true)})
	if !errors.Is(err, wantErr) {
		t.Fatalf("writeJSON() = %v, want %v", err, wantErr)
	}
}

func TestThroughputErrorDoesNotExposeZeroAsMeasurement(t *testing.T) {
	phase := throughputPhase(provider.ThroughputResult{Bytes: 1024, Elapsed: time.Second}, errors.New("offline"))
	if phase.Status != "error" {
		t.Fatalf("status = %q, want error", phase.Status)
	}
	if phase.Mbps != nil {
		t.Fatalf("mbps = %v, want nil for failed measurement", *phase.Mbps)
	}
	if phase.Bytes != 1024 || phase.ElapsedMs == nil {
		t.Fatalf("diagnostics were lost: %+v", phase)
	}

	phase = throughputPhase(provider.ThroughputResult{}, errors.New("offline before measurement"))
	if phase.ElapsedMs != nil {
		t.Fatalf("elapsed_ms = %v, want nil before measurement", *phase.ElapsedMs)
	}
}

func TestAppendUniqueWarningsPreservesOrder(t *testing.T) {
	warnings := appendUniqueWarnings([]string{"первая"}, []string{"context deadline exceeded", "первая", "connection refused"})
	want := []string{"первая", "истекло время ожидания", "в соединении отказано"}
	if len(warnings) != len(want) {
		t.Fatalf("warnings = %v, want %v", warnings, want)
	}
	for index := range want {
		if warnings[index] != want[index] {
			t.Fatalf("warnings = %v, want %v", warnings, want)
		}
	}
}

func TestExitCodes(t *testing.T) {
	if got := exitCode(nil, []runResult{{Status: "ok"}}, nil); got != 0 {
		t.Errorf("success exit code = %d", got)
	}
	if got := exitCode(nil, []runResult{{Status: "partial"}}, nil); got != 1 {
		t.Errorf("partial exit code = %d", got)
	}
	if got := exitCode(nil, nil, errors.New("broken pipe")); got != 1 {
		t.Errorf("JSON write error exit code = %d", got)
	}
	if got := exitCode(context.Canceled, nil, nil); got != 130 {
		t.Errorf("canceled exit code = %d", got)
	}
}
