#include "puls/net/url.hpp"

#include "puls/core/text.hpp"

#include <algorithm>

namespace puls::net {

namespace {

enum class Encoding { path, host, query_component, fragment };

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int unhex(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - 'A' + 10;
}

bool should_escape(char c, Encoding mode) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        return false;
    }
    if (mode == Encoding::host) {
        switch (c) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case ':':
        case '[':
        case ']':
        case '<':
        case '>':
        case '"':
            return false;
        default:
            break;
        }
    }
    switch (c) {
    case '-':
    case '_':
    case '.':
    case '~':
        return false;
    case '$':
    case '&':
    case '+':
    case ',':
    case '/':
    case ':':
    case ';':
    case '=':
    case '?':
    case '@':
        switch (mode) {
        case Encoding::path:
            return c == '?';
        case Encoding::query_component:
            return true;
        case Encoding::fragment:
            return false;
        case Encoding::host:
            break;
        }
        break;
    default:
        break;
    }
    if (mode == Encoding::fragment) {
        switch (c) {
        case '!':
        case '(':
        case ')':
        case '*':
            return false;
        default:
            break;
        }
    }
    return true;
}

std::string escape(std::string_view text, Encoding mode) {
    static constexpr char upper_hex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(text.size());
    for (const char c : text) {
        if (!should_escape(c, mode)) {
            result.push_back(c);
        } else if (c == ' ' && mode == Encoding::query_component) {
            result.push_back('+');
        } else {
            const auto byte = static_cast<unsigned char>(c);
            result.push_back('%');
            result.push_back(upper_hex[byte >> 4]);
            result.push_back(upper_hex[byte & 0xF]);
        }
    }
    return result;
}

Result<std::string> unescape(std::string_view text, Encoding mode) {
    for (std::size_t index = 0; index < text.size();) {
        const char c = text[index];
        if (c == '%') {
            if (index + 2 >= text.size() || !is_hex(text[index + 1]) || !is_hex(text[index + 2])) {
                return Error::make(
                    "invalid URL escape " +
                    text::quote(text.substr(index, std::min<std::size_t>(3, text.size() - index))));
            }
            if (mode == Encoding::host && unhex(text[index + 1]) < 8 &&
                text.substr(index, 3) != "%25") {
                return Error::make("invalid URL escape " + text::quote(text.substr(index, 3)));
            }
            index += 3;
            continue;
        }
        if (mode == Encoding::host && static_cast<unsigned char>(c) < 0x80 &&
            should_escape(c, mode)) {
            return Error::make("invalid character " + text::quote(text.substr(index, 1)) +
                               " in host name");
        }
        ++index;
    }
    std::string result;
    result.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '%') {
            result.push_back(
                static_cast<char>((unhex(text[index + 1]) << 4) | unhex(text[index + 2])));
            index += 2;
        } else if (c == '+' && mode == Encoding::query_component) {
            result.push_back(' ');
        } else {
            result.push_back(c);
        }
    }
    return result;
}

bool valid_encoded_path(std::string_view text) {
    for (const char c : text) {
        switch (c) {
        case '!':
        case '$':
        case '&':
        case '\'':
        case '(':
        case ')':
        case '*':
        case '+':
        case ',':
        case ';':
        case '=':
        case ':':
        case '@':
        case '[':
        case ']':
        case '%':
            break;
        default:
            if (should_escape(c, Encoding::path)) {
                return false;
            }
        }
    }
    return true;
}

bool valid_optional_port(std::string_view port) {
    if (port.empty()) {
        return true;
    }
    if (port[0] != ':') {
        return false;
    }
    return std::all_of(port.begin() + 1, port.end(), [](char c) { return c >= '0' && c <= '9'; });
}

