#pragma once
//
// Battery cranking and alternator ripple analysis for the Automotive page's
// guided tests. Plain C++ with no Qt, tested standalone on simulated
// signals (a cranking battery; a three-phase bridge rectifier, healthy and
// with one open diode). MainWindow converts at the edges and applies the
// pass/fail limits.
//
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace BatteryTools {

// Centred moving average over `window` samples (O(n)).
inline std::vector<double> movingAverage(const std::vector<double> &x, size_t window)
{
    const size_t n = x.size();
    std::vector<double> y(n);
    if (n == 0)
        return y;
    window = std::max<size_t>(1, std::min(window, n));
    const size_t half = window / 2;
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + x[i];
    for (size_t i = 0; i < n; ++i) {
        const size_t a = i >= half ? i - half : 0;
        const size_t b = std::min(n, i + half + 1);
        y[i] = (prefix[b] - prefix[a]) / double(b - a);
    }
    return y;
}

inline double median(std::vector<double> v)
{
    if (v.empty())
        return 0.0;
    std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
    return v[v.size() / 2];
}

struct Cranking {
    bool   crankFound = false;   // a drop that looks like the starter engaging
    double resting = 0.0;        // before cranking, volts
    double minimum = 0.0;        // lowest while cranking (1 ms average): the starter's engagement dip
    double plateau = 0.0;        // typical voltage held while cranking (after the engagement dip)
    double sag = 0.0;            // how far the plateau fell from its first third to its last
    double crankSeconds = 0.0;   // starter engaged until it let go
    bool   crankedToEnd = false; // still cranking when the capture ended
    double rippleHz = 0.0;       // compression pulses per second while cranking (0: unclear)
    bool   started = false;      // engine running afterwards
    double running = 0.0;        // average over the last second, if started
    double noiseBefore = 0.0;    // supply noise (rms) before cranking ...
    double noiseAfter = 0.0;     // ... and after: a running engine adds ripple and ignition noise
    size_t startIndex = 0;       // where cranking began and ended, in samples
    size_t endIndex = 0;
};

namespace detail {
inline double meanOf(const std::vector<double> &v, size_t a, size_t b)
{
    b = std::min(b, v.size());
    if (b <= a)
        return 0.0;
    double s = 0.0;
    for (size_t i = a; i < b; ++i)
        s += v[i];
    return s / double(b - a);
}
inline double rmsDiff(const std::vector<double> &x, const std::vector<double> &y, size_t a, size_t b)
{
    b = std::min({ b, x.size(), y.size() });
    if (b <= a)
        return 0.0;
    double s = 0.0;
    for (size_t i = a; i < b; ++i)
        s += (x[i] - y[i]) * (x[i] - y[i]);
    return std::sqrt(s / double(b - a));
}
}

// The starter engaging is the first fall of more than 6% below resting. The
// crank ends when the 50 ms average is back to within 3% of resting and
// stays within 5% for half a second — the starter letting go, whether or
// not the engine caught. Started: afterwards the supply is clearly noisier
// than before (alternator ripple, ignition) or sits above resting. Requiring
// a rise above resting alone (the first version) failed on cars with smart
// charging, which hold the battery near 12.6-13 V after a start. Compression
// pulses while cranking are counted against a 0.5 s average (crossings with
// hysteresis); pulses per second x 120 / cylinders is the cranking speed.
inline Cranking analyseCranking(const std::vector<double> &v, double rate)
{
    Cranking r;
    if (v.size() < 100 || rate <= 0.0)
        return r;
    const auto samples = [&](double seconds) { return std::max<size_t>(1, size_t(seconds * rate)); };
    const std::vector<double> fine = movingAverage(v, samples(0.001));
    const std::vector<double> slow = movingAverage(v, samples(0.05));
    const size_t n = v.size();

    const size_t head = std::min(n / 4, samples(0.5));
    r.resting = median(std::vector<double>(slow.begin(), slow.begin() + long(std::max<size_t>(head, 1))));
    size_t start = 0;
    for (size_t i = head; i < n; ++i) {
        if (fine[i] < r.resting * 0.94) {
            start = i;
            r.crankFound = true;
            break;
        }
    }
    if (!r.crankFound) {
        r.resting = median(slow);
        return r;
    }
    const size_t restFrom = start > samples(1.0) ? start - samples(1.0) : 0;
    const size_t restTo = start > samples(0.05) ? start - samples(0.05) : start;
    if (restTo > restFrom)
        r.resting = median(std::vector<double>(slow.begin() + long(restFrom), slow.begin() + long(restTo)));
    r.noiseBefore = detail::rmsDiff(v, slow, restFrom, restTo);

    // End of cranking.
    size_t end = n;
    const size_t hold = samples(0.5);
    for (size_t i = start + samples(0.2); i < n; ++i) {
        if (slow[i] < r.resting * 0.97)
            continue;
        bool stays = true;
        for (size_t k = i; k < std::min(n, i + hold) && stays; ++k)
            stays = slow[k] >= r.resting * 0.95;
        if (stays && i + hold <= n) {
            end = i;
            break;
        }
    }
    r.crankedToEnd = end >= n;
    if (r.crankedToEnd)
        end = n;
    r.crankSeconds = double(end - start) / rate;
    r.startIndex = start;
    r.endIndex = end;

    const size_t window = std::min(n, start + samples(3.0));
    r.minimum = *std::min_element(fine.begin() + long(start), fine.begin() + long(std::min(window, end)));

    // The plateau, past the engagement dip (first 150 ms).
    const size_t p0 = std::min(end, start + samples(0.15));
    const size_t p1 = end > samples(0.05) ? std::max(p0, end - samples(0.05)) : p0;
    if (p1 > p0 + 10) {
        r.plateau = median(std::vector<double>(slow.begin() + long(p0), slow.begin() + long(p1)));
        const size_t third = (p1 - p0) / 3;
        r.sag = detail::meanOf(slow, p0, p0 + third) - detail::meanOf(slow, p1 - third, p1);

        // Compression pulses: 10 ms average against a 0.5 s one.
        const std::vector<double> pulse = movingAverage(v, samples(0.01));
        const std::vector<double> base = movingAverage(v, samples(0.5));
        std::vector<double> d;
        for (size_t i = p0; i < p1; ++i)
            d.push_back(pulse[i] - base[i]);
        std::vector<double> sorted(d);
        std::sort(sorted.begin(), sorted.end());
        const double spread = sorted[sorted.size() * 9 / 10] - sorted[sorted.size() / 10];
        if (spread > 0.0) {
            const double hyst = 0.3 * spread;
            std::vector<size_t> rises;
            bool armed = d[0] < -hyst / 2.0;
            for (size_t i = 1; i < d.size(); ++i) {
                if (d[i] < -hyst / 2.0) {
                    armed = true;
                } else if (armed && d[i] > hyst / 2.0) {
                    rises.push_back(i);
                    armed = false;
                }
            }
            if (rises.size() >= 3) {
                std::vector<double> gaps;
                for (size_t j = 1; j < rises.size(); ++j)
                    gaps.push_back(double(rises[j] - rises[j - 1]));
                const double period = median(gaps) / rate;
                if (period > 0.02)            // under 50 pulses/s: not noise
                    r.rippleHz = 1.0 / period;
            }
        }
    }

    // Running afterwards?
    if (!r.crankedToEnd) {
        const size_t a = std::min(n, end + samples(0.3));
        const size_t b = std::min(n, end + samples(2.0));
        if (b > a + 10) {
            r.noiseAfter = detail::rmsDiff(v, slow, a, b);
            const double after = detail::meanOf(slow, a, b);
            r.started = r.noiseAfter > 3.0 * std::max(r.noiseBefore, 1e-4)
                        || after > r.resting * 1.012;
        }
        if (r.started) {
            const size_t tail = std::min(n, samples(1.0));
            r.running = detail::meanOf(slow, n - tail, n);
        }
    }
    return r;
}

// --- Internal resistance (with a current clamp) ---------------------------

struct Resistance {
    bool   valid = false;
    double ohms = 0.0;
    double peakAmps = 0.0;       // starter engagement (1 ms average)
    double crankAmps = 0.0;      // typical while cranking
    double fitQuality = 0.0;     // r^2 of the voltage-current fit (0 when the fallback was used)
    bool   fromPulses = false;   // true: slope of V against I across the compression pulses
};

// Voltage and current recorded together at the battery. Each compression
// pulse swings the cranking current by tens to hundreds of amps and the
// terminal voltage dips in step, so the slope of V against I across the
// whole crank is the battery's internal resistance, measured hundreds of
// times over. When the pulses are too small to fit well (current swing under
// 20 A, or r^2 below 0.6), falls back to resting vs. cranking: (Vrest -
// Vcrank) / (Icrank - Irest). A clamp fitted the other way round gives
// negative current: the sign is taken from the cranking current itself.
inline Resistance internalResistance(const std::vector<double> &volts,
                                     const std::vector<double> &amps, double rate,
                                     const Cranking &c)
{
    Resistance r;
    const size_t n = std::min(volts.size(), amps.size());
    if (!c.crankFound || n < 100 || rate <= 0.0 || c.endIndex <= c.startIndex)
        return r;
    const size_t ms = std::max<size_t>(1, size_t(rate * 0.001));
    const std::vector<double> v = movingAverage(std::vector<double>(volts.begin(), volts.begin() + long(n)), ms);
    std::vector<double> i = movingAverage(std::vector<double>(amps.begin(), amps.begin() + long(n)), ms);

    const size_t start = std::min(c.startIndex, n - 1);
    const size_t end = std::min(c.endIndex, n);
    const size_t restFrom = start > size_t(rate) ? start - size_t(rate) : 0;
    const size_t restTo = start > size_t(rate * 0.05) ? start - size_t(rate * 0.05) : start;
    const double restV = detail::meanOf(v, restFrom, restTo);
    double restI = detail::meanOf(i, restFrom, restTo);

    const size_t p0 = std::min(end, start + size_t(rate * 0.15));   // past the engagement dip
    const size_t p1 = end;
    if (p1 <= p0 + 20 * ms)
        return r;
    double crankI = detail::meanOf(i, p0, p1);
    if (crankI - restI < 0.0) {          // clamp the other way round
        for (double &a : i)
            a = -a;
        restI = -restI;
        crankI = -crankI;
    }
    r.crankAmps = crankI;
    const size_t inrushTo = std::min(end, start + size_t(rate * 0.05));
    r.peakAmps = *std::max_element(i.begin() + long(start), i.begin() + long(std::max(inrushTo, start + 1)));

    // Fit V = a - R*I over the cranking plateau, one point per millisecond.
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;
    double lo = 1e300, hi = -1e300;
    int count = 0;
    for (size_t k = p0; k < p1; k += ms) {
        const double x = i[k], y = v[k];
        sx += x; sy += y; sxx += x * x; sxy += x * y; syy += y * y;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
        ++count;
    }
    if (count >= 20) {
        const double cov = sxy - sx * sy / count;
        const double varX = sxx - sx * sx / count;
        const double varY = syy - sy * sy / count;
        if (varX > 0.0 && varY > 0.0) {
            const double slope = cov / varX;
            const double r2 = cov * cov / (varX * varY);
            if (hi - lo >= 20.0 && r2 >= 0.6 && slope < 0.0) {
                r.ohms = -slope;
                r.fitQuality = r2;
                r.fromPulses = true;
                r.valid = true;
                return r;
            }
        }
    }
    const double crankV = detail::meanOf(v, p0, p1);
    if (crankI - restI > 20.0 && restV > crankV) {
        r.ohms = (restV - crankV) / (crankI - restI);
        r.valid = true;
    }
    return r;
}

// Cold cranking amps from internal resistance — the approximation battery
// testers use: the current at which the battery would sag to 7.2 V (the CCA
// definition's end voltage), (open-circuit voltage - 7.2) / R. 12 V basis;
// for 24 V (two in series) pass system-scaled values.
inline double estimatedCca(double restingVolts, double ohms, double system)
{
    if (ohms <= 0.0)
        return 0.0;
    const double k = system / 12.0;
    return std::max(0.0, (restingVolts - 7.2 * k) / ohms);
}

// --- Battery verdict ------------------------------------------------------

// State of charge from resting (open-circuit) voltage, 12 V lead-acid at
// ~25 C (BCI table); 24 V systems use the same per-cell figures doubled.
inline double stateOfCharge(double resting12)
{
    static const double v[] = { 11.51, 11.66, 11.81, 11.96, 12.10, 12.24, 12.37, 12.50, 12.62, 12.73 };
    if (resting12 <= v[0])
        return 10.0 * std::max(0.0, (resting12 - 11.36) / 0.15);
    for (int i = 1; i < 10; ++i) {
        if (resting12 <= v[i])
            return 10.0 * i + 10.0 * (resting12 - v[i - 1]) / (v[i] - v[i - 1]);
    }
    return 100.0;
}

// Minimum voltage a healthy battery holds under cranking load, by
// temperature (BCI load-test table; 9.6 V at 21 C and above).
inline double crankLimitAt(double celsius)
{
    static const double t[] = { -18.0, -12.0, -7.0, -1.0, 4.0, 10.0, 16.0, 21.0 };
    static const double v[] = { 8.5, 8.7, 8.9, 9.1, 9.3, 9.4, 9.5, 9.6 };
    if (celsius <= t[0])
        return v[0];
    for (int i = 1; i < 8; ++i) {
        if (celsius <= t[i])
            return v[i - 1] + (v[i] - v[i - 1]) * (celsius - t[i - 1]) / (t[i] - t[i - 1]);
    }
    return v[7];
}

struct VerdictLine {
    std::string text;
    bool        warn = false;
};
struct BatteryVerdict {
    std::string headline;
    bool        warn = false;
    std::vector<VerdictLine> lines;
};

// Judges the battery from the whole cranking curve, the way a workshop
// tester does, instead of one voltage limit:
//  - state of charge from resting voltage (a flat battery can't be judged:
//    charge and retest), with a surface-charge warning above a full
//    battery's resting voltage;
//  - the voltage it *holds* while cranking, against a temperature-adjusted
//    limit (the user's 21 C limit, shifted by the BCI table);
//  - the starter's engagement dip, separately (a deep one with an otherwise
//    healthy curve points at terminals and cables);
//  - sag across the crank (losing capacity);
//  - cranking speed (slow at a healthy voltage: starter, cables or earth).
// system: 12 or 24. cylinders <= 0: speed reported as pulses per second.
// With a current clamp: internal resistance (12 V: up to 10 mOhm good,
// 10-15 marginal, above 15 worn; doubled for 24 V's two in series) and, with
// the battery's rated CCA, the estimated CCA as a share of it (under 80 %
// losing capacity, under 60 % weak). Pass nullptr / 0 without them.
inline BatteryVerdict judgeBattery(const Cranking &r, double system, double celsius, int cylinders,
                                   double restingGood, double crankMinAt21,
                                   double chargeLow, double chargeHigh,
                                   const Resistance *resistance = nullptr, double ratedCca = 0.0)
{
    BatteryVerdict v;
    const double k = system / 12.0;
    char buf[200];
    const auto add = [&](bool warn, const char *fmt, auto... args) {
        if constexpr (sizeof...(args) == 0) {
            v.lines.push_back({ fmt, warn });
        } else {
            std::snprintf(buf, sizeof buf, fmt, args...);
            v.lines.push_back({ buf, warn });
        }
    };

    const double soc = stateOfCharge(r.resting / k);
    const bool discharged = r.resting < restingGood;
    add(discharged, "Resting %.2f V, about %.0f %% charged", r.resting, soc);
    if (r.resting / k > 12.9)
        add(true, "Above a full battery's resting voltage: surface charge \u2014 "
                  "rest it or run the headlights a minute, then retest");
    if (!r.crankFound) {
        v.headline = "No cranking found \u2014 the voltage never dropped";
        v.warn = true;
        return v;
    }

    const double limit = crankMinAt21 + (crankLimitAt(celsius) - 9.6) * k;
    const bool weak = r.plateau < limit;
    add(weak, "Held %.2f V while cranking (at least %.2f V at %.0f \u00b0C)", r.plateau, limit, celsius);
    const bool deepDip = r.minimum < 7.0 * k;
    add(deepDip, "Dipped to %.2f V as the starter engaged", r.minimum);
    const bool sagging = r.sag > 0.3 * k;
    if (sagging)
        add(true, "Fell %.2f V during the crank: can't sustain the load", r.sag);
    else
        add(false, "Steady under load (%.2f V change)", r.sag);

    double rpm = 0.0;
    if (r.rippleHz > 0.0 && cylinders > 0) {
        rpm = r.rippleHz * 120.0 / cylinders;
        add(rpm < 150.0, "Cranking speed about %.0f rpm (%d cylinders)", rpm, cylinders);
    } else if (r.rippleHz > 0.0) {
        add(false, "%.1f compression pulses per second (set the cylinders for rpm)", r.rippleHz);
    }
    const bool slow = rpm > 0.0 && rpm < 150.0;

    // Internal resistance and CCA (current clamp).
    bool highR = false, veryHighR = false, lowCca = false, veryLowCca = false;
    if (resistance && resistance->valid) {
        const double mOhm = resistance->ohms * 1000.0;
        highR = mOhm > 10.0 * k;
        veryHighR = mOhm > 15.0 * k;
        add(highR, "Internal resistance %.1f m\u03a9 (%s)", mOhm,
            veryHighR ? "worn" : highR ? "marginal" : "good");
        add(false, "Cranking current %.0f A, %.0f A at engagement",
            resistance->crankAmps, resistance->peakAmps);
        const double cca = estimatedCca(r.resting, resistance->ohms, system);
        if (ratedCca > 0.0) {
            const double share = cca / ratedCca;
            lowCca = share < 0.8;
            veryLowCca = share < 0.6;
            add(lowCca, "About %.0f CCA \u2014 %.0f %% of its %.0f A rating", cca, share * 100.0, ratedCca);
        } else {
            add(false, "About %.0f CCA (enter the rated CCA to compare)", cca);
        }
    } else if (resistance) {
        add(true, "Internal resistance: couldn't measure (no clear cranking current on CH2)");
    }

    if (r.started)
        add(false, "Cranked %.2f s, then started", r.crankSeconds);
    else if (r.crankedToEnd)
        add(true, "Still cranking when the capture ended (%.1f s)", r.crankSeconds);
    else
        add(true, "Didn't start (cranked %.2f s)", r.crankSeconds);
    if (r.started && r.running > 0.0) {
        // Smart-charging cars hold the battery near its resting voltage after
        // a start — normal, not a fault. Only below resting (draining with
        // the engine running) is.
        if (r.running < r.resting - 0.05 * k)
            add(true, "Running at %.2f V, below resting: not charging \u2014 run the charging test",
                r.running);
        else if (r.running < chargeLow)
            add(false, "Running at %.2f V: gentle charging, normal on smart-charging cars "
                       "(the charging test checks the alternator)", r.running);
        else if (r.running > chargeHigh)
            add(true, "Charging %.2f V \u2014 high: regulator overcharging", r.running);
        else
            add(false, "Charging %.2f V \u2014 OK", r.running);
    }

    v.warn = true;
    if (discharged)
        v.headline = "Discharged \u2014 charge it and test again (a flat battery can't be judged)";
    else if (weak)
        v.headline = "Weak battery \u2014 can't hold its voltage under load: replace";
    else if (veryHighR || veryLowCca)
        v.headline = "Worn battery \u2014 high internal resistance: replace";
    else if (sagging || highR || lowCca)
        v.headline = sagging ? "Losing capacity \u2014 sags under sustained load: replace soon"
                             : "Ageing battery \u2014 internal resistance rising: replace soon";
    else if (slow && r.plateau >= limit + 0.5 * k)
        v.headline = "Battery OK \u2014 slow cranking at good voltage points to the starter, "
                     "cables or earth";
    else if (deepDip)
        v.headline = "Battery OK, but a deep dip at engagement \u2014 check terminals and cables";
    else {
        v.headline = "Battery healthy";
        v.warn = false;
    }
    return v;
}

// --- Parasitic drain (current clamp, long recording) ------------------------

struct Wake {
    double startSeconds = 0.0;
    double seconds = 0.0;
    double peakAmps = 0.0;
};
struct Drain {
    bool   valid = false;
    double initialAmps = 0.0;     // first 10 s: with the car just locked
    double sleepAmps = 0.0;       // settled level (median of the last tenth, >= 1 min)
    bool   settled = false;       // a settled stretch was found
    double sleepAfterSeconds = 0.0;
    std::vector<Wake> wakes;      // after settling
    double ampHours = 0.0;        // charge drawn over the whole test
    double seconds = 0.0;
};

// meanAmps / peakAmps: one value per second (average and highest 1 ms
// average). The settled level is the median of the last tenth of the test
// (at least a minute). Settled from: the first moment the following five
// minutes (or the rest of the test) stay within 1.5x that level + 10 mA,
// allowing 10 % of seconds above it (brief blips). Wake-ups after settling:
// runs above level + max(50 mA, 2x level), merged when under 3 s apart.
inline Drain analyseDrain(const std::vector<double> &meanAmps, const std::vector<double> &peakAmps)
{
    Drain d;
    const size_t n = meanAmps.size();
    if (n < 60)
        return d;
    d.valid = true;
    d.seconds = double(n);
    d.initialAmps = median(std::vector<double>(meanAmps.begin(), meanAmps.begin() + long(std::min<size_t>(10, n))));
    const size_t tail = std::max<size_t>(60, n / 10);
    d.sleepAmps = median(std::vector<double>(meanAmps.end() - long(std::min(tail, n)), meanAmps.end()));
    for (double a : meanAmps)
        d.ampHours += a / 3600.0;

    const double settleLimit = d.sleepAmps * 1.5 + 0.010;
    // Prefix count of seconds above the settle limit, for O(1) window checks.
    std::vector<size_t> above(n + 1, 0);
    for (size_t i = 0; i < n; ++i)
        above[i + 1] = above[i] + (meanAmps[i] > settleLimit ? 1 : 0);
    for (size_t t = 0; t + 30 <= n; ++t) {
        const size_t e = std::min(n, t + 300);
        if (meanAmps[t] > settleLimit)
            continue;
        if (double(above[e] - above[t]) <= 0.10 * double(e - t)) {
            d.settled = true;
            d.sleepAfterSeconds = double(t);
            break;
        }
    }
    if (!d.settled)
        return d;

    const double wakeLevel = d.sleepAmps + std::max(0.050, 2.0 * d.sleepAmps);
    size_t i = size_t(d.sleepAfterSeconds);
    while (i < n) {
        if (meanAmps[i] <= wakeLevel && (i >= peakAmps.size() || peakAmps[i] <= wakeLevel)) {
            ++i;
            continue;
        }
        Wake w;
        w.startSeconds = double(i);
        size_t last = i;
        double peak = 0.0;
        for (size_t j = i; j < n && j <= last + 3; ++j) {
            const double pk = j < peakAmps.size() ? peakAmps[j] : meanAmps[j];
            if (meanAmps[j] > wakeLevel || pk > wakeLevel) {
                last = j;
                peak = std::max(peak, pk);
            }
        }
        w.seconds = double(last - i + 1);
        w.peakAmps = peak;
        d.wakes.push_back(w);
        i = last + 1;
    }
    return d;
}

// Verdict: settled level against the acceptable drain (most cars: 20-50 mA
// once asleep), how long the modules took to sleep, wake-ups, and how many
// days the battery would last before it's too flat to start (half its
// capacity: below ~50 % a lead-acid battery struggles to crank).
inline BatteryVerdict judgeDrain(const Drain &d, double limitAmps, double capacityAh)
{
    BatteryVerdict v;
    char buf[200];
    const auto add = [&](bool warn, const char *fmt, auto... args) {
        if constexpr (sizeof...(args) == 0) {
            v.lines.push_back({ fmt, warn });
        } else {
            std::snprintf(buf, sizeof buf, fmt, args...);
            v.lines.push_back({ buf, warn });
        }
    };
    const auto clock = [](double s) {
        char b[32];
        const int m = int(s) / 60, sec = int(s) % 60;
        if (m >= 60) std::snprintf(b, sizeof b, "%d h %02d min", m / 60, m % 60);
        else std::snprintf(b, sizeof b, "%d min %02d s", m, sec);
        return std::string(b);
    };
    if (!d.valid) {
        v.headline = "Too short to judge \u2014 record at least a few minutes";
        v.warn = true;
        return v;
    }
    const bool tooHigh = d.sleepAmps > limitAmps;
    // A sleeping car draws far less than just after locking (typically 5-50x
    // less). High current that never fell well below the locking level is
    // a car that never slept — not one that "settled" at once (a flat
    // 0.6 A trace looked settled from the first second).
    const bool neverDropped = tooHigh && d.sleepAmps > 0.6 * d.initialAmps;
    const bool asleep = d.settled && !neverDropped;
    add(tooHigh, "%s drain %.0f mA (acceptable up to %.0f mA)", asleep ? "Settled" : "Current",
        d.sleepAmps * 1000.0, limitAmps * 1000.0);
    add(false, "Just after locking %.2f A", d.initialAmps);
    if (asleep)
        add(false, "Modules settled after %s", clock(d.sleepAfterSeconds).c_str());
    else
        add(true, "Never dropped to a sleep level in %s", clock(d.seconds).c_str());
    if (asleep && !d.wakes.empty()) {
        add(true, "%d wake-up%s after settling:", int(d.wakes.size()), d.wakes.size() == 1 ? "" : "s");
        for (size_t i = 0; i < d.wakes.size() && i < 6; ++i) {
            const Wake &w = d.wakes[i];
            add(true, "  at %s: %.0f s, up to %.2f A", clock(w.startSeconds).c_str(), w.seconds, w.peakAmps);
        }
        if (d.wakes.size() > 6)
            add(true, "  \u2026 %d more", int(d.wakes.size() - 6));
    } else if (asleep) {
        add(false, "No wake-ups after settling");
    }
    if (d.sleepAmps > 0.0005 && capacityAh > 0.0) {
        const double days = 0.5 * capacityAh / (d.sleepAmps * 24.0);
        add(days < 14.0, "A %.0f Ah battery would be too flat to start in about %.0f days",
            capacityAh, days);
    }
    add(false, "Charge used during the test %.2f Ah", d.ampHours);

    v.warn = true;
    if (!asleep && d.seconds < 3600.0)
        v.headline = "Still awake \u2014 some cars take up to an hour to sleep: run longer";
    else if (!asleep)
        v.headline = "Never went to sleep \u2014 a module or circuit is keeping the car awake";
    else if (tooHigh)
        v.headline = "Excessive drain while asleep \u2014 find the circuit (pull fuses one at a time)";
    else if (d.wakes.size() >= 3)
        v.headline = "Drain OK when asleep, but something keeps waking the car";
    else {
        v.headline = "Drain OK";
        v.warn = false;
    }
    return v;
}

struct Ripple {
    bool   valid = false;
    double vpp = 0.0;            // robust peak-to-peak (0.5th..99.5th percentile)
    double frequency = 0.0;      // humps per second
    double humpSpread = 0.0;     // hump heights' coefficient of variation
    double timingSpread = 0.0;   // hump intervals' coefficient of variation
    // The same, robust (1.4826 x MAD / median): occasional disturbances —
    // ignition / injector dips on a running engine — barely move them, where
    // the plain spreads above called a healthy alternator uneven.
    double humpSpreadRobust = 0.0;
    double timingSpreadRobust = 0.0;
    int    faultEvery = 0;       // 0, or every Nth hump is consistently off (3 or 6)
    double faultDepth = 0.0;     // how far that hump is from the rest, fraction
};

// A healthy three-phase alternator leaves six even humps per electrical
// revolution. An open or shorted diode removes or shrinks one phase's
// contribution, which repeats every 3rd or 6th hump: humps become uneven
// in height and spacing, with one position consistently off. Humps are
// counted between real dips (see below); each hump's height is its max
// above the dips either side.
inline Ripple analyseRipple(const std::vector<double> &x, double rate)
{
    Ripple r;
    if (x.size() < 200 || rate <= 0.0)
        return r;
    std::vector<double> w(x);
    const size_t lo = w.size() / 200, hi = w.size() - 1 - w.size() / 200;
    std::nth_element(w.begin(), w.begin() + long(hi), w.end());
    const double top = w[hi];
    std::nth_element(w.begin(), w.begin() + long(lo), w.begin() + long(hi));
    const double bottom = w[lo];
    r.vpp = top - bottom;
    if (r.vpp <= 0.0)
        return r;
    // Humps are counted between dips: local minima at least 8% of the
    // ripple deep relative to their surroundings. Counting crossings of the
    // average instead failed on exactly the fault that matters: an open
    // diode leaves one big dip per electrical cycle, so the average-crossing
    // count saw one huge "even" hump instead of the small humps and the gap.
    // The window is short against any real hump (alternator ripple runs
    // from a few hundred to several thousand humps per second).
    const size_t win = std::max<size_t>(2, size_t(rate / 12000.0));
    const std::vector<double> s = movingAverage(x, std::max<size_t>(1, win / 3));
    std::vector<size_t> rises;   // dip positions
    for (size_t i = win; i + win < s.size(); ++i) {
        bool isMin = true;
        for (size_t k = i - win; k <= i + win && isMin; ++k)
            isMin = s[k] >= s[i];
        if (!isMin)
            continue;
        double around = s[i];
        for (size_t k = (i > 3 * win ? i - 3 * win : 0); k < std::min(s.size(), i + 3 * win + 1); ++k)
            around = std::max(around, s[k]);
        if (around - s[i] < 0.08 * r.vpp)
            continue;
        if (!rises.empty() && i - rises.back() <= win) {
            if (s[i] < s[rises.back()])
                rises.back() = i;
            continue;
        }
        rises.push_back(i);
    }
    if (rises.size() < 8)
        return r;

    std::vector<double> gaps, heights;
    for (size_t j = 1; j < rises.size(); ++j) {
        gaps.push_back(double(rises[j] - rises[j - 1]));
        double mx = s[rises[j - 1]];
        for (size_t i = rises[j - 1]; i < rises[j]; ++i)
            mx = std::max(mx, s[i]);
        heights.push_back(mx - (s[rises[j - 1]] + s[rises[j]]) / 2.0);
    }
    const auto cv = [](const std::vector<double> &v) {
        double m = 0.0, s = 0.0;
        for (double a : v) m += a;
        m /= double(v.size());
        for (double a : v) s += (a - m) * (a - m);
        return m > 0.0 ? std::sqrt(s / double(v.size())) / m : 0.0;
    };
    r.frequency = rate / median(gaps);
    r.humpSpread = cv(heights);
    r.timingSpread = cv(gaps);
    const auto robustCv = [&](const std::vector<double> &v) {
        const double m = median(v);
        std::vector<double> d;
        d.reserve(v.size());
        for (double a : v)
            d.push_back(std::fabs(a - m));
        return m > 0.0 ? 1.4826 * median(d) / m : 0.0;
    };
    r.humpSpreadRobust = robustCv(heights);
    r.timingSpreadRobust = robustCv(gaps);

    // A position that is consistently off, repeating every 3 or 6 humps —
    // within runs of regularly spaced humps. A disturbance (an ignition dip)
    // adds a "hump" and shifts the count, which hid an open diode's pattern
    // entirely; it also shows as irregular spacing, while a bad diode leaves
    // the spacing regular and only changes one hump's height. So the humps
    // are split at spacing outliers and each run is checked on its own.
    const double typicalGap = median(gaps);
    std::vector<std::pair<size_t, size_t>> runs;   // [first, last) hump indices
    size_t runStart = 0;
    for (size_t j = 0; j <= heights.size(); ++j) {
        const bool broken = j == heights.size() || std::fabs(gaps[j] - typicalGap) > 0.25 * typicalGap;
        if (broken) {
            if (j > runStart)
                runs.emplace_back(runStart, j);
            runStart = j + 1;
        }
    }
    for (int every : { 3, 6 }) {
        double weighted = 0.0, weight = 0.0;
        for (const auto &[first, last] : runs) {
            const size_t n = last - first;
            if (n < size_t(every) * 3)
                continue;
            std::vector<double> sum(size_t(every), 0.0);
            std::vector<int> count(size_t(every), 0);
            double overall = 0.0;
            for (size_t j = first; j < last; ++j) {
                sum[(j - first) % size_t(every)] += heights[j];
                ++count[(j - first) % size_t(every)];
                overall += heights[j];
            }
            overall /= double(n);
            if (overall <= 0.0)
                continue;
            double worst = 0.0;
            for (int p = 0; p < every; ++p)
                worst = std::max(worst, std::fabs(sum[size_t(p)] / count[size_t(p)] - overall) / overall);
            weighted += worst * double(n);
            weight += double(n);
        }
        const double depth = weight > 0.0 ? weighted / weight : 0.0;
        if (depth > 0.25 && depth > r.faultDepth + 0.05) {
            r.faultEvery = every;
            r.faultDepth = depth;
        }
    }
    r.valid = true;
    return r;
}

} // namespace BatteryTools
