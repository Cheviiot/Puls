package measure

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"time"
)

// RunConfig controls a group of long-lived transfer workers.
type RunConfig struct {
	Duration       time.Duration
	Warmup         time.Duration
	StartupTimeout time.Duration
	ReadyGrace     time.Duration
	InitialWorkers int
	MaxWorkers     int
	Reconnects     int
	Adaptive       func(warmupMbps float64) int
	OnWorkerError  func(index, attempt int, err error)
}

// RunProgress describes confirmed transfer progress.
type RunProgress struct {
	Mbps    float64
	Bytes   int64
	Elapsed time.Duration
	Active  int
}

// RunResult summarizes a transfer. WorkerErrors are non-fatal when at least
// one worker transferred confirmed data.
type RunResult struct {
	Bytes         int64
	Elapsed       time.Duration
	WorkersOK     int
	WorkersFailed int
	WorkerErrors  []error
}

// Worker must call ready after the connection and protocol handshake are
// usable. record adds only bytes confirmed by the remote protocol.
type Worker func(ctx context.Context, index int, ready func(), record func(int64)) error

type workerState struct {
	measured atomic.Int64
	ready    atomic.Bool
	errors   []error
}

const maxRunWorkers = 16

// Run starts a bounded group of workers, waits for at least one to become
// ready, performs an optional warm-up, and only then starts the measurement
// clock. It fails early when every worker exits.
func Run(ctx context.Context, cfg RunConfig, worker Worker, report func(RunProgress)) (RunResult, error) {
	if cfg.Duration <= 0 {
		return RunResult{}, errors.New("длительность проверки должна быть положительной")
	}
	if worker == nil {
		return RunResult{}, errors.New("обработчик потока не задан")
	}
	if cfg.InitialWorkers < 1 {
		cfg.InitialWorkers = 1
	}
	if cfg.InitialWorkers > maxRunWorkers {
		return RunResult{}, fmt.Errorf("начальное число потоков (%d) превышает предел %d", cfg.InitialWorkers, maxRunWorkers)
	}
	if cfg.MaxWorkers < cfg.InitialWorkers {
		cfg.MaxWorkers = cfg.InitialWorkers
	}
	if cfg.MaxWorkers > maxRunWorkers {
		return RunResult{}, fmt.Errorf("максимальное число потоков (%d) превышает предел %d", cfg.MaxWorkers, maxRunWorkers)
	}
	if cfg.Reconnects < 0 || cfg.Reconnects > 1 {
		return RunResult{}, fmt.Errorf("число переподключений должно быть 0 или 1, получено %d", cfg.Reconnects)
	}
	if cfg.StartupTimeout <= 0 {
		cfg.StartupTimeout = 5 * time.Second
	}
	if cfg.ReadyGrace <= 0 {
		cfg.ReadyGrace = 300 * time.Millisecond
	}

	runCtx, cancel := context.WithCancel(ctx)
	defer cancel()

	var measured atomic.Int64
	var warmup atomic.Int64
	var active atomic.Int32
	var measuring atomic.Bool

	readyCh := make(chan struct{}, cfg.MaxWorkers)
	doneCh := make(chan struct{}, cfg.MaxWorkers)
	states := make([]workerState, cfg.MaxWorkers)

	var wg sync.WaitGroup
	startWorker := func(index int) {
		wg.Add(1)
		go func() {
			defer wg.Done()
			defer func() { doneCh <- struct{}{} }()
			state := &states[index]
			for attempt := 0; attempt <= cfg.Reconnects; attempt++ {
				if runCtx.Err() != nil {
					break
				}
				var attemptReady atomic.Bool
				ready := func() {
					if !attemptReady.CompareAndSwap(false, true) {
						return
					}
					active.Add(1)
					if state.ready.CompareAndSwap(false, true) {
						readyCh <- struct{}{}
					}
				}
				record := func(n int64) {
					if n <= 0 {
						return
					}
					if measuring.Load() {
						measured.Add(n)
						state.measured.Add(n)
					} else {
						warmup.Add(n)
					}
				}

				err := callWorker(worker, runCtx, index, ready, record)
				if attemptReady.Load() {
					active.Add(-1)
				}
				if runCtx.Err() != nil {
					break
				}
				if err == nil {
					err = fmt.Errorf("поток %d завершился раньше времени", index+1)
				}
				state.errors = append(state.errors, err)
				if cfg.OnWorkerError != nil {
					if callbackErr := callWorkerError(cfg.OnWorkerError, index, attempt, err); callbackErr != nil {
						state.errors = append(state.errors, callbackErr)
					}
				}
				if attempt < cfg.Reconnects {
					if !waitContext(runCtx, time.Duration(200*(attempt+1))*time.Millisecond) {
						break
					}
				}
			}
		}()
	}

	workers := cfg.InitialWorkers
	for i := 0; i < workers; i++ {
		startWorker(i)
	}

	doneWorkers := 0
	waitForFirstReady := func() error {
		timer := time.NewTimer(cfg.StartupTimeout)
		defer timer.Stop()
		for {
			select {
			case <-readyCh:
				return nil
			case <-doneCh:
				doneWorkers++
				if doneWorkers == workers {
					return fmt.Errorf("все потоки (%d) завершились с ошибкой при запуске", workers)
				}
			case <-timer.C:
				return fmt.Errorf("ни один поток не был готов за %s", cfg.StartupTimeout)
			case <-runCtx.Done():
				return runCtx.Err()
			}
		}
	}
	if err := waitForFirstReady(); err != nil {
		cancel()
		wg.Wait()
		result := makeResult(states, workers, measured.Load(), 0)
		return result, addWorkerErrors(err, result.WorkerErrors)
	}
	waitStage := func(duration time.Duration, stage string) error {
		if duration <= 0 {
			return nil
		}
		timer := time.NewTimer(duration)
		defer timer.Stop()
		for {
			select {
			case <-timer.C:
				return nil
			case <-doneCh:
				doneWorkers++
				if doneWorkers >= workers {
					return fmt.Errorf("все потоки (%d) завершились с ошибкой на этапе «%s»", workers, stage)
				}
			case <-runCtx.Done():
				return runCtx.Err()
			}
		}
	}

	// Give the remaining initial workers a short opportunity to join without
	// making one slow endpoint hold the whole test hostage.
	if err := waitStage(cfg.ReadyGrace, "запуск"); err != nil {
		cancel()
		wg.Wait()
		result := makeResult(states, workers, measured.Load(), 0)
		return result, addWorkerErrors(err, result.WorkerErrors)
	}

	if cfg.Warmup > 0 {
		warmupStart := time.Now()
		if err := waitStage(cfg.Warmup, "разогрев"); err != nil {
			cancel()
			wg.Wait()
			result := makeResult(states, workers, measured.Load(), 0)
			return result, addWorkerErrors(err, result.WorkerErrors)
		}
		if cfg.Adaptive != nil {
			target := cfg.Adaptive(Mbps(warmup.Load(), time.Since(warmupStart)))
			if target < workers {
				target = workers
			}
			if target > cfg.MaxWorkers {
				target = cfg.MaxWorkers
			}
			for i := workers; i < target; i++ {
				startWorker(i)
			}
			workers = target
			if target > cfg.InitialWorkers {
				if err := waitStage(cfg.ReadyGrace, "запуск дополнительных потоков"); err != nil {
					cancel()
					wg.Wait()
					result := makeResult(states, workers, measured.Load(), 0)
					return result, addWorkerErrors(err, result.WorkerErrors)
				}
			}
		}
	}

	start := time.Now()
	deadline := start.Add(cfg.Duration)
	measuring.Store(true)
	phaseTimer := time.NewTimer(cfg.Duration)
	defer phaseTimer.Stop()
	var progressTicks <-chan time.Time
	var progressTicker *time.Ticker
	if report != nil {
		progressTicker = time.NewTicker(200 * time.Millisecond)
		progressTicks = progressTicker.C
		defer progressTicker.Stop()
	}

	lastBytes := int64(0)
	lastTime := start
	smoothed := 0.0
	hasSample := false
	finished := false
	workersEndedEarly := false
	for !finished {
		select {
		case now := <-progressTicks:
			bytes := measured.Load()
			instant := Mbps(bytes-lastBytes, now.Sub(lastTime))
			if hasSample {
				smoothed = ema(smoothed, instant)
			} else {
				smoothed = instant
				hasSample = true
			}
			report(RunProgress{Mbps: smoothed, Bytes: bytes, Elapsed: now.Sub(start), Active: int(active.Load())})
			lastBytes, lastTime = bytes, now
		case <-doneCh:
			doneWorkers++
			if doneWorkers >= workers {
				workersEndedEarly = time.Now().Before(deadline)
				finished = true
			}
		case <-phaseTimer.C:
			finished = true
		case <-runCtx.Done():
			finished = true
		}
	}
	measuring.Store(false)
	finishAt := time.Now()
	cancel()
	wg.Wait()

	result := makeResult(states, workers, measured.Load(), finishAt.Sub(start))
	if err := ctx.Err(); err != nil {
		return result, err
	}
	if workersEndedEarly {
		return result, addWorkerErrors(errors.New("все потоки передачи остановились раньше времени"), result.WorkerErrors)
	}
	if result.Bytes == 0 || result.WorkersOK == 0 {
		return result, addWorkerErrors(errors.New("передача завершилась без подтверждённых данных"), result.WorkerErrors)
	}
	return result, nil
}

