package main

import (
	"fmt"
	"io"
	"strings"

	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

const (
	labelWidth = 22
	rowIndent  = "  "
)

func renderServiceHeader(w io.Writer, style *ui.Style, id service.ServiceID) {
	name := service.DisplayName(id)
	if id == service.Yandex {
		name = "Яндекс.Интернетометр"
	}
	fmt.Fprintf(w, "%s %s\n", style.Cyan(style.Bold("Puls ·")), style.Bold(name))
}

func renderConnection(w io.Writer, style *ui.Style, result connectionResult) {
	fmt.Fprintf(w, "%s %s\n", style.Cyan(style.Bold("Puls ·")), style.Bold("Подключение"))
	if result.Status == service.StatusOK {
		fmt.Fprintln(w, metricLabel(style, "Внешний IP")+valueOrDash(result.ExternalIP))
		fmt.Fprintln(w, metricLabel(style, "Интернет-провайдер")+valueOrDash(result.ISP))
		if result.DetectedBy != nil {
			fmt.Fprintln(w, metricLabel(style, "Сервис")+service.DisplayName(*result.DetectedBy))
		}
	} else if result.Error != nil {
		fmt.Fprintln(w, metricLabel(style, "Результат")+style.Red("не удалось")+style.Dim(" · "+result.Error.Message))
	}
	fmt.Fprintln(w)
}

func valueOrDash(value *string) string {
	if value == nil || *value == "" {
		return "—"
	}
	return *value
}

func renderPingMetric(style *ui.Style, ping service.PingResult) string {
	return fmt.Sprintf(
		"%s%s%s%s",
		metricLabel(style, "Задержка"),
		style.Latency(ping.ValueMs, "мс"),
		style.Dim("  ·  джиттер "),
		style.Latency(ping.JitterMs, "мс"),
	)
}

func renderMeasurementFooter(w io.Writer, style *ui.Style, result measurementResult) {
	for _, warning := range result.Warnings {
		fmt.Fprintf(w, "%s%s %s\n", rowIndent, style.Yellow("!"), style.Dim(warning))
	}
	icon, text, paint := "✓", "готово", style.Green
	switch result.Status {
	case service.StatusPartial:
		icon, text, paint = "!", "частичный результат", style.Yellow
	case service.StatusError:
		icon, text, paint = "×", "измерение не выполнено", style.Red
	case service.StatusCanceled:
		icon, text, paint = "•", "остановлено", style.Yellow
	}
	fmt.Fprintf(w, "%s%s %s\n\n", rowIndent, paint(icon), style.Dim(text))
}

func renderSummary(w io.Writer, style *ui.Style, results []measurementResult) {
	ok, partial, failed := 0, 0, 0
	for _, result := range results {
		switch result.Status {
		case service.StatusOK:
			ok++
		case service.StatusPartial:
			partial++
		default:
			failed++
		}
	}
	parts := make([]string, 0, 3)
	if ok > 0 {
		parts = append(parts, style.Green(fmt.Sprintf("%d успешно", ok)))
	}
	if partial > 0 {
		parts = append(parts, style.Yellow(fmt.Sprintf("%d частично", partial)))
	}
	if failed > 0 {
		parts = append(parts, style.Red(fmt.Sprintf("%d с ошибкой", failed)))
	}
	fmt.Fprintf(w, "%s  %s\n", style.Bold("Итог"), strings.Join(parts, style.Dim(" · ")))
}

func metricLabel(style *ui.Style, label string) string {
	return rowIndent + style.Dim(ui.PadLabel(label, labelWidth))
}

func formatServer(server service.Server) string {
	switch {
	case server.City != "" && server.Region != "":
		return fmt.Sprintf("%s · %s, %s", server.Name, server.City, server.Region)
	case server.City != "":
		return fmt.Sprintf("%s · %s", server.Name, server.City)
	default:
		return server.Name
	}
}
