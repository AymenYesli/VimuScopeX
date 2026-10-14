#include "liveworker.h"
#include "perftrace.h"
#include "streamengine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kNoData = std::numeric_limits<double>::quiet_NaN();
constexpr qint64 kRawMax = 4 * 1024 * 1024;   // windows up to this are read in full
}

// Each frame covers the whole window, newest sample at the right edge: B
// buckets, each 1/B of the window, as min / max pairs. Buckets older than
// the history hold NaN ("no data"; ScopeView::setTraceGaps). The first
// version read only the newest 4M samples and the plot stretched them over
// the full width — 0.84 s drawn as 50 s, the time axis wrong.
LiveResult LiveWorker::process(const LiveJob &job)
{
    VSX_TRACE("worker.liveFrame");
    LiveResult r;
    r.wanted = job.wanted;
    r.rate = job.rate;
    const qint64 W = std::max<qint64>(2, job.wanted);
    const int B = std::max(2, job.pairs);
    const double reducedRate = double(B) / (double(W) / job.rate);   // buckets per second
    QVector<float> block;

    for (const LiveJob::Channel &jc : job.channels) {
        const quint64 available = m_stream->historyAvailable(jc.ch);
        if (available < 2)
            continue;
        r.available = std::max(r.available, available);
        fold(jc.ch, available);
        const HistoryCache &h = m_history[size_t(jc.ch)];

        LiveResult::Channel out;
        out.ch = jc.ch;
        out.pairs.fill(kNoData, 2 * B);
        out.shownRate = 2.0 * reducedRate;
        const bool lowFilter = (jc.filter == ScopeView::FilterLowPass || jc.filter == ScopeView::FilterBandPass)
                               && jc.highHz * 8.0 <= reducedRate;
        // Bucket b covers window samples b*W/B up to (b+1)*W/B; the window ends
        // `offset` samples before the newest.
        const auto bucketSpan = [&](int b, qint64 *a, qint64 *e) {
            *a = qint64(double(W) * b / B);
            *e = std::max(qint64(double(W) * (b + 1) / B), *a + 1);
        };

        QVector<double> raw;   // newest full-rate samples (filtered if the channel is)
        const auto readRaw = [&](qint64 count) {
            const int got = m_stream->rangeFromNewest(jc.ch, job.offset, int(count), &block);
            raw.resize(std::max(0, got));
            for (int i = 0; i < got; ++i)
                raw[i] = double(block[i]);
            if (jc.filter != ScopeView::FilterOff && got > 2)
                raw = ScopeView::applyFilter(raw, job.rate, jc.filter, jc.lowHz, jc.highHz);
        };

        if (W <= kRawMax) {
            // The whole window, read: exact.
            readRaw(std::min<qint64>(W, qint64(available)));
            const qint64 got = raw.size();
            const qint64 missing = W - got;   // window older than the history
            QVector<double> averages(B, kNoData);
            for (int b = 0; b < B; ++b) {
                qint64 a, e;
                bucketSpan(b, &a, &e);
                a -= missing;
                e -= missing;
                if (e <= 0)
                    continue;
                a = std::max<qint64>(a, 0);
                e = std::min(e, got);
                if (e <= a)
                    continue;
                const auto [lo, hi] = std::minmax_element(raw.constBegin() + a, raw.constBegin() + e);
                out.pairs[2 * b] = *lo;
                out.pairs[2 * b + 1] = *hi;
            }
            if (jc.measure && got > 2) {
                out.measurements = ScopeView::measureSamples(raw, jc.gain, jc.bias, job.rate);
                out.measured = true;
            }
        } else {
            // Longer: from the history blocks. Absolute sample positions.
            const qint64 nb = qint64(h.blocks.size());
            const qint64 bs = qint64(h.blockSize);
            const qint64 end = qint64(h.processed) - qint64(job.offset);       // one past the window
            const qint64 start = end - W;
            const qint64 lastFullEnd = qint64(h.processed) - qint64(h.partialCount);
            const qint64 firstBlock = lastFullEnd - nb * bs;
            const qint64 oldest = std::max(firstBlock, qint64(h.processed) - qint64(available));
            QVector<double> averages(B, kNoData);
            double wMin = 0.0, wMax = 0.0, wSum = 0.0, wSq = 0.0;
            qint64 wCount = 0;
            for (int b = 0; b < B; ++b) {
                qint64 a, e;
                bucketSpan(b, &a, &e);
                a += start;
                e += start;
                a = std::max(a, oldest);
                e = std::min(e, qint64(h.processed));
                if (e <= a)
                    continue;
                // Blocks overlapping a up to e, plus the partial block at the end.
                qint64 k0 = (a - firstBlock) / bs, k1 = (e - firstBlock + bs - 1) / bs;
                k0 = std::clamp<qint64>(k0, 0, nb);
                k1 = std::clamp<qint64>(k1, 0, nb);
                bool found = false;
                double mn = 0.0, mx = 0.0, sum = 0.0;
                qint64 count = 0;
                const auto take = [&](const Block &blk, qint64 n) {
                    if (!found) {
                        mn = blk.min;
                        mx = blk.max;
                        found = true;
                    } else {
                        mn = std::min(mn, double(blk.min));
                        mx = std::max(mx, double(blk.max));
                    }
                    sum += blk.sum;
                    count += n;
                };
                for (qint64 k = k0; k < k1; ++k)
                    take(h.blocks[size_t(k)], bs);
                if (h.partialCount > 0 && e > lastFullEnd)
                    take(h.partial, qint64(h.partialCount));
                if (!found)
                    continue;
                out.pairs[2 * b] = mn;
                out.pairs[2 * b + 1] = mx;
                averages[b] = sum / double(std::max<qint64>(1, count));
            }
            // Window-wide statistics, each block once.
            {
                const qint64 a = std::max(start, oldest), e = qint64(h.processed) - qint64(job.offset);
                bool found = false;
                const auto add = [&](const Block &blk, qint64 n) {
                    if (!found) {
                        wMin = blk.min;
                        wMax = blk.max;
                        found = true;
                    } else {
                        wMin = std::min(wMin, double(blk.min));
                        wMax = std::max(wMax, double(blk.max));
                    }
                    wSum += blk.sum;
                    wSq += blk.sumSq;
                    wCount += n;
                };
                if (e > a) {
                    const qint64 k0 = std::clamp<qint64>((a - firstBlock) / bs, 0, nb);
                    const qint64 k1 = std::clamp<qint64>((e - firstBlock + bs - 1) / bs, 0, nb);
                    for (qint64 k = k0; k < k1; ++k)
                        add(h.blocks[size_t(k)], bs);
                    if (h.partialCount > 0 && e > lastFullEnd)
                        add(h.partial, qint64(h.partialCount));
                }
            }
            if (jc.filter != ScopeView::FilterOff) {
                // Filtered at this zoom from the bucket averages: exact enough
                // for low-pass / band-pass well below the bucket rate; high-
                // pass / band-stop subtract the removed part from the
                // envelope (approximate at this zoom).
                int first = 0;
                while (first < B && !std::isfinite(averages[first]))
                    ++first;
                if (B - first > 3) {
                    const QVector<double> valid = averages.mid(first);
                    if (lowFilter) {
                        const QVector<double> f = ScopeView::applyFilter(valid, reducedRate, jc.filter, jc.lowHz, jc.highHz);
                        for (int b = first; b < B; ++b)
                            out.pairs[2 * b] = out.pairs[2 * b + 1] = f[b - first];
                    } else if (jc.filter == ScopeView::FilterHighPass || jc.filter == ScopeView::FilterBandStop) {
                        const QVector<double> removed = ScopeView::removedPart(valid, reducedRate, jc.filter, jc.lowHz, jc.highHz);
                        for (int b = first; b < B; ++b) {
                            out.pairs[2 * b] -= removed[b - first];
                            out.pairs[2 * b + 1] -= removed[b - first];
                        }
                    }
                }
            }
            if (jc.measure) {
                // Frequency, period, duty, widths: the newest full-rate samples.
                readRaw(std::min<qint64>(kRawMax, qint64(available)));
                if (raw.size() > 2) {
                    out.measurements = ScopeView::measureSamples(raw, jc.gain, jc.bias, job.rate);
                    out.measured = true;
                }
                // Level measurements over the whole window, exactly, from the
                // blocks (unfiltered data, so only for unfiltered channels).
                if (jc.filter == ScopeView::FilterOff && wCount > 0) {
                    const double g = jc.gain, bias = jc.bias;
                    const double meanRaw = wSum / double(wCount);
                    const double meanSq = wSq / double(wCount);
                    ScopeView::Measurements &m = out.measurements;
                    const double a = g * wMin + bias, b2 = g * wMax + bias;
                    m.min = std::min(a, b2);
                    m.max = std::max(a, b2);
                    m.vpp = m.max - m.min;
                    m.mean = g * meanRaw + bias;
                    m.vrms = std::sqrt(std::max(0.0, g * g * meanSq + 2.0 * g * bias * meanRaw + bias * bias));
                    m.acRms = std::fabs(g) * std::sqrt(std::max(0.0, meanSq - meanRaw * meanRaw));
                    m.valid = true;
                    out.measured = true;
                }
            }
        }

        // Spectrum / XY: the newest full-rate samples.
        {
            const int got = m_stream->rangeFromNewest(jc.ch, job.offset, int(std::min<quint64>(65536, available)), &block);
            out.tail.resize(std::max(0, got));
            for (int i = 0; i < got; ++i)
                out.tail[i] = double(block[i]);
        }
        if (job.overview)
            out.overview = overviewPairs(jc.ch);
        r.channels.push_back(std::move(out));
    }
    return r;
}

