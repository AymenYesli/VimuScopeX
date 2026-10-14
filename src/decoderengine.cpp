// libsigrokdecode (and the glib it uses) first, before any Qt header, so
// Qt's keyword macros never touch them.
#ifdef VSX_HAVE_SRD
#include <glib.h>
#include <libsigrokdecode/libsigrokdecode.h>
// Written against the 0.5 API (unit size passed to srd_session_send(), not
// to srd_inst_channel_set_all()); older releases differ in both.
#if defined(SRD_PACKAGE_VERSION_MAJOR) && defined(SRD_PACKAGE_VERSION_MINOR) \
    && SRD_PACKAGE_VERSION_MAJOR == 0 && SRD_PACKAGE_VERSION_MINOR < 5
#error "libsigrokdecode 0.5 or newer is required for protocol decoding"
#endif
#endif

#include "decoderengine.h"
#include <cstdint>
#include <cstdio>
#include <cmath>
#include "perftrace.h"
#include <limits>

#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QPair>

#include <algorithm>
#include <cstring>
#include <mutex>

bool DecoderEngine::available()
{
#ifdef VSX_HAVE_SRD
    return true;
#else
    return false;
#endif
}

#ifdef VSX_HAVE_SRD
namespace {

// Annotations arrive on libsigrokdecode's own decoder threads, one per
// instance, so collecting them needs a lock. Only the top decoder of the
// stack is kept (the UART under LIN / K-Line reports its own bytes too).
struct Collector {
    std::mutex mutex;
    QByteArray topId;
    QVector<DecodeAnnotation> annotations;
    // CAN only, for the CRC check (see checkCanCrc()): the decoder's own
    // per-bit results, which bits were stuff bits, and frame landmarks.
    struct Bit { quint64 start, end; int value; };
    QVector<Bit> bits;
    QVector<QPair<quint64, quint64>> stuffBits;
    QVector<quint64> frameStarts;
    struct Crc { quint64 start, end; quint32 value; };
    QVector<Crc> classicCrcs;
};

void annotationCallback(struct srd_proto_data *pdata, void *cbData)
{
    auto *c = static_cast<Collector *>(cbData);
    if (!pdata || !pdata->pdo || !pdata->pdo->di || !pdata->pdo->di->decoder)
        return;
    const struct srd_decoder *dec = pdata->pdo->di->decoder;
    if (!dec->id || c->topId != QByteArray(dec->id))
        return;
    const auto *ann = static_cast<const struct srd_proto_data_annotation *>(pdata->data);
    if (!ann || !ann->ann_text)
        return;

    // The class's own id ("bit", "warnings", ...), for filtering and to
    // tell errors from ordinary fields.
    QByteArray className;
    if (const auto *entry = static_cast<char **>(
            g_slist_nth_data(dec->annotations, guint(ann->ann_class)))) {
        if (entry[0])
            className = entry[0];
    }
    // Individual bits (and CAN stuff bits) are far too fine for one bus
    // row; the fields they make up are what the row shows. They are kept
    // aside for the CAN CRC check, though.
    if (className == "bit" || className == "stuff-bit") {
        std::lock_guard<std::mutex> lock(c->mutex);
        if (className == "stuff-bit") {
            c->stuffBits.append({ pdata->start_sample, pdata->end_sample });
        } else if (ann->ann_text[0] && (ann->ann_text[0][0] == '0' || ann->ann_text[0][0] == '1')) {
            c->bits.append({ pdata->start_sample, pdata->end_sample,
                             ann->ann_text[0][0] == '1' ? 1 : 0 });
        }
        return;
    }

    DecodeAnnotation a;
    a.start = double(pdata->start_sample);
    a.end   = double(pdata->end_sample);
    for (char **t = ann->ann_text; *t; ++t)
        a.texts << QString::fromUtf8(*t);
    if (a.texts.isEmpty())
        return;
    a.error = className.contains("warn") || className.contains("error");
    a.group = QString::fromUtf8(className);

    std::lock_guard<std::mutex> lock(c->mutex);
    if (className == "sof" || a.texts.first().startsWith(QLatin1String("Start of frame")))
        c->frameStarts.append(pdata->start_sample);
    // Classic CAN's CRC-15 only; CAN FD's CRC-17/21 also cover stuff bits
    // and a stuff count, which this check doesn't model.
    for (const QString &text : a.texts) {
        if (text.contains(QLatin1String("CRC")) && !text.contains(QLatin1String("CRC-17"))
            && !text.contains(QLatin1String("CRC-21"))) {
            const int hex = text.indexOf(QLatin1String("0x"));
            bool ok = false;
            const quint32 value = hex >= 0 ? text.mid(hex + 2).section(QLatin1Char(' '), 0, 0).toUInt(&ok, 16) : 0;
            if (ok)
                c->classicCrcs.append({ pdata->start_sample, pdata->end_sample, value });
            break;
        }
    }
    c->annotations.append(a);
}

// sigrok's CAN decoder shows each frame's CRC but doesn't check it, so a
// frame with a corrupted payload but valid structure looked clean. This
// recomputes classic CAN's CRC-15 (poly 0x4599) over the decoder's own
// destuffed bits from the start of frame to the CRC field, and adds a red
// annotation where it disagrees with the CRC on the wire. Verified against
// a real capture (ID 0x12E, 8 bytes, CRC 0x67b6). If the bit count isn't
// a valid classic frame length (19 + 8n standard, 39 + 8n extended, one
// fewer if the SOF bit isn't reported), the bits aren't what this expects
// and the frame is skipped rather than flagged falsely.
void checkCanCrc(Collector &c, const QByteArray &logic, double bitWidth, double samplePointPercent)
{
    for (const Collector::Crc &crc : c.classicCrcs) {
        quint64 sof = 0;
        bool haveSof = false;
        for (quint64 s : c.frameStarts) {
            if (s < crc.start && (!haveSof || s > sof)) {
                sof = s;
                haveSof = true;
            }
        }
        if (!haveSof)
            continue;
        // The frame's bits: each belongs by where its middle falls, not by
        // comparing exact start / end samples. At a whole number of samples
        // per bit the decoder's ranges meet exactly at their boundaries, and
        // a comparison that is off by one sample there drops or keeps a bit.
        QVector<Collector::Bit> frame;
        for (const Collector::Bit &b : c.bits) {
            const double mid = 0.5 * (double(b.start) + double(b.end));
            if (mid < double(sof) || mid >= double(crc.start))
                continue;
            bool stuffed = false;
            for (const auto &s : c.stuffBits) {
                if (mid >= double(s.first) && mid <= double(s.second)) {
                    stuffed = true;
                    break;
                }
            }
            if (!stuffed)
                frame.append(b);
        }
        const int mod = int(frame.size() % 8);
        if (frame.size() < 18 || !(mod == 2 || mod == 3 || mod == 6 || mod == 7))
            continue;
        std::sort(frame.begin(), frame.end(),
                  [](const Collector::Bit &x, const Collector::Bit &y) { return x.start < y.start; });
        quint32 value = 0;
        for (const Collector::Bit &b : frame) {
            const int next = b.value ^ int((value >> 14) & 1);
            value = (value << 1) & 0x7FFF;
            if (next)
                value ^= 0x4599;
        }
        if (value == crc.value)
            continue;

        // The decoder's bits disagree with its CRC. Before saying so: read the
        // frame again straight from the samples. ACKed frames were being
        // flagged (an ACK means the other nodes' CRC checks passed), so an
        // accusation needs this second, independent witness.
        const CanTools::ClassicVerdict independent = CanTools::verifyClassicFrameAnyPoint(
            reinterpret_cast<const std::uint8_t *>(logic.constData()), logic.size(), qint64(sof),
            bitWidth, samplePointPercent);
        if (PerfTrace::enabled()) {
            std::fprintf(stderr,
                         "[can] frame at sample %llu: decoder bits give CRC 0x%04x vs 0x%04x on the wire (%d bits used); "
                         "independent read: %s%s, %d bits\n",
                         static_cast<unsigned long long>(sof), unsigned(value), unsigned(crc.value),
                         int(frame.size()), independent.valid ? "readable, CRC " : "unreadable",
                         independent.valid ? (independent.crcOk ? "OK" : "mismatch") : "", independent.bitsUsed);
            std::fflush(stderr);
        }
        if (!independent.valid || independent.crcOk)
            continue;                       // not confirmed: no accusation

        // Confirmed. Replace the decoder's own CRC box (two overlapping
        // annotations made an unreadable mess) with the error.
        for (int i = c.annotations.size() - 1; i >= 0; --i) {
            const DecodeAnnotation &old = c.annotations[i];
            if (quint64(old.start) == crc.start && quint64(old.end) == crc.end
                && !old.texts.isEmpty() && old.texts.first().contains(QLatin1String("CRC"))
                && old.group != QLatin1String("crc-check"))
                c.annotations.removeAt(i);
        }
        DecodeAnnotation a;
        a.start = double(crc.start);
        a.end = double(crc.end);
        a.texts << QStringLiteral("CRC error: 0x%1, expected 0x%2")
                       .arg(independent.received, 4, 16, QLatin1Char('0'))
                       .arg(independent.computed, 4, 16, QLatin1Char('0'))
                << QStringLiteral("CRC error") << QStringLiteral("CRC!");
        a.error = true;
        a.group = QStringLiteral("crc-check");
        c.annotations.append(a);
    }
}

// CAN FD frames: CRC-17 / CRC-21, the stuff count and the fixed stuff bits,
// checked from the raw on-wire bits (CanTools::checkFdCrc(), ISO 11898-1:2015)
// rather than from the decoder's own reading of the FD CRC field. The raw
// stream is the decoder's bits plus any stuff bit it marked without a bit of
// its own (complement of the bit before), so it's right either way. A red
// annotation over the CRC field where it disagrees; nothing when it agrees.
void checkCanFd(Collector &c)
{
    if (c.bits.isEmpty() || c.frameStarts.isEmpty())
        return;
    QVector<Collector::Bit> raw = c.bits;
    for (const auto &s : c.stuffBits) {
        const double stuffMid = 0.5 * (double(s.first) + double(s.second));
        const bool covered = std::any_of(c.bits.cbegin(), c.bits.cend(), [&](const Collector::Bit &b) {
            return stuffMid >= double(b.start) && stuffMid <= double(b.end);   // by the middle (see checkCanCrc)
        });
        if (!covered)
            raw.append({ s.first, s.second, -1 });
    }
    std::sort(raw.begin(), raw.end(),
              [](const Collector::Bit &x, const Collector::Bit &y) { return x.start < y.start; });
    for (int i = 0; i < raw.size(); ++i) {
        if (raw[i].value < 0)
            raw[i].value = i > 0 ? 1 - raw[i - 1].value : 1;
    }
    QVector<quint64> starts = c.frameStarts;
    std::sort(starts.begin(), starts.end());
    for (int f = 0; f < starts.size(); ++f) {
        const quint64 sof = starts[f];
        const quint64 next = f + 1 < starts.size() ? starts[f + 1] : std::numeric_limits<quint64>::max();
        auto it = std::lower_bound(raw.cbegin(), raw.cend(), sof,
                                   [](const Collector::Bit &b, quint64 s) { return b.start < s; });
        std::vector<int> bits;
        QVector<const Collector::Bit *> where;
        for (; it != raw.cend() && it->start < next && bits.size() < 1200; ++it) {
            bits.push_back(it->value);
            where.append(&*it);
        }
        if (bits.size() < 30)
            continue;
        const CanTools::FdCrcCheck r = CanTools::checkFdCrc(bits);
        if (!r.checked || r.ok())
            continue;
        DecodeAnnotation a;
        a.start = double(where[r.crcFieldStartRaw]->start);
        a.end = double(where[r.crcFieldEndRaw - 1]->end);
        if (!r.fixedStuffOk) {
            a.texts << QStringLiteral("Fixed stuff bit error in the CRC field")
                    << QStringLiteral("Stuff bit error") << QStringLiteral("Stuff!");
        } else if (r.stuffCountReceived != r.stuffCountExpected || !r.parityOk) {
            a.texts << QStringLiteral("Stuff count error: %1, expected %2%3")
                           .arg(r.stuffCountReceived).arg(r.stuffCountExpected)
                           .arg(r.parityOk ? QString() : QStringLiteral(" (parity)"))
                    << QStringLiteral("Stuff count error") << QStringLiteral("SC!");
        } else {
            const int digits = (r.crcBits + 3) / 4;
            a.texts << QStringLiteral("CRC-%1 error: 0x%2, expected 0x%3").arg(r.crcBits)
                           .arg(r.received, digits, 16, QLatin1Char('0'))
                           .arg(r.computed, digits, 16, QLatin1Char('0'))
                    << QStringLiteral("CRC error") << QStringLiteral("CRC!");
        }
        a.error = true;
        a.group = QStringLiteral("crc-check");
        c.annotations.append(a);
    }
}

// A frame running into the end of the decoded data (its bits reach the last
// few bit times) used to vanish from the bus row; it gets a marker instead.
// Not an error — the bus-health statistics leave it out.
void markCutOffFrame(Collector &c, quint64 sampleCount)
{
    if (c.frameStarts.isEmpty() || c.bits.isEmpty())
        return;
    const quint64 lastSof = *std::max_element(c.frameStarts.cbegin(), c.frameStarts.cend());
    double lastEof = -1.0;
    for (const DecodeAnnotation &a : c.annotations) {
        if (a.group == QLatin1String("eof") || (!a.texts.isEmpty() && a.texts.first().startsWith(QLatin1String("End of frame"))))
            lastEof = std::max(lastEof, a.end);
    }
    if (lastEof >= double(lastSof))
        return;                                                   // the last frame completed
    quint64 lastBitEnd = 0, bitLength = 0;
    for (const Collector::Bit &b : c.bits) {
        if (b.end > lastBitEnd) {
            lastBitEnd = b.end;
            bitLength = b.end - b.start;
        }
    }
    if (lastBitEnd + 3 * std::max<quint64>(bitLength, 1) < sampleCount)
        return;                                                   // broke off mid-way: the decoder's own warnings say so
    DecodeAnnotation a;
    a.start = double(lastSof);
    a.end = double(sampleCount > 0 ? sampleCount - 1 : lastSof);
    a.texts << QStringLiteral("Frame cut off at the end of the view") << QStringLiteral("Cut off")
            << QStringLiteral("\u2026");
    a.error = false;
    a.group = QStringLiteral("cut-off");
    c.annotations.append(a);
}

// Whole CAN frames from the decoder's annotations, for the bus-health
// panel: SOF opens a frame, EOF closes it; the identifier (the full one for
// extended frames) and data bytes come from their annotations' "0x.." text;
// any warning or CRC error inside makes it an error frame. A SOF before the
// previous frame closed means that one broke off: kept, as an error.
std::vector<CanTools::CanFrame> assembleCanFrames(QVector<DecodeAnnotation> anns, int *stray,
                                                  double from = 0.0, double to = 0.0)
{
    // from / to: only frames starting inside the window (each is counted once
    // however you pan) and errors outside any frame inside it.
    const bool windowed = to > from;
    const auto inside = [&](double position) { return !windowed || (position >= from && position < to); };
    std::sort(anns.begin(), anns.end(), [](const DecodeAnnotation &a, const DecodeAnnotation &b) {
        return a.start < b.start;
    });
    static const QRegularExpression hex(QStringLiteral("0x([0-9a-fA-F]+)"));
    std::vector<CanTools::CanFrame> frames;
    CanTools::CanFrame frame;
    bool open = false;
    *stray = 0;
    for (const DecodeAnnotation &a : anns) {
        if (a.group == QLatin1String("sof")) {
            if (open) {
                frame.error = true;
                frame.end = a.start;
                if (inside(frame.start))
                    frames.push_back(frame);
            }
            frame = CanTools::CanFrame();
            frame.start = a.start;
            open = true;
            continue;
        }
        if (!open) {
            if (a.error && inside(a.start))
                ++*stray;
            continue;
        }
        if (a.error) {
            frame.error = true;
            continue;
        }
        const QRegularExpressionMatch m = hex.match(a.texts.first());
        if (a.group == QLatin1String("id") || a.group == QLatin1String("full-id")) {
            if (m.hasMatch()) {
                frame.id = m.captured(1).toUInt(nullptr, 16);
                frame.extended = a.group == QLatin1String("full-id");
            }
        } else if (a.group == QLatin1String("data")) {
            if (m.hasMatch())
                frame.data.push_back(uint8_t(m.captured(1).toUInt(nullptr, 16)));
        } else if (a.group == QLatin1String("eof")) {
            frame.end = a.end;
            if (inside(frame.start))
                frames.push_back(frame);
            open = false;
        }
    }
    return frames;
}

// Options table for srd_inst_new(): string keys, GVariant values, both
// owned by the table. Integer options must be int64 and float ones double
// to match the decoder's own defaults, or libsigrokdecode rejects them.
GHashTable *newOptions()
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                 reinterpret_cast<GDestroyNotify>(g_variant_unref));
}

