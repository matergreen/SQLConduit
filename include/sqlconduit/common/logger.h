#ifndef SQLCONDUIT_COMMON_LOGGER_H
#define SQLCONDUIT_COMMON_LOGGER_H

#include <iostream>
#include <string>
#include <chrono>

namespace sqlconduit::common {
    enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

    inline const char *logLevelStr(LogLevel l) {
        switch (l) {
            case LogLevel::Debug: return "DEBUG";
            case LogLevel::Info: return "INFO";
            case LogLevel::Warn: return "WARN";
            case LogLevel::Error: return "ERROR";
        }
        return "?";
    }

    class Logger {
    public:
        static LogLevel minLevel() { return minLevel_; }
        static void setMinLevel(LogLevel l) { minLevel_ = l; }

        static void log(LogLevel level, const std::string &msg) {
            if (level < minLevel_) return;
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &t);
#else
            localtime_r(&t, &tm);
#endif
            char buf[32] = {0};
            std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
            std::cerr << "[" << logLevelStr(level) << "] " << buf << " " << msg << std::endl;
        }

    private:
        static LogLevel minLevel_;
    };

    inline LogLevel Logger::minLevel_ = LogLevel::Info;
}

#define SQLCONDUIT_LOG_DEBUG(m) ::sqlconduit::common::Logger::log(::sqlconduit::common::LogLevel::Debug, m)
#define SQLCONDUIT_LOG_INFO(m)  ::sqlconduit::common::Logger::log(::sqlconduit::common::LogLevel::Info,  m)
#define SQLCONDUIT_LOG_WARN(m)  ::sqlconduit::common::Logger::log(::sqlconduit::common::LogLevel::Warn,  m)
#define SQLCONDUIT_LOG_ERROR(m) ::sqlconduit::common::Logger::log(::sqlconduit::common::LogLevel::Error, m)

#endif
