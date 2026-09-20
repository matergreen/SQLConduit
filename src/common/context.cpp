#include "dbmw/common/context.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace dbmw::common {
    namespace {
        bool isHex(char c) noexcept {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        }

        bool copyHex(const std::string &s, std::size_t pos, std::size_t len,
                     std::string &out) {
            if (pos + len > s.size()) return false;
            out.resize(len);
            for (std::size_t i = 0; i < len; ++i) {
                char c = s[pos + i];
                if (!isHex(c)) return false;
                out[i] = c;
            }
            return true;
        }

        constexpr char kHex[] = "0123456789abcdef";
    }

    std::vector<SqlContext> &ContextScope::stack() {
        static thread_local std::vector<SqlContext> s;
        return s;
    }

    const SqlContext &ContextScope::defaultInstance() {
        static const SqlContext kDefault{};
        return kDefault;
    }

    ContextScope::ContextScope(SqlContext ctx) : entered_(false) {
        auto &s = stack();
        if (s.size() < kMaxDepth) {
            s.push_back(std::move(ctx));
            entered_ = true;
            return;
        }
        static std::atomic<bool> warned{false};
        if (!warned.exchange(true)) {
            std::fprintf(stderr,
                         "dbmw: common::ContextScope stack exceeded %zu levels; "
                         "new scopes will not be pushed (possible recursive SQL in an interceptor).\n",
                         kMaxDepth);
        }
    }

    ContextScope::~ContextScope() {
        if (entered_) {
            stack().pop_back();
        }
    }

    const SqlContext &ContextScope::current() noexcept {
        auto &s = stack();
        if (s.empty()) return defaultInstance();
        return s.back();
    }

    std::size_t ContextScope::depth() noexcept {
        return stack().size();
    }

    std::string nextSpanId() {
        const auto &ctx = ContextScope::current();
        if (ctx.traceId.empty() && ctx.spanId.empty()) {
            return {};
        }
        static std::atomic<std::uint64_t> globalSeq{0};
        std::uint64_t seq = globalSeq.fetch_add(1, std::memory_order_relaxed) + 1;
        std::array<char, 16> buf{};
        for (int i = 15; i >= 0; --i) {
            buf[i] = kHex[seq & 0x0F];
            seq >>= 4;
        }
        return std::string(buf.data(), buf.size());
    }

    bool parseTraceparent(const std::string &header, SqlContext &out) {
        constexpr std::size_t kTotalLen = 55;
        if (header.size() != kTotalLen) return false;
        if (header[0] != '0' || header[1] != '0') return false;
        if (header[2] != '-' || header[35] != '-' || header[52] != '-') return false;

        SqlContext tmp;
        if (!copyHex(header, 3, 32, tmp.traceId)) return false;
        if (!copyHex(header, 36, 16, tmp.spanId)) return false;
        std::string flagsBuf;
        if (!copyHex(header, 53, 2, flagsBuf)) return false;

        bool allZero = true;
        for (char c: tmp.traceId) {
            if (c != '0') {
                allZero = false;
                break;
            }
        }
        if (allZero) return false;

        allZero = true;
        for (char c: tmp.spanId) {
            if (c != '0') {
                allZero = false;
                break;
            }
        }
        if (allZero) return false;

        out = std::move(tmp);
        return true;
    }

    std::string formatTraceparent(const SqlContext &ctx) {
        if (ctx.traceId.size() != 32 || ctx.spanId.size() != 16) return {};
        if (!std::all_of(ctx.traceId.begin(), ctx.traceId.end(), isHex) ||
            !std::all_of(ctx.spanId.begin(), ctx.spanId.end(), isHex))
            return {};
        if (std::all_of(ctx.traceId.begin(), ctx.traceId.end(), [](char c) { return c == '0'; }) ||
            std::all_of(ctx.spanId.begin(), ctx.spanId.end(), [](char c) { return c == '0'; }))
            return {};
        std::string out;
        out.reserve(55);
        out.append("00-");
        out.append(ctx.traceId);
        out.append("-");
        out.append(ctx.spanId);
        out.append("-01");
        return out;
    }
}
