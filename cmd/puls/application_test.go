package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/netip"
	"strings"
	"testing"
	"time"

	"github.com/Cheviiot/Puls/internal/gui"
	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

type fakeBackend struct {
	id              service.ServiceID
	selectErr       error
	pingErr         error
	downloadErr     error
	uploadErr       error
	connection      service.ConnectionInfo
	connectionErr   error
	connectionCalls int
}

func (f *fakeBackend) ID() service.ServiceID { return f.id }
func (f *fakeBackend) Capabilities() service.Capability {
	return service.CapPing | service.CapDownload | service.CapUpload
}
func (f *fakeBackend) SelectServer(context.Context) (service.Server, error) {
	return service.Server{Name: "mock.example", City: "Владивосток"}, f.selectErr
}
func (f *fakeBackend) Ping(context.Context) (service.PingResult, error) {
	return service.StatsWithMethod([]float64{10, 12, 11}, "median"), f.pingErr
}
func (f *fakeBackend) Download(context.Context, service.MeasurementConfig, func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	return service.ThroughputResult{Mbps: 100, Bytes: 12_500_000, Elapsed: time.Second, SuccessfulConnections: 2, Warnings: []string{}}, f.downloadErr
}
func (f *fakeBackend) Upload(context.Context, service.MeasurementConfig, func(service.ThroughputProgress)) (service.ThroughputResult, error) {
	return service.ThroughputResult{Mbps: 50, Bytes: 6_250_000, Elapsed: time.Second, SuccessfulConnections: 1, Warnings: []string{}}, f.uploadErr
}
func (f *fakeBackend) DetectConnection(context.Context) (service.ConnectionInfo, error) {
	f.connectionCalls++
	return f.connection, f.connectionErr
}

func testApplication(stdout, stderr io.Writer, yandexBackend, speedtestBackend *fakeBackend) *application {
	return &application{
		stdout: stdout,
		stderr: stderr,
		yandexFactory: func(service.LogFunc) service.Backend {
			return yandexBackend
		},
		speedtestFactory: func(string, service.LogFunc) service.Backend {
			return speedtestBackend
		},
		selectService: func(*ui.Style) (service.ServiceID, error) { return service.Yandex, nil },
	}
}

func successfulBackends() (*fakeBackend, *fakeBackend) {
	yandexBackend := &fakeBackend{id: service.Yandex, connection: service.ConnectionInfo{ExternalIP: netip.MustParseAddr("203.0.113.8")}}
	speedtestBackend := &fakeBackend{id: service.Speedtest, connection: service.ConnectionInfo{ExternalIP: netip.MustParseAddr("203.0.113.7"), ISP: "Ростелеком"}}
	return yandexBackend, speedtestBackend
}

func TestApplicationDefaultsToYandexWithoutTTY(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	var stdout, stderr bytes.Buffer
	app := testApplication(&stdout, &stderr, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"--only", "ping", "--json"}); code != 0 {
		t.Fatalf("Run() = %d, stderr=%s", code, stderr.String())
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Status != service.StatusOK || len(result.Results) != 1 || result.Results[0].Service != service.Yandex {
		t.Fatalf("result = %+v", result)
	}
}

func TestApplicationUsesInteractiveSelectionInTTY(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	var stdout bytes.Buffer
	app := testApplication(&stdout, io.Discard, yandexBackend, speedtestBackend)
	app.inputTTY, app.outputTTY = true, true
	app.selectService = func(*ui.Style) (service.ServiceID, error) { return service.Speedtest, nil }
	if code := app.Run(context.Background(), []string{"--only", "ping"}); code != 0 {
		t.Fatalf("Run() = %d", code)
	}
	if !strings.Contains(stdout.String(), "Puls · speedtest.ru") || strings.Contains(stdout.String(), "Яндекс.Интернетометр") {
		t.Fatalf("interactive output = %q", stdout.String())
	}
}

func TestApplicationAllContinuesAfterServiceFailure(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	yandexBackend.selectErr = service.NewError(service.Yandex, service.PhaseSelect, service.CodeUnavailable, false, errors.New("недоступен"))
	var stdout, stderr bytes.Buffer
	app := testApplication(&stdout, &stderr, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"all", "--only", "ping", "--json"}); code != 1 {
		t.Fatalf("Run() = %d, want 1; stderr=%s", code, stderr.String())
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Status != service.StatusPartial || len(result.Results) != 2 || result.Results[1].Status != service.StatusOK {
		t.Fatalf("result = %+v", result)
	}
}

func TestIPUsesSpeedtestThenYandexFallback(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	speedtestBackend.connectionErr = errors.New("speedtest недоступен")
	var stdout, stderr bytes.Buffer
	app := testApplication(&stdout, &stderr, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"ip", "--json", "--verbose"}); code != 0 {
		t.Fatalf("Run() = %d, stderr=%s", code, stderr.String())
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Connection == nil || result.Connection.DetectedBy == nil || *result.Connection.DetectedBy != service.Yandex {
		t.Fatalf("connection = %+v", result.Connection)
	}
	if len(result.Connection.Warnings) == 0 || !strings.Contains(stderr.String(), "fallback") {
		t.Fatalf("fallback diagnostics missing: connection=%+v stderr=%q", result.Connection, stderr.String())
	}
}

