#pragma once
//
// LIN and K-Line bus health, Qt-free (tested on its own). Works on the same
// digital stream the protocol decoder gets (1 = recessive / idle, 0 =
// dominant) plus the analog samples for the wire levels, and measures what
// the decoder doesn't report: the real bit rate (LIN: from each sync byte,
// as the spec does it), break lengths, both LIN checksum types, headers that
// get no answer, KWP2000 structure and timing.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace SerialBus {

// ---- the wire ---------------------------------------------------------------

// Recessive (idle, near the battery) and dominant (driven low) levels of a
// single-wire bus: the medians of the samples above and below the middle of
// the 2nd..98th percentile span. Inactive without a 1 V swing.
struct WireLevels {
    bool   active = false;
    double recessive = 0.0, dominant = 0.0, idle = 0.0;
};

inline WireLevels analyseWire(const std::vector<double> &v)
{
    WireLevels w;
    if (v.size() < 16)
        return w;
    std::vector<double> s = v;
    std::sort(s.begin(), s.end());
    const double lo = s[s.size() / 50], hi = s[s.size() - 1 - s.size() / 50];
    w.idle = s[s.size() / 2];
    if (hi - lo < 1.0)
        return w;
    const double mid = 0.5 * (lo + hi);
    std::vector<double> top, bottom;
    for (double x : v)
        (x >= mid ? top : bottom).push_back(x);
    if (top.size() < v.size() / 100 || bottom.size() < v.size() / 200 || bottom.empty() || top.empty())
        return w;
    const auto median = [](std::vector<double> &a) {
        std::nth_element(a.begin(), a.begin() + std::ptrdiff_t(a.size() / 2), a.end());
        return a[a.size() / 2];
    };
    w.recessive = median(top);
    w.dominant = median(bottom);
    w.active = true;
    return w;
}

// ---- UART -------------------------------------------------------------------

struct Byte {
    long long start = 0;          // the start bit's first sample
    int  value = 0;
    bool framingError = false;    // stop bit not recessive
};
struct Break {
    long long start = 0, length = 0;   // samples
};
struct Stream {
    std::vector<Byte>  bytes;
    std::vector<Break> breaks;   // low for longer than any byte can be: a LIN break, a K-Line init / wake-up
    double bitTime = 0.0;        // measured, samples per bit (0: not enough edges)
};

// Bytes 8N1, LSB first, sampled at bit centres from each start edge. A low
// stretch of 11+ bit times is a break (a byte is low for 9 at most).
inline Stream decodeUart(const std::uint8_t *logic, long long n, double bitWidth)
{
    Stream out;
    if (!logic || n < 2 || bitWidth < 2.0)
        return out;
    const long long breakLen = static_cast<long long>(std::llround(11.0 * bitWidth));
    double sumPK = 0.0, sumKK = 0.0;
    long long i = 1;
    while (i < n) {
        if (!(logic[i] == 0 && logic[i - 1] == 1)) {
            ++i;
            continue;
        }
        const long long start = i;
        long long low = start;
        while (low < n && logic[low] == 0)
            ++low;
        if (low - start >= breakLen) {
            if (low < n)                                   // a break that ended inside the data
                out.breaks.push_back({ start, low - start });
            i = low;
            continue;
        }
        const long long stopAt = start + static_cast<long long>(std::llround(9.5 * bitWidth));
        if (stopAt >= n)
            break;                                         // cut off at the end
        Byte b;
        b.start = start;
        for (int k = 0; k < 8; ++k) {
            const long long at = start + static_cast<long long>(std::llround((k + 1.5) * bitWidth));
            if (logic[at])
                b.value |= 1 << k;
        }
        b.framingError = logic[stopAt] == 0;
        // The bit time, from where this byte's own edges fall (least squares
        // through the start edge): the bus's real rate, not the nominal one.
        const long long last = start + static_cast<long long>(9.0 * bitWidth);
        for (long long j = start + 1; j <= last && j < n; ++j) {
            if (logic[j] != logic[j - 1]) {
                const double p = double(j - start);
                const double k = std::round(p / bitWidth);
                if (k >= 1.0) {
                    sumPK += p * k;
                    sumKK += k * k;
                }
            }
        }
        out.bytes.push_back(b);
        i = stopAt;            // the next start edge comes after this stop bit's middle
    }
    if (sumKK > 0.0)
        out.bitTime = sumPK / sumKK;
    return out;
}

