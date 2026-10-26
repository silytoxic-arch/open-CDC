#include "core/TextEncoding.h"

#include <catch2/catch_test_macros.hpp>

using namespace opencdc;

// "数据位宽" in GBK and in UTF-8
constexpr const char* kGbk = "\xCA\xFD\xBE\xDD\xCE\xBB\xBF\xED";
constexpr const char* kUtf8 = "\xE6\x95\xB0\xE6\x8D\xAE\xE4\xBD\x8D\xE5\xAE\xBD";

TEST_CASE("UTF-8 validation", "[core][encoding]") {
    CHECK(isValidUtf8("module m; endmodule"));
    CHECK(isValidUtf8(kUtf8));
    CHECK(isValidUtf8("\xF0\x9F\x98\x80")); // 4-byte sequence
    // negative
    CHECK_FALSE(isValidUtf8(kGbk));
    CHECK_FALSE(isValidUtf8("\xC0\xAF"));         // overlong '/'
    CHECK_FALSE(isValidUtf8("\xED\xA0\x80"));     // surrogate
    CHECK_FALSE(isValidUtf8("\xE6\x95"));         // truncated
    CHECK_FALSE(isValidUtf8("\xF4\x90\x80\x80")); // beyond U+10FFFF
}

TEST_CASE("Source text decoding: UTF-8, BOM, UTF-16, GB18030, Windows-1252", "[core][encoding]") {
    const std::string comment = std::string("input clk, //") + kGbk + "\n";
    auto gbk = decodeSourceText(comment);
    CHECK(gbk.encoding == "GB18030");
    CHECK(gbk.utf8 == std::string("input clk, //") + kUtf8 + "\n");
    CHECK(gbk.transcoded());

    auto plain = decodeSourceText(std::string("// ") + kUtf8);
    CHECK(plain.encoding == "UTF-8");
    CHECK_FALSE(plain.transcoded());

    auto bom = decodeSourceText("\xEF\xBB\xBFmodule m;");
    CHECK(bom.encoding == "UTF-8 with BOM");
    CHECK(bom.utf8 == "module m;");

    auto u16 = decodeSourceText(std::string("\xFF\xFE\x61\x00\x62\x00\x2D\x4E", 8));
    CHECK(u16.encoding == "UTF-16LE");
    CHECK(u16.utf8 == "ab\xE4\xB8\xAD");

    // French comment in Windows-1252: 0xE0 followed by a space is not GB18030
    auto latin = decodeSourceText("// d\xE9j\xE0 vu\n");
    CHECK(latin.encoding == "Windows-1252");
    CHECK(latin.utf8 == "// d\xC3\xA9j\xC3\xA0 vu\n");
}
