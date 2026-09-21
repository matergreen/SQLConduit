#ifndef SQLCONDUIT_ASYNC_TASK_H
#define SQLCONDUIT_ASYNC_TASK_H

#if !defined(SQLCONDUIT_ENABLE_ASYNC_CORO)
#error "sqlconduit/async/task.h requires building sqlconduit with -DSQLCONDUIT_ENABLE_ASYNC_CORO=ON (C++20)"
#endif

#include "sqlconduit/async/sqlconduit_async.h"

#include <coroutine>
#include <exception>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

namespace sqlconduit::async
{
    template <class T>
    class Task;

    void run(Task<void> t);

    namespace detail
    {
        template <class T>
        class TaskPromise;

        template <class T>
        class TaskPromiseBase
        {
        public:
            Task<T> get_return_object() noexcept
            {
                return Task<T>{
                    std::coroutine_handle<TaskPromise<T>>::from_promise(
                        *static_cast<TaskPromise<T>*>(this))
                };
            }

            std::suspend_always initial_suspend() noexcept { return {}; }

            struct FinalAwaiter
            {
                bool await_ready() const noexcept { return false; }

                std::coroutine_handle<> await_suspend(
                    std::coroutine_handle<TaskPromise<T>> h) noexcept
                {
                    auto& p = h.promise();
                    std::coroutine_handle<> cont = p.continuation_;
                    if (cont == nullptr)
                    {
                        if (p.exception_) std::terminate();
                        h.destroy();
                        return std::noop_coroutine();
                    }
                    return cont;
                }

                void await_resume() const noexcept
                {
                }
            };

            FinalAwaiter final_suspend() noexcept { return {}; }

            void unhandled_exception() noexcept
            {
                exception_ = std::current_exception();
            }

        private:
            friend class Task<T>;

            std::coroutine_handle<> continuation_;
            std::exception_ptr exception_;
        };

        template <class T>
        class TaskPromise final : public TaskPromiseBase<T>
        {
        public:
            void return_value(T v) { value_.emplace(std::move(v)); }

        private:
            friend class Task<T>;

            std::optional<T> value_;
        };

        template <>
        class TaskPromise<void> final : public TaskPromiseBase<void>
        {
        public:
            void return_void() noexcept
            {
            }
        };

        template <class R>
        class OpAwaiter
        {
        public:
            using Callback = std::function<void(R&&)>;
            using Launcher = std::function<void(Callback)>;

            explicit OpAwaiter(Launcher launch) : launch_(std::move(launch))
            {
            }

            bool await_ready() const noexcept { return false; }

            void await_suspend(std::coroutine_handle<> self)
            {
                self_ = self;
                Launcher launch = std::move(launch_);
                launch(Callback([this](R&& r)
                {
                    result_ = std::move(r);
                    self_.resume();
                }));
            }

            R await_resume() { return std::move(result_); }

        private:
            Launcher launch_;
            R result_{};
            std::coroutine_handle<> self_;
        };
    }

    template <class T>
    class [[nodiscard]] Task
    {
    public:
        using promise_type = detail::TaskPromise<T>;

        Task(Task&& other) noexcept : h_(std::exchange(other.h_, {}))
        {
        }

        Task& operator=(Task&& other) noexcept
        {
            if (this != &other)
            {
                if (h_) h_.destroy();
                h_ = std::exchange(other.h_, {});
            }
            return *this;
        }

        Task(const Task&) = delete;

        Task& operator=(const Task&) = delete;

        ~Task()
        {
            if (h_) h_.destroy();
        }

        bool await_ready() const noexcept { return false; }

        std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
        {
            h_.promise().continuation_ = awaiting;
            return h_;
        }

        T await_resume()
        {
            auto& p = h_.promise();
            if (p.exception_) std::rethrow_exception(p.exception_);
            if constexpr (!std::is_void_v<T>)
                return std::move(*p.value_);
        }

    private:
        friend class detail::TaskPromiseBase<T>;

        friend void run(Task<void> t);

        explicit Task(std::coroutine_handle<promise_type> h) noexcept : h_(h)
        {
        }

        std::coroutine_handle<promise_type> h_;
    };

    inline void run(Task<void> t)
    {
        auto h = std::exchange(t.h_, {});
        if (h && !h.done()) h.resume();
    }

    Task<QueryResult> queryAsync(std::string sql, common::Params params = {},
                                 Options opts = {});

    Task<QueryResult> queryAsync(std::string dataSource, std::string sql,
                                 common::Params params, Options opts = {});

    Task<ExecResult> executeAsync(std::string sql, common::Params params = {},
                                  Options opts = {});

    Task<ExecKeysResult> executeAsync(std::string dataSource, std::string sql,
                                      common::Params params, Options opts = {});

    Task<ExecKeysResult> executeKeysAsync(std::string sql, common::Params params = {},
                                          Options opts = {});

    Task<BatchResult> executeBatchAsync(std::string sql, common::ParamBatch batch,
                                        Options opts = {});

    Task<OpResult> transactionAsync(common::TransactionOptions txOpts,
                                    core::SessionFn fn);
}

#endif