func callWorker(worker Worker, ctx context.Context, index int, ready func(), record func(int64)) (err error) {
	defer func() {
		if value := recover(); value != nil {
			err = fmt.Errorf("поток %d аварийно завершился: %v", index+1, value)
		}
	}()
	return worker(ctx, index, ready, record)
}

func callWorkerError(callback func(index, attempt int, err error), index, attempt int, workerErr error) (err error) {
	defer func() {
		if value := recover(); value != nil {
			err = fmt.Errorf("обработчик ошибки потока %d аварийно завершился: %v", index+1, value)
		}
	}()
	callback(index, attempt, workerErr)
	return nil
}

func waitContext(ctx context.Context, duration time.Duration) bool {
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-timer.C:
		return true
	case <-ctx.Done():
		return false
	}
}

func makeResult(states []workerState, workers int, bytes int64, elapsed time.Duration) RunResult {
	result := RunResult{Bytes: bytes, Elapsed: elapsed}
	for i := 0; i < workers; i++ {
		state := &states[i]
		if state.measured.Load() > 0 {
			result.WorkersOK++
		} else {
			result.WorkersFailed++
		}
		result.WorkerErrors = append(result.WorkerErrors, state.errors...)
	}
	return result
}

func addWorkerErrors(err error, workerErrors []error) error {
	if len(workerErrors) == 0 {
		return err
	}
	return fmt.Errorf("%w: %w", err, errors.Join(workerErrors...))
}
