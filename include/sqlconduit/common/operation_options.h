#ifndef SQLCONDUIT_COMMON_OPERATION_OPTIONS_H
#define SQLCONDUIT_COMMON_OPERATION_OPTIONS_H

#include "sqlconduit/common/context.h"
#include "sqlconduit/common/named_params.h"
#include <chrono>
#include <memory>
#include <type_traits>
#include <utility>

namespace sqlconduit::common {
    namespace detail { class StopSignal; class OperationState; }

    class CancellationToken {
    public:
        CancellationToken() = default;
        [[nodiscard]] bool cancellationRequested() const noexcept;
    private:
        friend class CancellationSource;
        friend class detail::OperationState;
        std::shared_ptr<detail::StopSignal> signal_;
    };

    class CancellationSource {
    public:
        CancellationSource();
        [[nodiscard]] CancellationToken token() const noexcept;
        // Idempotent. Registered cancellation callbacks run on the requesting thread.
        bool requestCancellation() const;
    private:
        std::shared_ptr<detail::StopSignal> signal_;
    };

    struct OperationOptions {
        std::chrono::milliseconds timeout{0}; // 0: no additional relative limit
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
        CancellationToken cancellation;
        std::string dataSource;
        Idempotency idempotency = Idempotency::Unspecified;
        bool allowCompatibilityFallback = true;
    };

    // A scope creates a fresh budget. Async requests retain it through their captured context.
    class OperationScope {
    public:
        explicit OperationScope(const OperationOptions &options);
        OperationScope(const OperationScope &) = delete;
        OperationScope &operator=(const OperationScope &) = delete;
    private:
        ContextScope scope_;
    };
}

namespace sqlconduit {
    // A non-owning, reusable view: the Client/Session must outlive calls through this view.
    // Each call creates its own deadline, even if the view is reused.
    template<class Owner>
    class OperationView {
    public:
        OperationView(Owner &owner, common::OperationOptions options)
            : owner_(owner), options_(std::move(options)) {}
#define SQLCONDUIT_OPERATION_FORWARD(method, ParamsType) \
        template<class T = void> \
        decltype(auto) method(const std::string &sql, const common::ParamsType &params = {}) const { \
            common::OperationScope scope(options_); \
            if constexpr (std::is_void_v<T>) return owner_.method(sql, params); \
            else return owner_.template method<T>(sql, params); \
        } \
        template<class T = void> \
        decltype(auto) method(const std::string &source, const std::string &sql, const common::ParamsType &params) const { \
            common::OperationScope scope(options_); \
            if constexpr (std::is_void_v<T>) return owner_.method(source, sql, params); \
            else return owner_.template method<T>(source, sql, params); \
        } \
        template<class T = void, class... Args> \
        decltype(auto) method(Args &&...args) const { \
            common::OperationScope scope(options_); \
            if constexpr (std::is_void_v<T>) return owner_.method(std::forward<Args>(args)...); \
            else return owner_.template method<T>(std::forward<Args>(args)...); \
        }
        SQLCONDUIT_OPERATION_FORWARD(query, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryOne, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryScalar, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryColumn, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryNamed, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryOneNamed, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryScalarNamed, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryColumnNamed, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryAsync, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryOneAsync, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryScalarAsync, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryColumnAsync, Params)
        SQLCONDUIT_OPERATION_FORWARD(queryNamedAsync, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryOneNamedAsync, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryScalarNamedAsync, NamedParams)
        SQLCONDUIT_OPERATION_FORWARD(queryColumnNamedAsync, NamedParams)
#undef SQLCONDUIT_OPERATION_FORWARD
        // Writes have no typed result overload. Do not form method<T> even in a
        // discarded if constexpr branch: MSVC still diagnoses the non-template name.
#define SQLCONDUIT_OPERATION_WRITE_FORWARD(method, ParamsType) \
        decltype(auto) method(const std::string &sql, const common::ParamsType &params = {}) const { \
            common::OperationScope scope(options_); \
            return owner_.method(sql, params); \
        } \
        decltype(auto) method(const std::string &source, const std::string &sql, const common::ParamsType &params) const { \
            common::OperationScope scope(options_); \
            return owner_.method(source, sql, params); \
        } \
        template<class... Args> \
        decltype(auto) method(Args &&...args) const { \
            common::OperationScope scope(options_); \
            return owner_.method(std::forward<Args>(args)...); \
        }
        SQLCONDUIT_OPERATION_WRITE_FORWARD(execute, Params)
        SQLCONDUIT_OPERATION_WRITE_FORWARD(executeNamed, NamedParams)
        SQLCONDUIT_OPERATION_WRITE_FORWARD(executeAsync, Params)
        SQLCONDUIT_OPERATION_WRITE_FORWARD(executeNamedAsync, NamedParams)
#undef SQLCONDUIT_OPERATION_WRITE_FORWARD
    private:
        Owner &owner_;
        common::OperationOptions options_;
    };
}
#endif
