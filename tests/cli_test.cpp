#include "puls/application/runner.hpp"
#include "puls/cli/application.hpp"
#include "puls/cli/config.hpp"
#include "puls/cli/gui_launcher.hpp"
#include "puls/cli/render.hpp"
#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/ui/select.hpp"
#include "puls/ui/terminal.hpp"

#include "support/fake_backend.hpp"
#include "support/temporary_directory.hpp"

#include <boost/json/serialize.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;
#endif

namespace puls {
namespace {

using namespace std::chrono_literals;
using service::ServiceId;
using service::Status;

using testing::FakeBackend;

struct Fixture {
    Fixture() {
        yandex->connection.external_ip = *IpAddress::parse("203.0.113.8");
        speedtest->connection.external_ip = *IpAddress::parse("203.0.113.7");
        speedtest->connection.isp = "Ростелеком";
    }

    cli::Application application(ui::Output& output, ui::Output& errors) {
        cli::Application app(output, errors, "test");
        app.yandex_factory = [this](const std::string&, const service::LogFunc&) {
            return std::shared_ptr<service::Backend>(yandex);
        };
        app.speedtest_factory = [this](const std::string&, const service::LogFunc&) {
            return std::shared_ptr<service::Backend>(speedtest);
        };
        app.select_service = [](const ui::Style&) -> Result<ServiceId> {
            return ServiceId::yandex;
        };
        return app;
    }

    std::shared_ptr<FakeBackend> yandex = std::make_shared<FakeBackend>(ServiceId::yandex);
    std::shared_ptr<FakeBackend> speedtest = std::make_shared<FakeBackend>(ServiceId::speedtest);
};

class FailingOutput final : public ui::Output {
public:
    bool write(std::string_view) override { return false; }
};

boost::json::value parse_output(const ui::StringOutput& output) {
    auto value = json::parse(output.text());
    EXPECT_TRUE(value) << output.text();
    return value ? *value : boost::json::value();
}

std::string at(const boost::json::value& value, std::initializer_list<const char*> path) {
    const boost::json::value* current = &value;
    for (const char* key : path) {
        current = &current->as_object().at(key);
    }
    if (current->is_string()) {
        return std::string(current->as_string());
    }
    return current->is_null() ? "null" : boost::json::serialize(*current);
}

// ---- command line --------------------------------------------------------

TEST(Config, ParsesMeasurementCommands) {
    struct Case {
        std::vector<std::string> args;
        std::optional<ServiceId> service;
        std::chrono::seconds duration;
        int connections;
        app::PhaseSelection only;
    };
    const std::vector<Case> cases = {
        {{}, std::nullopt, 10s, 0, app::PhaseSelection::all},
        {{"yandex"}, ServiceId::yandex, 10s, 0, app::PhaseSelection::all},
        {{"speedtest", "--profile", "quick", "--connections", "4"},
         ServiceId::speedtest,
         5s,
         4,
         app::PhaseSelection::all},
        {{"all", "--duration", "12", "--only", "ping"},
         ServiceId::all,
         12s,
         0,
         app::PhaseSelection::ping},
        {{"yandex", "-duration=7", "-connections=0x10"},
         ServiceId::yandex,
         7s,
         16,
         app::PhaseSelection::all},
        {{"speedtest", "--profile=ACCURATE", "--only", " Upload "},
         ServiceId::speedtest,
         15s,
         0,
         app::PhaseSelection::upload},
    };
    for (const auto& test : cases) {
        const auto config = cli::parse_config(test.args);
        ASSERT_TRUE(config) << config.error().message();
        EXPECT_EQ(config->service, test.service);
        EXPECT_EQ(config->duration, test.duration);
        EXPECT_EQ(config->connections, test.connections);
        EXPECT_EQ(config->only, test.only);
    }
}

TEST(Config, ParsesIpCommands) {
    const auto automatic = cli::parse_config({"ip"});
    ASSERT_TRUE(automatic);
    EXPECT_EQ(automatic->command, cli::Command::ip);
    EXPECT_FALSE(automatic->service.has_value());
    EXPECT_FALSE(automatic->service_explicit);
    const auto explicit_service = cli::parse_config({"ip", "yandex", "--json"});
    ASSERT_TRUE(explicit_service);
    EXPECT_EQ(explicit_service->service, ServiceId::yandex);
    EXPECT_TRUE(explicit_service->service_explicit);
    EXPECT_TRUE(explicit_service->json);
}

TEST(Config, ParsesHelpVersionAndGui) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"help"}, {"--help"}, {"-h"}, {"yandex", "--help"}, {"ip", "-h"}}) {
        const auto config = cli::parse_config(args);
        ASSERT_TRUE(config);
        EXPECT_EQ(config->command, cli::Command::help);
    }
    for (const auto& args : std::vector<std::vector<std::string>>{{"version"}, {"--version"}}) {
        const auto config = cli::parse_config(args);
        ASSERT_TRUE(config);
        EXPECT_EQ(config->command, cli::Command::version);
    }
    const auto gui = cli::parse_config({"gui", "--verbose"});
    ASSERT_TRUE(gui);
    EXPECT_EQ(gui->command, cli::Command::gui);
    EXPECT_TRUE(gui->verbose);
}

