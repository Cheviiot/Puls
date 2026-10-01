// Verifies that the platform certificate store is usable. Built only with
// PULS_SYSTEM_TLS_TESTS=ON because it needs direct access to the Internet.

#include "puls/net/http.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace puls::net {
namespace {

TEST(SystemTls, VerifiesPublicCertificateChain) {
    const CancelScope scope(Context(), std::chrono::seconds(30));
    HttpClientOptions options;
    options.tls = TlsContext::system();
    HttpSession session(options);
    HttpRequest request;
    request.url = *Url::parse("https://github.com/");
    request.headers = {{"User-Agent", "Puls TLS smoke test"}};
    auto response = session.send(scope.context(), request);
    ASSERT_TRUE(response) << response.error().message();
    EXPECT_GE(response->status_code(), 200);
    EXPECT_LT(response->status_code(), 500);
}

} // namespace
} // namespace puls::net
