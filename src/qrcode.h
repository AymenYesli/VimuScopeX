#pragma once
//
// Minimal QR code encoder for the Remote page's URL: byte mode, error
// correction level M (about 15 % damage recovered), versions 1-10 (up to
// 213 bytes), the best of the 8 masks by the standard's penalty rules.
// Plain C++, no dependencies; checked by decoding its output with OpenCV's
// QR detector over many strings and every version.
//
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

namespace Qr {

class Code {
public:
    int size = 0;                          // modules per side; 0 = too long to encode
    bool at(int x, int y) const { return m_mod[size_t(y * size + x)] != 0; }

    static Code encode(const std::string &text)
    {
        Code c;
        const std::vector<uint8_t> bytes(text.begin(), text.end());
        for (int version = 1; version <= 10; ++version) {
            const Spec s = spec(version);
            const int dataCw = s.g1Blocks * s.g1Data + s.g2Blocks * (s.g1Data + 1);
            // Mode (4 bits) + count (8 bits for versions 1-9, 16 for 10+) + data.
            const int countBits = version <= 9 ? 8 : 16;
            if (4 + countBits + 8 * int(bytes.size()) <= dataCw * 8) {
                c.build(version, bytes);
                return c;
            }
        }
        return c;
    }

private:
    struct Spec { int ecPerBlock, g1Blocks, g1Data, g2Blocks; };   // group 2 blocks hold g1Data + 1
    static Spec spec(int v)
    {
        // Level M, versions 1-10 (ISO/IEC 18004 table 9).
        static const Spec t[] = {
            { 10, 1, 16, 0 }, { 16, 1, 28, 0 }, { 26, 1, 44, 0 }, { 18, 2, 32, 0 },
            { 24, 2, 43, 0 }, { 16, 4, 27, 0 }, { 18, 4, 31, 0 }, { 22, 2, 38, 2 },
            { 22, 3, 36, 2 }, { 26, 4, 43, 1 } };
        return t[v - 1];
    }
    static std::vector<int> alignment(int v)
    {
        static const std::vector<int> t[] = {
            {}, { 6, 18 }, { 6, 22 }, { 6, 26 }, { 6, 30 }, { 6, 34 },
            { 6, 22, 38 }, { 6, 24, 42 }, { 6, 26, 46 }, { 6, 28, 50 } };
        return t[v - 1];
    }

    // GF(256), polynomial 0x11D.
    static uint8_t gfMul(uint8_t a, uint8_t b)
    {
        int r = 0;
        for (int i = 7; i >= 0; --i) {
            r = (r << 1) ^ ((r >> 7) * 0x11D);
            r ^= ((b >> i) & 1) * a;
        }
        return uint8_t(r);
    }
    static std::vector<uint8_t> rsGenerator(int degree)
    {
        std::vector<uint8_t> g(size_t(degree), 0);
        g[size_t(degree - 1)] = 1;
        uint8_t root = 1;
        for (int i = 0; i < degree; ++i) {
            for (size_t j = 0; j < g.size(); ++j) {
                g[j] = gfMul(g[j], root);
                if (j + 1 < g.size())
                    g[j] ^= g[j + 1];
            }
            root = gfMul(root, 0x02);
        }
        return g;
    }
    static std::vector<uint8_t> rsRemainder(const std::vector<uint8_t> &data,
                                            const std::vector<uint8_t> &gen)
    {
        std::vector<uint8_t> r(gen.size(), 0);
        for (uint8_t b : data) {
            const uint8_t factor = b ^ r[0];
            r.erase(r.begin());
            r.push_back(0);
            for (size_t i = 0; i < r.size(); ++i)
                r[i] ^= gfMul(gen[i], factor);
        }
        return r;
    }

    std::vector<uint8_t> m_mod, m_fn;   // modules; function-pattern flags
    void set(int x, int y, bool dark, bool function = true)
    {
        m_mod[size_t(y * size + x)] = dark;
        if (function)
            m_fn[size_t(y * size + x)] = 1;
    }
    bool isFn(int x, int y) const { return m_fn[size_t(y * size + x)] != 0; }

    void finder(int cx, int cy)
    {
        for (int dy = -4; dy <= 4; ++dy) {
            for (int dx = -4; dx <= 4; ++dx) {
                const int x = cx + dx, y = cy + dy;
                if (x < 0 || y < 0 || x >= size || y >= size)
                    continue;
                const int d = std::max(std::abs(dx), std::abs(dy));
                set(x, y, d != 2 && d != 4);
            }
        }
    }
    void alignmentAt(int cx, int cy)
    {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                set(cx + dx, cy + dy, std::max(std::abs(dx), std::abs(dy)) != 1);
    }

