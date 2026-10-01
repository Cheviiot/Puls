#include "puls/core/context.hpp"

#include <atomic>
#include <condition_variable>
#include <list>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

namespace puls {
namespace detail {

struct ContextCallbackNode {
    enum class State { registered, running, finished, removed };

    explicit ContextCallbackNode(std::function<void()> fn) : callback(std::move(fn)) {}

    std::function<void()> callback;
    State state = State::registered;
    bool listed = false;
    std::list<std::shared_ptr<ContextCallbackNode>>::iterator position;
    std::thread::id runner;
};

class ContextState : public std::enable_shared_from_this<ContextState> {
public:
    explicit ContextState(std::optional<Context::Clock::time_point> deadline)
        : deadline_(deadline) {}

    ~ContextState();

    ContextState(const ContextState&) = delete;
    ContextState& operator=(const ContextState&) = delete;

    // Side-effect free, so that it is safe to call while holding locks that
    // cancellation callbacks may also take. An expired deadline is reported
    // immediately; the timer service runs the callbacks.
    [[nodiscard]] CancelCause cause() const {
        const CancelCause current = cause_.load(std::memory_order_acquire);
        if (current == CancelCause::none && deadline_ && Context::Clock::now() >= *deadline_) {
            return CancelCause::deadline_exceeded;
        }
        return current;
    }

    [[nodiscard]] std::optional<Context::Clock::time_point> deadline() const { return deadline_; }

    void cancel(CancelCause cause);

    std::shared_ptr<ContextCallbackNode> add_callback(std::function<void()>& callback);
    void remove_callback(const std::shared_ptr<ContextCallbackNode>& node);
    bool wait_until(std::optional<Context::Clock::time_point> when);

    void set_parent_link(ContextCallback link);
    void set_timer(std::uint64_t id);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic<CancelCause> cause_{CancelCause::none};
    const std::optional<Context::Clock::time_point> deadline_;
    std::list<std::shared_ptr<ContextCallbackNode>> callbacks_;
    ContextCallback parent_link_;
    std::uint64_t timer_id_ = 0;
};

namespace {

// TimerService fires context deadlines. Like the Go runtime timer, it is a
// process-wide helper; it is intentionally never destroyed so that contexts
// released during static destruction remain safe.
class TimerService {
public:
    static TimerService& instance() {
        static TimerService* service = new TimerService();
        return *service;
    }

    std::uint64_t schedule(Context::Clock::time_point when, std::weak_ptr<ContextState> target) {
        std::lock_guard lock(mutex_);
        const std::uint64_t id = next_id_++;
        timers_.emplace(Key{when, id}, std::move(target));
        index_.emplace(id, when);
        if (!started_) {
            std::thread([this] { run(); }).detach();
            started_ = true;
        }
        changed_.notify_one();
        return id;
    }

    void cancel(std::uint64_t id) {
        if (id == 0) {
            return;
        }
        std::lock_guard lock(mutex_);
        const auto found = index_.find(id);
        if (found == index_.end()) {
            return;
        }
        timers_.erase(Key{found->second, id});
        index_.erase(found);
    }

private:
    using Key = std::pair<Context::Clock::time_point, std::uint64_t>;

