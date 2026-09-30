#include "puls/core/json.hpp"
#include "puls/core/text.hpp"
#include "puls/service/http.hpp"
#include "puls/service/speedtestru/internal.hpp"

#include <set>

namespace puls::speedtestru {

namespace {

using service::ErrorCode;
using service::Phase;
using service::ServiceId;

Error send_text(const Context& ctx, net::WebSocket& socket, std::string_view command) {
    if (Error error = socket.write(ctx, true, command)) {
        return Error::wrap("отправка " + std::string(command), error);
    }
    return {};
}

Error expect_exact(const Context& ctx, net::WebSocket& socket, std::string_view expected) {
    auto message = socket.read(ctx);
    if (!message) {
        return Error::wrap("получение " + std::string(expected), message.error());
    }
    if (!message->text || text::trim_space(message->payload) != expected) {
        return service::protocol_error("неожиданный ответ " + text::quote(message->payload) +
                                       ", ожидался " + text::quote(expected));
    }
    return {};
}

Error expect_server_info(const Context& ctx, net::WebSocket& socket) {
    auto message = socket.read(ctx);
    if (!message) {
        return Error::wrap("получение qms_testing", message.error());
    }
    if (!message->text) {
        return service::protocol_error("неожиданный тип кадра со сведениями о сервере: 2");
    }
    const std::string info = text::to_lower_ascii(text::trim_space(message->payload));
    if (info == "qms_testing" || info.rfind("qms_testing/", 0) == 0) {
        return {};
    }
    if (auto value = json::parse(message->payload)) {
        if (auto reader = json::ObjectReader::from(*value)) {
            if (auto name = reader->string("name");
                name && text::equal_fold_ascii(*name, "qms_testing")) {
                return {};
            }
        }
    }
    return service::protocol_error("неожиданный ответ со сведениями о сервере: " +
                                   text::quote(message->payload));
}

Error handshake(const Context& ctx, net::WebSocket& socket) {
    if (Error error = send_text(ctx, socket, "HI")) {
        return error;
    }
    if (Error error = expect_exact(ctx, socket, "HELLO")) {
        return error;
    }
    if (Error error = send_text(ctx, socket, "GETINFO")) {
        return error;
    }
    if (Error error = expect_server_info(ctx, socket)) {
        return error;
    }
    if (Error error = send_text(ctx, socket, "AUTH")) {
        return error;
    }
    return expect_exact(ctx, socket, "READY_TO_TEST");
}

Result<double> ping_once(const Context& ctx, net::WebSocket& socket) {
    const auto started = std::chrono::steady_clock::now();
    if (Error error = socket.write(ctx, true, "PING")) {
        return error;
    }
    auto message = socket.read(ctx);
    if (!message) {
        return message.error();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (!message->text) {
        return service::protocol_error("неожиданный тип ответа PONG: 2");
    }
    const auto parts = text::fields(message->payload);
    if (parts.size() != 2 || parts[0] != "PONG") {
        return service::protocol_error("неожиданный ответ на проверку задержки: " +
                                       text::quote(message->payload));
    }
    const auto stamp = text::parse_int(parts[1], 10);
    if (!stamp.value || *stamp.value < 0) {
        return service::protocol_error("неверная временная метка PONG: " + text::quote(parts[1]));
    }
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

} // namespace

Result<service::PingResult> Backend::probe_ping(const Context& ctx, const QmsServer& server,
                                                int sample_count) const {
    auto url = net::Url::parse(server.websocket_url());
    if (!url) {
        return url.error();
    }
    auto socket = net::WebSocket::connect(ctx, *url, websocket_options());
    if (!socket) {
        return Error::wrap("подключение WebSocket к " + server.websocket_url(), socket.error());
    }
    if (Error error = handshake(ctx, **socket)) {
        return error;
    }
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(sample_count));
    for (int index = 0; index < sample_count; ++index) {
        auto sample = ping_once(ctx, **socket);
        if (!sample) {
            return sample.error();
        }
        samples.push_back(*sample);
    }
    service::PingResult result = service::stats_with_method(samples, "median");
    result.jitter_ms = detail::qms_jitter(samples, result.median_ms);
    return result;
}

Result<service::PingResult> Backend::ping(const Context& ctx) {
    if (ctx.done()) {
        return service::new_error(ServiceId::speedtest, Phase::ping, ErrorCode::canceled, false,
                                  ctx.err());
    }
    QmsServer selected;
    std::vector<QmsServer> servers;
    {
        std::lock_guard lock(mutex_);
        selected = selected_;
        servers = servers_;
    }
    if (selected.host.empty()) {
        return service::new_error(ServiceId::speedtest, Phase::ping, ErrorCode::internal, false,
                                  Error::make("сначала необходимо выбрать сервер"));
    }
    if (selected.ping.samples == detail::ping_samples && selected.ping.value_ms > 0) {
        return selected.ping;
    }
    if (servers.empty()) {
        servers = {selected};
    }
    std::vector<QmsServer> ordered{selected};
    for (const auto& candidate : servers) {
        if (candidate.host != selected.host) {
            ordered.push_back(candidate);
        }
    }

    std::vector<Error> failures;
    std::set<std::string> failed_hosts;
    for (QmsServer candidate : ordered) {
        auto result = probe_ping(ctx, candidate, detail::ping_samples);
        if (result) {
            if (candidate.host != selected.host) {
                log("speedtest.ru · задержка: переход с недоступного сервера " + selected.host +
                    " на " + candidate.host);
            }
            candidate.ping = *result;
            candidate.rtt = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::duration<double, std::milli>(result->median_ms));
            std::vector<QmsServer> healthy{candidate};
            for (const auto& remaining : servers) {
                if (remaining.host != candidate.host && failed_hosts.count(remaining.host) == 0) {
                    healthy.push_back(remaining);
                }
            }
            std::lock_guard lock(mutex_);
            selected_ = candidate;
            servers_ = std::move(healthy);
            return result;
        }
        failed_hosts.insert(candidate.host);
        failures.push_back(Error::wrap(candidate.host, result.error()));
        if (ctx.done()) {
            break;
        }
    }
    const Error error = Error::join(std::move(failures));
    const ErrorCode code = service::classify_error(error);
    return service::new_error(ServiceId::speedtest, Phase::ping, code,
                              service::retryable_code(code), error);
}

} // namespace puls::speedtestru
