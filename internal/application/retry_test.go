package application

import (
	"context"
	"errors"
	"testing"

	"github.com/Cheviiot/Puls/internal/service"
)

func TestWithRetrySucceedsOnSecondAttempt(t *testing.T) {
	calls := 0
	err := withRetry(t.Context(), 2, func() error {
		calls++
		if calls == 1 {
			return errors.New("temporary")
		}
		return nil
	})
	if err != nil || calls != 2 {
		t.Fatalf("withRetry = (%v, %d calls)", err, calls)
	}
}

func TestWithRetryStopsOnPermanentTypedError(t *testing.T) {
	calls := 0
	want := service.NewError(service.Yandex, service.PhasePing, service.CodeProtocol, false, errors.New("broken"))
	err := withRetry(context.Background(), 2, func() error {
		calls++
		return want
	})
	if !errors.Is(err, want) || calls != 1 {
		t.Fatalf("withRetry = (%v, %d calls)", err, calls)
	}
}

func TestWithRetryStopsOnCancellation(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	calls := 0
	err := withRetry(ctx, 2, func() error {
		calls++
		cancel()
		return errors.New("temporary")
	})
	if !errors.Is(err, context.Canceled) || calls != 1 {
		t.Fatalf("withRetry = (%v, %d calls)", err, calls)
	}
}
