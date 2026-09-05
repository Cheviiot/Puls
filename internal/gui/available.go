//go:build !nogui

package gui

import (
	"context"
	"runtime"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/app"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

func Available() bool { return true }

func MobileBuild() bool { return runtime.GOOS == "android" || runtime.GOOS == "ios" }

func Run(ctx context.Context, options Options) error {
	application := app.NewWithID(appID)
	preferences := loadPreferences(application.Preferences())
	application.Settings().SetTheme(newPulsTheme(preferences.Theme))
	application.SetIcon(appIcon())

	runnerFactory := options.RunnerFactory
	if runnerFactory == nil {
		runnerFactory = func(log service.LogFunc) *appcore.Runner {
			return appcore.NewRunner(appcore.Options{Log: log})
		}
	}

	window := application.NewWindow("Puls")
	dashboard := newDashboard(ctx, application, window, preferences, options, runnerFactory)
	window.SetContent(dashboard.content)
	if !fyne.CurrentDevice().IsMobile() {
		window.Resize(fyne.NewSize(preferences.WindowWidth, preferences.WindowHeight))
	}
	window.SetCloseIntercept(func() {
		dashboard.cancelActive()
		dashboard.saveWindowSize()
		application.Quit()
	})
	if fyne.CurrentDevice().IsMobile() {
		application.Lifecycle().SetOnExitedForeground(dashboard.cancelActive)
	}

	done := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			dashboard.cancelActive()
			schedule(application.Quit)
		case <-done:
		}
	}()
	window.Show()
	application.Run()
	close(done)
	dashboard.cancelActive()
	dashboard.saveWindowSize()
	return nil
}
