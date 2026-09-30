#include "puls/core/ip.hpp"

#include <algorithm>

namespace puls {

namespace {

bool parse_v4_fields(std::string_view text, std::uint8_t* fields) {
    int value = 0;
    int position = 0;
    int digits = 0;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c >= '0' && c <= '9') {
            if (digits == 1 && value == 0) {
                return false;
            }
            value = value * 10 + (c - '0');
            ++digits;
            if (value > 255) {
                return false;
            }
        } else if (c == '.') {
            if (index == 0 || index == text.size() - 1 || text[index - 1] == '.') {
                return false;
            }
            if (position == 3) {
                return false;
            }
            fields[position++] = static_cast<std::uint8_t>(value);
            value = 0;
            digits = 0;
        } else {
            return false;
        }
    }
    if (position < 3) {
        return false;
    }
    fields[3] = static_cast<std::uint8_t>(value);
    return true;
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

void append_hex16(std::string& output, unsigned value) {
    static constexpr char digits[] = "0123456789abcdef";
    bool started = false;
    for (int shift = 12; shift >= 0; shift -= 4) {
        const unsigned digit = (value >> shift) & 0xF;
        if (digit != 0 || started || shift == 0) {
            output.push_back(digits[digit]);
            started = true;
        }
    }
}

} // namespace

std::optional<IpAddress> IpAddress::parse(std::string_view text) {
    for (const char c : text) {
        if (c == '.') {
            IpAddress address;
            address.family_ = Family::v4;
            if (!parse_v4_fields(text, address.bytes_.data())) {
                return std::nullopt;
            }
            return address;
        }
        if (c == ':') {
            break;
        }
        if (c == '%') {
            return std::nullopt;
        }
    }
    if (text.find(':') == std::string_view::npos) {
        return std::nullopt;
    }

    std::string_view rest = text;
    std::string zone;
    if (const auto percent = rest.find('%'); percent != std::string_view::npos) {
        zone = std::string(rest.substr(percent + 1));
        rest = rest.substr(0, percent);
        if (zone.empty()) {
            return std::nullopt;
        }
    }

    std::array<std::uint8_t, 16> bytes{};
    int ellipsis = -1;
    if (rest.size() >= 2 && rest[0] == ':' && rest[1] == ':') {
        ellipsis = 0;
        rest.remove_prefix(2);
        if (rest.empty()) {
            IpAddress address;
            address.family_ = Family::v6;
            address.zone_ = std::move(zone);
            return address;
        }
    }

    int index = 0;
    while (index < 16) {
        std::size_t offset = 0;
        unsigned accumulator = 0;
        for (; offset < rest.size(); ++offset) {
            const int digit = hex_value(rest[offset]);
            if (digit < 0) {
                break;
            }
            accumulator = (accumulator << 4) + static_cast<unsigned>(digit);
            if (offset > 3 || accumulator > 0xFFFF) {
                return std::nullopt;
            }
        }
        if (offset == 0) {
            return std::nullopt;
        }
        if (offset < rest.size() && rest[offset] == '.') {
            if ((ellipsis < 0 && index != 12) || index + 4 > 16) {
                return std::nullopt;
            }
            if (!parse_v4_fields(rest, bytes.data() + index)) {
                return std::nullopt;
            }
            rest = {};
            index += 4;
            break;
        }
        bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(accumulator >> 8);
        bytes[static_cast<std::size_t>(index) + 1] = static_cast<std::uint8_t>(accumulator);
        index += 2;
        rest.remove_prefix(offset);
        if (rest.empty()) {
            break;
        }
        if (rest[0] != ':' || rest.size() == 1) {
            return std::nullopt;
        }
        rest.remove_prefix(1);
        if (rest[0] == ':') {
            if (ellipsis >= 0) {
                return std::nullopt;
            }
            ellipsis = index;
            rest.remove_prefix(1);
            if (rest.empty()) {
                break;
            }
        }
    }
    if (!rest.empty()) {
        return std::nullopt;
    }
    if (index < 16) {
        if (ellipsis < 0) {
            return std::nullopt;
        }
        const int shift = 16 - index;
        for (int position = index - 1; position >= ellipsis; --position) {
            bytes[static_cast<std::size_t>(position + shift)] =
                bytes[static_cast<std::size_t>(position)];
        }
        std::fill(bytes.begin() + ellipsis, bytes.begin() + ellipsis + shift, std::uint8_t{0});
    } else if (ellipsis >= 0) {
        return std::nullopt;
    }

    IpAddress address;
    address.family_ = Family::v6;
    address.bytes_ = bytes;
    address.zone_ = std::move(zone);
    return address;
}

IpAddress IpAddress::unmap() const {
    if (family_ != Family::v6) {
        return *this;
    }
    for (std::size_t index = 0; index < 10; ++index) {
        if (bytes_[index] != 0) {
            return *this;
        }
    }
    if (bytes_[10] != 0xFF || bytes_[11] != 0xFF) {
        return *this;
    }
    IpAddress address;
    address.family_ = Family::v4;
    std::copy(bytes_.begin() + 12, bytes_.end(), address.bytes_.begin());
    return address;
}

std::string IpAddress::to_string() const {
    std::string result;
    const auto append_v4 = [&result](const std::uint8_t* octets) {
        for (int index = 0; index < 4; ++index) {
            if (index > 0) {
                result.push_back('.');
            }
            result += std::to_string(octets[index]);
        }
    };
    switch (family_) {
    case Family::none:
        return "invalid IP";
    case Family::v4:
        append_v4(bytes_.data());
        return result;
    case Family::v6:
        break;
    }

    if (unmap().is_v4()) {
        result = "::ffff:";
        append_v4(bytes_.data() + 12);
    } else {
        const auto group = [this](int index) {
            return (static_cast<unsigned>(bytes_[static_cast<std::size_t>(index) * 2]) << 8) |
                   bytes_[static_cast<std::size_t>(index) * 2 + 1];
        };
        int zero_start = 255;
        int zero_end = 255;
        for (int index = 0; index < 8; ++index) {
            int end = index;
            while (end < 8 && group(end) == 0) {
                ++end;
            }
            if (const int length = end - index; length >= 2 && length > zero_end - zero_start) {
                zero_start = index;
                zero_end = end;
            }
        }
        for (int index = 0; index < 8; ++index) {
            if (index == zero_start) {
                result += "::";
                index = zero_end;
                if (index >= 8) {
                    break;
                }
            } else if (index > 0) {
                result.push_back(':');
            }
            append_hex16(result, group(index));
        }
    }
    if (!zone_.empty()) {
        result.push_back('%');
        result += zone_;
    }
    return result;
}

} // namespace puls
