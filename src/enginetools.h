#pragma once
//
// Engine analysis for the Rulers > Engine tools: cycles, cylinder bands and
// the relative compression test. Plain C++ with no Qt, so it is tested
// standalone (simulated cranking data) and MainWindow just converts at the
// edges. All positions are sample indices of whatever buffer is analysed.
//
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace EngineTools {

struct Band {
    double start = 0.0;
    double end = 0.0;
    double tdc = 0.0;      // TDC, or the compression peak in the compression test
    int    cylinder = 0;
    bool   excluded = false;
};

// "1-3-4-2", "1 3 4 2", "1,3,4,2": every cylinder 1..n exactly once.
inline bool parseFiringOrder(const std::string &text, int cylinders, std::vector<int> *order)
{
    order->clear();
    int value = -1;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? text[i] : ' ';
        if (c >= '0' && c <= '9') {
            value = (value < 0 ? 0 : value * 10) + (c - '0');
        } else if (value >= 0) {
            order->push_back(value);
            value = -1;
        }
    }
    if (int(order->size()) != cylinders)
        return false;
    std::vector<bool> seen(size_t(cylinders) + 1, false);
    for (int c : *order) {
        if (c < 1 || c > cylinders || seen[size_t(c)])
            return false;
        seen[size_t(c)] = true;
    }
    return true;
}

// Manual mode: the cycle from marker a to marker b, repeated across the whole buffer (0 to n)
// (assumes a steady speed). Starts at a + k(b - a).
inline std::vector<double> manualCycles(double a, double b, double n)
{
    std::vector<double> starts;
    const double len = b - a;
    if (len <= 1.0 || n <= 0.0)
        return starts;
    const double kFirst = std::floor((0.0 - a) / len);
    const double kLast = std::ceil((n - a) / len);
    for (double k = kFirst; k <= kLast && starts.size() < 100000; k += 1.0)
        starts.push_back(a + k * len);
    return starts;
}

// Sync mode: one edge per engine cycle (cylinder-1 ignition/injector, or a
// one-pulse-per-cycle cam sensor). Hysteresis rejects noise around the
// threshold; holdoff rejects ringing after each edge.
inline std::vector<double> syncEdges(const std::vector<double> &x, double threshold, bool rising,
                                     double hysteresis, size_t holdoff)
{
    std::vector<double> edges;
    if (x.size() < 2)
        return edges;
    const double hi = threshold + hysteresis / 2.0;
    const double lo = threshold - hysteresis / 2.0;
    bool armed = rising ? x[0] < lo : x[0] > hi;
    size_t last = 0;
    bool any = false;
    for (size_t i = 1; i < x.size(); ++i) {
        if (rising) {
            if (x[i] < lo) armed = true;
            else if (armed && x[i] > hi) {
                if (!any || i - last >= holdoff) {
                    edges.push_back(double(i));
                    last = i;
                    any = true;
                }
                armed = false;
            }
        } else {
            if (x[i] > hi) armed = true;
            else if (armed && x[i] < lo) {
                if (!any || i - last >= holdoff) {
                    edges.push_back(double(i));
                    last = i;
                    any = true;
                }
                armed = false;
            }
        }
    }
    return edges;
}

// One band per cylinder per cycle, centred on its TDC. Cylinder 1 TDC is at
// 0 degrees; offsetDeg is the angle at each cycle start (where the sync
// event sits relative to cylinder 1 TDC; 0 for manual markers).
inline std::vector<Band> bandsFromCycles(const std::vector<double> &starts, double cycleDeg,
                                         double offsetDeg, const std::vector<int> &order)
{
    std::vector<Band> bands;
    const int cyl = int(order.size());
    if (cyl == 0 || cycleDeg <= 0.0)
        return bands;
    const double seg = cycleDeg / cyl;
    for (size_t k = 0; k + 1 < starts.size(); ++k) {
        const double len = starts[k + 1] - starts[k];
        if (len <= 0.0)
            continue;
        for (int p = 0; p < cyl; ++p) {
            double rel = std::fmod(p * seg - offsetDeg, cycleDeg);
            if (rel < 0.0)
                rel += cycleDeg;
            Band b;
            b.tdc = starts[k] + rel / cycleDeg * len;
            b.start = b.tdc - seg / 2.0 / cycleDeg * len;
            b.end = b.tdc + seg / 2.0 / cycleDeg * len;
            b.cylinder = order[size_t(p)];
            bands.push_back(b);
        }
    }
    std::sort(bands.begin(), bands.end(), [](const Band &a, const Band &b) { return a.tdc < b.tdc; });
    return bands;
}

