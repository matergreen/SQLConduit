#include "operation_state.h"
#include "sqlconduit/core/idatabase_connection.h"
#include "sqlconduit/async/executor.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

namespace sqlconduit::common::detail {
    class StopSignal : public std::enable_shared_from_this<StopSignal> {
        struct Slot {
            std::recursive_mutex mutex;
            std::function<void()> callback;
            void run() {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                auto copy = callback;
                if (copy) copy();
            }
        };
        std::mutex mutex_;
        std::map<std::uint64_t, std::shared_ptr<Slot>> slots_;
        std::uint64_t next_ = 0;
    public:
        std::atomic<bool> requested{false};
        std::shared_ptr<void> subscribe(std::function<void()> callback) {
            auto slot = std::make_shared<Slot>(); slot->callback = std::move(callback);
            std::uint64_t id;
            bool call;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                id = ++next_; call = requested.load();
                if (!call) slots_.emplace(id, slot);
            }
            auto lease = std::shared_ptr<void>(slot.get(), [weak = weak_from_this(), id, slot](void *) {
                if (auto signal = weak.lock()) {
                    std::lock_guard<std::mutex> lock(signal->mutex_);
                    signal->slots_.erase(id);
                }
                // Wait for an in-progress cancel before its connection can be reused.
                std::lock_guard<std::recursive_mutex> lock(slot->mutex);
                slot->callback = {};
            });
            if (call) slot->run();
            return lease;
        }
        bool request() {
            std::vector<std::shared_ptr<Slot>> callbacks;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (requested.exchange(true)) return false;
                for (auto &entry : slots_) callbacks.push_back(entry.second);
            }
            for (auto &slot : callbacks) { try { slot->run(); } catch (...) {} }
            return true;
        }
    };

    namespace {
        using Clock = std::chrono::steady_clock;
        class Deadlines {
            struct Queue {
                std::mutex mutex;
                std::condition_variable cv;
                bool stop = false;
                std::map<std::pair<Clock::time_point, std::uint64_t>, std::weak_ptr<OperationState>> entries;
                std::uint64_t next = 0;
            };
            std::shared_ptr<Queue> queue_ = std::make_shared<Queue>();
            std::thread thread_;
        public:
            Deadlines() : thread_([queue = queue_] {
                std::unique_lock<std::mutex> lock(queue->mutex);
                while (!queue->stop) {
                    if (queue->entries.empty()) { queue->cv.wait(lock); continue; }
                    const auto first = queue->entries.begin();
                    const auto when = first->first.first;
                    if (when > Clock::now()) { queue->cv.wait_until(lock, when); continue; }
                    auto op = first->second.lock();
                    queue->entries.erase(first);
                    lock.unlock();
                    if (op) op->stop(ErrorCode::QueryTimeout);
                    op.reset();
                    lock.lock();
                }
            }) {}
            ~Deadlines() {
                { std::lock_guard<std::mutex> lock(queue_->mutex); queue_->stop = true; }
                queue_->cv.notify_all();
                if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
                else thread_.join();
            }
            std::uint64_t add(Clock::time_point when, std::weak_ptr<OperationState> op) {
                std::lock_guard<std::mutex> lock(queue_->mutex);
                const auto id = ++queue_->next;
                queue_->entries.emplace(std::make_pair(when, id), std::move(op));
                queue_->cv.notify_all();
                return id;
            }
            void remove(Clock::time_point when, std::uint64_t id) {
                std::lock_guard<std::mutex> lock(queue_->mutex);
                queue_->entries.erase({when, id}); queue_->cv.notify_all();
            }
        };
        std::shared_ptr<Deadlines> deadlines() {
            static auto scheduler = std::make_shared<Deadlines>();
            return scheduler;
        }
        std::shared_ptr<async::IExecutor> cancellations() {
            // Driver cancellation may block. Never run it on the deadline scheduler.
            static auto executor = async::makeThreadPoolExecutor(2, 1024);
            return executor;
        }
    }

    struct OperationState::Impl {
        std::shared_ptr<StopSignal> signal = std::make_shared<StopSignal>();
        std::atomic<ErrorCode> reason{ErrorCode::Ok};
        std::atomic<bool> writeStarted{false}, native{false};
        OperationOptions options;
        std::shared_ptr<void> tokenLease, parentLease;
        std::shared_ptr<Deadlines> scheduler;
        std::uint64_t timer = 0;
    };
    OperationState::OperationState(const OperationOptions &options) : impl_(std::make_shared<Impl>()) {
        impl_->options = options;
        if (options.timeout.count() > 0) {
            const auto now = Clock::now();
            const auto room = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - now);
            if (options.timeout < room) impl_->options.deadline = std::min(options.deadline, now + options.timeout);
        }
    }
    std::shared_ptr<OperationState> OperationState::create(const OperationOptions &options,
                                                          const std::shared_ptr<OperationState> &parent) {
        auto op = std::shared_ptr<OperationState>(new OperationState(options));
        const std::weak_ptr<OperationState> weak = op;
        if (parent) {
            op->impl_->options.deadline = std::min(op->deadline(), parent->deadline());
            op->impl_->options.allowCompatibilityFallback &= parent->fallbackAllowed();
            op->impl_->parentLease = parent->subscribe([weak, parent] {
                if (auto child = weak.lock()) child->stop(parent->status().code);
            });
        }
        if (options.cancellation.signal_) op->impl_->tokenLease = options.cancellation.signal_->subscribe([weak] {
            if (auto state = weak.lock()) state->stop(ErrorCode::Cancelled);
        });
        if (options.timeout.count() < 0) op->stop(ErrorCode::ConfigError);
        if (op->deadline() != Clock::time_point::max()) {
            op->impl_->scheduler = deadlines();
            op->impl_->timer = op->impl_->scheduler->add(op->deadline(), op);
        }
        return op;
    }
    OperationState::~OperationState() { finish(); }
    void OperationState::finish() {
        if (impl_->scheduler) impl_->scheduler->remove(deadline(), impl_->timer);
    }
    bool OperationState::stop(ErrorCode code) {
        if (code == ErrorCode::Ok) return false;
        auto expected = ErrorCode::Ok;
        if (!impl_->reason.compare_exchange_strong(expected, code)) return false;
        return impl_->signal->request();
    }
    Status OperationState::status() const {
        auto code = impl_->reason.load();
        if (code == ErrorCode::Ok && Clock::now() >= deadline()) code = ErrorCode::QueryTimeout;
        if (code == ErrorCode::Ok) return {};
        auto status = Status::error(code, code == ErrorCode::Cancelled ? "operation cancelled" :
            code == ErrorCode::QueryTimeout ? "operation deadline exceeded" : "invalid operation options");
        status.outcomeUnknown = impl_->writeStarted.load();
        return status;
    }
    std::chrono::milliseconds OperationState::remaining(std::chrono::milliseconds other) const {
        if (deadline() == Clock::time_point::max()) return other;
        const auto left = std::max(std::chrono::milliseconds(0),
            std::chrono::ceil<std::chrono::milliseconds>(deadline() - Clock::now()));
        return other.count() < 0 ? left : std::min(other, left);
    }
    Clock::time_point OperationState::deadline() const { return impl_->options.deadline; }
    bool OperationState::native() const { return impl_->native.load(); }
    bool OperationState::fallbackAllowed() const { return impl_->options.allowCompatibilityFallback; }
    std::shared_ptr<void> OperationState::subscribe(std::function<void()> callback) {
        return impl_->signal->subscribe(std::move(callback));
    }
    std::shared_ptr<void> OperationState::bind(core::IDatabaseConnection *connection, bool write, bool native) {
        if (write) impl_->writeStarted = true;
        impl_->native = native;
        struct Binding { std::mutex mutex; bool active = true; core::IDatabaseConnection *connection; };
        auto binding = std::make_shared<Binding>(); binding->connection = connection;
        auto executor = cancellations();
        auto subscription = subscribe([binding, executor] {
            (void) executor->tryPost([binding] {
                std::lock_guard<std::mutex> lock(binding->mutex);
                if (binding->active && binding->connection) (void) binding->connection->cancel();
            });
        });
        return std::shared_ptr<void>(binding.get(), [binding, subscription = std::move(subscription)](void *) mutable {
            subscription.reset();
            std::lock_guard<std::mutex> lock(binding->mutex);
            binding->active = false;
        });
    }
}

namespace sqlconduit::common {
    CancellationSource::CancellationSource() : signal_(std::make_shared<detail::StopSignal>()) {}
    CancellationToken CancellationSource::token() const noexcept { CancellationToken token; token.signal_ = signal_; return token; }
    bool CancellationSource::requestCancellation() const { return signal_ && signal_->request(); }
    bool CancellationToken::cancellationRequested() const noexcept { return signal_ && signal_->requested.load(); }
    namespace {
        SqlContext operationContext(const OperationOptions &options) {
            auto context = ContextScope::current();
            context.operation = detail::OperationState::create(options, context.operation);
            if (!options.dataSource.empty()) context.targetDataSource = options.dataSource;
            if (options.idempotency != Idempotency::Unspecified) context.idempotency = options.idempotency;
            return context;
        }
    }
    OperationScope::OperationScope(const OperationOptions &options) : scope_(operationContext(options)) {}
}