void setInt(GHashTable *t, const char *key, qint64 v)
{
    g_hash_table_insert(t, g_strdup(key), g_variant_ref_sink(g_variant_new_int64(v)));
}

void setDouble(GHashTable *t, const char *key, double v)
{
    g_hash_table_insert(t, g_strdup(key), g_variant_ref_sink(g_variant_new_double(v)));
}

void setString(GHashTable *t, const char *key, const char *v)
{
    g_hash_table_insert(t, g_strdup(key), g_variant_ref_sink(g_variant_new_string(v)));
}

struct srd_decoder_inst *newInstance(struct srd_session *sess, const char *id,
                                     GHashTable *options, const char *channel)
{
    if (!srd_decoder_get_by_id(id))
        srd_decoder_load(id);
    if (!srd_decoder_get_by_id(id)) {
        g_hash_table_destroy(options);
        return nullptr;
    }
    struct srd_decoder_inst *di = srd_inst_new(sess, id, options);
    g_hash_table_destroy(options);
    if (!di)
        return nullptr;
    if (channel) {
        // Every instance reads bit 0 of the one-byte-per-sample stream.
        GHashTable *channels = newOptions();
        g_hash_table_insert(channels, g_strdup(channel),
                            g_variant_ref_sink(g_variant_new_int32(0)));
        srd_inst_channel_set_all(di, channels);
        g_hash_table_destroy(channels);
    }
    return di;
}

} // namespace
#endif

