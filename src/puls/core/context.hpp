#pragma once

#include "puls/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace puls {

namespace detail {
class ContextState;
struct ContextCallbackNode;
} // namespace detail

enum class CancelCause : std::uint8_t { none, canceled, deadline_exceeded };

class ContextCallback;

// Context carries cancellation and a deadline across API boundaries, like
// Go's context.Context. It is a cheap, thread-safe handle; copies observe
// the same cancellation state. The default Context is never canceled.
class Context {
public:
    using Clock = std::chrono::steady_clock;

    Context() noexcept = default;

    [[nodiscard]] bool done() const;
    [[nodiscard]] CancelCause cause() const;
    // errors::canceled(), errors::deadline_exceeded() or an empty Error.
    [[nodiscard]] Error err() const;
    [[nodiscard]] std::optional<Clock::time_point> deadline() const;

    // Blocks until the context is done or the timeout elapses. Returns true
    // when the context finished first.
    bool wait_for(Clock::duration timeout) const;
    bool wait_until(Clock::time_point when) const;
    // Blocks until the context is done. A background context never is.
    void wait() const;

    // Invokes callback once when the context becomes done; if it already is,
    // callback runs immediately on the calling thread. Destroying the returned
    // handle unregisters the callback and waits for a concurrently running
    // invocation to return.
    [[nodiscard]] ContextCallback on_done(std::function<void()> callback) const;

private:
    friend class CancelScope;
    explicit Context(std::shared_ptr<detail::ContextState> state) noexcept;

    std::shared_ptr<detail::ContextState> state_;
};

// ContextCallback is the registration returned by Context::on_done.
class ContextCallback {
public:
    ContextCallback() noexcept = default;
    ContextCallback(ContextCallback&& other) noexcept;
    ContextCallback& operator=(ContextCallback&& other) noexcept;
    ContextCallback(const ContextCallback&) = delete;
    ContextCallback& operator=(const ContextCallback&) = delete;
    ~ContextCallback();

    // Unregisters the callback now.
    void reset() noexcept;

private:
    friend class Context;
    ContextCallback(std::shared_ptr<detail::ContextState> state,
                    std::shared_ptr<detail::ContextCallbackNode> node) noexcept;

    std::shared_ptr<detail::ContextState> state_;
    std::shared_ptr<detail::ContextCallbackNode> node_;
};

// CancelScope owns a derived context (context.WithCancel, WithTimeout and
// WithDeadline). The derived context is canceled when the parent is done,
// when cancel() is called, when its deadline passes, or when the scope is
// destroyed.
class CancelScope {
public:
    explicit CancelScope(const Context& parent);
    CancelScope(const Context& parent, Context::Clock::duration timeout);
    CancelScope(const Context& parent, Context::Clock::time_point deadline);
    CancelScope(const CancelScope&) = delete;
    CancelScope& operator=(const CancelScope&) = delete;
    ~CancelScope();

    [[nodiscard]] const Context& context() const noexcept { return context_; }
    void cancel() noexcept;

private:
    void init(const Context& parent, std::optional<Context::Clock::time_point> deadline);

    Context context_;
};

} // namespace puls
