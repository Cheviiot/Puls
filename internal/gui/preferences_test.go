//go:build !nogui

package gui

import (
	"testing"
	"time"

	"fyne.io/fyne/v2/test"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

func TestPreferencesSanitizeInvalidValues(t *testing.T) {
	application := test.NewApp()
	t.Cleanup(application.Quit)
	store := application.Preferences()
	store.SetString("theme", "neon")
	store.SetString("service", "unknown")
	store.SetString("profile", "slow")
	store.SetInt("duration_seconds", 90)
	store.SetInt("connections", 99)
	store.SetString("phase", "everything")
	store.SetFloat("window_width", 10)
	store.SetFloat("window_height", 10)

	preferences := loadPreferences(store)
	if preferences.Theme != themeSystem || preferences.Service != service.Yandex || preferences.Profile != appcore.ProfileBalanced {
		t.Fatalf("sanitized preferences = %+v", preferences)
	}
	if preferences.Duration != 10*time.Second || preferences.Connections != 0 || preferences.Only != appcore.PhaseAll {
		t.Fatalf("sanitized measurement preferences = %+v", preferences)
	}
	if preferences.WindowWidth != defaultWindowWidth || preferences.WindowHeight != defaultWindowHeight {
		t.Fatalf("sanitized window = %vx%v", preferences.WindowWidth, preferences.WindowHeight)
	}
}

func TestPreferencesNeverPersistPrivateResults(t *testing.T) {
	application := test.NewApp()
	t.Cleanup(application.Quit)
	store := application.Preferences()
	preferences := loadPreferences(store)
	preferences.saveMeasurement(measurementSettings{
		Service: service.Speedtest, Profile: appcore.ProfileQuick, Duration: 7 * time.Second,
		Connections: 4, Only: appcore.PhasePing, Server: "private.example:443", ShowIP: true,
	})

	if got := store.String("server"); got != "" {
		t.Fatalf("server persisted: %q", got)
	}
	if store.Bool("show_ip") {
		t.Fatal("show_ip was persisted")
	}
	if got := store.String("external_ip"); got != "" {
		t.Fatalf("external IP persisted: %q", got)
	}
}

func TestParseSettingsValidatesAndClearsForeignServer(t *testing.T) {
	settings, err := parseSettings(
		"Яндекс.Интернетометр", "Быстрый · 5 с", "8", "4", "Только задержка",
		"server.example:443", true, "Тёмное",
	)
	if err != nil {
		t.Fatal(err)
	}
	if settings.Service != service.Yandex || settings.Server != "" || settings.Duration != 8*time.Second || settings.Theme != themeDark {
		t.Fatalf("settings = %+v", settings)
	}
	if summary := settingsSummary(settings); summary != "Быстрый  ·  8 с  ·  4  ·  Только задержка" {
		t.Fatalf("settings summary = %q", summary)
	}
	if _, err := parseSettings("speedtest.ru", "Быстрый · 5 с", "2", "4", "Все этапы", "", false, "Как в системе"); err == nil {
		t.Fatal("duration below limit was accepted")
	}
}
