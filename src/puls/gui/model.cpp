#include "puls/gui/model.hpp"

#include "puls/application/runner.hpp"
#include "puls/core/text.hpp"

#include <algorithm>

namespace puls::gui {

namespace {

using service::Phase;
using service::ServiceId;
using service::Status;

template <class T>
const T* item(const std::vector<T>& values, int index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= values.size()) {
        return nullptr;
    }
    return &values[static_cast<std::size_t>(index)];
}

constexpr ServiceId services[] = {ServiceId::yandex, ServiceId::speedtest, ServiceId::all};
constexpr app::Profile profiles[] = {app::Profile::quick, app::Profile::balanced,
                                     app::Profile::accurate};
constexpr app::PhaseSelection phases[] = {app::PhaseSelection::all, app::PhaseSelection::ping,
                                          app::PhaseSelection::download,
                                          app::PhaseSelection::upload};
constexpr ThemeMode themes[] = {ThemeMode::system, ThemeMode::light, ThemeMode::dark};

template <class T, std::size_t Size>
int index_of(const T (&values)[Size], T value) noexcept {
    for (std::size_t index = 0; index < Size; ++index) {
        if (values[index] == value) {
            return static_cast<int>(index);
        }
    }
    return 0;
}

template <class T, std::size_t Size>
std::optional<T> value_at(const T (&values)[Size], int index) noexcept {
    if (index < 0 || static_cast<std::size_t>(index) >= Size) {
        return std::nullopt;
    }
    return values[static_cast<std::size_t>(index)];
}

std::string phase_status(Phase phase) {
    switch (phase) {
    case Phase::select:
        return "Выбор сервера…";
    case Phase::ping:
        return "Измерение задержки…";
    case Phase::download:
        return "Измерение загрузки…";
    case Phase::upload:
        return "Измерение отдачи…";
    case Phase::connection:
        break;
    }
    return "Выполнение…";
}

std::string phase_unit(Phase phase) {
    return phase == Phase::ping ? "мс" : "Мбит/с";
}

std::string phase_title(Phase phase) {
    switch (phase) {
    case Phase::select:
        return "Сервер";
    case Phase::ping:
        return "Задержка";
    case Phase::download:
        return "Загрузка";
    case Phase::upload:
        return "Отдача";
    case Phase::connection:
        break;
    }
    return std::string(service::to_string(phase));
}

std::string status_text(Status status) {
    switch (status) {
    case Status::ok:
        return "готово";
    case Status::partial:
        return "частично";
    case Status::canceled:
        return "остановлено";
    default:
        return "ошибка";
    }
}

std::string phase_value(const app::PhaseResult& result, std::string_view unit) {
    if (result.mbps) {
        return format_number(*result.mbps) + " " + std::string(unit);
    }
    if (result.value_ms) {
        return format_number(*result.value_ms) + " " + std::string(unit);
    }
    if (result.status == Status::error) {
        return "ошибка";
    }
    return "—";
}

ResultCard measurement_card(const app::MeasurementResult& result) {
    ResultCard card;
    card.title = display_service(result.service) + " · " + status_text(result.status);
    card.subtitle = "Сервер не выбран";
    if (result.server) {
        card.subtitle = result.server->name;
        if (result.server->city) {
            card.subtitle += " · " + *result.server->city;
        }
    }
    card.details = text::join(
        {
            "Задержка  " + phase_value(result.phases.ping, "мс"),
            "Загрузка  " + phase_value(result.phases.download, "Мбит/с"),
            "Отдача  " + phase_value(result.phases.upload, "Мбит/с"),
        },
        "    ");
    return card;
}

} // namespace

std::string_view to_string(ThemeMode mode) noexcept {
    switch (mode) {
    case ThemeMode::light:
        return "light";
    case ThemeMode::dark:
        return "dark";
    case ThemeMode::system:
        break;
    }
    return "system";
}

std::optional<ThemeMode> parse_theme_mode(std::string_view text) {
    for (const ThemeMode mode : themes) {
        if (to_string(mode) == text) {
            return mode;
        }
    }
    return std::nullopt;
}

app::MeasureRequest Settings::request() const {
    app::MeasureRequest request;
    request.service = service;
    request.profile = profile;
    request.duration = duration;
    request.connections = connections;
    request.only = only;
    request.server = server;
    request.show_connection = show_ip;
    return request;
}

const std::vector<std::string>& service_labels() {
    static const std::vector<std::string> labels = {"Яндекс.Интернетометр", "speedtest.ru",
                                                    "Все сервисы"};
    return labels;
}

const std::vector<std::string>& profile_labels() {
    static const std::vector<std::string> labels = {"Быстрый · 5 с", "Сбалансированный · 10 с",
                                                    "Точный · 15 с"};
    return labels;
}