// ---- LIN --------------------------------------------------------------------

// A LIN bus's bit time, measured the way LIN slaves synchronise: after each
// break, the sync byte 0x55's five falling edges are two bits apart. Kept
// only when those gaps are even (within 15 %) and the low before them was
// at least 11 of those bits long (a break, not a data byte); the median of
// all frames. 0: no usable sync byte.
inline double linBitTimeFromSync(const std::uint8_t *logic, long long n)
{
    std::vector<double> bits;
    long long i = 1;
    while (logic && i < n) {
        if (!(logic[i] == 0 && logic[i - 1] == 1)) {
            ++i;
            continue;
        }
        const long long lowStart = i;
        while (i < n && logic[i] == 0)
            ++i;
        const double lowLen = double(i - lowStart);
        std::vector<long long> falls;
        for (long long j = i + 1; j < n && falls.size() < 5; ++j) {
            if (logic[j] == 0 && logic[j - 1] == 1)
                falls.push_back(j);
        }
        if (falls.size() < 5)
            break;
        double mean = 0.0;
        for (int k = 0; k < 4; ++k)
            mean += double(falls[size_t(k) + 1] - falls[size_t(k)]) / 4.0;
        bool even = true;
        for (int k = 0; k < 4; ++k)
            even = even && std::fabs(double(falls[size_t(k) + 1] - falls[size_t(k)]) - mean) <= 0.15 * mean;
        const double bit = mean / 2.0;
        if (even && bit >= 2.0 && lowLen >= 11.0 * bit)
            bits.push_back(bit);
    }
    if (bits.empty())
        return 0.0;
    std::nth_element(bits.begin(), bits.begin() + std::ptrdiff_t(bits.size() / 2), bits.end());
    return bits[bits.size() / 2];
}

// The standard rate within 2 % of a measured one, else the measurement.
inline long long linStandardBaud(double measured)
{
    for (long long standard : { 2400LL, 4800LL, 9600LL, 10417LL, 19200LL, 20000LL }) {
        if (std::fabs(measured - double(standard)) <= 0.02 * double(standard))
            return standard;
    }
    return static_cast<long long>(std::llround(measured));
}

struct LinFrame {
    long long start = 0, end = 0;   // break start .. last byte's stop bit
    double breakBits = 0.0;         // break length in nominal bit times
    double bitTime = 0.0;           // from this frame's sync byte (0: no good sync)
    bool   syncOk = true;           // 0x55 after the break
    int    pid = -1, id = -1;
    bool   parityOk = true;
    std::vector<std::uint8_t> data; // response without its checksum
    int    checksum = -1;
    bool   checksumOk = true;
    bool   otherTypeMatches = false;   // wrong, but right for the other checksum type
    bool   noResponse = false;         // a header nobody answered
    bool   framingError = false;
    bool error() const { return !syncOk || !parityOk || (!checksumOk && !otherTypeMatches) || framingError; }
};

inline bool linParityOk(int pid)
{
    const int id = pid & 0x3F;
    const auto b = [id](int k) { return (id >> k) & 1; };
    const int p0 = b(0) ^ b(1) ^ b(2) ^ b(4);
    const int p1 = 1 - (b(1) ^ b(3) ^ b(4) ^ b(5));
    return ((pid >> 6) & 1) == p0 && ((pid >> 7) & 1) == p1;
}

// Inverted 8-bit sum with carry (LIN): classic over the data, enhanced over
// the protected ID and the data.
inline int linChecksum(const std::vector<std::uint8_t> &data, int pid)
{
    unsigned sum = pid >= 0 ? unsigned(pid) : 0u;
    for (std::uint8_t b : data) {
        sum += b;
        if (sum > 255)
            sum -= 255;
    }
    return int(~sum & 0xFFu);
}

