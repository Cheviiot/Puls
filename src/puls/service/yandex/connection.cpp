#include "puls/core/json.hpp"
#include "puls/service/http.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

namespace puls::yandex {

namespace {

using service::ErrorCode;
using service::Phase;
using service::ServiceId;

Error connection_error(ErrorCode code, bool retryable, Error cause) {
    return service::new_error(ServiceId::yandex, Phase::connection, code, retryable,
                              std::move(cause));
}

} // namespace

Result<service::ConnectionInfo> Backend::detect_connection(const Context& ctx) {
    auto url = net::Url::parse(options_.internet_page_url);
    if (!url) {
        return connection_error(ErrorCode::internal, false,
                                Error::wrap("запрос страницы интернетометра", url.error()));
    }
    net::HttpSession session(http_options());
    net::HttpRequest request;
    request.url = std::move(url).value();
    request.headers = {{"User-Agent", std::string(service::user_agent)}, {"Accept", "text/html"}};
    auto response = session.send(ctx, request);
    if (!response) {
        const ErrorCode code = service::classify_error(response.error());
        return connection_error(code, service::retryable_code(code),
                                Error::wrap("запрос страницы интернетометра", response.error()));
    }
    const int status = response->status_code();
    if (status < 200 || status >= 300) {
        const auto [code, retryable] = service::classify_http_status(status);
        return connection_error(
            code, retryable,
            service::http_status_error(status, response->status(), "страница интернетометра"));
    }
    if (Error error =
            service::validate_content_type(*response, {"text/html", "application/xhtml+xml"})) {
        return connection_error(ErrorCode::protocol, false,
                                Error::wrap("страница интернетометра", error));
    }
    auto body = response->read_up_to(ctx, detail::max_internet_page_size + 1);
    if (!body) {
        const ErrorCode code = service::classify_error(body.error());
        return connection_error(code, service::retryable_code(code),
                                Error::wrap("чтение страницы интернетометра", body.error()));
    }
    if (body->size() > detail::max_internet_page_size) {
        return connection_error(ErrorCode::protocol, false,
                                Error::make("страница интернетометра превышает безопасный предел"));
    }

    auto object = detail::extract_balanced_json_object(*body, detail::client_state_marker);
    if (!object) {
        return connection_error(
            ErrorCode::protocol, false,
            Error::wrap("поиск состояния клиента на странице интернетометра", object.error()));
    }
    auto state = json::parse(*object);
    Result<json::ObjectReader> reader =
        state ? json::ObjectReader::from(*state) : Result<json::ObjectReader>(state.error());
    Result<json::ObjectReader> ip =
        reader ? reader->object("ip") : Result<json::ObjectReader>(reader.error());
    Result<std::string> v4 = ip ? ip->string("v4") : Result<std::string>(ip.error());
    Result<std::string> v6 = v4 ? ip->string("v6") : Result<std::string>(v4.error());
    if (!v6) {
        return connection_error(ErrorCode::protocol, false,
                                Error::wrap("разбор состояния клиента интернетометра", v6.error()));
    }
    for (const std::string& candidate : {*v4, *v6}) {
        if (const auto address = IpAddress::parse(candidate); address && address->zone().empty()) {
            service::ConnectionInfo info;
            info.external_ip = address->unmap();
            return info;
        }
    }
    return connection_error(
        ErrorCode::protocol, false,
        Error::make("страница интернетометра не сообщила действительный IP-адрес"));
}

} // namespace puls::yandex