const std::vector<std::string>& connection_labels() {
    static const std::vector<std::string> labels = [] {
        std::vector<std::string> values = {"Автоматически"};
        for (int value = 1; value <= 16; ++value) {
            values.push_back(std::to_string(value));
        }
        return values;
    }();
    return labels;
}

const std::vector<std::string>& phase_labels() {
    static const std::vector<std::string> labels = {"Все этапы", "Только задержка",
                                                    "Только загрузка", "Только отдача"};
    return labels;
}

const std::vector<std::string>& theme_labels() {
    static const std::vector<std::string> labels = {"Как в системе", "Светлое", "Тёмное"};
    return labels;
}

int service_index(ServiceId id) noexcept {
    return index_of(services, id);
}

int profile_index(app::Profile profile) noexcept {
    return index_of(profiles, profile);
}

int connection_index(int connections) noexcept {
    return connections >= 1 && connections <= 16 ? connections : 0;
}

int phase_index(app::PhaseSelection only) noexcept {
    return index_of(phases, only);
}

int theme_index(ThemeMode mode) noexcept {
    return index_of(themes, mode);
}

std::optional<app::Profile> profile_at(int index) noexcept {
    return value_at(profiles, index);
}

Result<Settings> parse_settings(const SettingsInput& input) {
    const auto id = value_at(services, input.service);
    if (!id) {
        return Error::make("выберите сервис измерения");
    }
    const auto profile = value_at(profiles, input.profile);
    if (!profile) {
        return Error::make("выберите профиль");
    }
    const text::ParsedInt seconds = text::atoi(text::trim_space(input.duration));
    if (!seconds.value || *seconds.value < 3 || *seconds.value > 60) {
        return Error::make("длительность должна быть от 3 до 60 секунд");
    }
    if (item(connection_labels(), input.connections) == nullptr) {
        return Error::make("выберите число соединений");
    }
    const auto only = value_at(phases, input.phase);
    if (!only) {
        return Error::make("выберите этап измерения");
    }
    const auto mode = value_at(themes, input.theme);
    if (!mode) {
        return Error::make("выберите оформление");
    }
    Settings settings;
    settings.service = *id;
    settings.profile = *profile;
    settings.duration = std::chrono::seconds(*seconds.value);
    settings.connections = input.connections;
    settings.only = *only;
    settings.server =
        *id == ServiceId::speedtest ? std::string(text::trim_space(input.server)) : std::string();
    settings.show_ip = input.show_ip;
    settings.theme = *mode;
    if (Error error = app::validate_measure_request(settings.request())) {
        return error;
    }
    return settings;
}

std::string settings_summary(const Settings& settings) {
    const std::string& profile =
        profile_labels()[static_cast<std::size_t>(profile_index(settings.profile))];
    return text::join(
        {
            profile.substr(0, profile.find(" · ")),
            std::to_string(settings.duration.count()) + " с",
            connection_labels()[static_cast<std::size_t>(connection_index(settings.connections))],
            phase_labels()[static_cast<std::size_t>(phase_index(settings.only))],
        },
        "  ·  ");
}

std::string display_service(ServiceId id) {
    switch (id) {
    case ServiceId::yandex:
        return "Яндекс.Интернетометр";
    case ServiceId::all:
        return "Все сервисы";
    case ServiceId::speedtest:
        break;
    }
    return service::display_name(id);
}

std::string format_server(const service::Server& server) {
    std::vector<std::string> parts = {server.name};
    if (!server.city.empty()) {
        parts.push_back(server.city);
    }
    if (!server.region.empty() && server.region != server.city) {
        parts.push_back(server.region);
    }
    return text::join(parts, " · ");
}

std::string format_number(double value) {
    std::string formatted = text::format_fixed(value, 2);
    std::replace(formatted.begin(), formatted.end(), '.', ',');
    return formatted;
}

Preferences load_preferences(const PreferenceStore& store) {
    Preferences preferences;
    if (const auto value = store.text("theme")) {
        preferences.theme = parse_theme_mode(*value).value_or(ThemeMode::system);
    }
    if (const auto value = store.text("service")) {
        const auto id = service::parse_service_id(*value);
        preferences.service = id.value_or(ServiceId::yandex);
    }
    if (const auto value = store.text("profile")) {
        preferences.profile = app::parse_profile(*value).value_or(app::Profile::balanced);
    }
    if (const auto value = store.number("duration_seconds")) {
        preferences.duration = *value >= 3 && *value <= 60
                                   ? std::chrono::seconds(static_cast<std::int64_t>(*value))
                                   : app::profile_duration(preferences.profile);
    }
    if (const auto value = store.number("connections"); value && *value >= 0 && *value <= 16) {
        preferences.connections = static_cast<int>(*value);
    }
    if (const auto value = store.text("phase")) {
        preferences.only = app::parse_phase_selection(*value).value_or(app::PhaseSelection::all);
    }
    if (const auto value = store.number("window_width");
        value && *value >= minimum_window_width && *value <= 2560) {
        preferences.window_width = *value;
    }
    if (const auto value = store.number("window_height");
        value && *value >= minimum_window_height && *value <= 2160) {
        preferences.window_height = *value;
    }
    return preferences;
}