    TimerService() = default;

    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            if (timers_.empty()) {
                changed_.wait(lock);
                continue;
            }
            const auto next = timers_.begin()->first.first;
            if (Context::Clock::now() < next) {
                changed_.wait_until(lock, next);
                continue;
            }
            auto target = timers_.begin()->second.lock();
            index_.erase(timers_.begin()->first.second);
            timers_.erase(timers_.begin());
            lock.unlock();
            if (target) {
                target->cancel(CancelCause::deadline_exceeded);
            }
            target.reset();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::map<Key, std::weak_ptr<ContextState>> timers_;
    std::unordered_map<std::uint64_t, Context::Clock::time_point> index_;
    std::uint64_t next_id_ = 1;
    bool started_ = false;
};

} // namespace

ContextState::~ContextState() {
    TimerService::instance().cancel(timer_id_);
}

void ContextState::set_parent_link(ContextCallback link) {
    std::unique_lock lock(mutex_);
    if (cause_.load(std::memory_order_relaxed) == CancelCause::none) {
        parent_link_ = std::move(link);
        return;
    }
    lock.unlock();
    link.reset();
}

void ContextState::set_timer(std::uint64_t id) {
    std::unique_lock lock(mutex_);
    if (cause_.load(std::memory_order_relaxed) == CancelCause::none) {
        timer_id_ = id;
        return;
    }
    lock.unlock();
    TimerService::instance().cancel(id);
}

void ContextState::cancel(CancelCause cause) {
    std::list<std::shared_ptr<ContextCallbackNode>> pending;
    ContextCallback parent_link;
    std::uint64_t timer_id = 0;
    {
        std::lock_guard lock(mutex_);
        if (cause_.load(std::memory_order_relaxed) != CancelCause::none) {
            return;
        }
        cause_.store(cause, std::memory_order_release);
        pending.swap(callbacks_);
        for (auto& node : pending) {
            node->listed = false;
        }
        parent_link = std::move(parent_link_);
        timer_id = std::exchange(timer_id_, 0);
    }
    changed_.notify_all();

    for (auto& node : pending) {
        {
            std::lock_guard lock(mutex_);
            if (node->state != ContextCallbackNode::State::registered) {
                continue;
            }
            node->state = ContextCallbackNode::State::running;
            node->runner = std::this_thread::get_id();
        }
        try {
            node->callback();
        } catch (...) {
            // Cancellation callbacks must not throw; a failing callback cannot
            // be allowed to prevent the remaining ones from running.
        }
        {
            std::lock_guard lock(mutex_);
            node->state = ContextCallbackNode::State::finished;
        }
        changed_.notify_all();
    }

    // A finished context no longer needs its deadline timer or the link that
    // propagates cancellation from its parent.
    TimerService::instance().cancel(timer_id);
    parent_link.reset();
}

std::shared_ptr<ContextCallbackNode> ContextState::add_callback(std::function<void()>& callback) {
    if (cause() != CancelCause::none) {
        return nullptr;
    }
    std::lock_guard lock(mutex_);
    if (cause_.load(std::memory_order_relaxed) != CancelCause::none) {
        return nullptr;
    }
    auto node = std::make_shared<ContextCallbackNode>(std::move(callback));
    callbacks_.push_back(node);
    node->position = std::prev(callbacks_.end());
    node->listed = true;
    return node;
}

void ContextState::remove_callback(const std::shared_ptr<ContextCallbackNode>& node) {
    std::unique_lock lock(mutex_);
    if (node->listed) {
        callbacks_.erase(node->position);
        node->listed = false;
        node->state = ContextCallbackNode::State::removed;
        return;
    }
    if (node->state == ContextCallbackNode::State::registered) {
        node->state = ContextCallbackNode::State::removed;
        return;
    }
    if (node->state == ContextCallbackNode::State::running &&
        node->runner != std::this_thread::get_id()) {
        changed_.wait(lock, [&] { return node->state != ContextCallbackNode::State::running; });
    }
}

bool ContextState::wait_until(std::optional<Context::Clock::time_point> when) {
    std::unique_lock lock(mutex_);
    for (;;) {
        if (cause_.load(std::memory_order_acquire) != CancelCause::none) {
            return true;
        }
        const auto now = Context::Clock::now();
        if (deadline_ && now >= *deadline_) {
            lock.unlock();
            cancel(CancelCause::deadline_exceeded);
            return true;
        }
        if (when && now >= *when) {
            return false;
        }
        std::optional<Context::Clock::time_point> wake = when;
        if (deadline_ && (!wake || *deadline_ < *wake)) {
            wake = deadline_;
        }
        if (wake) {
            changed_.wait_until(lock, *wake);
        } else {
            changed_.wait(lock);
        }
    }
}

} // namespace detail

Context::Context(std::shared_ptr<detail::ContextState> state) noexcept : state_(std::move(state)) {}

bool Context::done() const {
    return cause() != CancelCause::none;
}

CancelCause Context::cause() const {
    return state_ ? state_->cause() : CancelCause::none;
}

Error Context::err() const {
    switch (cause()) {
    case CancelCause::canceled:
        return errors::canceled();
    case CancelCause::deadline_exceeded:
        return errors::deadline_exceeded();
    case CancelCause::none:
        break;
    }
    return {};
}

std::optional<Context::Clock::time_point> Context::deadline() const {
    return state_ ? state_->deadline() : std::nullopt;
}

bool Context::wait_for(Clock::duration timeout) const {
    return wait_until(Clock::now() + timeout);
}

bool Context::wait_until(Clock::time_point when) const {
    if (!state_) {
        std::this_thread::sleep_until(when);
        return false;
    }
    return state_->wait_until(when);
}

void Context::wait() const {
    if (!state_) {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(24));
        }
    }
    state_->wait_until(std::nullopt);
}

ContextCallback Context::on_done(std::function<void()> callback) const {
    if (!state_) {
        return {};
    }
    auto node = state_->add_callback(callback);
    if (!node) {
        callback();
        return {};
    }
    return ContextCallback(state_, std::move(node));
}

ContextCallback::ContextCallback(std::shared_ptr<detail::ContextState> state,
                                 std::shared_ptr<detail::ContextCallbackNode> node) noexcept
    : state_(std::move(state)), node_(std::move(node)) {}

ContextCallback::ContextCallback(ContextCallback&& other) noexcept
    : state_(std::move(other.state_)), node_(std::move(other.node_)) {}

ContextCallback& ContextCallback::operator=(ContextCallback&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        node_ = std::move(other.node_);
    }
    return *this;
}

ContextCallback::~ContextCallback() {
    reset();
}

void ContextCallback::reset() noexcept {
    if (state_ && node_) {
        state_->remove_callback(node_);
    }
    node_.reset();
    state_.reset();
}

CancelScope::CancelScope(const Context& parent) {
    init(parent, std::nullopt);
}

CancelScope::CancelScope(const Context& parent, Context::Clock::duration timeout) {
    init(parent, Context::Clock::now() + timeout);
}

CancelScope::CancelScope(const Context& parent, Context::Clock::time_point deadline) {
    init(parent, deadline);
}

void CancelScope::init(const Context& parent, std::optional<Context::Clock::time_point> deadline) {
    if (const auto inherited = parent.deadline();
        inherited && (!deadline || *inherited < *deadline)) {
        deadline = inherited;
    }
    auto state = std::make_shared<detail::ContextState>(deadline);
    context_ = Context(state);
    if (parent.state_) {
        auto* parent_state = parent.state_.get();
        std::weak_ptr<detail::ContextState> child = state;
        state->set_parent_link(parent.on_done([child, parent_state] {
            if (auto target = child.lock()) {
                target->cancel(parent_state->cause());
            }
        }));
    }
    if (deadline && state->cause() == CancelCause::none) {
        state->set_timer(detail::TimerService::instance().schedule(*deadline, state));
    }
}

CancelScope::~CancelScope() {
    cancel();
}

void CancelScope::cancel() noexcept {
    if (context_.state_) {
        context_.state_->cancel(CancelCause::canceled);
    }
}

} // namespace puls
