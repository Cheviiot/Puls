// Package provider defines the common interface implemented by every
// speed test backend (Yandex Internetometer, speedtest.ru, ...).
package provider

import (
	"context"
	"errors"
	"fmt"
	"math"
	"sort"
	"time"
)

// Capability flags what a Provider can actually measure.
type Capability int

const (
	CapPing Capability = 1 << iota
	CapDownload
	CapUpload
)

func (c Capability) Has(x Capability) bool { return c&x != 0 }

// PingResult is the outcome of a latency measurement.
type PingResult struct {
	// ValueMs is the provider-native latency value shown to the user. Some
	// services report the minimum RTT, others the median.
	ValueMs  float64
	MinMs    float64
	MedianMs float64
	AvgMs    float64
	JitterMs float64
	Samples  int
	Method   string
}

// MeasurementConfig controls a throughput phase. Connections=0 asks the
// provider to use its native defaults. MaxConnections is a hard safety cap.
type MeasurementConfig struct {
	Duration       time.Duration
	Connections    int
	MaxConnections int
	Verbose        func(format string, args ...any)
}

// ThroughputProgress is emitted periodically while a transfer is running.
type ThroughputProgress struct {
	Mbps              float64
	Bytes             int64
	Elapsed           time.Duration
	ActiveConnections int
}

// ThroughputResult contains only bytes confirmed by a successful HTTP or
// provider-protocol response.
type ThroughputResult struct {
	Mbps                  float64
	Bytes                 int64
	Elapsed               time.Duration
	SuccessfulConnections int
	FailedConnections     int
	Warnings              []string
}

// ErrorCode is stable enough for scripts while the wrapped error remains
// useful for humans and errors.Is/errors.As.
type ErrorCode string

const (
	CodeUnavailable ErrorCode = "unavailable"
	CodeTimeout     ErrorCode = "timeout"
	CodeProtocol    ErrorCode = "protocol"
	CodeAuth        ErrorCode = "auth"
	CodeCanceled    ErrorCode = "canceled"
	CodeInternal    ErrorCode = "internal"
)

// OpError describes which provider phase failed and whether retrying the
// operation can plausibly help.
type OpError struct {
	Provider  string
	Phase     string
	Code      ErrorCode
	Retryable bool
	Err       error
}

func (e *OpError) Error() string {
	if e == nil {
		return "<nil>"
	}
	prefix := providerDisplayName(e.Provider)
	if e.Phase != "" {
		prefix += ": " + phaseDisplayName(e.Phase)
	}
	if e.Err == nil {
		return prefix + ": " + string(e.Code)
	}
	return fmt.Sprintf("%s: %v", prefix, e.Err)
}

func providerDisplayName(name string) string {
	if name == "yandex" {
		return "Яндекс"
	}
	return name
}

func phaseDisplayName(phase string) string {
	switch phase {
	case "select":
		return "выбор сервера"
	case "ping":
		return "задержка"
	case "download":
		return "скачивание"
	case "upload":
		return "отдача"
	default:
		return phase
	}
}

func (e *OpError) Unwrap() error {
	if e == nil {
		return nil
	}
	return e.Err
}

func NewError(providerName, phase string, code ErrorCode, retryable bool, err error) error {
	// Retrying an explicit user cancellation is never useful. Keep every
	// other caller-supplied retry decision intact: the provider knows whether
	// an authentication/protocol failure is transient better than this layer.
	if code == CodeCanceled || errors.Is(err, context.Canceled) {
		retryable = false
	}
	return &OpError{Provider: providerName, Phase: phase, Code: code, Retryable: retryable, Err: err}
}

// Server describes the measurement endpoint that was selected.
type Server struct {
	Name   string
	City   string
	Region string
}

