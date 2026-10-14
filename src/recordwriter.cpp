#include "recordwriter.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStorageInfo>

#include <algorithm>
#include <cmath>

namespace {

// About a second of backlog at 25 MS/s with 30 blocks a second. Past this the
// disk is not keeping up and the record is going to have holes, so say so
// rather than growing the queue until memory runs out.
constexpr int kMaxQueuedBlocks = 64;

class WriterThread : public QThread {
public:
    explicit WriterThread(std::function<void()> body) : m_body(std::move(body)) {}
    void run() override { m_body(); }
private:
    std::function<void()> m_body;
};

QString metaPath(const QDir &dir)
{
    return dir.filePath(QStringLiteral("meta.ini"));
}

// One name for the whole recording's data now, not one per channel —
// "recording", not "channelN": the file holds every channel's own region
// concatenated together, so a per-channel name would no longer describe it.
QString dataFilePath(const QDir &dir)
{
    return dir.filePath(QStringLiteral("recording.%1")
                            .arg(QLatin1String(RecordWriter::kFileExtension)));
}

// For reading: recording.vsxrec, or else the folder's one .vsxrec — a
// recording saved under a name of the user's choosing (MyRun/MyRun.vsxrec).
QString dataFilePathForReading(const QDir &dir)
{
    const QString standard = dataFilePath(dir);
    if (QFileInfo::exists(standard))
        return standard;
    const QStringList found = dir.entryList({ QStringLiteral("*.%1").arg(QLatin1String(RecordWriter::kFileExtension)) },
                                            QDir::Files);
    return found.size() == 1 ? dir.filePath(found.first()) : standard;
}

}   // namespace

RecordWriter::RecordWriter()
{
    for (auto &w : m_written)
        w.store(0);
    for (auto &f : m_flushed)
        f.store(0);
    m_scale.fill(1.0);
    m_reader.fill(nullptr);
}

RecordWriter::~RecordWriter()
{
    close();
}

bool RecordWriter::enoughSpace(const QString &directory, quint64 bytesNeeded,
                               quint64 *freeBytes)
{
    // QStorageInfo reports nothing for a path that does not exist yet, and
    // bytesAvailable() then returns -1 — which read as "0 bytes free" and made
    // every record fail before it started. Create the directory first, and fall
    // back to the nearest existing ancestor if that is not possible.
    QDir().mkpath(directory);

    QString probe = directory;
    QStorageInfo storage(probe);
    while ((!storage.isValid() || storage.bytesAvailable() < 0)
           && probe.contains(QLatin1Char('/'))) {
        probe = probe.left(probe.lastIndexOf(QLatin1Char('/')));
        if (probe.isEmpty())
            probe = QStringLiteral("/");
        storage = QStorageInfo(probe);
    }

    const quint64 available = (storage.isValid() && storage.bytesAvailable() > 0)
        ? quint64(storage.bytesAvailable()) : 0;
    if (freeBytes)
        *freeBytes = available;
    // A margin, so filling the disk exactly is not the outcome.
    return available > bytesNeeded + 256ull * 1024 * 1024;
}

QString RecordWriter::newRecordingDirectory(const QString &baseDirectory)
{
    QDir base(baseDirectory);
    base.mkpath(QStringLiteral("."));
    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString name = stamp;
    // A numeric suffix on the rare collision — two recordings started within
    // the same second — rather than ever reusing an existing name, which is
    // the one thing this whole feature exists to stop happening.
    for (int suffix = 2; base.exists(name); ++suffix)
        name = stamp + QStringLiteral("_%1").arg(suffix);
    const QString path = base.filePath(name);
    QDir().mkpath(path);
    return path;
}

bool RecordWriter::recordingInfo(const QString &directory, RecordInfo *info)
{
    std::array<double, MaxChannels> scale{};
    quint64 samplesPerChannel = 0;
    std::array<quint64, MaxChannels> written{};
    return readInfo(directory, info, &scale, &samplesPerChannel, &written);
}

