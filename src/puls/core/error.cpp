#include "puls/core/error.hpp"

namespace puls {

struct Error::Node {
    enum class Kind { leaf, prefixed, chained, surrounded, joined };

    Kind kind = Kind::leaf;
    std::string text;
    std::string suffix;
    const ErrorTag* tag = nullptr;
    std::shared_ptr<const ErrorDetail> detail;
    std::vector<Error> causes;
};

Error Error::make(std::string message) {
    auto node = std::make_shared<Node>();
    node->text = std::move(message);
    return Error(std::move(node));
}

Error Error::wrap(std::string context, Error cause) {
    if (!cause) {
        return make(std::move(context));
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::prefixed;
    node->text = std::move(context);
    node->causes.push_back(std::move(cause));
    return Error(std::move(node));
}

Error Error::chain(Error first, Error second) {
    if (!first) {
        return second;
    }
    if (!second) {
        return first;
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::chained;
    node->causes.push_back(std::move(first));
    node->causes.push_back(std::move(second));
    return Error(std::move(node));
}

Error Error::surround(std::string before, Error cause, std::string after) {
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::surrounded;
    node->text = std::move(before);
    node->suffix = std::move(after);
    node->causes.push_back(std::move(cause));
    return Error(std::move(node));
}

Error Error::join(std::vector<Error> errors) {
    std::vector<Error> present;
    present.reserve(errors.size());
    for (auto& error : errors) {
        if (error) {
            present.push_back(std::move(error));
        }
    }
    if (present.empty()) {
        return {};
    }
    auto node = std::make_shared<Node>();
    node->kind = Node::Kind::joined;
    node->causes = std::move(present);
    return Error(std::move(node));
}

Error Error::join(std::initializer_list<Error> errors) {
    return join(std::vector<Error>(errors));
}

Error Error::tagged(const ErrorTag& tag, std::string message, Error cause) {
    auto node = std::make_shared<Node>();
    node->kind = cause ? Node::Kind::prefixed : Node::Kind::leaf;
    node->text = std::move(message);
    node->tag = &tag;
    if (cause) {
        node->causes.push_back(std::move(cause));
    }
    return Error(std::move(node));
}

Error Error::with_detail(std::shared_ptr<const ErrorDetail> detail, std::string message,
                         Error cause) {
    auto node = std::make_shared<Node>();
    node->kind = cause ? Node::Kind::prefixed : Node::Kind::leaf;
    node->text = std::move(message);
    node->detail = std::move(detail);
    if (cause) {
        node->causes.push_back(std::move(cause));
    }
    return Error(std::move(node));
}

std::string Error::message() const {
    if (!node_) {
        return "<nil>";
    }
    switch (node_->kind) {
    case Node::Kind::leaf:
        return node_->text;
    case Node::Kind::prefixed: {
        std::string cause = node_->causes.front().message();
        if (node_->text.empty()) {
            return cause;
        }
        return node_->text + ": " + cause;
    }
    case Node::Kind::chained:
        return node_->causes[0].message() + ": " + node_->causes[1].message();
    case Node::Kind::surrounded:
        return node_->text + node_->causes.front().message() + node_->suffix;
    case Node::Kind::joined: {
        std::string text;
        for (std::size_t index = 0; index < node_->causes.size(); ++index) {
            if (index > 0) {
                text.push_back('\n');
            }
            text += node_->causes[index].message();
        }
        return text;
    }
    }
    return node_->text;
}

bool Error::is(const ErrorTag& tag) const {
    if (!node_) {
        return false;
    }
    if (node_->tag == &tag) {
        return true;
    }
    for (const auto& cause : node_->causes) {
        if (cause.is(tag)) {
            return true;
        }
    }
    return false;
}

bool Error::is(const Error& target) const {
    if (!node_ || !target.node_) {
        return false;
    }
    if (node_ == target.node_) {
        return true;
    }
    for (const auto& cause : node_->causes) {
        if (cause.is(target)) {
            return true;
        }
    }
    return false;
}

const std::vector<Error>& Error::causes() const {
    static const std::vector<Error> empty;
    return node_ ? node_->causes : empty;
}

bool Error::visit_details_impl(bool (*callback)(const ErrorDetail&, void*), void* state) const {
    if (!node_) {
        return false;
    }
    if (node_->detail && callback(*node_->detail, state)) {
        return true;
    }
    for (const auto& cause : node_->causes) {
        if (cause.visit_details_impl(callback, state)) {
            return true;
        }
    }
    return false;
}

namespace errors {

const ErrorTag canceled_tag{"context.Canceled"};
const ErrorTag deadline_exceeded_tag{"context.DeadlineExceeded"};

const Error& canceled() {
    static const Error error = Error::tagged(canceled_tag, "context canceled");
    return error;
}

const Error& deadline_exceeded() {
    static const Error error = Error::tagged(deadline_exceeded_tag, "context deadline exceeded");
    return error;
}

} // namespace errors

} // namespace puls
