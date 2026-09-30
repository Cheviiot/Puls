#pragma once

#include "puls/core/context.hpp"
#include "puls/core/error.hpp"
#include "puls/net/http.hpp"

#include <boost/json/value.hpp>

#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

// HTTP helpers shared by the first-party service protocols.
namespace puls::service {

// Identifies Puls to measurement services.
inline constexpr std::string_view user_agent =
    "Mozilla/5.0 (compatible; Puls; +https://github.com/Cheviiot/Puls)";

// Reads at most limit bytes and rejects oversized bodies instead of silently
// accepting their prefix.
Result<std::string> read_limited(const Context& ctx, net::HttpResponse& response,
                                 std::size_t limit);
// Validates the size and requires exactly one JSON value.
Result<boost::json::value> decode_json_limited(const Context& ctx, net::HttpResponse& response,
                                               std::size_t limit);
// Checks the media type of the response, ignoring parameters such as
// charset, with the syntax rules of Go's mime.ParseMediaType.
Error validate_content_type(const net::HttpResponse& response,
                            std::initializer_list<std::string_view> expected);
// Parses a Content-Type value; returns the lower-case media type or nothing
// when the value is malformed.
std::optional<std::string> parse_media_type(std::string_view value);
// Releases a response for connection reuse without reading an unbounded
// error body.
void drain_and_close(const Context& ctx, net::HttpResponse& response);

} // namespace puls::service
