#include "puls/gui/model.hpp"

#include "puls/application/runner.hpp"
#include "puls/core/text.hpp"

#include <algorithm>
#include <cmath>

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
// The phases that the dashboard shows, in the order of a measurement.
constexpr Phase shown_phases[] = {Phase::ping, Phase::download, Phase::upload};

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

std::string phase_title(Phase phase) {
    switch (phase) {
    case Phase::select:
        return "Выбор сервера";
    case Phase::ping:
        return "Задержка";
    case Phase::download:
        return "Загрузка";
    case Phase::upload:
        return "Отдача";
    case Phase::connection:
        break;
    }
    return "Подключение";
}

Metric phase_metric(Phase phase) noexcept {
    switch (phase) {
    case Phase::ping:
        return Metric::ping;
    case Phase::download:
        return Metric::download;
    case Phase::upload:
        return Metric::upload;
    default:
        return Metric::none;
    }
}

bool selected(app::PhaseSelection only, Phase phase) noexcept {
    switch (only) {
    case app::PhaseSelection::ping:
        return phase == Phase::ping;
    case app::PhaseSelection::download:
        return phase == Phase::download;
    case app::PhaseSelection::upload:
        return phase == Phase::upload;
    case app::PhaseSelection::all:
        break;
    }
    return true;
}

std::vector<PhaseView> initial_phases(app::PhaseSelection only) {
    std::vector<PhaseView> views;
    for (const Phase phase : shown_phases) {
        views.push_back({phase_title(phase),
                         selected(only, phase) ? PhaseState::pending : PhaseState::skipped, 0});
    }
    return views;
}

PhaseState phase_state(Status status) noexcept {
    switch (status) {
    case Status::pending:
        return PhaseState::pending;
    case Status::ok:
    case Status::partial:
        return PhaseState::done;
    case Status::error:
        return PhaseState::failed;
    case Status::skipped:
    case Status::canceled:
        break;
    }
    return PhaseState::skipped;
}

