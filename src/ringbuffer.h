#pragma once

#include <QMutex>
#include <QMutexLocker>
#include <QVector>

#include <algorithm>
#include <cstring>

/*
 * One channel's rolling history.
 *
 * Written from the SDK's callback thread, read from the GUI thread, so every
 * operation takes the lock. The lock is held only for a memcpy, which is what
 * keeps the callback short enough not to trip the hardware watchdog.
 *
 * Stored as float, not double: the SDK hands us doubles, but 24 bits of
 * mantissa is far more than a 12-bit ADC produces, and float halves the memory.
 * At 25 MS/s a ten-second history is 1 GB in double and 500 MB in float.
 */
class RingBuffer {
public:
    // wrapping: a live display, where the oldest samples fall off the back.
    // linear:   a record, which fills once and then ignores further data.
    void reset(int capacity, bool wrapping = true)
    {
        QMutexLocker lock(&m_mutex);
        m_data.assign(std::max(1, capacity), 0.0f);
        m_head = 0;
        m_written = 0;
        m_wrapping = wrapping;
    }

    bool full() const
    {
        QMutexLocker lock(&m_mutex);
        return m_written >= quint64(m_data.size());
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        std::fill(m_data.begin(), m_data.end(), 0.0f);
        m_head = 0;
        m_written = 0;
    }

    // Appends, converting to float. Older samples fall off the back.
    void append(const double *source, int count)
    {
        if (!source || count <= 0)
            return;

        QMutexLocker lock(&m_mutex);
        const int capacity = m_data.size();
        if (capacity <= 0)
            return;

        if (!m_wrapping) {
            // A record takes what still fits and drops the rest; the caller
            // stops the stream once it is full.
            const int room = capacity - int(std::min<quint64>(m_written, quint64(capacity)));
            const int take = std::min(count, room);
            for (int i = 0; i < take; ++i)
                m_data[m_head + i] = float(source[i]);
            m_head += take;
            m_written += quint64(take);
            return;
        }

        // A block larger than the buffer: only its tail can survive.
        if (count >= capacity) {
            source += count - capacity;
            count = capacity;
            m_head = 0;
        }

        for (int i = 0; i < count; ++i) {
            m_data[m_head] = float(source[i]);
            m_head = (m_head + 1) % capacity;
        }
        m_written += quint64(count);
    }

    // The newest `count` samples, oldest first. Returns how many were written.
    int newest(int count, float *out) const
    {
        if (!out || count <= 0)
            return 0;

        QMutexLocker lock(&m_mutex);
        const int capacity = m_data.size();
        const int available = int(std::min<quint64>(m_written, quint64(capacity)));
        const int take = std::min(count, available);
        if (take <= 0)
            return 0;

        int start = m_head - take;
        while (start < 0)
            start += capacity;

        const int firstRun = std::min(take, capacity - start);
        std::memcpy(out, m_data.constData() + start, size_t(firstRun) * sizeof(float));
        if (take > firstRun) {
            std::memcpy(out + firstRun, m_data.constData(),
                        size_t(take - firstRun) * sizeof(float));
        }
        return take;
    }

    // Absolute range, for navigating a record. Only meaningful while linear.
    int read(quint64 start, int count, float *out) const
    {
        if (!out || count <= 0)
            return 0;

        QMutexLocker lock(&m_mutex);
        const quint64 have = std::min<quint64>(m_written, quint64(m_data.size()));
        if (start >= have)
            return 0;

        const int take = int(std::min<quint64>(quint64(count), have - start));
        std::memcpy(out, m_data.constData() + start, size_t(take) * sizeof(float));
        return take;
    }

    // Generalizes newest(): `count` samples ending `offsetFromNewest`
    // samples before the newest one instead of AT it. offsetFromNewest == 0
    // reads exactly what newest() would. A wrapping ring has no meaningful
    // "absolute index" once it has wrapped more than once (that is why
    // read() above only makes sense for a non-wrapping/linear buffer), but
    // "how far back from the live edge" stays meaningful regardless of how
    // many times the ring has wrapped, which is what makes this safe to use
    // against a Live channel's rolling ring.
    int rangeFromNewest(quint64 offsetFromNewest, int count, float *out) const
    {
        if (!out || count <= 0)
            return 0;

        QMutexLocker lock(&m_mutex);
        const int capacity = m_data.size();
        const int available = int(std::min<quint64>(m_written, quint64(capacity)));
        if (available <= 0)
            return 0;

        // Same clamping newest() already does at its one end, just applied
        // to both ends of a window that may not reach all the way to the
        // newest sample.
        const int clampedOffset = int(std::min<quint64>(offsetFromNewest, quint64(available)));
        const int endExclusive  = available - clampedOffset;   // samples-from-oldest, one past the last wanted
        const int take = std::min(count, endExclusive);
        if (take <= 0)
            return 0;

        int start = m_head - clampedOffset - take;
        while (start < 0)
            start += capacity;

        const int firstRun = std::min(take, capacity - start);
        std::memcpy(out, m_data.constData() + start, size_t(firstRun) * sizeof(float));
        if (take > firstRun) {
            std::memcpy(out + firstRun, m_data.constData(),
                        size_t(take - firstRun) * sizeof(float));
        }
        return take;
    }

    // How many samples are actually resident right now — written() is the
    // raw lifetime counter and is not clamped to capacity() the way this is.
    quint64 available() const
    {
        QMutexLocker lock(&m_mutex);
        return std::min<quint64>(m_written, quint64(m_data.size()));
    }

    quint64 written() const
    {
        QMutexLocker lock(&m_mutex);
        return m_written;
    }

    int capacity() const
    {
        QMutexLocker lock(&m_mutex);
        return m_data.size();
    }

private:
    mutable QMutex m_mutex;
    QVector<float> m_data;
    int     m_head = 0;
    bool    m_wrapping = true;
    quint64 m_written = 0;
};
