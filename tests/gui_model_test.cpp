#include "puls/gui/model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>

namespace puls::gui {
namespace {

using namespace std::chrono_literals;
using service::Phase;
using service::ServiceId;
using service::Status;

class MemoryStore final : public PreferenceStore {
public:
    [[nodiscard]] std::optional<std::string> text(std::string_view key) const override {
        const auto found = texts.find(std::string(key));
        return found == texts.end() ? std::nullopt : std::optional<std::string>(found->second);
    }
    [[nodiscard]] std::optional<double> number(std::string_view key) const override {
        const auto found = numbers.find(std::string(key));
        return found == numbers.end() ? std::nullopt : std::optional<double>(found->second);
    }
    void set_text(std::string_view key, std::string_view value) override {
        texts[std::string(key)] = std::string(value);
    }
    void set_number(std::string_view key, double value) override {
        numbers[std::string(key)] = value;
    }

    std::map<std::string, std::string> texts;
    std::map<std::string, double> numbers;
};

app::RunEvent event(app::EventKind kind) {
    app::RunEvent value;
    value.kind = kind;
    return value;
}

SettingsInput input(int service, int profile, std::string duration, int connections, int phase,
                    std::string server, bool show_ip, int theme) {
    SettingsInput value;
    value.service = service;
    value.profile = profile;
    value.duration = std::move(duration);
    value.connections = connections;
    value.phase = phase;
    value.server = std::move(server);
    value.show_ip = show_ip;
    value.theme = theme;
    return value;
}

TEST(GuiPreferences, SanitizeInvalidValues) {
    MemoryStore store;
    store.set_text("theme", "neon");
    store.set_text("service", "unknown");
    store.set_text("profile", "slow");
    store.set_number("duration_seconds", 90);
    store.set_number("connections", 99);
    store.set_text("phase", "everything");
    store.set_number("window_width", 10);
    store.set_number("window_height", 10);
    const Preferences preferences = load_preferences(store);
    EXPECT_EQ(preferences.theme, ThemeMode::system);
    EXPECT_EQ(preferences.service, ServiceId::yandex);
    EXPECT_EQ(preferences.profile, app::Profile::balanced);
    EXPECT_EQ(preferences.duration, 10s);
    EXPECT_EQ(preferences.connections, 0);
    EXPECT_EQ(preferences.only, app::PhaseSelection::all);
    EXPECT_EQ(preferences.window_width, default_window_width);
    EXPECT_EQ(preferences.window_height, default_window_height);
}

TEST(GuiPreferences, LoadsStoredValuesAndProfileDurationFallback) {
    MemoryStore store;
    store.set_text("theme", "dark");
    store.set_text("service", "all");
    store.set_text("profile", "quick");
    store.set_number("duration_seconds", 2);
    store.set_number("connections", 8);
    store.set_text("phase", "upload");
    store.set_number("window_width", 800);
    store.set_number("window_height", 900);
    const Preferences preferences = load_preferences(store);
    EXPECT_EQ(preferences.theme, ThemeMode::dark);
    EXPECT_EQ(preferences.service, ServiceId::all);
    EXPECT_EQ(preferences.profile, app::Profile::quick);
    EXPECT_EQ(preferences.duration, 5s);
    EXPECT_EQ(preferences.connections, 8);
    EXPECT_EQ(preferences.only, app::PhaseSelection::upload);
    EXPECT_EQ(preferences.window_width, 800);
    EXPECT_EQ(preferences.window_height, 900);
    EXPECT_EQ(load_preferences(MemoryStore()).duration, 10s);
}

TEST(GuiPreferences, NeverPersistPrivateResults) {
    MemoryStore store;
    Dashboard dashboard(store);
    ASSERT_FALSE(dashboard.apply_settings(input(1, 0, "7", 4, 1, "private.example:443", true, 2)));
    app::ConnectionResult connection;
    connection.status = Status::ok;
    connection.external_ip = "203.0.113.7";
    connection.isp = "Ростелеком";
    app::RunEvent completed = event(app::EventKind::connection_completed);
    completed.connection = connection;
    dashboard.apply_event(completed);
    dashboard.save_window(500, 700);

    std::vector<std::string> keys;
    for (const auto& [key, value] : store.texts) {
        keys.push_back(key);
        EXPECT_EQ(value.find("203.0.113.7"), std::string::npos);
        EXPECT_EQ(value.find("private.example"), std::string::npos);
    }
    for (const auto& [key, value] : store.numbers) {
        keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys,
              (std::vector<std::string>{"connections", "duration_seconds", "phase", "profile",
                                        "service", "theme", "window_height", "window_width"}));
    EXPECT_EQ(store.texts["service"], "speedtest");
    EXPECT_EQ(store.numbers["duration_seconds"], 7);
    EXPECT_EQ(store.texts["theme"], "dark");
}

TEST(GuiPreferences, IgnoresTooSmallWindow) {
    MemoryStore store;
    save_window(store, 389, 700);
    save_window(store, 500, 599);
    EXPECT_TRUE(store.numbers.empty());
    save_window(store, 390, 600);
    EXPECT_EQ(store.numbers["window_width"], 390);
    EXPECT_EQ(store.numbers["window_height"], 600);
}

TEST(GuiSettings, ParseValidatesAndClearsForeignServer) {
    const auto settings = parse_settings(input(0, 0, "8", 4, 1, "server.example:443", true, 2));
    ASSERT_TRUE(settings) << settings.error().message();
    EXPECT_EQ(settings->service, ServiceId::yandex);
    EXPECT_EQ(settings->server, "");
    EXPECT_EQ(settings->duration, 8s);
    EXPECT_EQ(settings->theme, ThemeMode::dark);
    EXPECT_TRUE(settings->show_ip);
    EXPECT_EQ(settings_summary(*settings), "Быстрый · 8 с · 4 соединения · только задержка");
    EXPECT_EQ(idle_hint(*settings), "Измерим задержку через Яндекс.Интернетометр.");

    const auto speedtest = parse_settings(input(1, 1, " 12 ", 0, 0, " qms.example:443 ", false, 0));
    ASSERT_TRUE(speedtest);
    EXPECT_EQ(speedtest->server, "qms.example:443");
    EXPECT_EQ(settings_summary(*speedtest), "Сбалансированный · 12 с");
    EXPECT_EQ(idle_hint(*speedtest), "Измерим задержку, загрузку и отдачу через speedtest.ru.");

    const auto both = parse_settings(input(2, 2, "15", 1, 2, "", false, 0));
    ASSERT_TRUE(both);
    EXPECT_EQ(settings_summary(*both), "Точный · 15 с · 1 соединение · только загрузка");
    EXPECT_EQ(idle_hint(*both),
              "Измерим загрузку через Яндекс.Интернетометр и speedtest.ru по очереди.");
    const auto many = parse_settings(input(0, 1, "10", 16, 3, "", false, 0));
    ASSERT_TRUE(many);
    EXPECT_EQ(settings_summary(*many), "Сбалансированный · 10 с · 16 соединений · только отдача");
}

TEST(GuiSettings, ParseRejectsInvalidInput) {
    const std::vector<std::pair<SettingsInput, std::string>> cases = {
        {input(1, 0, "2", 4, 0, "", false, 0), "длительность должна быть от 3 до 60 секунд"},
        {input(1, 0, "61", 4, 0, "", false, 0), "длительность должна быть от 3 до 60 секунд"},
        {input(1, 0, "abc", 4, 0, "", false, 0), "длительность должна быть от 3 до 60 секунд"},
        {input(3, 0, "8", 4, 0, "", false, 0), "выберите сервис измерения"},
        {input(0, -1, "8", 4, 0, "", false, 0), "выберите профиль"},
        {input(0, 0, "8", 17, 0, "", false, 0), "выберите число соединений"},
        {input(0, 0, "8", 0, 4, "", false, 0), "выберите этап измерения"},
        {input(0, 0, "8", 0, 0, "", false, 3), "выберите оформление"},
    };
    for (const auto& [value, message] : cases) {
        const auto settings = parse_settings(value);
        ASSERT_FALSE(settings) << message;
        EXPECT_EQ(settings.error().message(), message);
    }
}

TEST(GuiSettings, LabelsAndIndexes) {
    EXPECT_EQ(service_labels(),
              (std::vector<std::string>{"Яндекс.Интернетометр", "speedtest.ru", "Оба сервиса"}));
    EXPECT_EQ(service_names(), (std::vector<std::string>{"Яндекс", "speedtest.ru", "Оба"}));
    EXPECT_EQ(profile_names().size(), 3u);
    EXPECT_EQ(profile_details(), (std::vector<std::string>{"5 с", "10 с", "15 с"}));
    EXPECT_EQ(phase_names().size(), phase_labels().size());
    EXPECT_EQ(connection_labels().size(), 17u);
    EXPECT_EQ(connection_labels()[16], "16");
    EXPECT_EQ(service_index(ServiceId::all), 2);
    EXPECT_EQ(profile_index(app::Profile::accurate), 2);
    EXPECT_EQ(connection_index(0), 0);
    EXPECT_EQ(connection_index(12), 12);
    EXPECT_EQ(phase_index(app::PhaseSelection::download), 2);
    EXPECT_EQ(theme_index(ThemeMode::light), 1);
    EXPECT_EQ(profile_at(0), app::Profile::quick);
    EXPECT_FALSE(profile_at(3));
    EXPECT_EQ(display_service(ServiceId::speedtest), "speedtest.ru");
    EXPECT_EQ(format_server({"host", "Москва", "Москва"}), "host · Москва");
    EXPECT_EQ(format_server({"host", "Казань", "Татарстан"}), "host · Казань · Татарстан");
    EXPECT_EQ(to_string(PhaseState::failed), "failed");
    EXPECT_EQ(to_string(Metric::download), "download");
    EXPECT_EQ(to_string(Metric::none), "");
}

TEST(GuiSettings, NumbersLoseDecimalsAsTheyGrow) {
    EXPECT_EQ(format_speed(0), "0,00");
    EXPECT_EQ(format_speed(8.754), "8,75");
    EXPECT_EQ(format_speed(9.996), "10,0");
    EXPECT_EQ(format_speed(94.24), "94,2");
    EXPECT_EQ(format_speed(99.96), "100");
    EXPECT_EQ(format_speed(487.4), "487");
    EXPECT_EQ(format_speed(std::numeric_limits<double>::infinity()), "—");
    EXPECT_EQ(format_latency(1.84), "1,8");
    EXPECT_EQ(format_latency(9.96), "10");
    EXPECT_EQ(format_latency(12.4), "12");
}

TEST(GuiDashboard, StartsFromPreferencesAndPersistsChoices) {
    MemoryStore store;
    store.set_text("service", "speedtest");
    store.set_text("theme", "light");
    Dashboard dashboard(store);
    EXPECT_EQ(dashboard.settings().service, ServiceId::speedtest);
    EXPECT_EQ(dashboard.settings().theme, ThemeMode::light);
    EXPECT_EQ(dashboard.start_label(), "Начать проверку");
    dashboard.cycle_theme();
    EXPECT_EQ(dashboard.settings().theme, ThemeMode::dark);
    dashboard.cycle_theme();
    EXPECT_EQ(dashboard.settings().theme, ThemeMode::system);
    EXPECT_EQ(store.texts["theme"], "system");
    dashboard.select_service(2);
    EXPECT_EQ(dashboard.settings().service, ServiceId::all);
    EXPECT_EQ(store.texts["service"], "all");
    EXPECT_FALSE(dashboard.connection_request().explicit_service);
    dashboard.select_service(0);
    EXPECT_TRUE(dashboard.connection_request().explicit_service);
    EXPECT_EQ(dashboard.connection_request().service, ServiceId::yandex);
    EXPECT_TRUE(dashboard.apply_settings(input(0, 0, "1", 0, 0, "", false, 0)));
    EXPECT_EQ(dashboard.settings().service, ServiceId::yandex);
}

std::vector<std::string> phase_states(const Dashboard& dashboard) {
    std::vector<std::string> states;
    for (const PhaseView& phase : dashboard.state().phases) {
        states.emplace_back(to_string(phase.state));
    }
    return states;
}

TEST(GuiDashboard, PhasesShowWhatWillBeMeasured) {
    MemoryStore store;
    Dashboard dashboard(store);
    ASSERT_EQ(dashboard.state().phases.size(), 3u);
    EXPECT_EQ(dashboard.state().phases[0].title, "Задержка");
    EXPECT_EQ(dashboard.state().phases[2].title, "Отдача");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"pending", "pending", "pending"}));
    ASSERT_FALSE(dashboard.apply_settings(input(0, 1, "10", 0, 2, "", false, 0)));
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"skipped", "pending", "skipped"}));
}