TEST(Config, RejectsRemovedAndInvalidFlags) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--ip"},
             {"ip", "all"},
             {"--duration", "2"},
             {"--duration", "61"},
             {"--connections", "17"},
             {"--profile", "slow"},
             {"--only", "latency"},
             {"yandex", "--server", "example.com"},
             {"all", "--server", "example.com"},
             {"unknown"},
             {"help", "extra"},
             {"yandex", "--json=maybe"},
             {"yandex", "--duration"},
             {"yandex", "extra"},
         }) {
        EXPECT_FALSE(cli::parse_config(args)) << text::join(args, " ");
    }
    EXPECT_EQ(cli::parse_config({"--bogus"}).error().message(), "неизвестный параметр: --bogus");
    EXPECT_EQ(cli::parse_config({"--connections", "abc"}).error().message(),
              "неверное значение \"abc\" для параметра --connections: parse error");
}

// ---- runner --------------------------------------------------------------

TEST(Runner, RetryStopsOnPermanentErrorsAndCancellation) {
    int calls = 0;
    EXPECT_FALSE(app::with_retry(Context(), 2, [&calls]() -> Error {
        return ++calls == 1 ? Error::make("temporary") : Error();
    }));
    EXPECT_EQ(calls, 2);

    calls = 0;
    const Error permanent =
        service::new_error(ServiceId::yandex, service::Phase::ping, service::ErrorCode::protocol,
                           false, Error::make("broken"));
    EXPECT_TRUE(app::with_retry(Context(), 2, [&]() {
                    ++calls;
                    return permanent;
                }).is(permanent));
    EXPECT_EQ(calls, 1);

    calls = 0;
    CancelScope scope{Context()};
    EXPECT_TRUE(app::with_retry(scope.context(), 2, [&]() {
                    ++calls;
                    scope.cancel();
                    return Error::make("temporary");
                }).is(errors::canceled_tag));
    EXPECT_EQ(calls, 1);
}

TEST(Runner, EmitsEventsInPhaseOrder) {
    Fixture fixture;
    app::RunnerOptions options;
    options.yandex_factory = [&fixture](const std::string&, const service::LogFunc&) {
        return std::shared_ptr<service::Backend>(fixture.yandex);
    };
    const app::Runner runner(options);
    std::vector<app::EventKind> kinds;
    app::MeasureRequest request;
    request.service = ServiceId::yandex;
    const auto envelope = runner.measure(
        Context(), request, [&kinds](const app::RunEvent& event) { kinds.push_back(event.kind); });
    EXPECT_EQ(envelope.status, Status::ok);
    using app::EventKind;
    EXPECT_EQ(
        kinds,
        (std::vector<EventKind>{
            EventKind::run_started, EventKind::service_started, EventKind::phase_started,
            EventKind::server_selected, EventKind::phase_completed, EventKind::phase_started,
            EventKind::ping_completed, EventKind::phase_completed, EventKind::phase_started,
            EventKind::throughput_progress, EventKind::phase_completed, EventKind::phase_started,
            EventKind::phase_completed, EventKind::service_completed, EventKind::run_completed}));
}

