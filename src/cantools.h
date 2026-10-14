#pragma once
//
// CAN bus health for the Automotive page: electrical levels from the analog
// channels, and frame statistics from the CAN decoder's frames. Plain C++
// with no Qt, tested standalone on simulated bus signals.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace CanTools {

// One decoded frame (assembled from the decoder's annotations).
struct CanFrame {
    double   start = 0.0;          // SOF, decoded-sample position
    double   end = 0.0;            // end of EOF
    uint32_t id = 0;
    bool     extended = false;
    std::vector<uint8_t> data;
    bool     error = false;        // a decoder warning or CRC error inside it
};

// What one wire (or a differential probe) does. Recessive is the bus's
// idle state — the level with the longest unbroken run (every frame ends
// with at least 11 recessive bits; bit stuffing caps dominant runs).
struct WireLevels {
    bool   active = false;     // switches between two levels (traffic)
    double idle = 0.0;         // median level, for a flat line
    double recessive = 0.0;
    double dominant = 0.0;
    // +1: dominant above recessive (CAN-H); -1: below (CAN-L);
    // +2: recessive near 0 V, dominant ~1.5..3 V (a differential probe).
    int    kind = 0;
};

inline double medianOf(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}

// Levels are medians of the samples well clear of the midpoint (outer 25%
// of the swing each side), so edges and ringing don't pull them.
inline WireLevels analyseWire(const std::vector<double> &v)
{
    WireLevels w;
    if (v.size() < 64)
        return w;
    std::vector<double> s(v);
    const size_t i5 = s.size() * 5 / 100, i95 = s.size() * 95 / 100;
    std::nth_element(s.begin(), s.begin() + long(i95), s.end());
    const double p95 = s[i95];
    std::nth_element(s.begin(), s.begin() + long(i5), s.begin() + long(i95));
    const double p5 = s[i5];
    w.idle = medianOf(v);
    const double range = p95 - p5;
    if (range < 0.3)
        return w;   // flat: no traffic (or the wire stuck)
    const double mid = (p5 + p95) / 2.0;
    std::vector<double> high, low;
    size_t runHigh = 0, runLow = 0, run = 0;
    bool state = v[0] > mid;
    for (double x : v) {
        const bool now = x > mid;
        if (now == state) {
            ++run;
        } else {
            (state ? runHigh : runLow) = std::max(state ? runHigh : runLow, run);
            state = now;
            run = 1;
        }
        if (x > mid + 0.25 * range)
            high.push_back(x);
        else if (x < mid - 0.25 * range)
            low.push_back(x);
    }
    (state ? runHigh : runLow) = std::max(state ? runHigh : runLow, run);
    if (high.empty() || low.empty())
        return w;
    const bool recessiveHigh = runHigh > runLow;
    w.recessive = medianOf(recessiveHigh ? high : low);
    w.dominant = medianOf(recessiveHigh ? low : high);
    w.active = true;
    if (std::fabs(w.recessive) < 0.6 && w.dominant > 0.8)
        w.kind = 2;
    else
        w.kind = w.dominant > w.recessive ? 1 : -1;
    return w;
}

// One row of the frame table.
struct IdStats {
    uint32_t id = 0;
    bool     extended = false;
    int      count = 0;
    int      errors = 0;
    double   period = 0.0;          // mean interval between its frames, samples (0: seen once)
    std::vector<uint8_t> lastData;
};

inline std::vector<IdStats> frameTable(const std::vector<CanFrame> &frames)
{
    std::map<std::pair<bool, uint32_t>, IdStats> byId;
    std::map<std::pair<bool, uint32_t>, std::pair<double, double>> firstLast;
    for (const CanFrame &f : frames) {
        const auto key = std::make_pair(f.extended, f.id);
        IdStats &s = byId[key];
        s.id = f.id;
        s.extended = f.extended;
        ++s.count;
        if (f.error)
            ++s.errors;
        else
            s.lastData = f.data;
        auto it = firstLast.find(key);
        if (it == firstLast.end())
            firstLast[key] = { f.start, f.start };
        else
            it->second.second = f.start;
    }
    std::vector<IdStats> rows;
    for (auto &entry : byId) {
        IdStats s = entry.second;
        const auto fl = firstLast[entry.first];
        if (s.count > 1)
            s.period = (fl.second - fl.first) / double(s.count - 1);
        rows.push_back(s);
    }
    return rows;   // sorted: standard IDs first, then extended, each by ID
}

