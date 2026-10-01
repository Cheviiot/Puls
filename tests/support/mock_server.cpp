#include "support/mock_server.hpp"

#include "puls/core/text.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <stdexcept>

namespace puls::testing {

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
using tcp = asio::ip::tcp;

// A connection socket that only MockServer closes. Stopping the server shuts
// connections down from another thread, which must not race with a close by
// the connection thread.
class ServerSocket : public tcp::socket {
public:
    using tcp::socket::basic_stream_socket;
};

// Beast's WebSocket teardown for TCP sockets without the final close; found
// by argument-dependent lookup for plain and TLS connections.
void teardown(beast::role_type role, ServerSocket& socket, boost::system::error_code& error) {
    if (role == beast::role_type::server) {
        socket.shutdown(tcp::socket::shutdown_send, error);
        if (error) {
            return;
        }
    }
    char buffer[2048];
    for (;;) {
        const std::size_t count = socket.read_some(asio::buffer(buffer), error);
        if (error) {
            if (error == asio::error::eof) {
                error = {};
            }
            return;
        }
        if (count == 0) {
            return;
        }
    }
}

struct TestCertificate {
    std::string certificate;
    std::string key;
};

std::string bio_to_string(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}

void add_extension(X509* certificate, int nid, const char* value) {
    X509V3_CTX context;
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    X509_EXTENSION* extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value);
    if (extension == nullptr) {
        throw std::runtime_error("cannot create certificate extension");
    }
    X509_add_ext(certificate, extension, -1);
    X509_EXTENSION_free(extension);
}

TestCertificate generate_certificate(const char* common_name, const char* subject_alt_names) {
    EVP_PKEY* key = EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "P-256");
    X509* certificate = X509_new();
    if (key == nullptr || certificate == nullptr) {
        throw std::runtime_error("cannot generate test certificate");
    }
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 20260930);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -3600);
    X509_gmtime_adj(X509_getm_notAfter(certificate), 7 * 24 * 3600);
    X509_set_pubkey(certificate, key);
    X509_NAME* name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0);
    X509_set_issuer_name(certificate, name);
    add_extension(certificate, NID_basic_constraints, "critical,CA:TRUE");
    add_extension(certificate, NID_key_usage, "critical,digitalSignature,keyCertSign");
    add_extension(certificate, NID_ext_key_usage, "serverAuth");
    add_extension(certificate, NID_subject_alt_name, subject_alt_names);
    add_extension(certificate, NID_subject_key_identifier, "hash");
    if (X509_sign(certificate, key, EVP_sha256()) == 0) {
        throw std::runtime_error("cannot sign test certificate");
    }

    TestCertificate result;
    BIO* certificate_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(certificate_bio, certificate);
    result.certificate = bio_to_string(certificate_bio);
    BIO_free(certificate_bio);
    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(key_bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    result.key = bio_to_string(key_bio);
    BIO_free(key_bio);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return result;
}

const TestCertificate& certificate() {
    static const TestCertificate value =
        generate_certificate("Puls test", "IP:127.0.0.1,IP:::1,DNS:localhost");
    return value;
}

const TestCertificate& wrong_name_certificate() {
    static const TestCertificate value =
        generate_certificate("Puls wrong name test", "DNS:puls.invalid");
    return value;
}

std::string reason_phrase(int status) {
    const auto reason = http::obsolete_reason(static_cast<http::status>(status));
    return std::string(reason.data(), reason.size());
}

template <class Stream>
class BasicWebSocket final : public MockWebSocket {
public:
    BasicWebSocket(Stream& stream, const http::request<http::string_body>& request)
        : stream_(stream) {
        stream_.read_message_max(64 << 20);
        stream_.auto_fragment(false);
        stream_.accept(request, error_);
    }

    [[nodiscard]] bool accepted() const { return !error_; }

    std::optional<MockMessage> read() override {
        beast::flat_buffer buffer;
        boost::system::error_code error;
        stream_.read(buffer, error);
        if (error) {
            return std::nullopt;
        }
        return MockMessage{stream_.got_text(), beast::buffers_to_string(buffer.data())};
    }

    bool write_text(std::string_view payload) override { return write(true, payload); }
    bool write_binary(std::string_view payload) override { return write(false, payload); }

    void close(std::uint16_t code) override {
        boost::system::error_code error;
        stream_.close(websocket::close_reason(code), error);
    }

private:
    bool write(bool text, std::string_view payload) {
        boost::system::error_code error;
        stream_.text(text);
        stream_.write(asio::buffer(payload.data(), payload.size()), error);
        return !error;
    }

    websocket::stream<Stream&> stream_;
    boost::system::error_code error_;
};