bool RecordWriter::readInfo(const QString &directory, RecordInfo *info,
                            std::array<double, MaxChannels> *scale,
                            quint64 *samplesPerChannel,
                            std::array<quint64, MaxChannels> *written)
{
    const QDir dir(directory);
    if (!QFile::exists(metaPath(dir)))
        return false;

    QSettings meta(metaPath(dir), QSettings::IniFormat);
    const unsigned short mask =
        meta.value(QStringLiteral("record/channelMask"), 0).toUInt();
    if (mask == 0)
        return false;

    if (info) {
        info->directory = directory;
        info->label = meta.value(QStringLiteral("record/label")).toString();
        if (info->label.isEmpty())
            info->label = QFileInfo(directory).fileName();   // fallback: the folder's own name
        info->sampleRate = meta.value(QStringLiteral("record/sampleRate"), 0).toUInt();
        info->channelMask = mask;
    }
    if (scale) {
        for (int ch = 0; ch < MaxChannels; ++ch) {
            (*scale)[ch] = meta.value(QStringLiteral("scale/channel%1").arg(ch + 1), 0.0)
                              .toDouble();
        }
    }
    if (samplesPerChannel) {
        *samplesPerChannel =
            meta.value(QStringLiteral("record/samplesPerChannel"), 0).toULongLong();
    }
    if (written) {
        for (int ch = 0; ch < MaxChannels; ++ch) {
            (*written)[ch] =
                meta.value(QStringLiteral("written/channel%1").arg(ch + 1), 0).toULongLong();
        }
    }
    return true;
}

QVector<RecordWriter::RecordInfo> RecordWriter::listRecordings(const QString &baseDirectory)
{
    QVector<RecordInfo> out;
    QDir base(baseDirectory);
    // Alphabetical sort on a yyyyMMdd_HHmmss name is also chronological —
    // oldest first, without needing to parse the name back into a QDateTime.
    const QStringList names = base.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString &name : names) {
        RecordInfo info;
        if (readInfo(base.filePath(name), &info, nullptr, nullptr, nullptr))
            out.append(info);
    }
    return out;
}

void RecordWriter::deleteRecording(const QString &directory)
{
    QDir(directory).removeRecursively();
}

qint64 RecordWriter::channelByteOffset(int channel) const
{
    qint64 offset = 0;
    for (int ch = 0; ch < channel; ++ch) {
        if (m_channelMask & (1u << ch))
            offset += qint64(m_samplesPerChannel) * qint64(sizeof(qint16));
    }
    return offset;
}

bool RecordWriter::open(const QString &directory, unsigned short channelMask,
                        unsigned int sampleRate, quint64 samplesPerChannel,
                        const double *voltsPerCode, QString *error)
{
    close();

    QDir dir(directory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QStringLiteral("Cannot create %1").arg(directory);
        return false;
    }

    const int activeChannels = std::max(1, __builtin_popcount(unsigned(channelMask)));

    m_directory = directory;
    m_channels = MaxChannels;
    m_channelMask = channelMask;
    m_sampleRate = sampleRate;
    m_samplesPerChannel = samplesPerChannel;
    m_requestedSamplesPerChannel = samplesPerChannel;

    const QString path = dataFilePath(dir);
    auto *out = new QFile(path);
    if (!out->open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Cannot write %1").arg(path);
        delete out;
        close();
        return false;
    }
    // Pre-sized to every active channel's full reserved region up front, so
    // any channel's own offset is always valid to seek into from the very
    // first write, regardless of which channel's data happens to arrive
    // first — blocks for different channels arrive interleaved in time on
    // the same queue, not one channel fully then the next.
    const qint64 totalBytes =
        qint64(activeChannels) * qint64(samplesPerChannel) * qint64(sizeof(qint16));
    // Checked: a refused reservation (FAT32's 4 GB file limit, a full or
    // read-only drive) otherwise failed silently part-way through recording.
    if (!out->resize(totalBytes)) {
        if (error)
            *error = QStringLiteral("Can't reserve %1 GB for the recording (%2). Drives "
                                    "formatted FAT32, as many USB sticks are, can't hold "
                                    "files over 4 GB: pick a smaller size or another folder.")
                         .arg(double(totalBytes) / 1073741824.0, 0, 'f', 1).arg(out->errorString());
        out->close();
        out->remove();
        delete out;
        close();
        return false;
    }
    m_write = out;

    bool any = false;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (!(channelMask & (1u << ch)))
            continue;   // disabled channel: no region, nothing ever written for it

        // Unbuffered: QFile's normal internal buffering caches whatever region
        // it last read, and seeking back into that region later returns the
        // stale cached bytes rather than re-reading from disk — a real problem
        // here specifically, since the file keeps growing underneath this
        // handle via a completely separate QFile on the writer thread. This is
        // what showed up as a distortion that appeared and disappeared as the
        // same file region was revisited while dragging.
        auto *in = new QFile(path);
        if (!in->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            delete in;
            m_reader[ch] = nullptr;
        } else {
            m_reader[ch] = in;
        }

        const double scale = voltsPerCode ? voltsPerCode[ch] : 0.0;
        m_scale[ch] = (scale > 0.0) ? scale : 1.0 / 2048.0;   // 12-bit fallback
        m_written[ch].store(0);
        m_flushed[ch].store(0);
        any = true;
    }

    if (!any) {
        if (error)
            *error = QStringLiteral("No channels selected to record");
        close();
        return false;
    }

    // meta.ini: everything a later session needs to read this recording back
    // correctly, with no RecordWriter instance carried over from now — the
    // scale especially, since it cannot be recovered from the raw codes
    // alone, samplesPerChannel since it is what makes each channel's own
    // region within the one file computable at all, and the mask, since a
    // channel simply has no region to imply it.
    {
        QSettings meta(metaPath(dir), QSettings::IniFormat);
        meta.setValue(QStringLiteral("record/channelMask"), channelMask);
        meta.setValue(QStringLiteral("record/sampleRate"), sampleRate);
        meta.setValue(QStringLiteral("record/samplesPerChannel"), qulonglong(samplesPerChannel));
        meta.setValue(QStringLiteral("record/requestedSamplesPerChannel"), qulonglong(samplesPerChannel));
        meta.setValue(QStringLiteral("record/label"),
                      QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")));
        for (int ch = 0; ch < MaxChannels; ++ch) {
            if (channelMask & (1u << ch))
                meta.setValue(QStringLiteral("scale/channel%1").arg(ch + 1), m_scale[ch]);
        }
    }

    m_stopping.store(false);
    m_queued.store(0);
    m_overflows.store(0);

    m_thread = new WriterThread([this] { writerLoop(); });
    m_thread->start();

    m_open = true;
    return true;
}