// Local maxima at least `minSpacing` apart that stand out from their
// surroundings by `minProminence`. Sliding-window max in O(n).
inline std::vector<size_t> findPeaks(const std::vector<double> &x, size_t halfWindow,
                                     double minProminence)
{
    std::vector<size_t> peaks;
    const size_t n = x.size();
    if (n < 3 || halfWindow == 0)
        return peaks;
    // winMax[i] / winMin[i] over [i - w, i + w].
    std::vector<double> winMax(n), winMin(n);
    std::deque<size_t> qMax, qMin;
    size_t next = 0;
    for (size_t i = 0; i < n; ++i) {
        const size_t hiEdge = std::min(n - 1, i + halfWindow);
        while (next <= hiEdge) {
            while (!qMax.empty() && x[qMax.back()] <= x[next]) qMax.pop_back();
            qMax.push_back(next);
            while (!qMin.empty() && x[qMin.back()] >= x[next]) qMin.pop_back();
            qMin.push_back(next);
            ++next;
        }
        const size_t loEdge = i >= halfWindow ? i - halfWindow : 0;
        while (qMax.front() < loEdge) qMax.pop_front();
        while (qMin.front() < loEdge) qMin.pop_front();
        winMax[i] = x[qMax.front()];
        winMin[i] = x[qMin.front()];
    }
    for (size_t i = 1; i + 1 < n; ++i) {
        if (x[i] == winMax[i] && x[i] > x[i - 1] && x[i] - winMin[i] >= minProminence) {
            if (!peaks.empty() && i - peaks.back() <= halfWindow) {
                if (x[i] > x[peaks.back()])
                    peaks.back() = i;
                continue;
            }
            peaks.push_back(i);
        }
    }
    return peaks;
}

struct Compression {
    std::vector<double> percent;     // index = cylinder - 1; empty if no result
    std::vector<Band> bands;         // one per detected peak, cylinder-assigned
    std::vector<double> cycleStarts; // cylinder-1 peaks (0 degrees)
    int cyclesUsed = 0;
    int cyclesTotal = 0;
};

