package measure

import (
	"context"
	"errors"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestRunStartsClockAfterReadyAndExcludesWarmup(t *testing.T) {
	started := time.Now()
	result, err := Run(context.Background(), RunConfig{
		Duration: 60 * time.Millisecond, Warmup: 70 * time.Millisecond,
		StartupTimeout: time.Second, ReadyGrace: 5 * time.Millisecond,
		InitialWorkers: 1, MaxWorkers: 1,
	}, func(ctx context.Context, _ int, ready func(), record func(int64)) error {
		time.Sleep(50 * time.Millisecond)
		ready()
		record(111)
		time.Sleep(40 * time.Millisecond)
		record(222)
		time.Sleep(50 * time.Millisecond)
		record(333)
		<-ctx.Done()
		return ctx.Err()
	}, nil)
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if result.Bytes != 333 {
		t.Fatalf("measured bytes = %d, want only the post-warmup 333 bytes", result.Bytes)
	}
	if result.Elapsed < 50*time.Millisecond || result.Elapsed > 130*time.Millisecond {
		t.Errorf("measured elapsed = %s, want approximately the 60ms phase", result.Elapsed)
	}
	if wall := time.Since(started); wall < 170*time.Millisecond {
		t.Errorf("wall time = %s: startup and warm-up were incorrectly included in measurement clock", wall)
	}
}

func TestRunFailsEarlyWhenAllWorkersDie(t *testing.T) {
	started := time.Now()
	_, err := Run(context.Background(), RunConfig{
		Duration: time.Second, StartupTimeout: 2 * time.Second,
		ReadyGrace: 5 * time.Millisecond, InitialWorkers: 2, MaxWorkers: 2,
	}, func(context.Context, int, func(), func(int64)) error {
		return errors.New("dial failed")
	}, nil)
	if err == nil {
		t.Fatal("Run() error = nil, want all-workers failure")
	}
	if elapsed := time.Since(started); elapsed > 300*time.Millisecond {
		t.Errorf("all-workers failure took %s, want early termination", elapsed)
	}
}

func TestRunReconnectsOnceAndCountsConfirmedBytes(t *testing.T) {
	var attempts atomic.Int32
	result, err := Run(context.Background(), RunConfig{
		Duration: 350 * time.Millisecond, StartupTimeout: time.Second,
		ReadyGrace: 5 * time.Millisecond, InitialWorkers: 1, MaxWorkers: 1, Reconnects: 1,
	}, func(ctx context.Context, _ int, ready func(), record func(int64)) error {
		attempt := attempts.Add(1)
		ready()
		if attempt == 1 {
			return errors.New("connection reset")
		}
		record(4096)
		<-ctx.Done()
		return ctx.Err()
	}, nil)
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if attempts.Load() != 2 {
		t.Errorf("worker attempts = %d, want 2", attempts.Load())
	}
	if result.Bytes != 4096 || result.WorkersOK != 1 {
		t.Errorf("result = %+v, want one successful stream with 4096 bytes", result)
	}
	if len(result.WorkerErrors) == 0 {
		t.Error("transient reconnect error was not retained as a warning")
	}
}

func TestRunCancellationReturnsPromptly(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	time.AfterFunc(30*time.Millisecond, cancel)
	started := time.Now()
	_, err := Run(ctx, RunConfig{
		Duration: 10 * time.Second, StartupTimeout: time.Second,
		ReadyGrace: 5 * time.Millisecond, InitialWorkers: 1, MaxWorkers: 1,
	}, func(ctx context.Context, _ int, ready func(), _ func(int64)) error {
		ready()
		<-ctx.Done()
		return ctx.Err()
	}, nil)
	if !errors.Is(err, context.Canceled) {
		t.Fatalf("Run() error = %v, want context.Canceled", err)
	}
	if elapsed := time.Since(started); elapsed > time.Second {
		t.Errorf("cancellation took %s, want <= 1s", elapsed)
	}
}

func TestRunReturnsPartialStreamDiagnostics(t *testing.T) {
	result, err := Run(context.Background(), RunConfig{
		Duration: 80 * time.Millisecond, StartupTimeout: time.Second,
		ReadyGrace: 5 * time.Millisecond, InitialWorkers: 2, MaxWorkers: 2,
	}, func(ctx context.Context, index int, ready func(), record func(int64)) error {
		ready()
		if index == 1 {
			return errors.New("one stream failed")
		}
		time.Sleep(20 * time.Millisecond)
		record(2048)
		<-ctx.Done()
		return ctx.Err()
	}, nil)
	if err != nil {
		t.Fatalf("Run() error = %v, want partial stream success", err)
	}
	if result.Bytes != 2048 || result.WorkersOK != 1 || result.WorkersFailed != 1 || len(result.WorkerErrors) != 1 {
		t.Errorf("result = %+v, want 1 successful and 1 failed stream", result)
	}
}

func TestRunAggregatesStartupWorkerErrors(t *testing.T) {
	firstErr := errors.New("first dial failed")
	secondErr := errors.New("second dial failed")
	result, err := Run(context.Background(), RunConfig{
		Duration: time.Second, StartupTimeout: time.Second,
		ReadyGrace: time.Millisecond, InitialWorkers: 2, MaxWorkers: 2,
	}, func(_ context.Context, index int, _ func(), _ func(int64)) error {
		if index == 0 {
			return firstErr
		}
		return secondErr
	}, nil)
	if !errors.Is(err, firstErr) || !errors.Is(err, secondErr) {
		t.Fatalf("Run() error = %v, want both worker causes", err)
	}
	if result.WorkersFailed != 2 || result.WorkersOK != 0 || len(result.WorkerErrors) != 2 {
		t.Errorf("result = %+v, want diagnostics for both failed workers", result)
	}
}

func TestRunRecoversWorkerPanic(t *testing.T) {
	result, err := Run(context.Background(), RunConfig{
		Duration: time.Second, StartupTimeout: time.Second,
		ReadyGrace: time.Millisecond, InitialWorkers: 1, MaxWorkers: 1,
	}, func(context.Context, int, func(), func(int64)) error {
		panic("broken worker")
	}, nil)
	if err == nil || !strings.Contains(err.Error(), "broken worker") {
		t.Fatalf("Run() error = %v, want recovered worker panic", err)
	}
	if result.WorkersFailed != 1 || len(result.WorkerErrors) != 1 {
		t.Errorf("result = %+v, want failed worker diagnostics", result)
	}
}

func TestRunReadyIsIdempotentUnderConcurrentCalls(t *testing.T) {
	var maximumActive atomic.Int32
	result, err := Run(context.Background(), RunConfig{
		Duration: 260 * time.Millisecond, StartupTimeout: time.Second,
		ReadyGrace: time.Millisecond, InitialWorkers: 1, MaxWorkers: 1,
	}, func(ctx context.Context, _ int, ready func(), record func(int64)) error {
		var callers sync.WaitGroup
		callers.Add(32)
		for range 32 {
			go func() {
				defer callers.Done()
				ready()
			}()
		}
		callers.Wait()
		ticker := time.NewTicker(5 * time.Millisecond)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				record(1024)
			case <-ctx.Done():
				return ctx.Err()
			}
		}
	}, func(progress RunProgress) {
		for {
			old := maximumActive.Load()
			if int32(progress.Active) <= old || maximumActive.CompareAndSwap(old, int32(progress.Active)) {
				break
			}
		}
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	if result.Bytes == 0 || result.WorkersOK != 1 {
		t.Errorf("result = %+v, want exactly one successful worker with confirmed bytes", result)
	}
	if got := maximumActive.Load(); got != 1 {
		t.Errorf("maximum active workers = %d, want 1", got)
	}
}

func TestRunCancellationInterruptsReconnectBackoff(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	firstAttempt := make(chan struct{})
	var attempts atomic.Int32
	resultCh := make(chan error, 1)
	go func() {
		_, err := Run(ctx, RunConfig{
			Duration: time.Second, StartupTimeout: time.Second,
			ReadyGrace: time.Millisecond, InitialWorkers: 1, MaxWorkers: 1, Reconnects: 1,
		}, func(context.Context, int, func(), func(int64)) error {
			attempts.Add(1)
			close(firstAttempt)
			return errors.New("connection reset")
		}, nil)
		resultCh <- err
	}()
	<-firstAttempt
	cancel()
	select {
	case err := <-resultCh:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("Run() error = %v, want context.Canceled", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run did not interrupt reconnect backoff within one second")
	}
	if got := attempts.Load(); got != 1 {
		t.Errorf("worker attempts = %d, want no attempt after cancellation", got)
	}
}

func TestRunRejectsUnsafeConfigurationBeforeStartingWorker(t *testing.T) {
	tests := []RunConfig{
		{Duration: time.Second, InitialWorkers: 17, MaxWorkers: 17},
		{Duration: time.Second, InitialWorkers: 1, MaxWorkers: 17},
		{Duration: time.Second, InitialWorkers: 1, MaxWorkers: 1, Reconnects: -1},
		{Duration: time.Second, InitialWorkers: 1, MaxWorkers: 1, Reconnects: 2},
	}
	for _, cfg := range tests {
		called := false
		_, err := Run(context.Background(), cfg, func(context.Context, int, func(), func(int64)) error {
			called = true
			return nil
		}, nil)
		if err == nil {
			t.Errorf("Run(%+v) error = nil, want validation error", cfg)
		}
		if called {
			t.Errorf("Run(%+v) started worker before validating configuration", cfg)
		}
	}

	if _, err := Run(context.Background(), RunConfig{Duration: time.Second}, nil, nil); err == nil {
		t.Error("Run with nil worker error = nil, want validation error")
	}
}

func TestRunWaitsForEveryWorkerOnCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	var started atomic.Int32
	var exited atomic.Int32
	allStarted := make(chan struct{})
	resultCh := make(chan error, 1)
	go func() {
		_, err := Run(ctx, RunConfig{
			Duration: 10 * time.Second, StartupTimeout: time.Second,
			ReadyGrace: 5 * time.Millisecond, InitialWorkers: 16, MaxWorkers: 16,
		}, func(ctx context.Context, _ int, ready func(), _ func(int64)) error {
			defer exited.Add(1)
			ready()
			if started.Add(1) == 16 {
				close(allStarted)
			}
			<-ctx.Done()
			return ctx.Err()
		}, nil)
		resultCh <- err
	}()
	<-allStarted
	cancel()
	select {
	case err := <-resultCh:
		if !errors.Is(err, context.Canceled) {
			t.Fatalf("Run() error = %v, want context.Canceled", err)
		}
	case <-time.After(time.Second):
		t.Fatal("Run did not return within one second of cancellation")
	}
	if got := exited.Load(); got != 16 {
		t.Errorf("workers exited before Run returned = %d, want 16", got)
	}
}

func TestRunContainsWorkerErrorCallbackPanic(t *testing.T) {
	result, err := Run(context.Background(), RunConfig{
		Duration: time.Second, StartupTimeout: time.Second,
		ReadyGrace: time.Millisecond, InitialWorkers: 1, MaxWorkers: 1,
		OnWorkerError: func(int, int, error) {
			panic("broken callback")
		},
	}, func(context.Context, int, func(), func(int64)) error {
		return errors.New("dial failed")
	}, nil)
	if err == nil || !strings.Contains(err.Error(), "broken callback") {
		t.Fatalf("Run() error = %v, want recovered callback panic", err)
	}
	if result.WorkersFailed != 1 || len(result.WorkerErrors) != 2 {
		t.Errorf("result = %+v, want worker and callback diagnostics", result)
	}
}

func TestRunCountsConcurrentConfirmedBytesExactly(t *testing.T) {
	const workers = 8
	release := make(chan struct{})
	var releaseOnce sync.Once
	result, err := Run(context.Background(), RunConfig{
		Duration: 280 * time.Millisecond, StartupTimeout: time.Second,
		ReadyGrace: 5 * time.Millisecond, InitialWorkers: workers, MaxWorkers: workers,
	}, func(ctx context.Context, index int, ready func(), record func(int64)) error {
		ready()
		<-release
		record(-1)
		record(0)
		record(int64(index+1) * 1000)
		<-ctx.Done()
		return ctx.Err()
	}, func(RunProgress) {
		releaseOnce.Do(func() { close(release) })
	})
	if err != nil {
		t.Fatalf("Run() error = %v", err)
	}
	const wantBytes = 36_000
	if result.Bytes != wantBytes {
		t.Errorf("measured bytes = %d, want exactly %d", result.Bytes, wantBytes)
	}
	if result.WorkersOK != workers || result.WorkersFailed != 0 {
		t.Errorf("result = %+v, want all %d workers successful", result, workers)
	}
}
