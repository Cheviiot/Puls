//go:build !nogui

package gui

import (
	"context"
	"errors"
	"strconv"
	"strings"
	"sync"
	"time"

	"fyne.io/fyne/v2"
	"fyne.io/fyne/v2/container"
	"fyne.io/fyne/v2/dialog"
	"fyne.io/fyne/v2/theme"
	"fyne.io/fyne/v2/widget"

	appcore "github.com/Cheviiot/Puls/internal/application"
	"github.com/Cheviiot/Puls/internal/service"
)

type measurementSettings struct {
	Service     service.ServiceID
	Profile     appcore.Profile
	Duration    time.Duration
	Connections int
	Only        appcore.PhaseSelection
	Server      string
	ShowIP      bool
	Theme       themeMode
}

type dashboard struct {
	root          context.Context
	application   fyne.App
	window        fyne.Window
	preferences   *userPreferences
	options       Options
	runnerFactory RunnerFactory
	settings      measurementSettings

	activeMu     sync.Mutex
	activeCancel context.CancelFunc

	content          fyne.CanvasObject
	serviceSelect    *widget.Select
	configLabel      *widget.Label
	settingsButton   *widget.Button
	themeButton      *widget.Button
	startButton      *widget.Button
	connectionButton *widget.Button
	statusLabel      *widget.Label
	currentValue     *widget.Label
	currentUnit      *widget.Label
	progress         *widget.ProgressBar
	serverLabel      *widget.Label
	connectionLabel  *widget.Label
	noticeLabel      *widget.Label
	resultBox        *fyne.Container
	metrics          map[service.Phase]*widget.Label
	jitter           *widget.Label
}

func newDashboard(
	ctx context.Context,
	application fyne.App,
	window fyne.Window,
	preferences *userPreferences,
	options Options,
	runnerFactory RunnerFactory,
) *dashboard {
	d := &dashboard{
		root: ctx, application: application, window: window, preferences: preferences,
		options: options, runnerFactory: runnerFactory,
		settings: measurementSettings{
			Service: preferences.Service, Profile: preferences.Profile, Duration: preferences.Duration,
			Connections: preferences.Connections, Only: preferences.Only, Theme: preferences.Theme,
		},
		metrics: make(map[service.Phase]*widget.Label),
	}
	d.build()
	return d
}