// Relative compression from a cranking signal where each compression stroke
// is a peak (current; battery voltage is negated by the caller).
//   expectedSpacing: samples between compression peaks (cycle / cylinders),
//                    from the markers or the sync — sets the peak window.
//   cylinder1At:     a position near cylinder 1's compression (marker A or
//                    the sync's cylinder-1 TDC); the nearest peak is cylinder 1.
// Peaks are assigned in firing order by counting from that one, so a varying
// cranking speed doesn't matter. A cycle (cylinder-1 peak plus the next n-1)
// is used only if its peak spacings are all within 0.5x..1.5x the median
// spacing — a missing or extra peak (stall, misfire, noise) drops that cycle
// rather than shifting every later assignment's result.
inline Compression relativeCompression(const std::vector<double> &x, double expectedSpacing,
                                       double cylinder1At, const std::vector<int> &order)
{
    Compression r;
    const int cyl = int(order.size());
    if (cyl < 1 || x.size() < 8 || expectedSpacing < 4.0)
        return r;

    // 5th and 95th percentiles in linear time (a full sort was slow on a
    // million samples while dragging a marker).
    std::vector<double> work(x);
    const size_t i5 = work.size() * 5 / 100, i95 = work.size() * 95 / 100;
    std::nth_element(work.begin(), work.begin() + long(i95), work.end());
    const double p95 = work[i95];
    std::nth_element(work.begin(), work.begin() + long(i5), work.begin() + long(i95));
    const double p5 = work[i5];
    const std::vector<size_t> peaks =
        findPeaks(x, size_t(expectedSpacing * 0.35), 0.15 * (p95 - p5));
    if (peaks.size() < size_t(cyl))
        return r;

    std::vector<double> gaps;
    for (size_t j = 1; j < peaks.size(); ++j)
        gaps.push_back(double(peaks[j] - peaks[j - 1]));
    std::vector<double> g(gaps);
    std::nth_element(g.begin(), g.begin() + long(g.size() / 2), g.end());
    const double median = g[g.size() / 2];
    const auto gapOk = [&](size_t j) {   // gap between peaks j-1 and j
        return gaps[j - 1] >= 0.5 * median && gaps[j - 1] <= 1.5 * median;
    };

    // Each peak's place in the stroke sequence. A gap about k times the
    // median means k - 1 strokes produced no peak (stall, misfire, a weak
    // stroke under the threshold): the count advances by k, so every later
    // peak still gets its own cylinder instead of all shifting by one.
    std::vector<long> seq(peaks.size(), 0);
    for (size_t j = 1; j < peaks.size(); ++j)
        seq[j] = seq[j - 1] + std::max(1L, long(std::lround(gaps[j - 1] / median)));

    // Cylinder 1 = the peak nearest the anchor; everything else counts from it.
    size_t p0 = 0;
    for (size_t j = 1; j < peaks.size(); ++j) {
        if (std::fabs(double(peaks[j]) - cylinder1At) < std::fabs(double(peaks[p0]) - cylinder1At))
            p0 = j;
    }
    const auto positionOf = [&](long s) {   // firing-order position of stroke s
        long d = (s - seq[p0]) % cyl;
        return int(d < 0 ? d + cyl : d);
    };

    // Height above the trough before it (a slow baseline drift cancels).
    std::vector<double> height(peaks.size());
    for (size_t j = 0; j < peaks.size(); ++j) {
        const size_t from = j > 0 ? peaks[j - 1]
                                  : (peaks[j] > size_t(median / 2) ? peaks[j] - size_t(median / 2) : 0);
        double trough = x[peaks[j]];
        for (size_t i = from; i <= peaks[j]; ++i)
            trough = std::min(trough, x[i]);
        height[j] = x[peaks[j]] - trough;
    }

    // Stroke number -> peak index (-1: no peak for that stroke).
    std::vector<long> peakOf(size_t(seq.back() + 1), -1);
    for (size_t j = 0; j < peaks.size(); ++j)
        peakOf[size_t(seq[j])] = long(j);
    const auto strokeAt = [&](long s) {   // where stroke s is, measured or estimated
        if (s >= 0 && s <= seq.back() && peakOf[size_t(s)] >= 0)
            return double(peaks[size_t(peakOf[size_t(s)])]);
        long nearest = 0;   // nearest stroke that has a peak
        for (size_t j = 0; j < peaks.size(); ++j) {
            if (std::labs(seq[j] - s) < std::labs(seq[size_t(nearest)] - s))
                nearest = long(j);
        }
        return double(peaks[size_t(nearest)]) + double(s - seq[size_t(nearest)]) * median;
    };

    // A cycle = cylinder 1's stroke plus the next n - 1. Used only when every
    // stroke in it has a peak and every gap inside it (and to the next
    // cycle) is a normal single-stroke gap.
    std::vector<double> sum(size_t(cyl), 0.0);
    std::vector<bool> usedPeak(peaks.size(), false);
    long first = seq[p0] - ((seq[p0] - seq.front()) / cyl) * cyl;
    for (long s = first; s + cyl - 1 <= seq.back(); s += cyl) {
        ++r.cyclesTotal;
        r.cycleStarts.push_back(strokeAt(s));
        bool ok = true;
        for (long m = 0; m < cyl && ok; ++m)
            ok = peakOf[size_t(s + m)] >= 0;
        for (long m = 1; m <= cyl && ok; ++m) {
            if (s + m > seq.back())
                break;
            const long j = peakOf[size_t(s + m)];
            ok = j >= 1 && seq[size_t(j - 1)] == s + m - 1 && gapOk(size_t(j));
        }
        if (!ok)
            continue;
        ++r.cyclesUsed;
        for (long m = 0; m < cyl; ++m) {
            const size_t j = size_t(peakOf[size_t(s + m)]);
            sum[size_t(order[size_t(positionOf(s + m))] - 1)] += height[j];
            usedPeak[j] = true;
        }
    }
    // The open cycle after the last full one still gets a start, so degrees
    // are defined up to the end.
    if (!r.cycleStarts.empty())
        r.cycleStarts.push_back(strokeAt(first + long(r.cycleStarts.size()) * cyl));

    for (size_t j = 0; j < peaks.size(); ++j) {
        Band b;
        b.tdc = double(peaks[j]);
        b.start = j > 0 ? (double(peaks[j - 1]) + peaks[j]) / 2.0 : peaks[j] - median / 2.0;
        b.end = j + 1 < peaks.size() ? (double(peaks[j]) + peaks[j + 1]) / 2.0 : peaks[j] + median / 2.0;
        b.cylinder = order[size_t(positionOf(seq[j]))];
        b.excluded = !usedPeak[j];
        r.bands.push_back(b);
    }

    if (r.cyclesUsed > 0) {
        double best = 0.0;
        for (double s : sum)
            best = std::max(best, s);
        if (best > 0.0) {
            r.percent.resize(size_t(cyl));
            for (int c = 0; c < cyl; ++c)
                r.percent[size_t(c)] = 100.0 * sum[size_t(c)] / best;
        }
    }
    return r;
}

