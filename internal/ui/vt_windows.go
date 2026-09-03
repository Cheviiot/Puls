//go:build windows

package ui

import (
	"os"

	"golang.org/x/sys/windows"
)

func init() {
	enableVirtualTerminal(os.Stdout)
	enableVirtualTerminal(os.Stderr)
}

// enableVirtualTerminal turns on ANSI escape sequence processing for f's
// console. Unlike Windows Terminal, the legacy conhost host used by
// "Windows PowerShell" and cmd.exe does not interpret escape codes by
// default, so colors and cursor movement leak through as raw text such as
// "\x1b[1m" instead of rendering. It is a silent no-op when f is not a
// console (redirected output) or the mode change fails.
func enableVirtualTerminal(f *os.File) {
	handle := windows.Handle(f.Fd())
	var mode uint32
	if err := windows.GetConsoleMode(handle, &mode); err != nil {
		return
	}
	if mode&windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING != 0 {
		return
	}
	_ = windows.SetConsoleMode(handle, mode|windows.ENABLE_VIRTUAL_TERMINAL_PROCESSING)
}