void set_phase_state(PhaseView& view, PhaseState state) noexcept {
    view.state = state;
    view.progress = state == PhaseState::done || state == PhaseState::failed ? 1 : 0;
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

Tone status_tone(Status status) noexcept {
    switch (status) {
    case Status::ok:
        return Tone::success;
    case Status::partial:
    case Status::canceled:
        return Tone::warning;
    default:
        return Tone::danger;
    }
}

std::string phase_value(const app::PhaseResult& result) {
    if (result.mbps) {
        return format_speed(*result.mbps) + " Мбит/с";
    }
    if (result.value_ms) {
        return format_latency(*result.value_ms) + " мс";
    }
    if (result.status == Status::error) {
        return "ошибка";
    }
    return "—";
}

std::string server_text(const std::optional<app::ServerResult>& server) {
    if (!server) {
        return {};
    }
    return format_server({server->name, server->city.value_or(""), server->region.value_or("")});
}

ServiceResult service_result(const app::MeasurementResult& result) {
    ServiceResult value;
    value.service = display_service(result.service);
    value.status = status_text(result.status);
    value.tone = status_tone(result.status);
    value.ping = phase_value(result.phases.ping);
    value.download = phase_value(result.phases.download);
    value.upload = phase_value(result.phases.upload);
    value.server = server_text(result.server);
    return value;
}

std::string_view plural(int count, std::string_view one, std::string_view few,
                        std::string_view many) noexcept {
    const int tens = count % 100;
    const int units = count % 10;
    if (tens >= 11 && tens <= 14) {
        return many;
    }
    if (units == 1) {
        return one;
    }
    return units >= 2 && units <= 4 ? few : many;
}

std::string decimal_comma(std::string value) {
    std::replace(value.begin(), value.end(), '.', ',');
    return value;
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

std::string_view to_string(PhaseState state) noexcept {
    switch (state) {
    case PhaseState::active:
        return "active";
    case PhaseState::done:
        return "done";
    case PhaseState::failed:
        return "failed";
    case PhaseState::skipped:
        return "skipped";
    case PhaseState::pending:
        break;
    }
    return "pending";
}

std::string_view to_string(Metric metric) noexcept {
    switch (metric) {
    case Metric::ping:
        return "ping";
    case Metric::download:
        return "download";
    case Metric::upload:
        return "upload";
    case Metric::none:
        break;
    }
    return "";
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
                                                    "Оба сервиса"};
    return labels;
}

const std::vector<std::string>& service_names() {
    static const std::vector<std::string> names = {"Яндекс", "speedtest.ru", "Оба"};
    return names;
}

const std::vector<std::string>& profile_names() {
    static const std::vector<std::string> names = {"Быстрый", "Сбалансированный", "Точный"};
    return names;
}

const std::vector<std::string>& profile_details() {
    static const std::vector<std::string> details = [] {
        std::vector<std::string> values;
        for (const app::Profile profile : profiles) {
            values.push_back(std::to_string(app::profile_duration(profile).count()) + " с");
        }
        return values;
    }();
    return details;
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

const std::vector<std::string>& phase_names() {
    static const std::vector<std::string> names = {"Все", "Задержка", "Загрузка", "Отдача"};
    return names;
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
    std::vector<std::string> parts = {
        profile_names()[static_cast<std::size_t>(profile_index(settings.profile))],
        std::to_string(settings.duration.count()) + " с",
    };
    if (settings.connections > 0) {
        parts.push_back(
            std::to_string(settings.connections) + " " +
            std::string(plural(settings.connections, "соединение", "соединения", "соединений")));
    }
    switch (settings.only) {
    case app::PhaseSelection::ping:
        parts.emplace_back("только задержка");
        break;
    case app::PhaseSelection::download:
        parts.emplace_back("только загрузка");
        break;
    case app::PhaseSelection::upload:
        parts.emplace_back("только отдача");
        break;
    case app::PhaseSelection::all:
        break;
    }
    return text::join(parts, " · ");
}

std::string idle_hint(const Settings& settings) {
    std::string measured = "задержку, загрузку и отдачу";
    switch (settings.only) {
    case app::PhaseSelection::ping:
        measured = "задержку";
        break;
    case app::PhaseSelection::download:
        measured = "загрузку";
        break;
    case app::PhaseSelection::upload:
        measured = "отдачу";
        break;
    case app::PhaseSelection::all:
        break;
    }
    std::string through = "через Яндекс.Интернетометр";
    switch (settings.service) {
    case ServiceId::speedtest:
        through = "через speedtest.ru";
        break;
    case ServiceId::all:
        through = "через Яндекс.Интернетометр и speedtest.ru по очереди";
        break;
    case ServiceId::yandex:
        break;
    }
    return "Измерим " + measured + " " + through + ".";
}

std::string display_service(ServiceId id) {
    switch (id) {
    case ServiceId::yandex:
        return "Яндекс.Интернетометр";
    case ServiceId::all:
        return "Оба сервиса";
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

std::string format_speed(double mbps) {
    if (!std::isfinite(mbps)) {
        return "—";
    }
    // The thresholds take rounding into account: 99,96 becomes "100".
    const int decimals = mbps >= 99.95 ? 0 : mbps >= 9.995 ? 1 : 2;
    return decimal_comma(text::format_fixed(mbps, decimals));
}

std::string format_latency(double milliseconds) {
    if (!std::isfinite(milliseconds)) {
        return "—";
    }
    return decimal_comma(text::format_fixed(milliseconds, milliseconds >= 9.95 ? 0 : 1));
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
    state_.phases = initial_phases(settings_.only);
    previous_connection_ = state_.connection;
}

std::string Dashboard::start_label() const {
    if (measuring()) {
        return state_.stopping ? "Останавливаем…" : "Остановить";
    }
    return state_.has_result ? "Проверить снова" : "Начать проверку";
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
    // Before the first measurement the phases show what will be measured.
    if (!busy() && !state_.has_result) {
        state_.phases = initial_phases(settings_.only);
    }
    return {};
}

void Dashboard::save_window(double width, double height) {
    gui::save_window(store_, width, height);
}

void Dashboard::reset_service() {
    state_.hero_value.clear();
    state_.hero_unit = "Мбит/с";
    state_.hero_label.clear();
    state_.phases = initial_phases(settings_.only);
    state_.active_metric = Metric::none;
    state_.ping = "—";
    state_.jitter = "—";
    state_.download = "—";
    state_.upload = "—";
    state_.server = "Выбор сервера измерения…";
    state_.server_known = false;
}

void Dashboard::begin_measurement() {
    state_.status = "Подготовка проверки…";
    state_.status_tone = Tone::neutral;
    state_.service_progress.clear();
    state_.notices.clear();
    state_.notice_tone = Tone::warning;
    state_.results.clear();
    reset_service();
    run_services_ = settings_.service == ServiceId::all ? 2 : 1;
    started_services_ = 0;
    service_.reset();
    state_.activity = Activity::measurement;
    state_.stopping = false;
}

void Dashboard::begin_connection() {
    start_lookup();
    state_.activity = Activity::connection;
    state_.stopping = false;
}

void Dashboard::begin_stopping() {
    if (busy()) {
        state_.stopping = true;
    }
}

void Dashboard::start_lookup() {
    if (lookup_active_) {
        return;
    }
    lookup_active_ = true;
    previous_connection_ = state_.connection;
    previous_connection_known_ = state_.connection_known;
    state_.connection = "Определение IP и интернет-провайдера…";
    state_.connection_known = false;
}

PhaseView* Dashboard::phase_view(Phase phase) {
    for (std::size_t index = 0; index < std::size(shown_phases); ++index) {
        if (shown_phases[index] == phase && index < state_.phases.size()) {
            return &state_.phases[index];
        }
    }
    return nullptr;
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

std::string Dashboard::notice_subject(std::string_view subject) const {
    if (run_services_ > 1 && service_) {
        return std::string(subject) + " (" + display_service(*service_) + ")";
    }
    return std::string(subject);
}

void Dashboard::apply_event(const app::RunEvent& event) {
    switch (event.kind) {
    case app::EventKind::connection_started:
        start_lookup();
        break;
    case app::EventKind::connection_completed:
        if (event.connection) {
            apply_connection(*event.connection);
        }
        break;
    case app::EventKind::service_started:
        if (event.service) {
            ++started_services_;
            service_ = event.service;
            if (started_services_ > 1) {
                reset_service();
            }
            if (run_services_ > 1) {
                state_.service_progress = "Сервис " + std::to_string(started_services_) + " из " +
                                          std::to_string(run_services_) + " · " +
                                          display_service(*event.service);
            }
        }
        break;
    case app::EventKind::phase_started:
        if (event.phase) {
            start_phase(*event.phase);
        }
        break;
    case app::EventKind::server_selected:
        if (event.server) {
            state_.server = format_server(*event.server);
            state_.server_known = true;
        }
        break;
    case app::EventKind::ping_completed:
        if (event.ping) {
            state_.ping = format_latency(event.ping->value_ms);
            state_.jitter = format_latency(event.ping->jitter_ms);
            state_.hero_value = state_.ping;
        }
        break;
    case app::EventKind::throughput_progress:
        if (event.phase && event.throughput) {
            const std::string value = format_speed(event.throughput->mbps);
            if (std::string* tile = metric(*event.phase)) {
                *tile = value;
            }
            state_.hero_value = value;
            if (PhaseView* view = phase_view(*event.phase)) {
                const auto duration =
                    std::chrono::duration_cast<std::chrono::nanoseconds>(settings_.duration);
                view->progress =
                    duration.count() > 0
                        ? std::clamp(static_cast<double>(event.throughput->elapsed.count()) /
                                         static_cast<double>(duration.count()),
                                     0.0, 1.0)
                        : 0;
            }
        }
        break;
    case app::EventKind::phase_completed:
        if (event.phase && event.phase_result) {
            apply_phase_result(*event.phase, *event.phase_result);
        }
        break;
    case app::EventKind::service_completed:
        if (event.measurement) {
            apply_measurement(*event.measurement);
        }
        break;
    case app::EventKind::run_started:
    case app::EventKind::run_completed:
        break;
    }
}

void Dashboard::start_phase(Phase phase) {
    state_.status = phase_status(phase);
    PhaseView* view = phase_view(phase);
    if (view == nullptr) {
        return;
    }
    view->state = PhaseState::active;
    // Latency has no progress; throughput reports its elapsed time.
    view->progress = phase == Phase::ping ? -1 : 0;
    state_.active_metric = phase_metric(phase);
    state_.hero_value.clear();
    state_.hero_unit = phase == Phase::ping ? "мс" : "Мбит/с";
    state_.hero_label = phase_title(phase);
}

void Dashboard::apply_phase_result(Phase phase, const app::PhaseResult& result) {
    const PhaseState state = phase_state(result.status);
    if (PhaseView* view = phase_view(phase)) {
        set_phase_state(*view, state);
    }
    if (state_.active_metric == phase_metric(phase)) {
        state_.active_metric = Metric::none;
    }
    if (state == PhaseState::done && result.mbps) {
        const std::string value = format_speed(*result.mbps);
        if (std::string* tile = metric(phase)) {
            *tile = value;
        }
        state_.hero_value = value;
    } else if (state != PhaseState::done && phase != Phase::select) {
        // A failed or stopped phase has no result, whatever it showed live.
        if (std::string* tile = metric(phase)) {
            *tile = "—";
            if (phase == Phase::ping) {
                state_.jitter = "—";
            }
        }
        if (state_.hero_label == phase_title(phase)) {
            state_.hero_value.clear();
        }
    }
    if (result.status == Status::error && result.error) {
        add_notice(Tone::danger, notice_subject(phase_title(phase)) + ": " + result.error->message);
    }
}

void Dashboard::apply_measurement(const app::MeasurementResult& result) {
    // The final states; a failed server selection skips the phases without
    // completing them one by one.
    const app::PhaseResult* results[] = {&result.phases.ping, &result.phases.download,
                                         &result.phases.upload};
    for (std::size_t index = 0; index < std::size(results); ++index) {
        if (PhaseView* view = phase_view(shown_phases[index])) {
            set_phase_state(*view, phase_state(results[index]->status));
        }
    }
    if (!result.server) {
        state_.server = "Сервер не выбран";
        state_.server_known = false;
    }
    state_.active_metric = Metric::none;
    if (run_services_ > 1) {
        state_.results.push_back(service_result(result));
    }
    for (const auto& warning : result.warnings) {
        add_notice(Tone::warning,
                   run_services_ > 1 ? display_service(result.service) + ": " + warning : warning);
    }
}

void Dashboard::apply_connection(const app::ConnectionResult& result) {
    lookup_active_ = false;
    if (result.status == Status::canceled) {
        state_.connection = previous_connection_;
        state_.connection_known = previous_connection_known_;
        return;
    }
    if (result.status != Status::ok || !result.external_ip) {
        std::string message = "Не удалось определить IP";
        if (result.error) {
            message += ": " + result.error->message;
        }
        state_.connection = std::move(message);
        state_.connection_known = false;
        return;
    }
    std::vector<std::string> parts = {*result.external_ip};
    if (result.isp) {
        parts.push_back(*result.isp);
    }
    state_.connection = text::join(parts, " · ");
    state_.connection_known = true;
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

void Dashboard::show_summary(const app::Envelope& envelope) {
    // The download speed of the last service with a value, otherwise its
    // upload speed or latency.
    for (auto result = envelope.results.rbegin(); result != envelope.results.rend(); ++result) {
        const app::MeasurementPhases& measured = result->phases;
        if (measured.download.mbps) {
            state_.hero_value = format_speed(*measured.download.mbps);
            state_.hero_unit = "Мбит/с";
            state_.hero_label = phase_title(Phase::download);
        } else if (measured.upload.mbps) {
            state_.hero_value = format_speed(*measured.upload.mbps);
            state_.hero_unit = "Мбит/с";
            state_.hero_label = phase_title(Phase::upload);
        } else if (measured.ping.value_ms) {
            state_.hero_value = format_latency(*measured.ping.value_ms);
            state_.hero_unit = "мс";
            state_.hero_label = phase_title(Phase::ping);
        } else {
            continue;
        }
        state_.hero_label += " · " + display_service(result->service);
        return;
    }
    state_.hero_value.clear();
    state_.hero_label = "Нет результата";
}

void Dashboard::finish_measurement(const app::Envelope& envelope) {
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
    // Nothing is in progress any more.
    for (PhaseView& view : state_.phases) {
        if (view.state == PhaseState::pending || view.state == PhaseState::active) {
            set_phase_state(view, PhaseState::skipped);
        }
    }
    state_.active_metric = Metric::none;
    state_.service_progress.clear();
    show_summary(envelope);
    state_.activity = Activity::idle;
    state_.stopping = false;
    state_.has_result = true;
}

void Dashboard::finish_connection(const app::ConnectionResult& result) {
    // The runner reports the result as an event first.
    if (lookup_active_) {
        apply_connection(result);
    }
    state_.activity = Activity::idle;
    state_.stopping = false;
}

} // namespace puls::gui