// ---------------------------------------------------------------------------
// Crank and cam sensors
// ---------------------------------------------------------------------------

// 5th and 95th percentiles (linear time).
inline void percentiles595(const std::vector<double> &x, double *p5, double *p95)
{
    std::vector<double> w(x);
    const size_t i5 = w.size() * 5 / 100, i95 = w.size() * 95 / 100;
    std::nth_element(w.begin(), w.begin() + long(i95), w.end());
    *p95 = w[i95];
    std::nth_element(w.begin(), w.begin() + long(i5), w.begin() + long(i95));
    *p5 = w[i5];
}

struct CrankAnalysis {
    bool   valid = false;
    bool   tooFewSamples = false;   // teeth too close together to resolve here
    int    teeth = 0;               // wheel pattern: teeth - missing (60-2 ...)
    int    missing = 0;
    std::vector<double> edges;      // every tooth edge (rising)
    std::vector<size_t> gapIndex;   // gap = edges[g] .. edges[g + 1]
    std::vector<double> refs;       // first tooth after each gap: 0 crank degrees
    std::vector<double> rpm;        // one per complete revolution
    std::vector<double> faults;     // where the pattern broke (sample positions)
};

// Inductive (AC sine) and Hall (square) sensors alike: one rising crossing
// of the mid level per tooth, with hysteresis for noise. The gap is an
// interval at least 1.7x both neighbours; teeth between gaps plus the
// missing count gives the pattern (the most common one wins, so a single
// bad revolution doesn't change it). forcedTeeth/forcedMissing > 0 set the
// pattern instead of detecting it.
inline CrankAnalysis analyseCrank(const std::vector<double> &x, double sampleRate,
                                  int forcedTeeth = 0, int forcedMissing = 0)
{
    CrankAnalysis a;
    if (x.size() < 100 || sampleRate <= 0.0)
        return a;
    double p5 = 0.0, p95 = 0.0;
    percentiles595(x, &p5, &p95);
    const double range = p95 - p5;
    if (range <= 0.0)
        return a;
    a.edges = syncEdges(x, (p5 + p95) / 2.0, true, 0.2 * range, 1);
    const size_t ne = a.edges.size();
    if (ne < 20)
        return a;
    std::vector<double> d(ne - 1);
    for (size_t i = 0; i + 1 < ne; ++i)
        d[i] = a.edges[i + 1] - a.edges[i];
    std::vector<double> dm(d);
    std::nth_element(dm.begin(), dm.begin() + long(dm.size() / 2), dm.end());
    if (dm[dm.size() / 2] < 6.0) {
        a.tooFewSamples = true;
        return a;
    }

    std::vector<int> missingAt;
    for (size_t i = 1; i + 1 < d.size(); ++i) {
        if (d[i] > 1.7 * d[i - 1] && d[i] > 1.7 * d[i + 1]) {
            a.gapIndex.push_back(i);
            const double normal = (d[i - 1] + d[i + 1]) / 2.0;
            missingAt.push_back(std::max(1, int(std::lround(d[i] / normal)) - 1));
        }
    }
    if (a.gapIndex.size() < 2)
        return a;

    // Most common pattern across revolutions.
    std::vector<std::pair<int, int>> seen;   // (present, missing)
    for (size_t k = 0; k + 1 < a.gapIndex.size(); ++k)
        seen.push_back({ int(a.gapIndex[k + 1] - a.gapIndex[k]), missingAt[k + 1] });
    std::pair<int, int> best = seen[0];
    size_t bestCount = 0;
    for (const auto &c : seen) {
        const size_t count = size_t(std::count(seen.begin(), seen.end(), c));
        if (count > bestCount) {
            best = c;
            bestCount = count;
        }
    }
    a.missing = forcedTeeth > 0 ? forcedMissing : best.second;
    a.teeth = forcedTeeth > 0 ? forcedTeeth : best.first + best.second;
    const int present = a.teeth - a.missing;

    // Only gaps one revolution apart are the wheel's own gap. A broken or
    // missing tooth leaves a gap of its own mid-revolution; taken as real,
    // it split that revolution in two (a false spike in rpm). Seeded from a
    // revolution that has exactly the pattern's tooth count, then walked
    // both ways; a gap that comes early is reported as a fault instead.
    {
        const std::vector<size_t> cand = a.gapIndex;
        size_t seed = cand.size();
        for (size_t k = 0; k + 1 < cand.size(); ++k) {
            if (int(cand[k + 1] - cand[k]) == present) {
                seed = k;
                break;
            }
        }
        if (seed < cand.size()) {
            std::vector<size_t> kept { cand[seed] };
            for (size_t k = seed + 1; k < cand.size(); ++k) {
                const long step = long(cand[k]) - long(kept.back());
                if (step < present - 1)
                    a.faults.push_back(a.edges[cand[k]]);   // early: a tooth's gap
                else
                    kept.push_back(cand[k]);
            }
            for (size_t k = seed; k-- > 0;) {
                const long step = long(kept.front()) - long(cand[k]);
                if (step < present - 1)
                    a.faults.push_back(a.edges[cand[k]]);
                else
                    kept.insert(kept.begin(), cand[k]);
            }
            a.gapIndex = kept;
        }
    }
    if (a.gapIndex.size() < 2)
        return a;

    for (size_t g : a.gapIndex)
        a.refs.push_back(a.edges[g + 1]);
    for (size_t k = 0; k + 1 < a.gapIndex.size(); ++k) {
        a.rpm.push_back(60.0 * sampleRate / (a.refs[k + 1] - a.refs[k]));
        // A revolution with the wrong tooth count, or a tooth interval that
        // jumps against its neighbour (speed changes far more gently), is a
        // fault: damaged, missing or extra tooth, or a dropout.
        bool located = false;
        for (const double f : a.faults)
            located = located || (f > a.refs[k] && f < a.refs[k + 1]);
        for (size_t i = a.gapIndex[k] + 2; i < a.gapIndex[k + 1]; ++i) {
            const double r = d[i] / d[i - 1];
            if (r > 1.45 || r < 0.65) {
                a.faults.push_back(a.edges[i]);
                located = true;
            }
        }
        // Wrong tooth count with nothing pinpointing where: mark the
        // revolution itself.
        if (!located && int(a.gapIndex[k + 1] - a.gapIndex[k]) != present)
            a.faults.push_back((a.refs[k] + a.refs[k + 1]) / 2.0);
    }
    // One fault per place: a single bad tooth trips several checks around
    // it (its early gap, the jumps either side), which should read as one.
    std::sort(a.faults.begin(), a.faults.end());
    std::vector<double> merged;
    const double near = 3.0 * dm[dm.size() / 2];
    for (double f : a.faults) {
        if (merged.empty() || f - merged.back() > near)
            merged.push_back(f);
    }
    a.faults = merged;
    a.valid = true;
    return a;
}

