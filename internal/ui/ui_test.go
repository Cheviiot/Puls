package ui

import (
	"bytes"
	"strings"
	"testing"
)

func TestBarWidthAndClamping(t *testing.T) {
	tests := []struct {
		frac float64
		want string
	}{
		{0, "[░░░░░░░░░░]"},
		{1, "[██████████]"},
		{0.5, "[█████░░░░░]"},
		{-1, "[░░░░░░░░░░]"}, // clamp below 0
		{2, "[██████████]"},  // clamp above 1
	}
	for _, tt := range tests {
		got := Bar(10, tt.frac)
		if got != tt.want {
			t.Errorf("Bar(10, %v) = %q, want %q", tt.frac, got, tt.want)
		}
	}
}

func TestPadLabel(t *testing.T) {
	if got := PadLabel("abc", 6); got != "abc   " {
		t.Errorf("PadLabel(%q, 6) = %q, want %q", "abc", got, "abc   ")
	}
	// Already at/over width: unchanged, never truncated.
	if got := PadLabel("abcdefgh", 6); got != "abcdefgh" {
		t.Errorf("PadLabel over width should be unchanged, got %q", got)
	}
	// Cyrillic runes must be counted as one column each, not by UTF-8 byte length.
	if got := PadLabel("Пинг", 10); got != "Пинг      " {
		t.Errorf("PadLabel(%q, 10) = %q (len %d), want 10 visible columns", "Пинг", got, len(got))
	}
}

func TestStyleDisabledPassesThrough(t *testing.T) {
	s := NewStyle(false)
	if got := s.Bold("x"); got != "x" {
		t.Errorf("disabled Bold() = %q, want %q", got, "x")
	}
	if got := s.Speed(100); got != "100.00 Мбит/с" {
		t.Errorf("disabled Speed(100) = %q, want %q", got, "100.00 Мбит/с")
	}
}

func TestStyleEnabledWrapsWithReset(t *testing.T) {
	s := NewStyle(true)
	got := s.Bold("x")
	if !strings.HasPrefix(got, "\x1b[1m") || !strings.HasSuffix(got, "\x1b[0m") {
		t.Errorf("enabled Bold() = %q, want ANSI-wrapped", got)
	}
}

func TestSpeedColorThresholds(t *testing.T) {
	s := NewStyle(true)
	if !strings.Contains(s.Speed(60), "\x1b[32m") {
		t.Errorf("Speed(60) should be green, got %q", s.Speed(60))
	}
	if !strings.Contains(s.Speed(20), "\x1b[33m") {
		t.Errorf("Speed(20) should be yellow, got %q", s.Speed(20))
	}
	if !strings.Contains(s.Speed(5), "\x1b[31m") {
		t.Errorf("Speed(5) should be red, got %q", s.Speed(5))
	}
}

func TestLineNonLiveOnlyPrintsOnFinal(t *testing.T) {
	var buf bytes.Buffer
	l := NewLine(&buf, false)
	l.Update("intermediate, should not appear")
	if buf.Len() != 0 {
		t.Errorf("Update on non-live Line wrote output: %q", buf.String())
	}
	l.Final("done")
	if got := buf.String(); got != "done\n" {
		t.Errorf("Final on non-live Line = %q, want %q", got, "done\n")
	}
}

func TestLineLiveRedrawsInPlace(t *testing.T) {
	var buf bytes.Buffer
	l := NewLine(&buf, true)
	l.Update("frame1")
	l.Final("result")
	got := buf.String()
	if !strings.Contains(got, "\r\x1b[K") {
		t.Errorf("live Line output missing carriage-return redraw sequence: %q", got)
	}
	if !strings.HasSuffix(got, "result\n") {
		t.Errorf("live Line Final should end with the final text and newline: %q", got)
	}
}

func TestSpinReturnsResultAndError(t *testing.T) {
	var buf bytes.Buffer
	line := NewLine(&buf, false)

	v, err := Spin(line, "loading…", func() (int, error) {
		return 42, nil
	})
	if err != nil || v != 42 {
		t.Errorf("Spin() = (%d, %v), want (42, nil)", v, err)
	}
}
