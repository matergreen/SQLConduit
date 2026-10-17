#include "../src/driver/odbc_unicode.h"
#include <cstdint>
#include <iostream>
#include <string>

int main() {
    using sqlconduit::driver::detail::OdbcWideText;
    int failed = 0;
    const auto check = [&](bool ok, const char *name) {
        if (!ok) { ++failed; std::cerr << name << '\n'; }
    };
    OdbcWideText<std::uint16_t> utf16;
    const std::uint16_t first[] = {0x6570, 0, 'A', 0xd83d};
    const std::uint16_t second[] = {0xde00, 0x636e};
    check(utf16.append(first, 4) && !utf16.complete(), "retain split high surrogate");
    check(utf16.append(second, 2) && utf16.complete(), "complete split surrogate pair");
    const std::string expected = std::string(u8"数") + '\0' + u8"A😀据";
    check(utf16.take() == expected, "UTF-16 chunks preserve CJK, supplementary characters and NUL");
    utf16.clear();
    check(utf16.complete() && utf16.take().empty(), "empty value after reset");
    const std::uint16_t orphan[] = {0xdc00};
    check(!utf16.append(orphan, 1), "reject unpaired low surrogate");
    utf16.clear();
    const std::uint16_t high[] = {0xd800};
    const std::uint16_t ascii[] = {'x'};
    check(utf16.append(high, 1) && !utf16.append(ascii, 1), "reject missing low surrogate");
    OdbcWideText<std::uint32_t> utf32;
    const std::uint32_t points[] = {0x6570, 0, 0x1f600};
    check(utf32.append(points, 3) && utf32.complete() &&
          utf32.take() == std::string(u8"数") + '\0' + u8"😀", "UTF-32 driver-manager support");
    const std::uint32_t invalid[] = {0x110000};
    check(!utf32.append(invalid, 1), "reject out-of-range code point");
    return failed ? 1 : 0;
}
