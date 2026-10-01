#pragma once

#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace puls {

// ErrorDetail is the base of typed error payloads. Error::as<T>() finds the
// first payload of type T in an error tree, like Go's errors.As.
class ErrorDetail {
public:
    virtual ~ErrorDetail() = default;
};

// NetworkError marks transport failures, like Go's net.Error. timeout reports
// whether an I/O deadline or connection timeout caused the failure.
class NetworkError final : public ErrorDetail {
public:
    explicit NetworkError(bool is_timeout) noexcept : timeout(is_timeout) {}

    bool timeout;
};

// ErrorTag identifies a sentinel error kind. Error::is(tag) reports whether
// any error in the tree carries the tag, like Go's errors.Is for sentinels.
struct ErrorTag {
    const char* name;
};

// Error is an immutable, cheaply copyable tree of error nodes. A
// default-constructed Error means "no error".
class Error {
public:
    Error() noexcept = default;

    // A leaf error with the given text (errors.New).
    static Error make(std::string message);
    // Prefixes cause with context (fmt.Errorf("context: %w", cause)).
    static Error wrap(std::string context, Error cause);
    // Combines two errors as "first: second" while keeping both in the tree
    // (fmt.Errorf("%w: %w", first, second)).
    static Error chain(Error first, Error second);
    // Embeds cause in surrounding text (fmt.Errorf("before %w after", cause)).
    static Error surround(std::string before, Error cause, std::string after);
    // Joins non-empty errors, separated by newlines (errors.Join). Returns an
    // empty Error when every input is empty.
    static Error join(std::vector<Error> errors);
    static Error join(std::initializer_list<Error> errors);
    // A sentinel node: text is rendered as "message" or "message: cause".
    static Error tagged(const ErrorTag& tag, std::string message, Error cause = {});
    // A typed payload node: text is rendered as "message" or "message: cause".
    static Error with_detail(std::shared_ptr<const ErrorDetail> detail, std::string message,
                             Error cause = {});

    explicit operator bool() const noexcept { return node_ != nullptr; }

    // Full human-readable text of the error tree (error.Error()).
    [[nodiscard]] std::string message() const;

    // Reports whether any node carries tag.
    [[nodiscard]] bool is(const ErrorTag& tag) const;
    // Reports whether target (by identity) is part of this tree.
    [[nodiscard]] bool is(const Error& target) const;

    // Returns the first payload of type T in pre-order traversal.
    template <class T>
    [[nodiscard]] const T* as() const {
        const T* found = nullptr;
        visit_details([&found](const ErrorDetail& detail) {
            found = dynamic_cast<const T*>(&detail);
            return found != nullptr;
        });
        return found;
    }

    // Direct children: the wrapped cause or the joined errors.
    [[nodiscard]] const std::vector<Error>& causes() const;

    friend bool operator==(const Error& left, const Error& right) noexcept {
        return left.node_ == right.node_;
    }

private:
    struct Node;
    explicit Error(std::shared_ptr<const Node> node) noexcept : node_(std::move(node)) {}

    template <class Visitor>
    bool visit_details(Visitor&& visitor) const;
    bool visit_details_impl(bool (*callback)(const ErrorDetail&, void*), void* state) const;

    std::shared_ptr<const Node> node_;
};

template <class Visitor>
bool Error::visit_details(Visitor&& visitor) const {
    return visit_details_impl(
        [](const ErrorDetail& detail, void* state) {
            return (*static_cast<std::remove_reference_t<Visitor>*>(state))(detail);
        },
        &visitor);
}

namespace errors {

extern const ErrorTag canceled_tag;
extern const ErrorTag deadline_exceeded_tag;

// context.Canceled
const Error& canceled();
// context.DeadlineExceeded
const Error& deadline_exceeded();

} // namespace errors

// Result holds either a value or an Error, mirroring Go's (value, error)
// return pair where exactly one side is meaningful.
template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : storage_(std::in_place_index<0>, std::move(value)) {}
    Result(Error error) : storage_(std::in_place_index<1>, std::move(error)) {}

    [[nodiscard]] bool ok() const noexcept { return storage_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() & { return std::get<0>(storage_); }
    const T& value() const& { return std::get<0>(storage_); }
    T&& value() && { return std::get<0>(std::move(storage_)); }
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }

    [[nodiscard]] const Error& error() const& { return std::get<1>(storage_); }
    [[nodiscard]] Error error() && { return std::get<1>(std::move(storage_)); }

private:
    std::variant<T, Error> storage_;
};

} // namespace puls