// Folds in what arrived since the last time (checked against a full walk in
// simulation: identical spike placement, bands within block boundaries).
void LiveWorker::fold(int ch, quint64 available)
{
    constexpr quint64 kChunk = 1024 * 1024;
    constexpr quint64 kMaxBlocks = 8192;
    HistoryCache &cache = m_history[size_t(ch)];
    const quint64 written = m_stream->written(ch);
    const quint64 capacity = std::max<quint64>(1, m_stream->historyCapacity(ch));
    if (written < cache.processed || capacity != cache.capacity) {
        cache = HistoryCache();
        cache.capacity = capacity;
        cache.blockSize = std::max<quint64>(1, (capacity + kMaxBlocks - 1) / kMaxBlocks);
        cache.processed = written - std::min(written, available);
    }
    quint64 fresh = written - cache.processed;
    if (fresh > available) {
        cache.blocks.clear();
        cache.partialCount = 0;
        cache.processed = written - available;
        fresh = available;
    }
    QVector<float> chunk;
    quint64 done = 0;
    while (done < fresh) {
        const quint64 count = std::min(kChunk, fresh - done);
        const int got = m_stream->rangeFromNewest(ch, fresh - done - count, int(count), &chunk);
        if (got <= 0)
            break;
        for (int i = 0; i < got; ++i) {
            const float v = chunk[i];
            Block &p = cache.partial;
            if (cache.partialCount == 0) {
                p = Block();
                p.min = p.max = v;
            } else {
                p.min = std::min(p.min, v);
                p.max = std::max(p.max, v);
            }
            p.sum += v;
            p.sumSq += double(v) * double(v);
            if (++cache.partialCount == cache.blockSize) {
                cache.blocks.push_back(p);
                cache.partialCount = 0;
            }
        }
        done += quint64(got);
    }
    cache.processed = written;
    const size_t keep = size_t(available / cache.blockSize + 1);
    while (cache.blocks.size() > keep)
        cache.blocks.pop_front();
}

QVector<double> LiveWorker::overviewPairs(int ch) const
{
    constexpr int kBuckets = 1000;
    const HistoryCache &cache = m_history[size_t(ch)];
    QVector<float> lo(kBuckets), hi(kBuckets);
    QVector<char> seen(kBuckets, 0);
    const size_t n = cache.blocks.size() + (cache.partialCount ? 1 : 0);
    for (size_t k = 0; k < n; ++k) {
        const Block &blk = k < cache.blocks.size() ? cache.blocks[k] : cache.partial;
        const int b = int(quint64(k) * kBuckets / quint64(n));
        if (!seen[b]) {
            lo[b] = blk.min;
            hi[b] = blk.max;
            seen[b] = 1;
        } else {
            lo[b] = std::min(lo[b], blk.min);
            hi[b] = std::max(hi[b], blk.max);
        }
    }
    QVector<double> overview;
    overview.reserve(2 * kBuckets);
    for (int b = 0; b < kBuckets; ++b) {
        if (!seen[b])
            continue;
        overview.append(double(lo[b]));
        overview.append(double(hi[b]));
    }
    return overview;
}
