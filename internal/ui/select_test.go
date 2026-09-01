package ui

import (
	"os"
	"testing"
)

func TestSelectRejectsEmptyOptions(t *testing.T) {
	_, err := Select(os.Stdin, os.Stdout, NewStyle(false), "title", nil)
	if err == nil {
		t.Fatal("Select with no options = nil error, want one")
	}
}

func TestSelectFailsOnNonTerminalInput(t *testing.T) {
	// A regular pipe isn't a terminal, so entering raw mode must fail
	// cleanly (not hang, not panic) rather than pretend to be interactive.
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatalf("os.Pipe() error = %v", err)
	}
	defer r.Close()
	defer w.Close()

	_, err = Select(r, os.Stdout, NewStyle(false), "title", []Option{{Label: "a"}})
	if err == nil {
		t.Error("Select on a non-terminal pipe = nil error, want one")
	}
}
