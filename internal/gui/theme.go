//go:build !nogui

package gui

import (
	"image/color"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/theme"
)

const (
	metricSizeName      fyne.ThemeSizeName = "puls.metric"
	metricSmallSizeName fyne.ThemeSizeName = "puls.metric.small"
)

type pulsTheme struct {
	mode themeMode
}

func newPulsTheme(mode themeMode) fyne.Theme { return &pulsTheme{mode: mode} }

func (t *pulsTheme) Color(name fyne.ThemeColorName, variant fyne.ThemeVariant) color.Color {
	variant = t.variant(variant)
	switch name {
	case theme.ColorNamePrimary, theme.ColorNameFocus, theme.ColorNameHyperlink:
		if variant == theme.VariantDark {
			return color.NRGBA{R: 0x22, G: 0xd3, B: 0xee, A: 0xff}
		}
		return color.NRGBA{R: 0x08, G: 0x91, B: 0xb2, A: 0xff}
	case theme.ColorNameBackground:
		if variant == theme.VariantDark {
			return color.NRGBA{R: 0x0b, G: 0x12, B: 0x16, A: 0xff}
		}
		return color.NRGBA{R: 0xf7, G: 0xfa, B: 0xfb, A: 0xff}
	case theme.ColorNameHeaderBackground:
		if variant == theme.VariantDark {
			return color.NRGBA{R: 0x10, G: 0x1a, B: 0x20, A: 0xff}
		}
		return color.NRGBA{R: 0xee, G: 0xf5, B: 0xf7, A: 0xff}
	case theme.ColorNameDisabled:
		if variant == theme.VariantDark {
			return color.NRGBA{R: 0x9a, G: 0xa8, B: 0xaf, A: 0xff}
		}
		return color.NRGBA{R: 0x5d, G: 0x6b, B: 0x72, A: 0xff}
	}
	return theme.DefaultTheme().Color(name, variant)
}

func (t *pulsTheme) Font(style fyne.TextStyle) fyne.Resource {
	return theme.DefaultTheme().Font(style)
}

func (t *pulsTheme) Icon(name fyne.ThemeIconName) fyne.Resource {
	return theme.DefaultTheme().Icon(name)
}

func (t *pulsTheme) Size(name fyne.ThemeSizeName) float32 {
	switch name {
	case metricSizeName:
		return 52
	case metricSmallSizeName:
		return 24
	case theme.SizeNameButtonRadius, theme.SizeNameCardRadius, theme.SizeNameDialogRadius, theme.SizeNamePopupRadius:
		return 12
	default:
		return theme.DefaultTheme().Size(name)
	}
}

func (t *pulsTheme) variant(requested fyne.ThemeVariant) fyne.ThemeVariant {
	switch t.mode {
	case themeLight:
		return theme.VariantLight
	case themeDark:
		return theme.VariantDark
	default:
		return requested
	}
}