TEST(Runner, RetriesRetryableSelectionOnce) {
    Fixture fixture;
    fixture.yandex->select_error =
        service::new_error(ServiceId::yandex, service::Phase::select,
                           service::ErrorCode::unavailable, true, Error::make("offline"));
    app::RunnerOptions options;
    options.yandex_factory = [&fixture](const std::string&, const service::LogFunc&) {
        return std::shared_ptr<service::Backend>(fixture.yandex);
    };
    const auto envelope = app::Runner(options).measure(Context(), app::MeasureRequest{}, nullptr);
    EXPECT_EQ(fixture.yandex->select_calls.load(), 2);
    ASSERT_EQ(envelope.results.size(), 1u);
    EXPECT_EQ(envelope.results[0].status, Status::error);
    EXPECT_EQ(envelope.results[0].phases.ping.status, Status::skipped);
}

TEST(Runner, CanceledRunIsCanceled) {
    Fixture fixture;
    app::RunnerOptions options;
    options.yandex_factory = [&fixture](const std::string&, const service::LogFunc&) {
        return std::shared_ptr<service::Backend>(fixture.yandex);
    };
    CancelScope scope{Context()};
    scope.cancel();
    const auto envelope =
        app::Runner(options).measure(scope.context(), app::MeasureRequest{}, nullptr);
    EXPECT_EQ(envelope.status, Status::canceled);
}

TEST(Runner, ValidatesRequests) {
    app::MeasureRequest request;
    EXPECT_FALSE(app::validate_measure_request(request));
    request.duration = 2s;
    EXPECT_TRUE(app::validate_measure_request(request));
    request.duration = {};
    request.connections = 17;
    EXPECT_TRUE(app::validate_measure_request(request));
    request.connections = 0;
    request.server = "qms.example";
    EXPECT_TRUE(app::validate_measure_request(request));
    request.service = ServiceId::speedtest;
    EXPECT_FALSE(app::validate_measure_request(request));
}

// ---- results -------------------------------------------------------------

TEST(Results, EnvelopeKeepsNullsAndEmptyArrays) {
    app::Envelope envelope = app::new_envelope(app::Command::measure);
    app::MeasurementResult measurement =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::ping);
    measurement.status = Status::error;
    envelope.status = Status::error;
    envelope.results.push_back(measurement);
    const auto text = app::encode_json(envelope);
    ASSERT_TRUE(text);
    for (const char* expected :
         {R"("schema_version": 1)", R"("command": "measure")", R"("connection": null)",
          R"("server": null)", R"("value_ms": null)", R"("mbps": null)", R"("warnings": [])",
          R"("error": null)"}) {
        EXPECT_TRUE(text::contains(*text, expected)) << expected;
    }
    EXPECT_FALSE(text::contains(*text, "provider"));
}

TEST(Results, ThroughputFailureNeverReportsSuccessfulZero) {
    const Error failure = Error::make("offline");
    service::ThroughputResult partial;
    partial.bytes = 1024;
    const app::PhaseResult phase = app::throughput_phase(partial, failure);
    EXPECT_EQ(phase.status, Status::error);
    EXPECT_FALSE(phase.mbps.has_value());
    EXPECT_EQ(phase.bytes, 1024);

    const app::PhaseResult empty = app::throughput_phase({}, failure);
    EXPECT_FALSE(empty.mbps || empty.bytes || empty.elapsed_ms || empty.successful_streams ||
                 empty.failed_streams);
}

