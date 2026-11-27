#include "hua/compress.hpp"
#include "hua/stream.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hua {
namespace {
constexpr std::size_t kDefaultMaxDecompress = 64 * 1024 * 1024;
constexpr std::size_t kHardMaxBytes = 128 * 1024 * 1024;

std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t* data, std::size_t len) {
    static const auto table = []() {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

Value ok_compress(Value payload, std::string success_type) {
    return Value(ResultValue{true, result_box(std::move(payload)), std::move(success_type), "std.error.Value", true, false});
}

// --- RFC 1951 DEFLATE Inflate (stored + fixed Huffman + dynamic Huffman) ---
struct HuffTable {
    static constexpr int kMaxBits = 15;
    std::array<int, kMaxBits + 1> counts{};
    std::vector<int> symbols;

    bool build(const int* lengths, int num_syms) {
        counts.fill(0);
        for (int i = 0; i < num_syms; ++i) {
            if (lengths[i] < 0 || lengths[i] > kMaxBits) return false;
            counts[lengths[i]]++;
        }
        if (counts[0] == num_syms) {
            symbols.clear();
            return true;
        }
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= counts[len];
            if (left < 0) return false; // over-subscribed
        }
        std::array<int, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = offs[len] + counts[len];
        symbols.assign(static_cast<std::size_t>(num_syms), 0);
        for (int sym = 0; sym < num_syms; ++sym) {
            if (lengths[sym] != 0) symbols[static_cast<std::size_t>(offs[lengths[sym]]++)] = sym;
        }
        return true;
    }
};

struct BitReader {
    const std::uint8_t* data{};
    std::size_t size{};
    std::size_t pos{};
    std::uint64_t bitbuf{};
    int bitcnt{};

    bool ensure(int n) {
        while (bitcnt < n) {
            if (pos >= size) return false;
            bitbuf |= (static_cast<std::uint64_t>(data[pos++]) << bitcnt);
            bitcnt += 8;
        }
        return true;
    }
    bool read_bits(int n, std::uint32_t& out) {
        if (n == 0) { out = 0; return true; }
        if (!ensure(n)) return false;
        out = static_cast<std::uint32_t>(bitbuf & ((std::uint64_t{1} << n) - 1));
        bitbuf >>= n;
        bitcnt -= n;
        return true;
    }
    void align_byte() {
        int drop = bitcnt & 7;
        bitbuf >>= drop;
        bitcnt -= drop;
    }
    bool read_u16_aligned(std::uint16_t& out) {
        align_byte();
        std::uint32_t v = 0;
        if (!read_bits(16, v)) return false;
        out = static_cast<std::uint16_t>(v);
        return true;
    }
    bool read_bytes_aligned(std::uint8_t* dst, std::size_t n) {
        align_byte();
        while (n > 0 && bitcnt >= 8) {
            *dst++ = static_cast<std::uint8_t>(bitbuf & 0xFFu);
            bitbuf >>= 8;
            bitcnt -= 8;
            --n;
        }
        if (pos + n > size) return false;
        if (n > 0) {
            std::memcpy(dst, data + pos, n);
            pos += n;
        }
        return true;
    }
    std::size_t byte_pos() const {
        return pos - static_cast<std::size_t>(bitcnt / 8);
    }
    bool decode_huff(const HuffTable& h, int& sym_out) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= HuffTable::kMaxBits; ++len) {
            std::uint32_t b = 0;
            if (!read_bits(1, b)) return false;
            code |= static_cast<int>(b);
            int count = h.counts[len];
            if (code - count < first) {
                sym_out = h.symbols[static_cast<std::size_t>(index + (code - first))];
                return true;
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }
};

bool inflate_deflate(BitReader& br, std::string& out, std::size_t max_bytes, std::string& err_code, std::string& err_msg) {
    static constexpr int kLenBase[29] = {
        3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
        35,43,51,59,67,83,99,115,131,163,195,227,258
    };
    static constexpr int kLenExtra[29] = {
        0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
        3,3,3,3,4,4,4,4,5,5,5,5,0
    };
    static constexpr int kDistBase[30] = {
        1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
        257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
    };
    static constexpr int kDistExtra[30] = {
        0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
        7,7,8,8,9,9,10,10,11,11,12,12,13,13
    };
    static constexpr int kClOrder[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };

    static const auto fixed_tables = []() {
        std::pair<HuffTable, HuffTable> p;
        int ll[288];
        for (int i = 0; i <= 143; ++i) ll[i] = 8;
        for (int i = 144; i <= 255; ++i) ll[i] = 9;
        for (int i = 256; i <= 279; ++i) ll[i] = 7;
        for (int i = 280; i <= 287; ++i) ll[i] = 8;
        p.first.build(ll, 288);
        int dl[32];
        for (int i = 0; i < 32; ++i) dl[i] = 5;
        p.second.build(dl, 32);
        return p;
    }();

    std::uint32_t bfinal = 0;
    do {
        std::uint32_t btype = 0;
        if (!br.read_bits(1, bfinal) || !br.read_bits(2, btype)) {
            err_code = "corrupt"; err_msg = "truncated deflate block header"; return false;
        }
        if (btype == 0) {
            std::uint16_t len = 0, nlen = 0;
            if (!br.read_u16_aligned(len) || !br.read_u16_aligned(nlen) || static_cast<std::uint16_t>(~len) != nlen) {
                err_code = "corrupt"; err_msg = "invalid stored block length"; return false;
            }
            if (out.size() + len > max_bytes) {
                err_code = "limit"; err_msg = "decompressed output exceeds max_bytes"; return false;
            }
            std::size_t old_sz = out.size();
            out.resize(old_sz + len);
            if (!br.read_bytes_aligned(reinterpret_cast<std::uint8_t*>(out.data() + old_sz), len)) {
                err_code = "corrupt"; err_msg = "truncated stored block data"; return false;
            }
        } else if (btype == 1 || btype == 2) {
            HuffTable dyn_lit, dyn_dist;
            const HuffTable* lit_h = &fixed_tables.first;
            const HuffTable* dist_h = &fixed_tables.second;
            if (btype == 2) {
                std::uint32_t hlit_r = 0, hdist_r = 0, hclen_r = 0;
                if (!br.read_bits(5, hlit_r) || !br.read_bits(5, hdist_r) || !br.read_bits(4, hclen_r)) {
                    err_code = "corrupt"; err_msg = "truncated dynamic huffman header"; return false;
                }
                int hlit = static_cast<int>(hlit_r) + 257;
                int hdist = static_cast<int>(hdist_r) + 1;
                int hclen = static_cast<int>(hclen_r) + 4;
                if (hlit > 286 || hdist > 30) {
                    err_code = "corrupt"; err_msg = "invalid dynamic huffman table sizes"; return false;
                }
                int cl_lens[19]{};
                for (int i = 0; i < hclen; ++i) {
                    std::uint32_t v = 0;
                    if (!br.read_bits(3, v)) { err_code = "corrupt"; err_msg = "truncated code-length table"; return false; }
                    cl_lens[kClOrder[i]] = static_cast<int>(v);
                }
                HuffTable cl_h;
                if (!cl_h.build(cl_lens, 19)) { err_code = "corrupt"; err_msg = "invalid code-length huffman tree"; return false; }
                std::vector<int> lengths(static_cast<std::size_t>(hlit + hdist), 0);
                int idx = 0;
                while (idx < hlit + hdist) {
                    int sym = 0;
                    if (!br.decode_huff(cl_h, sym)) { err_code = "corrupt"; err_msg = "invalid code-length symbol"; return false; }
                    if (sym < 16) {
                        lengths[static_cast<std::size_t>(idx++)] = sym;
                    } else if (sym == 16) {
                        if (idx == 0) { err_code = "corrupt"; err_msg = "repeat with no previous length"; return false; }
                        std::uint32_t rep = 0;
                        if (!br.read_bits(2, rep)) { err_code = "corrupt"; err_msg = "truncated repeat count"; return false; }
                        int count = static_cast<int>(rep) + 3;
                        if (idx + count > hlit + hdist) { err_code = "corrupt"; err_msg = "length overflow"; return false; }
                        int prev = lengths[static_cast<std::size_t>(idx - 1)];
                        for (int k = 0; k < count; ++k) lengths[static_cast<std::size_t>(idx++)] = prev;
                    } else if (sym == 17 || sym == 18) {
                        std::uint32_t rep = 0;
                        int bits = (sym == 17) ? 3 : 7;
                        int base = (sym == 17) ? 3 : 11;
                        if (!br.read_bits(bits, rep)) { err_code = "corrupt"; err_msg = "truncated zero repeat"; return false; }
                        int count = static_cast<int>(rep) + base;
                        if (idx + count > hlit + hdist) { err_code = "corrupt"; err_msg = "zero repeat overflow"; return false; }
                        for (int k = 0; k < count; ++k) lengths[static_cast<std::size_t>(idx++)] = 0;
                    } else {
                        err_code = "corrupt"; err_msg = "invalid code-length symbol"; return false;
                    }
                }
                if (!dyn_lit.build(lengths.data(), hlit) || !dyn_dist.build(lengths.data() + hlit, hdist)) {
                    err_code = "corrupt"; err_msg = "invalid dynamic huffman tree"; return false;
                }
                lit_h = &dyn_lit;
                dist_h = &dyn_dist;
            }
            for (;;) {
                int sym = 0;
                if (!br.decode_huff(*lit_h, sym)) { err_code = "corrupt"; err_msg = "invalid literal/length symbol"; return false; }
                if (sym < 256) {
                    if (out.size() + 1 > max_bytes) { err_code = "limit"; err_msg = "decompressed output exceeds max_bytes"; return false; }
                    out.push_back(static_cast<char>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    int lc = sym - 257;
                    if (lc < 0 || lc >= 29) { err_code = "corrupt"; err_msg = "invalid length code"; return false; }
                    std::uint32_t extra = 0;
                    if (!br.read_bits(kLenExtra[lc], extra)) { err_code = "corrupt"; err_msg = "truncated length extra bits"; return false; }
                    std::size_t match_len = static_cast<std::size_t>(kLenBase[lc] + static_cast<int>(extra));
                    int dc = 0;
                    if (!br.decode_huff(*dist_h, dc) || dc < 0 || dc >= 30) { err_code = "corrupt"; err_msg = "invalid distance code"; return false; }
                    std::uint32_t dextra = 0;
                    if (!br.read_bits(kDistExtra[dc], dextra)) { err_code = "corrupt"; err_msg = "truncated distance extra bits"; return false; }
                    std::size_t dist = static_cast<std::size_t>(kDistBase[dc] + static_cast<int>(dextra));
                    if (dist == 0 || dist > out.size()) { err_code = "corrupt"; err_msg = "invalid back-reference distance"; return false; }
                    if (out.size() + match_len > max_bytes) { err_code = "limit"; err_msg = "decompressed output exceeds max_bytes"; return false; }
                    std::size_t start = out.size() - dist;
                    out.reserve(out.size() + match_len);
                    for (std::size_t k = 0; k < match_len; ++k) out.push_back(out[start + k]);
                }
            }
        } else {
            err_code = "corrupt"; err_msg = "reserved deflate block type 3"; return false;
        }
    } while (!bfinal);
    return true;
}

bool gzip_decompress_impl(std::string_view input, std::size_t max_bytes, std::string& out, std::string& err_code, std::string& err_msg) {
    out.clear();
    if (input.size() < 18) {
        err_code = "corrupt"; err_msg = "gzip input too short"; return false;
    }
    const auto* p = reinterpret_cast<const std::uint8_t*>(input.data());
    std::size_t pos = 0;
    while (pos < input.size()) {
        if (input.size() - pos < 18) {
            err_code = "corrupt"; err_msg = "truncated gzip member"; return false;
        }
        if (p[pos] != 0x1Fu || p[pos + 1] != 0x8Bu || p[pos + 2] != 0x08u) {
            err_code = "corrupt"; err_msg = "invalid gzip header magic or method"; return false;
        }
        std::uint8_t flg = p[pos + 3];
        if (flg & 0xE0u) {
            err_code = "corrupt"; err_msg = "reserved gzip header flags set"; return false;
        }
        pos += 10;
        if (flg & 0x04u) { // FEXTRA
            if (pos + 2 > input.size()) { err_code = "corrupt"; err_msg = "truncated FEXTRA"; return false; }
            std::uint16_t xlen = static_cast<std::uint16_t>(p[pos] | (p[pos + 1] << 8));
            pos += 2;
            if (pos + xlen > input.size()) { err_code = "corrupt"; err_msg = "truncated FEXTRA payload"; return false; }
            pos += xlen;
        }
        if (flg & 0x08u) { // FNAME
            while (pos < input.size() && p[pos] != 0) ++pos;
            if (pos >= input.size()) { err_code = "corrupt"; err_msg = "unterminated FNAME"; return false; }
            ++pos;
        }
        if (flg & 0x10u) { // FCOMMENT
            while (pos < input.size() && p[pos] != 0) ++pos;
            if (pos >= input.size()) { err_code = "corrupt"; err_msg = "unterminated FCOMMENT"; return false; }
            ++pos;
        }
        if (flg & 0x02u) { // FHCRC
            if (pos + 2 > input.size()) { err_code = "corrupt"; err_msg = "truncated FHCRC"; return false; }
            pos += 2;
        }
        std::size_t member_start = out.size();
        BitReader br{p + pos, input.size() - pos, 0, 0, 0};
        if (!inflate_deflate(br, out, max_bytes, err_code, err_msg)) return false;
        br.align_byte();
        pos += br.byte_pos();
        if (pos + 8 > input.size()) {
            err_code = "corrupt"; err_msg = "truncated gzip trailer"; return false;
        }
        std::uint32_t expected_crc = static_cast<std::uint32_t>(p[pos])
            | (static_cast<std::uint32_t>(p[pos + 1]) << 8)
            | (static_cast<std::uint32_t>(p[pos + 2]) << 16)
            | (static_cast<std::uint32_t>(p[pos + 3]) << 24);
        std::uint32_t expected_isize = static_cast<std::uint32_t>(p[pos + 4])
            | (static_cast<std::uint32_t>(p[pos + 5]) << 8)
            | (static_cast<std::uint32_t>(p[pos + 6]) << 16)
            | (static_cast<std::uint32_t>(p[pos + 7]) << 24);
        pos += 8;
        std::size_t member_len = out.size() - member_start;
        std::uint32_t actual_crc = crc32_update(0, reinterpret_cast<const std::uint8_t*>(out.data() + member_start), member_len);
        if (actual_crc != expected_crc) {
            err_code = "corrupt"; err_msg = "gzip CRC32 mismatch"; return false;
        }
        if (static_cast<std::uint32_t>(member_len & 0xFFFFFFFFu) != expected_isize) {
            err_code = "corrupt"; err_msg = "gzip ISIZE mismatch"; return false;
        }
    }
    return true;
}

// --- RFC 1951 DEFLATE Deflate (fixed Huffman + LZ77 hash chain) + RFC 1952 GZIP ---
struct BitWriter {
    std::string out;
    std::uint64_t bitbuf{};
    int bitcnt{};

    void write_bits(std::uint32_t val, int n) {
        bitbuf |= (static_cast<std::uint64_t>(val) << bitcnt);
        bitcnt += n;
        while (bitcnt >= 8) {
            out.push_back(static_cast<char>(bitbuf & 0xFFu));
            bitbuf >>= 8;
            bitcnt -= 8;
        }
    }
    void write_rev_bits(std::uint32_t val, int n) {
        std::uint32_t rev = 0;
        for (int i = 0; i < n; ++i) {
            rev = (rev << 1) | ((val >> i) & 1u);
        }
        write_bits(rev, n);
    }
    void flush_byte() {
        if (bitcnt > 0) {
            out.push_back(static_cast<char>(bitbuf & 0xFFu));
            bitbuf = 0;
            bitcnt = 0;
        }
    }
    void emit_fixed_lit(int sym) {
        if (sym <= 143) write_rev_bits(static_cast<std::uint32_t>(0x30 + sym), 8);
        else if (sym <= 255) write_rev_bits(static_cast<std::uint32_t>(0x190 + (sym - 144)), 9);
        else if (sym <= 279) write_rev_bits(static_cast<std::uint32_t>(sym - 256), 7);
        else write_rev_bits(static_cast<std::uint32_t>(0xC0 + (sym - 280)), 8);
    }
    void emit_fixed_dist(int code) {
        write_rev_bits(static_cast<std::uint32_t>(code), 5);
    }
    void emit_match(int len, int dist) {
        static constexpr int kLenBase[29] = {
            3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,
            35,43,51,59,67,83,99,115,131,163,195,227,258
        };
        static constexpr int kLenExtra[29] = {
            0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,
            3,3,3,3,4,4,4,4,5,5,5,5,0
        };
        static constexpr int kDistBase[30] = {
            1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,
            257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
        };
        static constexpr int kDistExtra[30] = {
            0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,
            7,7,8,8,9,9,10,10,11,11,12,12,13,13
        };
        int lc = 28;
        if (len < 258) {
            for (int i = 0; i < 28; ++i) {
                if (len < kLenBase[i + 1]) { lc = i; break; }
            }
        }
        emit_fixed_lit(257 + lc);
        if (kLenExtra[lc] > 0) write_bits(static_cast<std::uint32_t>(len - kLenBase[lc]), kLenExtra[lc]);
        int dc = 29;
        for (int i = 0; i < 29; ++i) {
            if (dist < kDistBase[i + 1]) { dc = i; break; }
        }
        emit_fixed_dist(dc);
        if (kDistExtra[dc] > 0) write_bits(static_cast<std::uint32_t>(dist - kDistBase[dc]), kDistExtra[dc]);
    }
};

std::string gzip_compress_impl(std::string_view input) {
    BitWriter bw;
    bw.out.reserve(input.size() / 2 + 32);
    // RFC 1952 10-byte GZIP header
    const char hdr[10] = {'\x1F', '\x8B', '\x08', '\x00', '\x00', '\x00', '\x00', '\x00', '\x00', '\xFF'};
    bw.out.append(hdr, 10);

    // Single BFINAL=1, BTYPE=01 (fixed Huffman) block with LZ77 hash table
    bw.write_bits(1, 1); // BFINAL
    bw.write_bits(1, 2); // BTYPE = fixed Huffman

    const auto* p = reinterpret_cast<const std::uint8_t*>(input.data());
    const std::size_t n = input.size();
    constexpr std::size_t kHashSize = 1 << 15;
    std::vector<int> head(kHashSize, -1);
    std::vector<int> prev(n, -1);
    auto hash3 = [](const std::uint8_t* s) -> std::size_t {
        return ((static_cast<std::size_t>(s[0]) << 10) ^ (static_cast<std::size_t>(s[1]) << 5) ^ static_cast<std::size_t>(s[2])) & (kHashSize - 1);
    };

    std::size_t i = 0;
    while (i < n) {
        int best_len = 0;
        int best_dist = 0;
        if (i + 3 <= n) {
            std::size_t h = hash3(p + i);
            int cand = head[h];
            prev[i] = cand;
            head[h] = static_cast<int>(i);
            int chain = 0;
            while (cand >= 0 && chain < 32) {
                int dist = static_cast<int>(i) - cand;
                if (dist > 32768) break;
                int max_l = static_cast<int>(std::min<std::size_t>(258, n - i));
                if (p[cand + best_len] == p[i + best_len]) {
                    int l = 0;
                    while (l < max_l && p[cand + l] == p[i + l]) ++l;
                    if (l >= 3 && l > best_len) {
                        best_len = l;
                        best_dist = dist;
                        if (best_len == 258) break;
                    }
                }
                cand = prev[static_cast<std::size_t>(cand)];
                ++chain;
            }
        }
        if (best_len >= 3) {
            bw.emit_match(best_len, best_dist);
            std::size_t end = i + static_cast<std::size_t>(best_len);
            for (std::size_t k = i + 1; k < end && k + 3 <= n; ++k) {
                std::size_t h = hash3(p + k);
                prev[k] = head[h];
                head[h] = static_cast<int>(k);
            }
            i = end;
        } else {
            bw.emit_fixed_lit(p[i]);
            ++i;
        }
    }
    bw.emit_fixed_lit(256); // EOB
    bw.flush_byte();

    std::uint32_t crc = crc32_update(0, p, n);
    std::uint32_t isize = static_cast<std::uint32_t>(n & 0xFFFFFFFFu);
    auto push_u32 = [&](std::uint32_t v) {
        bw.out.push_back(static_cast<char>(v & 0xFFu));
        bw.out.push_back(static_cast<char>((v >> 8) & 0xFFu));
        bw.out.push_back(static_cast<char>((v >> 16) & 0xFFu));
        bw.out.push_back(static_cast<char>((v >> 24) & 0xFFu));
    };
    push_u32(crc);
    push_u32(isize);
    return bw.out;
}

class GzipReaderBackend final : public StreamReaderBackend {
    std::mutex mutex_;
    std::shared_ptr<StreamReaderData> src_;
    std::size_t max_bytes_;
    bool decoded_{false};
    std::string plain_;
    std::size_t offset_{0};
public:
    GzipReaderBackend(std::shared_ptr<StreamReaderData> src, std::size_t max_bytes)
        : src_(std::move(src)), max_bytes_(max_bytes) {}
    StreamReadResult read(std::size_t max_read, RuntimeContext* context) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!decoded_) {
            if (!src_) return {std::nullopt, StreamError{"compress", "closed", "gzip reader is closed", "", {}}};
            std::string compressed;
            for (;;) {
                auto chunk = src_->read(65536, context);
                if (chunk.error) return {std::nullopt, *chunk.error};
                if (!chunk.chunk) break;
                if (compressed.size() + chunk.chunk->size() > kHardMaxBytes) {
                    return {std::nullopt, StreamError{"compress", "limit", "compressed input exceeds limit", "", {}}};
                }
                compressed.append(*chunk.chunk);
            }
            std::string ec, em;
            if (!gzip_decompress_impl(compressed, max_bytes_, plain_, ec, em)) {
                return {std::nullopt, StreamError{"compress", ec, em, "", {}}};
            }
            decoded_ = true;
        }
        if (offset_ >= plain_.size()) return {std::nullopt, std::nullopt};
        std::size_t n = std::min(max_read, plain_.size() - offset_);
        std::string chunk(plain_.data() + offset_, n);
        offset_ += n;
        return {std::move(chunk), std::nullopt};
    }
    std::optional<StreamError> close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        src_.reset();
        plain_.clear();
        return std::nullopt;
    }
};

