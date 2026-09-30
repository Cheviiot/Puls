#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text helpers that reproduce the Go standard library behavior the original
// implementation relied on (strings, strconv, unicode/utf8 and time).
namespace puls::text {

// Decodes one UTF-8 sequence like utf8.DecodeRuneInString: invalid input
// yields U+FFFD with size 1.
struct DecodedRune {
    char32_t rune;
    std::size_t size;
    bool valid;
};
DecodedRune decode_rune(std::string_view text) noexcept;
void append_rune(std::string& output, char32_t rune);
std::size_t rune_count(std::string_view text) noexcept;
bool is_space(char32_t rune) noexcept;

// strings.TrimSpace with Unicode white space.
std::string_view trim_space(std::string_view text) noexcept;
// strings.Fields with Unicode white space.
std::vector<std::string_view> fields(std::string_view text);
std::string to_lower_ascii(std::string_view text);
bool equal_fold_ascii(std::string_view left, std::string_view right) noexcept;
bool contains(std::string_view text, std::string_view part) noexcept;
bool contains_any(std::string_view text, std::string_view characters) noexcept;
std::string join(const std::vector<std::string>& parts, std::string_view separator);
std::string repeat(std::string_view text, std::size_t count);
// Right-pads text to width runes; longer text is returned unchanged.
std::string pad_right(std::string_view text, std::size_t width);

// strconv.Quote: a double-quoted Go string literal.
std::string quote(std::string_view text);

// %.Nf formatting with correct rounding.
std::string format_fixed(double value, int precision);
// Shortest representation used by encoding/json for float64 values.
std::string format_json_number(double value);
// time.Duration.String().
std::string format_duration(std::chrono::nanoseconds duration);
// strconv.FormatInt(value, base) for bases 2..36.
std::string format_int(std::int64_t value, int base = 10);
std::string format_uint(std::uint64_t value, int base = 10);

enum class ParseIntError { syntax, range };
struct ParsedInt {
    std::optional<std::int64_t> value;
    ParseIntError error = ParseIntError::syntax;
};
// strconv.ParseInt(text, base, 64); base 0 selects 0x/0o/0b/0 prefixes and
// allows underscores like Go.
ParsedInt parse_int(std::string_view text, int base);
// strconv.Atoi.
ParsedInt atoi(std::string_view text);

} // namespace puls::text