template <class Stream>
class BasicExchange final : public MockExchange {
public:
    BasicExchange(Stream& stream, MockRequest request, const http::request<http::string_body>& raw)
        : stream_(stream), request_(std::move(request)), raw_(raw) {}

    const MockRequest& request() const override { return request_; }

    void respond(int status, const Headers& headers, std::string_view body) override {
        std::string head =
            "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
        bool framed = false;
        for (const auto& [name, value] : headers) {
            head += name + ": " + value + "\r\n";
            framed = framed || text::equal_fold_ascii(name, "Content-Length") ||
                     text::equal_fold_ascii(name, "Transfer-Encoding");
        }
        if (!framed) {
            head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        }
        head += "\r\n";
        write_raw(head);
        write_raw(body);
    }

    void respond_chunked(int status, const Headers& headers, std::string_view body) override {
        std::string head =
            "HTTP/1.1 " + std::to_string(status) + " " + reason_phrase(status) + "\r\n";
        for (const auto& [name, value] : headers) {
            head += name + ": " + value + "\r\n";
        }
        head += "Transfer-Encoding: chunked\r\n\r\n";
        write_raw(head);
        constexpr std::size_t chunk_size = 32 << 10;
        for (std::size_t offset = 0; offset < body.size(); offset += chunk_size) {
            const std::string_view chunk = body.substr(offset, chunk_size);
            write_raw(text::format_uint(chunk.size(), 16) + "\r\n");
            write_raw(chunk);
            write_raw("\r\n");
        }
        write_raw("0\r\n\r\n");
    }

    void write_raw(std::string_view bytes) override {
        responded_ = true;
        if (failed_ || bytes.empty()) {
            return;
        }
        boost::system::error_code error;
        asio::write(stream_, asio::buffer(bytes.data(), bytes.size()), error);
        failed_ = static_cast<bool>(error);
    }

    void close() override { closed_ = true; }

    MockWebSocket& websocket() override {
        if (!websocket_) {
            websocket_ = std::make_unique<BasicWebSocket<Stream>>(stream_, raw_);
        }
        return *websocket_;
    }

    [[nodiscard]] bool responded() const { return responded_; }
    [[nodiscard]] bool closed() const { return closed_ || failed_ || websocket_ != nullptr; }

private:
    Stream& stream_;
    MockRequest request_;
    const http::request<http::string_body>& raw_;
    std::unique_ptr<BasicWebSocket<Stream>> websocket_;
    bool responded_ = false;
    bool closed_ = false;
    bool failed_ = false;
};

MockRequest convert(const http::request<http::string_body>& raw) {
    MockRequest request;
    request.method = std::string(raw.method_string().data(), raw.method_string().size());
    request.target = std::string(raw.target().data(), raw.target().size());
    const auto question = request.target.find('?');
    request.path = request.target.substr(0, question);
    if (question != std::string::npos) {
        request.query = request.target.substr(question + 1);
    }
    for (const auto& field : raw) {
        request.headers.emplace_back(
            std::string(field.name_string().data(), field.name_string().size()),
            std::string(field.value().data(), field.value().size()));
    }
    request.body = raw.body();
    request.websocket_upgrade = websocket::is_upgrade(raw);
    return request;
}

template <class Stream>
void serve(Stream& stream, const MockServer::Handler& handler) {
    beast::flat_buffer buffer;
    for (;;) {
        http::request_parser<http::string_body> parser;
        parser.body_limit(256 << 20);
        parser.header_limit(1 << 20);
        boost::system::error_code error;
        http::read(stream, buffer, parser, error);
        if (error) {
            return;
        }
        http::request<http::string_body> raw = parser.release();
        BasicExchange<Stream> exchange(stream, convert(raw), raw);
        try {
            handler(exchange);
        } catch (...) {
            if (!exchange.responded()) {
                exchange.respond(500, {}, "handler failed");
            }
            return;
        }
        if (exchange.closed()) {
            return;
        }
        if (!exchange.responded()) {
            exchange.respond(200, {}, {});
        }
        if (!raw.keep_alive()) {
            return;
        }
    }
}

} // namespace

std::string MockRequest::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (text::equal_fold_ascii(key, name)) {
            return value;
        }
    }
    return {};
}

std::string MockRequest::query_value(std::string_view key) const {
    std::string_view rest = query;
    while (!rest.empty()) {
        const auto ampersand = rest.find('&');
        const std::string_view pair = rest.substr(0, ampersand);
        rest =
            ampersand == std::string_view::npos ? std::string_view() : rest.substr(ampersand + 1);
        const auto equals = pair.find('=');
        if (pair.substr(0, equals) == key) {
            return equals == std::string_view::npos ? std::string()
                                                    : std::string(pair.substr(equals + 1));
        }
    }
    return {};
}