class GzipWriterBackend final : public StreamWriterBackend {
    std::mutex mutex_;
    std::shared_ptr<StreamWriterData> dst_;
    std::string buf_;
    bool closed_{false};
public:
    explicit GzipWriterBackend(std::shared_ptr<StreamWriterData> dst) : dst_(std::move(dst)) {}
    StreamWriteResult write(std::span<const char> data, RuntimeContext*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_ || !dst_) return {0, StreamError{"compress", "closed", "gzip writer is closed", "", {}}};
        if (buf_.size() + data.size() > kHardMaxBytes) {
            return {0, StreamError{"compress", "limit", "gzip writer input exceeds limit", "", {}}};
        }
        buf_.append(data.data(), data.size());
        return {data.size(), std::nullopt};
    }
    std::optional<StreamError> flush(RuntimeContext*) override { return std::nullopt; }
    std::optional<StreamError> close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) return std::nullopt;
        closed_ = true;
        if (!dst_) return std::nullopt;
        std::string gz = gzip_compress_impl(buf_);
        buf_.clear();
        std::size_t off = 0;
        while (off < gz.size()) {
            auto r = dst_->write(std::span<const char>(gz.data() + off, gz.size() - off), nullptr);
            if (r.error) { dst_.reset(); return *r.error; }
            if (r.written == 0) { dst_.reset(); return StreamError{"compress", "io", "short zero write to underlying writer", "", {}}; }
            off += r.written;
        }
        auto fl = dst_->flush(nullptr);
        dst_.reset();
        return fl;
    }
};
} // namespace

