#include "puls/net/detail/transport.hpp"

#include "puls/core/ip.hpp"
#include "puls/core/text.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/ssl/error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/http/error.hpp>
#include <boost/beast/websocket/error.hpp>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace puls::net::detail {

namespace {

using namespace std::chrono_literals;

// Races the address families like Go's dialer (RFC 6555): the family of the
// first resolved address starts immediately and the other one joins after
// 300 ms or as soon as the first family has failed.
class ConnectRace : public std::enable_shared_from_this<ConnectRace> {
public:
    ConnectRace(asio::io_context& io, const std::vector<tcp::endpoint>& endpoints)
        : io_(io), fallback_timer_(io) {
        for (const auto& endpoint : endpoints) {
            const bool primary_family =
                endpoint.address().is_v6() == endpoints.front().address().is_v6();
            (primary_family ? primary_ : fallback_).push_back(endpoint);
        }
    }

    void start(OpRunner::Done done) {
        done_ = std::move(done);
        racers_[0].started = true;
        if (!fallback_.empty()) {
            fallback_timer_.expires_after(300ms);
            fallback_timer_.async_wait(
                [self = shared_from_this()](const boost::system::error_code& error) {
                    if (error || self->finished_ || self->aborted_ || self->racers_[1].started) {
                        return;
                    }
                    self->racers_[1].started = true;
                    self->step(1);
                });
        }
        step(0);
    }

    void abort() {
        aborted_ = true;
        for (auto& racer : racers_) {
            if (racer.socket) {
                boost::system::error_code ignored;
                racer.socket->close(ignored);
            }
        }
        fallback_timer_.cancel();
        if (pending_ == 0) {
            finish();
        }
    }

    std::optional<tcp::socket>& winner() { return winner_; }

    [[nodiscard]] Error error() const {
        for (const auto& racer : racers_) {
            if (racer.error) {
                return racer.error;
            }
        }
        return network_error("dial tcp: no suitable address found");
    }

private:
    struct Racer {
        std::optional<tcp::socket> socket;
        std::size_t next = 0;
        bool started = false;
        bool exhausted = false;
        Error error;
    };

    void step(int index) {
        Racer& racer = racers_[static_cast<std::size_t>(index)];
        const auto& endpoints = index == 0 ? primary_ : fallback_;
        while (racer.next < endpoints.size()) {
            const tcp::endpoint endpoint = endpoints[racer.next++];
            racer.socket.emplace(io_);
            boost::system::error_code error;
            racer.socket->open(endpoint.protocol(), error);
            if (error) {
                record(racer, endpoint, error);
                continue;
            }
            ++pending_;
            racer.socket->async_connect(endpoint, [self = shared_from_this(), index, endpoint](
                                                      const boost::system::error_code& result) {
                self->on_connect(index, endpoint, result);
            });
            return;
        }
        racer.exhausted = true;
        if (index == 0 && !fallback_.empty() && !racers_[1].started) {
            fallback_timer_.cancel();
            racers_[1].started = true;
            step(1);
            return;
        }
        const bool fallback_done = fallback_.empty() || racers_[1].exhausted;
        if (racers_[0].exhausted && fallback_done && pending_ == 0) {
            finish();
        }
    }

    void on_connect(int index, const tcp::endpoint& endpoint,
                    const boost::system::error_code& result) {
        --pending_;
        Racer& racer = racers_[static_cast<std::size_t>(index)];
        if (finished_) {
            return;
        }
        if (aborted_) {
            if (pending_ == 0) {
                finish();
            }
            return;
        }
        if (!result) {
            winner_ = std::move(*racer.socket);
            racer.socket.reset();
            Racer& other = racers_[index == 0 ? 1 : 0];
            if (other.socket) {
                boost::system::error_code ignored;
                other.socket->close(ignored);
            }
            fallback_timer_.cancel();
            finish();
            return;
        }
        record(racer, endpoint, result);
        step(index);
    }

    static void record(Racer& racer, const tcp::endpoint& endpoint,
                       const boost::system::error_code& error) {
        if (racer.error) {
            return;
        }
        const std::string address =
            join_host_port(endpoint.address().to_string(), std::to_string(endpoint.port()));
        racer.error = Error::wrap("dial tcp " + address + ": connect", describe(error));
    }

    void finish() {
        if (finished_) {
            return;
        }
        finished_ = true;
        if (done_) {
            done_();
        }
    }

    asio::io_context& io_;
    asio::steady_timer fallback_timer_;
    std::vector<tcp::endpoint> primary_;
    std::vector<tcp::endpoint> fallback_;
    Racer racers_[2];
    std::optional<tcp::socket> winner_;
    OpRunner::Done done_;
    int pending_ = 0;
    bool finished_ = false;
    bool aborted_ = false;
};

std::string alpn_wire_format(const std::string& protocol) {
    std::string wire;
    wire.push_back(static_cast<char>(protocol.size()));
    wire += protocol;
    return wire;
}

} // namespace

