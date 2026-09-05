//go:build !nogui

package gui

import (
	_ "embed"

	"fyne.io/fyne/v2"
)

//go:embed assets/Icon.svg
var iconData []byte

func appIcon() fyne.Resource {
	return fyne.NewStaticResource("Puls.svg", iconData)
}
