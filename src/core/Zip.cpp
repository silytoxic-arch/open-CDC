#include "core/Zip.h"

#include <array>
#include <fstream>
#include <iterator>
#include <vector>

namespace opencdc {

namespace {

/// Bit reader, least significant bit first (RFC 1951 section 3.1.1).
class Bits {
public:
    explicit Bits(std::string_view d) : d_(d) {}
    /// n <= 24 bits; false at the end of the input.
    bool get(unsigned n, std::uint32_t& out) {
        while (count_ < n) {
            if (pos_ >= d_.size())
                return false;
            buf_ |= static_cast<std::uint32_t>(static_cast<unsigned char>(d_[pos_++])) << count_;
            count_ += 8;
        }
        out = buf_ & ((1u << n) - 1u);
        buf_ >>= n;
        count_ -= n;
        return true;
    }
    void alignToByte() {
        buf_ = 0;
        count_ = 0;
    }
    bool bytes(std::size_t n, std::string& out) {
        if (pos_ + n > d_.size())
            return false;
        out.append(d_.substr(pos_, n));
        pos_ += n;
        return true;
    }

private:
    std::string_view d_;
    std::size_t pos_ = 0;
    std::uint32_t buf_ = 0;
    unsigned count_ = 0;
};

/// Canonical Huffman code (as in zlib's puff): symbols ordered by code length.
struct Huffman {
    std::array<std::uint16_t, 16> count{};
    std::vector<std::uint16_t> symbol;

    bool build(const std::uint8_t* lengths, std::size_t n) {
        count.fill(0);
        for (std::size_t i = 0; i < n; ++i)
            ++count[lengths[i]];
        if (count[0] == n) // no codes: valid only for an unused distance table
            return true;
        int left = 1;
        for (std::size_t len = 1; len < 16; ++len) {
            left <<= 1;
            left -= count[len];
            if (left < 0)
                return false; // over-subscribed
        }
        std::array<std::uint16_t, 16> offs{};
        for (std::size_t len = 1; len < 15; ++len)
            offs[len + 1] = static_cast<std::uint16_t>(offs[len] + count[len]);
        symbol.assign(n, 0);
        for (std::size_t i = 0; i < n; ++i)
            if (lengths[i] != 0)
                symbol[offs[lengths[i]]++] = static_cast<std::uint16_t>(i);
        return true;
    }

