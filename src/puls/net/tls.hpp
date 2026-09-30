#pragma once

#include "puls/core/error.hpp"

#include <boost/asio/ssl/context.hpp>

#include <memory>
#include <string>
#include <vector>

namespace puls::net {

struct TlsOptions {
    // Trust the platform certificate store (Windows ROOT store, macOS
    // keychains, well-known bundles elsewhere; SSL_CERT_FILE and SSL_CERT_DIR
    // override the Unix locations like in Go).
    bool use_system_roots = true;
    // Additional PEM trust anchors, used by tests with local servers.
    std::vector<std::string> extra_root_certificates;
};

// TlsContext is an immutable client TLS configuration: TLS 1.2 or newer,
// certificate and host name verification. It is safe to share between
// threads once created.
class TlsContext {
public:
    static Result<std::shared_ptr<TlsContext>> create(const TlsOptions& options);
    // Process-wide context trusting the system store.
    static std::shared_ptr<TlsContext> system();

    boost::asio::ssl::context& native() noexcept { return context_; }

private:
    TlsContext();

    boost::asio::ssl::context context_;
};

} // namespace puls::net
