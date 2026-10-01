#include "puls/gui/controller.hpp"

#include <QMetaObject>
#include <QVariantMap>

#include <cmath>
#include <utility>

namespace puls::gui {

namespace {

QString qstring(const std::string& value) {
    return QString::fromStdString(value);
}

QStringList labels(const std::vector<std::string>& values) {
    QStringList result;
    result.reserve(static_cast<qsizetype>(values.size()));
    for (const auto& value : values) {
        result.append(qstring(value));
    }
    return result;
}

int tone(Tone value) {
    return static_cast<int>(value);
}

} // namespace

DashboardController::DashboardController(const Context& root, QString version,
                                         app::RunnerOptions runner, PreferenceStore& store,
                                         QObject* parent)
    : QObject(parent), root_(root), version_(std::move(version)), runner_(std::move(runner)),
      model_(store) {}

DashboardController::~DashboardController() {
    shutdown();
}

template <class Function>
void DashboardController::post(Function&& function) {
    // Queued to this object: if it is destroyed first, the call is dropped.
    QMetaObject::invokeMethod(this, std::forward<Function>(function), Qt::QueuedConnection);
}

QStringList DashboardController::serviceLabels() const {
    return labels(service_labels());
}

QStringList DashboardController::profileLabels() const {
    return labels(profile_labels());
}

QStringList DashboardController::connectionLabels() const {
    return labels(connection_labels());
}

QStringList DashboardController::phaseLabels() const {
    return labels(phase_labels());
}

QStringList DashboardController::themeLabels() const {
    return labels(theme_labels());
}

int DashboardController::windowWidth() const {
    return static_cast<int>(std::lround(model_.preferences().window_width));
}

int DashboardController::windowHeight() const {
    return static_cast<int>(std::lround(model_.preferences().window_height));
}

void DashboardController::setSystemDark(bool dark) {
    if (dark != system_dark_) {
        system_dark_ = dark;
        emit systemDarkChanged();
    }
}

int DashboardController::serviceIndex() const {
    return service_index(model_.settings().service);
}

int DashboardController::profileIndex() const {
    return profile_index(model_.settings().profile);
}

int DashboardController::durationSeconds() const {
    return static_cast<int>(model_.settings().duration.count());
}

int DashboardController::connectionsIndex() const {
    return connection_index(model_.settings().connections);
}

int DashboardController::phaseIndex() const {
    return phase_index(model_.settings().only);
}

QString DashboardController::speedtestServer() const {
    return qstring(model_.settings().server);
}

bool DashboardController::showIp() const {
    return model_.settings().show_ip;
}

int DashboardController::themeIndex() const {
    return theme_index(model_.settings().theme);
}

QString DashboardController::themeLabel() const {
    return qstring(theme_labels()[static_cast<std::size_t>(themeIndex())]);
}

QString DashboardController::settingsSummary() const {
    return qstring(settings_summary(model_.settings()));
}

bool DashboardController::stopping() const {
    return model_.state().activity == Activity::stopping;
}

QString DashboardController::startLabel() const {
    return qstring(model_.start_label());
}

QString DashboardController::status() const {
    return qstring(model_.state().status);
}

int DashboardController::statusTone() const {
    return tone(model_.state().status_tone);
}

QString DashboardController::currentValue() const {
    return qstring(model_.state().current_value);
}

QString DashboardController::currentUnit() const {
    return qstring(model_.state().current_unit);
}

double DashboardController::progress() const {
    return model_.state().progress;
}

bool DashboardController::progressVisible() const {
    return model_.state().progress_visible;
}

QString DashboardController::ping() const {
    return qstring(model_.state().ping);
}

QString DashboardController::jitter() const {
    return qstring(model_.state().jitter);
}

QString DashboardController::download() const {
    return qstring(model_.state().download);
}

QString DashboardController::upload() const {
    return qstring(model_.state().upload);
}

QString DashboardController::serverText() const {
    return qstring(model_.state().server);
}

QString DashboardController::connectionText() const {
    return qstring(model_.state().connection);
}

QString DashboardController::notices() const {
    return labels(model_.state().notices).join(QLatin1Char('\n'));
}

int DashboardController::noticeTone() const {
    return tone(model_.state().notice_tone);
}

QVariantList DashboardController::results() const {
    QVariantList result;
    for (const ResultCard& card : model_.state().results) {
        result.append(QVariantMap{{QStringLiteral("title"), qstring(card.title)},
                                  {QStringLiteral("subtitle"), qstring(card.subtitle)},
                                  {QStringLiteral("details"), qstring(card.details)}});
    }
    return result;
}

void DashboardController::toggleMeasurement() {
    if (model_.busy()) {
        cancel();
        return;
    }
    const app::MeasureRequest request = model_.settings().request();
    if (Error error = app::validate_measure_request(request)) {
        emit errorOccurred(qstring(error.message()));
        return;
    }
    model_.begin_measurement();
    emit stateChanged();
    start([this, request](const Context& ctx, const app::Observer& observer) {
        const app::Runner runner(runner_);
        app::Envelope envelope = runner.measure(ctx, request, observer);
        post([this, envelope = std::move(envelope)] {
            join();
            model_.finish_measurement(envelope);
            emit stateChanged();
        });
    });
}

void DashboardController::detectConnection() {
    if (model_.busy()) {
        return;
    }
    const app::ConnectionRequest request = model_.connection_request();
    model_.begin_connection();
    emit stateChanged();
    start([this, request](const Context& ctx, const app::Observer& observer) {
        const app::Runner runner(runner_);
        app::ConnectionResult result = runner.detect_connection(ctx, request, observer);
        post([this, result = std::move(result)] {
            join();
            model_.finish_connection(result);
            emit stateChanged();
        });
    });
}

void DashboardController::selectService(int index) {
    if (model_.busy()) {
        return;
    }
    model_.select_service(index);
    emit settingsChanged();
}

void DashboardController::cycleTheme() {
    model_.cycle_theme();
    emit settingsChanged();
}

QString DashboardController::applySettings(int service, int profile, const QString& duration,
                                           int connections, int phase, const QString& server,
                                           bool showIp, int theme) {
    if (model_.busy()) {
        return QStringLiteral("дождитесь завершения проверки");
    }
    SettingsInput input;
    input.service = service;
    input.profile = profile;
    input.duration = duration.toStdString();
    input.connections = connections;
    input.phase = phase;
    input.server = server.toStdString();
    input.show_ip = showIp;
    input.theme = theme;
    if (Error error = model_.apply_settings(input)) {
        return qstring(error.message());
    }
    emit settingsChanged();
    return {};
}

int DashboardController::profileDurationSeconds(int profile) const {
    const auto value = profile_at(profile);
    return value ? static_cast<int>(app::profile_duration(*value).count()) : durationSeconds();
}

void DashboardController::saveWindowSize(int width, int height) {
    model_.save_window(width, height);
}

void DashboardController::cancel() {
    if (!model_.busy() || !active_) {
        return;
    }
    active_->cancel();
    model_.begin_stopping();
    emit stateChanged();
}

void DashboardController::shutdown() {
    if (active_) {
        active_->cancel();
    }
    join();
}

void DashboardController::start(Job job) {
    join();
    active_ = std::make_unique<CancelScope>(root_);
    const Context ctx = active_->context();
    worker_ = std::thread([this, ctx, job = std::move(job)] {
        job(ctx, [this](const app::RunEvent& event) {
            post([this, event] {
                model_.apply_event(event);
                emit stateChanged();
            });
        });
    });
}

void DashboardController::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
    active_.reset();
}

} // namespace puls::gui
