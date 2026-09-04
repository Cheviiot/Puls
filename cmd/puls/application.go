package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"

	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/service/speedtestru"
	"github.com/Cheviiot/Puls/internal/service/yandex"
	"github.com/Cheviiot/Puls/internal/ui"
)

type backendFactory func(service.LogFunc) service.Backend

type application struct {
	in         *os.File
	stdout     io.Writer
	stderr     io.Writer
	stdoutFile *os.File
	inputTTY   bool
	outputTTY  bool

	yandexFactory    backendFactory
	speedtestFactory func(string, service.LogFunc) service.Backend
	selectService    func(*ui.Style) (service.ServiceID, error)
}

func newApplication(in *os.File, stdout, stderr io.Writer) *application {
	app := &application{
		in:       in,
		stdout:   stdout,
		stderr:   stderr,
		inputTTY: ui.IsTerminal(in),
		yandexFactory: func(log service.LogFunc) service.Backend {
			return yandex.New(yandex.Options{Log: log})
		},
		speedtestFactory: func(server string, log service.LogFunc) service.Backend {
			return speedtestru.New(speedtestru.Options{Server: server, Log: log})
		},
	}
	if file, ok := stdout.(*os.File); ok {
		app.stdoutFile = file
		app.outputTTY = ui.IsTerminal(file)
	}
	app.selectService = func(style *ui.Style) (service.ServiceID, error) {
		if app.stdoutFile == nil {
			return "", errors.New("интерактивный вывод недоступен")
		}
		return selectMeasurementService(app.in, app.stdoutFile, style)
	}
	return app
}

func (app *application) Run(ctx context.Context, args []string) int {
	cfg, err := parseConfig(args)
	if err != nil {
		return app.printUsageError(cfg.NoColor, err)
	}

	style := ui.NewStyle(!cfg.JSON && app.outputTTY && ui.ColorEnabled(app.stdoutFile, cfg.NoColor))
	switch cfg.Command {
	case commandHelp:
		printHelp(app.stdout, style)
		return 0
	case commandVersion:
		fmt.Fprintf(app.stdout, "Puls %s\n", version)
		return 0
	}

	log := app.logger(cfg.Verbose, cfg.NoColor)
	if cfg.Command == commandMeasure && cfg.Service == "" {
		if !cfg.JSON && app.inputTTY && app.outputTTY {
			selected, selectErr := app.selectService(style)
			if selectErr == nil {
				cfg.Service = selected
				cfg.ServiceExplicit = true
			} else if errors.Is(selectErr, ui.ErrCanceled) || errors.Is(selectErr, context.Canceled) {
				return 130
			} else if log != nil {
				log("интерактивный выбор недоступен: %v", selectErr)
			}
		}
		if cfg.Service == "" {
			cfg.Service = service.Yandex
		}
	}

	if cfg.Command == commandIP {
		return app.runIP(ctx, cfg, style, log)
	}
	return app.runMeasurements(ctx, cfg, style, log)
}

func (app *application) runIP(ctx context.Context, cfg config, style *ui.Style, log service.LogFunc) int {
	result := newEnvelope(commandIP)
	connection, err := app.detectConnectionWithProgress(ctx, cfg, style, cfg.Service, cfg.ServiceExplicit, log)
	result.Connection = &connection
	result.Status = connection.Status

	if cfg.JSON {
		if writeErr := writeJSON(app.stdout, result); writeErr != nil {
			fmt.Fprintf(app.stderr, "ошибка записи JSON: %v\n", writeErr)
			return 1
		}
	} else {
		renderConnection(app.stdout, style, connection)
	}
	if errors.Is(err, context.Canceled) || result.Status == service.StatusCanceled {
		return 130
	}
	if err != nil {
		return 1
	}
	return 0
}