bool RecordWriter::openForReading(const QString &directory, QString *error)
{
    close();

    RecordInfo info;
    std::array<double, MaxChannels> scale{};
    quint64 samplesPerChannel = 0;
    std::array<quint64, MaxChannels> written{};
    if (!readInfo(directory, &info, &scale, &samplesPerChannel, &written)) {
        if (error)
            *error = QStringLiteral("Cannot read %1 (missing or corrupt meta.ini)").arg(directory);
        return false;
    }

    QDir dir(directory);
    m_directory = directory;
    m_channels = MaxChannels;
    m_channelMask = info.channelMask;
    m_sampleRate = info.sampleRate;
    m_samplesPerChannel = samplesPerChannel;
    {
        QSettings meta(metaPath(QDir(directory)), QSettings::IniFormat);
        m_requestedSamplesPerChannel = meta.value(
            QStringLiteral("record/requestedSamplesPerChannel"),
            qulonglong(samplesPerChannel)).toULongLong();
    }

    const QString path = dataFilePathForReading(dir);
    bool any = false;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (!(info.channelMask & (1u << ch)))
            continue;

        auto *in = new QFile(path);
        if (!in->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            delete in;
            continue;
        }

        // The ACTUAL length written, from meta.ini — not the reserved
        // capacity, which the file itself is always pre-sized to regardless
        // of whether the recording ran that long or was stopped early. No
        // writer thread here to confirm anything further, so this written
        // length counts as fully flushed outright.
        const quint64 length = written[ch];
        m_reader[ch] = in;
        m_scale[ch] = scale[ch] > 0.0 ? scale[ch] : 1.0 / 2048.0;
        m_written[ch].store(length);
        m_flushed[ch].store(length);
        any = true;
    }

    if (!any) {
        if (error)
            *error = QStringLiteral("No readable channels in %1").arg(directory);
        close();
        return false;
    }

    m_open = false;   // finished, read-only — same state a just-finished write leaves it in
    return true;
}

