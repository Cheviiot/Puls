#include "puls/core/text.hpp"
#include "puls/net/url.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

#include <algorithm>
#include <cmath>

namespace puls::speedtestru {

namespace detail {

namespace {

bool regex_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\f' || c == '\r';
}

bool key_character(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

bool quote_character(char c) {
    return c == '"' || c == '\'';
}

} // namespace

const std::vector<KnownServer>& known_servers() {
    static const std::vector<KnownServer> servers = {
        {"vladivostok.qms.ru:20000", "Владивосток"},
        {"vladivostok10.qms.ru:20000", "Владивосток"},
        {"khabarovsk1.qms.ru:20000", "Хабаровск"},
        {"blagoveshchensk.qms.ru:20000", "Благовещенск"},
        {"yuzhno-sahalinsk.qms.ru:20000", "Южно-Сахалинск"},
        {"chita.qms.ru:20000", "Чита"},
        {"ulan-ude.qms.ru:20000", "Улан-Удэ"},
        {"yakutsk.qms.ru:20000", "Якутск"},
        {"magadan.qms.ru:20000", "Магадан"},
    };
    return servers;
}

Result<std::string> normalize_host(std::string_view raw, std::string_view default_port) {
    const std::string host(text::trim_space(raw));
    if (host.empty()) {
        return Error::make("адрес сервера не может быть пустым");
    }
    if (text::contains_any(host, "/?#@") || text::contains(host, "://")) {
        return Error::make("неверный адрес сервера " + text::quote(host) +
                           ": укажите host или host:port");
    }
    std::string_view literal = host;
    while (!literal.empty() && (literal.front() == '[' || literal.front() == ']')) {
        literal.remove_prefix(1);
    }
    while (!literal.empty() && (literal.back() == '[' || literal.back() == ']')) {
        literal.remove_suffix(1);
    }
    if (const auto address = IpAddress::parse(literal); address && address->zone().empty()) {
        return net::join_host_port(address->unmap().to_string(), default_port);
    }
    std::string hostname;
    std::string port;
    if (auto split = net::split_host_port(host)) {
        hostname = split->first;
        port = split->second;
    } else if (text::contains(host, ":")) {
        return Error::wrap("неверный адрес сервера " + text::quote(host), split.error());
    } else {
        hostname = host;
        port = std::string(default_port);
    }
    if (hostname.empty() || text::contains_any(hostname, " \t\r\n")) {
        return Error::make("неверное имя сервера " + text::quote(hostname));
    }
    const auto number = text::atoi(port);
    if (!number.value || *number.value < 1 || *number.value > 65535) {
        return Error::make("неверный порт сервера " + text::quote(port));
    }
    return net::join_host_port(hostname, std::to_string(*number.value));
}

double qms_jitter(const std::vector<double>& samples, double median) {
    if (samples.size() < 2) {
        return 0;
    }
    std::vector<double> values = samples;
    std::sort(values.begin(), values.end());
    const std::size_t quarter = values.size() / 4;
    std::vector<double> trimmed(values.begin() + static_cast<std::ptrdiff_t>(quarter),
                                values.end() - static_cast<std::ptrdiff_t>(quarter));
    if (trimmed.size() < 2) {
        trimmed = values;
    }
    double total = 0;
    for (std::size_t index = 1; index < trimmed.size(); ++index) {
        total += std::fabs(trimmed[index] - trimmed[index - 1]);
    }
    double jitter = std::round(total / static_cast<double>(trimmed.size() - 1));
    if (jitter == 0) {
        jitter = 1;
    }
    if (jitter >= median) {
        jitter = values.front();
    }
    return jitter;
}

int next_download_chunk_mb(std::int64_t bytes, std::chrono::nanoseconds elapsed) {
    if (bytes <= 0 || elapsed <= std::chrono::nanoseconds::zero()) {
        return 25;
    }
    const double target =
        static_cast<double>(bytes) / std::chrono::duration<double>(elapsed).count() * 6 / 1e6;
    if (std::isnan(target) || target <= 25) {
        return 25;
    }
    if (std::isinf(target) || target >= 250) {
        return 250;
    }
    return static_cast<int>(std::round(target));
}

std::size_t next_server_index(std::size_t servers, int worker,
                              std::atomic<std::uint32_t>& attempts) {
    const std::uint32_t attempt = attempts.fetch_add(1);
    return (static_cast<std::size_t>(worker) + attempt) % servers;
}

std::vector<std::string> script_sources(std::string_view html, std::size_t limit) {
    std::vector<std::string> sources;
    std::size_t position = 0;
    while (sources.size() < limit) {
        const auto found = html.find("src=", position);
        if (found == std::string_view::npos) {
            break;
        }
        position = found + 1;
        const std::size_t start = found + 4;
        if (start >= html.size() || !quote_character(html[start])) {
            continue;
        }
        const auto end = html.find_first_of("\"'", start + 1);
        if (end == std::string_view::npos) {
            continue;
        }
        const std::string_view value = html.substr(start + 1, end - start - 1);
        if (value.find(".js", 1) == std::string_view::npos) {
            continue;
        }
        sources.emplace_back(value);
        position = end + 1;
    }
    return sources;
}

std::optional<std::string> qms_library_key(std::string_view script) {
    constexpr std::string_view marker = "QMSLibrary({";
    std::size_t position = 0;
    for (;;) {
        const auto found = script.find(marker, position);
        if (found == std::string_view::npos) {
            return std::nullopt;
        }
        position = found + 1;
        std::size_t index = found + marker.size();
        const auto skip_space = [&] {
            while (index < script.size() && regex_space(script[index])) {
                ++index;
            }
        };
        skip_space();
        if (script.substr(index, 2) != "id") {
            continue;
        }
        index += 2;
        skip_space();
        if (index >= script.size() || script[index] != ':') {
            continue;
        }
        ++index;
        skip_space();
        if (index >= script.size() || !quote_character(script[index])) {
            continue;
        }
        const std::size_t start = ++index;
        while (index < script.size() && key_character(script[index])) {
            ++index;
        }
        const std::size_t length = index - start;
        if (length < 16 || length > 128 || index >= script.size() ||
            !quote_character(script[index])) {
            continue;
        }
        return std::string(script.substr(start, length));
    }
}

bool is_auth_status(int status) {
    return status == 401 || status == 403;
}

std::string unix_millis() {
    return std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count());
}

std::string unix_nanos_base36() {
    return text::format_int(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count(),
                            36);
}

} // namespace detail

