#pragma once
//
// Live's data preparation for long windows, on its own thread: read each
// channel's window, filter, reduce to min/max pairs, measure the full-rate
// data exactly, keep the overview strip — so the interface thread only
// displays a small, finished frame. Measured on the interface thread
// (50 s at 5 MS/s, 2 channels + Math): ~190 ms per refresh after reduction,
// mostly the exact measurements and the filter over 4M samples per channel.
// The ring buffers are mutex-protected, so reading them here is safe.
//
#include "scopeview.h"

#include <QObject>
#include <QVector>

#include <array>
#include <deque>
#include <utility>

class StreamEngine;

struct LiveJob {
    struct Channel {
        int    ch = 0;
        int    filter = 0;              // ScopeView::FilterType
        double lowHz = 0.0, highHz = 0.0;
        double gain = 1.0, bias = 0.0;  // measure()'s, for the exact measurements
        bool   measure = false;         // a readout is on
    };
    qint64  wanted = 0;                 // the whole window, samples per channel (uncapped)
    quint64 offset = 0;                 // from the newest sample (Live panning)
    double  rate = 0.0;
    int     pairs = 8192;
    bool    overview = false;           // refresh the overview strip this time
    QVector<Channel> channels;
};

struct LiveResult {
    struct Channel {
        int    ch = 0;
        QVector<double> pairs;          // min, max per bucket
        double shownRate = 0.0;         // the pairs' rate (keeps the time axis)
        bool   measured = false;
        ScopeView::Measurements measurements;
        QVector<double> tail;           // newest full-rate samples (Spectrum, XY)
        QVector<double> overview;       // empty: unchanged this time
    };
    qint64  wanted = 0;
    double  rate = 0.0;
    quint64 available = 0;
    QVector<Channel> channels;
};

class LiveWorker : public QObject
{
public:
    explicit LiveWorker(StreamEngine *stream) : m_stream(stream) {}
    LiveResult process(const LiveJob &job);   // runs on the worker thread

private:
    // The whole history in fixed-size blocks — min / max for the envelope,
    // sum / sum of squares for exact window-wide mean and RMS. Folded in
    // incrementally (only what arrived since last time).
    struct Block {
        float  min = 0.0f, max = 0.0f;
        double sum = 0.0, sumSq = 0.0;
    };
    struct HistoryCache {
        quint64 processed = 0, capacity = 0, blockSize = 1, partialCount = 0;
        Block   partial;
        std::deque<Block> blocks;
    };
    void fold(int ch, quint64 available);
    QVector<double> overviewPairs(int ch) const;
    std::array<HistoryCache, 4> m_history;
    StreamEngine *m_stream;
};
