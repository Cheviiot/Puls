// Command puls measures Internet latency and throughput using public,
// first-party protocols exposed by supported measurement services.
package main

import (
	"context"
	"os"
	"os/signal"
	"syscall"

	"github.com/Cheviiot/Puls/internal/gui"
)

var version = "dev"

func main() {
	os.Exit(run(os.Args[1:]))
}

func run(args []string) int {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if gui.MobileBuild() {
		if err := gui.Run(ctx, gui.Options{Version: version}); err != nil {
			return 1
		}
		return 0
	}
	return newApplication(os.Stdin, os.Stdout, os.Stderr).Run(ctx, args)
}
