#include "puls/core/text.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>

namespace puls::text {

namespace {

constexpr char32_t rune_error = 0xFFFD;
constexpr char lower_hex[] = "0123456789abcdef";

bool in_range(unsigned char value, unsigned char low, unsigned char high) noexcept {
    return value >= low && value <= high;
}

bool is_print(char32_t rune) noexcept {
    if (rune <= 0xFF) {
        if (rune >= 0x20 && rune <= 0x7E) {
            return true;
        }
        if (rune >= 0xA1 && rune <= 0xFF) {
            return rune != 0xAD;
        }
        return false;
    }
    // Approximates unicode.IsPrint: separators other than ASCII space, format
    // characters, private use and non-characters are not printable.
    struct Range {
        char32_t low;
        char32_t high;
    };
    static constexpr std::array<Range, 28> not_printable{{
        {0x0600, 0x0605},   {0x061C, 0x061C},   {0x06DD, 0x06DD},     {0x070F, 0x070F},
        {0x0890, 0x0891},   {0x08E2, 0x08E2},   {0x1680, 0x1680},     {0x180E, 0x180E},
        {0x2000, 0x200F},   {0x2028, 0x202F},   {0x205F, 0x2064},     {0x2066, 0x206F},
        {0x3000, 0x3000},   {0xD800, 0xDFFF},   {0xE000, 0xF8FF},     {0xFDD0, 0xFDEF},
        {0xFEFF, 0xFEFF},   {0xFFF9, 0xFFFB},   {0x110BD, 0x110BD},   {0x110CD, 0x110CD},
        {0x13430, 0x1343F}, {0x1BCA0, 0x1BCA3}, {0x1D173, 0x1D17A},   {0xE0001, 0xE0001},
        {0xE0020, 0xE007F}, {0xF0000, 0xFFFFD}, {0x100000, 0x10FFFD}, {0x110000, 0xFFFFFFFF},
    }};
    for (const auto& range : not_printable) {
        if (rune >= range.low && rune <= range.high) {
            return false;
        }
    }
    return (rune & 0xFFFE) != 0xFFFE;
}

void append_hex(std::string& output, std::uint32_t value, int digits) {
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        output.push_back(lower_hex[(value >> shift) & 0xF]);
    }
}

bool underscore_ok(std::string_view text) noexcept {
    char saw = '^';
    std::size_t index = 0;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        text.remove_prefix(1);
    }
    bool hex = false;
    if (text.size() >= 2 && text[0] == '0') {
        const char prefix = static_cast<char>(text[1] | 0x20);
        if (prefix == 'b' || prefix == 'o' || prefix == 'x') {
            index = 2;
            saw = '0';
            hex = prefix == 'x';
        }
    }
    for (; index < text.size(); ++index) {
        const char c = text[index];
        const char lower = static_cast<char>(c | 0x20);
        if ((c >= '0' && c <= '9') || (hex && lower >= 'a' && lower <= 'f')) {
            saw = '0';
            continue;
        }
        if (c == '_') {
            if (saw != '0') {
                return false;
            }
            saw = '_';
            continue;
        }
        if (saw == '_') {
            return false;
        }
        saw = '!';
    }
    return saw != '_';
}

struct ParsedUint {
    std::optional<std::uint64_t> value;
    ParseIntError error = ParseIntError::syntax;
};

