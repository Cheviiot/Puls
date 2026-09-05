//go:build !nogui

package gui

import "fyne.io/fyne/v2"

type adaptiveGridLayout struct {
	breakpoint float32
	narrow     int
	wide       int
	gap        float32
}

func (l *adaptiveGridLayout) Layout(objects []fyne.CanvasObject, size fyne.Size) {
	visible := visibleObjects(objects)
	if len(visible) == 0 {
		return
	}
	columns := l.narrow
	if size.Width >= l.breakpoint {
		columns = l.wide
	}
	if columns < 1 {
		columns = 1
	}
	if columns > len(visible) {
		columns = len(visible)
	}
	width := (size.Width - float32(columns-1)*l.gap) / float32(columns)
	rowHeights := make([]float32, (len(visible)+columns-1)/columns)
	for index, object := range visible {
		row := index / columns
		if height := object.MinSize().Height; height > rowHeights[row] {
			rowHeights[row] = height
		}
	}
	y := float32(0)
	for index, object := range visible {
		row, column := index/columns, index%columns
		x := float32(column) * (width + l.gap)
		object.Move(fyne.NewPos(x, y))
		object.Resize(fyne.NewSize(width, rowHeights[row]))
		if column == columns-1 || index == len(visible)-1 {
			y += rowHeights[row] + l.gap
		}
	}
}

func (l *adaptiveGridLayout) MinSize(objects []fyne.CanvasObject) fyne.Size {
	visible := visibleObjects(objects)
	if len(visible) == 0 {
		return fyne.Size{}
	}
	columns := min(l.narrow, len(visible))
	if columns < 1 {
		columns = 1
	}
	maxWidth, totalHeight := float32(0), float32(0)
	rows := (len(visible) + columns - 1) / columns
	rowHeights := make([]float32, rows)
	for index, object := range visible {
		size := object.MinSize()
		if size.Width > maxWidth {
			maxWidth = size.Width
		}
		row := index / columns
		if size.Height > rowHeights[row] {
			rowHeights[row] = size.Height
		}
	}
	for _, height := range rowHeights {
		totalHeight += height
	}
	return fyne.NewSize(maxWidth*float32(columns)+l.gap*float32(columns-1), totalHeight+l.gap*float32(rows-1))
}

func visibleObjects(objects []fyne.CanvasObject) []fyne.CanvasObject {
	result := make([]fyne.CanvasObject, 0, len(objects))
	for _, object := range objects {
		if object.Visible() {
			result = append(result, object)
		}
	}
	return result
}
