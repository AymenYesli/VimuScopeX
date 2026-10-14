#pragma once

#include <QFile>
#include <QMutex>
#include <QQueue>
#include <QString>
#include <QThread>
#include <QVector>
#include <QWaitCondition>

#include <array>
#include <atomic>

/*
 * A record on disk: ONE .vsxrec file for the whole recording, regardless of
 * how many channels are in it — a fixed-size region per ENABLED channel
 * (bit ch = channel ch+1 of the mask; a channel left out gets no region at
 * all, since there is nothing to write and nothing to browse later),
 * concatenated in ascending channel order, raw little-endian int16. Each
 * recording lives inside its own uniquely-named subdirectory of the record
 * directory so multiple recordings can coexist and none of them overwrites
 * another. A small meta.ini alongside the data file carries what reading it
 * back correctly needs — sample rate, which channels are present, each
 * one's own volts-per-code scale, the per-channel region size reserved at
 * record time, and (once a recording finishes) how much of that region is
 * actually real data versus pre-allocated space never written to — since
 * none of that can be recovered from the raw codes alone, and a recording
 * opened for browsing in a later session has no RecordWriter instance left
 * over from when it was made to remember it.
 *
 * Sizing matches the vendor's: two bytes per sample, so a "1 GB record" is a
 * 1 GB record. The SDK hands us doubles in volts; those came from a 12-bit
 * converter, so scaling back to codes with the device's own voltage resolution
 * is lossless in any way that matters and halves both the file and the I/O.
 *
 * The SDK's callback must never block, so it only converts and enqueues. A
 * writer thread does the file I/O. If the disk cannot keep up the queue is
 * bounded and the overflow is counted rather than silently absorbed — a record
 * with a hole in it that nobody mentions is worse than one that reports it.
 *
 * One file, several channels' worth of data, means each channel writes to
 * its own byte region within that one file rather than its own file — the
 * writer thread seeks to the right region before every block it writes
 * (channelByteOffset() + however much of that channel is already written),
 * since blocks for different channels arrive interleaved in time on the
 * same queue. Reading mirrors this: each channel keeps its own QFile handle
 * (all pointing at the same path) so its own seek position never disturbs
 * another channel's, and every read seeks to channelByteOffset() + the
 * requested sample position first.
 */
class RecordWriter {
public:
    static constexpr int MaxChannels = 4;
    // Distinct from a plain .raw so a directory of recordings is
    // unambiguously this app's own, and so nothing else sharing the record
    // directory could plausibly collide with these names.
    static constexpr const char *kFileExtension = "vsxrec";

    // One past recording, as listRecordings() reports it — enough to fill a
    // tab (label) and to open it (directory), without opening it first.
    struct RecordInfo {
        QString directory;         // full path, unique per recording
        QString label;             // human-readable, e.g. "17:45:12"
        unsigned int sampleRate = 0;
        unsigned short channelMask = 0;
    };

    RecordWriter();
    ~RecordWriter();

    // voltsPerCode scales volts back to ADC codes; pass 0 for a channel to
    // have it derived from the channel range instead. channelMask decides
    // which channels get a region in the file at all (bit ch = channel
    // ch+1) — a channel left out is not merely empty, it occupies no space
    // and exists nowhere on disk, matching the rule that a disabled
    // channel's data should never be written in the first place.
    // samplesPerChannel is the CAPACITY reserved for each channel's own
    // region (the recording's configured target length) — the file is
    // pre-sized to hold every active channel's full region up front, so
    // any channel's own offset is always valid to seek into regardless of
    // which one's data happens to arrive first. Writes meta.ini into
    // `directory` describing this recording (sampleRate, channelMask,
    // samplesPerChannel, per-channel scale, and a label generated from the
    // current time) so a later session can browse it correctly with no
    // RecordWriter instance carried over from now.
    bool open(const QString &directory, unsigned short channelMask,
              unsigned int sampleRate, quint64 samplesPerChannel,
              const double *voltsPerCode, QString *error);
    // Opens a FINISHED recording — one open() (from this session or an
    // earlier one) already wrote — for reading only. Reads meta.ini for the
    // mask, per-channel scale, and each channel's own actually-written
    // length (not the reserved capacity, which is usually larger than what
    // a recording stopped early actually filled), and treats that written
    // length as fully flushed, since there is no writer thread to confirm
    // anything further.
    bool openForReading(const QString &directory, QString *error);
    // Stops the writer thread and flushes, but keeps the file readable. A
    // finished record is the thing you navigate, so closing it outright was
    // why nothing appeared after recording. Also records each channel's
    // actual written length into meta.ini — the file itself is pre-sized to
    // the full reserved capacity regardless of whether the recording ran
    // that long, so a later session needs this written elsewhere to know
    // where each channel's real data actually ends.
    void finishWriting();
    // The record size originally asked for, in samples per channel — kept
    // separately because finishWriting() shrinks the file (and its region
    // size) to what was actually recorded. Old recordings without it report
    // their region size, which for them is still the original request.
    quint64 requestedSamplesPerChannel() const { return m_requestedSamplesPerChannel; }
    void close();
    bool isOpen() const { return m_open; }
    // True once at least one channel has a readable file open — not just
    // channel 0, since a recording's mask can leave channel 0 out entirely.
    bool isReadable() const;
    unsigned short channelMask() const { return m_channelMask; }
    // The loaded recording's own sample rate — from meta.ini when opened
    // for reading, or the rate passed to open() while writing. Never the
    // device's current rate: a recording browsed later must read back at
    // whatever rate it actually was captured at, not at whatever the
    // hardware (if even attached at all) happens to be doing now.
    unsigned int sampleRate() const { return m_sampleRate; }

