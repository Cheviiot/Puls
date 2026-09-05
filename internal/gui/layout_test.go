//go:build !nogui

package gui

import (
	"context"
	"image/color"
	"testing"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/canvas"
	"fyne.io/fyne/v2/test"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

func TestAdaptiveGridSwitchesBetweenTwoAndFourColumns(t *testing.T) {
	objects := []fyne.CanvasObject{
		canvas.NewRectangle(color.White), canvas.NewRectangle(color.White),
		canvas.NewRectangle(color.White), canvas.NewRectangle(color.White),
	}
	for _, object := range objects {
		object.Resize(fyne.NewSize(40, 30))
	}
	layout := &adaptiveGridLayout{breakpoint: 720, narrow: 2, wide: 4, gap: 8}
	layout.Layout(objects, fyne.NewSize(400, 100))
	if objects[2].Position().Y == 0 || objects[1].Position().Y != 0 {
		t.Fatalf("narrow positions: %#v %#v %#v", objects[0].Position(), objects[1].Position(), objects[2].Position())
	}
	layout.Layout(objects, fyne.NewSize(800, 100))
	for index, object := range objects {
		if object.Position().Y != 0 {
			t.Fatalf("wide object %d position = %#v", index, object.Position())
		}
	}
}

func TestThemeHasLightAndDarkSurfaces(t *testing.T) {
	light := color.NRGBAModel.Convert(newPulsTheme(themeLight).Color(theme.ColorNameBackground, theme.VariantDark)).(color.NRGBA)
	dark := color.NRGBAModel.Convert(newPulsTheme(themeDark).Color(theme.ColorNameBackground, theme.VariantLight)).(color.NRGBA)
	if light == dark || light.R <= dark.R {
		t.Fatalf("theme backgrounds light=%#v dark=%#v", light, dark)
	}
}

func TestDashboardRendersConnectionAndMeasurementEvents(t *testing.T) {
	application := test.NewApp()
	t.Cleanup(application.Quit)
	window := application.NewWindow("Puls")
	dashboard := newDashboard(t.Context(), application, window, loadPreferences(application.Preferences()), Options{Version: "0.3.0"}, nil)
	window.SetContent(dashboard.content)
	window.Resize(fyne.NewSize(460, 800))

	ip := "203.0.113.7"
	isp := "Ростелеком"
	detectedBy := service.Speedtest
	dashboard.applyEvent(appcore.RunEvent{Kind: appcore.EventConnectionCompleted, Connection: &appcore.ConnectionResult{
		Status: service.StatusOK, ExternalIP: &ip, ISP: &isp, DetectedBy: &detectedBy, Warnings: []string{},
	}})
	if got := dashboard.connectionLabel.Text; got != "203.0.113.7 · Ростелеком · через speedtest.ru" {
		t.Fatalf("connection label = %q", got)
	}

	value := 91.25
	dashboard.applyEvent(appcore.RunEvent{Kind: appcore.EventPhaseCompleted, Phase: service.PhaseDownload, PhaseResult: &appcore.PhaseResult{Status: service.StatusOK, Mbps: &value}})
	if dashboard.metrics[service.PhaseDownload].Text != "91,25" || dashboard.currentValue.Text != "91,25" {
		t.Fatalf("download metric=%q current=%q", dashboard.metrics[service.PhaseDownload].Text, dashboard.currentValue.Text)
	}
	if dashboard.startButton.Importance != widget.HighImportance {
		t.Fatalf("start button importance = %v", dashboard.startButton.Importance)
	}
	dashboard.addNotices("резервный сервер", "резервный сервер")
	dashboard.addNotice(widget.DangerImportance, "загрузка: ошибка")
	if dashboard.noticeLabel.Text != "резервный сервер\nзагрузка: ошибка" || dashboard.noticeLabel.Importance != widget.DangerImportance {
		t.Fatalf("notices = %q (%v)", dashboard.noticeLabel.Text, dashboard.noticeLabel.Importance)
	}
}

func TestDashboardCancelStopsActiveContext(t *testing.T) {
	application := test.NewApp()
	t.Cleanup(application.Quit)
	window := application.NewWindow("Puls")
	dashboard := newDashboard(t.Context(), application, window, loadPreferences(application.Preferences()), Options{Version: "0.3.0"}, nil)
	ctx, cancel := context.WithCancel(context.Background())
	if !dashboard.setActive(cancel) {
		t.Fatal("active operation was rejected")
	}
	dashboard.cancelActive()
	select {
	case <-ctx.Done():
	case <-time.After(100 * time.Millisecond):
		t.Fatal("GUI cancellation did not propagate")
	}
}
