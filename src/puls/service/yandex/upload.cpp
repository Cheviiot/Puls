#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/net/websocket.hpp"
#include "puls/service/http.hpp"
#include "puls/service/yandex/backend.hpp"
#include "puls/service/yandex/internal.hpp"

#include <cmath>

namespace puls::yandex {

namespace {

using namespace std::chrono_literals;

Error invalid_acknowledgement(std::string detail) {
    return service::protocol_error(
        Error::wrap("неверное подтверждение WebSocket", Error::make(std::move(detail))));
}

// Parses {"k":"u","b":<bytes>}; the error never contains the untrusted payload.
Result<std::int64_t> parse_acknowledgement(const net::WebSocketMessage& message) {
    if (!message.text) {
        return invalid_acknowledgement("неожиданный тип сообщения 2");
    }
    auto value = json::parse(message.payload);
    if (!value) {
        return invalid_acknowledgement("ошибка JSON: " + value.error().message());
    }
    auto reader = json::ObjectReader::from(*value);
    if (!reader) {
        return invalid_acknowledgement("ошибка JSON: " + reader.error().message());
    }
    auto kind = reader->string("k");
    if (!kind) {
        return invalid_acknowledgement("ошибка JSON: " + kind.error().message());
    }
    auto bytes = reader->optional_integer("b");
    if (!bytes) {
        return invalid_acknowledgement("ошибка JSON: " + bytes.error().message());
    }
    if (*kind != "u" || !bytes->has_value() || **bytes < 0) {
        return invalid_acknowledgement("неверная схема");
    }
    return **bytes;
}

} // namespace

Error Backend::websocket_upload(const Context& ctx, const std::string& raw_url,
                                std::chrono::nanoseconds connection_timeout,
                                std::chrono::nanoseconds duration, const measure::ReadyFn& ready,
                                const measure::RecordFn& record) const {
    auto url = net::Url::parse(raw_url);
    if (!url) {
        return url.error();
    }
    if (!detail::valid_websocket_probe_url(raw_url)) {
        return service::protocol_error("неверный URL WebSocket upload-пробы");
    }
    auto query = net::parse_query(url->raw_query);
    const double seconds = std::chrono::duration<double>(duration).count();
    const auto native_duration =
        std::max<long long>(1, static_cast<long long>(std::ceil(seconds + 2)));
    net::set_query_value(query, "type", "upload");
    net::set_query_value(query, "duration", std::to_string(native_duration));
    url->raw_query = net::encode_query(std::move(query));

    net::WebSocketOptions options;
    options.tls = options_.tls;
    options.handshake_timeout = 10s;
    options.read_limit = detail::max_websocket_ack_size;
    options.headers = {{"User-Agent", std::string(service::user_agent)},
                       {"Origin", std::string(detail::origin)}};
    Result<std::unique_ptr<net::WebSocket>> socket = Error();
    if (connection_timeout > std::chrono::nanoseconds::zero()) {
        const CancelScope dial(ctx, connection_timeout);
        socket = net::WebSocket::connect(dial.context(), *url, options);
    } else {
        socket = net::WebSocket::connect(ctx, *url, options);
    }
    if (!socket) {
        return Error::wrap("подключение WebSocket", socket.error());
    }

    static const std::string chunk(detail::websocket_chunk_size, '\0');
    const auto chunk_size = static_cast<std::int64_t>(chunk.size());
    std::int64_t sent = 0;
    std::int64_t acknowledged = 0;
    Error excess;
    net::DuplexHandlers handlers;
    handlers.next_payload = [&] {
        sent += chunk_size;
        return std::span<const char>(chunk.data(), chunk.size());
    };
    handlers.write_failed = [&] { sent -= chunk_size; };
    handlers.on_message = [&](const net::WebSocketMessage& message) -> Error {
        auto bytes = parse_acknowledgement(message);
        if (!bytes) {
            return bytes.error();
        }
        const std::int64_t available = sent - acknowledged;
        if (available < 0 || *bytes > available) {
            excess = service::protocol_error(
                Error::wrap("неверное подтверждение WebSocket",
                            Error::make("объём превышает отправленные данные")));
            return excess;
        }
        if (*bytes == 0) {
            return {};
        }
        ready();
        acknowledged += *bytes;
        record(*bytes);
        return {};
    };
    const net::DuplexResult result = (*socket)->duplex(ctx, handlers, net::DuplexOptions{5s, 1ms});
    if (result.canceled || ctx.done()) {
        return ctx.err() ? ctx.err() : errors::canceled();
    }
    if (excess) {
        return excess;
    }
    if (result.write_error) {
        return Error::wrap("отправка через WebSocket", result.write_error);
    }
    if (result.handler_error) {
        return Error::wrap("получение через WebSocket", result.handler_error);
    }
    return Error::wrap("получение через WebSocket",
                       result.read_error ? result.read_error : Error::make("соединение закрыто"));
}

Error Backend::http_upload(const Context& ctx, net::HttpSession& session,
                           const std::string& post_url, std::string_view payload,
                           const measure::ReadyFn& ready, const measure::RecordFn& record) const {
    while (!ctx.done()) {
        auto url = net::Url::parse(detail::cache_bust(post_url));
        if (!url) {
            return url.error();
        }
        net::HttpRequest request;
        request.method = "POST";
        request.url = std::move(url).value();
        request.headers = {{"User-Agent", std::string(service::user_agent)},
                           {"Content-Type", "application/octet-stream"}};
        request.body = payload;
        auto response = session.send(ctx, request);
        if (!response) {
            return response.error();
        }
        const int status = response->status_code();
        if (status < 200 || status >= 300) {
            return service::http_status_error(status, response->status(), "HTTP-запрос отдачи");
        }
        if (const auto length = response->content_length();
            length && *length > detail::max_upload_response) {
            return service::protocol_error(
                "неожиданный размер ответа HTTP при отдаче: превышен безопасный предел");
        }
        auto response_bytes = response->discard(ctx, detail::max_upload_response + 1);
        if (!response_bytes) {
            return response_bytes.error();
        }
        if (*response_bytes > detail::max_upload_response) {
            return service::protocol_error("ответ HTTP при отдаче превышает безопасный предел");
        }
        response->close();
        ready();
        record(static_cast<std::int64_t>(payload.size()));
    }
    return ctx.err();
}

} // namespace puls::yandex
