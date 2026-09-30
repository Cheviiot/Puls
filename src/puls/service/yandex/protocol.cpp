#include "puls/core/text.hpp"
#include "puls/net/url.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

#include <atomic>
#include <chrono>

namespace puls::yandex::detail {

namespace {

std::atomic<std::uint64_t> cache_bust_counter{0};

bool json_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool valid_probe_url(std::string_view raw, std::string_view scheme) {
    const auto url = net::Url::parse(raw);
    return url && url->scheme == scheme && !url->hostname().empty() && !url->has_user &&
           url->fragment.empty() && url->opaque.empty();
}

} // namespace

std::string cache_bust(std::string_view url) {
    const char separator = url.find('?') == std::string_view::npos ? '?' : '&';
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    const std::uint64_t sequence = cache_bust_counter.fetch_add(1) + 1;
    std::string result(url);
    result.push_back(separator);
    result += "cb=" + text::format_int(now, 36) + "-" + text::format_uint(sequence, 36);
    return result;
}

bool valid_http_probe_url(std::string_view url) {
    return valid_probe_url(url, "https");
}

bool valid_websocket_probe_url(std::string_view url) {
    return valid_probe_url(url, "wss");
}

std::string endpoint_host(std::string_view url) {
    const auto parsed = net::Url::parse(url);
    return parsed ? text::to_lower_ascii(parsed->host) : std::string();
}

bool is_large_download_probe(std::string_view url) {
    const auto parsed = net::Url::parse(url);
    if (!parsed) {
        return false;
    }
    std::string_view path = parsed->path;
    while (!path.empty() && path.back() == '/') {
        path.remove_suffix(1);
    }
    constexpr std::string_view suffix = "/probes/50mb";
    return path.size() >= suffix.size() && path.substr(path.size() - suffix.size()) == suffix;
}

std::string host_of(std::string_view url) {
    const auto parsed = net::Url::parse(url);
    return parsed ? parsed->host : std::string();
}

Result<std::string_view> extract_balanced_json_object(std::string_view body,
                                                      std::string_view marker) {
    const auto index = body.find(marker);
    if (index == std::string_view::npos) {
        return Error::make("маркер " + text::quote(marker) + " не найден");
    }
    std::size_t start = index + marker.size();
    while (start < body.size() && json_space(body[start])) {
        ++start;
    }
    if (start >= body.size() || body[start] != '{') {
        return Error::make("после маркера нет JSON-объекта");
    }
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (std::size_t position = start; position < body.size(); ++position) {
        const char c = body[position];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return body.substr(start, position - start + 1);
            }
        }
    }
    return Error::make("не удалось найти конец JSON-объекта");
}

} // namespace puls::yandex::detail
