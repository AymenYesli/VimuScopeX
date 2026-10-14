#pragma once

#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "cantools.h"
#include "serialbus.h"
#include "sent.h"
#include "diag.h"

// One decode request: an analog trace, the threshold that turns it into a
// logic line, and the protocol settings. Built on the GUI thread, run on
// the decoder thread (see DecoderEngine), so it carries its own copy of the
// samples rather than pointing into anything the GUI may change meanwhile.
struct DecodeJob {
    int      slot = 0;
    quint64  id = 0;             // echoed in the result, to drop stale ones
    QString  protocol;           // "can", "lin" or "kline"
    QVector<float> volts;        // one value per sample, display volts
    double   sampleRate = 0.0;
    double   threshold = 0.0;    // volts
    bool     invert = false;     // logic 1 = below the threshold
    // Protocol settings. CAN: bitrate, fastBitrate (bit/s), samplePoint (%).
    // LIN: baud, linVersion (1/2). K-Line: baud, gapFraming, gapBits.
    // Record mode decodes a margin beyond the window on screen, so a frame
    // cut by an edge decodes whole; only from visibleFrom up to (not
    // including) visibleTo, in this job's sample numbers, is reported.
    // 0 / 0: everything.
    qint64   visibleFrom = 0;
    qint64   visibleTo = 0;
    qint64   bitrate = 500000;
    qint64   fastBitrate = 2000000;
    double   samplePoint = 70.0;
    qint64   baud = 19200;       // LIN: 0 = measure it from the sync bytes
    double   sentTickUs = 3.0;   // SENT: nominal tick
    int      sentNibbles = 6;    // SENT: data nibbles per frame
    int      sentMode = 0;       // SENT: 0 fast channel, 1 slow channel (serial messages), 2 SPC
    bool     diagnostics = false;   // CAN: show ISO-TP / OBD-II / UDS instead of the frames
    int      diagIds = 0;           // which IDs are diagnostic (Diag::diagnosticId())
    int      linVersion = 2;
    bool     gapFraming = false;
    qint64   gapBits = 260;
};

struct DecodeAnnotation {
    double      start = 0.0;     // in the job's own sample numbers
    double      end = 0.0;
    QStringList texts;           // longest first, as the decoder gives them
    bool        error = false;
    QString     group;           // the decoder's annotation class ("data", "id" ...)
};

struct DecodeResult {
    int      slot = 0;
    quint64  id = 0;
    QVector<DecodeAnnotation> annotations;
    QString  message;            // shown in the bus row instead, when set
    // CAN only: whole frames for the bus-health panel, in this job's own
    // sample positions, plus what the positions mean in time.
    std::vector<CanTools::CanFrame> canFrames;
    int      strayErrors = 0;    // decoder warnings outside any frame
    // LIN / K-Line: frames / messages for the bus-health panel (SerialBus,
    // from the decoder's own stream), in this job's sample positions, and the
    // bit time the bus really runs at (samples per bit; 0: unknown).
    std::vector<SerialBus::LinFrame> linFrames;
    SerialBus::KLineReport kline;
    double   busBitTime = 0.0;
    qint64   detectedBaud = 0;   // LIN set to Auto: the rate measured from the sync bytes (0: none found)
    std::vector<Sent::Frame> sentFrames;   // SENT: frames starting in the window, job sample positions
    std::vector<Sent::SerialMessage> sentMessages;   // SENT slow channel, likewise
    std::vector<Diag::Message> diagMessages;          // OBD-II / UDS messages in the window
    quint64  sampleCount = 0;
    double   sampleRate = 0.0;
};

Q_DECLARE_METATYPE(DecodeResult)

// Runs libsigrokdecode. Lives on its own thread for its whole life, so the
// library (and the Python interpreter it embeds) is only ever touched from
// that one thread; jobs arrive queued and are decoded one at a time.
class DecoderEngine : public QObject
{
    Q_OBJECT
public:
    // Whether this build was compiled with libsigrokdecode at all.
    static bool available();

public slots:
    void decode(const DecodeJob &job);

signals:
    void finished(const DecodeResult &result);

private:
    bool ensureInit();
    bool    m_initTried = false;
    bool    m_initOk = false;
    QString m_initError;
};