ParsedUint parse_uint(std::string_view text, int base) {
    if (text.empty()) {
        return {};
    }
    const bool base0 = base == 0;
    const std::string_view original = text;
    if (base == 0) {
        base = 10;
        if (text[0] == '0') {
            const char prefix = text.size() >= 3 ? static_cast<char>(text[1] | 0x20) : '\0';
            if (prefix == 'b') {
                base = 2;
                text.remove_prefix(2);
            } else if (prefix == 'o') {
                base = 8;
                text.remove_prefix(2);
            } else if (prefix == 'x') {
                base = 16;
                text.remove_prefix(2);
            } else {
                base = 8;
                text.remove_prefix(1);
            }
        }
    } else if (base < 2 || base > 36) {
        return {};
    }

    constexpr std::uint64_t max_value = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t cutoff = max_value / static_cast<std::uint64_t>(base) + 1;
    bool underscores = false;
    std::uint64_t number = 0;
    for (const char c : text) {
        unsigned digit = 0;
        const char lower = static_cast<char>(c | 0x20);
        if (c == '_' && base0) {
            underscores = true;
            continue;
        }
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (lower >= 'a' && lower <= 'z') {
            digit = static_cast<unsigned>(lower - 'a') + 10;
        } else {
            return {};
        }
        if (digit >= static_cast<unsigned>(base)) {
            return {};
        }
        if (number >= cutoff) {
            return {std::nullopt, ParseIntError::range};
        }
        number *= static_cast<std::uint64_t>(base);
        const std::uint64_t next = number + digit;
        if (next < number) {
            return {std::nullopt, ParseIntError::range};
        }
        number = next;
    }
    if (underscores && !underscore_ok(original)) {
        return {};
    }
    return {number, ParseIntError::syntax};
}

} // namespace

DecodedRune decode_rune(std::string_view text) noexcept {
    if (text.empty()) {
        return {rune_error, 0, false};
    }
    const auto b0 = static_cast<unsigned char>(text[0]);
    if (b0 < 0x80) {
        return {b0, 1, true};
    }
    std::size_t size = 0;
    unsigned char low = 0x80;
    unsigned char high = 0xBF;
    char32_t rune = 0;
    if (in_range(b0, 0xC2, 0xDF)) {
        size = 2;
        rune = b0 & 0x1F;
    } else if (in_range(b0, 0xE0, 0xEF)) {
        size = 3;
        rune = b0 & 0x0F;
        if (b0 == 0xE0) {
            low = 0xA0;
        } else if (b0 == 0xED) {
            high = 0x9F;
        }
    } else if (in_range(b0, 0xF0, 0xF4)) {
        size = 4;
        rune = b0 & 0x07;
        if (b0 == 0xF0) {
            low = 0x90;
        } else if (b0 == 0xF4) {
            high = 0x8F;
        }
    } else {
        return {rune_error, 1, false};
    }
    if (text.size() < size) {
        return {rune_error, 1, false};
    }
    for (std::size_t index = 1; index < size; ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        const unsigned char min = index == 1 ? low : 0x80;
        const unsigned char max = index == 1 ? high : 0xBF;
        if (!in_range(byte, min, max)) {
            return {rune_error, 1, false};
        }
        rune = (rune << 6) | (byte & 0x3F);
    }
    return {rune, size, true};
}

