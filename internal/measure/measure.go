// Package measure holds throughput-measurement helpers shared by every
// download/upload-capable provider: converting a byte count to Mbit/s, and
// reporting a live rate that stays readable instead of jumping around.
package measure

import (
	"context"
	"sync"
	"sync/atomic"
	"time"
)

// Mbps converts a byte count over a duration into megabits per second.
func Mbps(bytes int64, elapsed time.Duration) float64 {
	if bytes <= 0 || elapsed <= 0 {
		return 0
	}
	return float64(bytes) * 8 / 1e6 / elapsed.Seconds()
}

// emaAlpha controls how reactive the live progress display is. Raw
// per-tick throughput is noisy — the network delivers data in bursts and
// each finished chunk/request has a brief gap before the next one starts —
// so displaying it unsmoothed makes the number jump around (e.g. spiking to
// several times the real speed for one tick). Exponential smoothing damps
// that out while still catching up to a genuine speed change within about
// a second; it never affects the final reported average, which callers
// should always compute from total bytes over the whole test.
const emaAlpha = 0.2

func ema(prev, instant float64) float64 {
	return emaAlpha*instant + (1-emaAlpha)*prev
}

// Progress samples counter every 200ms and calls report with an
// exponentially smoothed Mbit/s rate (see emaAlpha). The returned function
// stops the sampler, waits for its goroutine to exit, and returns the elapsed
// wall-clock time. It is safe to call the stop function more than once.
func Progress(ctx context.Context, counter *int64, report func(mbps float64)) func() time.Duration {
	start := time.Now()
	if report == nil {
		var once sync.Once
		var elapsed time.Duration
		return func() time.Duration {
			once.Do(func() { elapsed = time.Since(start) })
			return elapsed
		}
	}

	progressCtx, cancel := context.WithCancel(ctx)
	done := make(chan struct{})
	var finishOnce sync.Once
	var elapsed time.Duration
	finish := func() {
		finishOnce.Do(func() {
			elapsed = time.Since(start)
			cancel()
		})
	}
	go func() {
		ticker := time.NewTicker(200 * time.Millisecond)
		defer ticker.Stop()
		defer close(done)
		defer finish()
		var lastBytes int64
		lastT := start
		smoothed := 0.0
		hasSample := false
		for {
			select {
			case <-progressCtx.Done():
				return
			case now := <-ticker.C:
				var b int64
				if counter != nil {
					b = atomic.LoadInt64(counter)
				}
				dt := now.Sub(lastT)
				instant := Mbps(b-lastBytes, dt)
				if hasSample {
					smoothed = ema(smoothed, instant)
				} else {
					smoothed = instant
					hasSample = true
				}
				report(smoothed)
				lastBytes, lastT = b, now
			}
		}
	}()
	return func() time.Duration {
		finish()
		<-done
		return elapsed
	}
}