// Crank angle (0..360, 0 = first tooth after the gap) at a sample position,
// interpolated between the actual tooth edges so it follows speed changes
// within a revolution. NaN outside every complete revolution.
inline double crankAngleAt(const CrankAnalysis &a, double s)
{
    if (!a.valid || a.refs.size() < 2)
        return std::nan("");
    const auto it = std::upper_bound(a.refs.begin(), a.refs.end(), s);
    const long k = long(it - a.refs.begin()) - 1;
    if (k < 0 || size_t(k) + 1 >= a.refs.size())
        return std::nan("");
    const double perTooth = 360.0 / a.teeth;
    const size_t first = a.gapIndex[size_t(k)] + 1;     // edge index of refs[k]
    const size_t last = a.gapIndex[size_t(k) + 1];      // last tooth before the next gap
    if (s >= a.edges[last]) {                           // across the gap
        const double a0 = double(last - first) * perTooth;
        return a0 + (360.0 - a0) * (s - a.edges[last]) / (a.refs[size_t(k) + 1] - a.edges[last]);
    }
    const auto e = std::upper_bound(a.edges.begin() + long(first), a.edges.begin() + long(last) + 1, s);
    const size_t j = size_t(e - a.edges.begin()) - 1;
    return (double(j - first) + (s - a.edges[j]) / (a.edges[j + 1] - a.edges[j])) * perTooth;
}

