#ifndef DBMW_COMMON_CONTEXT_H
#define DBMW_COMMON_CONTEXT_H

#include <cstddef>
#include <string>
#include <vector>

namespace dbmw::common {

    enum class Idempotency {
        Unspecified,
        Idempotent,
        NonIdempotent
    };

    struct SqlContext {
        std::string traceId;
        std::string spanId;
        std::string tenantId;
        std::string targetDataSource;
        bool shadow = false;
        bool wroteInThisRequest = false;
        Idempotency idempotency = Idempotency::Unspecified;

        [[nodiscard]] bool empty() const {
            return traceId.empty() && spanId.empty() && tenantId.empty()
                   && targetDataSource.empty() && !shadow && !wroteInThisRequest
                   && idempotency == Idempotency::Unspecified;
        }
    };

    class ContextScope {
    public:
        explicit ContextScope(SqlContext ctx);
        ~ContextScope();
        ContextScope(const ContextScope &) = delete;
        ContextScope &operator=(const ContextScope &) = delete;
        ContextScope(ContextScope &&) = delete;
        ContextScope &operator=(ContextScope &&) = delete;

        [[nodiscard]] static const SqlContext &current() noexcept;

        [[nodiscard]] static std::size_t depth() noexcept;

        static std::vector<SqlContext> &stack();
        static const SqlContext &defaultInstance();

        static constexpr std::size_t kMaxDepth = 64;

    private:
        bool entered_;
    };

    std::string nextSpanId();

    bool parseTraceparent(const std::string &header, SqlContext &out);

    std::string formatTraceparent(const SqlContext &ctx);

}

#endif