// Provider is a speed test backend.
type Provider interface {
	// Name is a short human-readable identifier, e.g. "yandex", "speedtest.ru".
	Name() string

	// Capabilities reports what this backend can measure.
	Capabilities() Capability

	// SelectServer picks (and returns) the measurement server/endpoint set
	// to be used by subsequent calls. Must be called before Ping/Download/Upload.
	SelectServer(ctx context.Context) (Server, error)

	// Ping measures latency and jitter against the selected server.
	Ping(ctx context.Context) (PingResult, error)

	// Download measures download throughput in Mbit/s over the given
	// duration using the given number of parallel connections.
	Download(ctx context.Context, cfg MeasurementConfig, progress func(ThroughputProgress)) (ThroughputResult, error)

	// Upload measures upload throughput in Mbit/s over the given
	// duration using the given number of parallel connections.
	Upload(ctx context.Context, cfg MeasurementConfig, progress func(ThroughputProgress)) (ThroughputResult, error)
}

// Stats reduces a set of round-trip-time samples (in milliseconds) to the
// min/avg/jitter summary every backend reports. Jitter is the mean absolute
// difference between consecutive samples — the same convention Ookla-style
// tools use — which makes it sensitive to actual variance, not just outliers.
func Stats(samplesMs []float64) PingResult {
	valid := validLatencySamples(samplesMs)
	if len(valid) == 0 {
		return PingResult{}
	}

	sort.Float64s(valid)
	min := valid[0]
	median := valid[len(valid)/2]
	if len(valid)%2 == 0 {
		median = midpoint(valid[len(valid)/2-1], valid[len(valid)/2])
	}

	average := 0.0
	count := 0
	for _, sample := range samplesMs {
		if !validLatency(sample) {
			continue
		}
		count++
		average += (sample - average) / float64(count)
	}

	jitter := 0.0
	jitterSamples := 0
	var previous float64
	hasPrevious := false
	for _, sample := range samplesMs {
		if !validLatency(sample) {
			continue
		}
		if hasPrevious {
			jitterSamples++
			difference := math.Abs(sample - previous)
			jitter += (difference - jitter) / float64(jitterSamples)
		}
		previous = sample
		hasPrevious = true
	}

	return PingResult{
		ValueMs:  average,
		MinMs:    min,
		MedianMs: median,
		AvgMs:    average,
		JitterMs: jitter,
		Samples:  len(valid),
		Method:   "average",
	}
}

// StatsWithMethod returns the same descriptive statistics as Stats while
// selecting the provider-native primary latency value.
func StatsWithMethod(samplesMs []float64, method string) PingResult {
	r := Stats(samplesMs)
	if r.Samples == 0 {
		return r
	}
	r.Method = method
	switch method {
	case "minimum":
		r.ValueMs = r.MinMs
	case "median":
		r.ValueMs = r.MedianMs
	default:
		r.Method = "average"
		r.ValueMs = r.AvgMs
	}
	return r
}

// MedianAbsoluteDeviation returns a robust latency-variation estimate that is
// not dominated by one queued HTTP/WebSocket response.
func MedianAbsoluteDeviation(samplesMs []float64) float64 {
	values := validLatencySamples(samplesMs)
	if len(values) < 2 {
		return 0
	}
	sort.Float64s(values)
	median := values[len(values)/2]
	if len(values)%2 == 0 {
		median = midpoint(values[len(values)/2-1], values[len(values)/2])
	}
	deviations := make([]float64, len(values))
	for index, value := range values {
		deviations[index] = math.Abs(value - median)
	}
	sort.Float64s(deviations)
	result := deviations[len(deviations)/2]
	if len(deviations)%2 == 0 {
		result = midpoint(deviations[len(deviations)/2-1], deviations[len(deviations)/2])
	}
	return result
}

func validLatencySamples(samples []float64) []float64 {
	valid := make([]float64, 0, len(samples))
	for _, sample := range samples {
		if validLatency(sample) {
			valid = append(valid, sample)
		}
	}
	return valid
}

func validLatency(sample float64) bool {
	return sample >= 0 && !math.IsInf(sample, 0) && !math.IsNaN(sample)
}

func midpoint(left, right float64) float64 {
	return left + (right-left)/2
}
