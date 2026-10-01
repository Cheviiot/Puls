#pragma once

#include "puls/application/types.hpp"
#include "puls/service/service.hpp"
#include "puls/ui/terminal.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace puls::cli {

void print_help(ui::Output& output, const ui::Style& style);
std::string format_server(const service::Server& server);
void render_summary(ui::Output& output, const ui::Style& style,
                    const std::vector<app::MeasurementResult>& results);

// Renders runner events as compact human output. Live terminals get
// in-place progress; pipes receive only final lines.
class TerminalObserver {
public:
    TerminalObserver(ui::Output& output, ui::Style style, bool live, int progress_width,
                     std::chrono::nanoseconds duration);

    void observe(const app::RunEvent& event);

private:
    void progress(service::Phase phase, const service::ThroughputProgress& value);
    void complete_phase(service::Phase phase, const app::PhaseResult& result);
    void finish(service::Phase phase, const std::string& text);
    ui::Line& line(service::Phase phase);

    ui::Output& output_;
    ui::Style style_;
    bool live_;
    int progress_width_;
    std::chrono::nanoseconds duration_;
    std::map<service::Phase, std::unique_ptr<ui::Line>> lines_;
    std::map<service::Phase, bool> rendered_;
};

} // namespace puls::cli