// One frame per break: the sync, the PID and everything until the next break
// (the response, its last byte the checksum). The frame after the last
// break counts only once the data has gone quiet after it.
inline std::vector<LinFrame> analyseLin(const std::uint8_t *logic, long long n, const Stream &s,
                                        double bitWidth, int version)
{
    std::vector<LinFrame> frames;
    for (size_t k = 0; k < s.breaks.size(); ++k) {
        const Break &br = s.breaks[k];
        const long long until = k + 1 < s.breaks.size() ? s.breaks[k + 1].start : n;
        std::vector<const Byte *> bytes;
        for (const Byte &b : s.bytes) {
            if (b.start > br.start + br.length && b.start < until)
                bytes.push_back(&b);
        }
        if (k + 1 == s.breaks.size()) {
            const long long lastEnd = bytes.empty() ? br.start + br.length
                : bytes.back()->start + static_cast<long long>(10.0 * bitWidth);
            if (n - lastEnd < static_cast<long long>(20.0 * bitWidth))
                break;                                   // may still be going: not judged
        }
        LinFrame f;
        f.start = br.start;
        f.breakBits = double(br.length) / bitWidth;
        f.end = bytes.empty() ? br.start + br.length
                              : bytes.back()->start + static_cast<long long>(10.0 * bitWidth);
        for (const Byte *b : bytes)
            f.framingError = f.framingError || b->framingError;
        if (bytes.empty() || bytes[0]->value != 0x55) {
            f.syncOk = false;
            frames.push_back(f);
            continue;
        }
        // The bit time from the sync byte's five falling edges (8 bits apart).
        {
            std::vector<long long> falls;
            const long long from = bytes[0]->start, to = from + static_cast<long long>(10.0 * bitWidth);
            for (long long j = from; j < to && j < n; ++j) {
                if (j > 0 && logic[j] == 0 && logic[j - 1] == 1)
                    falls.push_back(j);
            }
            if (falls.size() >= 5)
                f.bitTime = double(falls[4] - falls[0]) / 8.0;
        }
        if (bytes.size() < 2) {
            f.noResponse = true;                          // a break and sync only
            frames.push_back(f);
            continue;
        }
        f.pid = bytes[1]->value;
        f.id = f.pid & 0x3F;
        f.parityOk = linParityOk(f.pid);
        if (bytes.size() < 3) {
            f.noResponse = true;
            frames.push_back(f);
            continue;
        }
        for (size_t j = 2; j + 1 < bytes.size(); ++j)
            f.data.push_back(std::uint8_t(bytes[j]->value));
        f.checksum = bytes.back()->value;
        const bool diagnostic = f.id == 0x3C || f.id == 0x3D;     // always classic
        const int classic = linChecksum(f.data, -1), enhanced = linChecksum(f.data, f.pid);
        const bool enhancedExpected = version >= 2 && !diagnostic;
        f.checksumOk = f.checksum == (enhancedExpected ? enhanced : classic);
        f.otherTypeMatches = !f.checksumOk && !diagnostic && f.checksum == (enhancedExpected ? classic : enhanced);
        frames.push_back(f);
    }
    return frames;
}

// ---- K-Line -----------------------------------------------------------------

struct KLineMessage {
    long long start = 0, end = 0;
    std::vector<std::uint8_t> bytes;  // the whole message as received
    int  target = -1, source = -1;    // KWP2000 addresses, when present
    std::vector<std::uint8_t> data;   // KWP2000: service + parameters; otherwise the bytes
    bool checksumOk = true;           // KWP2000 only
    bool lengthOk = true;             // KWP2000: the bytes the header announced arrived
    bool framingError = false;
    bool error() const { return framingError || !checksumOk || !lengthOk; }
};
struct KLineReport {
    std::vector<KLineMessage> messages;
    int    inits = 0;                 // wake-up / init pulses (low for 11+ bit times)
    double maxInterByte = 0.0;        // samples, between bytes of one message
};