TEST(GuiDashboard, ConnectionEventsUpdateConnection) {
    MemoryStore store;
    Dashboard dashboard(store);
    EXPECT_FALSE(dashboard.state().connection_known);
    app::ConnectionResult connection;
    connection.status = Status::ok;
    connection.external_ip = "203.0.113.7";
    connection.isp = "Ростелеком";
    connection.detected_by = ServiceId::speedtest;
    connection.warnings = {"использован резервный сервис Яндекс"};
    app::RunEvent completed = event(app::EventKind::connection_completed);
    completed.connection = connection;
    dashboard.apply_event(event(app::EventKind::connection_started));
    EXPECT_EQ(dashboard.state().connection, "Определение IP и интернет-провайдера…");
    dashboard.apply_event(completed);
    EXPECT_EQ(dashboard.state().connection, "203.0.113.7 · Ростелеком");
    EXPECT_TRUE(dashboard.state().connection_known);
    EXPECT_EQ(dashboard.state().notices,
              (std::vector<std::string>{"использован резервный сервис Яндекс"}));
}

TEST(GuiDashboard, NoticesKeepTheMostSevereTone) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.add_notice(Tone::warning, "резервный сервер");
    dashboard.add_notice(Tone::warning, " резервный сервер ");
    EXPECT_EQ(dashboard.state().notice_tone, Tone::warning);
    dashboard.add_notice(Tone::danger, "загрузка: ошибка");
    dashboard.add_notice(Tone::warning, "ещё предупреждение");
    EXPECT_EQ(
        dashboard.state().notices,
        (std::vector<std::string>{"резервный сервер", "загрузка: ошибка", "ещё предупреждение"}));
    EXPECT_EQ(dashboard.state().notice_tone, Tone::danger);
}