TEST(Results, HumanErrorKeepsProtocolDetailsOut) {
    const Error error = service::new_error(
        ServiceId::speedtest, service::Phase::upload, service::ErrorCode::unavailable, true,
        Error::make(R"(Post "https://example.test/upload.php": timeout)"));
    EXPECT_EQ(app::human_error(error), "сервис временно недоступен");
    EXPECT_EQ(app::human_error(errors::deadline_exceeded()), "истекло время ожидания");
    EXPECT_EQ(app::human_error(errors::canceled()), "операция отменена");
    EXPECT_EQ(app::round2(87.5678), 87.57);
}

TEST(Results, StatusAggregation) {
    app::MeasurementResult result =
        app::new_measurement_result(ServiceId::yandex, app::PhaseSelection::all);
    result.phases.select.status = Status::ok;
    result.phases.ping.status = Status::ok;
    result.phases.download.status = Status::error;
    result.phases.upload.status = Status::ok;
    EXPECT_EQ(app::measurement_status(result), Status::partial);
    result.phases.upload.status = Status::canceled;
    EXPECT_EQ(app::measurement_status(result), Status::canceled);
    app::MeasurementResult ok;
    ok.status = Status::ok;
    app::MeasurementResult failed;
    failed.status = Status::error;
    EXPECT_EQ(app::aggregate_status({ok, failed}), Status::partial);
    EXPECT_EQ(app::aggregate_status({failed}), Status::error);
    EXPECT_EQ(app::aggregate_status({}), Status::error);
}

// ---- application ---------------------------------------------------------

TEST(Application, DefaultsToYandexWithoutTty) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"--only", "ping", "--json"}), 0) << errors.text();
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"status"}), "ok");
    ASSERT_EQ(result.at("results").as_array().size(), 1u);
    EXPECT_EQ(at(result.at("results").as_array()[0], {"service"}), "yandex");
}

TEST(Application, UsesInteractiveSelectionInTty) {
    Fixture fixture;
    ui::StringOutput output(true);
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    app.input_terminal = true;
    app.select_service = [](const ui::Style&) -> Result<ServiceId> { return ServiceId::speedtest; };
    EXPECT_EQ(app.run(Context(), {"--only", "ping", "--no-color"}), 0);
    EXPECT_TRUE(text::contains(output.text(), "Puls · speedtest.ru")) << output.text();
    EXPECT_FALSE(text::contains(output.text(), "Яндекс.Интернетометр"));

    app.select_service = [](const ui::Style&) -> Result<ServiceId> {
        return Error::tagged(ui::selection_canceled_tag, "selection canceled");
    };
    EXPECT_EQ(app.run(Context(), {"--only", "ping"}), 130);
}

TEST(Application, AllContinuesAfterServiceFailure) {
    Fixture fixture;
    fixture.yandex->select_error =
        service::new_error(ServiceId::yandex, service::Phase::select,
                           service::ErrorCode::unavailable, false, Error::make("недоступен"));
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"all", "--only", "ping", "--json"}), 1);
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"status"}), "partial");
    const auto& results = result.at("results").as_array();
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(at(results[1], {"status"}), "ok");
}

TEST(Application, IpUsesSpeedtestThenYandexFallback) {
    Fixture fixture;
    fixture.speedtest->connection_error = Error::make("speedtest недоступен");
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"ip", "--json", "--verbose"}), 0) << errors.text();
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"connection", "detected_by"}), "yandex");
    EXPECT_FALSE(result.at("connection").at("warnings").as_array().empty());
    EXPECT_TRUE(text::contains(errors.text(), "fallback"));
}

TEST(Application, ExplicitIpServiceDoesNotFallback) {
    Fixture fixture;
    fixture.speedtest->connection_error = Error::make("speedtest недоступен");
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"ip", "speedtest", "--json"}), 1);
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"connection", "detected_by"}), "null");
    EXPECT_EQ(at(result, {"connection", "status"}), "error");
    EXPECT_EQ(fixture.yandex->connection_calls.load(), 0);
}

TEST(Application, ShowIpFailureDoesNotChangeMeasurementStatus) {
    Fixture fixture;
    fixture.yandex->connection_error = Error::make("IP недоступен");
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"yandex", "--only", "ping", "--show-ip", "--json"}), 0);
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"status"}), "ok");
    EXPECT_EQ(at(result, {"connection", "status"}), "error");
}

TEST(Application, AllShowIpDetectsConnectionOnce) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"all", "--only", "ping", "--show-ip", "--json"}), 0);
    EXPECT_EQ(fixture.speedtest->connection_calls.load(), 1);
    EXPECT_EQ(fixture.yandex->connection_calls.load(), 0);
    const auto result = parse_output(output);
    EXPECT_EQ(at(result, {"connection", "isp"}), "Ростелеком");
    EXPECT_EQ(result.at("results").as_array().size(), 2u);
}

