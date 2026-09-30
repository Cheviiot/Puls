#include "puls/cli/render.hpp"

#include "puls/core/text.hpp"

#include <algorithm>

namespace puls::cli {

namespace {

using service::Phase;
using service::Status;

constexpr std::size_t label_width = 22;
constexpr std::string_view row_indent = "  ";

std::string help_row(std::string_view name, std::string_view description) {
    return "  " + text::pad_right(name, 28) + " " + std::string(description) + "\n";
}

std::string help_example(std::string_view example, std::string_view description) {
    return "  " + text::pad_right(example, 34) + " " + std::string(description) + "\n";
}

std::string metric_label(const ui::Style& style, std::string_view label) {
    return std::string(row_indent) + style.dim(text::pad_right(label, label_width));
}

std::string value_or_dash(const std::optional<std::string>& value) {
    return value && !value->empty() ? *value : "—";
}

std::string phase_label(Phase phase) {
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
        return "Подключение";
    }
    return std::string(service::to_string(phase));
}

std::string phase_activity(Phase phase) {
    switch (phase) {
    case Phase::select:
        return "выбор сервера…";
    case Phase::ping:
        return "измерение…";
    case Phase::download:
    case Phase::upload:
        return "подготовка…";
    case Phase::connection:
        break;
    }
    return "выполнение…";
}

void render_service_header(ui::Output& output, const ui::Style& style, service::ServiceId id) {
    const std::string name =
        id == service::ServiceId::yandex ? "Яндекс.Интернетометр" : service::display_name(id);
    output.write(style.cyan(style.bold("Puls ·")) + " " + style.bold(name) + "\n");
}

void render_connection(ui::Output& output, const ui::Style& style,
                       const app::ConnectionResult& result) {
    std::string text = style.cyan(style.bold("Puls ·")) + " " + style.bold("Подключение") + "\n";
    if (result.status == Status::ok) {
        text += metric_label(style, "Внешний IP") + value_or_dash(result.external_ip) + "\n";
        text += metric_label(style, "Интернет-провайдер") + value_or_dash(result.isp) + "\n";
        if (result.detected_by) {
            text +=
                metric_label(style, "Сервис") + service::display_name(*result.detected_by) + "\n";
        }
    } else if (result.error) {
        text += metric_label(style, "Результат") + style.red("не удалось") +
                style.dim(" · " + result.error->message) + "\n";
    }
    text += "\n";
    output.write(text);
}

std::string render_ping_metric(const ui::Style& style, const service::PingResult& ping) {
    return metric_label(style, "Задержка") + style.latency(ping.value_ms, "мс") +
           style.dim("  ·  джиттер ") + style.latency(ping.jitter_ms, "мс");
}

void render_measurement_footer(ui::Output& output, const ui::Style& style,
                               const app::MeasurementResult& result) {
    std::string text;
    for (const auto& warning : result.warnings) {
        text += std::string(row_indent) + style.yellow("!") + " " + style.dim(warning) + "\n";
    }
    std::string icon = "✓";
    std::string label = "готово";
    std::string painted;
    switch (result.status) {
    case Status::partial:
        painted = style.yellow("!");
        label = "частичный результат";
        break;
    case Status::error:
        painted = style.red("×");
        label = "измерение не выполнено";
        break;
    case Status::canceled:
        painted = style.yellow("•");
        label = "остановлено";
        break;
    default:
        painted = style.green(icon);
        break;
    }
    text += std::string(row_indent) + painted + " " + style.dim(label) + "\n\n";
    output.write(text);
}

} // namespace

void print_help(ui::Output& output, const ui::Style& style) {
    std::string text;
    text += style.cyan(style.bold("Puls")) + "  " +
            style.dim("проверка скорости и качества интернета") + "\n";
    text += style.dim("Яндекс.Интернетометр и speedtest.ru") + "\n";

    text += "\nИСПОЛЬЗОВАНИЕ\n";
    text += "  puls [service] [options]\n";
    text += "  puls ip [service] [options]\n";
    text += "  puls gui [options]\n";

    text += "\nСЕРВИСЫ\n";
    text += help_row("yandex", "Яндекс.Интернетометр");
    text += help_row("speedtest", "speedtest.ru");
    text += help_row("all", "оба сервиса последовательно");

    text += "\nКОМАНДЫ\n";
    text += help_row("ip [service]", "внешний IP и интернет-провайдер");
    text += help_row("gui", "открыть графический интерфейс");
    text += help_row("help", "показать справку");
    text += help_row("version", "показать версию");

    text += "\nПАРАМЕТРЫ ИЗМЕРЕНИЯ\n";
    text += help_row("--profile <name>", "quick, balanced или accurate");
    text += help_row("--duration <seconds>", "время фазы · от 3 до 60 секунд");
    text += help_row("--connections <number>", "0 — автоматически, вручную — от 1 до 16");
    text += help_row("--only <phase>", "all, ping, download или upload");
    text += help_row("--server <host>", "сервер измерения · только speedtest");
    text += help_row("--show-ip", "добавить данные подключения");

    text += "\nОБЩИЕ ПАРАМЕТРЫ\n";
    text += help_row("--json", "машинный JSON без оформления");
    text += help_row("--verbose", "адреса протокола, резервные пути и переподключения");
    text += help_row("--no-color", "отключить цвета");
    text += help_row("-h, --help", "показать справку");
    text += help_row("--version", "показать версию");

    text += "\nПРИМЕРЫ\n";
    text += help_example("puls", "выбрать сервис");
    text += help_example("puls gui", "открыть графический интерфейс");
    text += help_example("puls yandex", "полная проверка через Яндекс");
    text += help_example("puls all --profile quick", "быстро проверить оба сервиса");
    text += help_example("puls speedtest --only ping", "измерить только задержку");
    text += help_example("puls ip", "определить IP с резервным сервисом");
    text += help_example("puls ip speedtest", "IP и интернет-провайдер через speedtest.ru");
    text += help_example("puls ip yandex --json", "использовать конкретный сервис");

    text += style.dim("\nCtrl+C останавливает операцию · подробнее: README.md") + "\n";
    output.write(text);
}

