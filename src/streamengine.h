#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include <VmMultMsoLib.h>

#include "recordwriter.h"
#include "ringbuffer.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <array>
#include "adcwindow.h"
#include <atomic>

/*
 * Continuous acquisition, as a sibling of AcquisitionManager rather than a
 * replacement for it.
 *
 * The two cannot run at once: SetOscCaptureMode() puts the whole device into
 * one mode or the other, and both share a single InitDll() session. So
 * StreamEngine deliberately does NOT call InitDll or own the device callbacks —
 * it attaches to the session AcquisitionManager already established.
 *
 * Measured behaviour this is built around (MSO21AL PLUS, SDK 1.24):
 *
 *   - StreamCapture() returns 0 on success, -1 for a rejected sample rate.
 *     The header documents 1 for success. It is wrong.
 *   - The library holds exactly four blocks, whatever the rate, so
 *     count_every_1s sets display latency: 30/s gives ~130 ms, 60/s ~65 ms.
 *   - Block size is (rate / count_every_1s) rounded up to a whole KB.
 *   - One callback per channel per block.
 *   - 25 MS/s on two channels sustained 105 MB/s with zero drops; 62.5 MS/s
 *     and above are rejected outright on two channels.
 *   - StreamCapture is finite. Continuous means requesting an enormous length
 *     and ending it with StreamStopCapture().
 */
class StreamEngine : public QObject {
    Q_OBJECT

public:
    static constexpr int MaxChannels = 4;

    struct Stats {
        quint64 blocks      = 0;
        quint64 samples     = 0;
        unsigned drops      = 0;
        quint64 queueSamples = 0;   // the library's own backlog
        unsigned blockSize  = 0;
        double  blocksPerSecond = 0.0;
        double  latencyMs   = 0.0;
    };

    explicit StreamEngine(QObject *parent = nullptr);
    ~StreamEngine() override;

    // The device AcquisitionManager already opened. No InitDll here.
    void attach(unsigned int deviceId, int channels);
    void detach();

    // Live: endless length, bounded ring, display follows now.
    bool start(unsigned int sampleRate, unsigned short channelMask,
               int updatesPerSecond, double historySeconds);

    // Record: a finite length that fills once and is then navigated offline.
    // This is what the vendor's app calls a stream: pick a rate and a size, run
    // until full, then browse and zoom the whole thing.
    // Records go to disk, not memory: a 1 GB record is 1 GB. `directory` gets
    // one raw int16 file per channel.
    bool startRecord(unsigned int sampleRate, unsigned short channelMask,
                     int updatesPerSecond, quint64 samplesPerChannel,
                     const QString &directory);
    // Loads a FINISHED recording (this session's or an earlier one's) for
    // browsing — no acquisition involved, just opens its files read-only.
    // Fails while actively streaming or recording; stop() first. recorded()
    // and the MB/status accessors read this recording correctly afterward,
    // the same as they do for one just finished in this session.
    bool openRecordForReading(const QString &directory, QString *error);

    void stop();

    bool running() const { return m_running; }
    unsigned int sampleRate() const { return m_sampleRate; }
    int channels() const { return m_channels; }

    // Display-only pause: acquisition continues, the view stops advancing.
    void setFrozen(bool frozen) { m_frozen = frozen; }
    bool frozen() const { return m_frozen; }

    void clearHistory();

    bool recording() const { return m_recording; }
    quint64 recordTarget() const { return m_recordTarget; }
    quint64 recorded() const;              // samples per channel so far
    bool recordComplete() const;

    // Absolute range from a record, oldest sample is index 0.
    int read(int channel, quint64 start, int count, QVector<float> *out) const;
    int readDecimated(int channel, quint64 start, quint64 length, int points,
                      QVector<float> *out) const;
    int recordedChannels() const { return m_recordChannels; }

    quint64 recordOverflows() const { return m_writer.overflows(); }
    double  recordMegabytes() const { return m_writer.megabytesWritten(); }
    QString recordDirectory() const { return m_writer.directory(); }
    // The channel mask of whichever recording is currently loaded — the one
    // just made, or one openRecordForReading() loaded — not the live
    // channel rail's own on/off state, which has no necessary relationship
    // to what an older recording actually contains.
    unsigned short recordChannelMask() const { return m_writer.channelMask(); }

