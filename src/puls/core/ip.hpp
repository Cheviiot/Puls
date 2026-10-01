#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace puls {

// IpAddress is a strict IPv4/IPv6 address value mirroring Go's netip.Addr:
// IPv4 octets may not have leading zeros and IPv6 zones are preserved so that
// callers can reject them.
class IpAddress {
public:
    IpAddress() = default;

    static std::optional<IpAddress> parse(std::string_view text);

    [[nodiscard]] bool is_v4() const noexcept { return family_ == Family::v4; }
    [[nodiscard]] bool is_v6() const noexcept { return family_ == Family::v6; }
    [[nodiscard]] bool valid() const noexcept { return family_ != Family::none; }
    [[nodiscard]] const std::string& zone() const noexcept { return zone_; }
    // Converts an IPv4-mapped IPv6 address (::ffff:a.b.c.d) to IPv4.
    [[nodiscard]] IpAddress unmap() const;
    // Canonical text (RFC 5952 for IPv6, dotted decimal for IPv4).
    [[nodiscard]] std::string to_string() const;

    friend bool operator==(const IpAddress& left, const IpAddress& right) noexcept {
        return left.family_ == right.family_ && left.bytes_ == right.bytes_ &&
               left.zone_ == right.zone_;
    }

private:
    enum class Family : std::uint8_t { none, v4, v6 };

    Family family_ = Family::none;
    std::array<std::uint8_t, 16> bytes_{};
    std::string zone_;
};

} // namespace puls
