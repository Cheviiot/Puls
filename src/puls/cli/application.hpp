#pragma once

#include "puls/application/runner.hpp"
#include "puls/cli/config.hpp"
#include "puls/core/context.hpp"
#include "puls/ui/terminal.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace puls::cli {

extern const ErrorTag gui_unavailable_tag;

struct GuiOptions {
    std::string version;
    service::LogFunc log;
};

// The command-line application: parsing, configuration, orchestration
// through app::Runner, JSON and rendering. Dependencies are injected so that
// tests can replace terminals and services.
class Application {
public:
    Application(ui::Output& output, ui::Output& error_output, std::string version);

    // Runs the command line and returns the exit code: 0 success, 1 failure,
    // 2 usage error, 130 interrupted.
    int run(const Context& ctx, const std::vector<std::string>& args);

    bool input_terminal = false;
    app::BackendFactory yandex_factory;
    app::BackendFactory speedtest_factory;
    // Interactive choice of the measurement service.
    std::function<Result<service::ServiceId>(const ui::Style&)> select_service;
    std::function<Error(const Context&, const GuiOptions&)> launch_gui;

private:
    int run_ip(const Context& ctx, const Config& config, const ui::Style& style,
               const service::LogFunc& log);
    int run_measurements(const Context& ctx, const Config& config, const ui::Style& style,
                         const service::LogFunc& log);
    int print_usage_error(const Error& error);
    [[nodiscard]] app::Runner runner(const service::LogFunc& log) const;
    [[nodiscard]] service::LogFunc logger(bool enabled, bool no_color);
    bool write_json(const app::Envelope& envelope);

    ui::Output& output_;
    ui::Output& error_output_;
    std::string version_;
};

// Shows the interactive service menu on output.
Result<service::ServiceId> select_measurement_service(ui::Output& output, const ui::Style& style);

} // namespace puls::cli
