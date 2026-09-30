#include "puls/cli/application.hpp"

#include "puls/cli/render.hpp"
#include "puls/ui/select.hpp"

namespace puls::cli {

const ErrorTag gui_unavailable_tag{"gui.ErrUnavailable"};

namespace {

using service::ServiceId;
using service::Status;

} // namespace

Result<ServiceId> select_measurement_service(ui::Output& output, const ui::Style& style) {
    struct Choice {
        ServiceId id;
        const char* label;
        const char* hint;
    };
    static constexpr Choice choices[] = {
        {ServiceId::yandex, "Яндекс.Интернетометр", "CDN Яндекса"},
        {ServiceId::speedtest, "speedtest.ru", "серверы speedtest.ru"},
        {ServiceId::all, "Все сервисы", "последовательно"},
    };
    std::vector<ui::Option> options;
    for (const Choice& choice : choices) {
        options.push_back(ui::Option{choice.label, choice.hint});
    }
    auto selected = ui::select(output, style, "Puls · сервис измерения", options);
    if (!selected) {
        return selected.error();
    }
    return choices[*selected].id;
}

Application::Application(ui::Output& output, ui::Output& error_output, std::string version)
    : output_(output), error_output_(error_output), version_(std::move(version)) {
    select_service = [&output](const ui::Style& style) {
        return select_measurement_service(output, style);
    };
    launch_gui = [](const Context&, const GuiOptions&) {
        return Error::tagged(gui_unavailable_tag, "графический интерфейс недоступен в этой сборке");
    };
}

int Application::run(const Context& ctx, const std::vector<std::string>& args) {
    auto parsed = parse_config(args);
    if (!parsed) {
        return print_usage_error(parsed.error());
    }
    Config config = std::move(parsed).value();
    const bool output_terminal = output_.is_terminal();
    const ui::Style style(!config.json && output_terminal &&
                          ui::color_enabled(output_, config.no_color));
    switch (config.command) {
    case Command::help:
        print_help(output_, style);
        return 0;
    case Command::version:
        output_.write("Puls " + version_ + "\n");
        return 0;
    default:
        break;
    }

    const service::LogFunc log = logger(config.verbose, config.no_color);
    if (config.command == Command::gui) {
        const Error error = launch_gui(ctx, GuiOptions{version_, log});
        if (!error) {
            return 0;
        }
        error_output_.write("ошибка запуска GUI: " + error.message() + "\n");
        return error.is(gui_unavailable_tag) ? 2 : 1;
    }
    if (config.command == Command::measure && !config.service) {
        if (!config.json && input_terminal && output_terminal) {
            auto selected = select_service(style);
            if (selected) {
                config.service = *selected;
                config.service_explicit = true;
            } else if (selected.error().is(ui::selection_canceled_tag) ||
                       selected.error().is(errors::canceled_tag)) {
                return 130;
            } else if (log) {
                log("интерактивный выбор недоступен: " + selected.error().message());
            }
        }
        if (!config.service) {
            config.service = ServiceId::yandex;
        }
    }
    if (config.command == Command::ip) {
        return run_ip(ctx, config, style, log);
    }
    return run_measurements(ctx, config, style, log);
}

int Application::run_ip(const Context& ctx, const Config& config, const ui::Style& style,
                        const service::LogFunc& log) {
    app::Envelope envelope = app::new_envelope(app::Command::ip);
    const app::Runner measurement_runner = runner(log);
    std::unique_ptr<TerminalObserver> terminal;
    app::Observer observer;
    if (!config.json) {
        terminal = std::make_unique<TerminalObserver>(output_, style, output_.is_terminal(), 0,
                                                      std::chrono::nanoseconds::zero());
        observer = [&terminal](const app::RunEvent& event) { terminal->observe(event); };
    }
    app::ConnectionRequest request;
    request.service = config.service;
    request.explicit_service = config.service_explicit;
    envelope.connection = measurement_runner.detect_connection(ctx, request, observer);
    envelope.status = envelope.connection->status;
    if (config.json && !write_json(envelope)) {
        return 1;
    }
    if (envelope.status == Status::canceled) {
        return 130;
    }
    return envelope.status == Status::ok ? 0 : 1;
}

int Application::run_measurements(const Context& ctx, const Config& config, const ui::Style& style,
                                  const service::LogFunc& log) {
    const app::Runner measurement_runner = runner(log);
    std::unique_ptr<TerminalObserver> terminal;
    app::Observer observer;
    if (!config.json) {
        terminal = std::make_unique<TerminalObserver>(output_, style, output_.is_terminal(),
                                                      ui::progress_width(output_), config.duration);
        observer = [&terminal](const app::RunEvent& event) { terminal->observe(event); };
    }
    app::MeasureRequest request;
    request.service = config.service.value_or(ServiceId::yandex);
    request.profile = app::parse_profile(config.profile).value_or(app::Profile::balanced);
    request.duration = config.duration;
    request.connections = config.connections;
    request.only = config.only;
    request.server = config.server;
    request.show_connection = config.show_ip;
    const app::Envelope envelope = measurement_runner.measure(ctx, request, observer);
    if (!config.json && envelope.results.size() > 1) {
        render_summary(output_, style, envelope.results);
    }
    if (config.json && !write_json(envelope)) {
        return 1;
    }
    if (ctx.cause() == CancelCause::canceled || envelope.status == Status::canceled) {
        error_output_.write("операция прервана\n");
        return 130;
    }
    return envelope.status == Status::ok ? 0 : 1;
}

bool Application::write_json(const app::Envelope& envelope) {
    const auto encoded = app::encode_json(envelope);
    if (!encoded) {
        error_output_.write("ошибка записи JSON: " + encoded.error().message() + "\n");
        return false;
    }
    if (!output_.write(*encoded)) {
        error_output_.write("ошибка записи JSON: не удалось записать в стандартный вывод\n");
        return false;
    }
    return true;
}

int Application::print_usage_error(const Error& error) {
    const ui::Style style(ui::color_enabled(error_output_, false));
    error_output_.write(style.red("Ошибка:") + " " + error.message() + "\n" +
                        style.dim("Подсказка: puls help") + "\n");
    return 2;
}

app::Runner Application::runner(const service::LogFunc& log) const {
    app::RunnerOptions options;
    options.yandex_factory = yandex_factory;
    options.speedtest_factory = speedtest_factory;
    options.log = log;
    return app::Runner(std::move(options));
}

service::LogFunc Application::logger(bool enabled, bool no_color) {
    if (!enabled) {
        return nullptr;
    }
    const ui::Style style(ui::color_enabled(error_output_, no_color));
    return [this, style](std::string_view message) {
        error_output_.write(style.dim("подробно") + "  " + std::string(message) + "\n");
    };
}

} // namespace puls::cli