app::RunEvent phase_event(app::EventKind kind, Phase phase) {
    app::RunEvent value = event(kind);
    value.service = ServiceId::yandex;
    value.phase = phase;
    return value;
}

app::RunEvent phase_done(Phase phase, app::PhaseResult result) {
    app::RunEvent value = phase_event(app::EventKind::phase_completed, phase);
    value.phase_result = std::move(result);
    return value;
}

app::RunEvent service_event(app::EventKind kind, ServiceId id) {
    app::RunEvent value = event(kind);
    value.service = id;
    return value;
}

app::PhaseResult throughput(double mbps) {
    app::PhaseResult result = app::empty_phase(Status::ok);
    result.mbps = mbps;
    return result;
}

app::PhaseResult failure(Phase phase, std::string message) {
    app::PhaseResult result = app::empty_phase(Status::error);
    result.error =
        app::ErrorResult{service::ErrorCode::unavailable, phase, std::move(message), true};
    return result;
}

TEST(GuiDashboard, MeasurementLifecycle) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_measurement();
    EXPECT_TRUE(dashboard.busy());
    EXPECT_TRUE(dashboard.measuring());
    EXPECT_FALSE(dashboard.detecting());
    EXPECT_EQ(dashboard.start_label(), "Остановить");
    EXPECT_EQ(dashboard.state().status, "Подготовка проверки…");
    EXPECT_EQ(dashboard.state().server, "Выбор сервера измерения…");
    EXPECT_FALSE(dashboard.state().server_known);
    EXPECT_EQ(dashboard.state().hero_value, "");

    dashboard.apply_event(service_event(app::EventKind::service_started, ServiceId::yandex));
    EXPECT_EQ(dashboard.state().service_progress, "");
    dashboard.apply_event(phase_event(app::EventKind::phase_started, Phase::select));
    EXPECT_EQ(dashboard.state().status, "Выбор сервера…");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"pending", "pending", "pending"}));

    app::RunEvent selected = phase_event(app::EventKind::server_selected, Phase::select);
    selected.server = service::Server{"mock.example", "Владивосток", ""};
    dashboard.apply_event(selected);
    EXPECT_EQ(dashboard.state().server, "mock.example · Владивосток");
    EXPECT_TRUE(dashboard.state().server_known);

    dashboard.apply_event(phase_event(app::EventKind::phase_started, Phase::ping));
    EXPECT_EQ(dashboard.state().status, "Измерение задержки…");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"active", "pending", "pending"}));
    EXPECT_LT(dashboard.state().phases[0].progress, 0);
    EXPECT_EQ(dashboard.state().active_metric, Metric::ping);
    EXPECT_EQ(dashboard.state().hero_label, "Задержка");
    EXPECT_EQ(dashboard.state().hero_unit, "мс");

    app::RunEvent ping = phase_event(app::EventKind::ping_completed, Phase::ping);
    ping.ping = service::stats_with_method({10, 12, 11}, "median");
    dashboard.apply_event(ping);
    EXPECT_EQ(dashboard.state().ping, "11");
    EXPECT_EQ(dashboard.state().jitter, "1,5");
    EXPECT_EQ(dashboard.state().hero_value, "11");
    dashboard.apply_event(phase_done(Phase::ping, app::ping_phase(*ping.ping)));
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"done", "pending", "pending"}));
    EXPECT_EQ(dashboard.state().active_metric, Metric::none);

    dashboard.apply_event(phase_event(app::EventKind::phase_started, Phase::download));
    EXPECT_EQ(dashboard.state().status, "Измерение загрузки…");
    EXPECT_EQ(dashboard.state().active_metric, Metric::download);
    EXPECT_EQ(dashboard.state().hero_value, "");
    EXPECT_EQ(dashboard.state().hero_unit, "Мбит/с");
    app::RunEvent progress = phase_event(app::EventKind::throughput_progress, Phase::download);
    progress.throughput = service::ThroughputProgress{80.04, 1000, 5s, 2};
    dashboard.apply_event(progress);
    EXPECT_DOUBLE_EQ(dashboard.state().phases[1].progress, 0.5);
    EXPECT_EQ(dashboard.state().hero_value, "80,0");
    EXPECT_EQ(dashboard.state().download, "80,0");
    dashboard.apply_event(phase_done(Phase::download, throughput(100)));
    EXPECT_EQ(dashboard.state().download, "100");
    EXPECT_EQ(dashboard.state().hero_value, "100");
    EXPECT_DOUBLE_EQ(dashboard.state().phases[1].progress, 1);

    // A failed phase never keeps the live value as its result.
    dashboard.apply_event(phase_event(app::EventKind::phase_started, Phase::upload));
    progress.phase = Phase::upload;
    progress.throughput = service::ThroughputProgress{5, 1000, 1s, 2};
    dashboard.apply_event(progress);
    EXPECT_EQ(dashboard.state().upload, "5,00");
    dashboard.apply_event(phase_done(Phase::upload, failure(Phase::upload, "сервис недоступен")));
    EXPECT_EQ(dashboard.state().upload, "—");
    EXPECT_EQ(dashboard.state().hero_value, "");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"done", "done", "failed"}));
    EXPECT_EQ(dashboard.state().notices, (std::vector<std::string>{"Отдача: сервис недоступен"}));
    EXPECT_EQ(dashboard.state().notice_tone, Tone::danger);

    app::MeasurementResult measurement =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::all);
    measurement.status = Status::partial;
    measurement.server = app::ServerResult{"mock.example", "Владивосток", std::nullopt};
    measurement.phases.select.status = Status::ok;
    measurement.phases.ping = app::ping_phase(*ping.ping);
    measurement.phases.download = throughput(100);
    measurement.phases.upload = failure(Phase::upload, "сервис недоступен");
    measurement.warnings = {"сетевые сбои отдельных потоков: 1"};
    app::RunEvent service_done =
        service_event(app::EventKind::service_completed, ServiceId::yandex);
    service_done.measurement = measurement;
    dashboard.apply_event(service_done);
    // One service has no summary by service.
    EXPECT_TRUE(dashboard.state().results.empty());
    EXPECT_EQ(dashboard.state().notices.back(), "сетевые сбои отдельных потоков: 1");

    app::Envelope envelope = app::new_envelope(app::Command::measure);
    envelope.status = Status::partial;
    envelope.results = {measurement};
    dashboard.finish_measurement(envelope);
    EXPECT_FALSE(dashboard.busy());
    EXPECT_TRUE(dashboard.state().has_result);
    EXPECT_EQ(dashboard.state().status, "Получен частичный результат");
    EXPECT_EQ(dashboard.state().status_tone, Tone::warning);
    EXPECT_EQ(dashboard.state().hero_value, "100");
    EXPECT_EQ(dashboard.state().hero_unit, "Мбит/с");
    EXPECT_EQ(dashboard.state().hero_label, "Загрузка · Яндекс.Интернетометр");
    EXPECT_EQ(dashboard.start_label(), "Проверить снова");

    dashboard.begin_measurement();
    EXPECT_TRUE(dashboard.state().notices.empty());
    EXPECT_EQ(dashboard.state().ping, "—");
    EXPECT_EQ(dashboard.state().hero_value, "");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"pending", "pending", "pending"}));
}

