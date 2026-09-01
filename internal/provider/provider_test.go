package provider

import (
	"context"
	"errors"
	"math"
	"testing"
)

func TestStatsEmpty(t *testing.T) {
	got := Stats(nil)
	if got != (PingResult{}) {
		t.Errorf("Stats(nil) = %+v, want zero value", got)
	}
}

func TestStatsSingleSample(t *testing.T) {
	got := Stats([]float64{42})
	want := PingResult{ValueMs: 42, MinMs: 42, MedianMs: 42, AvgMs: 42, JitterMs: 0, Samples: 1, Method: "average"}
	if got != want {
		t.Errorf("Stats([42]) = %+v, want %+v", got, want)
	}
}

func TestStatsKnownValues(t *testing.T) {
	// min=10, avg=(10+20+15)/3=15, jitter = mean(|20-10|, |15-20|) = mean(10,5) = 7.5
	got := Stats([]float64{10, 20, 15})
	want := PingResult{ValueMs: 15, MinMs: 10, MedianMs: 15, AvgMs: 15, JitterMs: 7.5, Samples: 3, Method: "average"}
	if got != want {
		t.Errorf("Stats([10,20,15]) = %+v, want %+v", got, want)
	}
}

func TestStatsOrderIndependentMin(t *testing.T) {
	got := Stats([]float64{30, 5, 20})
	if got.MinMs != 5 {
		t.Errorf("MinMs = %v, want 5", got.MinMs)
	}
	if got.AvgMs != (30+5+20)/3.0 {
		t.Errorf("AvgMs = %v, want %v", got.AvgMs, (30+5+20)/3.0)
	}
}

func TestStatsDoesNotMutateInput(t *testing.T) {
	samples := []float64{30, 5, 20}
	orig := append([]float64(nil), samples...)
	Stats(samples)
	for i := range samples {
		if samples[i] != orig[i] {
			t.Errorf("Stats mutated input at index %d: got %v, want %v", i, samples[i], orig[i])
		}
	}
}

func TestMedianAbsoluteDeviationIsRobustToOutlier(t *testing.T) {
	got := MedianAbsoluteDeviation([]float64{10, 10.2, 80})
	if got < 0.19 || got > 0.21 {
		t.Errorf("MedianAbsoluteDeviation() = %v, want about 0.2", got)
	}
}

func TestStatsIgnoresInvalidLatencySamples(t *testing.T) {
	got := Stats([]float64{10, math.NaN(), -1, 20, math.Inf(1), 15})
	want := PingResult{ValueMs: 15, MinMs: 10, MedianMs: 15, AvgMs: 15, JitterMs: 7.5, Samples: 3, Method: "average"}
	if got != want {
		t.Errorf("Stats with invalid samples = %+v, want %+v", got, want)
	}
}

func TestStatsAllInvalidReturnsZeroResult(t *testing.T) {
	got := StatsWithMethod([]float64{math.NaN(), -1, math.Inf(1)}, "minimum")
	if got != (PingResult{}) {
		t.Errorf("StatsWithMethod with invalid samples = %+v, want zero result", got)
	}
}

func TestStatsFiniteForLargeSamples(t *testing.T) {
	got := Stats([]float64{math.MaxFloat64, math.MaxFloat64})
	if math.IsInf(got.AvgMs, 0) || math.IsNaN(got.AvgMs) || got.AvgMs != math.MaxFloat64 {
		t.Errorf("AvgMs = %v, want finite MaxFloat64", got.AvgMs)
	}
	if got.MedianMs != math.MaxFloat64 {
		t.Errorf("MedianMs = %v, want MaxFloat64", got.MedianMs)
	}
}

func TestMedianAbsoluteDeviationIgnoresInvalidSamples(t *testing.T) {
	got := MedianAbsoluteDeviation([]float64{10, math.NaN(), -3, 10.2, math.Inf(1)})
	if got < 0.09 || got > 0.11 {
		t.Errorf("MedianAbsoluteDeviation() = %v, want about 0.1", got)
	}
}

func TestNewErrorPreservesNonRetryableDecisionAndCause(t *testing.T) {
	cause := errors.New("bad response")
	err := NewError("test", "ping", CodeProtocol, false, cause)
	var operationError *OpError
	if !errors.As(err, &operationError) {
		t.Fatalf("NewError() = %T, want *OpError", err)
	}
	if operationError.Retryable {
		t.Error("Retryable = true, want caller-supplied false")
	}
	if !errors.Is(err, cause) {
		t.Error("NewError did not preserve wrapped cause")
	}
}

func TestNewErrorMakesCancellationNonRetryable(t *testing.T) {
	tests := []error{
		NewError("test", "ping", CodeCanceled, true, errors.New("canceled")),
		NewError("test", "ping", CodeUnavailable, true, context.Canceled),
	}
	for _, err := range tests {
		var operationError *OpError
		if !errors.As(err, &operationError) {
			t.Fatalf("error = %T, want *OpError", err)
		}
		if operationError.Retryable {
			t.Errorf("Retryable = true for canceled operation: %v", err)
		}
	}
}

func TestNilOpErrorUnwrap(t *testing.T) {
	var operationError *OpError
	if cause := operationError.Unwrap(); cause != nil {
		t.Errorf("nil OpError unwrap = %v, want nil", cause)
	}
}

func TestCapabilityHas(t *testing.T) {
	c := CapPing | CapDownload
	if !c.Has(CapPing) {
		t.Error("expected CapPing to be set")
	}
	if !c.Has(CapDownload) {
		t.Error("expected CapDownload to be set")
	}
	if c.Has(CapUpload) {
		t.Error("did not expect CapUpload to be set")
	}
}
