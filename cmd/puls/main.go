// Command puls measures Internet latency and throughput using the public
// first-party protocols of Yandex Internetometer and speedtest.ru.
package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"math"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	"github.com/Cheviiot/Puls/internal/provider"
	"github.com/Cheviiot/Puls/internal/provider/speedtestru"
	"github.com/Cheviiot/Puls/internal/provider/yandex"
	"github.com/Cheviiot/Puls/internal/ui"
)

var version = "dev"

const (
	labelWidth = 12
	rowIndent  = "  "
)

func main() { os.Exit(run(os.Args[1:])) }

func splitCommand(args []string) (string, []string) {
	if len(args) == 0 || strings.HasPrefix(args[0], "-") {
		return "", args
	}

	switch args[0] {
	case "help":
		return "", append([]string{"--help"}, args[1:]...)
	case "version":
		return "", append([]string{"--version"}, args[1:]...)
	default:
		return args[0], args[1:]
	}
}

func printHelp(w io.Writer, style *ui.Style) {
	fmt.Fprintf(w, "%s  %s\n", style.Cyan(style.Bold("Puls")), style.Dim("точная проверка скорости интернета"))
	fmt.Fprintln(w, style.Dim("Задержка, скачивание и отдача через Яндекс и speedtest.ru."))

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("ИСПОЛЬЗОВАНИЕ"))
	fmt.Fprintln(w, "  puls [источник] [параметры]")
	fmt.Fprintln(w, "  puls [параметры]")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("ИСТОЧНИКИ"))
	printHelpRow(w, "yandex", "Яндекс.Интернетометр · по умолчанию")
	printHelpRow(w, "speedtest", "серверы speedtest.ru / Ростелеком")
	printHelpRow(w, "all", "все источники последовательно")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("РЕЖИМЫ"))
	printHelpRow(w, "quick", "5 секунд на скачивание и отдачу")
	printHelpRow(w, "balanced", "10 секунд · по умолчанию")
	printHelpRow(w, "accurate", "15 секунд для более устойчивого результата")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("ПАРАМЕТРЫ ПРОВЕРКИ"))
	printHelpRow(w, "--profile <name>", "quick, balanced или accurate")
	printHelpRow(w, "--duration <seconds>", "вручную задать время · от 3 до 60 секунд")
	printHelpRow(w, "--connections <number>", "0 — автоматически, вручную — от 1 до 16")
	printHelpRow(w, "--only <phase>", "all, ping, download или upload")
	printHelpRow(w, "--server <host>", "сервер для speedtest")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("ВЫВОД И ДИАГНОСТИКА"))
	printHelpRow(w, "--json", "структурированный JSON в стандартный вывод")
	printHelpRow(w, "--show-ip", "показать внешний IP вместе с замером · yandex")
	printHelpRow(w, "--ip", "только внешний IP, без замера · yandex")
	printHelpRow(w, "--verbose", "выбор сервера, резервный путь и переподключения")
	printHelpRow(w, "--no-color", "отключить цвета")
	printHelpRow(w, "--version", "показать версию")
	printHelpRow(w, "-h, --help", "показать эту справку")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Bold("ПРИМЕРЫ"))
	printHelpExample(w, "puls", "выбрать источник интерактивно")
	printHelpExample(w, "puls yandex", "обычная проверка через Яндекс")
	printHelpExample(w, "puls all --profile quick", "быстро проверить все источники")
	printHelpExample(w, "puls speedtest --only download", "измерить только входящую скорость")
	printHelpExample(w, "puls all --json", "получить структурированный результат")
	printHelpExample(w, "puls --ip", "проверить только внешний IP")

	fmt.Fprintln(w)
	fmt.Fprintln(w, style.Dim("Ctrl+C останавливает замер · подробнее: README.md"))
}

func printHelpRow(w io.Writer, name, description string) {
	fmt.Fprintf(w, "  %-27s %s\n", name, description)
}

func printHelpExample(w io.Writer, command, description string) {
	fmt.Fprintf(w, "  %-31s %s\n", command, description)
}

func friendlyFlagError(err error) string {
	message := err.Error()
	message = strings.Replace(message, "flag provided but not defined: -", "неизвестный параметр: --", 1)
	message = strings.Replace(message, "flag needs an argument: -", "параметр требует значение: --", 1)
	message = strings.Replace(message, "invalid value ", "неверное значение ", 1)
	message = strings.Replace(message, " for flag -", " для параметра --", 1)
	message = strings.Replace(message, "parse error", "не удалось распознать значение", 1)
	message = strings.Replace(message, "invalid syntax", "неверная запись", 1)
	message = strings.Replace(message, "value out of range", "значение вне допустимого диапазона", 1)
	return message
}

