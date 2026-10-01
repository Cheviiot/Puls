#include "puls/cli/config.hpp"

#include "puls/core/text.hpp"

#include <functional>
#include <map>

namespace puls::cli {

namespace {

using service::ServiceId;

// A minimal equivalent of Go's flag.FlagSet with ContinueOnError.
class FlagSet {
public:
    void add_bool(std::string name, bool* target) {
        flags_[std::move(name)] = {target, nullptr, nullptr};
    }
    void add_string(std::string name, std::string* target) {
        flags_[std::move(name)] = {nullptr, target, nullptr};
    }
    void add_int(std::string name, std::int64_t* target) {
        flags_[std::move(name)] = {nullptr, nullptr, target};
    }

    // Returns the remaining positional arguments.
    Result<std::vector<std::string>> parse(std::vector<std::string> args) const {
        std::size_t index = 0;
        while (index < args.size()) {
            const std::string& argument = args[index];
            if (argument.size() < 2 || argument[0] != '-') {
                break;
            }
            std::size_t dashes = 1;
            if (argument[1] == '-') {
                dashes = 2;
                if (argument.size() == 2) {
                    ++index;
                    break;
                }
            }
            std::string name = argument.substr(dashes);
            if (name.empty() || name[0] == '-' || name[0] == '=') {
                return Error::make("неверный синтаксис параметра: " + argument);
            }
            ++index;
            bool has_value = false;
            std::string value;
            if (const auto equals = name.find('=', 1); equals != std::string::npos) {
                value = name.substr(equals + 1);
                name = name.substr(0, equals);
                has_value = true;
            }
            const auto flag = flags_.find(name);
            if (flag == flags_.end()) {
                return Error::make("неизвестный параметр: --" + name);
            }
            const Target& target = flag->second;
            if (target.boolean != nullptr) {
                if (!has_value) {
                    *target.boolean = true;
                    continue;
                }
                const auto parsed = parse_bool(value);
                if (!parsed) {
                    return Error::make("неверное значение " + text::quote(value) +
                                       " для параметра --" + name + ": parse error");
                }
                *target.boolean = *parsed;
                continue;
            }
            if (!has_value && index < args.size()) {
                value = args[index++];
                has_value = true;
            }
            if (!has_value) {
                return Error::make("параметр требует значение: --" + name);
            }
            if (target.string != nullptr) {
                *target.string = value;
                continue;
            }
            const auto parsed = text::parse_int(value, 0);
            if (!parsed.value) {
                return Error::make(
                    "неверное значение " + text::quote(value) + " для параметра --" + name + ": " +
                    (parsed.error == text::ParseIntError::range ? "value out of range"
                                                                : "parse error"));
            }
            *target.integer = *parsed.value;
        }
        return std::vector<std::string>(args.begin() + static_cast<std::ptrdiff_t>(index),
                                        args.end());
    }

private:
    struct Target {
        bool* boolean;
        std::string* string;
        std::int64_t* integer;
    };

    // strconv.ParseBool.
    static std::optional<bool> parse_bool(std::string_view value) {
        for (const char* truthy : {"1", "t", "T", "TRUE", "true", "True"}) {
            if (value == truthy) {
                return true;
            }
        }
        for (const char* falsy : {"0", "f", "F", "FALSE", "false", "False"}) {
            if (value == falsy) {
                return false;
            }
        }
        return std::nullopt;
    }

