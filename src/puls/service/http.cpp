#include "puls/service/http.hpp"

#include "puls/core/json.hpp"
#include "puls/core/text.hpp"

#include <set>

namespace puls::service {

namespace {

bool token_character(char c) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte <= 0x20 || byte >= 0x7F) {
        return false;
    }
    return std::string_view("()<>@,;:\\\"/[]?=").find(c) == std::string_view::npos;
}

std::string_view consume_token(std::string_view& text) {
    std::size_t length = 0;
    while (length < text.size() && token_character(text[length])) {
        ++length;
    }
    const std::string_view token = text.substr(0, length);
    text.remove_prefix(length);
    return token;
}

void trim_left_space(std::string_view& text) {
    while (!text.empty()) {
        const text::DecodedRune decoded = text::decode_rune(text);
        if (!decoded.valid || !text::is_space(decoded.rune)) {
            return;
        }
        text.remove_prefix(decoded.size);
    }
}

// Consumes a token or a quoted string. Returns false when neither is present.
bool consume_value(std::string_view& text) {
    if (text.empty()) {
        return false;
    }
    if (text[0] != '"') {
        return !consume_token(text).empty();
    }
    for (std::size_t index = 1; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '"') {
            text.remove_prefix(index + 1);
            return true;
        }
        if (c == '\\' && index + 1 < text.size()) {
            ++index;
        } else if (c == '\r' || c == '\n') {
            return false;
        }
    }
    return false;
}

} // namespace

Result<std::string> read_limited(const Context& ctx, net::HttpResponse& response,
                                 std::size_t limit) {
    if (limit < 1) {
        return Error::make("предел ответа должен быть положительным");
    }
    auto payload = response.read_up_to(ctx, limit + 1);
    if (!payload) {
        return std::move(payload).error();
    }
    if (payload->size() > limit) {
        return Error::make("ответ превышает безопасный предел " + std::to_string(limit) + " байт");
    }
    return payload;
}

Result<boost::json::value> decode_json_limited(const Context& ctx, net::HttpResponse& response,
                                               std::size_t limit) {
    auto payload = read_limited(ctx, response, limit);
    if (!payload) {
        return std::move(payload).error();
    }
    return json::parse(*payload);
}

std::optional<std::string> parse_media_type(std::string_view value) {
    const auto semicolon = value.find(';');
    std::string media_type = text::to_lower_ascii(text::trim_space(value.substr(0, semicolon)));

    std::string_view check = media_type;
    if (consume_token(check).empty()) {
        return std::nullopt;
    }
    if (!check.empty()) {
        if (check[0] != '/') {
            return std::nullopt;
        }
        check.remove_prefix(1);
        if (consume_token(check).empty() || !check.empty()) {
            return std::nullopt;
        }
    }

    std::string_view rest =
        semicolon == std::string_view::npos ? std::string_view() : value.substr(semicolon);
    std::set<std::string> names;
    while (!rest.empty()) {
        trim_left_space(rest);
        if (rest.empty()) {
            break;
        }
        std::string_view parameter = rest;
        bool valid = false;
        if (parameter[0] == ';') {
            parameter.remove_prefix(1);
            trim_left_space(parameter);
            const std::string name = text::to_lower_ascii(consume_token(parameter));
            trim_left_space(parameter);
            if (!name.empty() && !parameter.empty() && parameter[0] == '=') {
                parameter.remove_prefix(1);
                trim_left_space(parameter);
                if (consume_value(parameter)) {
                    if (!names.insert(name).second) {
                        return std::nullopt;
                    }
                    valid = true;
                }
            }
        }
        if (!valid) {
            if (text::trim_space(rest) == ";") {
                break;
            }
            return std::nullopt;
        }
        rest = parameter;
    }
    return media_type;
}

Error validate_content_type(const net::HttpResponse& response,
                            std::initializer_list<std::string_view> expected) {
    const std::string raw = response.header("Content-Type");
    if (const auto media_type = parse_media_type(raw)) {
        for (const auto allowed : expected) {
            if (text::equal_fold_ascii(*media_type, allowed)) {
                return {};
            }
        }
    }
    return Error::make("неожиданный Content-Type " + text::quote(raw));
}

void drain_and_close(const Context& ctx, net::HttpResponse& response) {
    (void)response.discard(ctx, 64 << 10);
    response.close();
}

} // namespace puls::service