// Share of the analysed span spent carrying frames (SOF to end of EOF).
inline double busLoad(const std::vector<CanFrame> &frames, double totalSamples)
{
    if (totalSamples <= 0.0)
        return 0.0;
    double busy = 0.0;
    for (const CanFrame &f : frames)
        busy += std::max(0.0, f.end - f.start);
    return std::min(1.0, busy / totalSamples);
}


// --- Independent check of one classic CAN frame ----------------------------
//
// Reads the frame's bits straight from the logic stream (recessive = 1,
// dominant = 0) the way sigrok's decoder does — sample point after each
// dominant edge, resynchronised on every dominant edge — destuffs, and checks
// the CRC-15. A second witness to the decoder-bit based check: a CRC error
// is only worth showing when both agree. Needs no decoder annotations, so it
// can't be thrown by how their sample ranges line up at exactly N samples per
// bit.
struct ClassicVerdict {
    bool valid = false;               // a well-formed frame was read (stuffing, enough bits)
    bool crcOk = false;
    std::uint32_t received = 0, computed = 0;
    int  bitsUsed = 0;
};

inline ClassicVerdict verifyClassicFrame(const std::uint8_t *logic, long long n, long long sof,
                                         double bitWidth, double samplePointPercent)
{
    ClassicVerdict v;
    if (!logic || sof < 0 || sof >= n || bitWidth < 3.0)
        return v;
    const double sp = bitWidth * samplePointPercent / 100.0;
    // Sampled wire bits (stuff bits included).
    std::vector<int> wire;
    double edge = double(sof);
    int edgeBit = 0;
    long long scan = sof;
    for (int k = 0; k < 170; ++k) {
        double pos = edge + double(k - edgeBit) * bitWidth + sp;
        while (double(scan) < pos && scan + 1 < n) {
            ++scan;
            if (logic[scan] == 0 && logic[scan - 1] == 1) {     // a dominant edge: resync
                edge = double(scan);
                edgeBit = k;
                pos = edge + sp;
            }
        }
        if (pos >= double(n))
            break;                          // out of data: whatever was read is judged below
        wire.push_back(logic[static_cast<long long>(pos)] ? 1 : 0);
    }
    // Destuff — and stop at the end of the CRC field: after it come the ACK
    // delimiter and EOF, legitimately 8+ recessive bits in a row, which would
    // read as a stuffing violation.
    std::vector<int> bits;
    int run = 0, last = -1;
    size_t dlcAt = 0, crcAt = 0, needed = 0;
    for (int b : wire) {
        if (run == 5) {
            if (b == last)
                return v;                                       // stuff error
            last = b;
            run = 1;
            continue;
        }
        run = (b == last) ? run + 1 : 1;
        last = b;
        bits.push_back(b);
        if (needed == 0 && bits.size() >= 14) {
            const bool extended = bits[13] == 1;                // IDE
            dlcAt = extended ? 35 : 15;
            if (bits.size() >= dlcAt + 4) {
                const bool rtr = extended ? bits[32] == 1 : bits[12] == 1;
                int dlc = 0;
                for (int k = 0; k < 4; ++k)
                    dlc = (dlc << 1) | bits[dlcAt + size_t(k)];
                crcAt = dlcAt + 4 + (rtr ? 0 : size_t(std::min(dlc, 8)) * 8);
                needed = crcAt + 15;
            }
        }
        if (needed != 0 && bits.size() >= needed)
            break;
    }
    if (bits.empty() || bits[0] != 0 || needed == 0 || bits.size() < needed)
        return v;
    std::uint32_t crc = 0;
    for (size_t i = 0; i < crcAt; ++i) {
        const std::uint32_t next = std::uint32_t(bits[i]) ^ ((crc >> 14) & 1u);
        crc = (crc << 1) & 0x7FFFu;
        if (next)
            crc ^= 0x4599u;
    }
    for (size_t i = 0; i < 15; ++i)
        v.received = (v.received << 1) | std::uint32_t(bits[crcAt + i]);
    v.computed = crc;
    v.crcOk = v.received == v.computed;
    v.valid = true;
    v.bitsUsed = int(crcAt + 15);
    return v;
}

