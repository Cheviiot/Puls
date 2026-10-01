#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

namespace puls::speedtestru {

Result<std::string> Backend::ensure_jwt(const Context& ctx, bool refresh,
                                        const std::string& previous) {
    std::lock_guard auth_lock(auth_mutex_);
    std::string current;
    std::string key;
    {
        std::lock_guard lock(mutex_);
        current = jwt_;
        key = browser_key_;
    }
    if (!current.empty() && (!refresh || (!previous.empty() && current != previous))) {
        return current;
    }
    Fetch<std::string> fetched = fetch_jwt(ctx, key);
    if (fetched.error && detail::is_auth_status(fetched.status)) {
        auto refreshed_key = extract_browser_key(ctx);
        if (refreshed_key) {
            key = std::move(refreshed_key).value();
            log("speedtest.ru · авторизация JWT обновила ключ браузерного клиента");
            fetched = fetch_jwt(ctx, key);
        } else {
            fetched.error = Error::join({fetched.error, refreshed_key.error()});
        }
    }
    std::lock_guard lock(mutex_);
    if (fetched.error) {
        jwt_.clear();
        return fetched.error;
    }
    browser_key_ = key;
    jwt_ = fetched.value;
    return fetched.value;
}

Backend::Fetch<std::string> Backend::fetch_jwt(const Context& ctx, const std::string& key) const {
    Fetch<std::string> result;
    const CancelScope request_scope(ctx, detail::api_request_timeout);
    const Context& request_ctx = request_scope.context();
    auto url =
        net::Url::parse(options_.api_base + "/api/server/gentoken?t=" + detail::unix_millis());
    if (!url) {
        result.error = url.error();
        return result;
    }
    net::HttpClientOptions options = api_options();
    options.follow_redirects = false;
    net::HttpSession session(options);
    net::HttpRequest request;
    request.method = "POST";
    request.url = std::move(url).value();
    request.headers = {{"x-api-key", key},
                       {"User-Agent", std::string(service::user_agent)},
                       {"Accept", "application/json"}};
    auto response = session.send(request_ctx, request);
    if (!response) {
        result.error = Error::wrap("запрос gentoken", response.error());
        return result;
    }
    result.status = response->status_code();
    if (result.status < 200 || result.status >= 300) {
        result.error = service::http_status_error(result.status, response->status(), "gentoken");
        return result;
    }
    if (Error error = service::validate_content_type(*response, {"application/json"})) {
        result.error = service::protocol_error(Error::wrap("gentoken", error));
        return result;
    }
    auto value = service::decode_json_limited(request_ctx, *response, detail::max_jwt_bytes);
    auto reader =
        value ? json::ObjectReader::from(*value) : Result<json::ObjectReader>(value.error());
    auto token = reader ? reader->string("token") : Result<std::string>(reader.error());
    if (!token) {
        result.error =
            service::protocol_error(Error::wrap("разбор ответа gentoken", token.error()));
        return result;
    }
    const std::string& text = *token;
    std::size_t parts = 1;
    bool empty_part = text.empty() || text.front() == '.' || text.back() == '.';
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] == '.') {
            ++parts;
            if (index + 1 < text.size() && text[index + 1] == '.') {
                empty_part = true;
            }
        }
    }
    if (text.size() < 16 || parts != 3 || empty_part) {
        result.error = service::authorization_error("gentoken вернул пустой или неверный токен");
        return result;
    }
    result.value = text;
    return result;
}

} // namespace puls::speedtestru