TEST(Application, UsageAndCancellationExitCodes) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"--duration", "1"}), 2);
    EXPECT_EQ(errors.text(), "Ошибка: длительность должна быть от 3 до 60 секунд\n"
                             "Подсказка: puls help\n");
    CancelScope scope{Context()};
    scope.cancel();
    EXPECT_EQ(app.run(scope.context(), {"yandex", "--json"}), 130);
}

TEST(Application, LaunchesGuiAndReportsUnavailableGui) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    bool called = false;
    app.launch_gui = [&called](const Context&, const cli::GuiOptions& options) {
        called = options.version == "test";
        return Error();
    };
    EXPECT_EQ(app.run(Context(), {"gui"}), 0);
    EXPECT_TRUE(called);
    app.launch_gui = [](const Context&, const cli::GuiOptions&) {
        return Error::tagged(cli::gui_unavailable_tag,
                             "графический интерфейс недоступен в этой сборке");
    };
    EXPECT_EQ(app.run(Context(), {"gui"}), 2);
    app.launch_gui = [](const Context&, const cli::GuiOptions&) { return Error::make("broken"); };
    EXPECT_EQ(app.run(Context(), {"gui"}), 1);
}

// Runs program with ASCII arguments and returns its exit code, or -1.
int run_program(const std::filesystem::path& program, const std::vector<std::string>& arguments) {
#if defined(_WIN32)
    std::wstring command_line = L"\"" + program.wstring() + L"\"";
    for (const std::string& argument : arguments) {
        command_line += L" " + std::wstring(argument.begin(), argument.end());
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(program.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &startup, &process) == 0) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    std::vector<std::string> storage{program.string()};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);
    pid_t child = 0;
    if (posix_spawn(&child, storage.front().c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
        return -1;
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

void set_environment(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

using testing::TemporaryDirectory;

TEST(GuiLauncher, LooksForTheGuiNextToPulsFirst) {
    namespace fs = std::filesystem;
    const fs::path directory = fs::path("opt") / "puls";
    const auto candidates = cli::gui_executable_candidates(directory, fs::path("home"));
#if defined(_WIN32)
    EXPECT_EQ(candidates, std::vector<fs::path>{directory / "puls-gui.exe"});
#elif defined(__APPLE__)
    const fs::path bundle = fs::path("Puls.app") / "Contents" / "MacOS" / "Puls";
    EXPECT_EQ(candidates, (std::vector<fs::path>{directory / "puls-gui", directory / bundle,
                                                 fs::path("home") / "Applications" / bundle,
                                                 fs::path("/Applications") / bundle}));
    EXPECT_EQ(cli::gui_executable_candidates(directory, {}).size(), 3U);
#else
    EXPECT_EQ(candidates, std::vector<fs::path>{directory / "puls-gui"});
#endif
}

TEST(GuiLauncher, PulsStartsTheInstalledGui) {
    namespace fs = std::filesystem;
    const TemporaryDirectory directory;
    const fs::path cli = directory.path() / fs::path(PULS_CLI_PATH).filename();
    fs::copy_file(PULS_CLI_PATH, cli);
    const fs::path output = directory.path() / "arguments.txt";
    set_environment("PULS_FAKE_GUI_OUTPUT", output.string());
#if defined(__APPLE__)
    const bool installed_elsewhere = fs::exists("/Applications/Puls.app");
#else
    const bool installed_elsewhere = false;
#endif
    if (!installed_elsewhere) {
        EXPECT_EQ(run_program(cli, {"gui"}), 2);
    }

#if defined(_WIN32)
    fs::copy_file(PULS_FAKE_GUI_PATH, directory.path() / "puls-gui.exe");
#else
    fs::copy_file(PULS_FAKE_GUI_PATH, directory.path() / "puls-gui");
#endif
    EXPECT_EQ(run_program(cli, {"gui", "--verbose"}), 0);
    // Windows does not wait for graphical programs.
    for (int attempt = 0; attempt < 100 && !fs::exists(output); ++attempt) {
        std::this_thread::sleep_for(100ms);
    }
    std::ifstream file(output, std::ios::binary);
    const std::string arguments((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    EXPECT_EQ(arguments, "--verbose\n");
}

TEST(Application, JsonWriteFailureReturnsOne) {
    Fixture fixture;
    FailingOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"ip", "speedtest", "--json"}), 1);
    EXPECT_TRUE(text::contains(errors.text(), "ошибка записи JSON"));
}

TEST(Application, GoldenIpJson) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"ip", "speedtest", "--json"}), 0);
    EXPECT_EQ(output.text(), "{\n"
                             "  \"schema_version\": 1,\n"
                             "  \"command\": \"ip\",\n"
                             "  \"status\": \"ok\",\n"
                             "  \"connection\": {\n"
                             "    \"status\": \"ok\",\n"
                             "    \"external_ip\": \"203.0.113.7\",\n"
                             "    \"isp\": \"Ростелеком\",\n"
                             "    \"detected_by\": \"speedtest\",\n"
                             "    \"warnings\": [],\n"
                             "    \"error\": null\n"
                             "  },\n"
                             "  \"results\": []\n"
                             "}\n");
}

