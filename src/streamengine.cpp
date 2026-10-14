#include "streamengine.h"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX   // windows.h's min / max macros break std::min / std::max
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#include "perftrace.h"
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QDebug>

#include <algorithm>
#include <cmath>

namespace {

// A stream is ended by StreamStopCapture, not by reaching its length, so the
// requested length just has to be longer than anyone will run it. A day at
// 25 MS/s is about 2 TB of samples, which the KB count holds comfortably in 64
// bits and the device never actually delivers.
constexpr unsigned long long kEffectivelyEndless = 2ull * 1024 * 1024 * 1024;

// History memory: up to 35 % of the machine's RAM (512 MB .. 16 GB), shared
// by the channels actually streaming. A fixed 64 M samples per channel,
// allocated for all four slots, cut 30 s at 5 MS/s to 13.4 s (and spent half
// its memory on channels a two-channel scope doesn't have).
quint64 physicalMemoryBytes()
{
#if defined(_WIN32)
    MEMORYSTATUSEX status;
    status.dwLength = sizeof status;
    if (GlobalMemoryStatusEx(&status))
        return quint64(status.ullTotalPhys);
#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    const long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0)
        return quint64(pages) * quint64(size);
#endif
    return 4ull * 1024 * 1024 * 1024;   // unknown: assume 4 GB
}
quint64 historyBudgetBytes()
{
    const quint64 budget = quint64(double(physicalMemoryBytes()) * 0.35);
    return std::clamp<quint64>(budget, 512ull * 1024 * 1024, 16ull * 1024 * 1024 * 1024);
}

}   // namespace

StreamEngine::StreamEngine(QObject *parent)
    : QObject(parent)
{
}

StreamEngine::~StreamEngine()
{
    stop();
}

void StreamEngine::attach(unsigned int deviceId, int channels)
{
    VSX_TRACE("stream.attach");
    m_deviceId = deviceId;
    m_channels = std::clamp(channels, 0, MaxChannels);
}

void StreamEngine::detach()
{
    VSX_TRACE("stream.detach");
    stop();
    m_deviceId = 0xFFFFFFFFu;
    m_channels = 0;
}

// ---------------------------------------------------------------------------

QVector<unsigned int> StreamEngine::supportedRates(unsigned int deviceId)
{
    VSX_TRACE("stream.supportedRates");
    QVector<unsigned int> rates;
    const int count = GetStreamSupportSampleRateNum(deviceId);
    if (count <= 0)
        return rates;

    rates.resize(count);
    const int got = GetStreamSupportSampleRates(deviceId, rates.data(), count);
    rates.resize(got > 0 ? got : 0);
    std::sort(rates.begin(), rates.end());
    return rates;
}

int StreamEngine::rangeMinMv(unsigned int deviceId)
{
    VSX_TRACE("stream.rangeMinMv");
    return GetStreamRangeMinmV(deviceId);
}

int StreamEngine::rangeMaxMv(unsigned int deviceId)
{
    VSX_TRACE("stream.rangeMaxMv");
    return GetStreamRangeMaxmV(deviceId);
}

// ---------------------------------------------------------------------------