void save_measurement(PreferenceStore& store, const Settings& settings) {
    store.set_text("service", service::to_string(settings.service));
    store.set_text("profile", app::to_string(settings.profile));
    store.set_number("duration_seconds", static_cast<double>(settings.duration.count()));
    store.set_number("connections", settings.connections);
    store.set_text("phase", app::to_string(settings.only));
}

void save_theme(PreferenceStore& store, ThemeMode mode) {
    store.set_text("theme", to_string(mode));
}

void save_window(PreferenceStore& store, double width, double height) {
    if (width < minimum_window_width || height < minimum_window_height) {
        return;
    }
    store.set_number("window_width", width);
    store.set_number("window_height", height);
}

Dashboard::Dashboard(PreferenceStore& store)
    : store_(store), preferences_(load_preferences(store)) {
    settings_.service = preferences_.service;
    settings_.profile = preferences_.profile;
    settings_.duration = preferences_.duration;
    settings_.connections = preferences_.connections;
    settings_.only = preferences_.only;
    settings_.theme = preferences_.theme;
}

std::string Dashboard::start_label() const {
    switch (state_.activity) {
    case Activity::measurement:
    case Activity::connection:
        return "Остановить";
    case Activity::stopping:
        return "Останавливаем…";
    case Activity::idle:
        break;
    }
    return state_.completed_once ? "Проверить снова" : "Начать проверку";
}

app::ConnectionRequest Dashboard::connection_request() const {
    app::ConnectionRequest request;
    request.explicit_service = settings_.service != ServiceId::all;
    if (request.explicit_service) {
        request.service = settings_.service;
    }
    return request;
}

void Dashboard::select_service(int index) {
    const auto id = value_at(services, index);
    if (!id) {
        return;
    }
    settings_.service = *id;
    if (*id != ServiceId::speedtest) {
        settings_.server.clear();
    }
    save_measurement(store_, settings_);
}

void Dashboard::cycle_theme() {
    switch (settings_.theme) {
    case ThemeMode::system:
        apply_theme(ThemeMode::light);
        break;
    case ThemeMode::light:
        apply_theme(ThemeMode::dark);
        break;
    case ThemeMode::dark:
        apply_theme(ThemeMode::system);
        break;
    }
}

void Dashboard::apply_theme(ThemeMode mode) {
    settings_.theme = mode;
    save_theme(store_, mode);
}

Error Dashboard::apply_settings(const SettingsInput& input) {
    auto parsed = parse_settings(input);
    if (!parsed) {
        return parsed.error();
    }
    settings_ = std::move(parsed).value();
    save_measurement(store_, settings_);
    apply_theme(settings_.theme);
    return {};
}

void Dashboard::save_window(double width, double height) {
    gui::save_window(store_, width, height);
}

void Dashboard::reset_metrics() {
    state_.ping = "—";
    state_.jitter = "—";
    state_.download = "—";
    state_.upload = "—";
}

void Dashboard::begin_measurement() {
    state_.status = "Подготовка проверки…";
    state_.status_tone = Tone::neutral;
    state_.current_value = "—";
    state_.current_unit = "Мбит/с";
    state_.progress = 0;
    state_.progress_visible = true;
    state_.server = "Выбор сервера измерения…";
    state_.notices.clear();
    state_.notice_tone = Tone::warning;
    state_.results.clear();
    reset_metrics();
    state_.activity = Activity::measurement;
}

void Dashboard::begin_connection() {
    state_.status = "Определение подключения…";
    state_.status_tone = Tone::neutral;
    state_.progress = 0;
    state_.progress_visible = true;
    state_.notices.clear();
    state_.notice_tone = Tone::warning;
    state_.connection = "Определение IP и интернет-провайдера…";
    state_.activity = Activity::connection;
}

void Dashboard::begin_stopping() {
    if (busy()) {
        state_.activity = Activity::stopping;
    }
}

std::string* Dashboard::metric(Phase phase) {
    switch (phase) {
    case Phase::ping:
        return &state_.ping;
    case Phase::download:
        return &state_.download;
    case Phase::upload:
        return &state_.upload;
    default:
        return nullptr;
    }
}

