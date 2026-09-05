//go:build !nogui

package gui

import (
	"time"

	"fyne.io/fyne/v2"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

const (
	appID               = "io.github.cheviiot.puls"
	defaultWindowWidth  = float32(460)
	defaultWindowHeight = float32(800)
)

type themeMode string

const (
	themeSystem themeMode = "system"
	themeLight  themeMode = "light"
	themeDark   themeMode = "dark"
)

type userPreferences struct {
	store        fyne.Preferences
	Theme        themeMode
	Service      service.ServiceID
	Profile      appcore.Profile
	Duration     time.Duration
	Connections  int
	Only         appcore.PhaseSelection
	WindowWidth  float32
	WindowHeight float32
}

func loadPreferences(store fyne.Preferences) *userPreferences {
	preferences := &userPreferences{
		store:        store,
		Theme:        themeMode(store.StringWithFallback("theme", string(themeSystem))),
		Service:      service.ServiceID(store.StringWithFallback("service", string(service.Yandex))),
		Profile:      appcore.Profile(store.StringWithFallback("profile", string(appcore.ProfileBalanced))),
		Duration:     time.Duration(store.IntWithFallback("duration_seconds", 10)) * time.Second,
		Connections:  store.IntWithFallback("connections", 0),
		Only:         appcore.PhaseSelection(store.StringWithFallback("phase", string(appcore.PhaseAll))),
		WindowWidth:  float32(store.FloatWithFallback("window_width", float64(defaultWindowWidth))),
		WindowHeight: float32(store.FloatWithFallback("window_height", float64(defaultWindowHeight))),
	}
	preferences.sanitize()
	return preferences
}

func (p *userPreferences) sanitize() {
	switch p.Theme {
	case themeSystem, themeLight, themeDark:
	default:
		p.Theme = themeSystem
	}
	switch p.Service {
	case service.Yandex, service.Speedtest, service.All:
	default:
		p.Service = service.Yandex
	}
	switch p.Profile {
	case appcore.ProfileQuick, appcore.ProfileBalanced, appcore.ProfileAccurate:
	default:
		p.Profile = appcore.ProfileBalanced
	}
	if p.Duration < 3*time.Second || p.Duration > 60*time.Second {
		p.Duration = p.Profile.Duration()
	}
	if p.Connections < 0 || p.Connections > 16 {
		p.Connections = 0
	}
	switch p.Only {
	case appcore.PhaseAll, appcore.PhasePing, appcore.PhaseDownload, appcore.PhaseUpload:
	default:
		p.Only = appcore.PhaseAll
	}
	if p.WindowWidth < 390 || p.WindowWidth > 2560 {
		p.WindowWidth = defaultWindowWidth
	}
	if p.WindowHeight < 640 || p.WindowHeight > 2160 {
		p.WindowHeight = defaultWindowHeight
	}
}

func (p *userPreferences) saveMeasurement(settings measurementSettings) {
	p.Service = settings.Service
	p.Profile = settings.Profile
	p.Duration = settings.Duration
	p.Connections = settings.Connections
	p.Only = settings.Only
	p.store.SetString("service", string(p.Service))
	p.store.SetString("profile", string(p.Profile))
	p.store.SetInt("duration_seconds", int(p.Duration/time.Second))
	p.store.SetInt("connections", p.Connections)
	p.store.SetString("phase", string(p.Only))
}

func (p *userPreferences) saveTheme(mode themeMode) {
	p.Theme = mode
	p.store.SetString("theme", string(mode))
}

func (p *userPreferences) saveWindow(size fyne.Size) {
	if size.Width < 390 || size.Height < 640 {
		return
	}
	p.WindowWidth, p.WindowHeight = size.Width, size.Height
	p.store.SetFloat("window_width", float64(size.Width))
	p.store.SetFloat("window_height", float64(size.Height))
}