void RecordWriter::finishWriting()
{
    if (m_thread) {
        m_stopping.store(true);
        m_wake.wakeAll();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }
    if (m_write) {
        m_write->flush();
        compactToWritten();
        for (int ch = 0; ch < MaxChannels; ++ch)
            m_flushed[ch].store(m_written[ch].load());   // final data now visible
        m_write->close();
        delete m_write;
        m_write = nullptr;

        // The actual length written per channel — the file itself stays
        // pre-sized to the full reserved capacity regardless of whether the
        // recording ran that long, so a later session opening this
        // recording for reading needs this recorded somewhere else to know
        // where each channel's real data actually ends.
        if (!m_directory.isEmpty()) {
            QSettings meta(metaPath(QDir(m_directory)), QSettings::IniFormat);
            for (int ch = 0; ch < MaxChannels; ++ch) {
                if (m_channelMask & (1u << ch)) {
                    meta.setValue(QStringLiteral("written/channel%1").arg(ch + 1),
                                 qulonglong(m_written[ch].load()));
                }
            }
            // The region size after compactToWritten(): what reading uses to
            // find each channel. The original request stays in
            // record/requestedSamplesPerChannel.
            meta.setValue(QStringLiteral("record/samplesPerChannel"),
                          qulonglong(m_samplesPerChannel));
        }
    }
    m_open = false;   // no more writing; the readers stay open
}

// The file is pre-sized to the full record size (one fixed region per
// recorded channel), so a recording stopped early used to keep its whole
// reserved size on disk — 128 MiB (134.2 MB) for a 0.4 MB recording. Here,
// once writing has stopped, each channel's written samples are moved down so
// the regions sit back to back at the size actually used, the file is cut to
// that, and m_samplesPerChannel (which channelByteOffset() reads) becomes the
// new region size so reads in this session keep working. Copying runs
// forward in chunks and every destination lies at or below its source, so
// nothing is overwritten before it has been read. Nothing to do when the
// recording filled its whole size.
void RecordWriter::compactToWritten()
{
    if (!m_write || m_samplesPerChannel == 0)
        return;
    quint64 used = 0;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (m_channelMask & (1u << ch))
            used = std::max<quint64>(used, m_written[ch].load());
    }
    used = std::max<quint64>(used, 1);
    if (used >= m_samplesPerChannel)
        return;

    const qint64 oldRegion = qint64(m_samplesPerChannel) * qint64(sizeof(qint16));
    const qint64 newRegion = qint64(used) * qint64(sizeof(qint16));
    constexpr qint64 kChunk = 4 * 1024 * 1024;
    int rank = 0;
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (!(m_channelMask & (1u << ch)))
            continue;
        const qint64 src = rank * oldRegion;
        const qint64 dst = rank * newRegion;
        const qint64 bytes = qint64(m_written[ch].load()) * qint64(sizeof(qint16));
        if (src != dst) {
            for (qint64 off = 0; off < bytes; off += kChunk) {
                const qint64 n = std::min(kChunk, bytes - off);
                if (!m_write->seek(src + off))
                    return;   // leave the file as it was: still valid, just large
                const QByteArray chunk = m_write->read(n);
                if (chunk.size() != n || !m_write->seek(dst + off)
                    || m_write->write(chunk) != n)
                    return;
            }
        }
        ++rank;
    }
    m_write->flush();
    if (m_write->resize(rank * newRegion))
        m_samplesPerChannel = used;
}

void RecordWriter::close()
{
    if (m_thread) {
        m_stopping.store(true);
        m_wake.wakeAll();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
    }

    if (m_write) {
        m_write->flush();
        m_write->close();
        delete m_write;
        m_write = nullptr;
    }
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (m_reader[ch]) {
            m_reader[ch]->close();
            delete m_reader[ch];
            m_reader[ch] = nullptr;
        }
    }
    m_queue.clear();
    m_channelMask = 0;
    m_samplesPerChannel = 0;
    m_open = false;
}

bool RecordWriter::isReadable() const
{
    for (QFile *f : m_reader) {
        if (f)
            return true;
    }
    return false;
}

// SDK thread. Converts volts to codes and hands the block to the writer.
void RecordWriter::append(int channel, const double *samples, int count)
{
    if (!m_open || channel < 0 || channel >= MaxChannels || !m_write
        || !(m_channelMask & (1u << channel)) || !samples || count <= 0)
        return;

    Block block;
    block.channel = channel;
    block.codes.resize(count);

    const double inverse = 1.0 / m_scale[channel];
    for (int i = 0; i < count; ++i) {
        const double code = samples[i] * inverse;
        block.codes[i] = qint16(std::clamp(code, -32768.0, 32767.0));
    }

    QMutexLocker lock(&m_mutex);
    if (m_queue.size() >= kMaxQueuedBlocks) {
        m_overflows.fetch_add(1);      // the disk is behind; the record has a gap
        return;
    }
    m_queue.enqueue(std::move(block));
    m_queued.store(quint64(m_queue.size()));
    m_wake.wakeOne();
}