func (d *dashboard) build() {
	title := widget.NewLabelWithStyle("Puls", fyne.TextAlignLeading, fyne.TextStyle{Bold: true})
	title.SizeName = metricSmallSizeName
	version := widget.NewLabel("v" + strings.TrimPrefix(d.options.Version, "v"))
	version.Importance = widget.LowImportance

	d.themeButton = widget.NewButtonWithIcon("", theme.ColorPaletteIcon(), d.cycleTheme)
	d.themeButton.Importance = widget.LowImportance
	d.settingsButton = widget.NewButtonWithIcon("", theme.SettingsIcon(), d.showSettings)
	d.settingsButton.Importance = widget.LowImportance
	header := container.NewBorder(nil, nil, nil, container.NewHBox(d.themeButton, d.settingsButton), container.NewVBox(title, version))

	d.serviceSelect = widget.NewSelect(serviceLabels(), d.selectService)
	d.serviceSelect.SetSelected(serviceLabel(d.settings.Service))
	d.serviceSelect.Alignment = fyne.TextAlignCenter
	d.configLabel = widget.NewLabelWithStyle(settingsSummary(d.settings), fyne.TextAlignCenter, fyne.TextStyle{})
	d.configLabel.Importance = widget.LowImportance

	d.statusLabel = widget.NewLabelWithStyle("Готов к проверке", fyne.TextAlignCenter, fyne.TextStyle{})
	d.statusLabel.Importance = widget.LowImportance
	d.currentValue = widget.NewLabelWithStyle("—", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	d.currentValue.SizeName = metricSizeName
	d.currentUnit = widget.NewLabelWithStyle("Мбит/с", fyne.TextAlignCenter, fyne.TextStyle{})
	d.currentUnit.Importance = widget.LowImportance
	d.progress = widget.NewProgressBar()
	d.progress.TextFormatter = func() string { return "" }
	d.progress.Hide()
	current := container.NewVBox(d.statusLabel, d.currentValue, d.currentUnit, d.progress)

	pingCard, pingValue := metricCard("Задержка", "мс")
	jitterCard, jitterValue := metricCard("Джиттер", "мс")
	downloadCard, downloadValue := metricCard("Загрузка", "Мбит/с")
	uploadCard, uploadValue := metricCard("Отдача", "Мбит/с")
	d.metrics[service.PhasePing] = pingValue
	d.metrics[service.PhaseDownload] = downloadValue
	d.metrics[service.PhaseUpload] = uploadValue
	d.jitter = jitterValue
	metricGrid := container.New(&adaptiveGridLayout{breakpoint: 720, narrow: 2, wide: 4, gap: theme.Padding()}, pingCard, jitterCard, downloadCard, uploadCard)

	d.serverLabel = detailLabel("Сервер измерения ещё не выбран")
	serverCard := widget.NewCard("Сервер измерения", "", d.serverLabel)

	d.connectionLabel = detailLabel("IP и интернет-провайдер не определены")
	d.connectionButton = widget.NewButtonWithIcon("Определить подключение", theme.SearchIcon(), d.startConnectionLookup)
	d.connectionButton.Importance = widget.LowImportance
	connectionCard := widget.NewCard("Подключение", "", container.NewVBox(d.connectionLabel, d.connectionButton))

	d.noticeLabel = detailLabel("")
	d.noticeLabel.Importance = widget.WarningImportance
	d.noticeLabel.Hide()
	d.resultBox = container.NewVBox()

	body := container.NewVBox(
		d.serviceSelect,
		d.configLabel,
		widget.NewSeparator(),
		current,
		metricGrid,
		serverCard,
		connectionCard,
		d.noticeLabel,
		d.resultBox,
	)
	scroll := container.NewVScroll(container.NewPadded(body))

	d.startButton = widget.NewButtonWithIcon("Начать проверку", theme.MediaPlayIcon(), d.toggleMeasurement)
	d.startButton.Importance = widget.HighImportance
	footer := container.NewPadded(d.startButton)
	d.content = container.NewBorder(container.NewPadded(header), footer, nil, nil, scroll)
}

func metricCard(title, unit string) (*widget.Card, *widget.Label) {
	value := widget.NewLabelWithStyle("—", fyne.TextAlignCenter, fyne.TextStyle{Bold: true})
	value.SizeName = metricSmallSizeName
	unitLabel := widget.NewLabelWithStyle(unit, fyne.TextAlignCenter, fyne.TextStyle{})
	unitLabel.Importance = widget.LowImportance
	return widget.NewCard(title, "", container.NewVBox(value, unitLabel)), value
}

func detailLabel(text string) *widget.Label {
	label := widget.NewLabel(text)
	label.Wrapping = fyne.TextWrapWord
	label.Selectable = true
	return label
}

func (d *dashboard) selectService(label string) {
	id, ok := serviceFromLabel(label)
	if !ok {
		return
	}
	d.settings.Service = id
	if id != service.Speedtest {
		d.settings.Server = ""
	}
	d.preferences.saveMeasurement(d.settings)
}

func (d *dashboard) cycleTheme() {
	switch d.settings.Theme {
	case themeSystem:
		d.applyTheme(themeLight)
	case themeLight:
		d.applyTheme(themeDark)
	default:
		d.applyTheme(themeSystem)
	}
}

func (d *dashboard) applyTheme(mode themeMode) {
	d.settings.Theme = mode
	d.preferences.saveTheme(mode)
	d.application.Settings().SetTheme(newPulsTheme(mode))
}

func (d *dashboard) showSettings() {
	serviceSelect := widget.NewSelect(serviceLabels(), nil)
	serviceSelect.SetSelected(serviceLabel(d.settings.Service))
	profileSelect := widget.NewSelect(profileLabels(), nil)
	profileSelect.SetSelected(profileLabel(d.settings.Profile))
	durationEntry := widget.NewEntry()
	durationEntry.SetText(strconv.Itoa(int(d.settings.Duration / time.Second)))
	connectionsSelect := widget.NewSelect(connectionLabels(), nil)
	connectionsSelect.SetSelected(connectionLabel(d.settings.Connections))
	phaseSelect := widget.NewSelect(phaseLabels(), nil)
	phaseSelect.SetSelected(phaseLabelForSettings(d.settings.Only))
	serverEntry := widget.NewEntry()
	serverEntry.SetPlaceHolder("host:port")
	serverEntry.SetText(d.settings.Server)
	showIP := widget.NewCheck("Показывать IP в результате", nil)
	showIP.SetChecked(d.settings.ShowIP)
	themeSelect := widget.NewSelect(themeLabels(), nil)
	themeSelect.SetSelected(themeLabel(d.settings.Theme))

	updateServer := func(label string) {
		id, _ := serviceFromLabel(label)
		if id == service.Speedtest {
			serverEntry.Enable()
		} else {
			serverEntry.Disable()
		}
	}
	serviceSelect.OnChanged = updateServer
	updateServer(serviceSelect.Selected)
	profileSelect.OnChanged = func(label string) {
		if profile, ok := profileFromLabel(label); ok {
			durationEntry.SetText(strconv.Itoa(int(profile.Duration() / time.Second)))
		}
	}

	items := []*widget.FormItem{
		widget.NewFormItem("Сервис", serviceSelect),
		widget.NewFormItem("Профиль", profileSelect),
		widget.NewFormItem("Длительность, с", durationEntry),
		widget.NewFormItem("Соединения", connectionsSelect),
		widget.NewFormItem("Этап", phaseSelect),
		widget.NewFormItem("Сервер speedtest", serverEntry),
		widget.NewFormItem("IP", showIP),
		widget.NewFormItem("Оформление", themeSelect),
	}
	settingsDialog := dialog.NewForm("Настройки", "Сохранить", "Отмена", items, func(confirm bool) {
		if !confirm {
			return
		}
		settings, err := parseSettings(serviceSelect.Selected, profileSelect.Selected, durationEntry.Text, connectionsSelect.Selected, phaseSelect.Selected, serverEntry.Text, showIP.Checked, themeSelect.Selected)
		if err != nil {
			dialog.ShowError(err, d.window)
			return
		}
		d.settings = settings
		d.serviceSelect.SetSelected(serviceLabel(settings.Service))
		d.configLabel.SetText(settingsSummary(settings))
		d.preferences.saveMeasurement(settings)
		d.applyTheme(settings.Theme)
	}, d.window)
	settingsDialog.Resize(fyne.NewSize(420, 620))
	settingsDialog.Show()
}

func settingsSummary(settings measurementSettings) string {
	connections := connectionLabel(settings.Connections)
	return strings.Join([]string{
		strings.Split(profileLabel(settings.Profile), " · ")[0],
		strconv.Itoa(int(settings.Duration/time.Second)) + " с",
		connections,
		phaseLabelForSettings(settings.Only),
	}, "  ·  ")
}

func parseSettings(serviceText, profileText, durationText, connectionsText, phaseText, serverText string, showIP bool, themeText string) (measurementSettings, error) {
	id, ok := serviceFromLabel(serviceText)
	if !ok {
		return measurementSettings{}, errors.New("выберите сервис измерения")
	}
	profile, ok := profileFromLabel(profileText)
	if !ok {
		return measurementSettings{}, errors.New("выберите профиль")
	}
	seconds, err := strconv.Atoi(strings.TrimSpace(durationText))
	if err != nil || seconds < 3 || seconds > 60 {
		return measurementSettings{}, errors.New("длительность должна быть от 3 до 60 секунд")
	}
	connections, ok := connectionsFromLabel(connectionsText)
	if !ok {
		return measurementSettings{}, errors.New("выберите число соединений")
	}
	only, ok := phaseFromLabel(phaseText)
	if !ok {
		return measurementSettings{}, errors.New("выберите этап измерения")
	}
	mode, ok := themeFromLabel(themeText)
	if !ok {
		return measurementSettings{}, errors.New("выберите оформление")
	}
	serverText = strings.TrimSpace(serverText)
	if id != service.Speedtest {
		serverText = ""
	}
	settings := measurementSettings{
		Service: id, Profile: profile, Duration: time.Duration(seconds) * time.Second,
		Connections: connections, Only: only, Server: serverText, ShowIP: showIP, Theme: mode,
	}
	request := settings.request()
	if err := appcore.ValidateMeasureRequest(request); err != nil {
		return measurementSettings{}, err
	}
	return settings, nil
}

func (s measurementSettings) request() appcore.MeasureRequest {
	return appcore.MeasureRequest{
		Service: s.Service, Profile: s.Profile, Duration: s.Duration, Connections: s.Connections,
		Only: s.Only, Server: s.Server, ShowConnection: s.ShowIP,
	}
}

func (d *dashboard) toggleMeasurement() {
	if d.isActive() {
		d.cancelActive()
		d.startButton.SetText("Останавливаем…")
		d.startButton.Disable()
		return
	}
	request := d.settings.request()
	if err := appcore.ValidateMeasureRequest(request); err != nil {
		dialog.ShowError(err, d.window)
		return
	}
	d.beginRun("Подготовка проверки…")
	runContext, cancel := context.WithCancel(d.root)
	if !d.setActive(cancel) {
		cancel()
		return
	}
	runner := d.runnerFactory(d.options.Log)
	go func() {
		result := runner.Measure(runContext, request, d.observe)
		d.clearActive()
		schedule(func() { d.finishMeasurement(result) })
	}()
}

func (d *dashboard) startConnectionLookup() {
	if d.isActive() {
		return
	}
	d.beginConnectionRun()
	runContext, cancel := context.WithCancel(d.root)
	if !d.setActive(cancel) {
		cancel()
		return
	}
	request := appcore.ConnectionRequest{Service: d.settings.Service, Explicit: d.settings.Service != service.All}
	if d.settings.Service == service.All {
		request.Service = ""
	}
	runner := d.runnerFactory(d.options.Log)
	go func() {
		result := runner.DetectConnection(runContext, request, d.observe)
		d.clearActive()
		schedule(func() { d.finishConnection(result) })
	}()
}

func (d *dashboard) beginRun(status string) {
	d.statusLabel.SetText(status)
	d.statusLabel.Importance = widget.LowImportance
	d.currentValue.SetText("—")
	d.currentUnit.SetText("Мбит/с")
	d.progress.SetValue(0)
	d.progress.Show()
	d.serverLabel.SetText("Выбор сервера измерения…")
	d.noticeLabel.SetText("")
	d.noticeLabel.Hide()
	d.resultBox.RemoveAll()
	for _, metric := range d.metrics {
		metric.SetText("—")
	}
	d.jitter.SetText("—")
	d.startButton.SetText("Остановить")
	d.startButton.SetIcon(theme.MediaStopIcon())
	d.startButton.Importance = widget.DangerImportance
	d.connectionButton.Disable()
	d.serviceSelect.Disable()
	d.settingsButton.Disable()
}

func (d *dashboard) beginConnectionRun() {
	d.statusLabel.SetText("Определение подключения…")
	d.statusLabel.Importance = widget.LowImportance
	d.progress.SetValue(0)
	d.progress.Show()
	d.noticeLabel.SetText("")
	d.noticeLabel.Hide()
	d.connectionLabel.SetText("Определение IP и интернет-провайдера…")
	d.startButton.SetText("Остановить")
	d.startButton.SetIcon(theme.MediaStopIcon())
	d.startButton.Importance = widget.DangerImportance
	d.connectionButton.Disable()
	d.serviceSelect.Disable()
	d.settingsButton.Disable()
}

func (d *dashboard) finishMeasurement(result appcore.Envelope) {
	d.progress.Hide()
	switch result.Status {
	case service.StatusOK:
		d.statusLabel.SetText("Проверка завершена")
		d.statusLabel.Importance = widget.SuccessImportance
	case service.StatusPartial:
		d.statusLabel.SetText("Получен частичный результат")
		d.statusLabel.Importance = widget.WarningImportance
	case service.StatusCanceled:
		d.statusLabel.SetText("Проверка остановлена")
		d.statusLabel.Importance = widget.WarningImportance
	default:
		d.statusLabel.SetText("Проверка не выполнена")
		d.statusLabel.Importance = widget.DangerImportance
	}
	d.finishRun()
}

func (d *dashboard) finishConnection(result appcore.ConnectionResult) {
	d.progress.Hide()
	if result.Status == service.StatusCanceled {
		d.statusLabel.SetText("Определение подключения остановлено")
		d.statusLabel.Importance = widget.WarningImportance
	} else if result.Status == service.StatusOK {
		d.statusLabel.SetText("Подключение определено")
		d.statusLabel.Importance = widget.SuccessImportance
	} else {
		d.statusLabel.SetText("Не удалось определить подключение")
		d.statusLabel.Importance = widget.DangerImportance
	}
	d.finishRun()
}

func (d *dashboard) finishRun() {
	d.startButton.SetText("Проверить снова")
	d.startButton.SetIcon(theme.ViewRefreshIcon())
	d.startButton.Importance = widget.HighImportance
	d.startButton.Enable()
	d.connectionButton.Enable()
	d.serviceSelect.Enable()
	d.settingsButton.Enable()
}

func (d *dashboard) observe(event appcore.RunEvent) {
	schedule(func() { d.applyEvent(event) })
}

func (d *dashboard) applyEvent(event appcore.RunEvent) {
	switch event.Kind {
	case appcore.EventServiceStarted:
		d.statusLabel.SetText("Сервис: " + displayService(event.Service))
	case appcore.EventPhaseStarted:
		d.statusLabel.SetText(phaseStatus(event.Phase))
		d.currentUnit.SetText(phaseUnit(event.Phase))
	case appcore.EventServerSelected:
		if event.Server != nil {
			d.serverLabel.SetText(formatServer(*event.Server))
		}
	case appcore.EventPingCompleted:
		if event.Ping != nil {
			d.currentValue.SetText(formatNumber(event.Ping.ValueMs))
			d.currentUnit.SetText("мс")
			d.metrics[service.PhasePing].SetText(formatNumber(event.Ping.ValueMs))
			d.jitter.SetText(formatNumber(event.Ping.JitterMs))
		}
	case appcore.EventThroughputProgress:
		if event.Throughput != nil {
			d.currentValue.SetText(formatNumber(event.Throughput.Mbps))
			d.currentUnit.SetText("Мбит/с")
			fraction := float64(event.Throughput.Elapsed) / float64(d.settings.Duration)
			d.progress.SetValue(max(0, min(1, fraction)))
		}
	case appcore.EventPhaseCompleted:
		if event.PhaseResult != nil {
			d.applyPhaseResult(event.Phase, *event.PhaseResult)
		}
	case appcore.EventConnectionCompleted:
		if event.Connection != nil {
			d.applyConnection(*event.Connection)
		}
	case appcore.EventServiceCompleted:
		if event.Measurement != nil {
			d.resultBox.Add(measurementCard(*event.Measurement))
			d.addNotices(event.Measurement.Warnings...)
		}
	}
}

func (d *dashboard) applyPhaseResult(phase service.Phase, result appcore.PhaseResult) {
	if result.Status == service.StatusOK && result.Mbps != nil {
		d.metrics[phase].SetText(formatNumber(*result.Mbps))
		d.currentValue.SetText(formatNumber(*result.Mbps))
		d.currentUnit.SetText("Мбит/с")
		return
	}
	if result.Status == service.StatusError && result.Error != nil {
		d.addNotice(widget.DangerImportance, phaseTitle(phase)+": "+result.Error.Message)
	}
}

func (d *dashboard) applyConnection(result appcore.ConnectionResult) {
	if result.Status != service.StatusOK || result.ExternalIP == nil {
		message := "Не удалось определить IP"
		if result.Error != nil {
			message += ": " + result.Error.Message
		}
		d.connectionLabel.SetText(message)
		return
	}
	parts := []string{*result.ExternalIP}
	if result.ISP != nil {
		parts = append(parts, *result.ISP)
	}
	if result.DetectedBy != nil {
		parts = append(parts, "через "+displayService(*result.DetectedBy))
	}
	d.connectionLabel.SetText(strings.Join(parts, " · "))
	if len(result.Warnings) > 0 {
		d.addNotices(result.Warnings...)
	}
}

func (d *dashboard) addNotices(messages ...string) {
	for _, message := range messages {
		d.addNotice(widget.WarningImportance, message)
	}
}

func (d *dashboard) addNotice(importance widget.Importance, message string) {
	message = strings.TrimSpace(message)
	if message == "" {
		return
	}
	existing := strings.Split(strings.TrimSpace(d.noticeLabel.Text), "\n")
	for _, current := range existing {
		if current == message {
			return
		}
	}
	if d.noticeLabel.Text == "" {
		d.noticeLabel.SetText(message)
	} else {
		d.noticeLabel.SetText(d.noticeLabel.Text + "\n" + message)
	}
	if importance == widget.DangerImportance || d.noticeLabel.Importance != widget.DangerImportance {
		d.noticeLabel.Importance = importance
	}
	d.noticeLabel.Show()
}

func measurementCard(result appcore.MeasurementResult) fyne.CanvasObject {
	status := statusText(result.Status)
	server := "Сервер не выбран"
	if result.Server != nil {
		server = result.Server.Name
		if result.Server.City != nil {
			server += " · " + *result.Server.City
		}
	}
	values := []string{
		"Задержка  " + phaseValue(result.Phases.Ping, "мс"),
		"Загрузка  " + phaseValue(result.Phases.Download, "Мбит/с"),
		"Отдача  " + phaseValue(result.Phases.Upload, "Мбит/с"),
	}
	content := detailLabel(strings.Join(values, "    "))
	content.Wrapping = fyne.TextWrapWord
	return widget.NewCard(displayService(result.Service)+" · "+status, server, content)
}

func phaseValue(result appcore.PhaseResult, unit string) string {
	if result.Mbps != nil {
		return formatNumber(*result.Mbps) + " " + unit
	}
	if result.ValueMs != nil {
		return formatNumber(*result.ValueMs) + " " + unit
	}
	if result.Status == service.StatusError {
		return "ошибка"
	}
	return "—"
}

func (d *dashboard) setActive(cancel context.CancelFunc) bool {
	d.activeMu.Lock()
	defer d.activeMu.Unlock()
	if d.activeCancel != nil {
		return false
	}
	d.activeCancel = cancel
	return true
}

func (d *dashboard) clearActive() {
	d.activeMu.Lock()
	d.activeCancel = nil
	d.activeMu.Unlock()
}

func (d *dashboard) isActive() bool {
	d.activeMu.Lock()
	defer d.activeMu.Unlock()
	return d.activeCancel != nil
}

func (d *dashboard) cancelActive() {
	d.activeMu.Lock()
	cancel := d.activeCancel
	d.activeMu.Unlock()
	if cancel != nil {
		cancel()
	}
}

func (d *dashboard) saveWindowSize() {
	if fyne.CurrentDevice().IsMobile() {
		return
	}
	d.preferences.saveWindow(d.window.Canvas().Size())
}

func schedule(operation func()) {
	defer func() { _ = recover() }()
	fyne.Do(operation)
}

func displayService(id service.ServiceID) string {
	if id == service.Yandex {
		return "Яндекс.Интернетометр"
	}
	if id == service.All {
		return "Все сервисы"
	}
	return service.DisplayName(id)
}

func formatServer(server service.Server) string {
	parts := []string{server.Name}
	if server.City != "" {
		parts = append(parts, server.City)
	}
	if server.Region != "" && server.Region != server.City {
		parts = append(parts, server.Region)
	}
	return strings.Join(parts, " · ")
}

func formatNumber(value float64) string {
	return strings.ReplaceAll(strconv.FormatFloat(value, 'f', 2, 64), ".", ",")
}

func phaseStatus(phase service.Phase) string {
	switch phase {
	case service.PhaseSelect:
		return "Выбор сервера…"
	case service.PhasePing:
		return "Измерение задержки…"
	case service.PhaseDownload:
		return "Измерение загрузки…"
	case service.PhaseUpload:
		return "Измерение отдачи…"
	default:
		return "Выполнение…"
	}
}

func phaseUnit(phase service.Phase) string {
	if phase == service.PhasePing {
		return "мс"
	}
	return "Мбит/с"
}

func phaseTitle(phase service.Phase) string {
	switch phase {
	case service.PhaseSelect:
		return "Сервер"
	case service.PhasePing:
		return "Задержка"
	case service.PhaseDownload:
		return "Загрузка"
	case service.PhaseUpload:
		return "Отдача"
	default:
		return string(phase)
	}
}

func statusText(status service.Status) string {
	switch status {
	case service.StatusOK:
		return "готово"
	case service.StatusPartial:
		return "частично"
	case service.StatusCanceled:
		return "остановлено"
	default:
		return "ошибка"
	}
}

func serviceLabels() []string {
	return []string{"Яндекс.Интернетометр", "speedtest.ru", "Все сервисы"}
}

func serviceLabel(id service.ServiceID) string { return displayService(id) }

func serviceFromLabel(label string) (service.ServiceID, bool) {
	switch label {
	case "Яндекс.Интернетометр":
		return service.Yandex, true
	case "speedtest.ru":
		return service.Speedtest, true
	case "Все сервисы":
		return service.All, true
	default:
		return "", false
	}
}

func profileLabels() []string {
	return []string{"Быстрый · 5 с", "Сбалансированный · 10 с", "Точный · 15 с"}
}

func profileLabel(profile appcore.Profile) string {
	switch profile {
	case appcore.ProfileQuick:
		return profileLabels()[0]
	case appcore.ProfileAccurate:
		return profileLabels()[2]
	default:
		return profileLabels()[1]
	}
}

func profileFromLabel(label string) (appcore.Profile, bool) {
	switch label {
	case profileLabels()[0]:
		return appcore.ProfileQuick, true
	case profileLabels()[1]:
		return appcore.ProfileBalanced, true
	case profileLabels()[2]:
		return appcore.ProfileAccurate, true
	default:
		return "", false
	}
}

func connectionLabels() []string {
	labels := []string{"Автоматически"}
	for value := 1; value <= 16; value++ {
		labels = append(labels, strconv.Itoa(value))
	}
	return labels
}

func connectionLabel(value int) string {
	if value == 0 {
		return connectionLabels()[0]
	}
	return strconv.Itoa(value)
}

func connectionsFromLabel(label string) (int, bool) {
	if label == connectionLabels()[0] {
		return 0, true
	}
	value, err := strconv.Atoi(label)
	return value, err == nil && value >= 1 && value <= 16
}

func phaseLabels() []string {
	return []string{"Все этапы", "Только задержка", "Только загрузка", "Только отдача"}
}

func phaseLabelForSettings(phase appcore.PhaseSelection) string {
	switch phase {
	case appcore.PhasePing:
		return phaseLabels()[1]
	case appcore.PhaseDownload:
		return phaseLabels()[2]
	case appcore.PhaseUpload:
		return phaseLabels()[3]
	default:
		return phaseLabels()[0]
	}
}

func phaseFromLabel(label string) (appcore.PhaseSelection, bool) {
	switch label {
	case phaseLabels()[0]:
		return appcore.PhaseAll, true
	case phaseLabels()[1]:
		return appcore.PhasePing, true
	case phaseLabels()[2]:
		return appcore.PhaseDownload, true
	case phaseLabels()[3]:
		return appcore.PhaseUpload, true
	default:
		return "", false
	}
}

func themeLabels() []string {
	return []string{"Как в системе", "Светлое", "Тёмное"}
}

func themeLabel(mode themeMode) string {
	switch mode {
	case themeLight:
		return themeLabels()[1]
	case themeDark:
		return themeLabels()[2]
	default:
		return themeLabels()[0]
	}
}

func themeFromLabel(label string) (themeMode, bool) {
	switch label {
	case themeLabels()[0]:
		return themeSystem, true
	case themeLabels()[1]:
		return themeLight, true
	case themeLabels()[2]:
		return themeDark, true
	default:
		return "", false
	}
}
