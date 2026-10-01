#include "puls/gui/model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
    save_window(store, 300, 500);
    EXPECT_TRUE(store.numbers.empty());
    save_window(store, 390, 640);
    EXPECT_EQ(store.numbers["window_width"], 390);
}

TEST(GuiSettings, ParseValidatesAndClearsForeignServer) {
    const auto settings = parse_settings(input(0, 0, "8", 4, 1, "server.example:443", true, 2));
    ASSERT_TRUE(settings) << settings.error().message();
    EXPECT_EQ(settings->service, ServiceId::yandex);
    EXPECT_EQ(settings->server, "");
    EXPECT_EQ(settings->duration, 8s);
    EXPECT_EQ(settings->theme, ThemeMode::dark);
    EXPECT_TRUE(settings->show_ip);
    EXPECT_EQ(settings_summary(*settings), "Быстрый  ·  8 с  ·  4  ·  Только задержка");

    const auto speedtest = parse_settings(input(1, 1, " 12 ", 0, 0, " qms.example:443 ", false, 0));
    ASSERT_TRUE(speedtest);
    EXPECT_EQ(speedtest->server, "qms.example:443");
    EXPECT_EQ(settings_summary(*speedtest),
              "Сбалансированный  ·  12 с  ·  Автоматически  ·  Все этапы");
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
    EXPECT_EQ(service_labels().size(), 3u);
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
    EXPECT_EQ(format_number(91.25), "91,25");
    EXPECT_EQ(format_number(11), "11,00");
    EXPECT_EQ(display_service(ServiceId::speedtest), "speedtest.ru");
    EXPECT_EQ(format_server({"host", "Москва", "Москва"}), "host · Москва");
    EXPECT_EQ(format_server({"host", "Казань", "Татарстан"}), "host · Казань · Татарстан");
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

TEST(GuiDashboard, RendersConnectionAndMeasurementEvents) {
    MemoryStore store;
    Dashboard dashboard(store);
    app::ConnectionResult connection;
    connection.status = Status::ok;
    connection.external_ip = "203.0.113.7";
    connection.isp = "Ростелеком";
    connection.detected_by = ServiceId::speedtest;
    app::RunEvent completed = event(app::EventKind::connection_completed);
    completed.connection = connection;
    dashboard.apply_event(completed);
    EXPECT_EQ(dashboard.state().connection, "203.0.113.7 · Ростелеком · через speedtest.ru");

    app::RunEvent download = event(app::EventKind::phase_completed);
    download.phase = Phase::download;
    download.phase_result = app::empty_phase(Status::ok);
    download.phase_result->mbps = 91.25;
    dashboard.apply_event(download);
    EXPECT_EQ(dashboard.state().download, "91,25");
    EXPECT_EQ(dashboard.state().current_value, "91,25");

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

TEST(GuiDashboard, MeasurementLifecycle) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_measurement();
    EXPECT_TRUE(dashboard.busy());
    EXPECT_EQ(dashboard.start_label(), "Остановить");
    EXPECT_EQ(dashboard.state().status, "Подготовка проверки…");
    EXPECT_EQ(dashboard.state().server, "Выбор сервера измерения…");
    EXPECT_TRUE(dashboard.state().progress_visible);

    app::RunEvent started = event(app::EventKind::service_started);
    started.service = ServiceId::yandex;
    dashboard.apply_event(started);
    EXPECT_EQ(dashboard.state().status, "Сервис: Яндекс.Интернетометр");

    app::RunEvent selected = event(app::EventKind::server_selected);
    selected.phase = Phase::select;
    selected.server = service::Server{"mock.example", "Владивосток", ""};
    dashboard.apply_event(selected);
    EXPECT_EQ(dashboard.state().server, "mock.example · Владивосток");

    app::RunEvent ping_started = event(app::EventKind::phase_started);
    ping_started.phase = Phase::ping;
    dashboard.apply_event(ping_started);
    EXPECT_EQ(dashboard.state().status, "Измерение задержки…");
    EXPECT_EQ(dashboard.state().current_unit, "мс");

    app::RunEvent ping = event(app::EventKind::ping_completed);
    ping.ping = service::stats_with_method({10, 12, 11}, "median");
    dashboard.apply_event(ping);
    EXPECT_EQ(dashboard.state().ping, "11,00");
    EXPECT_EQ(dashboard.state().jitter, "1,50");

    app::RunEvent progress = event(app::EventKind::throughput_progress);
    progress.phase = Phase::download;
    progress.throughput = service::ThroughputProgress{80, 1000, 5s, 2};
    dashboard.apply_event(progress);
    EXPECT_DOUBLE_EQ(dashboard.state().progress, 0.5);
    EXPECT_EQ(dashboard.state().current_value, "80,00");

    app::RunEvent failed = event(app::EventKind::phase_completed);
    failed.phase = Phase::upload;
    failed.phase_result = app::empty_phase(Status::error);
    failed.phase_result->error = app::ErrorResult{service::ErrorCode::unavailable, Phase::upload,
                                                  "сервис временно недоступен", true};
    dashboard.apply_event(failed);
    EXPECT_EQ(dashboard.state().notices,
              (std::vector<std::string>{"Отдача: сервис временно недоступен"}));
    EXPECT_EQ(dashboard.state().notice_tone, Tone::danger);

    app::MeasurementResult measurement =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::all);
    measurement.status = Status::partial;
    measurement.server = app::ServerResult{"mock.example", "Владивосток", std::nullopt};
    measurement.phases.ping = app::ping_phase(*ping.ping);
    measurement.phases.download.status = Status::ok;
    measurement.phases.download.mbps = 100;
    measurement.phases.upload.status = Status::error;
    measurement.warnings = {"сетевые сбои отдельных потоков: 1"};
    app::RunEvent service_done = event(app::EventKind::service_completed);
    service_done.measurement = measurement;
    dashboard.apply_event(service_done);
    ASSERT_EQ(dashboard.state().results.size(), 1u);
    EXPECT_EQ(dashboard.state().results[0].title, "Яндекс.Интернетометр · частично");
    EXPECT_EQ(dashboard.state().results[0].subtitle, "mock.example · Владивосток");
    EXPECT_EQ(dashboard.state().results[0].details,
              "Задержка  11,00 мс    Загрузка  100,00 Мбит/с    Отдача  ошибка");
    EXPECT_EQ(dashboard.state().notices.size(), 2u);

    dashboard.begin_stopping();
    EXPECT_EQ(dashboard.start_label(), "Останавливаем…");
    app::Envelope envelope = app::new_envelope(app::Command::measure);
    envelope.status = Status::canceled;
    dashboard.finish_measurement(envelope);
    EXPECT_FALSE(dashboard.busy());
    EXPECT_FALSE(dashboard.state().progress_visible);
    EXPECT_EQ(dashboard.state().status, "Проверка остановлена");
    EXPECT_EQ(dashboard.state().status_tone, Tone::warning);
    EXPECT_EQ(dashboard.start_label(), "Проверить снова");

    dashboard.begin_measurement();
    EXPECT_TRUE(dashboard.state().results.empty());
    EXPECT_TRUE(dashboard.state().notices.empty());
    EXPECT_EQ(dashboard.state().ping, "—");
    envelope.status = Status::ok;
    dashboard.finish_measurement(envelope);
    EXPECT_EQ(dashboard.state().status, "Проверка завершена");
    EXPECT_EQ(dashboard.state().status_tone, Tone::success);
}

TEST(GuiDashboard, ConnectionLifecycle) {
    MemoryStore store;
    Dashboard dashboard(store);
    dashboard.begin_connection();
    EXPECT_TRUE(dashboard.busy());
    EXPECT_EQ(dashboard.state().connection, "Определение IP и интернет-провайдера…");
    app::ConnectionResult failed;
    failed.status = Status::error;
    failed.error = app::ErrorResult{service::ErrorCode::unavailable, Phase::connection,
                                    "сервис временно недоступен", true};
    app::RunEvent completed = event(app::EventKind::connection_completed);
    completed.connection = failed;
    dashboard.apply_event(completed);
    EXPECT_EQ(dashboard.state().connection, "Не удалось определить IP: сервис временно недоступен");
    dashboard.finish_connection(failed);
    EXPECT_EQ(dashboard.state().status, "Не удалось определить подключение");
    EXPECT_EQ(dashboard.state().status_tone, Tone::danger);
    EXPECT_FALSE(dashboard.busy());
}

} // namespace
} // namespace puls::gui