    // SetOscChannelRangemV has no effect in stream mode; this is its equivalent.
    void setChannelRange(int channel, int minMv, int maxMv);
    // The window the ADC covers now, from the SDK's stream conversion
    // (adcwindow.h). Invalid while not streaming.
    AdcWindow adcWindow(int channel) const;
    // Makes the scope reconfigure a channel's input: the coupling switched to
    // the other setting and back, then the range sent again. A channel can
    // stop following its range while streaming (user: CH1 in Live stayed on
    // +-2 V whatever the page; re-sending the same coupling or the range
    // didn't help, toggling the coupling did).
    void resetChannelFrontEnd(int channel);
    // AC/DC coupling for stream mode (Live/Record). Stream mode has its own
    // SDK call, SetStreamAcDc — the block-mode SetAcDc doesn't reach it,
    // which left Live/Record DC-coupled with AC selected. Cached and re-sent
    // when streaming starts; returns whether GetStreamAcDc reads back as asked.
    bool setChannelCoupling(int channel, bool ac);
    // Zero calibration: subtracted from every streamed sample (input volts)
    // before the ring buffer and the recording see it. Read on the SDK's
    // thread, hence atomic.
    void setZeroOffset(int channel, double volts);

    // Newest `count` samples for a channel, oldest first.
    int newest(int channel, int count, QVector<float> *out) const;
    quint64 written(int channel) const;

    // Like newest(), but the window ends `offsetFromNewest` samples before
    // the newest one instead of at it — lets a frozen Live display pan back
    // through whatever the ring still holds. Meaningless for a channel that
    // is being recorded: a Record bypasses the ring entirely and is read via
    // read()/readDecimated() instead.
    int rangeFromNewest(int channel, quint64 offsetFromNewest, int count,
                        QVector<float> *out) const;
    // How many samples the ring actually holds right now (bounded by its
    // capacity, unlike written()'s raw lifetime count) — the largest
    // offsetFromNewest that still returns anything.
    quint64 historyAvailable(int channel) const;
    // Samples the history holds when full (the ring's size).
    quint64 historyCapacity(int channel) const;

    Stats stats() const;

    static QVector<unsigned int> supportedRates(unsigned int deviceId);
    static int rangeMinMv(unsigned int deviceId);
    static int rangeMaxMv(unsigned int deviceId);

signals:
    void status(const QString &text);
    void started();
    void stopped();
    void failed(const QString &reason);
    void recordFinished();
    // After streaming starts, per active channel whose streaming detail is
    // coarser than its range implies, or whose range the scope refused:
    // the range asked for, the one in use, and the scope's own step size.
    void rangeReport(int channel, int askedMinMv, int askedMaxMv, int usedMinMv,
                     int usedMaxMv, double stepMv, int sdkMinMv, int sdkMaxMv,
                     bool acAsked, int acReadBack);

private:
    static void CALLBACK onStreamData(void *context, unsigned int deviceId,
                                      unsigned char channel, double *buffer,
                                      unsigned int length, unsigned int failed,
                                      unsigned int success,
                                      unsigned long long needTotal,
                                      unsigned long long total,
                                      unsigned long long memoryUse);

    void ingest(unsigned char channel, const double *buffer, unsigned int length,
                unsigned int failedFlag, unsigned long long memoryUse);

    std::array<RingBuffer, MaxChannels> m_rings;   // live display only
    mutable RecordWriter m_writer;                 // records

    // The last range requested per channel via setChannelRange(), re-applied
    // in start() right after SetOscCaptureMode(1) — see the comment there
    // for why a call made before that point does not actually take effect.
    std::array<int, MaxChannels>  m_rangeMinMv{};
    std::array<int, MaxChannels>  m_rangeMaxMv{};
    std::array<bool, MaxChannels> m_rangeSet{};

    std::array<bool, MaxChannels> m_acCoupling{};
    std::array<std::atomic<double>, MaxChannels> m_zero{};
    std::array<int, MaxChannels>  m_usedMinMv{};   // what the scope accepted
    std::array<int, MaxChannels>  m_usedMaxMv{};
    void applyStreamRange(int channel);
    void reportStreamRanges();
    void reapplyCoupling();        // stream mode (SetStreamAcDc)
    void reapplyBlockCoupling();   // block mode (SetAcDc), after leaving stream mode    // false: never set, leave the device's own default alone

    unsigned int m_deviceId   = 0xFFFFFFFFu;
    int          m_channels   = 0;
    unsigned int m_sampleRate = 0;
    unsigned short m_mask     = 0;
    int          m_updates    = 30;

    bool m_running   = false;
    bool m_recording = false;
    quint64 m_recordTarget = 0;
    int     m_recordChannels = 1;
    int     m_recordPrimaryChannel = 0;   // lowest channel index actually being
                                           // recorded; progress is read from it,
                                           // never from a hardcoded channel 0
    std::atomic<bool>     m_frozen{false};
    std::atomic<quint64>  m_blocks{0};
    std::atomic<quint64>  m_samples{0};
    std::atomic<unsigned> m_drops{0};
    std::atomic<quint64>  m_queue{0};
    std::atomic<unsigned> m_blockSize{0};
    std::atomic<qint64>   m_firstBlockMs{0};
    std::atomic<qint64>   m_lastBlockMs{0};
};