    std::map<std::string, Target> flags_;
};

Error usage(std::string message) {
    return Error::make(std::move(message));
}

struct CommonFlags {
    bool help = false;
    bool short_help = false;
    bool version = false;
};

void add_common(FlagSet& flags, CommonFlags& common) {
    flags.add_bool("help", &common.help);
    flags.add_bool("h", &common.short_help);
    flags.add_bool("version", &common.version);
}

// Applies help/version flags and rejects positional arguments.
Result<bool> finish_common(Config& config, const CommonFlags& common,
                           const std::vector<std::string>& remaining) {
    if (common.help || common.short_help) {
        config.command = Command::help;
        return true;
    }
    if (common.version) {
        config.command = Command::version;
        return true;
    }
    if (!remaining.empty()) {
        return usage("неизвестный аргумент " + text::quote(remaining.front()));
    }
    return false;
}

Result<Config> parse_gui(Config config, const std::vector<std::string>& args) {
    FlagSet flags;
    CommonFlags common;
    flags.add_bool("verbose", &config.verbose);
    add_common(flags, common);
    auto remaining = flags.parse(args);
    if (!remaining) {
        return std::move(remaining).error();
    }
    auto finished = finish_common(config, common, *remaining);
    if (!finished) {
        return std::move(finished).error();
    }
    return config;
}

Result<Config> parse_ip(Config config, const std::vector<std::string>& args) {
    FlagSet flags;
    CommonFlags common;
    flags.add_bool("json", &config.json);
    flags.add_bool("verbose", &config.verbose);
    flags.add_bool("no-color", &config.no_color);
    add_common(flags, common);
    auto remaining = flags.parse(args);
    if (!remaining) {
        return std::move(remaining).error();
    }
    auto finished = finish_common(config, common, *remaining);
    if (!finished) {
        return std::move(finished).error();
    }
    return config;
}

Result<Config> parse_measure(Config config, const std::vector<std::string>& args) {
    FlagSet flags;
    CommonFlags common;
    std::string duration;
    std::int64_t connections = 0;
    std::string only(app::to_string(config.only));
    flags.add_string("duration", &duration);
    flags.add_string("profile", &config.profile);
    flags.add_int("connections", &connections);
    flags.add_string("only", &only);
    flags.add_string("server", &config.server);
    flags.add_bool("show-ip", &config.show_ip);
    flags.add_bool("json", &config.json);
    flags.add_bool("verbose", &config.verbose);
    flags.add_bool("no-color", &config.no_color);
    add_common(flags, common);
    auto remaining = flags.parse(args);
    if (!remaining) {
        return std::move(remaining).error();
    }
    auto finished = finish_common(config, common, *remaining);
    if (!finished) {
        return std::move(finished).error();
    }
    if (*finished) {
        return config;
    }

    const std::string profile = text::to_lower_ascii(text::trim_space(config.profile));
    const auto parsed_profile = app::parse_profile(profile);
    if (!parsed_profile) {
        return usage("неизвестный профиль " + text::quote(config.profile) +
                     "; выберите quick, balanced или accurate");
    }
    config.profile = profile;
    std::int64_t seconds = app::profile_duration(*parsed_profile).count();
    if (!duration.empty()) {
        const auto value = text::atoi(duration);
        if (!value.value) {
            return usage("неверная длительность " + text::quote(duration) +
                         ": требуется целое число секунд");
        }
        seconds = *value.value;
    }
    if (seconds < 3 || seconds > 60) {
        return usage("длительность должна быть от 3 до 60 секунд");
    }
    config.duration = std::chrono::seconds(seconds);
    if (connections < 0 || connections > 16) {
        return usage("число соединений должно быть 0 (авто) или от 1 до 16");
    }
    config.connections = static_cast<int>(connections);
    const auto selection = app::parse_phase_selection(text::to_lower_ascii(text::trim_space(only)));
    if (!selection) {
        return usage("неизвестный этап " + text::quote(only) +
                     "; выберите all, ping, download или upload");
    }
    config.only = *selection;
    if (!config.server.empty() && config.service != ServiceId::speedtest) {
        return usage("--server можно использовать только с сервисом speedtest");
    }
    return config;
}

} // namespace

Result<Config> parse_config(const std::vector<std::string>& args) {
    Config config;
    if (args.empty()) {
        config.duration = std::chrono::seconds(10);
        return config;
    }
    std::vector<std::string> remaining = args;
    if (args.front().empty() || args.front()[0] != '-') {
        const std::string command = text::to_lower_ascii(text::trim_space(args.front()));
        if (command == "help") {
            if (args.size() != 1) {
                return usage("команда help не принимает аргументы");
            }
            config.command = Command::help;
            return config;
        }
        if (command == "version") {
            if (args.size() != 1) {
                return usage("команда version не принимает аргументы");
            }
            config.command = Command::version;
            return config;
        }
        if (command == "ip") {
            config.command = Command::ip;
            remaining.erase(remaining.begin());
            if (!remaining.empty() && (remaining.front().empty() || remaining.front()[0] != '-')) {
                const auto id = service::parse_service_id(remaining.front());
                if (!id || *id == ServiceId::all) {
                    return usage("неизвестный сервис " + text::quote(remaining.front()) +
                                 "; выберите speedtest или yandex");
                }
                config.service = id;
                config.service_explicit = true;
                remaining.erase(remaining.begin());
            }
            return parse_ip(std::move(config), remaining);
        }
        if (command == "gui") {
            config.command = Command::gui;
            remaining.erase(remaining.begin());
            return parse_gui(std::move(config), remaining);
        }
        const auto id = service::parse_service_id(args.front());
        if (!id) {
            return usage("неизвестная команда " + text::quote(args.front()) +
                         "; выберите yandex, speedtest, all или ip");
        }
        config.service = id;
        config.service_explicit = true;
        remaining.erase(remaining.begin());
    }
    return parse_measure(std::move(config), remaining);
}

} // namespace puls::cli
