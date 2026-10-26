#include "core/TextEncoding.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <iconv.h>
#endif

namespace opencdc {

namespace {

void appendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string fromUtf16(std::string_view bytes, bool bigEndian) {
    std::string out;
    out.reserve(bytes.size());
    for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
        const auto b0 = static_cast<unsigned char>(bytes[i]);
        const auto b1 = static_cast<unsigned char>(bytes[i + 1]);
        std::uint32_t u = bigEndian ? (static_cast<std::uint32_t>(b0) << 8) | b1 : (static_cast<std::uint32_t>(b1) << 8) | b0;
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < bytes.size()) {
            const auto c0 = static_cast<unsigned char>(bytes[i + 2]);
            const auto c1 = static_cast<unsigned char>(bytes[i + 3]);
            const std::uint32_t lo =
                bigEndian ? (static_cast<std::uint32_t>(c0) << 8) | c1 : (static_cast<std::uint32_t>(c1) << 8) | c0;
            if (lo >= 0xDC00 && lo < 0xE000) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

/// GB18030 -> UTF-8; nullopt when the bytes are not valid GB18030.
std::optional<std::string> fromGb18030(std::string_view bytes) {
#ifdef _WIN32
    constexpr UINT kGb18030 = 54936;
    const int n = MultiByteToWideChar(kGb18030, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()),
                                      nullptr, 0);
    if (n <= 0)
        return std::nullopt;
    std::wstring wide(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(kGb18030, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), wide.data(), n);
    const int m = WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(m), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), n, out.data(), m, nullptr, nullptr);
    return out;
#else
    iconv_t cd = iconv_open("UTF-8", "GB18030");
    if (cd == reinterpret_cast<iconv_t>(-1))
        return std::nullopt;
    std::string out;
    out.resize(bytes.size() * 2 + 16);
    std::string in(bytes);
    char* inPtr = in.data();
    std::size_t inLeft = in.size();
    std::size_t done = 0;
    bool ok = true;
    while (inLeft > 0) {
        char* outPtr = out.data() + done;
        std::size_t outLeft = out.size() - done;
        const std::size_t r = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
        done = out.size() - outLeft;
        if (r == static_cast<std::size_t>(-1)) {
            if (errno == E2BIG) {
                out.resize(out.size() * 2);
                continue;
            }
            ok = false; // EILSEQ / EINVAL: not GB18030
            break;
        }
    }
    iconv_close(cd);
    if (!ok)
        return std::nullopt;
    out.resize(done);
    return out;
#endif
}

std::string fromWindows1252(std::string_view bytes) {
    // 0x80..0x9F differ from Latin-1
    static constexpr std::array<std::uint16_t, 32> kHigh = {
        0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
        0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
        0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178};
    std::string out;
    out.reserve(bytes.size() + bytes.size() / 4);
    for (const char ch : bytes) {
        const auto b = static_cast<unsigned char>(ch);
        appendUtf8(out, b >= 0x80 && b < 0xA0 ? kHigh[b - 0x80] : b);
    }
    return out;
}

} // namespace

bool isValidUtf8(std::string_view s) noexcept {
    std::size_t i = 0;
    while (i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        std::size_t n = 0;
        std::uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        }
        else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        }
        else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        }
        else {
            return false;
        }
        if (i + n >= s.size())
            return false;
        for (std::size_t k = 1; k <= n; ++k) {
            const auto cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        // overlong forms, surrogates, beyond U+10FFFF
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp < 0xE000))
            return false;
        i += n + 1;
    }
    return true;
}

DecodedText decodeSourceText(std::string_view bytes) {
    if (bytes.size() >= 3 && bytes.substr(0, 3) == "\xEF\xBB\xBF" && isValidUtf8(bytes.substr(3)))
        return {std::string(bytes.substr(3)), "UTF-8 with BOM"};
    if (bytes.size() >= 2 && bytes.substr(0, 2) == "\xFF\xFE")
        return {fromUtf16(bytes.substr(2), false), "UTF-16LE"};
    if (bytes.size() >= 2 && bytes.substr(0, 2) == "\xFE\xFF")
        return {fromUtf16(bytes.substr(2), true), "UTF-16BE"};
    if (isValidUtf8(bytes))
        return {std::string(bytes), "UTF-8"};
    if (auto gb = fromGb18030(bytes))
        return {std::move(*gb), "GB18030"};
    return {fromWindows1252(bytes), "Windows-1252"};
}

std::optional<DecodedText> readSourceText(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return decodeSourceText(ss.str());
}

} // namespace opencdc
