#pragma once

#include "puls/service/speedtestru/backend.hpp"

#include <chrono>
#include <cstddef>

// Protocol limits and shared templates of the speedtest.ru backend.
namespace puls::speedtestru {

namespace detail {

inline constexpr std::size_t upload_block_size = 2 * 1024 * 1024;
inline constexpr std::size_t max_discovery_bytes = 4 << 20;
inline constexpr std::size_t max_page_bytes = 8 << 20;
inline constexpr std::size_t max_jwt_bytes = 1 << 20;
inline constexpr std::size_t max_connection_bytes = 64 << 10;
inline constexpr std::size_t max_isp_name_bytes = 255;
inline constexpr std::uint64_t max_upload_response = 1 << 20;
inline constexpr std::chrono::seconds api_request_timeout{15};
inline constexpr std::size_t websocket_read_limit = 64 << 10;
inline constexpr int ping_samples = 10;

bool is_auth_status(int status);
// Milliseconds since the Unix epoch, used as the t= cache-busting parameter.
std::string unix_millis();
// strconv.FormatInt(time.Now().UnixNano(), 36).
std::string unix_nanos_base36();

} // namespace detail

template <class T, class FetchFunction>
Result<T> Backend::with_browser_key(const Context& ctx, FetchFunction fetch) {
    const std::string key = current_browser_key();
    Fetch<T> first = fetch(ctx, key);
    if (!first.error) {
        return std::move(first.value);
    }
    if (!detail::is_auth_status(first.status)) {
        return first.error;
    }
    auto refreshed = refresh_browser_key(ctx, key);
    if (!refreshed) {
        return Error::join({first.error, refreshed.error()});
    }
    Fetch<T> second = fetch(ctx, *refreshed);
    if (second.error) {
        return second.error;
    }
    return std::move(second.value);
}

} // namespace puls::speedtestru