void RecordWriter::writerLoop()
{
    for (;;) {
        Block block;
        {
            QMutexLocker lock(&m_mutex);
            while (m_queue.isEmpty() && !m_stopping.load())
                m_wake.wait(&m_mutex);
            if (m_queue.isEmpty() && m_stopping.load())
                return;
            block = m_queue.dequeue();
            m_queued.store(quint64(m_queue.size()));
        }

        if (!m_write)
            continue;

        // Seeks to this channel's own region every time, not just once: a
        // block for a different channel could have run through this same
        // loop iteration ago, moving the file's shared position elsewhere —
        // blocks for different channels arrive interleaved in time on one
        // queue, not one channel's worth fully before the next begins.
        const qint64 offset = channelByteOffset(block.channel)
                             + qint64(m_written[block.channel].load()) * qint64(sizeof(qint16));
        if (!m_write->seek(offset)) {
            m_overflows.fetch_add(1);
            continue;
        }

        const qint64 bytes = qint64(block.codes.size()) * qint64(sizeof(qint16));
        if (m_write->write(reinterpret_cast<const char *>(block.codes.constData()), bytes)
            == bytes) {
            m_written[block.channel].fetch_add(quint64(block.codes.size()));
        } else {
            m_overflows.fetch_add(1);
        }

        // Flushed here, on the writer thread, which is the only thread allowed
        // to touch m_write. m_flushed is what the reader thread trusts. A
        // flush covers everything written to the file so far regardless of
        // channel, so every channel's own flushed mark advances together
        // here, not just the one this particular block belonged to.
        if (m_queue.isEmpty() || m_flushCounter++ % 8 == 0) {
            m_write->flush();
            for (int ch = 0; ch < MaxChannels; ++ch)
                m_flushed[ch].store(m_written[ch].load());
        }
    }
}

// GUI thread. Reads straight out of the file: a record is far too large to hold
// anywhere else, and only the visible window is ever needed at once.
int RecordWriter::read(int channel, quint64 start, int count, float *out)
{
    // Deliberately not gated on m_open: a finished record is closed for writing
    // and must still be readable.
    if (channel < 0 || channel >= m_channels || !out || count <= 0)
        return 0;

    QFile *file = m_reader[channel];
    if (!file)
        return 0;

    // Only data the writer thread has confirmed flushed is safe to read.
    const quint64 have = m_flushed[channel].load();
    if (start >= have)
        return 0;
    const int take = int(std::min<quint64>(quint64(count), have - start));

    const qint64 seekPos = channelByteOffset(channel)
                          + qint64(start) * qint64(sizeof(qint16));
    if (!file->seek(seekPos))
        return 0;

    QVector<qint16> codes(take);
    const qint64 bytes = file->read(reinterpret_cast<char *>(codes.data()),
                                    qint64(take) * qint64(sizeof(qint16)));
    const int got = int(bytes / qint64(sizeof(qint16)));

    const double scale = m_scale[channel];
    for (int i = 0; i < got; ++i)
        out[i] = float(codes[i] * scale);
    return got;
}

