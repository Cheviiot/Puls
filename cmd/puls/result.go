package main

import (
	"io"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

type envelope = appcore.Envelope
type connectionResult = appcore.ConnectionResult
type measurementResult = appcore.MeasurementResult
type phaseResult = appcore.PhaseResult

func newEnvelope(command command) envelope {
	return appcore.NewEnvelope(appcore.Command(command))
}

func newMeasurementResult(id service.ServiceID, selection phaseSelection) measurementResult {
	return appcore.NewMeasurementResult(id, appcore.PhaseSelection(selection))
}

func throughputPhase(value service.ThroughputResult, err error) phaseResult {
	return appcore.ThroughputPhase(value, err)
}

func writeJSON(writer io.Writer, result envelope) error { return appcore.WriteJSON(writer, result) }

func humanError(err error) string { return appcore.HumanError(err) }

func round2(value float64) float64 { return appcore.Round2(value) }
