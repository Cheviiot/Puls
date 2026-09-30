#pragma once

#include "puls/core/error.hpp"

#include <boost/json/value.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace puls::json {

// Parses exactly one JSON value; surrounding white space is allowed while a
// second value or any other trailing data is rejected.
Result<boost::json::value> parse(std::string_view text);

// Serializes like Go's json.Encoder with SetIndent("", "  "), including HTML
// escaping, float formatting and the trailing newline. Non-finite numbers
// are rejected like encoding/json does.
Result<std::string> encode_indented(const boost::json::value& value);

// Appends a JSON string literal escaped like encoding/json.
void append_string(std::string& output, std::string_view text);

// ObjectReader decodes a JSON object with encoding/json struct semantics:
// missing and null members yield zero values, members of the wrong type are
// errors and unknown members are ignored.
class ObjectReader {
public:
    // The value must be an object or null (a null value reads as empty).
    static Result<ObjectReader> from(const boost::json::value& value, std::string path = {});

    [[nodiscard]] Result<std::string> string(std::string_view key) const;
    [[nodiscard]] Result<std::int64_t> integer(std::string_view key) const;
    // Distinguishes a missing or null member from zero, like a *int64 field.
    [[nodiscard]] Result<std::optional<std::int64_t>> optional_integer(std::string_view key) const;
    [[nodiscard]] Result<ObjectReader> object(std::string_view key) const;
    // An array of objects; a missing or null member is an empty array.
    [[nodiscard]] Result<std::vector<ObjectReader>> objects(std::string_view key) const;

private:
    ObjectReader(const boost::json::object* object, std::string path)
        : object_(object), path_(std::move(path)) {}

    [[nodiscard]] const boost::json::value* member(std::string_view key) const;
    [[nodiscard]] std::string field_path(std::string_view key) const;

    const boost::json::object* object_ = nullptr;
    std::string path_;
};

} // namespace puls::json
