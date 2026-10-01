#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Protocol limits of Яндекс.Интернетометр shared by the backend sources.
namespace puls::yandex::detail {

inline constexpr std::size_t upload_chunk_size = 2 * 1024 * 1024;
inline constexpr std::size_t websocket_chunk_size = 64 << 10;
inline constexpr std::uint64_t expected_download_size = 50 << 20;
inline constexpr std::size_t max_discovery_body_size = 2 << 20;
inline constexpr std::uint64_t max_ping_body_size = 64 << 10;
inline constexpr std::uint64_t max_download_body_size = 1 << 30;
inline constexpr std::uint64_t max_upload_response = 1 << 20;
inline constexpr std::size_t max_websocket_ack_size = 4 << 10;
inline constexpr std::size_t max_internet_page_size = 8 << 20;
inline constexpr std::string_view client_state_marker = "Client.default(";
inline constexpr std::string_view origin = "https://yandex.ru";

} // namespace puls::yandex::detail
