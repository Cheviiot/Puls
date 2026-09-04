package main

import (
	"context"
	"errors"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

func withRetry(ctx context.Context, attempts int, operation func() error) error {
	if attempts < 1 {
		return errors.New("число попыток должно быть положительным")
	}
	var lastErr error
	for attempt := 0; attempt < attempts; attempt++ {
		if err := ctx.Err(); err != nil {
			return err
		}
		if lastErr = operation(); lastErr == nil {
			return nil
		}
		var operationError *service.OpError
		if errors.As(lastErr, &operationError) && !operationError.Retryable {
			return lastErr
		}
		if attempt == attempts-1 {
			break
		}
		timer := time.NewTimer(time.Duration(attempt+1) * 300 * time.Millisecond)
		select {
		case <-timer.C:
		case <-ctx.Done():
			if !timer.Stop() {
				select {
				case <-timer.C:
				default:
				}
			}
			return ctx.Err()
		}
	}
	return lastErr
}