    void formatBits(int mask)
    {
        const int data = (0 /* level M = 00 */ << 3) | mask;
        int rem = data;
        for (int i = 0; i < 10; ++i)
            rem = (rem << 1) ^ ((rem >> 9) * 0x537);
        const int bits = ((data << 10) | rem) ^ 0x5412;
        const auto bit = [&](int i) { return ((bits >> i) & 1) != 0; };
        for (int i = 0; i <= 5; ++i)
            set(8, i, bit(i));
        set(8, 7, bit(6));
        set(8, 8, bit(7));
        set(7, 8, bit(8));
        for (int i = 9; i < 15; ++i)
            set(14 - i, 8, bit(i));
        for (int i = 0; i < 8; ++i)
            set(size - 1 - i, 8, bit(i));
        for (int i = 8; i < 15; ++i)
            set(8, size - 15 + i, bit(i));
        set(8, size - 8, true);   // the dark module
    }
    void versionBits(int version)
    {
        if (version < 7)
            return;
        int rem = version;
        for (int i = 0; i < 12; ++i)
            rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
        const long bits = (long(version) << 12) | rem;
        for (int i = 0; i < 18; ++i) {
            const bool b = ((bits >> i) & 1) != 0;
            const int a = size - 11 + i % 3, c = i / 3;
            set(a, c, b);
            set(c, a, b);
        }
    }

    void functionPatterns(int version)
    {
        for (int i = 0; i < size; ++i) {   // timing
            set(6, i, i % 2 == 0);
            set(i, 6, i % 2 == 0);
        }
        finder(3, 3);
        finder(size - 4, 3);
        finder(3, size - 4);
        const std::vector<int> a = alignment(version);
        for (size_t i = 0; i < a.size(); ++i) {
            for (size_t j = 0; j < a.size(); ++j) {
                const bool corner = (i == 0 && j == 0) || (i == 0 && j == a.size() - 1)
                                    || (i == a.size() - 1 && j == 0);
                if (!corner)
                    alignmentAt(a[i], a[j]);
            }
        }
        formatBits(0);       // reserved; rewritten with the chosen mask
        versionBits(version);
    }

    static bool maskBit(int mask, int x, int y)
    {
        switch (mask) {
        case 0: return (x + y) % 2 == 0;
        case 1: return y % 2 == 0;
        case 2: return x % 3 == 0;
        case 3: return (x + y) % 3 == 0;
        case 4: return (x / 3 + y / 2) % 2 == 0;
        case 5: return x * y % 2 + x * y % 3 == 0;
        case 6: return (x * y % 2 + x * y % 3) % 2 == 0;
        default: return ((x + y) % 2 + x * y % 3) % 2 == 0;
        }
    }
    void applyMask(int mask)
    {
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if (!isFn(x, y) && maskBit(mask, x, y))
                    m_mod[size_t(y * size + x)] ^= 1;
    }

