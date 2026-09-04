package service

import (
	"context"
	"errors"
	"net/http"
	"strings"
	"testing"
	"time"

	"github.com/Cheviiot/Puls/internal/measure"
)

func TestDecodeJSONLimitedRequiresOneBoundedValue(t *testing.T) {
	var result struct {
		Value int `json:"value"`
	}
	if err := DecodeJSONLimited(strings.NewReader(`{"value":7}`), 64, &result); err != nil || result.Value != 7 {
		t.Fatalf("DecodeJSONLimited() = (%+v, %v)", result, err)
	}
	for _, input := range []string{`{"value":7}{}`, strings.Repeat(" ", 65), `{`} {
		if err := DecodeJSONLimited(strings.NewReader(input), 64, &result); err == nil {
			t.Errorf("DecodeJSONLimited(%q) succeeded", input)
		}
	}
}

func TestTypedErrorClassification(t *testing.T) {
	protocolCause := errors.New("bad frame")
	authorizationCause := errors.New("bad token")
	tests := []struct {
		err  error
		code ErrorCode
	}{
		{context.Canceled, CodeCanceled},
		{context.DeadlineExceeded, CodeTimeout},
		{ProtocolError(protocolCause), CodeProtocol},
		{AuthorizationError(authorizationCause), CodeAuth},
		{&HTTPStatusError{StatusCode: http.StatusForbidden}, CodeAuth},
		{&HTTPStatusError{StatusCode: http.StatusServiceUnavailable}, CodeUnavailable},
	}
	for _, test := range tests {
		if got := ClassifyError(test.err); got != test.code {
			t.Errorf("ClassifyError(%v) = %s, want %s", test.err, got, test.code)
		}
	}
	if !errors.Is(ProtocolError(protocolCause), protocolCause) || !errors.Is(AuthorizationError(authorizationCause), authorizationCause) {
		t.Fatal("typed wrappers do not preserve their causes")
	}
}

func TestSharedMeasurementConversion(t *testing.T) {
	converted := ConvertRunResult(measure.RunResult{
		Bytes: 1_000_000, Elapsed: time.Second, WorkersOK: 2, WorkersFailed: 1,
		WorkerErrors: []error{errors.New("same"), errors.New("same")},
	})
	if converted.Mbps != 8 || converted.SuccessfulConnections != 2 || converted.FailedConnections != 1 || len(converted.Warnings) != 1 {
		t.Fatalf("ConvertRunResult() = %+v", converted)
	}
	if strings.Contains(converted.Warnings[0], "same") {
		t.Fatalf("worker diagnostics leaked into public warning: %q", converted.Warnings[0])
	}
}

func TestConnectionLimits(t *testing.T) {
	initial, maximum, err := ConnectionLimits(MeasurementConfig{Duration: 10 * time.Second, MaxConnections: 16}, 20)
	if err != nil || initial != 16 || maximum != 16 {
		t.Fatalf("ConnectionLimits() = (%d, %d, %v)", initial, maximum, err)
	}
}