bool DecoderEngine::ensureInit()
{
#ifdef VSX_HAVE_SRD
    if (m_initTried)
        return m_initOk;
    m_initTried = true;
    // Our own decoders (K-Line) ship next to the executable; srd_init()
    // adds that directory alongside the system decoder directories.
    const QByteArray extra = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("decoders")).toLocal8Bit();
    const int ret = srd_init(extra.constData());
    if (ret != SRD_OK) {
        m_initError = QStringLiteral("libsigrokdecode failed to start: %1")
                          .arg(QString::fromUtf8(srd_strerror(ret)));
        return false;
    }
    m_initOk = true;
    return true;
#else
    return false;
#endif
}

void DecoderEngine::decode(const DecodeJob &job)
{
    DecodeResult result;
    result.slot = job.slot;
    result.id = job.id;

#ifndef VSX_HAVE_SRD
    result.message = QStringLiteral(
        "Decoding needs libsigrokdecode — install it and rebuild");
    emit finished(result);
    return;
#else
    if (!ensureInit()) {
        result.message = m_initError;
        emit finished(result);
        return;
    }
    int n = job.volts.size();
    if (n < 16 || job.sampleRate <= 0.0) {
        result.message = QStringLiteral("No signal to decode");
        emit finished(result);
        return;
    }

    // Threshold the analog trace into a one-bit logic stream.
    QByteArray logic(n, '\0');
    for (int i = 0; i < n; ++i) {
        bool high = job.volts[i] > job.threshold;
        if (job.invert)
            high = !high;
        logic[i] = high ? 1 : 0;
    }

    // CAN polarity, from the signal itself rather than the Input/Invert
    // choice: sigrok's decoder needs recessive = 1, and choosing CAN-H vs
    // CAN-L wrongly (or Invert) makes it decode the idle bus as a stream of
    // dominant 0 bits — frames of ID 0 and 0x00 data over silence, real
    // frames garbled into red errors and false CAN FD. Recessive is always
    // the level with the longest unbroken run: every frame ends with at
    // least 11 recessive bits (ACK delimiter, EOF, intermission) while bit
    // stuffing caps dominant runs at 5 bits (12 in the worst case of
    // overlapping error flags). A flat, idle trace also comes out as all
    // recessive, so it decodes to nothing instead of zeros.
    if (job.protocol == QLatin1String("can") && n > 0) {
        int longest[2] = { 0, 0 };
        char level = logic[0];
        int run = 0;
        for (int i = 0; i < n; ++i) {
            if (logic[i] == level) {
                ++run;
            } else {
                longest[int(level)] = std::max(longest[int(level)], run);
                level = logic[i];
                run = 1;
            }
        }
        longest[int(level)] = std::max(longest[int(level)], run);
        if (longest[0] > longest[1]) {
            for (int i = 0; i < n; ++i)
                logic[i] = char(logic[i] ^ 1);
        }
    }

    // LIN on Auto: the bit rate from the sync bytes (LIN slaves do the same),
    // used for everything below. Nothing usable: 19200, and the row says so.
    qint64 baud = job.baud;
    if (job.protocol == QLatin1String("lin") && baud <= 0) {
        const double bit = SerialBus::linBitTimeFromSync(reinterpret_cast<const std::uint8_t *>(logic.constData()),
                                                         logic.size());
        if (bit > 0.0) {
            baud = SerialBus::linStandardBaud(job.sampleRate / bit);
            result.detectedBaud = baud;
        } else {
            baud = 19200;
        }
    }

    // Record mode: the data starts well before the window, so a frame cut by
    // the window's left edge is here whole. Decoding starts where a frame
    // can't be in progress — never mid-frame, which reads as garbage full of
    // errors:
    //   CAN: the last bus-idle gap before the window. Nine recessive bit
    //     times in a row happen only between frames (inside one, stuffing
    //     allows 5, and the ACK delimiter + EOF make 8).
    //   LIN: just before the last break field (a dominant stretch of at
    //     least 11 bit times, which only a break makes) that starts before
    //     the window — every frame begins with one.
    //   K-Line: inside the last idle gap of at least gapBits before the
    //     window (what separates messages; bytes within one are closer).
    qint64 trimmed = 0;
    if (job.visibleTo > job.visibleFrom && job.visibleFrom > 0 && job.protocol != QLatin1String("sent")) {
        const bool can = job.protocol == QLatin1String("can");
        const bool lin = job.protocol == QLatin1String("lin");
        const double bitRate = double(std::max<qint64>(1, can ? job.bitrate : baud));
        const double bw = job.sampleRate / bitRate;
        const qint64 limit = std::min<qint64>(job.visibleFrom, qint64(n) - 1);
        qint64 best = -1;
        if (lin) {
            const qint64 breakRun = qint64(std::ceil(11.0 * bw));
            const qint64 scanTo = std::min<qint64>(qint64(n) - 1, limit + qint64(std::ceil(20.0 * bw)));
            qint64 run = 0;
            for (qint64 i = 0; i <= scanTo; ++i) {
                run = logic[int(i)] ? 0 : run + 1;
                if (run == breakRun && i - run + 1 <= limit)
                    best = i - run + 1;                        // where this break began
            }
        } else {
            const double gapBits = can ? 9.0 : double(std::max<qint64>(9, job.gapBits));
            const qint64 idleRun = qint64(std::ceil(gapBits * bw));
            qint64 run = 0;
            for (qint64 i = 0; i <= limit; ++i) {
                run = logic[int(i)] ? run + 1 : 0;
                if (run >= idleRun)
                    best = i;
            }
        }
        if (best >= 0)
            trimmed = std::max<qint64>(0, best - qint64(std::ceil(2.0 * bw)));
        if (trimmed > 0) {
            logic.remove(0, int(trimmed));
            n = logic.size();
        }
    }

    // SENT is decoded here, natively (sent.h): pure edge timing, no sigrok
    // decoder for it. It finds its own frames from the sync pulses, so it
    // needs no aligned start; it reports what overlaps the window.
    if (job.protocol == QLatin1String("sent")) {
        const double tick = job.sampleRate * job.sentTickUs * 1e-6;          // samples per nominal tick
        const auto *bits = reinterpret_cast<const std::uint8_t *>(logic.constData());
        const std::vector<Sent::Frame> frames = Sent::decode(bits, logic.size(), tick, job.sentNibbles);
        const bool windowed = job.visibleTo > job.visibleFrom;
        const double from = windowed ? double(job.visibleFrom - trimmed) : -1e300;
        const double to = windowed ? double(job.visibleTo - trimmed) : 1e300;
        const auto ann = [&](long long a, long long b, const QStringList &texts, bool error, const char *group) {
            DecodeAnnotation x;
            x.start = double(a + trimmed);
            x.end = double(b + trimmed);
            x.texts = texts;
            x.error = error;
            x.group = QLatin1String(group);
            result.annotations.push_back(x);
        };
        // Slow channel: the serial messages carried one bit per frame.
        const std::vector<Sent::SerialMessage> messages =
            job.sentMode == 1 ? Sent::slowChannel(frames) : std::vector<Sent::SerialMessage>();
        for (const Sent::SerialMessage &m : messages) {
            if (double(m.end) < from || double(m.start) > to)
                continue;
            const QString crc = m.crcOk ? QStringLiteral("CRC OK")
                              : m.crcVerified ? QStringLiteral("CRC error") : QStringLiteral("CRC not verified");
            QStringList texts;
            if (m.enhanced) {
                texts << QStringLiteral("Enhanced message ID 0x%1: 0x%2 (%3), %4-bit \u00b7 %5")
                             .arg(QString::number(m.id, 16).toUpper()).arg(QString::number(m.data, 16).toUpper())
                             .arg(m.data).arg(m.dataBits).arg(crc)
                      << QStringLiteral("ID 0x%1: 0x%2").arg(QString::number(m.id, 16).toUpper())
                             .arg(QString::number(m.data, 16).toUpper())
                      << QStringLiteral("0x%1").arg(QString::number(m.data, 16).toUpper());
            } else {
                texts << QStringLiteral("Short message ID %1: 0x%2 (%3) \u00b7 %4").arg(m.id)
                             .arg(QString::number(m.data, 16).toUpper()).arg(m.data).arg(crc)
                      << QStringLiteral("ID %1: 0x%2").arg(m.id).arg(QString::number(m.data, 16).toUpper())
                      << QStringLiteral("0x%1").arg(QString::number(m.data, 16).toUpper());
            }
            ann(m.start, m.end, texts, m.error(), m.enhanced ? "enhanced" : "short");
        }
        for (const Sent::Frame &f : frames) {
            if (job.sentMode == 1)
                break;                                   // slow channel: the messages are the row
            const long long frameEnd = f.end + static_cast<long long>(f.pauseTicks * f.tick);
            if (double(frameEnd) < from || double(f.start) > to)
                continue;
            if (job.sentMode == 2 && f.triggerTicks > 0.0 && f.triggerStart >= 0 && f.triggerStart < f.sync.start)
                ann(f.triggerStart, f.sync.start, { QStringLiteral("Master trigger: %1 ticks low").arg(f.triggerTicks, 0, 'f', 1),
                                                    QStringLiteral("Trigger"), QStringLiteral("T") }, false, "trigger");
            const double tickUs = f.tick / job.sampleRate * 1e6;
            QString sync = QStringLiteral("Sync: tick %1 \u00b5s").arg(tickUs, 0, 'f', 3);
            if (job.sentMode == 2 && f.triggerTicks > 0.0 && f.triggerStart == f.sync.start)
                sync.prepend(QStringLiteral("Master trigger %1 ticks + ").arg(f.triggerTicks, 0, 'f', 1));
            if (f.driftError)
                sync += QStringLiteral(", changed %1 %").arg(f.tickDrift * 100.0, 0, 'f', 2);
            if (f.shortLow)
                sync += QStringLiteral(", low pulse under 4 ticks");
            ann(f.sync.start, f.sync.end, { sync, QStringLiteral("Sync %1 \u00b5s").arg(tickUs, 0, 'f', 2), QStringLiteral("Sync") },
                f.driftError || f.shortLow, "sync");
            if (f.nibbleError) {
                ann(f.status.start, f.end, { QStringLiteral("Nibble out of range (12\u201327 ticks)"),
                                             QStringLiteral("Nibble error"), QStringLiteral("Err") }, true, "error");
                continue;
            }
            ann(f.status.start, f.status.end,
                { QStringLiteral("Status %1 (%2)").arg(f.status.value).arg(f.status.value, 4, 2, QLatin1Char('0')),
                  QStringLiteral("Status %1").arg(f.status.value), QStringLiteral("S") }, false, "status");
            for (size_t i = 0; i < f.data.size(); ++i) {
                const Sent::Nibble &d = f.data[i];
                QString text = QStringLiteral("D%1: %2 (0x%3)").arg(int(i) + 1).arg(d.value)
                                   .arg(QString::number(d.value, 16).toUpper());
                if (i == 0 && f.fastChannel1() >= 0)
                    text += QStringLiteral(" \u00b7 FC1 = %1").arg(f.fastChannel1());
                if (i == 3 && f.fastChannel2() >= 0)
                    text += QStringLiteral(" \u00b7 FC2 = %1").arg(f.fastChannel2());
                ann(d.start, d.end, { text, QStringLiteral("D%1: %2").arg(int(i) + 1).arg(d.value), QString::number(d.value, 16).toUpper() },
                    false, "data");
            }
            if (f.crcOk)
                ann(f.crc.start, f.crc.end, { QStringLiteral("CRC %1 OK%2").arg(f.crc.value)
                                                  .arg(f.legacyCrc ? QStringLiteral(" (legacy)") : QString()),
                                              QStringLiteral("CRC OK"), QStringLiteral("C") }, false, "crc");
            else
                ann(f.crc.start, f.crc.end, { QStringLiteral("CRC error: %1, expected %2").arg(f.crc.value).arg(f.crcRecommended),
                                              QStringLiteral("CRC error"), QStringLiteral("C!") }, true, "crc");
            if (f.pauseTicks > 0.0) {
                // SPC: the end pulse that closes the CRC nibble, then idle.
                const bool spc = job.sentMode == 2;
                ann(f.end, frameEnd, { QStringLiteral("%1 %2 ticks").arg(spc ? QStringLiteral("End") : QStringLiteral("Pause"))
                                           .arg(f.pauseTicks, 0, 'f', 0),
                                       spc ? QStringLiteral("End") : QStringLiteral("Pause"),
                                       spc ? QStringLiteral("E") : QStringLiteral("P") }, false, "pause");
            }
        }
        for (Sent::Frame f : frames) {
            if (double(f.start) < from || double(f.start) >= to)
                continue;
            f.start += trimmed;
            f.end += trimmed;
            result.sentFrames.push_back(f);
        }
        for (Sent::SerialMessage m : messages) {
            if (double(m.start) < from || double(m.start) >= to)
                continue;
            m.start += trimmed;
            m.end += trimmed;
            result.sentMessages.push_back(m);
        }
        result.sampleCount = windowed ? quint64(job.visibleTo - job.visibleFrom) : quint64(n);
        result.sampleRate = job.sampleRate;
        emit finished(result);
        return;
    }

    struct srd_session *sess = nullptr;
    if (srd_session_new(&sess) != SRD_OK || !sess) {
        result.message = QStringLiteral("Could not start a decoder session");
        emit finished(result);
        return;
    }

    Collector collector;
    struct srd_decoder_inst *top = nullptr;
    QString missing;

    if (job.protocol == QLatin1String("can")) {
        GHashTable *o = newOptions();
        setInt(o, "nominal_bitrate", job.bitrate);
        setInt(o, "fast_bitrate", job.fastBitrate);
        setDouble(o, "sample_point", job.samplePoint);
        top = newInstance(sess, "can", o, "can_rx");
        missing = QStringLiteral("can");
    } else {
        GHashTable *u = newOptions();
        setInt(u, "baudrate", baud);
        struct srd_decoder_inst *uart = newInstance(sess, "uart", u, "rx");
        if (!uart) {
            missing = QStringLiteral("uart");
        } else if (job.protocol == QLatin1String("lin")) {
            GHashTable *o = newOptions();
            setInt(o, "version", job.linVersion);
            top = newInstance(sess, "lin", o, nullptr);
            missing = QStringLiteral("lin");
        } else {
            GHashTable *o = newOptions();
            setString(o, "framing", job.gapFraming ? "gap" : "kwp2000");
            setInt(o, "gap_bits", job.gapBits);
            top = newInstance(sess, "vsx_kline", o, nullptr);
            missing = QStringLiteral("vsx_kline (should be in the app's decoders folder)");
        }
        if (uart && top)
            srd_inst_stack(sess, uart, top);
    }

    if (!top) {
        srd_session_destroy(sess);
        result.message = QStringLiteral("Decoder not available: %1").arg(missing);
        emit finished(result);
        return;
    }
    collector.topId = QByteArray(top->decoder->id);

    srd_pd_output_callback_add(sess, SRD_OUTPUT_ANN, annotationCallback, &collector);
    srd_session_metadata_set(sess, SRD_CONF_SAMPLERATE,
                             g_variant_new_uint64(quint64(job.sampleRate)));
    int ret = srd_session_start(sess);
    if (ret == SRD_OK) {
        ret = srd_session_send(sess, 0, quint64(n),
                               reinterpret_cast<const uint8_t *>(logic.constData()),
                               quint64(n), 1);
    }
    srd_session_destroy(sess);

    if (ret != SRD_OK) {
        result.message = QStringLiteral("Decoding failed: %1")
                             .arg(QString::fromUtf8(srd_strerror(ret)));
    } else {
        std::lock_guard<std::mutex> lock(collector.mutex);
        if (job.protocol == QLatin1String("can")) {
            checkCanCrc(collector, logic, job.sampleRate / double(std::max<qint64>(1, job.bitrate)),
                        job.samplePoint);
            checkCanFd(collector);
            markCutOffFrame(collector, quint64(n));
        }
        result.annotations = collector.annotations;
        const bool windowed = job.visibleTo > job.visibleFrom;
        if (job.protocol == QLatin1String("can"))
            result.canFrames = assembleCanFrames(result.annotations, &result.strayErrors,
                                                 double(job.visibleFrom - trimmed),
                                                 windowed ? double(job.visibleTo - trimmed) : 0.0);
        // LIN / K-Line health: our own reading of the same stream (frames,
        // bit rate, checksums, timing), only what starts in the window.
        if (job.protocol == QLatin1String("lin") || job.protocol == QLatin1String("kline")) {
            const double bw = job.sampleRate / double(std::max<qint64>(1, baud));
            const auto *bits = reinterpret_cast<const std::uint8_t *>(logic.constData());
            const SerialBus::Stream stream = SerialBus::decodeUart(bits, logic.size(), bw);
            result.busBitTime = stream.bitTime;
            const double from = windowed ? double(job.visibleFrom - trimmed) : -1e300;
            const double to = windowed ? double(job.visibleTo - trimmed) : 1e300;
            const auto inWindow = [&](long long start) { return double(start) >= from && double(start) < to; };
            if (job.protocol == QLatin1String("lin")) {
                for (SerialBus::LinFrame f : SerialBus::analyseLin(bits, logic.size(), stream, bw, job.linVersion)) {
                    if (!inWindow(f.start))
                        continue;
                    f.start += trimmed;
                    f.end += trimmed;
                    result.linFrames.push_back(f);
                }
            } else {
                SerialBus::KLineReport report = SerialBus::analyseKLine(stream, bw, double(job.gapBits), !job.gapFraming);
                result.kline.inits = report.inits;
                result.kline.maxInterByte = report.maxInterByte;
                for (SerialBus::KLineMessage m : report.messages) {
                    if (!inWindow(m.start))
                        continue;
                    m.start += trimmed;
                    m.end += trimmed;
                    result.kline.messages.push_back(std::move(m));
                }
            }
        }
        // Back to the job's own sample numbers; only what's in the window.
        if (trimmed > 0) {
            for (DecodeAnnotation &a : result.annotations) {
                a.start += double(trimmed);
                a.end += double(trimmed);
            }
            for (CanTools::CanFrame &f : result.canFrames) {
                f.start += double(trimmed);
                f.end += double(trimmed);
            }
        }
        // OBD-II / UDS: the CAN frames (already in the window, positions
        // final) through ISO-TP and the diagnostic layer; the row shows what
        // the messages mean, one annotation per frame.
        if (job.diagnostics && job.protocol == QLatin1String("can")) {
            std::vector<Diag::Frame> frames;
            frames.reserve(result.canFrames.size());
            for (const CanTools::CanFrame &cf : result.canFrames) {
                Diag::Frame f;
                f.start = static_cast<long long>(cf.start);
                f.end = static_cast<long long>(cf.end);
                f.id = cf.id;
                f.extended = cf.extended;
                f.canError = cf.error;
                f.data = cf.data;
                frames.push_back(std::move(f));
            }
            const Diag::Result diag = Diag::analyse(frames, job.diagIds);
            result.annotations.clear();
            for (const Diag::Note &n : diag.notes) {
                const Diag::Frame &f = frames[n.frame];
                DecodeAnnotation a;
                a.start = double(f.start);
                a.end = double(f.end);
                a.texts << QString::fromStdString(n.text) << QString::fromStdString(n.brief)
                        << QString::fromStdString(n.tiny);
                a.error = n.error;
                a.group = QStringLiteral("diag");
                result.annotations.push_back(a);
            }
            result.diagMessages = diag.messages;
            if (result.annotations.isEmpty() && result.message.isEmpty())
                result.message = QStringLiteral("No diagnostic traffic in view (CAN frames: %1)")
                                     .arg(qulonglong(result.canFrames.size()));
        }
        if (windowed) {
            result.annotations.erase(std::remove_if(result.annotations.begin(), result.annotations.end(),
                [&](const DecodeAnnotation &a) {
                    return a.end < double(job.visibleFrom) || a.start > double(job.visibleTo);
                }), result.annotations.end());
            result.sampleCount = quint64(job.visibleTo - job.visibleFrom);
        } else {
            result.sampleCount = quint64(n);
        }
        result.sampleRate = job.sampleRate;
    }
    emit finished(result);
#endif
}
