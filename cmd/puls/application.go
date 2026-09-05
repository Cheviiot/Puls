package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/gui"
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
	launchGUI        func(context.Context, gui.Options) error
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
		launchGUI: gui.Run,
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
	if cfg.Command == commandGUI {
		err := app.launchGUI(ctx, gui.Options{Version: version, Log: log})
		if err == nil {
			return 0
		}
		fmt.Fprintf(app.stderr, "ошибка запуска GUI: %v\n", err)
		if errors.Is(err, gui.ErrUnavailable) {
			return 2
		}
		return 1
	}
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
	runner := app.runner(log)
	var observer appcore.Observer
	if !cfg.JSON {
		terminal := newTerminalObserver(app.stdout, style, app.outputTTY, 0, 0)
		observer = terminal.Observe
	}
	connection := runner.DetectConnection(ctx, appcore.ConnectionRequest{Service: cfg.Service, Explicit: cfg.ServiceExplicit}, observer)
	result.Connection = &connection
	result.Status = connection.Status

	if cfg.JSON {
		if writeErr := writeJSON(app.stdout, result); writeErr != nil {
			fmt.Fprintf(app.stderr, "ошибка записи JSON: %v\n", writeErr)
			return 1
		}
	}
	if result.Status == service.StatusCanceled {
		return 130
	}
	if result.Status != service.StatusOK {
		return 1
	}
	return 0
}

func (app *application) runMeasurements(ctx context.Context, cfg config, style *ui.Style, log service.LogFunc) int {
	runner := app.runner(log)
	var observer appcore.Observer
	if !cfg.JSON {
		terminal := newTerminalObserver(app.stdout, style, app.outputTTY, ui.ProgressWidth(app.stdoutFile), cfg.Duration)
		observer = terminal.Observe
	}
	result := runner.Measure(ctx, appcore.MeasureRequest{
		Service:        cfg.Service,
		Profile:        appcore.Profile(cfg.Profile),
		Duration:       cfg.Duration,
		Connections:    cfg.Connections,
		Only:           appcore.PhaseSelection(cfg.Only),
		Server:         cfg.Server,
		ShowConnection: cfg.ShowIP,
	}, observer)
	if !cfg.JSON && len(result.Results) > 1 {
		renderSummary(app.stdout, style, result.Results)
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

func (app *application) runner(log service.LogFunc) *appcore.Runner {
	return appcore.NewRunner(appcore.Options{
		Log: log,
		YandexFactory: func(_ string, log service.LogFunc) service.Backend {
			return app.yandexFactory(log)
		},
		SpeedtestFactory: func(server string, log service.LogFunc) service.Backend {
			return app.speedtestFactory(server, log)
		},
	})
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
