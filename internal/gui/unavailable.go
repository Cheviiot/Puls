//go:build nogui

package gui

import "context"

func Available() bool { return false }

func MobileBuild() bool { return false }

func Run(context.Context, Options) error { return ErrUnavailable }