TEST(GuiDashboard, StoppedMeasurementHasNoPendingPhases) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_measurement();
    dashboard.apply_event(phase_event(app::EventKind::phase_started, Phase::download));
    app::RunEvent progress = phase_event(app::EventKind::throughput_progress, Phase::download);
    progress.throughput = service::ThroughputProgress{80, 1000, 2s, 2};
    dashboard.apply_event(progress);
    dashboard.begin_stopping();
    EXPECT_TRUE(dashboard.state().stopping);
    EXPECT_EQ(dashboard.start_label(), "Останавливаем…");
    app::PhaseResult canceled = app::empty_phase(Status::canceled);
    dashboard.apply_event(phase_done(Phase::download, canceled));
    EXPECT_EQ(dashboard.state().download, "—");
    EXPECT_TRUE(dashboard.state().notices.empty());

    app::Envelope envelope = app::new_envelope(app::Command::measure);
    envelope.status = Status::canceled;
    dashboard.finish_measurement(envelope);
    EXPECT_FALSE(dashboard.state().stopping);
    EXPECT_EQ(dashboard.state().status, "Проверка остановлена");
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"skipped", "skipped", "skipped"}));
    EXPECT_EQ(dashboard.state().hero_value, "");
    EXPECT_EQ(dashboard.state().hero_label, "Нет результата");
}

