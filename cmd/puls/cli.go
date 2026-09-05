package main

import (
	"errors"
	"flag"
	"fmt"
	"io"
	"strconv"
	"strings"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
)

type command string

const (
	commandMeasure command = "measure"
	commandIP      command = "ip"
	commandGUI     command = "gui"
	commandHelp    command = "help"
	commandVersion command = "version"
)

type phaseSelection string

const (
	phaseAll      phaseSelection = "all"
	phasePing     phaseSelection = "ping"
	phaseDownload phaseSelection = "download"
	phaseUpload   phaseSelection = "upload"
)

type config struct {
	Command         command
	Service         service.ServiceID
	ServiceExplicit bool
	Profile         string
	Duration        time.Duration
	Connections     int
	Only            phaseSelection
	Server          string
	ShowIP          bool
	JSON            bool
	Verbose         bool
	NoColor         bool
}

type usageError struct{ message string }

func (e *usageError) Error() string { return e.message }

func parseConfig(args []string) (config, error) {
	result := config{Command: commandMeasure, Profile: "balanced", Only: phaseAll}
	if len(args) == 0 {
		result.Duration = 10 * time.Second
		return result, nil
	}

	remaining := args
	if !strings.HasPrefix(args[0], "-") {
		switch strings.ToLower(strings.TrimSpace(args[0])) {
		case "help":
			if len(args) != 1 {
				return config{}, newUsageError("команда help не принимает аргументы")
			}
			result.Command = commandHelp
			return result, nil
		case "version":
			if len(args) != 1 {
				return config{}, newUsageError("команда version не принимает аргументы")
			}
			result.Command = commandVersion
			return result, nil
		case "ip":
			result.Command = commandIP
			remaining = args[1:]
			if len(remaining) > 0 && !strings.HasPrefix(remaining[0], "-") {
				id, ok := parseServiceID(remaining[0])
				if !ok || id == service.All {
					return config{}, newUsageError(fmt.Sprintf("неизвестный сервис %q; выберите speedtest или yandex", remaining[0]))
				}
				result.Service, result.ServiceExplicit = id, true
				remaining = remaining[1:]
			}
			return parseIPFlags(result, remaining)
		case "gui":
			result.Command = commandGUI
			return parseGUIFlags(result, args[1:])
		default:
			id, ok := parseServiceID(args[0])
			if !ok {
				return config{}, newUsageError(fmt.Sprintf("неизвестная команда %q; выберите yandex, speedtest, all или ip", args[0]))
			}
			result.Service, result.ServiceExplicit = id, true
			remaining = args[1:]
		}
	}
	return parseMeasureFlags(result, remaining)
}

func parseGUIFlags(result config, args []string) (config, error) {
	fs := newFlagSet("gui")
	fs.BoolVar(&result.Verbose, "verbose", false, "")
	showHelp := fs.Bool("help", false, "")
	shortHelp := fs.Bool("h", false, "")
	showVersion := fs.Bool("version", false, "")
	if err := fs.Parse(args); err != nil {
		return config{}, flagUsageError(err)
	}
	if *showHelp || *shortHelp {
		result.Command = commandHelp
		return result, nil
	}
	if *showVersion {
		result.Command = commandVersion
		return result, nil
	}
	if fs.NArg() != 0 {
		return config{}, newUsageError(fmt.Sprintf("неизвестный аргумент %q", fs.Arg(0)))
	}
	return result, nil
}

func parseIPFlags(result config, args []string) (config, error) {
	fs := newFlagSet("ip")
	fs.BoolVar(&result.JSON, "json", false, "")
	fs.BoolVar(&result.Verbose, "verbose", false, "")
	fs.BoolVar(&result.NoColor, "no-color", false, "")
	showHelp := fs.Bool("help", false, "")
	shortHelp := fs.Bool("h", false, "")
	showVersion := fs.Bool("version", false, "")
	if err := fs.Parse(args); err != nil {
		return config{}, flagUsageError(err)
	}
	if *showHelp || *shortHelp {
		result.Command = commandHelp
		return result, nil
	}
	if *showVersion {
		result.Command = commandVersion
		return result, nil
	}
	if fs.NArg() != 0 {
		return config{}, newUsageError(fmt.Sprintf("неизвестный аргумент %q", fs.Arg(0)))
	}
	return result, nil
}