struct MockServer::Impl {
    struct Connection {
        explicit Connection(asio::io_context& io) : socket(io) {}
        ServerSocket socket;
        std::thread thread;
    };

    asio::io_context io;
    tcp::acceptor acceptor{io};
    std::optional<asio::ssl::context> tls;
    Handler handler;
    std::thread accept_thread;
    std::atomic<bool> stopping{false};
    std::mutex mutex;
    std::vector<std::unique_ptr<Connection>> connections;

    void serve_connection(ServerSocket& socket) {
        if (tls) {
            asio::ssl::stream<ServerSocket&> stream(socket, *tls);
            boost::system::error_code error;
            stream.handshake(asio::ssl::stream_base::server, error);
            if (!error) {
                serve(stream, handler);
            }
        } else {
            serve(socket, handler);
        }
        boost::system::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
};

MockServer::MockServer(Handler handler, Transport transport)
    : impl_(std::make_unique<Impl>()), tls_(transport != Transport::plain) {
    impl_->handler = std::move(handler);
    if (tls_) {
        const TestCertificate& served =
            transport == Transport::tls_wrong_name ? wrong_name_certificate() : certificate();
        impl_->tls.emplace(asio::ssl::context::tls_server);
        impl_->tls->use_certificate_chain(
            asio::buffer(served.certificate.data(), served.certificate.size()));
        impl_->tls->use_private_key(asio::buffer(served.key.data(), served.key.size()),
                                    asio::ssl::context::pem);
    }
    const tcp::endpoint endpoint(asio::ip::make_address("127.0.0.1"), 0);
    impl_->acceptor.open(endpoint.protocol());
    impl_->acceptor.set_option(tcp::acceptor::reuse_address(true));
    impl_->acceptor.bind(endpoint);
    impl_->acceptor.listen();
    port_ = impl_->acceptor.local_endpoint().port();

    impl_->accept_thread = std::thread([this] {
        Impl& impl = *impl_;
        for (;;) {
            auto connection = std::make_unique<Impl::Connection>(impl.io);
            boost::system::error_code error;
            impl.acceptor.accept(connection->socket, error);
            if (impl.stopping.load()) {
                return;
            }
            if (error) {
                continue;
            }
            accepted_.fetch_add(1);
            boost::system::error_code ignored;
            connection->socket.set_option(tcp::no_delay(true), ignored);
            Impl::Connection* raw = connection.get();
            std::lock_guard lock(impl.mutex);
            impl.connections.push_back(std::move(connection));
            raw->thread = std::thread([&impl, raw] { impl.serve_connection(raw->socket); });
        }
    });
}

MockServer::~MockServer() {
    impl_->stopping.store(true);
    {
        asio::io_context wake;
        tcp::socket socket(wake);
        boost::system::error_code ignored;
        socket.connect(tcp::endpoint(asio::ip::make_address("127.0.0.1"), port_), ignored);
    }
    impl_->accept_thread.join();
    std::lock_guard lock(impl_->mutex);
    for (auto& connection : impl_->connections) {
        boost::system::error_code ignored;
        connection->socket.shutdown(tcp::socket::shutdown_both, ignored);
    }
    for (auto& connection : impl_->connections) {
        if (connection->thread.joinable()) {
            connection->thread.join();
        }
        boost::system::error_code ignored;
        connection->socket.close(ignored);
    }
    boost::system::error_code ignored;
    impl_->acceptor.close(ignored);
}

std::string MockServer::host() const {
    return "127.0.0.1:" + std::to_string(port_);
}

std::string MockServer::url(std::string_view path) const {
    return (tls_ ? "https://" : "http://") + host() + std::string(path);
}

std::string MockServer::ws_url(std::string_view path) const {
    return (tls_ ? "wss://" : "ws://") + host() + std::string(path);
}

std::shared_ptr<net::TlsContext> test_tls_context() {
    static const std::shared_ptr<net::TlsContext> context = [] {
        net::TlsOptions options;
        options.use_system_roots = false;
        options.extra_root_certificates.push_back(certificate().certificate);
        options.extra_root_certificates.push_back(wrong_name_certificate().certificate);
        auto created = net::TlsContext::create(options);
        if (!created) {
            throw std::runtime_error(created.error().message());
        }
        return std::move(created).value();
    }();
    return context;
}

const std::string& test_certificate_pem() {
    return certificate().certificate;
}

const std::string& test_private_key_pem() {
    return certificate().key;
}

} // namespace puls::testing