bool StreamEngine::startRecord(unsigned int sampleRate, unsigned short channelMask,
                               int updatesPerSecond, quint64 samplesPerChannel,
                               const QString &directory)
{
    VSX_TRACE("stream.startRecord");
    if (m_running)
        return true;

    // Two genuinely different counts, not one. The BYTE BUDGET must use how
    // many channels this record actually covers (its mask's popcount) —
    // mainwindow already divided the requested total by that same count to
    // get samplesPerChannel, and this has to agree or the space check and the
    // status bar disagree with what was actually asked for. But FILE SLOTS
    // and per-channel SCALE below must span the device's real channel range,
    // because the SDK tags every callback with the channel's own physical
    // index — CH2 is always index 1, whether or not CH1 is also recording —
    // so a CH2-only record with only 1 "active" slot silently dropped every
    // sample tagged as channel 1. Conflating these two into one variable is
    // exactly how that regression got introduced.
    const int recordedChannelCount = std::max(1, __builtin_popcount(unsigned(channelMask)));
    const quint64 bytes = samplesPerChannel * 2ull * quint64(recordedChannelCount);
    quint64 freeBytes = 0;
    if (!RecordWriter::enoughSpace(directory, bytes, &freeBytes)) {
        emit failed(QStringLiteral("Not enough space in %1: %2 GB free, %3 GB needed.")
                        .arg(directory)
                        .arg(freeBytes / 1073741824.0, 0, 'f', 1)
                        .arg(bytes / 1073741824.0, 0, 'f', 1));
        return false;
    }

    // Volts per code, so the file holds what the converter produced rather than
    // a re-expanded copy of it. Indexed by the ABSOLUTE physical channel, and
    // sized to the device's real channel count — not recordedChannelCount —
    // for exactly the reason above.
    double scale[RecordWriter::MaxChannels] = { 0, 0, 0, 0 };
    for (int ch = 0; ch < m_channels && ch < RecordWriter::MaxChannels; ++ch)
        scale[ch] = GetStreamVoltageResolution(m_deviceId, char(ch));

    QString error;
    if (!m_writer.open(directory, channelMask, sampleRate, samplesPerChannel, scale, &error)) {
        emit failed(error);
        return false;
    }

    // Progress has to be read from a channel that is actually part of this
    // record. Channel 0 (CH1) previously served as that reference unconditionally,
    // so a record with CH1 disabled never advanced past zero: its callback
    // never fires, its file never grows, and recorded() reported nothing no
    // matter how much CH2 had captured. The plot never appeared because
    // refreshRecordView() bails out whenever recorded() == 0.
    m_recordPrimaryChannel = 0;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (channelMask & (1u << ch)) {
            m_recordPrimaryChannel = ch;
            break;
        }
    }

    m_recording = true;
    m_recordTarget = samplesPerChannel;
    m_recordChannels = recordedChannelCount;   // for the MB/status-bar math only
    return start(sampleRate, channelMask, updatesPerSecond, 0.0);
}

bool StreamEngine::openRecordForReading(const QString &directory, QString *error)
{
    VSX_TRACE("stream.openRecordForReading");
    if (m_running)
        return false;   // cannot browse while actively streaming or recording

    if (!m_writer.openForReading(directory, error))
        return false;

    // Never read from any hardware/attach-derived state: browsing a past
    // recording must work with no device connected at all, so both of
    // these have to come from the recording's own metadata, not from
    // whatever m_stream otherwise knows.
    m_sampleRate = m_writer.sampleRate();

    // Same "lowest channel actually present" rule startRecord() uses just
    // above, so recorded() and the MB/status math read an old recording
    // correctly too, not only a fresh one.
    const unsigned short mask = m_writer.channelMask();
    m_recordPrimaryChannel = 0;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (mask & (1u << ch)) {
            m_recordPrimaryChannel = ch;
            break;
        }
    }
    m_recordChannels = std::max(1, __builtin_popcount(unsigned(mask)));
    m_recording = false;   // finished, not actively recording — same state stop() leaves one in
    // The record size originally chosen, for the status bar's "x / y MB" —
    // not what was written, which showed "0.4 / 0 MB" for a recording
    // stopped early. Browsing never checks recordComplete() (only a running
    // recording does), so this has no other effect.
    m_recordTarget = m_writer.requestedSamplesPerChannel();
    return true;
}