void append_rune(std::string& output, char32_t rune) {
    if (rune > 0x10FFFF || (rune >= 0xD800 && rune <= 0xDFFF)) {
        rune = rune_error;
    }
    if (rune < 0x80) {
        output.push_back(static_cast<char>(rune));
    } else if (rune < 0x800) {
        output.push_back(static_cast<char>(0xC0 | (rune >> 6)));
        output.push_back(static_cast<char>(0x80 | (rune & 0x3F)));
    } else if (rune < 0x10000) {
        output.push_back(static_cast<char>(0xE0 | (rune >> 12)));
        output.push_back(static_cast<char>(0x80 | ((rune >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (rune & 0x3F)));
    } else {
        output.push_back(static_cast<char>(0xF0 | (rune >> 18)));
        output.push_back(static_cast<char>(0x80 | ((rune >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((rune >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (rune & 0x3F)));
    }
}

std::size_t rune_count(std::string_view text) noexcept {
    std::size_t count = 0;
    while (!text.empty()) {
        text.remove_prefix(decode_rune(text).size);
        ++count;
    }
    return count;
}

bool is_space(char32_t rune) noexcept {
    switch (rune) {
    case U'\t':
    case U'\n':
    case U'\v':
    case U'\f':
    case U'\r':
    case U' ':
    case 0x85:
    case 0xA0:
    case 0x1680:
    case 0x2028:
    case 0x2029:
    case 0x202F:
    case 0x205F:
    case 0x3000:
        return true;
    default:
        return rune >= 0x2000 && rune <= 0x200A;
    }
}

std::string_view trim_space(std::string_view text) noexcept {
    std::size_t first = text.size();
    std::size_t last = 0;
    std::size_t position = 0;
    while (position < text.size()) {
        const DecodedRune decoded = decode_rune(text.substr(position));
        const bool space = decoded.valid && is_space(decoded.rune);
        if (!space) {
            if (first == text.size()) {
                first = position;
            }
            last = position + decoded.size;
        }
        position += decoded.size;
    }
    if (first == text.size()) {
        return text.substr(text.size());
    }
    return text.substr(first, last - first);
}

std::vector<std::string_view> fields(std::string_view text) {
    std::vector<std::string_view> result;
    std::size_t position = 0;
    std::size_t start = std::string_view::npos;
    while (position < text.size()) {
        const DecodedRune decoded = decode_rune(text.substr(position));
        const bool space = decoded.valid && is_space(decoded.rune);
        if (space) {
            if (start != std::string_view::npos) {
                result.push_back(text.substr(start, position - start));
                start = std::string_view::npos;
            }
        } else if (start == std::string_view::npos) {
            start = position;
        }
        position += decoded.size;
    }
    if (start != std::string_view::npos) {
        result.push_back(text.substr(start));
    }
    return result;
}

std::string to_lower_ascii(std::string_view text) {
    std::string result(text);
    for (char& c : result) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return result;
}

bool equal_fold_ascii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a >= 'A' && a <= 'Z') {
            a = static_cast<char>(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = static_cast<char>(b - 'A' + 'a');
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

bool contains(std::string_view text, std::string_view part) noexcept {
    return text.find(part) != std::string_view::npos;
}

bool contains_any(std::string_view text, std::string_view characters) noexcept {
    return text.find_first_of(characters) != std::string_view::npos;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string result;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        if (index > 0) {
            result += separator;
        }
        result += parts[index];
    }
    return result;
}

std::string repeat(std::string_view text, std::size_t count) {
    std::string result;
    result.reserve(text.size() * count);
    for (std::size_t index = 0; index < count; ++index) {
        result += text;
    }
    return result;
}

std::string pad_right(std::string_view text, std::size_t width) {
    const std::size_t count = rune_count(text);
    std::string result(text);
    if (count < width) {
        result.append(width - count, ' ');
    }
    return result;
}

std::string quote(std::string_view text) {
    std::string result;
    result.reserve(text.size() + 2);
    result.push_back('"');
    while (!text.empty()) {
        const DecodedRune decoded = decode_rune(text);
        const char32_t rune = decoded.rune;
        if (!decoded.valid) {
            result += "\\x";
            append_hex(result, static_cast<unsigned char>(text[0]), 2);
            text.remove_prefix(1);
            continue;
        }
        text.remove_prefix(decoded.size);
        if (rune == U'"' || rune == U'\\') {
            result.push_back('\\');
            result.push_back(static_cast<char>(rune));
            continue;
        }
        if (is_print(rune)) {
            append_rune(result, rune);
            continue;
        }
        switch (rune) {
        case U'\a':
            result += "\\a";
            break;
        case U'\b':
            result += "\\b";
            break;
        case U'\f':
            result += "\\f";
            break;
        case U'\n':
            result += "\\n";
            break;
        case U'\r':
            result += "\\r";
            break;
        case U'\t':
            result += "\\t";
            break;
        case U'\v':
            result += "\\v";
            break;
        default:
            if (rune < U' ' || rune == 0x7F) {
                result += "\\x";
                append_hex(result, rune, 2);
            } else if (rune < 0x10000) {
                result += "\\u";
                append_hex(result, rune, 4);
            } else {
                result += "\\U";
                append_hex(result, rune, 8);
            }
        }
    }
    result.push_back('"');
    return result;
}

std::string format_fixed(double value, int precision) {
    std::array<char, 512> buffer{};
    const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                            std::chars_format::fixed, precision);
    if (error != std::errc()) {
        return "NaN";
    }
    return {buffer.data(), end};
}

std::string format_json_number(double value) {
    std::array<char, 512> buffer{};
    const double magnitude = std::fabs(value);
    const bool scientific = magnitude != 0 && (magnitude < 1e-6 || magnitude >= 1e21);
    const auto [end, error] =
        std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                      scientific ? std::chars_format::scientific : std::chars_format::fixed);
    if (error != std::errc()) {
        return "NaN";
    }
    std::string result(buffer.data(), end);
    const std::size_t size = result.size();
    if (scientific && size >= 4 && result[size - 4] == 'e' && result[size - 3] == '-' &&
        result[size - 2] == '0') {
        result.erase(size - 2, 1);
    }
    return result;
}

std::string format_duration(std::chrono::nanoseconds duration) {
    std::array<char, 40> buffer{};
    std::size_t write = buffer.size();
    const std::int64_t signed_value = duration.count();
    const bool negative = signed_value < 0;
    std::uint64_t value = negative ? 0 - static_cast<std::uint64_t>(signed_value)
                                   : static_cast<std::uint64_t>(signed_value);

    const auto format_fraction = [&](std::uint64_t number, int precision) {
        bool print = false;
        for (int index = 0; index < precision; ++index) {
            const auto digit = static_cast<char>(number % 10);
            print = print || digit != 0;
            if (print) {
                buffer[--write] = static_cast<char>('0' + digit);
            }
            number /= 10;
        }
        if (print) {
            buffer[--write] = '.';
        }
        return number;
    };
    const auto format_integer = [&](std::uint64_t number) {
        if (number == 0) {
            buffer[--write] = '0';
            return;
        }
        while (number > 0) {
            buffer[--write] = static_cast<char>('0' + number % 10);
            number /= 10;
        }
    };

    constexpr std::uint64_t second = 1'000'000'000;
    if (value < second) {
        if (value == 0) {
            return "0s";
        }
        int precision = 0;
        buffer[--write] = 's';
        if (value < 1'000) {
            buffer[--write] = 'n';
        } else if (value < 1'000'000) {
            precision = 3;
            buffer[--write] = '\xB5';
            buffer[--write] = '\xC2';
        } else {
            precision = 6;
            buffer[--write] = 'm';
        }
        value = format_fraction(value, precision);
        format_integer(value);
    } else {
        buffer[--write] = 's';
        value = format_fraction(value, 9);
        format_integer(value % 60);
        value /= 60;
        if (value > 0) {
            buffer[--write] = 'm';
            format_integer(value % 60);
            value /= 60;
            if (value > 0) {
                buffer[--write] = 'h';
                format_integer(value);
            }
        }
    }
    if (negative) {
        buffer[--write] = '-';
    }
    return {buffer.data() + write, buffer.size() - write};
}

std::string format_int(std::int64_t value, int base) {
    if (value < 0) {
        return "-" + format_uint(0 - static_cast<std::uint64_t>(value), base);
    }
    return format_uint(static_cast<std::uint64_t>(value), base);
}

std::string format_uint(std::uint64_t value, int base) {
    if (base < 2 || base > 36) {
        base = 10;
    }
    static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::array<char, 65> buffer{};
    std::size_t write = buffer.size();
    do {
        buffer[--write] = digits[value % static_cast<std::uint64_t>(base)];
        value /= static_cast<std::uint64_t>(base);
    } while (value > 0);
    return {buffer.data() + write, buffer.size() - write};
}

ParsedInt parse_int(std::string_view text, int base) {
    if (text.empty()) {
        return {};
    }
    bool negative = false;
    if (text[0] == '+') {
        text.remove_prefix(1);
    } else if (text[0] == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    const ParsedUint parsed = parse_uint(text, base);
    if (!parsed.value) {
        return {std::nullopt, parsed.error};
    }
    constexpr std::uint64_t cutoff = std::uint64_t{1} << 63;
    const std::uint64_t magnitude = *parsed.value;
    if (!negative && magnitude >= cutoff) {
        return {std::nullopt, ParseIntError::range};
    }
    if (negative && magnitude > cutoff) {
        return {std::nullopt, ParseIntError::range};
    }
    if (negative) {
        return {static_cast<std::int64_t>(0 - magnitude), ParseIntError::syntax};
    }
    return {static_cast<std::int64_t>(magnitude), ParseIntError::syntax};
}

ParsedInt atoi(std::string_view text) {
    return parse_int(text, 10);
}

} // namespace puls::text