tcp::socket& lowest_layer(AnyStream& stream) {
    return std::visit(
        [](auto& value) -> tcp::socket& { return boost::beast::get_lowest_layer(value); }, stream);
}

void close_stream(AnyStream& stream) noexcept {
    boost::system::error_code ignored;
    lowest_layer(stream).close(ignored);
}

OpRunner::Outcome OpRunner::run(const Context& ctx, std::optional<Duration> timeout,
                                const std::function<void(Done)>& start,
                                const std::function<void()>& abort) {
    struct State {
        bool done = false;
        bool canceled = false;
        bool timed_out = false;
    };
    if (ctx.done()) {
        return Outcome::canceled;
    }
    auto state = std::make_shared<State>();
    std::optional<asio::steady_timer> timer;
    if (timeout) {
        timer.emplace(io_, *timeout);
        timer->async_wait([state, abort](const boost::system::error_code& error) {
            if (error || state->done || state->canceled) {
                return;
            }
            state->timed_out = true;
            abort();
        });
    }
    const ContextCallback registration = ctx.on_done([this, state, abort] {
        asio::post(io_, [state, abort] {
            if (state->done || state->timed_out || state->canceled) {
                return;
            }
            state->canceled = true;
            abort();
        });
    });

    asio::steady_timer* timer_pointer = timer ? &*timer : nullptr;
    start([state, timer_pointer] {
        state->done = true;
        if (timer_pointer != nullptr) {
            timer_pointer->cancel();
        }
    });
    io_.restart();
    while (!state->done) {
        if (io_.run_one() == 0) {
            break;
        }
    }
    if (state->canceled) {
        return Outcome::canceled;
    }
    if (state->timed_out) {
        return Outcome::timed_out;
    }
    return Outcome::completed;
}

Result<std::vector<tcp::endpoint>> resolve(const Context& ctx, const std::string& host,
                                           const std::string& port) {
    const auto parsed_port = text::atoi(port);
    if (!parsed_port.value || *parsed_port.value < 0 || *parsed_port.value > 65535) {
        return network_error("dial tcp: invalid port " + text::quote(port));
    }
    const auto port_number = static_cast<unsigned short>(*parsed_port.value);
    if (const auto literal = IpAddress::parse(host); literal && literal->zone().empty()) {
        boost::system::error_code error;
        const auto address = asio::ip::make_address(literal->to_string(), error);
        if (!error) {
            return std::vector<tcp::endpoint>{tcp::endpoint(address, port_number)};
        }
    }
    if (ctx.done()) {
        return ctx.err();
    }

    struct Shared {
        std::mutex mutex;
        std::condition_variable changed;
        bool done = false;
        std::vector<tcp::endpoint> endpoints;
        boost::system::error_code error;
    };
    auto shared = std::make_shared<Shared>();
    try {
        std::thread([shared, host, port] {
            asio::io_context local;
            tcp::resolver resolver(local);
            boost::system::error_code error;
            const auto results = resolver.resolve(host, port, error);
            std::vector<tcp::endpoint> endpoints;
            if (!error) {
                for (const auto& entry : results) {
                    endpoints.push_back(entry.endpoint());
                }
            }
            {
                std::lock_guard lock(shared->mutex);
                shared->endpoints = std::move(endpoints);
                shared->error = error;
                shared->done = true;
            }
            shared->changed.notify_all();
        }).detach();
    } catch (const std::system_error& error) {
        return network_error("lookup " + host + ": " + error.what());
    }

    const ContextCallback registration = ctx.on_done([shared] {
        std::lock_guard lock(shared->mutex);
        shared->changed.notify_all();
    });
    std::unique_lock lock(shared->mutex);
    shared->changed.wait(lock, [&] { return shared->done || ctx.done(); });
    if (!shared->done) {
        return ctx.err();
    }
    if (shared->error) {
        const std::string reason = shared->error == asio::error::host_not_found
                                       ? std::string("no such host")
                                       : shared->error.message();
        return network_error("lookup " + host + ": " + reason);
    }
    if (shared->endpoints.empty()) {
        return network_error("lookup " + host + ": no such host");
    }
    return std::move(shared->endpoints);
}