bool StreamEngine::start(unsigned int sampleRate, unsigned short channelMask,
                         int updatesPerSecond, double historySeconds)
{
    VSX_TRACE("stream.start");
    if (m_running)
        return true;
    if (m_deviceId == 0xFFFFFFFFu || m_channels <= 0) {
        emit failed(QStringLiteral("No device attached."));
        return false;
    }

    m_sampleRate = sampleRate;
    m_mask       = channelMask;
    m_updates    = std::clamp(updatesPerSecond, 1, 120);

    // A record never touches the rings: it goes straight to disk, so its size
    // is bounded by the filesystem rather than by memory.
    if (!m_recording) {
        int active = 0;
        for (int ch = 0; ch < MaxChannels; ++ch)
            active += (channelMask >> ch) & 1u ? 1 : 0;
        const double perChannelLimit = double(historyBudgetBytes()) / double(sizeof(float))
                                       / double(std::max(1, active));
        const int capacity = int(std::min({ std::max(1.0, historySeconds) * double(sampleRate),
                                            perChannelLimit, 1.5e9 }));
        for (int ch = 0; ch < MaxChannels; ++ch)
            m_rings[ch].reset((channelMask >> ch) & 1u ? capacity : 1, true);
    }

    m_blocks = 0;
    m_samples = 0;
    m_drops = 0;
    m_queue = 0;
    m_blockSize = 0;
    m_firstBlockMs = 0;
    m_lastBlockMs = 0;

    if (!SetOscCaptureMode(m_deviceId, 1)) {
        emit failed(QStringLiteral("The device refused to enter stream mode."));
        return false;
    }

    // Re-applied here, not just cached by setChannelRange() itself: every
    // caller (MainWindow::startAcquisition(), via applyChannel()) sets each
    // channel's range BEFORE calling start(), while the device is still in
    // block/Capture mode from any previous session — SetStreamChannelRangemV()
    // does not take effect until the device has actually switched into
    // stream mode via SetOscCaptureMode(1), immediately above. Without this,
    // the requested range is silently lost and the hardware's own
    // programmable gain amplifier is left wherever Capture mode (or the
    // device's own power-on default) had it — usually a wider, less
    // sensitive range than intended, which spreads the same signal over
    // more volts per ADC step and reads as noticeably more quantization
    // noise on screen, in both Live and Record, since both share this same
    // start(). This is the vendor SDK manual's own documented order for
    // entering stream mode (SetOscCaptureMode, then SetStreamChannelRangemV
    // per channel, then StreamCapture) — section 9.2's example follows it
    // exactly, and this call was the missing middle step.
    // Coupling before ranges: switching coupling resets the input range to
    // the device default (+-2.5 V here: 19.5 mV steps on a +-100 mV page —
    // Live/Record went "blocky" when coupling was sent after the ranges).
    reapplyCoupling();   // the mode switch above resets it, like the ranges
    for (int ch = 0; ch < m_channels && ch < MaxChannels; ++ch) {
        if (m_rangeSet[ch])
            applyStreamRange(ch);
    }

    SetStreamDataReadyCallBack(this, &StreamEngine::onStreamData);

    // A record asks for exactly its length so the library stops on its own; a
    // live stream asks for more than anyone will ever take and is stopped by
    // hand.
    const unsigned long long lengthKb = m_recording
        ? std::max<unsigned long long>(1, m_recordTarget / 1024ull + 1ull)
        : kEffectivelyEndless;

    const int rc = StreamCapture(m_deviceId, unsigned(m_updates), lengthKb,
                                 m_mask, m_sampleRate);

    // 0 means success here. The header says 1; the vendor's own demo ignores
    // the value entirely, which is how the discrepancy survived.
    if (rc != 0 && rc != 1) {
        m_recording = false;
        SetOscCaptureMode(m_deviceId, 0);
        reapplyBlockCoupling();
        emit failed(rc == -1
            ? QStringLiteral("%1 S/s is not available for streaming on %2 channels.")
                  .arg(sampleRate).arg(m_channels)
            : QStringLiteral("StreamCapture failed (%1).").arg(rc));
        return false;
    }

    m_running = true;
    emit status(QStringLiteral("Streaming at %1 S/s, %2 updates/s")
                    .arg(sampleRate).arg(m_updates));
    reportStreamRanges();
    emit started();
    return true;
}

// Sets a channel's stream range, checking the result: SetStreamChannelRangemV
// returns 0 when the scope refuses it, and a refused range left streaming on
// the device default (+-2.5 V: 19.5 mV steps on a +-100 mV page — "blocky").
// A refused range is doubled about the same centre until one is accepted.
// The data callback delivers volts, so a wider range only coarsens detail.
void StreamEngine::applyStreamRange(int channel)
{
    VSX_TRACE("stream.applyStreamRange");
    int lo = m_rangeMinMv[channel], hi = m_rangeMaxMv[channel];
    const int centre = (lo + hi) / 2;
    int half = std::max(1, (hi - lo) / 2);
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (SetStreamChannelRangemV(m_deviceId, channel, lo, hi) != 0)
            break;
        half *= 2;
        lo = centre - half;
        hi = centre + half;
    }
    m_usedMinMv[channel] = lo;
    m_usedMaxMv[channel] = hi;
}

