#pragma once
//
// SENT (SAE J2716) decoding, Qt-free (tested on its own). Works on the
// digital stream the other decoders get (1 = high / idle, 0 = low).
//
// A frame is timed between falling edges, in ticks (nominally 3..90 us):
// a sync / calibration pulse of 56 ticks — the receiver measures the
// sensor's actual tick from it (clocks may be off by up to 20 %) — then a
// status nibble, 1..6 data nibbles and a CRC nibble, each 12..27 ticks for
// a value 0..15, then optionally a pause pulse (12..768 ticks) to keep
// frames a constant length.
//
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Sent {

struct Nibble {
    long long start = 0, end = 0;   // falling edge to falling edge
    int  value = -1;                // -1: out of range
};

struct Frame {
    long long start = 0, end = 0;   // sync's falling edge .. CRC nibble's end
    double tick = 0.0;              // samples per tick, from this frame's sync
    double tickDrift = 0.0;         // vs the previous frame's tick, fraction (0 for the first)
    Nibble sync, status, crc;
    std::vector<Nibble> data;
    int  crcRecommended = -1, crcLegacy = -1;
    bool crcOk = false;
    bool legacyCrc = false;         // matches only the legacy (2008) CRC
    bool nibbleError = false;       // a nibble outside 12..27 ticks
    bool shortLow = false;          // a low pulse under 4 ticks
    bool driftError = false;        // tick changed by more than 1.5625 % since the last frame
    double pauseTicks = 0.0;        // 0: no pause pulse after it
    // SPC (Short PWM Code): the ECU's master trigger pulse before the frame,
    // either merged with the sync's low part or just before it.
    double triggerTicks = 0.0;      // its low time in ticks (0: none seen)
    long long triggerStart = -1;    // its falling edge (= sync.start when merged)
    bool error() const { return !crcOk || nibbleError || shortLow || driftError; }
    // The two 12-bit fast channels most dual-signal sensors send with six
    // data nibbles: the first in nibbles 1..3, the second in 6..4.
    int fastChannel1() const { return data.size() == 6 ? data[0].value << 8 | data[1].value << 4 | data[2].value : -1; }
    int fastChannel2() const { return data.size() == 6 ? data[5].value << 8 | data[4].value << 4 | data[3].value : -1; }
};

// CRC-4, x^4 + x^3 + x^2 + 1, seed 5, over the data nibbles (not status).
// Recommended (J2716 2010): one more step with a zero nibble at the end;
// legacy (2008): without it.
inline void crc4(const std::vector<Nibble> &data, int *recommended, int *legacy)
{
    int table[16];
    for (int x = 0; x < 16; ++x) {
        int r = x << 4;
        for (int bit = 7; bit >= 4; --bit) {
            if (r & (1 << bit))
                r ^= 0x1D << (bit - 4);
        }
        table[x] = r & 0xF;
    }
    int c = 5;
    for (const Nibble &n : data)
        c = table[c] ^ (n.value & 0xF);
    *legacy = c;
    *recommended = table[c];
}

inline std::vector<Frame> decode(const std::uint8_t *logic, long long n, double nominalTick, int dataNibbles)
{
    std::vector<Frame> frames;
    if (!logic || n < 4 || nominalTick < 1.0)
        return frames;
    dataNibbles = std::clamp(dataNibbles, 1, 6);
    std::vector<long long> falls;
    for (long long i = 1; i < n; ++i) {
        if (logic[i] == 0 && logic[i - 1] == 1)
            falls.push_back(i);
    }
    const auto lowTime = [&](long long edge) {
        long long j = edge;
        while (j < n && logic[j] == 0)
            ++j;
        return double(j - edge);
    };
    const double syncMin = 56.0 * nominalTick * 0.75, syncMax = 56.0 * nominalTick * 1.25;
    const auto isSync = [&](size_t k) {
        if (k + 1 >= falls.size())
            return false;
        const double p = double(falls[k + 1] - falls[k]);
        return p >= syncMin && p <= syncMax;
    };
    const size_t nibbles = size_t(dataNibbles) + 2;      // status, data, CRC
    double lastTick = 0.0;
    size_t k = 0;
    while (k + 1 + nibbles < falls.size()) {
        if (!isSync(k) || isSync(k + 1)) {               // a sync followed by a sync-length period: that was a pause
            ++k;
            continue;
        }
        Frame f;
        f.start = falls[k];
        f.sync = { falls[k], falls[k + 1], -1 };
        f.tick = double(falls[k + 1] - falls[k]) / 56.0;
        f.shortLow = lowTime(falls[k]) < 4.0 * f.tick;
        {
            // SPC trigger: a sync whose low part is long (the trigger and the
            // sync merged), or a short low pulse just before it that isn't
            // the end of the previous frame.
            const double syncLow = lowTime(falls[k]) / f.tick;
            const bool afterFrame = !frames.empty() && falls[k] <= frames.back().end
                                    + static_cast<long long>(frames.back().pauseTicks * frames.back().tick) + 1;
            if (syncLow >= 8.0) {
                f.triggerTicks = syncLow;
                f.triggerStart = falls[k];
            } else if (k > 0 && !afterFrame && double(falls[k] - falls[k - 1]) < 100.0 * f.tick
                       && (frames.empty() || falls[k - 1] > frames.back().end)) {
                f.triggerTicks = lowTime(falls[k - 1]) / f.tick;
                f.triggerStart = falls[k - 1];
            }
        }
        std::vector<Nibble> read;
        bool ok = true;
        for (size_t j = 0; j < nibbles; ++j) {
            const long long a = falls[k + 1 + j], b = falls[k + 2 + j];
            const double ticks = double(b - a) / f.tick;
            Nibble nb{ a, b, -1 };
            if (ticks >= 11.5 && ticks <= 27.5)
                nb.value = int(std::lround(ticks)) - 12;
            else
                ok = false;
            f.shortLow = f.shortLow || lowTime(a) < 4.0 * f.tick;
            read.push_back(nb);
            if (!ok)
                break;
        }
        if (!ok) {
            // A real sync with a broken nibble after it: an error frame,
            // then look for the next sync from the broken one.
            f.nibbleError = true;
            f.status = read.front();
            f.end = read.back().end;
            frames.push_back(f);
            k += read.size();
            continue;
        }
        f.status = read[0];
        f.data.assign(read.begin() + 1, read.end() - 1);
        f.crc = read.back();
        f.end = f.crc.end;
        crc4(f.data, &f.crcRecommended, &f.crcLegacy);
        f.crcOk = f.crc.value == f.crcRecommended || f.crc.value == f.crcLegacy;
        f.legacyCrc = f.crc.value == f.crcLegacy && f.crc.value != f.crcRecommended;
        if (lastTick > 0.0) {
            f.tickDrift = (f.tick - lastTick) / lastTick;
            f.driftError = std::fabs(f.tickDrift) > 0.015625;
        }
        lastTick = f.tick;
        k += 1 + nibbles;                                // falls[k] is now the CRC's end
        // A pause pulse: the next period, when it isn't the next sync.
        if (k + 1 < falls.size() && !isSync(k)) {
            const double ticks = double(falls[k + 1] - falls[k]) / f.tick;
            if (ticks >= 11.5 && ticks <= 768.5) {
                f.pauseTicks = ticks;
                ++k;
            }
        }
        frames.push_back(f);
    }
    return frames;
}

// ---- slow channel: serial messages in status bits 2 and 3 --------------

struct SerialMessage {
    long long start = 0, end = 0;   // first frame's start .. last frame's end
    bool enhanced = false;
    bool config = false;            // enhanced, C bit set: 4-bit ID and 16-bit data
    int  id = -1, data = -1, dataBits = 0;
    int  crc = -1;
    bool crcOk = false;
    // Enhanced: the CRC-6 bit order isn't confirmed here, so a mismatch is
    // shown as unverified, never as an error.
    bool crcVerified = true;
    bool error() const { return !crcOk && crcVerified; }
};

// CRC-6, x^6 + x^4 + x^3 + 1, seed 010101, over the bits MSB first,
// then six zero bits.
inline int crc6(const std::vector<int> &bits)
{
    int c = 0x15;
    const auto step = [&c](int bit) {
        const int feedback = ((c >> 5) & 1) ^ (bit & 1);
        c = (c << 1) & 0x3F;
        if (feedback)
            c ^= 0x19;
    };
    for (int b : bits)
        step(b);
    for (int i = 0; i < 6; ++i)
        step(0);
    return c;
}

// Short messages: 16 frames, bit 3 set in the first only; bit 2 carries a
// 4-bit ID, 8-bit data and a 4-bit CRC (the fast channel's CRC-4 over the
// three nibbles, either variant). Enhanced: 18 frames, bit 3 =
// 1 1 1 1 1 1 0 C a a a a 0 b b b b 0; bit 2 = a 6-bit CRC then 12 data
// bits; C = 0: 8-bit ID (a, b), 12-bit data; C = 1: 4-bit ID (a), the b
// bits on top of the data (16-bit). Frames with a bad nibble break a message.
inline std::vector<SerialMessage> slowChannel(const std::vector<Frame> &frames)
{
    std::vector<SerialMessage> out;
    const auto b3 = [&](size_t k) { return (frames[k].status.value >> 3) & 1; };
    const auto b2 = [&](size_t k) { return (frames[k].status.value >> 2) & 1; };
    const auto clean = [&](size_t from, size_t count) {
        if (from + count > frames.size())
            return false;
        for (size_t k = from; k < from + count; ++k) {
            if (frames[k].nibbleError || frames[k].status.value < 0)
                return false;
        }
        return true;
    };
    size_t i = 0;
    while (i < frames.size()) {
        if (clean(i, 18)) {
            bool marker = true;
            for (size_t k = 0; k < 6; ++k)
                marker = marker && b3(i + k) == 1;
            marker = marker && b3(i + 6) == 0 && b3(i + 12) == 0 && b3(i + 17) == 0;
            if (marker) {
                SerialMessage m;
                m.enhanced = true;
                m.start = frames[i].start;
                m.end = frames[i + 17].end;
                m.config = b3(i + 7) == 1;
                int a = 0, b = 0, d = 0, crc = 0;
                for (size_t k = 8; k <= 11; ++k) a = a << 1 | b3(i + k);
                for (size_t k = 13; k <= 16; ++k) b = b << 1 | b3(i + k);
                for (size_t k = 6; k <= 17; ++k) d = d << 1 | b2(i + k);
                for (size_t k = 0; k <= 5; ++k) crc = crc << 1 | b2(i + k);
                m.id = m.config ? a : (a << 4 | b);
                m.data = m.config ? (b << 12 | d) : d;
                m.dataBits = m.config ? 16 : 12;
                m.crc = crc;
                std::vector<int> bit2First, bit3First;          // the 24 bits of frames 7..18, both orders
                for (size_t k = 6; k <= 17; ++k) {
                    bit2First.push_back(b2(i + k));
                    bit2First.push_back(b3(i + k));
                    bit3First.push_back(b3(i + k));
                    bit3First.push_back(b2(i + k));
                }
                m.crcOk = crc == crc6(bit2First) || crc == crc6(bit3First);
                m.crcVerified = m.crcOk;
                out.push_back(m);
                i += 18;
                continue;
            }
        }
        if (clean(i, 16) && b3(i) == 1) {
            bool marker = true;
            for (size_t k = 1; k < 16; ++k)
                marker = marker && b3(i + k) == 0;
            if (marker) {
                SerialMessage m;
                m.start = frames[i].start;
                m.end = frames[i + 15].end;
                int v = 0;
                for (size_t k = 0; k < 16; ++k)
                    v = v << 1 | b2(i + k);
                m.id = v >> 12;
                m.data = (v >> 4) & 0xFF;
                m.dataBits = 8;
                m.crc = v & 0xF;
                std::vector<Nibble> nibbles(3);
                nibbles[0].value = m.id;
                nibbles[1].value = (m.data >> 4) & 0xF;
                nibbles[2].value = m.data & 0xF;
                int recommended = 0, legacy = 0;
                crc4(nibbles, &recommended, &legacy);
                m.crcOk = m.crc == recommended || m.crc == legacy;
                out.push_back(m);
                i += 16;
                continue;
            }
        }
        ++i;
    }
    return out;
}

} // namespace Sent