struct CamAnalysis {
    bool   valid = false;
    double angle = 0.0;          // cam reference edge, crank degrees after the gap
    double spread = 0.0;         // largest deviation from that across cycles
    int    count = 0;
    std::vector<double> cycleStarts;   // crank refs that start each 720-degree cycle
};

// The cam's reference edge — every rising edge for a one-pulse-per-cycle
// cam; for a multi-tooth cam, the edge after its longest gap — placed in
// crank degrees. The crank ref at the start of the revolution holding it
// starts each 720-degree engine cycle.
inline CamAnalysis analyseCam(const std::vector<double> &x, const CrankAnalysis &crank)
{
    CamAnalysis c;
    if (!crank.valid || x.size() < 100 || crank.refs.size() < 3)
        return c;
    double p5 = 0.0, p95 = 0.0;
    percentiles595(x, &p5, &p95);
    const double range = p95 - p5;
    if (range <= 0.0)
        return c;
    std::vector<double> edges = syncEdges(x, (p5 + p95) / 2.0, true, 0.2 * range, 1);
    if (edges.empty())
        return c;
    const double revolution = (crank.refs.back() - crank.refs.front()) / double(crank.refs.size() - 1);
    if (edges.size() >= 3 && (edges.back() - edges.front()) / double(edges.size() - 1) < 1.5 * revolution) {
        std::vector<double> ref;   // several edges per cycle: the one after the longest gap
        for (size_t i = 1; i + 1 < edges.size(); ++i) {
            const double before = edges[i] - edges[i - 1], after = edges[i + 1] - edges[i];
            if (before > 1.3 * after && (i < 2 || before > 1.3 * (edges[i - 1] - edges[i - 2])))
                ref.push_back(edges[i]);
        }
        edges = ref;
    }
    std::vector<double> angles;
    for (double e : edges) {
        const double ang = crankAngleAt(crank, e);
        if (std::isnan(ang))
            continue;
        angles.push_back(ang);
        const auto it = std::upper_bound(crank.refs.begin(), crank.refs.end(), e);
        const double start = *(it - 1);
        if (c.cycleStarts.empty() || start > c.cycleStarts.back())
            c.cycleStarts.push_back(start);
    }
    if (angles.empty())
        return c;
    // Circular mean, so 359 and 1 average to 0, not 180.
    double sx = 0.0, sy = 0.0;
    for (double a : angles) {
        sx += std::cos(a * M_PI / 180.0);
        sy += std::sin(a * M_PI / 180.0);
    }
    c.angle = std::atan2(sy, sx) * 180.0 / M_PI;
    if (c.angle < 0.0)
        c.angle += 360.0;
    for (double a : angles) {
        double dev = std::fabs(a - c.angle);
        c.spread = std::max(c.spread, std::min(dev, 360.0 - dev));
    }
    // Close the last cycle two revolutions on, when the capture has them.
    const auto it = std::find(crank.refs.begin(), crank.refs.end(), c.cycleStarts.back());
    if (it != crank.refs.end() && size_t(it - crank.refs.begin()) + 2 < crank.refs.size())
        c.cycleStarts.push_back(*(it + 2));
    c.count = int(angles.size());
    c.valid = true;
    return c;
}

// TDC calibration from a compression capture: each compression peak's crank
// angle, folded into one firing interval (all cylinders peak alike, so which
// is cylinder 1 can't be told here), circular-averaged. NaN if none.
inline double tdcFromCompression(const CrankAnalysis &crank, const std::vector<double> &peaks,
                                 double firingInterval)
{
    double sx = 0.0, sy = 0.0;
    int n = 0;
    for (double p : peaks) {
        const double ang = crankAngleAt(crank, p);
        if (std::isnan(ang))
            continue;
        const double phase = std::fmod(ang, firingInterval) / firingInterval * 2.0 * M_PI;
        sx += std::cos(phase);
        sy += std::sin(phase);
        ++n;
    }
    if (n == 0)
        return std::nan("");
    double phase = std::atan2(sy, sx);
    if (phase < 0.0)
        phase += 2.0 * M_PI;
    return phase / (2.0 * M_PI) * firingInterval;
}

} // namespace EngineTools