func printUsageError(style *ui.Style, message string) int {
	fmt.Fprintf(os.Stderr, "%s %s\n", style.Red("Ошибка:"), message)
	fmt.Fprintln(os.Stderr, style.Dim("Подсказка: puls --help"))
	return 2
}

func run(args []string) int {
	providerKey, args := splitCommand(args)
	providerKey = strings.ToLower(strings.TrimSpace(providerKey))
	fs := flag.NewFlagSet("puls", flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	var (
		profile     = fs.String("profile", "balanced", "")
		duration    = fs.Int("duration", 0, "")
		connections = fs.Int("connections", 0, "")
		only        = fs.String("only", "all", "")
		jsonOut     = fs.Bool("json", false, "")
		verbose     = fs.Bool("verbose", false, "")
		noColor     = fs.Bool("no-color", false, "")
		server      = fs.String("server", "", "")
		showIP      = fs.Bool("show-ip", false, "")
		ipOnly      = fs.Bool("ip", false, "")
		showVersion = fs.Bool("version", false, "")
	)
	// The standard flag package prints the full usage for every parse error.
	// Puls keeps errors concise and renders help only when it was requested.
	fs.Usage = func() {}
	if err := fs.Parse(args); err != nil {
		if errors.Is(err, flag.ErrHelp) {
			printHelp(os.Stderr, ui.NewStyle(ui.ColorEnabled(os.Stderr, *noColor)))
			return 0
		}
		return printUsageError(ui.NewStyle(ui.ColorEnabled(os.Stderr, *noColor)), friendlyFlagError(err))
	}
	errStyle := ui.NewStyle(ui.ColorEnabled(os.Stderr, *noColor))
	if fs.NArg() != 0 {
		return printUsageError(errStyle, fmt.Sprintf("неизвестная команда: %s", fs.Arg(0)))
	}
	if *showVersion {
		fmt.Printf("Puls %s\n", version)
		return 0
	}
	if *ipOnly {
		if providerKey != "" && providerKey != "yandex" {
			return printUsageError(errStyle, "--ip доступен только для источника yandex")
		}
		return runIPOnly(*jsonOut, *noColor, *verbose)
	}
	durationFlagSet := false
	fs.Visit(func(f *flag.Flag) {
		switch f.Name {
		case "duration":
			durationFlagSet = true
		}
	})

	profileKey := strings.ToLower(strings.TrimSpace(*profile))
	profileDuration, ok := map[string]int{"quick": 5, "balanced": 10, "accurate": 15}[profileKey]
	if !ok {
		return printUsageError(errStyle, fmt.Sprintf("неизвестный профиль %q; выберите quick, balanced или accurate", *profile))
	}
	seconds := profileDuration
	if durationFlagSet {
		seconds = *duration
	}
	if seconds < 3 || seconds > 60 {
		return printUsageError(errStyle, "длительность должна быть от 3 до 60 секунд")
	}
	if *connections < 0 || *connections > 16 {
		return printUsageError(errStyle, "число соединений должно быть 0 (авто) или от 1 до 16")
	}
	doPing, doDownload, doUpload := true, true, true
	onlyKey := strings.ToLower(strings.TrimSpace(*only))
	switch onlyKey {
	case "all":
	case "ping":
		doDownload, doUpload = false, false
	case "download":
		doPing, doUpload = false, false
	case "upload":
		doPing, doDownload = false, false
	default:
		return printUsageError(errStyle, fmt.Sprintf("неизвестный этап %q; выберите all, ping, download или upload", *only))
	}

	printHuman := !*jsonOut
	live := printHuman && ui.IsTerminal(os.Stdout)
	style := ui.NewStyle(printHuman && ui.ColorEnabled(os.Stdout, *noColor))
	var output io.Writer = io.Discard
	if printHuman {
		output = os.Stdout
	}

	if providerKey == "" && live && ui.IsTerminal(os.Stdin) {
		key, err := pickProvider(style)
		if err != nil {
			if errors.Is(err, ui.ErrCanceled) {
				return 130
			}
			fmt.Fprintln(os.Stderr, "интерактивный выбор недоступен:", err)
		} else {
			providerKey = key
		}
	}
	if providerKey == "" {
		providerKey = "yandex"
	}
	if *server != "" && providerKey != "speedtest" {
		return printUsageError(errStyle, "--server можно использовать только с источником speedtest")
	}

	var providers []provider.Provider
	switch providerKey {
	case "yandex":
		providers = []provider.Provider{yandex.New()}
	case "speedtest":
		providers = []provider.Provider{speedtestru.New(*server)}
	case "all":
		providers = []provider.Provider{yandex.New(), speedtestru.New("")}
	default:
		return printUsageError(errStyle, fmt.Sprintf("неизвестный источник %q; выберите yandex, speedtest или all", providerKey))
	}

	var verbosef func(string, ...any)
	if *verbose {
		verbosef = func(format string, values ...any) {
			fmt.Fprint(os.Stderr, errStyle.Dim("подробно"), "  ")
			fmt.Fprintf(os.Stderr, format+"\n", values...)
		}
	}
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	cfg := provider.MeasurementConfig{
		Duration: time.Duration(seconds) * time.Second, Connections: *connections,
		MaxConnections: 16, Verbose: verbosef,
	}

	results := make([]runResult, 0, len(providers))
	for _, backend := range providers {
		if configurable, ok := backend.(interface{ SetVerbose(func(string, ...any)) }); ok {
			configurable.SetVerbose(verbosef)
		}
		results = append(results, runProvider(ctx, backend, output, style, live, cfg, doPing, doDownload, doUpload, *showIP))
		if ctx.Err() != nil {
			break
		}
	}
	if printHuman && len(providers) > 1 {
		printSummary(output, style, results)
	}

	if *jsonOut {
		if err := writeJSON(os.Stdout, results); err != nil {
			fmt.Fprintln(os.Stderr, "ошибка записи JSON:", err)
			return exitCode(ctx.Err(), results, err)
		}
	}
	code := exitCode(ctx.Err(), results, nil)
	if code == 130 {
		fmt.Fprintln(os.Stderr, style.Dim("прервано пользователем"))
	}
	return code
}

type ipOnlyResult struct {
	ExternalIP *string `json:"external_ip"`
	Error      string  `json:"error,omitempty"`
}

// runIPOnly implements --ip: a single-purpose lookup that skips server
// selection and every measurement phase, unlike --show-ip which only adds
// the address to an otherwise normal run.
func runIPOnly(jsonOut, noColor, verbose bool) int {
	printHuman := !jsonOut
	live := printHuman && ui.IsTerminal(os.Stdout)
	style := ui.NewStyle(printHuman && ui.ColorEnabled(os.Stdout, noColor))
	var output io.Writer = io.Discard
	if printHuman {
		output = os.Stdout
	}
	var verbosef func(string, ...any)
	if verbose {
		verbosef = func(format string, values ...any) {
			fmt.Fprint(os.Stderr, style.Dim("подробно"), "  ")
			fmt.Fprintf(os.Stderr, format+"\n", values...)
		}
	}
	return runIPOnlyWithDetector(yandex.New(), output, os.Stdout, style, live, jsonOut, verbosef)
}

func runIPOnlyWithDetector(detector externalIPProvider, output, jsonWriter io.Writer, style *ui.Style, live, jsonOut bool, verbosef func(string, ...any)) int {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	line := ui.NewLine(output, live)
	ip, err := ui.Spin(line, "определение внешнего IP…", func() (string, error) {
		attemptCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
		defer cancel()
		return detector.DetectExternalIP(attemptCtx)
	})

	result := ipOnlyResult{}
	if err != nil {
		if verbosef != nil {
			verbosef("внешний IP: %v", err)
		}
		result.Error = humanError(err)
	} else {
		result.ExternalIP = &ip
	}

	switch {
	case err == nil:
		line.Final(metricLabel(style, "Внешний IP") + ip)
	case errors.Is(err, context.Canceled):
		line.Final(metricLabel(style, "Внешний IP") + style.Dim("остановлено"))
	default:
		line.Final(metricLabel(style, "Внешний IP") + phaseFailure(style, err))
	}
	if jsonOut {
		if encodeErr := json.NewEncoder(jsonWriter).Encode(result); encodeErr != nil {
			fmt.Fprintln(os.Stderr, "ошибка записи JSON:", encodeErr)
			return 1
		}
	}

	switch {
	case err == nil:
		return 0
	case errors.Is(err, context.Canceled):
		fmt.Fprintln(os.Stderr, style.Dim("прервано пользователем"))
		return 130
	default:
		return 1
	}
}

func writeJSON(output io.Writer, results []runResult) error {
	encoder := json.NewEncoder(output)
	encoder.SetIndent("", "  ")
	if len(results) == 1 {
		return encoder.Encode(results[0])
	}
	return encoder.Encode(results)
}

func exitCode(contextErr error, results []runResult, outputErr error) int {
	if outputErr != nil {
		return 1
	}
	if errors.Is(contextErr, context.Canceled) {
		return 130
	}
	for _, result := range results {
		if result.Status != "ok" {
			return 1
		}
	}
	return 0
}

var providerChoices = []struct{ key, label, hint string }{
	{"yandex", "Яндекс.Интернетометр", "полная проверка"},
	{"speedtest", "speedtest.ru", "полная проверка"},
	{"all", "Все источники", "последовательно"},
}

func pickProvider(style *ui.Style) (string, error) {
	opts := make([]ui.Option, len(providerChoices))
	for i, choice := range providerChoices {
		opts[i] = ui.Option{Label: choice.label, Hint: choice.hint}
	}
	index, err := ui.Select(os.Stdin, os.Stdout, style, "Puls · источник измерения", opts)
	if err != nil {
		return "", err
	}
	return providerChoices[index].key, nil
}

type phaseResult struct {
	Status                string   `json:"status"`
	ValueMs               *float64 `json:"value_ms"`
	MinMs                 *float64 `json:"min_ms"`
	MedianMs              *float64 `json:"median_ms"`
	AverageMs             *float64 `json:"average_ms"`
	JitterMs              *float64 `json:"jitter_ms"`
	Samples               int      `json:"samples,omitempty"`
	Method                string   `json:"method,omitempty"`
	Mbps                  *float64 `json:"mbps"`
	Bytes                 int64    `json:"bytes,omitempty"`
	ElapsedMs             *float64 `json:"elapsed_ms"`
	SuccessfulConnections int      `json:"successful_streams,omitempty"`
	FailedConnections     int      `json:"failed_streams,omitempty"`
	Warnings              []string `json:"warnings,omitempty"`
	ErrorCode             string   `json:"error_code,omitempty"`
	Error                 string   `json:"error,omitempty"`
}

type phasesResult struct {
	Select   phaseResult `json:"select"`
	Ping     phaseResult `json:"ping"`
	Download phaseResult `json:"download"`
	Upload   phaseResult `json:"upload"`
}

type runResult struct {
	Provider   string       `json:"provider"`
	Server     string       `json:"server,omitempty"`
	Status     string       `json:"status"`
	PingMs     *float64     `json:"ping_ms"`
	JitterMs   *float64     `json:"jitter_ms"`
	Download   *float64     `json:"download_mbps"`
	Upload     *float64     `json:"upload_mbps"`
	ExternalIP *string      `json:"external_ip"`
	Phases     phasesResult `json:"phases"`
	ErrorCode  string       `json:"error_code,omitempty"`
	Err        string       `json:"error,omitempty"`
	Warnings   []string     `json:"warnings,omitempty"`
}

func newRunResult(name string, ping, download, upload bool) runResult {
	result := runResult{Provider: name}
	result.Phases.Select.Status = "pending"
	result.Phases.Ping.Status = "skipped"
	result.Phases.Download.Status = "skipped"
	result.Phases.Upload.Status = "skipped"
	if ping {
		result.Phases.Ping.Status = "pending"
	}
	if download {
		result.Phases.Download.Status = "pending"
	}
	if upload {
		result.Phases.Upload.Status = "pending"
	}
	return result
}

func withRetry(ctx context.Context, attempts int, fn func() error) error {
	return withRetryBackoff(ctx, attempts, 300*time.Millisecond, fn)
}

func withRetryBackoff(ctx context.Context, attempts int, baseDelay time.Duration, fn func() error) error {
	if attempts < 1 {
		return errors.New("число попыток должно быть положительным")
	}
	var err error
	for i := 0; i < attempts; i++ {
		if contextErr := ctx.Err(); contextErr != nil {
			return contextErr
		}
		if err = fn(); err == nil {
			return err
		}
		if contextErr := ctx.Err(); contextErr != nil {
			return contextErr
		}
		var operationError *provider.OpError
		if errors.As(err, &operationError) && !operationError.Retryable {
			return err
		}
		if i < attempts-1 {
			timer := time.NewTimer(time.Duration(i+1) * baseDelay)
			select {
			case <-timer.C:
			case <-ctx.Done():
				if !timer.Stop() {
					select {
					case <-timer.C:
					default:
					}
				}
				return ctx.Err()
			}
		}
	}
	return err
}

func formatServer(server provider.Server) string {
	switch {
	case server.City != "" && server.Region != "":
		return fmt.Sprintf("%s (%s, %s)", server.Name, server.City, server.Region)
	case server.City != "":
		return fmt.Sprintf("%s (%s)", server.Name, server.City)
	default:
		return server.Name
	}
}

func displayProviderName(name string) string {
	switch name {
	case "yandex":
		return "Яндекс"
	case "speedtest.ru":
		return "speedtest.ru"
	default:
		return name
	}
}

func metricLabel(style *ui.Style, label string) string {
	return rowIndent + style.Dim(ui.PadLabel(label, labelWidth))
}

func phaseFailure(style *ui.Style, err error) string {
	return style.Red("не удалось") + style.Dim(" · "+humanError(err))
}

func humanError(err error) string {
	if err == nil {
		return "неизвестная ошибка"
	}
	message := err.Error()
	replacements := []struct{ old, new string }{
		{"context deadline exceeded", "истекло время ожидания"},
		{"context canceled", "операция отменена"},
		{"i/o timeout", "истекло время ожидания"},
		{"connection refused", "в соединении отказано"},
		{"connection reset by peer", "соединение сброшено другой стороной"},
		{"unexpected EOF", "неожиданный конец ответа"},
		{"bad handshake", "не удалось согласовать соединение"},
	}
	for _, replacement := range replacements {
		message = strings.ReplaceAll(message, replacement.old, replacement.new)
	}
	return message
}

func localizedWarnings(warnings []string) []string {
	if len(warnings) == 0 {
		return nil
	}
	localized := make([]string, len(warnings))
	for i, warning := range warnings {
		localized[i] = humanError(errors.New(warning))
	}
	return localized
}

func appendUniqueWarnings(destination []string, warnings []string) []string {
	for _, warning := range localizedWarnings(warnings) {
		if warning == "" {
			continue
		}
		found := false
		for _, existing := range destination {
			if existing == warning {
				found = true
				break
			}
		}
		if !found {
			destination = append(destination, warning)
		}
	}
	return destination
}

func finishProvider(output io.Writer, style *ui.Style, result runResult) {
	seen := make(map[string]struct{}, len(result.Warnings))
	for _, warning := range result.Warnings {
		if warning == "" {
			continue
		}
		if _, ok := seen[warning]; ok {
			continue
		}
		seen[warning] = struct{}{}
		fmt.Fprintf(output, "%s%s %s\n", rowIndent, style.Yellow("!"), style.Dim(humanError(errors.New(warning))))
	}

	icon, text := "✓", "Готово"
	paint := style.Green
	switch result.Status {
	case "partial":
		icon, text, paint = "!", "Частичный результат", style.Yellow
	case "error":
		icon, text, paint = "×", "Замер не выполнен", style.Red
	case "canceled":
		icon, text, paint = "•", "Остановлено", style.Yellow
	}
	fmt.Fprintf(output, "%s%s %s\n\n", rowIndent, paint(icon), style.Dim(text))
}

func printSummary(output io.Writer, style *ui.Style, results []runResult) {
	ok, partial, failed := 0, 0, 0
	for _, result := range results {
		switch result.Status {
		case "ok":
			ok++
		case "partial":
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
	fmt.Fprintf(output, "%s  %s\n", style.Bold("Итог"), strings.Join(parts, style.Dim(" · ")))
}

// externalIPProvider is implemented by providers that can report the
// visitor's public IP address as detected by their own first-party
// endpoints. Detection is best-effort metadata, not a measurement phase: a
// failure never affects result.Status or exit code.
type externalIPProvider interface {
	DetectExternalIP(context.Context) (string, error)
}

func runProvider(ctx context.Context, backend provider.Provider, output io.Writer, style *ui.Style, live bool, cfg provider.MeasurementConfig, doPing, doDownload, doUpload, showIP bool) runResult {
	result := newRunResult(backend.Name(), doPing, doDownload, doUpload)
	fmt.Fprintf(output, "%s %s\n", style.Cyan("●"), style.Bold(displayProviderName(backend.Name())))

	selectLine := ui.NewLine(output, live)
	server, err := ui.Spin(selectLine, "выбор сервера…", func() (provider.Server, error) {
		var selected provider.Server
		err := withRetry(ctx, 2, func() error {
			attemptCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
			defer cancel()
			var selectErr error
			selected, selectErr = backend.SelectServer(attemptCtx)
			return selectErr
		})
		return selected, err
	})
	if err != nil {
		setPhaseError(&result, &result.Phases.Select, err)
		result.Phases.Ping.Status = "skipped"
		if doDownload {
			result.Phases.Download.Status = "skipped"
		}
		if doUpload {
			result.Phases.Upload.Status = "skipped"
		}
		selectLine.Final(metricLabel(style, "Сервер") + phaseFailure(style, err))
		result.Status = statusOf(result)
		finishProvider(output, style, result)
		return result
	}
	result.Phases.Select.Status = "ok"
	result.Server = server.Name
	if cfg.Verbose != nil {
		cfg.Verbose("источник=%s сервер=%s", displayProviderName(backend.Name()), server.Name)
	}
	selectLine.Final(metricLabel(style, "Сервер") + formatServer(server))

	if showIP {
		ipLine := ui.NewLine(output, live)
		if detector, ok := backend.(externalIPProvider); ok {
			ip, ipErr := ui.Spin(ipLine, "определение внешнего IP…", func() (string, error) {
				attemptCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
				defer cancel()
				return detector.DetectExternalIP(attemptCtx)
			})
			if ipErr != nil {
				if cfg.Verbose != nil {
					cfg.Verbose("внешний IP: %v", ipErr)
				}
				ipLine.Final(metricLabel(style, "Внешний IP") + style.Dim("недоступно"))
			} else {
				result.ExternalIP = &ip
				ipLine.Final(metricLabel(style, "Внешний IP") + ip)
			}
		} else {
			ipLine.Final(metricLabel(style, "Внешний IP") + style.Dim("не поддерживается"))
		}
	}

	if doPing {
		pingLine := ui.NewLine(output, live)
		ping, pingErr := ui.Spin(pingLine, "измерение задержки…", func() (provider.PingResult, error) {
			var measured provider.PingResult
			err := withRetry(ctx, 2, func() error {
				attemptCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
				defer cancel()
				var attemptErr error
				measured, attemptErr = backend.Ping(attemptCtx)
				return attemptErr
			})
			return measured, err
		})
		if pingErr != nil {
			setPhaseError(&result, &result.Phases.Ping, pingErr)
			pingLine.Final(metricLabel(style, "Задержка") + phaseFailure(style, pingErr))
		} else {
			result.Phases.Ping = pingPhase(ping)
			value, jitter := round2(ping.ValueMs), round2(ping.JitterMs)
			result.PingMs, result.JitterMs = &value, &jitter
			pingLine.Final(fmt.Sprintf("%s%s%s%s", metricLabel(style, "Задержка"), style.Latency(ping.ValueMs, "мс"), style.Dim("  ·  джиттер "), style.Latency(ping.JitterMs, "мс")))
		}
	}

	if doDownload {
		if ctx.Err() != nil {
			setPhaseError(&result, &result.Phases.Download, ctx.Err())
		} else if !backend.Capabilities().Has(provider.CapDownload) {
			result.Phases.Download.Status = "skipped"
			fmt.Fprintln(output, metricLabel(style, "Загрузка")+style.Dim("не поддерживается"))
		} else {
			measured, phaseErr := measurePhase(ctx, output, style, live, "Загрузка", cfg, func(progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
				return backend.Download(ctx, cfg, progress)
			})
			result.Phases.Download = throughputPhase(measured, phaseErr)
			result.Warnings = appendUniqueWarnings(result.Warnings, measured.Warnings)
			if phaseErr != nil {
				setPhaseError(&result, &result.Phases.Download, phaseErr)
			} else {
				value := round2(measured.Mbps)
				result.Download = &value
			}
		}
	}

	if doUpload {
		if ctx.Err() != nil {
			setPhaseError(&result, &result.Phases.Upload, ctx.Err())
		} else if !backend.Capabilities().Has(provider.CapUpload) {
			result.Phases.Upload.Status = "skipped"
			fmt.Fprintln(output, metricLabel(style, "Отдача")+style.Dim("не поддерживается"))
		} else {
			measured, phaseErr := measurePhase(ctx, output, style, live, "Отдача", cfg, func(progress func(provider.ThroughputProgress)) (provider.ThroughputResult, error) {
				return backend.Upload(ctx, cfg, progress)
			})
			result.Phases.Upload = throughputPhase(measured, phaseErr)
			result.Warnings = appendUniqueWarnings(result.Warnings, measured.Warnings)
			if phaseErr != nil {
				setPhaseError(&result, &result.Phases.Upload, phaseErr)
			} else {
				value := round2(measured.Mbps)
				result.Upload = &value
			}
		}
	}

	result.Status = statusOf(result)
	finishProvider(output, style, result)
	return result
}

func pingPhase(value provider.PingResult) phaseResult {
	primary, minValue := round2(value.ValueMs), round2(value.MinMs)
	median, average, jitter := round2(value.MedianMs), round2(value.AvgMs), round2(value.JitterMs)
	return phaseResult{
		Status: "ok", ValueMs: &primary, MinMs: &minValue, MedianMs: &median,
		AverageMs: &average, JitterMs: &jitter, Samples: value.Samples, Method: value.Method,
	}
}

func throughputPhase(value provider.ThroughputResult, err error) phaseResult {
	result := phaseResult{
		Status: "error", Bytes: value.Bytes,
		SuccessfulConnections: value.SuccessfulConnections, FailedConnections: value.FailedConnections,
		Warnings: localizedWarnings(value.Warnings),
	}
	if value.Elapsed > 0 {
		elapsed := round2(float64(value.Elapsed) / float64(time.Millisecond))
		result.ElapsedMs = &elapsed
	}
	if err == nil {
		mbps := round2(value.Mbps)
		result.Status = "ok"
		result.Mbps = &mbps
	}
	return result
}

func setPhaseError(result *runResult, phase *phaseResult, err error) {
	phase.Status = "error"
	if errors.Is(err, context.Canceled) {
		phase.Status = "canceled"
	}
	errorText := humanError(err)
	phase.Error = errorText
	code := provider.CodeUnavailable
	var operationError *provider.OpError
	if errors.As(err, &operationError) {
		code = operationError.Code
	} else if errors.Is(err, context.Canceled) {
		code = provider.CodeCanceled
	} else if errors.Is(err, context.DeadlineExceeded) {
		code = provider.CodeTimeout
	}
	phase.ErrorCode = string(code)
	if result.ErrorCode == "" {
		result.ErrorCode = string(code)
	}
	if result.Err == "" {
		result.Err = errorText
	} else if !strings.Contains(result.Err, errorText) {
		result.Err += "; " + errorText
	}
}

func statusOf(result runResult) string {
	if result.Phases.Select.Status == "canceled" {
		return "canceled"
	}
	phases := []phaseResult{result.Phases.Ping, result.Phases.Download, result.Phases.Upload}
	succeeded, failed := 0, 0
	for _, phase := range phases {
		switch phase.Status {
		case "ok":
			succeeded++
		case "error":
			failed++
		case "canceled":
			return "canceled"
		}
	}
	if result.Phases.Select.Status == "error" {
		return "error"
	}
	if failed == 0 {
		return "ok"
	}
	if succeeded > 0 {
		return "partial"
	}
	return "error"
}

func measurePhase(ctx context.Context, output io.Writer, style *ui.Style, live bool, label string, cfg provider.MeasurementConfig, run func(func(provider.ThroughputProgress)) (provider.ThroughputResult, error)) (provider.ThroughputResult, error) {
	line := ui.NewLine(output, live)
	progress := func(value provider.ThroughputProgress) {
		fraction := max(0, min(1, float64(value.Elapsed)/float64(cfg.Duration)))
		line.Update(fmt.Sprintf("%s%s %3.0f%%  %s", metricLabel(style, label), style.Cyan(ui.Bar(18, fraction)), fraction*100, style.Speed(value.Mbps)))
	}
	result, err := run(progress)
	if err != nil {
		if errors.Is(err, context.Canceled) {
			line.Final(metricLabel(style, label) + style.Dim("остановлено"))
		} else {
			line.Final(metricLabel(style, label) + phaseFailure(style, err))
		}
		return result, err
	}
	line.Final(metricLabel(style, label) + style.Speed(result.Mbps))
	return result, nil
}

func round2(value float64) float64 { return math.Round(value*100) / 100 }