TEST(GuiDashboard, FailedServerSelectionSkipsPhases) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_measurement();
    dashboard.apply_event(service_event(app::EventKind::service_started, ServiceId::yandex));
    dashboard.apply_event(phase_done(Phase::select, failure(Phase::select, "нет серверов")));
    app::MeasurementResult measurement =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::all);
    measurement.status = Status::error;
    measurement.phases.select = failure(Phase::select, "нет серверов");
    measurement.phases.ping.status = Status::skipped;
    measurement.phases.download.status = Status::skipped;
    measurement.phases.upload.status = Status::skipped;
    app::RunEvent service_done =
        service_event(app::EventKind::service_completed, ServiceId::yandex);
    service_done.measurement = measurement;
    dashboard.apply_event(service_done);
    EXPECT_EQ(phase_states(dashboard), (std::vector<std::string>{"skipped", "skipped", "skipped"}));
    EXPECT_EQ(dashboard.state().server, "Сервер не выбран");
    EXPECT_EQ(dashboard.state().notices, (std::vector<std::string>{"Выбор сервера: нет серверов"}));

    app::Envelope envelope = app::new_envelope(app::Command::measure);
    envelope.status = Status::error;
    envelope.results = {measurement};
    dashboard.finish_measurement(envelope);
    EXPECT_EQ(dashboard.state().status, "Проверка не выполнена");
    EXPECT_EQ(dashboard.state().status_tone, Tone::danger);
    EXPECT_EQ(dashboard.state().hero_label, "Нет результата");
}