// Reads the frame at several sample points and judges it good if any of them
// gives a valid CRC (a corrupted frame passes one by chance about 1 time in
// 32768; with eight points, 1 in 4000 — and a corrupted frame mostly reads
// the same at every point). Otherwise the verdict at the nominal point.
inline ClassicVerdict verifyClassicFrameAnyPoint(const std::uint8_t *logic, long long n, long long sof,
                                                 double bitWidth, double nominalPercent)
{
    ClassicVerdict nominal = verifyClassicFrame(logic, n, sof, bitWidth, nominalPercent);
    if (nominal.valid && nominal.crcOk)
        return nominal;
    for (double percent : { 60.0, 80.0, 50.0, 65.0, 75.0, 55.0, 85.0 }) {
        const ClassicVerdict v = verifyClassicFrame(logic, n, sof, bitWidth, percent);
        if (v.valid && v.crcOk)
            return v;
    }
    return nominal;
}

// --- CAN FD CRC (ISO 11898-1:2015) ----------------------------------------
//
// From the raw on-wire bits of one frame, raw[0] = SOF, stuff bits included.
// Destuffs (5 equal bits -> a complement stuff bit) to read the header; if
// FDF is set: the data length from the DLC, the CRC field after the data
// (a fixed stuff bit before it and after every 4th bit inside it, each the
// complement of the bit before), the stuff count (dynamic stuff bits mod 8,
// Gray-coded, plus even parity) and the CRC — CRC-17 (poly 0x1685B) for up
// to 16 data bytes, CRC-21 (0x102899, i.e. x^21 + 0x102899) above, register starting at 1 followed
// by zeros, over the raw bits from SOF to the end of the data field (dynamic
// stuff bits included) followed by the 4 stuff-count bits.
struct FdCrcCheck {
    bool   checked = false;          // an FD frame, complete enough to check
    int    crcBits = 0;              // 17 or 21
    int    dataBytes = 0;
    std::uint32_t received = 0, computed = 0;
    int    stuffCountReceived = -1, stuffCountExpected = -1;
    bool   parityOk = true;
    bool   fixedStuffOk = true;
    int    crcFieldStartRaw = 0, crcFieldEndRaw = 0;   // raw bit indices (inclusive start, exclusive end)
    bool   ok() const
    {
        return fixedStuffOk && parityOk && stuffCountReceived == stuffCountExpected && received == computed;
    }
};

inline int fdDataLength(int dlc)
{
    static const int table[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64 };
    return table[dlc & 15];
}