TEST(Application, GoldenMeasurementJson) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"yandex", "--only", "download", "--json"}), 0);
    const std::string phases_before_download = "      \"phases\": {\n"
                                               "        \"select\": {\n"
                                               "          \"status\": \"ok\",\n";
    EXPECT_TRUE(text::contains(output.text(), phases_before_download)) << output.text();
    EXPECT_TRUE(text::contains(output.text(), "          \"mbps\": 100,\n"
                                              "          \"bytes\": 12500000,\n"
                                              "          \"elapsed_ms\": 1000,\n"
                                              "          \"successful_streams\": 2,\n"
                                              "          \"failed_streams\": 0,\n"))
        << output.text();
    EXPECT_TRUE(text::contains(output.text(), "      \"server\": {\n"
                                              "        \"name\": \"mock.example\",\n"
                                              "        \"city\": \"Владивосток\",\n"
                                              "        \"region\": null\n"
                                              "      },\n"));
}

TEST(Application, GoldenHumanYandex) {
    Fixture fixture;
    ui::StringOutput output;
    ui::StringOutput errors;
    auto app = fixture.application(output, errors);
    EXPECT_EQ(app.run(Context(), {"yandex"}), 0);
    EXPECT_EQ(output.text(), "Puls · Яндекс.Интернетометр\n"
                             "  Сервер                mock.example · Владивосток\n"
                             "  Задержка              11.0 мс  ·  джиттер 1.5 мс\n"
                             "  Загрузка              100.00 Мбит/с\n"
                             "  Отдача                50.00 Мбит/с\n"
                             "  ✓ готово\n\n");
    EXPECT_FALSE(text::contains(output.text(), "────"));
}

TEST(Application, HelpUsesServiceTerminologyAndEnglishFlags) {
    ui::StringOutput output;
    cli::print_help(output, ui::Style(false));
    const std::string help = output.text();
    for (const char* expected :
         {"СЕРВИСЫ", "сервер измерения", "интернет-провайдер", "--show-ip", "puls ip speedtest"}) {
        EXPECT_TRUE(text::contains(help, expected)) << expected;
    }
    for (const char* forbidden : {"ИСТОЧНИКИ", "--ip", "--режим", "--длительность"}) {
        EXPECT_FALSE(text::contains(help, forbidden)) << forbidden;
    }
}

TEST(Application, TtyProgressIsAdaptiveAndPipeHasOnlyFinalLine) {
    const auto render = [](ui::Output& output, bool live, int width) {
        cli::TerminalObserver observer(output, ui::Style(false), live, width, 10s);
        app::RunEvent started;
        started.kind = app::EventKind::phase_started;
        started.phase = service::Phase::download;
        observer.observe(started);
        app::RunEvent progress;
        progress.kind = app::EventKind::throughput_progress;
        progress.phase = service::Phase::download;
        progress.throughput = service::ThroughputProgress{80, 0, 5s, 1};
        observer.observe(progress);
        app::RunEvent completed;
        completed.kind = app::EventKind::phase_completed;
        completed.phase = service::Phase::download;
        completed.phase_result = app::empty_phase(Status::ok);
        completed.phase_result->mbps = 80;
        observer.observe(completed);
    };
    ui::StringOutput terminal(true);
    render(terminal, true, 10);
    EXPECT_TRUE(text::contains(terminal.text(), "[█████░░░░░]"));
    EXPECT_TRUE(text::contains(terminal.text(), " 50%  80.00 Мбит/с"));
    EXPECT_TRUE(text::contains(terminal.text(), "\r\x1b[K"));
    ui::StringOutput pipe;
    render(pipe, false, 0);
    const std::string piped = pipe.text();
    EXPECT_EQ(std::count(piped.begin(), piped.end(), '\n'), 1) << piped;
    EXPECT_FALSE(text::contains(piped, "["));
}

