package gui

import (
	"errors"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

var ErrUnavailable = errors.New("графический интерфейс недоступен в этой сборке")

type RunnerFactory func(service.LogFunc) *appcore.Runner

type Options struct {
	Version       string
	Log           service.LogFunc
	RunnerFactory RunnerFactory
}