std::string format_server(const service::Server& server) {
    if (!server.city.empty() && !server.region.empty()) {
        return server.name + " · " + server.city + ", " + server.region;
    }
    if (!server.city.empty()) {
        return server.name + " · " + server.city;
    }
    return server.name;
}

void render_summary(ui::Output& output, const ui::Style& style,
                    const std::vector<app::MeasurementResult>& results) {
    int ok = 0;
    int partial = 0;
    int failed = 0;
    for (const auto& result : results) {
        if (result.status == Status::ok) {
            ++ok;
        } else if (result.status == Status::partial) {
            ++partial;
        } else {
            ++failed;
        }
    }
    std::vector<std::string> parts;
    if (ok > 0) {
        parts.push_back(style.green(std::to_string(ok) + " успешно"));
    }
    if (partial > 0) {
        parts.push_back(style.yellow(std::to_string(partial) + " частично"));
    }
    if (failed > 0) {
        parts.push_back(style.red(std::to_string(failed) + " с ошибкой"));
    }
    output.write(style.bold("Итог") + "  " + text::join(parts, style.dim(" · ")) + "\n");
}

TerminalObserver::TerminalObserver(ui::Output& output, ui::Style style, bool live,
                                   int progress_width, std::chrono::nanoseconds duration)
    : output_(output), style_(style), live_(live), progress_width_(progress_width),
      duration_(duration) {}

void TerminalObserver::observe(const app::RunEvent& event) {
    switch (event.kind) {
    case app::EventKind::connection_started: {
        auto& connection = lines_[Phase::connection];
        connection = std::make_unique<ui::Line>(output_, live_);
        connection->update("определение подключения…");
        break;
    }
    case app::EventKind::connection_completed:
        if (const auto found = lines_.find(Phase::connection);
            found != lines_.end() && found->second) {
            found->second->update("");
        }
        if (event.connection) {
            render_connection(output_, style_, *event.connection);
        }
        break;
    case app::EventKind::service_started:
        lines_.clear();
        rendered_.clear();
        if (event.service) {
            render_service_header(output_, style_, *event.service);
        }
        break;
    case app::EventKind::phase_started:
        if (event.phase) {
            auto& started = lines_[*event.phase];
            started = std::make_unique<ui::Line>(output_, live_);
            started->update(metric_label(style_, phase_label(*event.phase)) +
                            style_.dim(phase_activity(*event.phase)));
        }
        break;
    case app::EventKind::server_selected:
        if (event.server && event.phase) {
            finish(*event.phase, metric_label(style_, "Сервер") + format_server(*event.server));
        }
        break;
    case app::EventKind::ping_completed:
        if (event.ping) {
            finish(Phase::ping, render_ping_metric(style_, *event.ping));
        }
        break;
    case app::EventKind::throughput_progress:
        if (event.throughput && event.phase) {
            progress(*event.phase, *event.throughput);
        }
        break;
    case app::EventKind::phase_completed:
        if (event.phase_result && event.phase) {
            complete_phase(*event.phase, *event.phase_result);
        }
        break;
    case app::EventKind::service_completed:
        if (event.measurement) {
            render_measurement_footer(output_, style_, *event.measurement);
        }
        break;
    case app::EventKind::run_started:
    case app::EventKind::run_completed:
        break;
    }
}

void TerminalObserver::progress(Phase phase, const service::ThroughputProgress& value) {
    double fraction = 0;
    if (duration_ > std::chrono::nanoseconds::zero()) {
        fraction = std::clamp(static_cast<double>(value.elapsed.count()) /
                                  static_cast<double>(duration_.count()),
                              0.0, 1.0);
    }
    std::string bar;
    if (progress_width_ > 0) {
        bar = style_.cyan(ui::bar(progress_width_, fraction)) + "  ";
    }
    std::string percent = text::format_fixed(fraction * 100, 0);
    if (percent.size() < 3) {
        percent.insert(0, 3 - percent.size(), ' ');
    }
    line(phase).update(metric_label(style_, phase_label(phase)) + bar + percent + "%  " +
                       style_.speed(value.mbps));
}

void TerminalObserver::complete_phase(Phase phase, const app::PhaseResult& result) {
    if (rendered_[phase]) {
        return;
    }
    const std::string label = phase_label(phase);
    switch (result.status) {
    case Status::ok:
        if (result.mbps) {
            finish(phase, metric_label(style_, label) + style_.speed(*result.mbps));
        }
        break;
    case Status::skipped:
        finish(phase, metric_label(style_, label) + style_.dim("пропущено"));
        break;
    case Status::canceled:
        finish(phase, metric_label(style_, label) + style_.dim("остановлено"));
        break;
    case Status::error: {
        const std::string message = result.error ? " · " + result.error->message : std::string();
        finish(phase, metric_label(style_, label) + style_.red("не удалось") + style_.dim(message));
        break;
    }
    default:
        break;
    }
}

void TerminalObserver::finish(Phase phase, const std::string& text) {
    line(phase).finish(text);
    rendered_[phase] = true;
}

ui::Line& TerminalObserver::line(Phase phase) {
    auto& value = lines_[phase];
    if (!value) {
        value = std::make_unique<ui::Line>(output_, live_);
    }
    return *value;
}

} // namespace puls::cli
