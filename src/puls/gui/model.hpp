#pragma once

#include "puls/application/types.hpp"
#include "puls/core/error.hpp"
#include "puls/service/service.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// State and behavior of the graphical dashboard without a toolkit dependency.
// The Qt layer renders this model and forwards user actions and runner events
// to it on the interface thread.
namespace puls::gui {

enum class ThemeMode : std::uint8_t { system, light, dark };

std::string_view to_string(ThemeMode mode) noexcept;
std::optional<ThemeMode> parse_theme_mode(std::string_view text);

// The measurement configuration of the dashboard. The speedtest.ru server and
// the IP lookup flag are kept for the session only.
struct Settings {
    service::ServiceId service = service::ServiceId::yandex;
    app::Profile profile = app::Profile::balanced;
    std::chrono::seconds duration{10};
    int connections = 0;
    app::PhaseSelection only = app::PhaseSelection::all;
    std::string server;
    bool show_ip = false;
    ThemeMode theme = ThemeMode::system;

    [[nodiscard]] app::MeasureRequest request() const;
};

// Values entered in the settings panel; list choices are indexes into the
// label lists below.
struct SettingsInput {
    int service = -1;
    int profile = -1;
    std::string duration;
    int connections = -1;
    int phase = -1;
    std::string server;
    bool show_ip = false;
    int theme = -1;
};

// Full names, for example for screen readers.
const std::vector<std::string>& service_labels();
// Short names of the service selector.
const std::vector<std::string>& service_names();
const std::vector<std::string>& profile_names();
// The phase duration of each profile: "5 с".
const std::vector<std::string>& profile_details();
const std::vector<std::string>& connection_labels();
const std::vector<std::string>& phase_labels();
const std::vector<std::string>& phase_names();
const std::vector<std::string>& theme_labels();

int service_index(service::ServiceId id) noexcept;
int profile_index(app::Profile profile) noexcept;
int connection_index(int connections) noexcept;
int phase_index(app::PhaseSelection only) noexcept;
int theme_index(ThemeMode mode) noexcept;
std::optional<app::Profile> profile_at(int index) noexcept;

// Validates panel input; every error is a Russian message for the user.
Result<Settings> parse_settings(const SettingsInput& input);
// "Сбалансированный · 10 с"; connections and phases only when chosen.
std::string settings_summary(const Settings& settings);
// What a measurement with these settings does.
std::string idle_hint(const Settings& settings);

std::string display_service(service::ServiceId id);
std::string format_server(const service::Server& server);
// Fewer decimals as a value grows, with a decimal comma: "8,75", "94,2", "487".
std::string format_speed(double mbps);
// "1,8", "12".
std::string format_latency(double milliseconds);

// Stores user preferences; the Qt layer implements it with QSettings.
class PreferenceStore {
public:
    virtual ~PreferenceStore() = default;
    [[nodiscard]] virtual std::optional<std::string> text(std::string_view key) const = 0;
    [[nodiscard]] virtual std::optional<double> number(std::string_view key) const = 0;
    virtual void set_text(std::string_view key, std::string_view value) = 0;
    virtual void set_number(std::string_view key, double value) = 0;
};

inline constexpr double default_window_width = 460;
inline constexpr double default_window_height = 800;
inline constexpr double minimum_window_width = 390;
inline constexpr double minimum_window_height = 600;

// Only the interface theme, measurement settings and window size are stored;
// IP addresses, providers, servers, results and logs never are.
struct Preferences {
    ThemeMode theme = ThemeMode::system;
    service::ServiceId service = service::ServiceId::yandex;
    app::Profile profile = app::Profile::balanced;
    std::chrono::seconds duration{10};
    int connections = 0;
    app::PhaseSelection only = app::PhaseSelection::all;
    double window_width = default_window_width;
    double window_height = default_window_height;
};

Preferences load_preferences(const PreferenceStore& store);
void save_measurement(PreferenceStore& store, const Settings& settings);
void save_theme(PreferenceStore& store, ThemeMode mode);
// Ignores sizes below the minimum window size.
void save_window(PreferenceStore& store, double width, double height);

enum class Tone : std::uint8_t { neutral, success, warning, danger };

enum class PhaseState : std::uint8_t { pending, active, done, failed, skipped };

std::string_view to_string(PhaseState state) noexcept;

// One of the latency, download and upload phases of the active service.
struct PhaseView {
    std::string title;
    PhaseState state = PhaseState::pending;
    // From 0 to 1; negative while the phase has no measurable progress.
    double progress = 0;
};

// The metric that the measurement updates right now.
enum class Metric : std::uint8_t { none, ping, download, upload };

std::string_view to_string(Metric metric) noexcept;

// The result of one service when the run measures several.
struct ServiceResult {
    std::string service;
    std::string status;
    Tone tone = Tone::neutral;
    std::string ping;
    std::string download;
    std::string upload;
    std::string server;
};

enum class Activity : std::uint8_t { idle, measurement, connection };

struct DashboardState {
    std::string status = "Готов к проверке";
    Tone status_tone = Tone::neutral;
    // The main number: the live value of the active phase while measuring,
    // then the result. Empty until a value is known.
    std::string hero_value;
    std::string hero_unit = "Мбит/с";
    std::string hero_label;
    std::vector<PhaseView> phases;
    Metric active_metric = Metric::none;
    // "Сервис 2 из 2 · speedtest.ru" while a run measures several services.
    std::string service_progress;
    std::string ping = "—";
    std::string jitter = "—";
    std::string download = "—";
    std::string upload = "—";
    std::string server = "Сервер ещё не выбран";
    bool server_known = false;
    std::string connection = "IP и интернет-провайдер не определены";
    bool connection_known = false;
    std::vector<std::string> notices;
    Tone notice_tone = Tone::warning;
    std::vector<ServiceResult> results;
    Activity activity = Activity::idle;
    bool stopping = false;
    // A measurement has finished in this session.
    bool has_result = false;
};

// The dashboard model. It is not thread-safe: every call happens on the
// interface thread.
class Dashboard {
public:
    explicit Dashboard(PreferenceStore& store);