Value invoke_compress_standard(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& s, RuntimeContext*) {
    if (f.name == "gzip_compress") {
        auto* b = get_if<BytesValue>(&args.at(0).data);
        if (!b || !b->data) runtime_error(s, "std.compress.gzip_compress expects Bytes", "E4003");
        if (b->data->size() > kHardMaxBytes) {
            return stream_error_result({"compress", "limit", "input exceeds maximum compress size", "", {}}, "Bytes", "std.compress.gzip_compress");
        }
        std::string gz = gzip_compress_impl(*b->data);
        return ok_compress(Value(BytesValue{managed<const std::string>(std::move(gz))}), "Bytes");
    }
    if (f.name == "gzip_decompress") {
        auto* b = get_if<BytesValue>(&args.at(0).data);
        auto* max_b = get_if<std::int64_t>(&args.at(1).data);
        if (!b || !b->data || !max_b) runtime_error(s, "std.compress.gzip_decompress expects (Bytes, int)", "E4003");
        if (*max_b < 0) return stream_error_result({"compress", "invalid_argument", "max_bytes must be >= 0", "", {}}, "Bytes", "std.compress.gzip_decompress");
        std::size_t lim = (*max_b == 0) ? kDefaultMaxDecompress : std::min<std::size_t>(static_cast<std::size_t>(*max_b), kHardMaxBytes);
        std::string plain, ec, em;
        if (!gzip_decompress_impl(*b->data, lim, plain, ec, em)) {
            return stream_error_result({"compress", ec, em, "", {}}, "Bytes", "std.compress.gzip_decompress");
        }
        return ok_compress(Value(BytesValue{managed<const std::string>(std::move(plain))}), "Bytes");
    }
    if (f.name == "gzip_reader") {
        auto* r = get_if<ReaderValue>(&args.at(0).data);
        auto* max_b = get_if<std::int64_t>(&args.at(1).data);
        if (!r || !r->data || !max_b) runtime_error(s, "std.compress.gzip_reader expects (std.stream.Reader, int)", "E4003");
        if (*max_b < 0) return stream_error_result({"compress", "invalid_argument", "max_bytes must be >= 0", "", {}}, "std.stream.Reader", "std.compress.gzip_reader");
        std::size_t lim = (*max_b == 0) ? kDefaultMaxDecompress : std::min<std::size_t>(static_cast<std::size_t>(*max_b), kHardMaxBytes);
        auto reader = std::make_shared<StreamReaderData>(std::make_shared<GzipReaderBackend>(r->data, lim));
        return ok_compress(Value(ReaderValue{std::move(reader)}), "std.stream.Reader");
    }
    if (f.name == "gzip_writer") {
        auto* w = get_if<WriterValue>(&args.at(0).data);
        if (!w || !w->data) runtime_error(s, "std.compress.gzip_writer expects std.stream.Writer", "E4003");
        auto writer = std::make_shared<StreamWriterData>(std::make_shared<GzipWriterBackend>(w->data));
        return ok_compress(Value(WriterValue{std::move(writer)}), "std.stream.Writer");
    }
    runtime_error(s, "unknown std.compress function: " + f.name, "E4003");
}
} // namespace hua