Result<std::string> parse_host(std::string_view host) {
    if (!host.empty() && host[0] == '[') {
        const auto close = host.rfind(']');
        if (close == std::string_view::npos) {
            return Error::make("missing ']' in host");
        }
        const std::string_view colon_port = host.substr(close + 1);
        if (!valid_optional_port(colon_port)) {
            return Error::make("invalid port " + text::quote(colon_port) + " after host");
        }
    } else if (const auto colon = host.rfind(':'); colon != std::string_view::npos) {
        const std::string_view colon_port = host.substr(colon);
        if (!valid_optional_port(colon_port)) {
            return Error::make("invalid port " + text::quote(colon_port) + " after host");
        }
    }
    return unescape(host, Encoding::host);
}

Error set_path(Url& url, std::string_view raw) {
    auto path = unescape(raw, Encoding::path);
    if (!path) {
        return std::move(path).error();
    }
    url.path = std::move(path).value();
    if (escape(url.path, Encoding::path) == raw) {
        url.raw_path.clear();
    } else {
        url.raw_path = std::string(raw);
    }
    return {};
}

std::string resolve_path(std::string_view base, std::string_view reference) {
    std::string full;
    if (reference.empty()) {
        full = std::string(base);
    } else if (reference[0] != '/') {
        const auto slash = base.rfind('/');
        full = std::string(base.substr(0, slash == std::string_view::npos ? 0 : slash + 1));
        full += reference;
    } else {
        full = std::string(reference);
    }
    if (full.empty()) {
        return {};
    }

    std::string output = "/";
    bool first = true;
    std::string_view remaining = full;
    std::string_view element;
    bool found = true;
    while (found) {
        const auto slash = remaining.find('/');
        found = slash != std::string_view::npos;
        element = remaining.substr(0, found ? slash : remaining.size());
        remaining = found ? remaining.substr(slash + 1) : std::string_view();
        if (element == ".") {
            first = false;
            continue;
        }
        if (element == "..") {
            const std::string current = output.substr(1);
            const auto index = current.rfind('/');
            output = "/";
            if (index == std::string::npos) {
                first = true;
            } else {
                output += current.substr(0, index);
            }
        } else {
            if (!first) {
                output.push_back('/');
            }
            output += element;
            first = false;
        }
    }
    if (element == "." || element == "..") {
        output.push_back('/');
    }
    if (output.size() > 1 && output[1] == '/') {
        output.erase(0, 1);
    }
    return output;
}

} // namespace

Result<Url> Url::parse(std::string_view raw) {
    std::string_view rest = raw;
    std::string_view fragment;
    bool has_fragment = false;
    if (const auto hash = rest.find('#'); hash != std::string_view::npos) {
        fragment = rest.substr(hash + 1);
        rest = rest.substr(0, hash);
        has_fragment = true;
    }
    const auto fail = [raw](const Error& cause) {
        return Error::wrap("parse " + text::quote(raw), cause);
    };
    if (std::any_of(raw.begin(), raw.end(), [](char c) {
            const auto byte = static_cast<unsigned char>(c);
            return byte < 0x20 || byte == 0x7F;
        })) {
        return fail(Error::make("net/url: invalid control character in URL"));
    }

    Url url;
    if (rest == "*") {
        url.path = "*";
        return url;
    }

    for (std::size_t index = 0; index < rest.size(); ++index) {
        const char c = rest[index];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
            continue;
        }
        if ((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.') {
            if (index == 0) {
                break;
            }
            continue;
        }
        if (c == ':') {
            if (index == 0) {
                return fail(Error::make("missing protocol scheme"));
            }
            url.scheme = text::to_lower_ascii(rest.substr(0, index));
            rest = rest.substr(index + 1);
        }
        break;
    }

    if (!rest.empty() && rest.back() == '?' && std::count(rest.begin(), rest.end(), '?') == 1) {
        url.force_query = true;
        rest.remove_suffix(1);
    } else if (const auto question = rest.find('?'); question != std::string_view::npos) {
        url.raw_query = std::string(rest.substr(question + 1));
        rest = rest.substr(0, question);
    }

    if (rest.empty() || rest[0] != '/') {
        if (!url.scheme.empty()) {
            url.opaque = std::string(rest);
            return url;
        }
        const std::string_view segment = rest.substr(0, rest.find('/'));
        if (segment.find(':') != std::string_view::npos) {
            return fail(Error::make("first path segment in URL cannot contain colon"));
        }
    }

    if ((!url.scheme.empty() || rest.substr(0, 3) != "///") && rest.substr(0, 2) == "//") {
        std::string_view authority = rest.substr(2);
        rest = {};
        if (const auto slash = authority.find('/'); slash != std::string_view::npos) {
            rest = authority.substr(slash);
            authority = authority.substr(0, slash);
        }
        std::string_view host_part = authority;
        if (const auto at = authority.rfind('@'); at != std::string_view::npos) {
            url.has_user = true;
            host_part = authority.substr(at + 1);
        }
        auto host = parse_host(host_part);
        if (!host) {
            return fail(host.error());
        }
        url.host = std::move(host).value();
    }

    if (Error error = set_path(url, rest)) {
        return fail(error);
    }
    if (has_fragment) {
        auto decoded = unescape(fragment, Encoding::fragment);
        if (!decoded) {
            return fail(decoded.error());
        }
        url.fragment = std::move(decoded).value();
    }
    return url;
}