    // Called from the SDK thread. Converts and hands off; never touches disk.
    void append(int channel, const double *samples, int count);

    // Called from the GUI thread, against the file on disk.
    int read(int channel, quint64 start, int count, float *out);

    // Evenly spaced samples across a range, for showing a whole record without
    // reading all of it.
    int readDecimated(int channel, quint64 start, quint64 length, int points,
                      float *out);

    quint64 samplesWritten(int channel) const;
    quint64 queuedBlocks() const { return m_queued.load(); }
    quint64 overflows() const { return m_overflows.load(); }
    double  megabytesWritten() const;
    QString directory() const { return m_directory; }

    static bool enoughSpace(const QString &directory, quint64 bytesNeeded,
                            quint64 *freeBytes);
    // A fresh, never-yet-used subdirectory of `baseDirectory` for a new
    // recording to live in, named from the current date and time
    // (yyyyMMdd_HHmmss) so recordings sort chronologically by name alone —
    // with a numeric suffix appended on the rare collision (two recordings
    // started within the same second).
    static QString newRecordingDirectory(const QString &baseDirectory);
    // Every past recording directly under `baseDirectory`, oldest first,
    // each with its meta.ini already read — for filling a tab bar without
    // opening every recording just to list it.
    static QVector<RecordInfo> listRecordings(const QString &baseDirectory);
    // One recording's info, wherever it lives (for recordings opened from
    // outside the record directory). False if it isn't a readable recording.
    static bool recordingInfo(const QString &directory, RecordInfo *info);
    // Deletes one recording's entire directory (its data file and
    // meta.ini) — for a "Temporary" recording being cleaned up on exit.
    // A no-op if the directory does not exist.
    static void deleteRecording(const QString &directory);

private:
    struct Block {
        int channel = 0;
        QVector<qint16> codes;
    };

    void writerLoop();
    static bool readInfo(const QString &directory, RecordInfo *info,
                         std::array<double, MaxChannels> *scale,
                         quint64 *samplesPerChannel,
                         std::array<quint64, MaxChannels> *written);
    // Byte offset of `channel`'s own region within the single data file —
    // the sum of every lower-numbered active channel's reserved region
    // size ahead of it. The same computation whether writing or reading,
    // so the two can never disagree about where a channel's data actually
    // lives.
    qint64 channelByteOffset(int channel) const;

    QString m_directory;
    int     m_channels = 0;
    unsigned short m_channelMask = 0;
    unsigned int m_sampleRate = 0;
    quint64 m_samplesPerChannel = 0;   // reserved capacity per channel, in samples
    quint64 m_requestedSamplesPerChannel = 0;
    void compactToWritten();
    bool    m_open = false;

    QFile  *m_write = nullptr;   // one handle for the whole file, all channels
    std::array<QFile *, MaxChannels> m_reader{};   // one per channel, same underlying path
    std::array<double, MaxChannels>  m_scale{};      // volts per code
    std::array<std::atomic<quint64>, MaxChannels> m_written{};
    // Set by the writer thread once it has flushed; only data up to this point
    // is safe for the reader thread to see. Never touch m_write outside the
    // writer thread — QFile is not safe for concurrent use from two threads,
    // and flushing one thread's handle while the other writes to it is what
    // produced the double-free crash.
    std::array<std::atomic<quint64>, MaxChannels> m_flushed{};

    QQueue<Block>   m_queue;
    mutable QMutex  m_mutex;
    QWaitCondition  m_wake;
    QThread        *m_thread = nullptr;
    std::atomic<bool>    m_stopping{false};
    std::atomic<quint64> m_queued{0};
    std::atomic<quint64> m_overflows{0};
    quint64 m_flushCounter = 0;   // writer thread only
};