Backend::Backend(Options options)
    : options_(std::move(options)), browser_key_(options_.browser_key) {}

net::HttpClientOptions Backend::api_options() const {
    net::HttpClientOptions options;
    options.tls = options_.tls;
    options.connect_timeout = std::chrono::seconds(5);
    options.tls_handshake_timeout = std::chrono::seconds(8);
    options.response_header_timeout = std::chrono::seconds(12);
    options.follow_redirects = true;
    return options;
}

net::HttpClientOptions Backend::measurement_options() const {
    net::HttpClientOptions options = api_options();
    // Measurement requests must reach the selected server: a redirect would
    // change the server and could confirm bytes that were never transferred.
    options.follow_redirects = false;
    return options;
}

net::WebSocketOptions Backend::websocket_options() const {
    net::WebSocketOptions options;
    options.tls = options_.tls;
    options.connect_timeout = std::chrono::seconds(5);
    options.handshake_timeout = std::chrono::seconds(8);
    options.read_limit = detail::websocket_read_limit;
    options.headers = {{"User-Agent", std::string(service::user_agent)}};
    return options;
}

void Backend::log(std::string_view message) const {
    if (options_.log) {
        options_.log(message);
    }
}

std::string Backend::current_browser_key() const {
    std::lock_guard lock(mutex_);
    return browser_key_;
}

} // namespace puls::speedtestru