    /// -1 at the end of the input or for an invalid code.
    int decode(Bits& in) const {
        int code = 0, first = 0, index = 0;
        for (std::size_t len = 1; len < 16; ++len) {
            std::uint32_t b = 0;
            if (!in.get(1, b))
                return -1;
            code |= static_cast<int>(b);
            const int c = count[len];
            if (code - c < first)
                return symbol.at(static_cast<std::size_t>(index + (code - first)));
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr std::array<std::uint16_t, 29> kLenBase = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLenExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                    2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,
                                                     33,  49,  65,  97,  129, 193,  257,  385,  513,  769,
                                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistExtra = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(Bits& in, std::string& out, const Huffman& lit, const Huffman& dist, std::size_t limit) {
    for (;;) {
        const int sym = lit.decode(in);
        if (sym < 0)
            return false;
        if (sym < 256) {
            out.push_back(static_cast<char>(sym));
        }
        else if (sym == 256) {
            return true;
        }
        else {
            const auto li = static_cast<std::size_t>(sym - 257);
            if (li >= kLenBase.size())
                return false;
            std::uint32_t extra = 0;
            if (!in.get(kLenExtra[li], extra))
                return false;
            const std::size_t len = kLenBase[li] + extra;
            const int ds = dist.decode(in);
            if (ds < 0 || static_cast<std::size_t>(ds) >= kDistBase.size())
                return false;
            if (!in.get(kDistExtra[static_cast<std::size_t>(ds)], extra))
                return false;
            const std::size_t d = kDistBase[static_cast<std::size_t>(ds)] + extra;
            if (d > out.size())
                return false;
            for (std::size_t k = 0; k < len; ++k)
                out.push_back(out[out.size() - d]);
        }
        if (out.size() > limit)
            return false;
    }
}

std::uint32_t le(std::string_view d, std::size_t at, unsigned n) {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i)
        v |= static_cast<std::uint32_t>(static_cast<unsigned char>(d[at + i])) << (8 * i);
    return v;
}

} // namespace

std::optional<std::string> inflate(std::string_view data, std::size_t limit) {
    Bits in(data);
    std::string out;
    std::uint32_t last = 0;
    do {
        std::uint32_t type = 0;
        if (!in.get(1, last) || !in.get(2, type))
            return std::nullopt;
        if (type == 0) { // stored
            in.alignToByte();
            std::string hdr;
            if (!in.bytes(4, hdr))
                return std::nullopt;
            const std::uint32_t len = le(hdr, 0, 2), nlen = le(hdr, 2, 2);
            if ((len ^ 0xFFFFu) != nlen || !in.bytes(len, out))
                return std::nullopt;
        }
        else if (type == 1) { // fixed codes
            static const auto fixed = [] {
                std::array<std::uint8_t, 288 + 30> l{};
                for (std::size_t i = 0; i < 144; ++i)
                    l[i] = 8;
                for (std::size_t i = 144; i < 256; ++i)
                    l[i] = 9;
                for (std::size_t i = 256; i < 280; ++i)
                    l[i] = 7;
                for (std::size_t i = 280; i < 288; ++i)
                    l[i] = 8;
                for (std::size_t i = 288; i < l.size(); ++i)
                    l[i] = 5;
                std::pair<Huffman, Huffman> h;
                h.first.build(l.data(), 288);
                h.second.build(l.data() + 288, 30);
                return h;
            }();
            if (!codes(in, out, fixed.first, fixed.second, limit))
                return std::nullopt;
        }
        else if (type == 2) { // dynamic codes
            std::uint32_t hlit = 0, hdist = 0, hclen = 0;
            if (!in.get(5, hlit) || !in.get(5, hdist) || !in.get(4, hclen))
                return std::nullopt;
            hlit += 257;
            hdist += 1;
            hclen += 4;
            if (hlit > 286 || hdist > 30)
                return std::nullopt;
            static constexpr std::array<std::uint8_t, 19> order = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                                                   11, 4,  12, 3, 13, 2, 14, 1, 15};
            std::array<std::uint8_t, 19> cl{};
            for (std::uint32_t i = 0; i < hclen; ++i) {
                std::uint32_t v = 0;
                if (!in.get(3, v))
                    return std::nullopt;
                cl[order[i]] = static_cast<std::uint8_t>(v);
            }
            Huffman lencode;
            if (!lencode.build(cl.data(), cl.size()))
                return std::nullopt;
            std::array<std::uint8_t, 320> lengths{};
            std::uint32_t idx = 0;
            while (idx < hlit + hdist) {
                const int sym = lencode.decode(in);
                if (sym < 0)
                    return std::nullopt;
                if (sym < 16) {
                    lengths[idx++] = static_cast<std::uint8_t>(sym);
                    continue;
                }
                std::uint8_t value = 0;
                std::uint32_t rep = 0;
                if (sym == 16) {
                    if (idx == 0 || !in.get(2, rep))
                        return std::nullopt;
                    value = lengths[idx - 1];
                    rep += 3;
                }
                else if (sym == 17) {
                    if (!in.get(3, rep))
                        return std::nullopt;
                    rep += 3;
                }
                else {
                    if (!in.get(7, rep))
                        return std::nullopt;
                    rep += 11;
                }
                if (idx + rep > hlit + hdist)
                    return std::nullopt;
                while (rep-- > 0)
                    lengths[idx++] = value;
            }
            if (lengths[256] == 0)
                return std::nullopt; // no end-of-block code
            Huffman lit, dist;
            if (!lit.build(lengths.data(), hlit) || !dist.build(lengths.data() + hlit, hdist))
                return std::nullopt;
            if (!codes(in, out, lit, dist, limit))
                return std::nullopt;
        }
        else {
            return std::nullopt;
        }
        if (out.size() > limit)
            return std::nullopt;
    } while (last == 0);
    return out;
}

std::optional<std::map<std::string, std::string>> readZip(std::string_view z) {
    // the end of central directory record: the last "PK\5\6" within the final 64 KiB + 22 bytes
    if (z.size() < 22)
        return std::nullopt;
    std::size_t eocd = std::string_view::npos;
    const std::size_t lowest = z.size() > 65557 ? z.size() - 65557 : 0;
    for (std::size_t i = z.size() - 22 + 1; i-- > lowest;)
        if (le(z, i, 4) == 0x06054b50u) {
            eocd = i;
            break;
        }
    if (eocd == std::string_view::npos)
        return std::nullopt;
    const std::uint32_t entries = le(z, eocd + 10, 2);
    std::size_t cd = le(z, eocd + 16, 4);
    std::map<std::string, std::string> files;
    for (std::uint32_t e = 0; e < entries; ++e) {
        if (cd + 46 > z.size() || le(z, cd, 4) != 0x02014b50u)
            return std::nullopt;
        const std::uint32_t flags = le(z, cd + 8, 2), method = le(z, cd + 10, 2);
        const std::uint32_t csize = le(z, cd + 20, 4), usize = le(z, cd + 24, 4);
        const std::uint32_t nlen = le(z, cd + 28, 2), xlen = le(z, cd + 30, 2), clen = le(z, cd + 32, 2);
        const std::size_t local = le(z, cd + 42, 4);
        if (cd + 46 + nlen > z.size())
            return std::nullopt;
        const std::string name(z.substr(cd + 46, nlen));
        cd += 46 + nlen + xlen + clen;
        if (name.empty() || name.back() == '/')
            continue; // a directory
        if ((flags & 1u) != 0 || csize == 0xFFFFFFFFu)
            return std::nullopt; // encrypted, ZIP64
        if (local + 30 > z.size() || le(z, local, 4) != 0x04034b50u)
            return std::nullopt;
        const std::size_t data = local + 30 + le(z, local + 26, 2) + le(z, local + 28, 2);
        if (data + csize > z.size())
            return std::nullopt;
        const std::string_view raw = z.substr(data, csize);
        if (method == 0) {
            files[name] = std::string(raw);
        }
        else if (method == 8) {
            auto content = inflate(raw, std::max<std::size_t>(usize, 1) + 16);
            if (!content || content->size() != usize)
                return std::nullopt;
            files[name] = std::move(*content);
        }
        else {
            return std::nullopt;
        }
    }
    return files;
}

std::optional<std::map<std::string, std::string>> readZipFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return std::nullopt;
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return readZip(bytes);
}

} // namespace opencdc