    [[nodiscard]] const Settings& settings() const noexcept { return settings_; }
    [[nodiscard]] const DashboardState& state() const noexcept { return state_; }
    [[nodiscard]] const Preferences& preferences() const noexcept { return preferences_; }
    [[nodiscard]] bool busy() const noexcept { return state_.activity != Activity::idle; }
    [[nodiscard]] bool measuring() const noexcept {
        return state_.activity == Activity::measurement;
    }
    [[nodiscard]] bool detecting() const noexcept {
        return state_.activity == Activity::connection;
    }
    [[nodiscard]] std::string start_label() const;
    [[nodiscard]] app::ConnectionRequest connection_request() const;

    void select_service(int index);
    void cycle_theme();
    // Applies and stores valid settings; returns the validation error otherwise.
    Error apply_settings(const SettingsInput& input);
    void save_window(double width, double height);

    void begin_measurement();
    void begin_connection();
    void begin_stopping();
    void apply_event(const app::RunEvent& event);
    void finish_measurement(const app::Envelope& envelope);
    void finish_connection(const app::ConnectionResult& result);
    void add_notice(Tone tone, std::string_view message);

private:
    // Clears what the dashboard shows about the measured service.
    void reset_service();
    void apply_theme(ThemeMode mode);
    void start_lookup();
    void start_phase(service::Phase phase);
    void apply_phase_result(service::Phase phase, const app::PhaseResult& result);
    void apply_measurement(const app::MeasurementResult& result);
    void apply_connection(const app::ConnectionResult& result);
    void show_summary(const app::Envelope& envelope);
    // Names the service in notices when the run measures several.
    [[nodiscard]] std::string notice_subject(std::string_view subject) const;
    PhaseView* phase_view(service::Phase phase);
    std::string* metric(service::Phase phase);

    PreferenceStore& store_;
    Preferences preferences_;
    Settings settings_;
    DashboardState state_;
    // The number of services in the active run, the started ones and the
    // service being measured.
    int run_services_ = 1;
    int started_services_ = 0;
    std::optional<service::ServiceId> service_;
    // A connection lookup is running; stopping it restores the previous text.
    bool lookup_active_ = false;
    std::string previous_connection_;
    bool previous_connection_known_ = false;
};

} // namespace puls::gui