Result<AnyStream> establish(asio::io_context& io, OpRunner& runner, const Context& ctx,
                            const Url& url, const ConnectOptions& options) {
    if (ctx.done()) {
        return ctx.err();
    }
    const std::string host = url.hostname();
    const std::string port = effective_port(url);
    if (host.empty()) {
        return network_error("dial tcp: missing host");
    }
    auto endpoints = resolve(ctx, host, port);
    if (!endpoints) {
        return std::move(endpoints).error();
    }

    auto race = std::make_shared<ConnectRace>(io, *endpoints);
    const auto outcome = runner.run(
        ctx,
        options.connect_timeout > Duration::zero() ? std::optional(options.connect_timeout)
                                                   : std::nullopt,
        [race](OpRunner::Done done) { race->start(std::move(done)); }, [race] { race->abort(); });
    if (outcome == OpRunner::Outcome::canceled || (!race->winner() && ctx.done())) {
        return ctx.err() ? ctx.err() : errors::canceled();
    }
    if (outcome == OpRunner::Outcome::timed_out) {
        return network_error("dial tcp " + join_host_port(host, port) + ": i/o timeout", true);
    }
    if (!race->winner()) {
        return race->error();
    }
    tcp::socket socket = std::move(*race->winner());
    race->winner().reset();
    boost::system::error_code ignored;
    socket.set_option(tcp::no_delay(true), ignored);
    socket.set_option(asio::socket_base::keep_alive(true), ignored);
    if (!uses_tls(url)) {
        return AnyStream(std::in_place_index<0>, std::move(socket));
    }

    auto tls = options.tls ? options.tls : TlsContext::system();
    AnyStream stream(std::in_place_index<1>, std::move(socket), tls->native());
    auto& tls_stream = std::get<1>(stream);
    SSL* ssl = tls_stream.native_handle();
    if (IpAddress::parse(host)) {
        X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(ssl), host.c_str());
    } else {
        SSL_set_tlsext_host_name(ssl, host.c_str());
        SSL_set1_host(ssl, host.c_str());
    }
    SSL_set_verify(ssl, SSL_VERIFY_PEER, nullptr);
    if (!options.alpn.empty()) {
        const std::string wire = alpn_wire_format(options.alpn);
        SSL_set_alpn_protos(ssl, reinterpret_cast<const unsigned char*>(wire.data()),
                            static_cast<unsigned>(wire.size()));
    }

    boost::system::error_code result;
    const auto handshake = runner.run(
        ctx,
        options.tls_handshake_timeout > Duration::zero()
            ? std::optional(options.tls_handshake_timeout)
            : std::nullopt,
        [&tls_stream, &result](OpRunner::Done done) {
            tls_stream.async_handshake(TlsStream::client,
                                       [&result, done](const boost::system::error_code& error) {
                                           result = error;
                                           done();
                                       });
        },
        [&stream] { close_stream(stream); });
    if (handshake == OpRunner::Outcome::canceled || (result && ctx.done())) {
        close_stream(stream);
        return ctx.err() ? ctx.err() : errors::canceled();
    }
    if (handshake == OpRunner::Outcome::timed_out) {
        close_stream(stream);
        return network_error("net/http: TLS handshake timeout", true);
    }
    if (result) {
        const long verify = SSL_get_verify_result(ssl);
        close_stream(stream);
        if (verify != X509_V_OK) {
            return network_error(std::string("tls: failed to verify certificate: ") +
                                 X509_verify_cert_error_string(verify));
        }
        return Error::wrap("tls", describe(result));
    }
    return stream;
}

std::string effective_port(const Url& url) {
    std::string port = url.port();
    if (!port.empty()) {
        return port;
    }
    return uses_tls(url) ? "443" : "80";
}

bool uses_tls(const Url& url) {
    return url.scheme == "https" || url.scheme == "wss";
}

Error network_error(std::string message, bool timeout) {
    return Error::with_detail(std::make_shared<NetworkError>(timeout), std::move(message));
}

Error describe(const boost::system::error_code& code) {
    if (code == asio::error::eof || code == boost::beast::http::error::end_of_stream) {
        return network_error("EOF");
    }
    if (code == boost::beast::http::error::partial_message ||
        code == asio::ssl::error::stream_truncated) {
        return network_error("unexpected EOF");
    }
    if (code == asio::error::connection_reset) {
        return network_error("connection reset by peer");
    }
    if (code == asio::error::broken_pipe) {
        return network_error("broken pipe");
    }
    if (code == asio::error::connection_refused) {
        return network_error("connection refused");
    }
    if (code == asio::error::timed_out) {
        return network_error("i/o timeout", true);
    }
    if (code == boost::beast::websocket::error::message_too_big) {
        return network_error("websocket: read limit exceeded");
    }
    return network_error(code.message());
}

Error outcome_error(const Context& ctx, OpRunner::Outcome outcome,
                    const boost::system::error_code& code, std::string_view timeout_message) {
    if (outcome == OpRunner::Outcome::canceled || (code && ctx.done())) {
        return ctx.err() ? ctx.err() : errors::canceled();
    }
    if (outcome == OpRunner::Outcome::timed_out) {
        return network_error(std::string(timeout_message), true);
    }
    if (code) {
        return describe(code);
    }
    return {};
}

} // namespace puls::net::detail
