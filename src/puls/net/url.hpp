#pragma once

#include "puls/core/error.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace puls::net {

// Url follows the parsing rules of Go's net/url for the subset Puls needs:
// absolute http(s)/ws(s) URLs and relative references in HTML pages.
struct Url {
    std::string scheme;    // lower case
    std::string opaque;    // e.g. "mailto:x" has opaque "x"
    bool has_user = false; // authority contains userinfo
    std::string host;      // host or host:port, IPv6 literals keep brackets
    std::string path;      // decoded path
    std::string raw_path;  // original encoding when it differs from the default one
    bool force_query = false;
    std::string raw_query;
    std::string fragment;

    static Result<Url> parse(std::string_view raw);

    // Host without port and IPv6 brackets.
    [[nodiscard]] std::string hostname() const;
    // Port text without the colon, or empty.
    [[nodiscard]] std::string port() const;
    [[nodiscard]] std::string escaped_path() const;
    // Path and query as sent in an HTTP request line.
    [[nodiscard]] std::string request_uri() const;
    [[nodiscard]] std::string to_string() const;
    // RFC 3986 reference resolution (URL.ResolveReference).
    [[nodiscard]] Url resolve_reference(const Url& reference) const;
};

// url.QueryEscape / url.QueryUnescape.
std::string query_escape(std::string_view text);
Result<std::string> query_unescape(std::string_view text);

// Parses a query string like url.ParseQuery, skipping malformed pairs.
std::vector<std::pair<std::string, std::string>> parse_query(std::string_view raw_query);
// url.Values.Encode: keys sorted, values escaped.
std::string encode_query(std::vector<std::pair<std::string, std::string>> values);
// Replaces every value of key with value (url.Values.Set).
void set_query_value(std::vector<std::pair<std::string, std::string>>& values,
                     const std::string& key, const std::string& value);

// net.SplitHostPort and net.JoinHostPort.
Result<std::pair<std::string, std::string>> split_host_port(std::string_view host_port);
std::string join_host_port(std::string_view host, std::string_view port);

} // namespace puls::net