TEST(GuiDashboard, BothServicesAreSummarizedByService) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.select_service(2);
    dashboard.begin_measurement();

    dashboard.apply_event(service_event(app::EventKind::service_started, ServiceId::yandex));
    EXPECT_EQ(dashboard.state().service_progress, "Сервис 1 из 2 · Яндекс.Интернетометр");
    app::RunEvent ping = phase_event(app::EventKind::ping_completed, Phase::ping);
    ping.ping = service::stats_with_method({12, 12, 12}, "median");
    dashboard.apply_event(ping);
    app::MeasurementResult yandex =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::all);
    yandex.status = Status::ok;
    yandex.server = app::ServerResult{"yandex.example", "Москва", std::nullopt};
    yandex.phases.ping = app::ping_phase(*ping.ping);
    yandex.phases.download = throughput(94.24);
    yandex.phases.upload = throughput(48.6);
    yandex.warnings = {"сетевые сбои отдельных потоков: 1"};
    app::RunEvent yandex_done = service_event(app::EventKind::service_completed, ServiceId::yandex);
    yandex_done.measurement = yandex;
    dashboard.apply_event(yandex_done);
    ASSERT_EQ(dashboard.state().results.size(), 1u);
    EXPECT_EQ(dashboard.state().results[0].service, "Яндекс.Интернетометр");
    EXPECT_EQ(dashboard.state().results[0].status, "готово");
    EXPECT_EQ(dashboard.state().results[0].tone, Tone::success);
    EXPECT_EQ(dashboard.state().results[0].ping, "12 мс");
    EXPECT_EQ(dashboard.state().results[0].download, "94,2 Мбит/с");
    EXPECT_EQ(dashboard.state().results[0].upload, "48,6 Мбит/с");
    EXPECT_EQ(dashboard.state().results[0].server, "yandex.example · Москва");
    EXPECT_EQ(
        dashboard.state().notices,
        (std::vector<std::string>{"Яндекс.Интернетометр: сетевые сбои отдельных потоков: 1"}));

    // The second service starts from a clean dashboard.
    dashboard.apply_event(service_event(app::EventKind::service_started, ServiceId::speedtest));
    EXPECT_EQ(dashboard.state().service_progress, "Сервис 2 из 2 · speedtest.ru");
    EXPECT_EQ(dashboard.state().ping, "—");
    EXPECT_FALSE(dashboard.state().server_known);
    app::RunEvent failed = phase_done(Phase::upload, failure(Phase::upload, "обрыв соединения"));
    failed.service = ServiceId::speedtest;
    dashboard.apply_event(failed);
    EXPECT_EQ(dashboard.state().notices.back(), "Отдача (speedtest.ru): обрыв соединения");

    app::MeasurementResult speedtest =
        app::new_measurement_result(ServiceId::speedtest, app::PhaseSelection::all);
    speedtest.status = Status::error;
    speedtest.phases.ping = failure(Phase::ping, "обрыв соединения");
    speedtest.phases.download.status = Status::canceled;
    speedtest.phases.upload = failure(Phase::upload, "обрыв соединения");
    app::RunEvent speedtest_done =
        service_event(app::EventKind::service_completed, ServiceId::speedtest);
    speedtest_done.measurement = speedtest;
    dashboard.apply_event(speedtest_done);
    ASSERT_EQ(dashboard.state().results.size(), 2u);
    EXPECT_EQ(dashboard.state().results[1].status, "ошибка");
    EXPECT_EQ(dashboard.state().results[1].tone, Tone::danger);
    EXPECT_EQ(dashboard.state().results[1].ping, "ошибка");
    EXPECT_EQ(dashboard.state().results[1].download, "—");
    EXPECT_EQ(dashboard.state().results[1].server, "");

    app::Envelope envelope = app::new_envelope(app::Command::measure);
    envelope.status = Status::partial;
    envelope.results = {yandex, speedtest};
    dashboard.finish_measurement(envelope);
    EXPECT_EQ(dashboard.state().service_progress, "");
    // The last service without a value does not hide the first one.
    EXPECT_EQ(dashboard.state().hero_value, "94,2");
    EXPECT_EQ(dashboard.state().hero_label, "Загрузка · Яндекс.Интернетометр");
}