// The scope's own word on each active channel once streaming runs: the
// range asked for and the one in use, its step size, its streaming limits,
// and the coupling as read back *while streaming* (read back before the
// stream starts it means nothing). Reported every start, so what the
// hardware actually did is always visible.
void StreamEngine::reportStreamRanges()
{
    VSX_TRACE("stream.reportStreamRanges");
    const int sdkMin = GetStreamRangeMinmV(m_deviceId);
    const int sdkMax = GetStreamRangeMaxmV(m_deviceId);
    for (int ch = 0; ch < m_channels && ch < MaxChannels; ++ch) {
        if (!(m_mask & (1u << ch)) || !m_rangeSet[ch])
            continue;
        const double step = GetStreamVoltageResolution(m_deviceId, char(ch));
        const int ac = GetStreamAcDc(m_deviceId, static_cast<unsigned int>(ch));
        emit rangeReport(ch, m_rangeMinMv[ch], m_rangeMaxMv[ch], m_usedMinMv[ch],
                         m_usedMaxMv[ch], step, sdkMin, sdkMax, m_acCoupling[ch], ac);
    }
}

void StreamEngine::stop()
{
    VSX_TRACE("stream.stop");
    if (!m_running)
        return;
    m_running = false;

    // Unregistered BEFORE the mode switch, mirroring AcquisitionManager's own
    // "detach callbacks before tearing the device down" pattern. This SDK has
    // repeatedly shown it does not tolerate abrupt state changes cleanly
    // (AbortCapture and an out-of-range mask bit have both crashed the library
    // outright elsewhere in this project); if its internal delivery thread is
    // still mid-callback when SetOscCaptureMode/StreamStopCapture run, our
    // callback could fire into a buffer the SDK is simultaneously tearing
    // down. Telling it to stop calling us first closes that window at our end.
    SetStreamDataReadyCallBack(nullptr, nullptr);

    StreamStopCapture(m_deviceId);
    SetOscCaptureMode(m_deviceId, 0);
    reapplyBlockCoupling();

    if (m_recording) {
        m_recording = false;
        m_writer.finishWriting();    // flushed and closed for writing, still readable
        emit recordFinished();
    }

    emit stopped();
}

// Stream mode's own coupling call. Sent after entering stream mode (the
// switch resets the channels, as it does their ranges); the block-mode
// coupling is AcquisitionManager's and is re-sent after returning to block
// mode.
void StreamEngine::reapplyCoupling()
{
    VSX_TRACE("stream.reapplyCoupling");
    if (m_deviceId == 0xFFFFFFFFu)
        return;
    for (int ch = 0; ch < m_channels && ch < MaxChannels; ++ch)
        SetStreamAcDc(m_deviceId, static_cast<unsigned int>(ch), m_acCoupling[ch] ? 1 : 0);
}

// Back in block mode (Capture): the switch reset the channels again, so
// the block-mode coupling goes back on too.
void StreamEngine::reapplyBlockCoupling()
{
    VSX_TRACE("stream.reapplyBlockCoupling");
    if (m_deviceId == 0xFFFFFFFFu)
        return;
    for (int ch = 0; ch < m_channels && ch < MaxChannels; ++ch)
        SetAcDc(m_deviceId, static_cast<unsigned int>(ch), m_acCoupling[ch] ? 1 : 0);
}

void StreamEngine::setZeroOffset(int channel, double volts)
{
    if (channel >= 0 && channel < MaxChannels)
        m_zero[size_t(channel)].store(volts, std::memory_order_relaxed);
}

