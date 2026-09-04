package main

import (
	"bytes"
	"context"
	"encoding/json"
	"strings"
	"testing"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestJSONEnvelopeKeepsNullsAndEmptyArrays(t *testing.T) {
	result := newEnvelope(commandMeasure)
	measurement := newMeasurementResult(service.Yandex, phasePing)
	measurement.Status = service.StatusError
	result.Status = service.StatusError
	result.Results = append(result.Results, measurement)
	var output bytes.Buffer
	if err := writeJSON(&output, result); err != nil {
		t.Fatal(err)
	}
	text := output.String()
	for _, expected := range []string{
		`"schema_version": 1`, `"command": "measure"`, `"connection": null`,
		`"server": null`, `"value_ms": null`, `"mbps": null`,
		`"warnings": []`, `"error": null`,
	} {
		if !strings.Contains(text, expected) {
			t.Errorf("JSON missing %s:\n%s", expected, text)
		}
	}
	var decoded map[string]any
	if err := json.Unmarshal(output.Bytes(), &decoded); err != nil {
		t.Fatal(err)
	}
	if _, exists := decoded["provider"]; exists {
		t.Fatal("legacy provider field is present")
	}
}

func TestThroughputFailureNeverReportsSuccessfulZero(t *testing.T) {
	phase := throughputPhase(service.ThroughputResult{Bytes: 1024}, assertError("offline"))
	if phase.Status != service.StatusError || phase.Mbps != nil || phase.Bytes == nil || *phase.Bytes != 1024 {
		t.Fatalf("phase = %+v", phase)
	}
}

func TestThroughputFailureBeforeTransferUsesNullDiagnostics(t *testing.T) {
	phase := throughputPhase(service.ThroughputResult{}, assertError("offline"))
	if phase.Mbps != nil || phase.Bytes != nil || phase.ElapsedMs != nil || phase.SuccessfulStreams != nil || phase.FailedStreams != nil {
		t.Fatalf("phase = %+v, want null throughput values", phase)
	}
}

func TestHumanErrorKeepsProtocolDetailsOutOfNormalOutput(t *testing.T) {
	err := service.NewError(service.Speedtest, service.PhaseUpload, service.CodeUnavailable, true,
		assertError(`Post "https://example.test/upload.php": timeout`))
	if got := humanError(err); got != "сервис временно недоступен" || strings.Contains(got, "https://") {
		t.Fatalf("humanError() = %q", got)
	}
	if got := humanError(context.DeadlineExceeded); got != "истекло время ожидания" {
		t.Fatalf("deadline humanError() = %q", got)
	}
}

type assertError string

func (e assertError) Error() string { return string(e) }

func TestRound2(t *testing.T) {
	if got := round2(87.5678); got != 87.57 {
		t.Fatalf("round2 = %v", got)
	}
}