// Messages are separated by idle of at least gapBits; with KWP2000 framing
// each is parsed from its format byte (length in its low 6 bits, or a
// length byte; addresses when the top bits are set) and its checksum checked
// (sum of the preceding bytes, mod 256).
inline KLineReport analyseKLine(const Stream &s, double bitWidth, double gapBits, bool kwp)
{
    KLineReport r;
    r.inits = int(s.breaks.size());
    const double byteLen = 10.0 * bitWidth;
    std::vector<std::vector<const Byte *>> chunks;
    for (size_t i = 0; i < s.bytes.size(); ++i) {
        const Byte &b = s.bytes[i];
        bool split = chunks.empty();
        if (!split) {
            const Byte &prev = *chunks.back().back();
            const double gap = double(b.start) - (double(prev.start) + byteLen);
            split = gap >= gapBits * bitWidth;
            for (const Break &br : s.breaks)
                split = split || (br.start > prev.start && br.start < b.start);
        }
        if (split)
            chunks.emplace_back();
        chunks.back().push_back(&b);
    }
    const auto make = [&](const std::vector<const Byte *> &c, size_t from, size_t to) {
        KLineMessage m;
        m.start = c[from]->start;
        m.end = c[to - 1]->start + static_cast<long long>(byteLen);
        for (size_t j = from; j < to; ++j) {
            m.bytes.push_back(std::uint8_t(c[j]->value));
            m.framingError = m.framingError || c[j]->framingError;
            if (j > from)
                r.maxInterByte = std::max(r.maxInterByte,
                                          double(c[j]->start) - (double(c[j - 1]->start) + byteLen));
        }
        return m;
    };
    for (const auto &c : chunks) {
        if (!kwp) {
            KLineMessage m = make(c, 0, c.size());
            m.data = m.bytes;
            r.messages.push_back(std::move(m));
            continue;
        }
        size_t at = 0;
        while (at < c.size()) {
            const int fmt = c[at]->value;
            size_t idx = at + 1;
            int target = -1, source = -1;
            if ((fmt & 0xC0) != 0 && idx + 1 < c.size()) {
                target = c[idx]->value;
                source = c[idx + 1]->value;
                idx += 2;
            }
            int len = fmt & 0x3F;
            if (len == 0 && idx < c.size())
                len = c[idx++]->value;
            const size_t total = idx - at + size_t(len) + 1;
            const bool complete = at + total <= c.size();
            KLineMessage m = make(c, at, complete ? at + total : c.size());
            m.target = target;
            m.source = source;
            m.lengthOk = complete;
            if (complete) {
                unsigned sum = 0;
                for (size_t j = 0; j + 1 < m.bytes.size(); ++j)
                    sum += m.bytes[j];
                m.checksumOk = (sum & 0xFFu) == m.bytes.back();
                m.data.assign(m.bytes.begin() + std::ptrdiff_t(idx - at), m.bytes.end() - 1);
            }
            r.messages.push_back(std::move(m));
            at = complete ? at + total : c.size();
        }
    }
    return r;
}

// ---- tables -----------------------------------------------------------------

struct Row {
    int    key = 0;                  // LIN: frame ID; K-Line: target << 8 | source (or first byte)
    int    count = 0, errors = 0;
    double period = 0.0;             // samples between repeats (0: once)
    std::vector<std::uint8_t> lastData;
};

template <typename T, typename KeyFn, typename DataFn>
std::vector<Row> table(const std::vector<T> &items, KeyFn key, DataFn data)
{
    std::map<int, std::pair<Row, std::pair<long long, long long>>> rows;   // row, first / last start
    for (const T &it : items) {
        auto &e = rows[key(it)];
        Row &row = e.first;
        row.key = key(it);
        if (row.count == 0)
            e.second.first = it.start;
        e.second.second = it.start;
        ++row.count;
        row.errors += it.error() ? 1 : 0;
        row.lastData = data(it);
    }
    std::vector<Row> out;
    for (auto &[k, e] : rows) {
        Row row = e.first;
        if (row.count > 1)
            row.period = double(e.second.second - e.second.first) / double(row.count - 1);
        out.push_back(row);
    }
    return out;
}

} // namespace SerialBus