inline FdCrcCheck checkFdCrc(const std::vector<int> &raw)
{
    FdCrcCheck r;
    // Destuff from SOF, remembering where each data bit sits in the raw stream.
    std::vector<int> bits;            // destuffed
    std::vector<int> rawIndex;        // raw index of each destuffed bit
    int run = 0, last = -1, dynamicStuff = 0;
    size_t i = 0;
    int dataEnd = -1;                 // destuffed index one past the data field
    for (; i < raw.size(); ++i) {
        const int b = raw[i];
        if (run == 5) {               // this raw bit is a stuff bit
            if (b == last)
                return r;             // stuff error: not a valid frame to check
            ++dynamicStuff;
            run = 1;
            last = b;
            continue;
        }
        run = (b == last) ? run + 1 : 1;
        last = b;
        bits.push_back(b);
        rawIndex.push_back(int(i));
        const int n = int(bits.size());
        if (dataEnd < 0) {
            // Header layout, once enough bits are known.
            if (n >= 15 && bits[13] == 0 && bits[14] == 0)
                return r;                                         // base format, classic frame
            if (n >= 34 && bits[13] == 1 && bits[33] == 0)
                return r;                                         // extended format, classic frame
            const bool ext = n > 13 && bits[13] == 1;
            const int dlcAt = ext ? 37 : 18;
            if (n == dlcAt + 4) {
                int dlc = 0;
                for (int k = 0; k < 4; ++k)
                    dlc = (dlc << 1) | bits[dlcAt + k];
                r.dataBytes = fdDataLength(dlc);
                dataEnd = dlcAt + 4 + 8 * r.dataBytes;
            }
        }
        if (dataEnd >= 0 && n == dataEnd)
            break;                                                // last data bit read
    }
    if (dataEnd < 0 || int(bits.size()) != dataEnd)
        return r;                                                 // cut off before the data ended
    r.crcBits = r.dataBytes > 16 ? 21 : 17;
    // If the data field ends on five equal bits, a dynamic stuff bit would be
    // due exactly where the CRC field's fixed stuff bit starts. Readings of
    // the standard differ on whether it is inserted (then counted) or
    // replaced by the fixed stuff bit; both are tried, and the frame passes
    // if either is consistent (a corrupted frame fails both).
    const auto readField = [&](size_t afterData, int stuffCount) {
        FdCrcCheck c;
        c.dataBytes = r.dataBytes;
        c.crcBits = r.crcBits;
        const int fieldBits = 4 + c.crcBits;                      // stuff count (3 + parity) + CRC
        const int fixedStuff = (fieldBits + 3) / 4;               // before every group of 4
        const size_t fieldRaw = size_t(fieldBits + fixedStuff);
        if (raw.size() < afterData + fieldRaw)
            return c;                                             // cut off inside the CRC field
        c.crcFieldStartRaw = int(afterData);
        c.crcFieldEndRaw = int(afterData + fieldRaw);
        std::vector<int> field;
        int prev = raw[afterData - 1];
        size_t k = afterData;
        for (int g = 0; g < fieldBits; ++g) {
            if (g % 4 == 0) {
                if (raw[k] == prev)
                    c.fixedStuffOk = false;
                prev = raw[k];
                ++k;
            }
            field.push_back(raw[k]);
            prev = raw[k];
            ++k;
        }
        const int gray = (field[0] << 2) | (field[1] << 1) | field[2];
        int count = 0;                                            // Gray -> binary
        for (int g = gray; g; g >>= 1)
            count ^= g;
        c.stuffCountReceived = count;
        c.stuffCountExpected = stuffCount % 8;
        c.parityOk = ((field[0] + field[1] + field[2] + field[3]) % 2) == 0;
        for (int b = 0; b < c.crcBits; ++b)
            c.received = (c.received << 1) | std::uint32_t(field[size_t(4 + b)]);
        // The CRC over SOF .. end of data (raw, dynamic stuff bits included)
        // followed by the stuff count.
        const std::uint32_t poly = c.crcBits == 17 ? 0x1685Bu : 0x102899u;
        const std::uint32_t top = 1u << (c.crcBits - 1), mask = (1u << c.crcBits) - 1u;
        std::uint32_t crc = top;
        const auto feed = [&](int bit) {
            const std::uint32_t next = std::uint32_t(bit) ^ ((crc & top) ? 1u : 0u);
            crc = (crc << 1) & mask;
            if (next)
                crc ^= poly;
        };
        for (size_t b = 0; b < afterData; ++b)
            feed(raw[b]);
        for (int b = 0; b < 4; ++b)
            feed(field[size_t(b)]);
        c.computed = crc;
        c.checked = true;
        return c;
    };
    const size_t afterData = i + 1;                               // raw index after the last data bit
    FdCrcCheck plain = readField(afterData, dynamicStuff);
    if (run == 5 && afterData < raw.size() && raw[afterData] != last) {
        const FdCrcCheck withStuff = readField(afterData + 1, dynamicStuff + 1);
        if (withStuff.checked && withStuff.ok() && !(plain.checked && plain.ok()))
            return withStuff;
    }
    return plain;
}
} // namespace CanTools
