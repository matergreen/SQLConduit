#ifndef DBMW_CORE_RATE_LIMITER_H
#define DBMW_CORE_RATE_LIMITER_H

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace dbmw::core {
    class TokenBucket {
    public:
        TokenBucket(double ratePerSec, double burst)
            : tokens_(burst), rate_(ratePerSec), burst_(burst), last_(clock::now()) {
        }

        bool tryAcquire() {
            std::lock_guard<std::mutex> lk(mtx_);
            const auto now = clock::now();
            const double elapsed = std::chrono::duration<double>(now - last_).count();
            last_ = now;
            tokens_ += elapsed * rate_;
            if (tokens_ > burst_) tokens_ = burst_;
            if (tokens_ >= 1.0) {
                tokens_ -= 1.0;
                return true;
            }
            return false;
        }

    private:
        using clock = std::chrono::steady_clock;
        std::mutex mtx_;
        double tokens_;
        double rate_;
        double burst_;
        clock::time_point last_;
    };

    class IRateLimiter {
    public:
        virtual ~IRateLimiter() = default;

        virtual bool acquire(std::uint64_t fingerprint) = 0;

        virtual bool usesFingerprint() const { return false; }
    };

    class RateLimiter : public IRateLimiter {
    public:
        RateLimiter(double globalQps, double perFpQps, int burst, std::string fpMode)
            : perFpQps_(perFpQps), burst_(burst), fpMode_(std::move(fpMode)) {
            if (globalQps > 0) {
                const double b = burst > 0 ? static_cast<double>(burst) : globalQps;
                global_ = std::make_shared<TokenBucket>(globalQps, b);
            }
        }

        bool usesFingerprint() const override {
            return fpMode_ != "off" && perFpQps_ > 0;
        }

        bool acquire(std::uint64_t fp) override {
            if (global_ && !global_->tryAcquire()) return false;
            if (fp != 0 && fpMode_ != "off" && perFpQps_ > 0) {
                std::shared_ptr<TokenBucket> bucket;
                {
                    std::lock_guard<std::mutex> lk(mapMtx_);
                    auto it = fpBuckets_.find(fp);
                    if (it == fpBuckets_.end()) {
                        if (fpBuckets_.size() >= kFpCap) return true;
                        const double b = burst_ > 0 ? static_cast<double>(burst_) : perFpQps_;
                        bucket = std::make_shared<TokenBucket>(perFpQps_, b);
                        fpBuckets_[fp] = bucket;
                    } else {
                        bucket = it->second;
                    }
                }
                if (!bucket->tryAcquire()) return false;
            }
            return true;
        }

    private:
        static constexpr std::size_t kFpCap = 1024;
        std::shared_ptr<TokenBucket> global_;
        double perFpQps_;
        int burst_;
        std::string fpMode_;
        std::mutex mapMtx_;
        std::unordered_map<std::uint64_t, std::shared_ptr<TokenBucket> > fpBuckets_;
    };
}

#endif