std::string Url::hostname() const {
    std::string_view value = host;
    if (const auto colon = value.rfind(':');
        colon != std::string_view::npos && valid_optional_port(value.substr(colon))) {
        value = value.substr(0, colon);
    }
    if (value.size() >= 2 && value.front() == '[' && value.back() == ']') {
        value = value.substr(1, value.size() - 2);
    }
    return std::string(value);
}

std::string Url::port() const {
    std::string_view value = host;
    if (const auto colon = value.rfind(':');
        colon != std::string_view::npos && valid_optional_port(value.substr(colon))) {
        return std::string(value.substr(colon + 1));
    }
    return {};
}

std::string Url::escaped_path() const {
    if (!raw_path.empty() && valid_encoded_path(raw_path)) {
        auto decoded = unescape(raw_path, Encoding::path);
        if (decoded && *decoded == path) {
            return raw_path;
        }
    }
    if (path == "*") {
        return "*";
    }
    return escape(path, Encoding::path);
}

std::string Url::request_uri() const {
    std::string result = opaque;
    if (result.empty()) {
        result = escaped_path();
        if (result.empty()) {
            result = "/";
        }
    } else if (result.rfind("//", 0) == 0) {
        result = scheme + ":" + result;
    }
    if (force_query || !raw_query.empty()) {
        result += "?" + raw_query;
    }
    return result;
}

std::string Url::to_string() const {
    std::string result;
    if (!scheme.empty()) {
        result += scheme + ":";
    }
    if (!opaque.empty()) {
        result += opaque;
    } else {
        if (!scheme.empty() || !host.empty() || has_user) {
            if (!host.empty() || !path.empty() || has_user) {
                result += "//";
            }
            result += escape(host, Encoding::host);
        }
        const std::string escaped = escaped_path();
        if (!escaped.empty() && escaped[0] != '/' && !host.empty()) {
            result.push_back('/');
        }
        if (result.empty()) {
            const std::string_view segment = std::string_view(escaped).substr(0, escaped.find('/'));
            if (segment.find(':') != std::string_view::npos) {
                result += "./";
            }
        }
        result += escaped;
    }
    if (force_query || !raw_query.empty()) {
        result += "?" + raw_query;
    }
    if (!fragment.empty()) {
        result += "#" + escape(fragment, Encoding::fragment);
    }
    return result;
}

Url Url::resolve_reference(const Url& reference) const {
    Url url = reference;
    if (reference.scheme.empty()) {
        url.scheme = scheme;
    }
    if (!reference.scheme.empty() || !reference.host.empty() || reference.has_user) {
        (void)set_path(url, resolve_path(reference.escaped_path(), ""));
        return url;
    }
    if (!reference.opaque.empty()) {
        url.has_user = false;
        url.host.clear();
        url.path.clear();
        return url;
    }
    if (reference.path.empty() && !reference.force_query && reference.raw_query.empty()) {
        url.raw_query = raw_query;
        if (reference.fragment.empty()) {
            url.fragment = fragment;
        }
    }
    if (reference.path.empty() && !opaque.empty()) {
        url.opaque = opaque;
        url.has_user = false;
        url.host.clear();
        url.path.clear();
        return url;
    }
    url.host = host;
    url.has_user = has_user;
    (void)set_path(url, resolve_path(escaped_path(), reference.escaped_path()));
    return url;
}