// One sample every `length / points`, so a 500 M sample record can be drawn
// without reading 500 M samples.
// `points` is a CAPACITY, not a target count: the caller allocated exactly
// this many floats. Each bucket contributes its min and max (a proper
// envelope, matching what a real scope shows when it cannot draw every
// sample), so at most points/2 buckets are used to stay within that capacity.
//
// The previous version picked one sample per bucket by simple striding. That
// silently discards everything between stride points, which is what made a
// zoomed record look coarse and blocky rather than like a 12-bit capture: a
// fast transition landing between two stride points vanished entirely, and
// what did land could alias against the true shape of the signal.
int RecordWriter::readDecimated(int channel, quint64 start, quint64 length,
                                int points, float *out)
{
    if (channel < 0 || channel >= m_channels || !out || points <= 0 || length == 0)
        return 0;

    QFile *file = m_reader[channel];
    if (!file)
        return 0;

    const quint64 have = m_flushed[channel].load();
    if (start >= have)
        return 0;
    length = std::min(length, have - start);

    const qint64 base = channelByteOffset(channel);

    // Skip decimation whenever there is enough capacity to show every raw
    // sample exactly, one float per sample — not merely enough for half that
    // many min/max pairs. The previous threshold compared points/2 against
    // length, so anywhere the raw sample count fell between points/2 and
    // points it decimated needlessly: a bucket holding two nearly-identical
    // samples still emits (min, max) as two separate points regardless of
    // which came first, so a flat run of samples with a hair of ADC noise in
    // it draws as a visible zigzag that should not be there. Because the
    // bucket boundaries shift with every pan, the same region would show that
    // artifact on some frames and not others — the flicker this was reported
    // as.
    if (quint64(points) >= length || points < 2)
        return read(channel, start, int(std::min<quint64>(length, quint64(points))), out);

    // Past this point length > points >= 2, so points/2 >= 1 and 2*(points/2)
    // never exceeds the caller's capacity.
    const int buckets = points / 2;

    const double scale = m_scale[channel];
    const double bucketSize = double(length) / double(buckets);
    int got = 0;

    // Fast path: the whole span fits comfortably in memory, so read it once
    // sequentially and decimate locally. This is the common case — even a
    // fairly deep zoom is a small fraction of a multi-hundred-MB record.
    constexpr quint64 kSequentialCap = 8ull * 1024 * 1024;   // 16 MB as int16
    if (length <= kSequentialCap) {
        // static_cast, not int(length): the latter is grammatically ambiguous
        // with a function declaration ("vexing parse"), and brace-init here
        // would be worse — QVector has an initializer_list constructor, so
        // {int(length)} silently builds a ONE-element vector holding the value
        // `length`, not a vector of that many elements.
        QVector<qint16> buf(static_cast<int>(length));
        if (!file->seek(base + qint64(start) * qint64(sizeof(qint16))))
            return 0;
        const qint64 bytes = file->read(reinterpret_cast<char *>(buf.data()),
                                        qint64(length) * qint64(sizeof(qint16)));
        const quint64 haveRead = quint64(std::max<qint64>(0, bytes)) / sizeof(qint16);

        for (int b = 0; b < buckets; ++b) {
            const quint64 a = quint64(b * bucketSize);
            const quint64 z = std::min(haveRead, quint64((b + 1) * bucketSize));
            if (z <= a)
                continue;
            qint16 mn = buf[int(a)], mx = buf[int(a)];
            for (quint64 k = a + 1; k < z; ++k) {
                mn = std::min(mn, buf[int(k)]);
                mx = std::max(mx, buf[int(k)]);
            }
            out[got++] = float(mn * scale);
            out[got++] = float(mx * scale);
        }
        return got;
    }

    // The span is too large to read wholesale (a fully zoomed-out view of a
    // multi-gigabyte record). Bound the I/O by sampling a fixed window at the
    // start of each bucket rather than reading the whole bucket: this trades
    // some fidelity for a predictable, small amount of disk traffic, which is
    // an acceptable trade this far zoomed out.
    constexpr int kWindowSamples = 4096;
    QVector<qint16> window(kWindowSamples);

    for (int b = 0; b < buckets; ++b) {
        const quint64 bucketStart = quint64(b * bucketSize);
        const quint64 a = start + bucketStart;
        const int wlen = int(std::min<quint64>(kWindowSamples, length - bucketStart));
        if (wlen <= 0)
            continue;
        if (!file->seek(base + qint64(a) * qint64(sizeof(qint16))))
            break;
        const qint64 bytes = file->read(reinterpret_cast<char *>(window.data()),
                                        qint64(wlen) * qint64(sizeof(qint16)));
        const int n = int(std::max<qint64>(0, bytes) / qint64(sizeof(qint16)));
        if (n <= 0)
            continue;

        qint16 mn = window[0], mx = window[0];
        for (int k = 1; k < n; ++k) {
            mn = std::min(mn, window[k]);
            mx = std::max(mx, window[k]);
        }
        out[got++] = float(mn * scale);
        out[got++] = float(mx * scale);
    }
    return got;
}

quint64 RecordWriter::samplesWritten(int channel) const
{
    // Progress is reported from m_written (immediate), but only m_flushed is
    // ever used to gate a read.
    if (channel < 0 || channel >= MaxChannels)
        return 0;
    return m_written[channel].load();
}

double RecordWriter::megabytesWritten() const
{
    quint64 total = 0;
    for (int ch = 0; ch < MaxChannels; ++ch)
        total += m_written[ch].load();
    return double(total) * double(sizeof(qint16)) / 1048576.0;
}