TEST(Application, FormatsServer) {
    EXPECT_EQ(cli::format_server({"host", "Москва", "Москва"}), "host · Москва, Москва");
    EXPECT_EQ(cli::format_server({"host", "Москва", ""}), "host · Москва");
    EXPECT_EQ(cli::format_server({"host", "", ""}), "host");
}

// ---- terminal ------------------------------------------------------------

TEST(Terminal, BarWidthAndClamping) {
    EXPECT_EQ(ui::bar(10, 0), "[░░░░░░░░░░]");
    EXPECT_EQ(ui::bar(10, 1), "[██████████]");
    EXPECT_EQ(ui::bar(10, 0.5), "[█████░░░░░]");
    EXPECT_EQ(ui::bar(10, -1), "[░░░░░░░░░░]");
    EXPECT_EQ(ui::bar(10, 2), "[██████████]");
}

TEST(Terminal, PadLabelCountsRunes) {
    EXPECT_EQ(text::pad_right("abc", 6), "abc   ");
    EXPECT_EQ(text::pad_right("abcdefgh", 6), "abcdefgh");
    EXPECT_EQ(text::pad_right("Пинг", 10), "Пинг      ");
}

TEST(Terminal, StylePassesThroughWhenDisabled) {
    const ui::Style plain(false);
    EXPECT_EQ(plain.bold("x"), "x");
    EXPECT_EQ(plain.speed(100), "100.00 Мбит/с");
    EXPECT_EQ(plain.latency(12.34, "мс"), "12.3 мс");
    const ui::Style colored(true);
    EXPECT_EQ(colored.bold("x"), "\x1b[1mx\x1b[0m");
    EXPECT_TRUE(text::contains(colored.speed(60), "\x1b[32m"));
    EXPECT_TRUE(text::contains(colored.speed(20), "\x1b[33m"));
    EXPECT_TRUE(text::contains(colored.speed(5), "\x1b[31m"));
    EXPECT_TRUE(text::contains(colored.latency(10, "мс"), "\x1b[32m"));
    EXPECT_TRUE(text::contains(colored.latency(81, "мс"), "\x1b[31m"));
}

TEST(Terminal, LinesRedrawOnlyWhenLive) {
    ui::StringOutput pipe;
    ui::Line quiet(pipe, false);
    quiet.update("intermediate");
    EXPECT_EQ(pipe.text(), "");
    quiet.finish("done");
    EXPECT_EQ(pipe.text(), "done\n");
    ui::StringOutput terminal(true);
    ui::Line live(terminal, true);
    live.update("frame");
    live.finish("result");
    EXPECT_EQ(terminal.text(), "\r\x1b[Kframe\r\x1b[Kresult\n");
}

TEST(Terminal, ProgressWidthAdaptsToColumns) {
    EXPECT_EQ(ui::progress_width(ui::StringOutput(false, 120)), 0);
    EXPECT_EQ(ui::progress_width(ui::StringOutput(true, 120)), 18);
    EXPECT_EQ(ui::progress_width(ui::StringOutput(true, 60)), 10);
    EXPECT_EQ(ui::progress_width(ui::StringOutput(true, 40)), 0);
    EXPECT_EQ(ui::progress_width(ui::StringOutput(true)), 12);
    EXPECT_FALSE(ui::color_enabled(ui::StringOutput(false), false));
    EXPECT_FALSE(ui::color_enabled(ui::StringOutput(true), true));
}

TEST(Terminal, SelectRejectsEmptyOptionsAndNonTerminalInput) {
    ui::StringOutput output;
    EXPECT_FALSE(ui::select(output, ui::Style(false), "title", {}));
    if (!ui::stdin_is_terminal()) {
        EXPECT_FALSE(ui::select(output, ui::Style(false), "title", {{"a", ""}}));
    }
}

} // namespace
} // namespace puls
