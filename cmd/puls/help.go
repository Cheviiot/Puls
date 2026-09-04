package main

import (
	"fmt"
	"io"

	"github.com/Cheviiot/Puls/internal/ui"
)

func printHelp(w io.Writer, style *ui.Style) {
	fmt.Fprintf(w, "%s  %s\n", style.Cyan(style.Bold("Puls")), style.Dim("проверка скорости и качества интернета"))
	fmt.Fprintln(w, style.Dim("Яндекс.Интернетометр и speedtest.ru"))

	fmt.Fprintln(w, "\nИСПОЛЬЗОВАНИЕ")
	fmt.Fprintln(w, "  puls [service] [options]")
	fmt.Fprintln(w, "  puls ip [service] [options]")

	fmt.Fprintln(w, "\nСЕРВИСЫ")
	printHelpRow(w, "yandex", "Яндекс.Интернетометр")
	printHelpRow(w, "speedtest", "speedtest.ru · Ростелеком")
	printHelpRow(w, "all", "оба сервиса последовательно")

	fmt.Fprintln(w, "\nКОМАНДЫ")
	printHelpRow(w, "ip [service]", "внешний IP и интернет-провайдер")
	printHelpRow(w, "help", "показать справку")
	printHelpRow(w, "version", "показать версию")

	fmt.Fprintln(w, "\nПАРАМЕТРЫ ИЗМЕРЕНИЯ")
	printHelpRow(w, "--profile <name>", "quick, balanced или accurate")
	printHelpRow(w, "--duration <seconds>", "время фазы · от 3 до 60 секунд")
	printHelpRow(w, "--connections <number>", "0 — автоматически, вручную — от 1 до 16")
	printHelpRow(w, "--only <phase>", "all, ping, download или upload")
	printHelpRow(w, "--server <host>", "сервер измерения · только speedtest")
	printHelpRow(w, "--show-ip", "добавить данные подключения")

	fmt.Fprintln(w, "\nОБЩИЕ ПАРАМЕТРЫ")
	printHelpRow(w, "--json", "машинный JSON без оформления")
	printHelpRow(w, "--verbose", "адреса протокола, резервные пути и переподключения")
	printHelpRow(w, "--no-color", "отключить цвета")
	printHelpRow(w, "-h, --help", "показать справку")
	printHelpRow(w, "--version", "показать версию")

	fmt.Fprintln(w, "\nПРИМЕРЫ")
	printHelpExample(w, "puls", "выбрать сервис")
	printHelpExample(w, "puls yandex", "полная проверка через Яндекс")
	printHelpExample(w, "puls all --profile quick", "быстро проверить оба сервиса")
	printHelpExample(w, "puls speedtest --only ping", "измерить только задержку")
	printHelpExample(w, "puls ip", "определить IP с резервным сервисом")
	printHelpExample(w, "puls ip speedtest", "IP и интернет-провайдер через speedtest.ru")
	printHelpExample(w, "puls ip yandex --json", "использовать конкретный сервис")

	fmt.Fprintln(w, style.Dim("\nCtrl+C останавливает операцию · подробнее: README.md"))
}

func printHelpRow(w io.Writer, name, description string) {
	fmt.Fprintf(w, "  %-28s %s\n", name, description)
}

func printHelpExample(w io.Writer, example, description string) {
	fmt.Fprintf(w, "  %-34s %s\n", example, description)
}
