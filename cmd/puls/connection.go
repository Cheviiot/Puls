package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"time"

	"github.com/Cheviiot/Puls/internal/service"
	"github.com/Cheviiot/Puls/internal/ui"
)

const connectionTimeout = 15 * time.Second

func (app *application) detectConnectionWithProgress(
	ctx context.Context,
	cfg config,
	style *ui.Style,
	requested service.ServiceID,
	explicit bool,
	log service.LogFunc,
) (connectionResult, error) {
	output := app.stdout
	if cfg.JSON {
		output = io.Discard
	}
	line := ui.NewLine(output, app.outputTTY && !cfg.JSON)
	result, err := ui.Spin(line, "определение подключения…", func() (connectionResult, error) {
		return app.detectConnection(ctx, requested, explicit, log)
	})
	line.Update("")
	return result, err
}

func (app *application) detectConnection(ctx context.Context, requested service.ServiceID, explicit bool, log service.LogFunc) (connectionResult, error) {
	result := connectionResult{Status: service.StatusPending, Warnings: []string{}}
	services := []service.ServiceID{requested}
	if !explicit {
		services = []service.ServiceID{service.Speedtest, service.Yandex}
	}

	var lookupErrors []error
	for index, id := range services {
		backend := app.connectionBackend(id, log)
		detector, ok := backend.(service.ConnectionInfoBackend)
		if !ok {
			err := service.NewError(id, service.PhaseConnection, service.CodeInternal, false, errors.New("сервис не поддерживает определение подключения"))
			lookupErrors = append(lookupErrors, err)
			continue
		}
		attemptCtx, cancel := context.WithTimeout(ctx, connectionTimeout)
		info, err := detector.DetectConnection(attemptCtx)
		cancel()
		if err == nil && !info.ExternalIP.IsValid() {
			err = service.NewError(id, service.PhaseConnection, service.CodeProtocol, false, errors.New("сервис не вернул действительный IP-адрес"))
		}
		if err != nil {
			lookupErrors = append(lookupErrors, err)
			if log != nil {
				log("данные подключения через %s: %v", service.DisplayName(id), err)
			}
			if errors.Is(err, context.Canceled) || errors.Is(ctx.Err(), context.Canceled) {
				result.Status = service.StatusCanceled
				result.Error = phaseError(service.PhaseConnection, context.Canceled)
				return result, context.Canceled
			}
			continue
		}

		ip := info.ExternalIP.String()
		result.Status = service.StatusOK
		result.ExternalIP = &ip
		result.ISP = stringPointer(info.ISP)
		result.DetectedBy = &id
		result.Warnings = appendUnique(result.Warnings, info.Warnings...)
		if log != nil {
			for _, warning := range info.Warnings {
				log("данные подключения через %s: %s", service.DisplayName(id), warning)
			}
		}
		if index > 0 {
			warning := fmt.Sprintf("использован резервный сервис %s", service.DisplayName(id))
			result.Warnings = appendUnique(result.Warnings, warning)
			if log != nil {
				log("fallback определения подключения: %s", service.DisplayName(id))
			}
		}
		return result, nil
	}

	err := errors.Join(lookupErrors...)
	if err == nil {
		err = errors.New("нет доступного сервиса определения подключения")
	}
	result.Status = service.StatusError
	result.Error = phaseError(service.PhaseConnection, err)
	return result, err
}

func (app *application) connectionBackend(id service.ServiceID, log service.LogFunc) service.Backend {
	if id == service.Yandex {
		return app.yandexFactory(log)
	}
	return app.speedtestFactory("", log)
}
