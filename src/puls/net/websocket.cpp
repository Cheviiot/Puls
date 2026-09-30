#include "puls/net/websocket.hpp"

#include "puls/net/detail/transport.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket/error.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>

#include <algorithm>
#include <optional>
#include <variant>

namespace puls::net {

namespace {

namespace websocket = boost::beast::websocket;
using detail::OpRunner;
using PlainWebSocket = websocket::stream<detail::tcp::socket>;
using TlsWebSocket = websocket::stream<detail::TlsStream>;
using AnyWebSocket = std::variant<PlainWebSocket, TlsWebSocket>;

template <class Stream>
void configure(Stream& stream, const WebSocketOptions& options) {
    stream.set_option(websocket::stream_base::timeout{websocket::stream_base::none(),
                                                      websocket::stream_base::none(), false});
    std::vector<Header> headers = options.headers;
    stream.set_option(websocket::stream_base::decorator(
        [headers = std::move(headers)](websocket::request_type& request) {
            for (const auto& header : headers) {
                request.set(header.name, header.value);
            }
        }));
    // The read limit is enforced by read_message below so that an oversized
    // message fails immediately instead of after a close handshake.
    stream.read_message_max(0);
    stream.auto_fragment(false);
}

// Reads one complete message into buffer, failing with message_too_big as soon
// as more than limit bytes arrive. handler(error, text) runs on completion.
template <class Stream, class Handler>
void read_message(Stream& stream, boost::beast::flat_buffer& buffer, std::size_t limit,
                  Handler handler) {
    const std::size_t remaining = limit + 1 - std::min(buffer.size(), limit);
    stream.async_read_some(
        buffer, remaining,
        [&stream, &buffer, limit, handler = std::move(handler)](
            const boost::system::error_code& error, std::size_t) mutable {
            if (error) {
                handler(error, false);
                return;
            }
            if (buffer.size() > limit) {
                handler(boost::system::error_code(websocket::error::message_too_big), false);
                return;
            }
            if (stream.is_message_done()) {
                handler(boost::system::error_code(), stream.got_text());
                return;
            }
            read_message(stream, buffer, limit, std::move(handler));
        });
}

} // namespace

struct WebSocket::Impl {
    Impl() : runner(io) {}

    detail::asio::io_context io;
    OpRunner runner;
    std::optional<AnyWebSocket> stream;
    boost::beast::flat_buffer buffer;
    std::size_t read_limit = 64 << 10;

    void close() noexcept {
        if (!stream) {
            return;
        }
        std::visit(
            [](auto& value) {
                boost::system::error_code ignored;
                boost::beast::get_lowest_layer(value).close(ignored);
            },
            *stream);
    }