std::string query_escape(std::string_view text) {
    return escape(text, Encoding::query_component);
}

Result<std::string> query_unescape(std::string_view text) {
    return unescape(text, Encoding::query_component);
}

std::vector<std::pair<std::string, std::string>> parse_query(std::string_view raw_query) {
    std::vector<std::pair<std::string, std::string>> values;
    while (!raw_query.empty()) {
        const auto ampersand = raw_query.find('&');
        std::string_view pair = raw_query.substr(0, ampersand);
        raw_query = ampersand == std::string_view::npos ? std::string_view()
                                                        : raw_query.substr(ampersand + 1);
        if (pair.find(';') != std::string_view::npos || pair.empty()) {
            continue;
        }
        const auto equals = pair.find('=');
        auto key = query_unescape(pair.substr(0, equals));
        auto value = query_unescape(equals == std::string_view::npos ? std::string_view()
                                                                     : pair.substr(equals + 1));
        if (!key || !value) {
            continue;
        }
        values.emplace_back(std::move(key).value(), std::move(value).value());
    }
    return values;
}

std::string encode_query(std::vector<std::pair<std::string, std::string>> values) {
    std::stable_sort(values.begin(), values.end(),
                     [](const auto& left, const auto& right) { return left.first < right.first; });
    std::string result;
    for (const auto& [key, value] : values) {
        if (!result.empty()) {
            result.push_back('&');
        }
        result += query_escape(key);
        result.push_back('=');
        result += query_escape(value);
    }
    return result;
}

void set_query_value(std::vector<std::pair<std::string, std::string>>& values,
                     const std::string& key, const std::string& value) {
    values.erase(std::remove_if(values.begin(), values.end(),
                                [&key](const auto& entry) { return entry.first == key; }),
                 values.end());
    values.emplace_back(key, value);
}

Result<std::pair<std::string, std::string>> split_host_port(std::string_view host_port) {
    const auto fail = [host_port](std::string_view why) {
        return Error::make("address " + std::string(host_port) + ": " + std::string(why));
    };
    const auto last_colon = host_port.rfind(':');
    if (last_colon == std::string_view::npos) {
        return fail("missing port in address");
    }
    std::string_view host;
    std::size_t open_search = 0;
    std::size_t close_search = 0;
    if (host_port[0] == '[') {
        const auto end = host_port.find(']');
        if (end == std::string_view::npos) {
            return fail("missing ']' in address");
        }
        if (end + 1 == host_port.size()) {
            return fail("missing port in address");
        }
        if (end + 1 != last_colon) {
            if (host_port[end + 1] == ':') {
                return fail("too many colons in address");
            }
            return fail("missing port in address");
        }
        host = host_port.substr(1, end - 1);
        open_search = 1;
        close_search = end + 1;
    } else {
        host = host_port.substr(0, last_colon);
        if (host.find(':') != std::string_view::npos) {
            return fail("too many colons in address");
        }
    }
    if (host_port.substr(open_search).find('[') != std::string_view::npos) {
        return fail("unexpected '[' in address");
    }
    if (host_port.substr(close_search).find(']') != std::string_view::npos) {
        return fail("unexpected ']' in address");
    }
    return std::pair{std::string(host), std::string(host_port.substr(last_colon + 1))};
}

std::string join_host_port(std::string_view host, std::string_view port) {
    if (host.find(':') != std::string_view::npos) {
        return "[" + std::string(host) + "]:" + std::string(port);
    }
    return std::string(host) + ":" + std::string(port);
}

} // namespace puls::net
