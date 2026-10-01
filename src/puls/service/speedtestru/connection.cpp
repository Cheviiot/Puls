#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

namespace puls::speedtestru {

namespace {

using service::ErrorCode;
using service::Phase;
using service::ServiceId;

} // namespace

Result<service::ConnectionInfo> Backend::detect_connection(const Context& ctx) {
    const auto fetch_ip = [this](const Context& request_ctx, const std::string& key) {
        Fetch<ExternalIp> result;
        auto fetched =
            fetch_api_json(request_ctx, "/api/asn_provider/ip", key, detail::max_connection_bytes);
        result.status = fetched.status;
        result.error = fetched.error;
        if (!result.error) {
            auto reader = json::ObjectReader::from(fetched.value);
            auto ip = reader ? reader->string("ip") : Result<std::string>(reader.error());
            if (!ip) {
                result.error = service::protocol_error(
                    Error::wrap("разбор ответа /api/asn_provider/ip", ip.error()));
            } else {
                result.value.ip = *ip;
            }
        }
        return result;
    };
    auto ip_payload = with_browser_key<ExternalIp>(ctx, fetch_ip);
    if (!ip_payload) {
        const ErrorCode code = service::classify_error(ip_payload.error());
        return service::new_error(ServiceId::speedtest, Phase::connection, code,
                                  service::retryable_code(code), ip_payload.error());
    }
    const auto address = IpAddress::parse(text::trim_space(ip_payload->ip));
    if (!address || !address->zone().empty()) {
        return service::new_error(
            ServiceId::speedtest, Phase::connection, ErrorCode::protocol, false,
            Error::make("speedtest.ru вернул неверный IP-адрес " + text::quote(ip_payload->ip)));
    }
    service::ConnectionInfo info;
    info.external_ip = address->unmap();

    const auto fetch_asn = [this](const Context& request_ctx, const std::string& key) {
        Fetch<Asn> result;
        auto fetched =
            fetch_api_json(request_ctx, "/api/asn_provider/asn", key, detail::max_connection_bytes);
        result.status = fetched.status;
        result.error = fetched.error;
        if (!result.error) {
            auto reader = json::ObjectReader::from(fetched.value);
            auto ip = reader ? reader->string("ip") : Result<std::string>(reader.error());
            auto provider = ip ? reader->string("provider_name") : Result<std::string>(ip.error());
            if (!provider) {
                result.error = service::protocol_error(
                    Error::wrap("разбор ответа /api/asn_provider/asn", provider.error()));
            } else {
                result.value = Asn{*ip, *provider};
            }
        }
        return result;
    };
    auto asn = with_browser_key<Asn>(ctx, fetch_asn);
    if (!asn) {
        info.warnings.push_back("интернет-провайдер недоступен: " + asn.error().message());
        return info;
    }
    const auto asn_address = IpAddress::parse(text::trim_space(asn->ip));
    if (!asn_address || !asn_address->zone().empty() ||
        !(asn_address->unmap() == info.external_ip)) {
        info.warnings.push_back(
            "speedtest.ru вернул несовпадающий IP при определении интернет-провайдера");
        return info;
    }
    const std::string isp(text::trim_space(asn->provider_name));
    if (isp.empty() || isp.size() > detail::max_isp_name_bytes ||
        isp.find_first_of(std::string_view("\r\n\0", 3)) != std::string::npos) {
        info.warnings.push_back("speedtest.ru не сообщил корректное название интернет-провайдера");
        return info;
    }
    info.isp = isp;
    return info;
}

Backend::Fetch<boost::json::value> Backend::fetch_api_json(const Context& ctx,
                                                           std::string_view path,
                                                           const std::string& key,
                                                           std::size_t limit) const {
    Fetch<boost::json::value> result;
    const CancelScope request_scope(ctx, detail::api_request_timeout);
    const Context& request_ctx = request_scope.context();
    const std::string operation(path);
    auto url = net::Url::parse(options_.api_base + operation + "?t=" + detail::unix_millis());
    if (!url) {
        result.error = url.error();
        return result;
    }
    net::HttpSession session(api_options());
    net::HttpRequest request;
    request.url = std::move(url).value();
    request.headers = {{"x-api-key", key},
                       {"User-Agent", std::string(service::user_agent)},
                       {"Accept", "application/json"}};
    auto response = session.send(request_ctx, request);
    if (!response) {
        result.error = Error::wrap("запрос " + operation, response.error());
        return result;
    }
    result.status = response->status_code();
    if (result.status < 200 || result.status >= 300) {
        result.error = service::http_status_error(result.status, response->status(), operation);
        return result;
    }
    if (Error error = service::validate_content_type(*response, {"application/json"})) {
        result.error = service::protocol_error(Error::wrap(operation, error));
        return result;
    }
    auto value = service::decode_json_limited(request_ctx, *response, limit);
    if (!value) {
        result.error =
            service::protocol_error(Error::wrap("разбор ответа " + operation, value.error()));
        return result;
    }
    result.value = std::move(value).value();
    return result;
}

} // namespace puls::speedtestru