    long penalty() const
    {
        long p = 0;
        // Rule 1: runs of 5+ same-colour modules in a row / column.
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < size; ++a) {
                int run = 1;
                for (int b = 1; b < size; ++b) {
                    const bool cur = pass ? at(a, b) : at(b, a);
                    const bool prev = pass ? at(a, b - 1) : at(b - 1, a);
                    if (cur == prev) {
                        ++run;
                    } else {
                        if (run >= 5) p += 3 + (run - 5);
                        run = 1;
                    }
                }
                if (run >= 5) p += 3 + (run - 5);
            }
        }
        // Rule 2: 2x2 blocks.
        for (int y = 0; y + 1 < size; ++y)
            for (int x = 0; x + 1 < size; ++x) {
                const bool c = at(x, y);
                if (c == at(x + 1, y) && c == at(x, y + 1) && c == at(x + 1, y + 1))
                    p += 3;
            }
        // Rule 3: finder-like 1:1:3:1:1 patterns with 4 light on a side.
        static const int pat[] = { 1, 0, 1, 1, 1, 0, 1 };
        for (int pass = 0; pass < 2; ++pass) {
            for (int a = 0; a < size; ++a) {
                for (int b = 0; b + 7 <= size; ++b) {
                    bool match = true;
                    for (int k = 0; k < 7 && match; ++k)
                        match = (pass ? at(a, b + k) : at(b + k, a)) == (pat[k] != 0);
                    if (!match)
                        continue;
                    const auto light = [&](int from, int to) {
                        for (int k = from; k < to; ++k) {
                            if (k < 0 || k >= size) continue;
                            if (pass ? at(a, k) : at(k, a)) return false;
                        }
                        return true;
                    };
                    if (light(b - 4, b) || light(b + 7, b + 11))
                        p += 40;
                }
            }
        }
        // Rule 4: dark-module balance.
        int dark = 0;
        for (uint8_t m : m_mod) dark += m;
        const int total = size * size;
        const int k = std::abs(dark * 20 - total * 10) / total;
        p += 10L * k;
        return p;
    }

    void build(int version, const std::vector<uint8_t> &bytes)
    {
        size = 17 + 4 * version;
        m_mod.assign(size_t(size * size), 0);
        m_fn.assign(size_t(size * size), 0);
        functionPatterns(version);

        // Data bits.
        const Spec s = spec(version);
        const int blocks = s.g1Blocks + s.g2Blocks;
        const int dataCw = s.g1Blocks * s.g1Data + s.g2Blocks * (s.g1Data + 1);
        std::vector<int> bits;
        const auto put = [&](int value, int count) {
            for (int i = count - 1; i >= 0; --i)
                bits.push_back((value >> i) & 1);
        };
        put(0x4, 4);
        put(int(bytes.size()), version <= 9 ? 8 : 16);
        for (uint8_t b : bytes)
            put(b, 8);
        put(0, std::min(4, dataCw * 8 - int(bits.size())));
        while (bits.size() % 8)
            bits.push_back(0);
        std::vector<uint8_t> data;
        for (size_t i = 0; i < bits.size(); i += 8) {
            int v = 0;
            for (int k = 0; k < 8; ++k)
                v = (v << 1) | bits[i + size_t(k)];
            data.push_back(uint8_t(v));
        }
        for (int pad = 0; int(data.size()) < dataCw; pad ^= 1)
            data.push_back(pad ? 0x11 : 0xEC);

        // Blocks, error correction, interleaving.
        const std::vector<uint8_t> gen = rsGenerator(s.ecPerBlock);
        std::vector<std::vector<uint8_t>> dataBlocks, ecBlocks;
        size_t pos = 0;
        for (int b = 0; b < blocks; ++b) {
            const int len = b < s.g1Blocks ? s.g1Data : s.g1Data + 1;
            dataBlocks.emplace_back(data.begin() + long(pos), data.begin() + long(pos) + len);
            pos += size_t(len);
            ecBlocks.push_back(rsRemainder(dataBlocks.back(), gen));
        }
        std::vector<uint8_t> stream;
        for (int i = 0; i <= s.g1Data; ++i)
            for (int b = 0; b < blocks; ++b)
                if (i < int(dataBlocks[size_t(b)].size()))
                    stream.push_back(dataBlocks[size_t(b)][size_t(i)]);
        for (int i = 0; i < s.ecPerBlock; ++i)
            for (int b = 0; b < blocks; ++b)
                stream.push_back(ecBlocks[size_t(b)][size_t(i)]);

        // Zig-zag placement, two columns at a time, right to left.
        size_t bit = 0;
        for (int right = size - 1; right >= 1; right -= 2) {
            if (right == 6)
                right = 5;
            for (int vert = 0; vert < size; ++vert) {
                for (int j = 0; j < 2; ++j) {
                    const int x = right - j;
                    const bool upward = ((right + 1) & 2) == 0;
                    const int y = upward ? size - 1 - vert : vert;
                    if (isFn(x, y))
                        continue;
                    bool dark = false;
                    if (bit < stream.size() * 8)
                        dark = ((stream[bit >> 3] >> (7 - (bit & 7))) & 1) != 0;
                    m_mod[size_t(y * size + x)] = dark;
                    ++bit;
                }
            }
        }

        // Best mask.
        int best = 0;
        long bestPenalty = -1;
        for (int mask = 0; mask < 8; ++mask) {
            applyMask(mask);
            formatBits(mask);
            const long p = penalty();
            if (bestPenalty < 0 || p < bestPenalty) {
                bestPenalty = p;
                best = mask;
            }
            applyMask(mask);   // undo (XOR)
        }
        applyMask(best);
        formatBits(best);
    }
};

} // namespace Qr
