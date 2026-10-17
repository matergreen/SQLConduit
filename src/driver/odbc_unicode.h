#ifndef SQLCONDUIT_DRIVER_ODBC_UNICODE_H
#define SQLCONDUIT_DRIVER_ODBC_UNICODE_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace sqlconduit::driver::detail {
    // SQLWCHAR is UTF-16 on Windows/unixODBC and can be UTF-32 with other managers.
    // Keep a trailing high surrogate across SQLGetData chunks. Lengths, not NUL
    // terminators, delimit input so embedded U+0000 remains part of the value.
    template<class Unit>
    class OdbcWideText {
    public:
        bool append(const Unit *units, std::size_t count) {
            for (std::size_t i = 0; i < count; ++i) {
                std::uint32_t cp = static_cast<std::uint32_t>(units[i]);
                if constexpr (sizeof(Unit) == 2) {
                    if (pending_) {
                        if (cp < 0xdc00 || cp > 0xdfff) return false;
                        cp = 0x10000 + ((pending_ - 0xd800) << 10) + (cp - 0xdc00);
                        pending_ = 0;
                    } else if (cp >= 0xd800 && cp <= 0xdbff) {
                        pending_ = cp;
                        continue;
                    } else if (cp >= 0xdc00 && cp <= 0xdfff) return false;
                }
                if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
                if (cp < 0x80) text_.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    text_.push_back(static_cast<char>(0xc0 | (cp >> 6)));
                    text_.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                } else if (cp < 0x10000) {
                    text_.push_back(static_cast<char>(0xe0 | (cp >> 12)));
                    text_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                    text_.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                } else {
                    text_.push_back(static_cast<char>(0xf0 | (cp >> 18)));
                    text_.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
                    text_.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
                    text_.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
                }
            }
            return true;
        }

        bool complete() const { return pending_ == 0; }
        std::string take() { return std::move(text_); }
        void clear() { text_.clear(); pending_ = 0; }

    private:
        std::uint32_t pending_ = 0;
        std::string text_;
    };
}

#endif