func parseMeasureFlags(result config, args []string) (config, error) {
	fs := newFlagSet("measure")
	duration := fs.String("duration", "", "")
	fs.StringVar(&result.Profile, "profile", result.Profile, "")
	fs.IntVar(&result.Connections, "connections", 0, "")
	only := fs.String("only", string(result.Only), "")
	fs.StringVar(&result.Server, "server", "", "")
	fs.BoolVar(&result.ShowIP, "show-ip", false, "")
	fs.BoolVar(&result.JSON, "json", false, "")
	fs.BoolVar(&result.Verbose, "verbose", false, "")
	fs.BoolVar(&result.NoColor, "no-color", false, "")
	showVersion := fs.Bool("version", false, "")
	showHelp := fs.Bool("help", false, "")
	shortHelp := fs.Bool("h", false, "")
	if err := fs.Parse(args); err != nil {
		return config{}, flagUsageError(err)
	}
	if *showHelp || *shortHelp {
		result.Command = commandHelp
		return result, nil
	}
	if *showVersion {
		result.Command = commandVersion
		return result, nil
	}
	if fs.NArg() != 0 {
		return config{}, newUsageError(fmt.Sprintf("неизвестный аргумент %q", fs.Arg(0)))
	}

	profile := strings.ToLower(strings.TrimSpace(result.Profile))
	seconds, ok := map[string]int{"quick": 5, "balanced": 10, "accurate": 15}[profile]
	if !ok {
		return config{}, newUsageError(fmt.Sprintf("неизвестный профиль %q; выберите quick, balanced или accurate", result.Profile))
	}
	result.Profile = profile
	if *duration != "" {
		value, err := strconv.Atoi(*duration)
		if err != nil {
			return config{}, newUsageError(fmt.Sprintf("неверная длительность %q: требуется целое число секунд", *duration))
		}
		seconds = value
	}
	if seconds < 3 || seconds > 60 {
		return config{}, newUsageError("длительность должна быть от 3 до 60 секунд")
	}
	result.Duration = time.Duration(seconds) * time.Second
	if result.Connections < 0 || result.Connections > 16 {
		return config{}, newUsageError("число соединений должно быть 0 (авто) или от 1 до 16")
	}
	result.Only = phaseSelection(strings.ToLower(strings.TrimSpace(*only)))
	switch result.Only {
	case phaseAll, phasePing, phaseDownload, phaseUpload:
	default:
		return config{}, newUsageError(fmt.Sprintf("неизвестный этап %q; выберите all, ping, download или upload", *only))
	}
	if result.Server != "" && result.Service != service.Speedtest {
		return config{}, newUsageError("--server можно использовать только с сервисом speedtest")
	}
	return result, nil
}

func newFlagSet(name string) *flag.FlagSet {
	fs := flag.NewFlagSet(name, flag.ContinueOnError)
	fs.SetOutput(io.Discard)
	fs.Usage = func() {}
	return fs
}

func parseServiceID(value string) (service.ServiceID, bool) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case string(service.Yandex):
		return service.Yandex, true
	case string(service.Speedtest):
		return service.Speedtest, true
	case "all":
		return service.All, true
	default:
		return "", false
	}
}

func newUsageError(message string) error { return &usageError{message: message} }

func flagUsageError(err error) error {
	if errors.Is(err, flag.ErrHelp) {
		return err
	}
	message := err.Error()
	message = strings.Replace(message, "flag provided but not defined: -", "неизвестный параметр: --", 1)
	message = strings.Replace(message, "flag needs an argument: -", "параметр требует значение: --", 1)
	message = strings.Replace(message, "invalid value ", "неверное значение ", 1)
	message = strings.Replace(message, " for flag -", " для параметра --", 1)
	return newUsageError(message)
}
