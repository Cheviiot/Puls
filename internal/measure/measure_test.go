package measure

import (
	"context"
	"sync/atomic"
	"testing"
	"time"
)

func TestMbps(t *testing.T) {
	tests := []struct {
		bytes   int64
		elapsed time.Duration
		want    float64
	}{
		{0, time.Second, 0},
		{1_000_000, time.Second, 8},
		{12_500_000, time.Second, 100},
		{6_250_000, 500 * time.Millisecond, 100},
	}
	for _, tt := range tests {
		got := Mbps(tt.bytes, tt.elapsed)
		if diff := got - tt.want; diff > 1e-9 || diff < -1e-9 {
			t.Errorf("Mbps(%d, %v) = %v, want %v", tt.bytes, tt.elapsed, got, tt.want)
		}
	}
}

func TestMbpsGuardsZeroAndNegativeElapsed(t *testing.T) {
	if got := Mbps(1_000_000, 0); got != 0 {
		t.Errorf("Mbps with zero elapsed = %v, want 0", got)
	}
	if got := Mbps(1_000_000, -time.Second); got != 0 {
		t.Errorf("Mbps with negative elapsed = %v, want 0", got)
	}
	if got := Mbps(-1, time.Second); got != 0 {
		t.Errorf("Mbps with negative bytes = %v, want 0", got)
	}
}

func TestEmaDampsSpikes(t *testing.T) {
	prev := 50.0
	got := ema(prev, 500.0)
	if got >= prev+(500-prev)*emaAlpha+1e-9 {
		t.Errorf("ema(50, 500) = %v, moved further than alpha allows", got)
	}
	if got <= prev {
		t.Errorf("ema(50, 500) = %v, should have moved up from %v toward the spike", got, prev)
	}
}

func TestEmaConvergesToSteadyValue(t *testing.T) {
	smoothed := 0.0
	for i := 0; i < 100; i++ {
		smoothed = ema(smoothed, 100)
	}
	if diff := smoothed - 100; diff > 0.01 || diff < -0.01 {
		t.Errorf("ema did not converge: got %v, want ~100", smoothed)
	}
}

func TestProgressStopTerminatesSamplerWithoutCancelingParent(t *testing.T) {
	var counter int64
	stop := Progress(context.Background(), &counter, func(float64) {})
	atomic.AddInt64(&counter, 1024)

	returned := make(chan time.Duration, 1)
	go func() { returned <- stop() }()
	var first time.Duration
	select {
	case first = <-returned:
	case <-time.After(250 * time.Millisecond):
		t.Fatal("Progress stop did not terminate the sampler promptly")
	}
	if first < 0 || first > 250*time.Millisecond {
		t.Errorf("Progress elapsed = %s, want a prompt non-negative duration", first)
	}
	if second := stop(); second != first {
		t.Errorf("second Progress stop = %s, want idempotent result %s", second, first)
	}
}

func TestProgressStopsAfterParentCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	var counter int64
	stop := Progress(ctx, &counter, func(float64) {})
	cancel()

	returned := make(chan time.Duration, 1)
	go func() { returned <- stop() }()
	select {
	case <-returned:
	case <-time.After(250 * time.Millisecond):
		t.Fatal("Progress sampler did not stop after parent cancellation")
	}
}

func TestProgressWithoutReporterHasIdempotentStop(t *testing.T) {
	var counter int64
	stop := Progress(context.Background(), &counter, nil)
	first := stop()
	if second := stop(); second != first {
		t.Errorf("second Progress stop = %s, want %s", second, first)
	}
}

func TestProgressTreatsNilCounterAsZero(t *testing.T) {
	reported := make(chan float64, 1)
	stop := Progress(context.Background(), nil, func(value float64) {
		select {
		case reported <- value:
		default:
		}
	})
	defer stop()
	select {
	case value := <-reported:
		if value != 0 {
			t.Errorf("reported Mbps = %v, want 0", value)
		}
	case <-time.After(300 * time.Millisecond):
		t.Fatal("Progress did not report its first sample")
	}
}