bool StreamEngine::setChannelCoupling(int channel, bool ac)
{
    VSX_TRACE("stream.setChannelCoupling");
    if (channel < 0 || channel >= MaxChannels)
        return true;
    m_acCoupling[channel] = ac;
    if (m_deviceId == 0xFFFFFFFFu || channel >= m_channels)
        return true;
    SetStreamAcDc(m_deviceId, static_cast<unsigned int>(channel), ac ? 1 : 0);
    // The coupling switch resets the range: put it back.
    if (m_rangeSet[channel])
        applyStreamRange(channel);
    return (GetStreamAcDc(m_deviceId, static_cast<unsigned int>(channel)) != 0) == ac;
}

AdcWindow StreamEngine::adcWindow(int channel) const
{
    AdcWindow w;
    if (!m_running || m_deviceId == 0xFFFFFFFFu || channel < 0 || channel >= MaxChannels || channel >= m_channels)
        return w;
    w.bits = GetOscAdcBit(m_deviceId);
    w.zoom = ReadStreamADCToVoltageZoom(m_deviceId, static_cast<char>(channel));
    w.bias = ReadStreamADCToVoltageBias(m_deviceId, static_cast<char>(channel));
    w.sdkResolution = GetStreamVoltageResolution(m_deviceId, static_cast<char>(channel));
    w.requestedMinMv = m_rangeMinMv[channel];
    w.requestedMaxMv = m_rangeMaxMv[channel];
    w.sentMinMv = m_usedMinMv[channel];
    w.sentMaxMv = m_usedMaxMv[channel];
    w.complete();
    return w;
}

void StreamEngine::resetChannelFrontEnd(int channel)
{
    VSX_TRACE("stream.resetChannelFrontEnd");
    if (m_deviceId == 0xFFFFFFFFu || channel < 0 || channel >= MaxChannels || channel >= m_channels)
        return;
    const auto id = static_cast<unsigned int>(channel);
    SetStreamAcDc(m_deviceId, id, m_acCoupling[channel] ? 0 : 1);   // a real change...
    SetStreamAcDc(m_deviceId, id, m_acCoupling[channel] ? 1 : 0);   // ...and back
    if (m_rangeSet[channel])
        applyStreamRange(channel);      // the coupling switch resets the range
    if (m_running)
        reportStreamRanges();
}

void StreamEngine::setChannelRange(int channel, int minMv, int maxMv)
{
    VSX_TRACE("stream.setChannelRange");
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_rangeMinMv[channel] = minMv;
    m_rangeMaxMv[channel] = maxMv;
    m_rangeSet[channel]   = true;
    if (m_deviceId == 0xFFFFFFFFu || channel >= m_channels)
        return;
    applyStreamRange(channel);
    if (m_running)
        reportStreamRanges();
}

void StreamEngine::clearHistory()
{
    VSX_TRACE("stream.clearHistory");
    for (RingBuffer &ring : m_rings)
        ring.clear();
}

// ---------------------------------------------------------------------------
// SDK thread
// ---------------------------------------------------------------------------

void CALLBACK StreamEngine::onStreamData(void *context, unsigned int,
                                         unsigned char channel, double *buffer,
                                         unsigned int length, unsigned int failedFlag,
                                         unsigned int, unsigned long long,
                                         unsigned long long,
                                         unsigned long long memoryUse)
{
    auto *self = static_cast<StreamEngine *>(context);
    if (self)
        self->ingest(channel, buffer, length, failedFlag, memoryUse);
}

// Deliberately does no more than copy and count. The buffer is destroyed when
// this returns, and the manual is explicit that a slow callback trips the
// hardware watchdog and resets the USB link.
void StreamEngine::ingest(unsigned char channel, const double *buffer,
                          unsigned int length, unsigned int failedFlag,
                          unsigned long long memoryUse)
{
    // Unregistering the callback in stop() closes most of this window, but
    // the SDK's own delivery thread could already have read the old callback
    // pointer a moment before that unregister call lands there — a race we
    // cannot fully close from this side. Refusing to touch the buffer once we
    // are no longer running is the last line of defence against that.
    if (!m_running)
        return;

    if (failedFlag)
        m_drops.fetch_add(1);
    if (!buffer || length == 0)
        return;

    // Zero calibration, before Live's ring buffer and the recording.
    if (channel < MaxChannels) {
        const double zero = m_zero[channel].load(std::memory_order_relaxed);
        if (zero != 0.0) {
            thread_local std::vector<double> corrected;
            corrected.assign(buffer, buffer + length);
            for (double &v : corrected)
                v -= zero;
            buffer = corrected.data();
        }
    }

    if (m_recording) {
        // Straight to the writer's queue; the disk I/O happens on its thread.
        m_writer.append(channel, buffer, int(length));
    } else if (channel < MaxChannels && !m_frozen.load()) {
        m_rings[channel].append(buffer, int(length));
    }

    m_blocks.fetch_add(1);
    m_samples.fetch_add(length);
    m_blockSize.store(length);
    m_queue.store(memoryUse);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 expected = 0;
    m_firstBlockMs.compare_exchange_strong(expected, now);
    m_lastBlockMs.store(now);
}