void Dashboard::apply_event(const app::RunEvent& event) {
    switch (event.kind) {
    case app::EventKind::service_started:
        if (event.service) {
            state_.status = "Сервис: " + display_service(*event.service);
        }
        break;
    case app::EventKind::phase_started:
        if (event.phase) {
            state_.status = phase_status(*event.phase);
            state_.current_unit = phase_unit(*event.phase);
        }
        break;
    case app::EventKind::server_selected:
        if (event.server) {
            state_.server = format_server(*event.server);
        }
        break;
    case app::EventKind::ping_completed:
        if (event.ping) {
            state_.current_value = format_number(event.ping->value_ms);
            state_.current_unit = "мс";
            state_.ping = format_number(event.ping->value_ms);
            state_.jitter = format_number(event.ping->jitter_ms);
        }
        break;
    case app::EventKind::throughput_progress:
        if (event.throughput) {
            state_.current_value = format_number(event.throughput->mbps);
            state_.current_unit = "Мбит/с";
            const auto duration =
                std::chrono::duration_cast<std::chrono::nanoseconds>(settings_.duration);
            state_.progress =
                duration.count() > 0
                    ? std::clamp(static_cast<double>(event.throughput->elapsed.count()) /
                                     static_cast<double>(duration.count()),
                                 0.0, 1.0)
                    : 0;
        }
        break;
    case app::EventKind::phase_completed:
        if (event.phase && event.phase_result) {
            apply_phase_result(*event.phase, *event.phase_result);
        }
        break;
    case app::EventKind::connection_completed:
        if (event.connection) {
            apply_connection(*event.connection);
        }
        break;
    case app::EventKind::service_completed:
        if (event.measurement) {
            state_.results.push_back(measurement_card(*event.measurement));
            for (const auto& warning : event.measurement->warnings) {
                add_notice(Tone::warning, warning);
            }
        }
        break;
    case app::EventKind::run_started:
    case app::EventKind::connection_started:
    case app::EventKind::run_completed:
        break;
    }
}

void Dashboard::apply_phase_result(Phase phase, const app::PhaseResult& result) {
    if (result.status == Status::ok && result.mbps) {
        if (std::string* value = metric(phase)) {
            *value = format_number(*result.mbps);
        }
        state_.current_value = format_number(*result.mbps);
        state_.current_unit = "Мбит/с";
        return;
    }
    if (result.status == Status::error && result.error) {
        add_notice(Tone::danger, phase_title(phase) + ": " + result.error->message);
    }
}

void Dashboard::apply_connection(const app::ConnectionResult& result) {
    if (result.status != Status::ok || !result.external_ip) {
        std::string message = "Не удалось определить IP";
        if (result.error) {
            message += ": " + result.error->message;
        }
        state_.connection = std::move(message);
        return;
    }
    std::vector<std::string> parts = {*result.external_ip};
    if (result.isp) {
        parts.push_back(*result.isp);
    }
    if (result.detected_by) {
        parts.push_back("через " + display_service(*result.detected_by));
    }
    state_.connection = text::join(parts, " · ");
    for (const auto& warning : result.warnings) {
        add_notice(Tone::warning, warning);
    }
}

void Dashboard::add_notice(Tone tone, std::string_view message) {
    const std::string trimmed(text::trim_space(message));
    if (trimmed.empty() ||
        std::find(state_.notices.begin(), state_.notices.end(), trimmed) != state_.notices.end()) {
        return;
    }
    if (state_.notices.empty()) {
        state_.notice_tone = tone;
    } else if (tone == Tone::danger || state_.notice_tone != Tone::danger) {
        state_.notice_tone = tone;
    }
    state_.notices.push_back(trimmed);
}

void Dashboard::finish_measurement(const app::Envelope& envelope) {
    state_.progress_visible = false;
    switch (envelope.status) {
    case Status::ok:
        state_.status = "Проверка завершена";
        state_.status_tone = Tone::success;
        break;
    case Status::partial:
        state_.status = "Получен частичный результат";
        state_.status_tone = Tone::warning;
        break;
    case Status::canceled:
        state_.status = "Проверка остановлена";
        state_.status_tone = Tone::warning;
        break;
    default:
        state_.status = "Проверка не выполнена";
        state_.status_tone = Tone::danger;
        break;
    }
    finish_run();
}

void Dashboard::finish_connection(const app::ConnectionResult& result) {
    state_.progress_visible = false;
    if (result.status == Status::canceled) {
        state_.status = "Определение подключения остановлено";
        state_.status_tone = Tone::warning;
    } else if (result.status == Status::ok) {
        state_.status = "Подключение определено";
        state_.status_tone = Tone::success;
    } else {
        state_.status = "Не удалось определить подключение";
        state_.status_tone = Tone::danger;
    }
    finish_run();
}

void Dashboard::finish_run() {
    state_.activity = Activity::idle;
    state_.completed_once = true;
}

} // namespace puls::gui