TEST(GuiDashboard, ConnectionLifecycle) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_connection();
    EXPECT_TRUE(dashboard.busy());
    EXPECT_TRUE(dashboard.detecting());
    EXPECT_FALSE(dashboard.measuring());
    EXPECT_EQ(dashboard.start_label(), "Начать проверку");
    EXPECT_EQ(dashboard.state().connection, "Определение IP и интернет-провайдера…");
    app::ConnectionResult failed;
    failed.status = Status::error;
    failed.error = app::ErrorResult{service::ErrorCode::unavailable, Phase::connection,
                                    "сервис временно недоступен", true};
    app::RunEvent completed = event(app::EventKind::connection_completed);
    completed.connection = failed;
    dashboard.apply_event(completed);
    dashboard.finish_connection(failed);
    EXPECT_EQ(dashboard.state().connection, "Не удалось определить IP: сервис временно недоступен");
    EXPECT_FALSE(dashboard.state().connection_known);
    EXPECT_FALSE(dashboard.busy());
    // A lookup is not a measurement result.
    EXPECT_FALSE(dashboard.state().has_result);
    EXPECT_EQ(dashboard.state().status, "Готов к проверке");

    app::ConnectionResult found;
    found.status = Status::ok;
    found.external_ip = "203.0.113.8";
    dashboard.begin_connection();
    dashboard.finish_connection(found);
    EXPECT_EQ(dashboard.state().connection, "203.0.113.8");
    EXPECT_TRUE(dashboard.state().connection_known);

    // Stopping a lookup keeps what was known before it.
    app::ConnectionResult canceled;
    canceled.status = Status::canceled;
    dashboard.begin_connection();
    EXPECT_FALSE(dashboard.state().connection_known);
    dashboard.begin_stopping();
    EXPECT_TRUE(dashboard.state().stopping);
    dashboard.finish_connection(canceled);
    EXPECT_EQ(dashboard.state().connection, "203.0.113.8");
    EXPECT_TRUE(dashboard.state().connection_known);
    EXPECT_FALSE(dashboard.state().stopping);
}

} // namespace
} // namespace puls::gui