func TestExplicitIPServiceDoesNotFallback(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	speedtestBackend.connectionErr = errors.New("speedtest недоступен")
	var stdout, stderr bytes.Buffer
	app := testApplication(&stdout, &stderr, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"ip", "speedtest", "--json"}); code != 1 {
		t.Fatalf("Run() = %d, want 1", code)
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Connection == nil || result.Connection.DetectedBy != nil || result.Connection.Status != service.StatusError {
		t.Fatalf("connection = %+v", result.Connection)
	}
}

func TestShowIPFailureDoesNotChangeMeasurementStatus(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	yandexBackend.connectionErr = errors.New("IP недоступен")
	var stdout, stderr bytes.Buffer
	app := testApplication(&stdout, &stderr, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"yandex", "--only", "ping", "--show-ip", "--json"}); code != 0 {
		t.Fatalf("Run() = %d, stderr=%s", code, stderr.String())
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Status != service.StatusOK || result.Connection == nil || result.Connection.Status != service.StatusError {
		t.Fatalf("result = %+v", result)
	}
}

func TestAllShowIPDetectsConnectionOnce(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	var stdout bytes.Buffer
	app := testApplication(&stdout, io.Discard, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"all", "--only", "ping", "--show-ip", "--json"}); code != 0 {
		t.Fatalf("Run() = %d", code)
	}
	if speedtestBackend.connectionCalls != 1 || yandexBackend.connectionCalls != 0 {
		t.Fatalf("connection calls speedtest=%d yandex=%d, want 1/0", speedtestBackend.connectionCalls, yandexBackend.connectionCalls)
	}
	var result envelope
	if err := json.Unmarshal(stdout.Bytes(), &result); err != nil {
		t.Fatal(err)
	}
	if result.Connection == nil || result.Connection.ISP == nil || *result.Connection.ISP != "Ростелеком" || len(result.Results) != 2 {
		t.Fatalf("result = %+v", result)
	}
}

func TestApplicationUsageAndCancellationExitCodes(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	app := testApplication(io.Discard, io.Discard, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"--duration", "1"}); code != 2 {
		t.Errorf("usage code = %d", code)
	}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if code := app.Run(ctx, []string{"yandex", "--json"}); code != 130 {
		t.Errorf("cancellation code = %d", code)
	}
}

func TestApplicationLaunchesGUI(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	app := testApplication(io.Discard, io.Discard, yandexBackend, speedtestBackend)
	called := false
	app.launchGUI = func(_ context.Context, options gui.Options) error {
		called = true
		if options.Version != version {
			t.Fatalf("GUI version = %q", options.Version)
		}
		return nil
	}
	if code := app.Run(context.Background(), []string{"gui"}); code != 0 || !called {
		t.Fatalf("Run(gui) = %d, called=%v", code, called)
	}
}

func TestUnavailableGUIIsUsageFailure(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	app := testApplication(io.Discard, io.Discard, yandexBackend, speedtestBackend)
	app.launchGUI = func(context.Context, gui.Options) error { return gui.ErrUnavailable }
	if code := app.Run(context.Background(), []string{"gui"}); code != 2 {
		t.Fatalf("Run(gui) = %d, want 2", code)
	}
}

type errorWriter struct{ err error }

func (w errorWriter) Write([]byte) (int, error) { return 0, w.err }

func TestJSONWriterFailureReturnsOne(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	app := testApplication(errorWriter{err: errors.New("broken pipe")}, io.Discard, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"ip", "speedtest", "--json"}); code != 1 {
		t.Fatalf("Run() = %d, want 1", code)
	}
}

func TestGoldenIPJSON(t *testing.T) {
	yandexBackend, speedtestBackend := successfulBackends()
	var stdout bytes.Buffer
	app := testApplication(&stdout, io.Discard, yandexBackend, speedtestBackend)
	if code := app.Run(context.Background(), []string{"ip", "speedtest", "--json"}); code != 0 {
		t.Fatalf("Run() = %d", code)
	}
	want := "{\n" +
		"  \"schema_version\": 1,\n" +
		"  \"command\": \"ip\",\n" +
		"  \"status\": \"ok\",\n" +
		"  \"connection\": {\n" +
		"    \"status\": \"ok\",\n" +
		"    \"external_ip\": \"203.0.113.7\",\n" +
		"    \"isp\": \"Ростелеком\",\n" +
		"    \"detected_by\": \"speedtest\",\n" +
		"    \"warnings\": [],\n" +
		"    \"error\": null\n" +
		"  },\n" +
		"  \"results\": []\n" +
		"}\n"
	if stdout.String() != want {
		t.Fatalf("IP JSON changed:\n--- got ---\n%s--- want ---\n%s", stdout.String(), want)
	}
}