func (app *application) runMeasurements(ctx context.Context, cfg config, style *ui.Style, log service.LogFunc) int {
	result := newEnvelope(commandMeasure)
	if cfg.ShowIP {
		connectionService := cfg.Service
		explicit := true
		if cfg.Service == service.All {
			connectionService, explicit = "", false
		}
		connection, _ := app.detectConnectionWithProgress(ctx, cfg, style, connectionService, explicit, log)
		result.Connection = &connection
		if !cfg.JSON {
			renderConnection(app.stdout, style, connection)
		}
	}

	backends := app.backends(cfg, log)
	output := app.stdout
	if cfg.JSON {
		output = io.Discard
	}
	measurementConfig := service.MeasurementConfig{
		Duration: cfg.Duration, Connections: cfg.Connections, MaxConnections: 16,
	}
	for _, backend := range backends {
		measured := runMeasurement(ctx, backend, output, style, app.outputTTY && !cfg.JSON, ui.ProgressWidth(app.stdoutFile), measurementConfig, cfg.Only, log)
		result.Results = append(result.Results, measured)
		if ctx.Err() != nil {
			break
		}
	}
	result.Status = aggregateStatus(result.Results)
	if errors.Is(ctx.Err(), context.Canceled) {
		result.Status = service.StatusCanceled
	}
	if !cfg.JSON && len(result.Results) > 1 {
		renderSummary(output, style, result.Results)
	}
	if cfg.JSON {
		if writeErr := writeJSON(app.stdout, result); writeErr != nil {
			fmt.Fprintf(app.stderr, "ошибка записи JSON: %v\n", writeErr)
			return 1
		}
	}
	if errors.Is(ctx.Err(), context.Canceled) || result.Status == service.StatusCanceled {
		fmt.Fprintln(app.stderr, "операция прервана")
		return 130
	}
	if result.Status != service.StatusOK {
		return 1
	}
	return 0
}

func (app *application) backends(cfg config, log service.LogFunc) []service.Backend {
	switch cfg.Service {
	case service.Speedtest:
		return []service.Backend{app.speedtestFactory(cfg.Server, log)}
	case service.All:
		return []service.Backend{app.yandexFactory(log), app.speedtestFactory("", log)}
	default:
		return []service.Backend{app.yandexFactory(log)}
	}
}

func (app *application) logger(enabled, noColor bool) service.LogFunc {
	if !enabled {
		return nil
	}
	style := ui.NewStyle(false)
	if file, ok := app.stderr.(*os.File); ok {
		style = ui.NewStyle(ui.ColorEnabled(file, noColor))
	}
	return func(format string, args ...any) {
		fmt.Fprint(app.stderr, style.Dim("подробно"), "  ")
		fmt.Fprintf(app.stderr, format+"\n", args...)
	}
}

func (app *application) printUsageError(noColor bool, err error) int {
	style := ui.NewStyle(false)
	if file, ok := app.stderr.(*os.File); ok {
		style = ui.NewStyle(ui.ColorEnabled(file, noColor))
	}
	message := err.Error()
	fmt.Fprintf(app.stderr, "%s %s\n", style.Red("Ошибка:"), message)
	fmt.Fprintln(app.stderr, style.Dim("Подсказка: puls help"))
	return 2
}

func selectMeasurementService(in, out *os.File, style *ui.Style) (service.ServiceID, error) {
	choices := []struct {
		id    service.ServiceID
		label string
		hint  string
	}{
		{service.Yandex, "Яндекс.Интернетометр", "CDN Яндекса"},
		{service.Speedtest, "speedtest.ru", "серверы speedtest.ru"},
		{service.All, "Все сервисы", "последовательно"},
	}
	options := make([]ui.Option, len(choices))
	for index, choice := range choices {
		options[index] = ui.Option{Label: choice.label, Hint: choice.hint}
	}
	selected, err := ui.Select(in, out, style, "Puls · сервис измерения", options)
	if err != nil {
		return "", err
	}
	return choices[selected].id, nil
}
