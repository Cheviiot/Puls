package ui

import (
	"errors"
	"fmt"
	"os"
	"unicode/utf8"

	"golang.org/x/term"
)

// ErrCanceled is returned by Select when the user backs out (Esc, q, Ctrl+C).
var ErrCanceled = errors.New("selection canceled")

// Option is one choice in an interactive Select menu.
type Option struct {
	Label string
	Hint  string
}

// Select shows an interactive, arrow-key-driven menu on out (reading keys
// from in) and returns the index the user confirmed. Both in and out must
// be real terminals — callers should check IsTerminal on each first, or
// Select will fail when it tries to put in into raw mode.
//
// ↑/↓ move the highlight, Enter confirms it, and digit keys 1-9 jump
// straight to and confirm that option in one keystroke. Esc, 'q'/'Q', or
// Ctrl+C cancel with ErrCanceled.
func Select(in, out *os.File, style *Style, title string, options []Option) (int, error) {
	if len(options) == 0 {
		return 0, fmt.Errorf("ui: Select called with no options")
	}

	oldState, err := term.MakeRaw(int(in.Fd()))
	if err != nil {
		return 0, fmt.Errorf("ui: enter raw mode: %w", err)
	}
	defer term.Restore(int(in.Fd()), oldState)

	labelWidth := 0
	for _, o := range options {
		if n := utf8.RuneCountInString(o.Label); n > labelWidth {
			labelWidth = n
		}
	}
	// title + blank + one line per option + blank + hint line.
	frameLines := len(options) + 4

	render := func(selected int, first bool) {
		if !first {
			fmt.Fprintf(out, "\x1b[%dA\r", frameLines)
		}
		fmt.Fprintf(out, "%s\r\n\r\n", style.Bold(title))
		for i, o := range options {
			marker := "  "
			label := PadLabel(o.Label, labelWidth+2)
			if i == selected {
				marker = style.Cyan("› ")
				label = style.Bold(label)
			}
			fmt.Fprintf(out, "%s%s%s\r\n", marker, label, style.Dim(o.Hint))
		}
		fmt.Fprintf(out, "\r\n%s\r\n", style.Dim("↑/↓ выбор · Enter подтвердить · Esc отмена"))
	}
	clear := func() {
		fmt.Fprintf(out, "\x1b[%dA\r\x1b[J", frameLines)
	}
	confirm := func(idx int) (int, error) {
		clear()
		fmt.Fprintf(out, "%s %s\r\n\r\n", style.Cyan("›"), style.Bold(options[idx].Label))
		return idx, nil
	}
	cancel := func(err error) (int, error) {
		clear()
		fmt.Fprintf(out, "%s\r\n\r\n", style.Dim("отменено"))
		return 0, err
	}

	selected := 0
	render(selected, true)

	buf := make([]byte, 3)
	for {
		n, err := in.Read(buf)
		if err != nil {
			return cancel(err)
		}
		if n == 0 {
			continue
		}
		switch {
		case buf[0] == 3, buf[0] == 'q', buf[0] == 'Q':
			return cancel(ErrCanceled)
		case buf[0] == 13, buf[0] == 10: // Enter (CR or LF)
			return confirm(selected)
		case buf[0] >= '1' && buf[0] <= '9':
			if idx := int(buf[0] - '1'); idx < len(options) {
				return confirm(idx)
			}
		case buf[0] == 27: // Esc, or the start of an arrow-key sequence
			if n == 1 {
				return cancel(ErrCanceled)
			}
			if n >= 3 && buf[1] == '[' {
				switch buf[2] {
				case 'A': // up
					selected = (selected - 1 + len(options)) % len(options)
					render(selected, false)
				case 'B': // down
					selected = (selected + 1) % len(options)
					render(selected, false)
				}
			}
		}
	}
}