    // Describes a read failure, including a close frame from the server.
    Error read_failure(const Context& ctx, OpRunner::Outcome outcome,
                       const boost::system::error_code& code) {
        if (code == websocket::error::closed && outcome == OpRunner::Outcome::completed) {
            const auto reason = std::visit([](auto& value) { return value.reason(); }, *stream);
            std::string message = "websocket: close " + std::to_string(reason.code);
            if (!reason.reason.empty()) {
                message += ": ";
                message.append(reason.reason.data(), reason.reason.size());
            }
            return detail::network_error(std::move(message));
        }
        return detail::outcome_error(ctx, outcome, code, "i/o timeout");
    }
};

WebSocket::WebSocket(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WebSocket::~WebSocket() {
    close();
}

void WebSocket::close() noexcept {
    impl_->close();
}

Result<std::unique_ptr<WebSocket>> WebSocket::connect(const Context& ctx, const Url& url,
                                                      const WebSocketOptions& options) {
    if (url.scheme != "ws" && url.scheme != "wss") {
        return Error::make("websocket: bad scheme " + url.scheme);
    }
    auto impl = std::make_unique<Impl>();
    const auto started = std::chrono::steady_clock::now();

    detail::ConnectOptions connect;
    connect.tls = options.tls;
    connect.connect_timeout = options.connect_timeout;
    connect.tls_handshake_timeout = options.handshake_timeout;
    auto established = detail::establish(impl->io, impl->runner, ctx, url, connect);
    if (!established) {
        return std::move(established).error();
    }
    if (established->index() == 0) {
        impl->stream.emplace(std::in_place_index<0>, std::move(std::get<0>(*established)));
    } else {
        impl->stream.emplace(std::in_place_index<1>, std::move(std::get<1>(*established)));
    }
    std::visit([&options](auto& value) { configure(value, options); }, *impl->stream);
    impl->read_limit = options.read_limit;

    std::optional<Duration> remaining;
    if (options.handshake_timeout > Duration::zero()) {
        remaining = options.handshake_timeout - (std::chrono::steady_clock::now() - started);
        if (*remaining <= Duration::zero()) {
            impl->close();
            return detail::network_error("websocket: handshake timeout", true);
        }
    }
    const std::string host = url.host;
    const std::string target = url.request_uri();
    websocket::response_type response;
    boost::system::error_code code;
    const auto outcome = impl->runner.run(
        ctx, remaining,
        [&](OpRunner::Done done) {
            std::visit(
                [&](auto& value) {
                    value.async_handshake(response, host, target,
                                          [&code, done](const boost::system::error_code& error) {
                                              code = error;
                                              done();
                                          });
                },
                *impl->stream);
        },
        [&impl] { impl->close(); });
    if (outcome != OpRunner::Outcome::completed || code) {
        Error error;
        if (code == websocket::error::upgrade_declined && outcome == OpRunner::Outcome::completed) {
            error = detail::network_error("websocket: bad handshake");
        } else {
            error = detail::outcome_error(ctx, outcome, code, "websocket: handshake timeout");
        }
        impl->close();
        return error;
    }
    return std::unique_ptr<WebSocket>(new WebSocket(std::move(impl)));
}

Result<WebSocketMessage> WebSocket::read(const Context& ctx) {
    auto& impl = *impl_;
    impl.buffer.clear();
    boost::system::error_code code;
    bool text = false;
    const auto outcome = impl.runner.run(
        ctx, std::nullopt,
        [&](OpRunner::Done done) {
            std::visit(
                [&](auto& value) {
                    read_message(value, impl.buffer, impl.read_limit,
                                 [&code, &text, done](const boost::system::error_code& error,
                                                      bool got_text) {
                                     code = error;
                                     text = got_text;
                                     done();
                                 });
                },
                *impl.stream);
        },
        [&impl] { impl.close(); });
    if (outcome != OpRunner::Outcome::completed || code) {
        Error error = impl.read_failure(ctx, outcome, code);
        impl.close();
        return error;
    }
    WebSocketMessage message;
    message.text = text;
    message.payload = boost::beast::buffers_to_string(impl.buffer.data());
    impl.buffer.clear();
    return message;
}

Error WebSocket::write(const Context& ctx, bool text, std::string_view payload) {
    auto& impl = *impl_;
    boost::system::error_code code;
    const auto outcome = impl.runner.run(
        ctx, std::nullopt,
        [&](OpRunner::Done done) {
            std::visit(
                [&](auto& value) {
                    value.text(text);
                    value.async_write(
                        boost::asio::buffer(payload.data(), payload.size()),
                        [&code, done](const boost::system::error_code& error, std::size_t) {
                            code = error;
                            done();
                        });
                },
                *impl.stream);
        },
        [&impl] { impl.close(); });
    if (outcome != OpRunner::Outcome::completed || code) {
        Error error = detail::outcome_error(ctx, outcome, code, "i/o timeout");
        impl.close();
        return error;
    }
    return {};
}

DuplexResult WebSocket::duplex(const Context& ctx, const DuplexHandlers& handlers,
                               const DuplexOptions& options) {
    auto& impl = *impl_;
    DuplexResult result;
    boost::beast::flat_buffer read_buffer;
    detail::asio::steady_timer write_timer(impl.io);
    detail::asio::steady_timer gap_timer(impl.io);
    int pending = 0;
    bool stopping = false;
    bool writing = false;
    bool write_timed_out = false;
    OpRunner::Done done;

    const auto maybe_done = [&] {
        if (stopping && pending == 0 && done) {
            auto finish = std::move(done);
            done = nullptr;
            finish();
        }
    };
    const auto stop = [&] {
        if (!stopping) {
            stopping = true;
            impl.close();
            write_timer.cancel();
            gap_timer.cancel();
        }
    };

    std::function<void()> start_read;
    std::function<void()> start_write;
    start_read = [&] {
        ++pending;
        std::visit(
            [&](auto& value) {
                read_message(value, read_buffer, impl.read_limit,
                             [&](const boost::system::error_code& error, bool text) {
                                 --pending;
                                 if (stopping) {
                                     maybe_done();
                                     return;
                                 }
                                 if (error) {
                                     result.read_error = impl.read_failure(
                                         ctx, OpRunner::Outcome::completed, error);
                                     stop();
                                     maybe_done();
                                     return;
                                 }
                                 WebSocketMessage message;
                                 message.text = text;
                                 message.payload =
                                     boost::beast::buffers_to_string(read_buffer.data());
                                 read_buffer.clear();
                                 if (Error handler_error = handlers.on_message(message)) {
                                     result.handler_error = std::move(handler_error);
                                     stop();
                                     maybe_done();
                                     return;
                                 }
                                 start_read();
                             });
            },
            *impl.stream);
    };
    start_write = [&] {
        const std::span<const char> payload = handlers.next_payload();
        ++pending;
        write_timer.expires_after(options.write_timeout);
        ++pending;
        write_timer.async_wait([&](const boost::system::error_code& error) {
            --pending;
            if (!error && !stopping && writing) {
                write_timed_out = true;
                impl.close();
            }
            maybe_done();
        });
        writing = true;
        std::visit(
            [&](auto& value) {
                value.binary(true);
                value.async_write(
                    boost::asio::buffer(payload.data(), payload.size()),
                    [&](const boost::system::error_code& error, std::size_t) {
                        --pending;
                        writing = false;
                        write_timer.cancel();
                        if (error) {
                            if (!stopping || write_timed_out) {
                                result.write_error =
                                    write_timed_out
                                        ? detail::network_error("i/o timeout", true)
                                        : detail::outcome_error(ctx, OpRunner::Outcome::completed,
                                                                error, "i/o timeout");
                            }
                            if (handlers.write_failed) {
                                handlers.write_failed();
                            }
                            stop();
                            maybe_done();
                            return;
                        }
                        if (stopping) {
                            maybe_done();
                            return;
                        }
                        ++pending;
                        gap_timer.expires_after(options.write_gap);
                        gap_timer.async_wait([&](const boost::system::error_code& gap_error) {
                            --pending;
                            if (gap_error || stopping) {
                                maybe_done();
                                return;
                            }
                            start_write();
                        });
                    });
            },
            *impl.stream);
    };

    const auto outcome = impl.runner.run(
        ctx, std::nullopt,
        [&](OpRunner::Done finished) {
            done = std::move(finished);
            start_read();
            start_write();
        },
        [&] {
            stop();
            maybe_done();
        });
    result.canceled = outcome == OpRunner::Outcome::canceled || ctx.done();
    stop();
    return result;
}

} // namespace puls::net
