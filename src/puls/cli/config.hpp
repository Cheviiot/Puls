#pragma once

#include "puls/application/types.hpp"
#include "puls/core/error.hpp"
#include "puls/service/service.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace puls::cli {

enum class Command : std::uint8_t { measure, ip, gui, help, version };

struct Config {
    Command command = Command::measure;
    // Empty when the user did not name a service.
    std::optional<service::ServiceId> service;
    bool service_explicit = false;
    std::string profile = "balanced";
    std::chrono::seconds duration{};
    int connections = 0;
    app::PhaseSelection only = app::PhaseSelection::all;
    std::string server;
    bool show_ip = false;
    bool json = false;
    bool verbose = false;
    bool no_color = false;
};

// Parses the public command line. Flags follow Go's flag package: -name and
// --name are equivalent, values may follow "=" or the next argument, and
// parsing stops at the first positional argument. Every error is a usage
// error with a Russian message.
Result<Config> parse_config(const std::vector<std::string>& args);

} // namespace puls::cli
