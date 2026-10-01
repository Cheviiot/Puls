#include "puls/core/json.hpp"

#include "puls/core/text.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/parse_options.hpp>

#include <cmath>
#include <limits>

namespace puls::json {

namespace {

std::string kind_name(const boost::json::value& value) {
    switch (value.kind()) {
    case boost::json::kind::null:
        return "null";
    case boost::json::kind::bool_:
        return "bool";
    case boost::json::kind::int64:
    case boost::json::kind::uint64:
    case boost::json::kind::double_:
        return "number";
    case boost::json::kind::string:
        return "string";
    case boost::json::kind::array:
        return "array";
    case boost::json::kind::object:
        return "object";
    }
    return "value";
}

Error type_error(const boost::json::value& value, const std::string& path, std::string_view type) {
    std::string message = "json: cannot unmarshal " + kind_name(value);
    if (!path.empty()) {
        message += " into field \"" + path + "\"";
    }
    message += " of type ";
    message += type;
    return Error::make(std::move(message));
}

void encode(std::string& output, const boost::json::value& value, int depth, Error& error) {
    if (error) {
        return;
    }
    const auto newline = [&output](int level) {
        output.push_back('\n');
        output.append(static_cast<std::size_t>(level) * 2, ' ');
    };
    switch (value.kind()) {
    case boost::json::kind::null:
        output += "null";
        return;
    case boost::json::kind::bool_:
        output += value.get_bool() ? "true" : "false";
        return;
    case boost::json::kind::int64:
        output += std::to_string(value.get_int64());
        return;
    case boost::json::kind::uint64:
        output += std::to_string(value.get_uint64());
        return;
    case boost::json::kind::double_: {
        const double number = value.get_double();
        if (!std::isfinite(number)) {
            error = Error::make("json: unsupported value: " + text::format_json_number(number));
            return;
        }
        output += text::format_json_number(number);
        return;
    }
    case boost::json::kind::string:
        append_string(output, std::string_view(value.get_string()));
        return;
    case boost::json::kind::array: {
        const auto& array = value.get_array();
        if (array.empty()) {
            output += "[]";
            return;
        }
        output.push_back('[');
        for (std::size_t index = 0; index < array.size(); ++index) {
            if (index > 0) {
                output.push_back(',');
            }
            newline(depth + 1);
            encode(output, array[index], depth + 1, error);
        }
        newline(depth);
        output.push_back(']');
        return;
    }
    case boost::json::kind::object: {
        const auto& object = value.get_object();
        if (object.empty()) {
            output += "{}";
            return;
        }
        output.push_back('{');
        bool first = true;
        for (const auto& member : object) {
            if (!first) {
                output.push_back(',');
            }
            first = false;
            newline(depth + 1);
            append_string(output, std::string_view(member.key()));
            output += ": ";
            encode(output, member.value(), depth + 1, error);
        }
        newline(depth);
        output.push_back('}');
        return;
    }
    }
}

} // namespace

Result<boost::json::value> parse(std::string_view text) {
    boost::json::parse_options options;
    options.max_depth = 512;
    options.allow_invalid_utf8 = true;
    boost::system::error_code code;
    boost::json::value value = boost::json::parse(text, code, {}, options);
    if (code) {
        return Error::make("json: " + code.message());
    }
    return value;
}

Result<std::string> encode_indented(const boost::json::value& value) {
    std::string output;
    Error error;
    encode(output, value, 0, error);
    if (error) {
        return error;
    }
    output.push_back('\n');
    return output;
}

void append_string(std::string& output, std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    std::size_t start = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte < 0x80) {
            const bool safe = byte >= 0x20 && byte != '"' && byte != '\\' && byte != '<' &&
                              byte != '>' && byte != '&';
            if (safe) {
                ++index;
                continue;
            }
            output.append(text.substr(start, index - start));
            switch (byte) {
            case '\\':
            case '"':
                output.push_back('\\');
                output.push_back(static_cast<char>(byte));
                break;
            case '\b':
                output += "\\b";
                break;
            case '\f':
                output += "\\f";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                output += "\\u00";
                output.push_back(hex[byte >> 4]);
                output.push_back(hex[byte & 0xF]);
            }
            ++index;
            start = index;
            continue;
        }
        const text::DecodedRune decoded = text::decode_rune(text.substr(index));
        if (!decoded.valid) {
            output.append(text.substr(start, index - start));
            output += "\\ufffd";
            index += decoded.size;
            start = index;
            continue;
        }
        if (decoded.rune == 0x2028 || decoded.rune == 0x2029) {
            output.append(text.substr(start, index - start));
            output += "\\u202";
            output.push_back(hex[decoded.rune & 0xF]);
            index += decoded.size;
            start = index;
            continue;
        }
        index += decoded.size;
    }
    output.append(text.substr(start));
    output.push_back('"');
}

Result<ObjectReader> ObjectReader::from(const boost::json::value& value, std::string path) {
    if (value.is_null()) {
        return ObjectReader(nullptr, std::move(path));
    }
    if (!value.is_object()) {
        return type_error(value, path, "object");
    }
    return ObjectReader(&value.get_object(), std::move(path));
}

const boost::json::value* ObjectReader::member(std::string_view key) const {
    if (object_ == nullptr) {
        return nullptr;
    }
    const auto* value = object_->if_contains(key);
    if (value == nullptr || value->is_null()) {
        return nullptr;
    }
    return value;
}

std::string ObjectReader::field_path(std::string_view key) const {
    if (path_.empty()) {
        return std::string(key);
    }
    return path_ + "." + std::string(key);
}

Result<std::string> ObjectReader::string(std::string_view key) const {
    const auto* value = member(key);
    if (value == nullptr) {
        return std::string();
    }
    if (!value->is_string()) {
        return type_error(*value, field_path(key), "string");
    }
    return std::string(value->get_string());
}

Result<std::int64_t> ObjectReader::integer(std::string_view key) const {
    auto parsed = optional_integer(key);
    if (!parsed) {
        return std::move(parsed).error();
    }
    return parsed->value_or(0);
}

Result<std::optional<std::int64_t>> ObjectReader::optional_integer(std::string_view key) const {
    const auto* value = member(key);
    if (value == nullptr) {
        return std::optional<std::int64_t>();
    }
    if (value->is_int64()) {
        return std::optional<std::int64_t>(value->get_int64());
    }
    if (value->is_uint64() &&
        value->get_uint64() <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::optional<std::int64_t>(static_cast<std::int64_t>(value->get_uint64()));
    }
    return type_error(*value, field_path(key), "int");
}

Result<ObjectReader> ObjectReader::object(std::string_view key) const {
    const auto* value = member(key);
    if (value == nullptr) {
        return ObjectReader(nullptr, field_path(key));
    }
    return from(*value, field_path(key));
}

Result<std::vector<ObjectReader>> ObjectReader::objects(std::string_view key) const {
    std::vector<ObjectReader> result;
    const auto* value = member(key);
    if (value == nullptr) {
        return result;
    }
    if (!value->is_array()) {
        return type_error(*value, field_path(key), "array");
    }
    for (const auto& item : value->get_array()) {
        auto reader = from(item, field_path(key));
        if (!reader) {
            return std::move(reader).error();
        }
        result.push_back(std::move(reader).value());
    }
    return result;
}

} // namespace puls::json