// ---------------------------------------------------------------------------
// GUI thread
// ---------------------------------------------------------------------------

int StreamEngine::newest(int channel, int count, QVector<float> *out) const
{
    if (!out || channel < 0 || channel >= MaxChannels || count <= 0)
        return 0;

    out->resize(count);
    const int got = m_rings[channel].newest(count, out->data());
    out->resize(got);
    return got;
}

int StreamEngine::rangeFromNewest(int channel, quint64 offsetFromNewest, int count,
                                  QVector<float> *out) const
{
    VSX_TRACE("stream.rangeFromNewest");
    if (!out || channel < 0 || channel >= MaxChannels || count <= 0)
        return 0;

    out->resize(count);
    const int got = m_rings[channel].rangeFromNewest(offsetFromNewest, count, out->data());
    out->resize(got);
    return got;
}

quint64 StreamEngine::historyCapacity(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0;
    return quint64(std::max(0, m_rings[channel].capacity()));
}

quint64 StreamEngine::historyAvailable(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0;
    return m_rings[channel].available();
}

quint64 StreamEngine::recorded() const
{
    return m_writer.samplesWritten(m_recordPrimaryChannel);
}

bool StreamEngine::recordComplete() const
{
    return m_recordTarget > 0 && recorded() >= m_recordTarget;
}

// Reads out of the record file. Only the visible window is ever asked for, so
// the record's size is irrelevant here.
int StreamEngine::read(int channel, quint64 start, int count, QVector<float> *out) const
{
    if (!out || channel < 0 || channel >= MaxChannels || count <= 0)
        return 0;
    out->resize(count);
    const int got = m_writer.read(channel, start, count, out->data());
    out->resize(got);
    return got;
}

int StreamEngine::readDecimated(int channel, quint64 start, quint64 length,
                                int points, QVector<float> *out) const
{
    VSX_TRACE("stream.readDecimated");
    if (!out || channel < 0 || channel >= MaxChannels || points <= 0)
        return 0;
    out->resize(points);
    const int got = m_writer.readDecimated(channel, start, length, points, out->data());
    out->resize(got);
    return got;
}

quint64 StreamEngine::written(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0;
    return m_rings[channel].written();
}

StreamEngine::Stats StreamEngine::stats() const
{
    Stats s;
    s.blocks       = m_blocks.load();
    s.samples      = m_samples.load();
    s.drops        = m_drops.load();
    s.queueSamples = m_queue.load();
    s.blockSize    = m_blockSize.load();

    // Same fix as startRecord(): the callback count divided by how many
    // channels are actually producing callbacks, not by the device's total
    // channel count. Dividing by the device total under-reported the
    // per-channel block rate whenever fewer than all channels were streaming.
    const int active = std::max(1, __builtin_popcount(unsigned(m_mask)));
    const qint64 first = m_firstBlockMs.load();
    const qint64 last  = m_lastBlockMs.load();
    if (first > 0 && last > first) {
        const double seconds = double(last - first) / 1000.0;
        s.blocksPerSecond = double(s.blocks) / active / seconds;
    }

    // The library's backlog, expressed as the delay it puts between the signal
    // and the screen. Measured at a steady four blocks at every rate tried.
    if (s.blockSize > 0 && s.blocksPerSecond > 0.0) {
        s.latencyMs = double(s.queueSamples) / double(s.blockSize)
                    / s.blocksPerSecond * 1000.0;
    }
    return s;
}
