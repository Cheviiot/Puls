// Package ui renders Puls terminal output: ANSI colors, an
// animated spinner for indeterminate waits, a progress bar for
// duration-bounded measurements, and a live-updating line that gracefully
// degrades to plain one-shot printing when stdout isn't a terminal (piped
// output, log files, NO_COLOR, dumb terminals) or when output is suppressed
// entirely (JSON mode).
package ui

import (
	"fmt"
	"io"
	"os"
	"strings"
	"time"
	"unicode/utf8"

	"golang.org/x/term"
)

// IsTerminal reports whether f is an interactive terminal.
func IsTerminal(f *os.File) bool {
	return term.IsTerminal(int(f.Fd()))
}

// ColorEnabled reports whether ANSI styling should be used for f: it must
// be a terminal, forceOff (e.g. a -no-color flag) must not be set, and
// neither NO_COLOR nor TERM=dumb may be present (https://no-color.org).
func ColorEnabled(f *os.File, forceOff bool) bool {
	if forceOff {
		return false
	}
	if _, set := os.LookupEnv("NO_COLOR"); set {
		return false
	}
	if os.Getenv("TERM") == "dumb" {
		return false
	}
	return IsTerminal(f)
}

const (
	ansiReset  = "\x1b[0m"
	ansiBold   = "\x1b[1m"
	ansiDim    = "\x1b[2m"
	ansiRed    = "\x1b[31m"
	ansiGreen  = "\x1b[32m"
	ansiYellow = "\x1b[33m"
	ansiCyan   = "\x1b[36m"
)

// Style renders text with ANSI colors when enabled, or plain text otherwise
// — every method is safe to call unconditionally regardless of terminal
// support.
type Style struct{ enabled bool }

// NewStyle creates a Style. When enabled is false every method returns its
// input unchanged.
func NewStyle(enabled bool) *Style { return &Style{enabled: enabled} }

func (s *Style) wrap(code, text string) string {
	if !s.enabled {
		return text
	}
	return code + text + ansiReset
}

func (s *Style) Bold(t string) string { return s.wrap(ansiBold, t) }
func (s *Style) Dim(t string) string  { return s.wrap(ansiDim, t) }
func (s *Style) Cyan(t string) string { return s.wrap(ansiCyan, t) }
func (s *Style) Red(t string) string  { return s.wrap(ansiRed, t) }
func (s *Style) Green(t string) string {
	return s.wrap(ansiGreen, t)
}
func (s *Style) Yellow(t string) string {
	return s.wrap(ansiYellow, t)
}

// Speed formats a Mbit/s value and colors it by how good it is:
// green >= 50, yellow >= 10, red below that.
func (s *Style) Speed(mbps float64) string {
	text := fmt.Sprintf("%.2f Мбит/с", mbps)
	switch {
	case mbps >= 50:
		return s.wrap(ansiGreen, text)
	case mbps >= 10:
		return s.wrap(ansiYellow, text)
	default:
		return s.wrap(ansiRed, text)
	}
}

// Latency formats a millisecond value and colors it by how good it is:
// green <= 30ms, yellow <= 80ms, red above that. label is appended as-is
// (e.g. "мс").
func (s *Style) Latency(ms float64, label string) string {
	text := fmt.Sprintf("%.1f %s", ms, label)
	switch {
	case ms <= 30:
		return s.wrap(ansiGreen, text)
	case ms <= 80:
		return s.wrap(ansiYellow, text)
	default:
		return s.wrap(ansiRed, text)
	}
}

// PadLabel right-pads a plain-text label (no ANSI codes) to width using
// spaces, for aligning "label   value" rows.
func PadLabel(label string, width int) string {
	n := utf8.RuneCountInString(label)
	if n >= width {
		return label
	}
	return label + strings.Repeat(" ", width-n)
}

// Bar renders a fixed-width progress bar like "[███████░░░░░░]".
func Bar(width int, frac float64) string {
	frac = max(0, min(1, frac))
	filled := int(frac*float64(width) + 0.5)
	return "[" + strings.Repeat("█", filled) + strings.Repeat("░", width-filled) + "]"
}

// Line is a single terminal line that redraws itself in place via carriage
// return when live redraws are enabled, and otherwise only ever prints once
// (via Final) so redirected output and log files stay clean and readable.
// Writing to w=io.Discard makes a Line fully silent, which is how JSON mode
// suppresses all human-readable output.
type Line struct {
	w    io.Writer
	live bool
}

// NewLine creates a Line. live should be true only when w is an actual
// terminal (see IsTerminal) — it controls whether Update() redraws in
// place or is dropped.
func NewLine(w io.Writer, live bool) *Line {
	return &Line{w: w, live: live}
}

// Update redraws the line in place. It is a no-op unless live redraws are
// enabled, so intermediate progress never spams piped output or log files.
func (l *Line) Update(s string) {
	if !l.live {
		return
	}
	fmt.Fprint(l.w, "\r\x1b[K", s)
}

// Final prints s once and moves to the next line — the only output a
// non-live Line ever produces.
func (l *Line) Final(s string) {
	if l.live {
		fmt.Fprint(l.w, "\r\x1b[K", s, "\n")
	} else {
		fmt.Fprintln(l.w, s)
	}
}

var spinnerFrames = [...]string{"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"}

// Spin runs fn in the background, animating a spinner with label on line
// while it's in flight, and returns fn's result once it completes. The
// caller is responsible for calling line.Final with the outcome — Spin only
// owns the line while fn is running.
func Spin[T any](line *Line, label string, fn func() (T, error)) (T, error) {
	if !line.live {
		return fn()
	}
	type result struct {
		v   T
		err error
	}
	done := make(chan result, 1)
	go func() {
		v, err := fn()
		done <- result{v, err}
	}()

	ticker := time.NewTicker(90 * time.Millisecond)
	defer ticker.Stop()
	i := 0
	for {
		select {
		case r := <-done:
			return r.v, r.err
		case <-ticker.C:
			line.Update(spinnerFrames[i%len(spinnerFrames)] + " " + label)
			i++
		}
	}
}
