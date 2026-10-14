#include "spectrumview.h"
#include "fonts.h"

#include "scopeview.h"
#include "theme.h"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {

// The graticule's fixed dB span: -100 keeps the noise floor readable
// without wasting most of the plot on levels nobody looks at, +20 gives
// room above 0 dBV for a signal larger than 1 V peak.
constexpr double kMinDb = -100.0;
constexpr double kMaxDb = 20.0;
// Linear mode's own fixed span, in peak volts.
constexpr double kMinLinearVolts = 0.0;
constexpr double kMaxLinearVolts = 2.0;
// Matches ScopeView's own kTopGutter exactly (scopeview.cpp) — used both to
// size the plot and to place the unit label with a clean gap above the
// topmost tick, mirroring drawVoltScale()'s own unit-box sizing.
constexpr double kTopGutter = 28.0;

// Not M_PI: that is a POSIX/GNU extension to <cmath>, not standard C++17,
// and relies on GNU extensions being enabled (CMake's default, but not
// guaranteed) — this needs nothing but the standard library.
constexpr double kPi = 3.14159265358979323846;

QString formatFrequency(double hz)
{
    if (hz >= 1e6)
        return QStringLiteral("%1 MHz").arg(hz / 1e6, 0, 'g', 3);
    if (hz >= 1e3)
        return QStringLiteral("%1 kHz").arg(hz / 1e3, 0, 'g', 3);
    return QStringLiteral("%1 Hz").arg(hz, 0, 'f', 0);
}

}   // namespace

SpectrumView::SpectrumView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(400, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    m_fftIn  = fftw_alloc_real(FftSize);
    m_fftOut = fftw_alloc_complex(FftSize / 2 + 1);
    // FFTW_MEASURE tries a few real transforms to pick the fastest strategy
    // for this exact size, overwriting m_fftIn while it does — harmless,
    // nothing meaningful is in it yet, and every real setTraceSamples()
    // call fills it fresh before use. FftSize is fixed, so this plan is
    // built exactly once and reused, for every trace, for the widget's
    // whole lifetime.
    m_plan = fftw_plan_dft_r2c_1d(FftSize, m_fftIn, m_fftOut, FFTW_MEASURE);

    rebuildWindow();
}

SpectrumView::~SpectrumView()
{
    if (m_plan)
        fftw_destroy_plan(m_plan);
    if (m_fftIn)
        fftw_free(m_fftIn);
    if (m_fftOut)
        fftw_free(m_fftOut);
}

void SpectrumView::setWindow(WindowFunction window)
{
    if (m_window == window)
        return;
    m_window = window;
    rebuildWindow();
    update();
}

void SpectrumView::rebuildWindow()
{
    m_windowTable.coefficients.resize(FftSize);
    double *w = m_windowTable.coefficients.data();
    const double N = double(FftSize);

    // All four in the "periodic" form (argument 2*pi*n/N, not N-1) rather
    // than the "symmetric" form filter design uses: periodic is what
    // spectral analysis wants, since it matches the periodic extension the
    // DFT itself assumes of the input block.
    switch (m_window) {
    case WindowFunction::Hann:
        for (int n = 0; n < FftSize; ++n)
            w[n] = 0.5 - 0.5 * std::cos(2.0 * kPi * n / N);
        break;
    case WindowFunction::Hamming:
        for (int n = 0; n < FftSize; ++n)
            w[n] = 0.54 - 0.46 * std::cos(2.0 * kPi * n / N);
        break;
    case WindowFunction::Blackman:
        for (int n = 0; n < FftSize; ++n) {
            w[n] = 0.42
                 - 0.50 * std::cos(2.0 * kPi * n / N)
                 + 0.08 * std::cos(4.0 * kPi * n / N);
        }
        break;
    case WindowFunction::FlatTop: {
        // The standard 5-term flat-top window (MATLAB's flattopwin; the
        // same coefficients are published by SRS and NI) — picked
        // specifically for amplitude accuracy: its main lobe is unusually
        // flat, so a tone that does not land exactly on a bin centre still
        // reads close to its true amplitude, at the cost of a much wider
        // main lobe (worse frequency resolution) than the other three.
        constexpr double a0 = 0.21557895, a1 = 0.41663158, a2 = 0.277263158,
                          a3 = 0.083578947, a4 = 0.006947368;
        for (int n = 0; n < FftSize; ++n) {
            const double x = 2.0 * kPi * n / N;
            w[n] = a0 - a1 * std::cos(x)       + a2 * std::cos(2.0 * x)
                      - a3 * std::cos(3.0 * x) + a4 * std::cos(4.0 * x);
        }
        break;
    }
    }

    double sum = 0.0;
    for (double c : m_windowTable.coefficients)
        sum += c;
    m_windowTable.coherentGain = sum / N;
}

void SpectrumView::setUnit(Unit unit)
{
    if (m_unit == unit)
        return;
    m_unit = unit;
    update();   // bins are stored linear; only the paint-time conversion changes
}

void SpectrumView::setYScale(YScale scale)
{
    if (m_yScale == scale)
        return;
    m_yScale = scale;
    update();
}

void SpectrumView::setDisplayMode(DisplayMode mode)
{
    if (m_displayMode == mode)
        return;
    m_displayMode = mode;
    update();
}

void SpectrumView::setRulersEnabled(bool enabled)
{
    if (m_rulersEnabled == enabled)
        return;
    m_rulersEnabled = enabled;
    if (!enabled)
        m_rulerDrag = RulerDrag::None;   // abandon any in-progress drag
    update();
}

void SpectrumView::setTraceActive(int slot, bool active)
{
    if (slot < 0 || slot >= MaxTraces)
        return;
    Trace &trace = m_traces[slot];
    trace.active = active;
    if (!active) {
        trace.hasSpectrum = false;
        trace.bins.clear();
        trace.avgBins.clear();
    }
    update();
}

void SpectrumView::setTraceColor(int slot, const QColor &color)
{
    if (slot < 0 || slot >= MaxTraces)
        return;
    m_traces[slot].color = color;
    update();
}

void SpectrumView::clearAll()
{
    for (Trace &trace : m_traces) {
        trace.active      = false;
        trace.hasSpectrum = false;
        trace.bins.clear();
        trace.avgBins.clear();
    }
    update();
}

void SpectrumView::setTraceSamples(int slot, const QVector<double> &samples, double sampleRate)
{
    if (slot < 0 || slot >= MaxTraces)
        return;
    Trace &trace = m_traces[slot];
    if (!trace.active)
        return;

    trace.sampleRate = sampleRate;

    // First time any active trace's real sample rate is known, seed the
    // ruler cursors to something sensible on the current scale — there is
    // nothing sane to seed a frequency cursor to before this.
    if (m_rulersEnabled && !m_rulersSeeded && sampleRate > 0.0) {
        const double nyquist = sampleRate / 2.0;
        m_freq1  = nyquist * 0.25;
        m_freq2  = nyquist * 0.75;
        m_level1 = plotFractionToValue(0.75);
        m_level2 = plotFractionToValue(0.25);
        m_rulersSeeded = true;
    }

    if (samples.size() < FftSize || sampleRate <= 0.0) {
        trace.hasSpectrum = false;
        update();
        return;
    }

    // The newest FftSize samples — the same "anchored to the newest data"
    // treatment Live already gives the main plot, and simply the most
    // recent slice of the buffer in Capture/Record when it is longer than
    // this.
    const double *tail = samples.constData() + (samples.size() - FftSize);
    for (int n = 0; n < FftSize; ++n)
        m_fftIn[n] = tail[n] * m_windowTable.coefficients[n];

    fftw_execute(m_plan);

    // Converts the raw DFT output into peak-equivalent volts for each
    // frequency bin. Full derivation in HANDOFF.md; in short:
    //   - dividing by FftSize is the DFT's own 1/N normalization;
    //   - dividing by coherentGain undoes the window's own average
    //     attenuation, which is what makes a bin-centred tone read its true
    //     amplitude regardless of which window is applied — the whole
    //     point of "normalize by coherent gain" in the first place;
    //   - multiplying by 2 reconstructs the energy a one-sided spectrum
    //     would otherwise split between +f and -f. DC, and Nyquist when
    //     FftSize is even, are their own unmirrored bin and never get it.
    const int bins = FftSize / 2 + 1;
    trace.bins.resize(bins);
    const double norm = 1.0 / (FftSize * m_windowTable.coherentGain);
    for (int k = 0; k < bins; ++k) {
        const double re  = m_fftOut[k][0];
        const double im  = m_fftOut[k][1];
        const double mag = std::sqrt(re * re + im * im);
        const bool mirrored = (k != 0) && !(FftSize % 2 == 0 && k == bins - 1);
        trace.bins[k] = mag * norm * (mirrored ? 2.0 : 1.0);
    }

    // Exponential moving average, maintained regardless of the current
    // DisplayMode so switching TO Average shows an already-warmed-up trace
    // rather than starting from nothing.
    constexpr double kAlpha = 0.25;
    if (trace.avgBins.size() != trace.bins.size()) {
        trace.avgBins = trace.bins;   // first data, or a size change: start clean
    } else {
        for (int k = 0; k < bins; ++k)
            trace.avgBins[k] = kAlpha * trace.bins[k] + (1.0 - kAlpha) * trace.avgBins[k];
    }

    trace.hasSpectrum = true;
    update();
}

double SpectrumView::toDb(double volts) const
{
    // Floor avoids log10(0); -180 dBV is already off the bottom of the
    // graticule, so nothing visible is affected by exactly where it sits.
    constexpr double kFloorVolts = 1e-9;
    const double v = std::max(volts, kFloorVolts);

    // dBV here means 20*log10(V_peak / 1 V) — peak-referenced, not the
    // audio-engineering convention of referencing dBV to 1 V RMS. This is
    // deliberate: volts is already the peak-equivalent amplitude the
    // coherent-gain normalization above reconstructs, and peak reference is
    // what makes a 1 V PEAK sine read exactly 0 dBV — the actual, testable
    // target this feature was built against.
    const double dbv = 20.0 * std::log10(v);
    if (m_unit == Unit::DBV)
        return dbv;

    // dBm assumes a 50 ohm reference load (the RF/spectrum-analyzer
    // convention, applied regardless of this instrument's real, much
    // higher, input impedance — the same convention PicoScope and every
    // other bench scope's spectrum mode uses for dBm).
    //   P = Vrms^2 / R,  Vrms = Vpeak / sqrt(2)
    //   dBm = 10*log10(P / 1 mW) = 20*log10(Vpeak) - 10*log10(2R) + 30
    // With R = 50 ohm: -10*log10(100) + 30 = -20 + 30 = +10.0 dB exactly.
    // NOT the textbook "dBm = dBV + 13.01" offset — that constant assumes
    // dBV is already RMS-referenced, which this one deliberately is not;
    // the extra 3.01 dB of peak-to-RMS conversion is already folded into
    // this +10.0 instead.
    return dbv + 10.0;
}

double SpectrumView::valueToPlotFraction(double volts) const
{
    if (m_yScale == YScale::Linear) {
        const double v = std::clamp(volts, kMinLinearVolts, kMaxLinearVolts);
        return (v - kMinLinearVolts) / (kMaxLinearVolts - kMinLinearVolts);
    }
    const double db = std::clamp(toDb(volts), kMinDb, kMaxDb);
    return (db - kMinDb) / (kMaxDb - kMinDb);
}

double SpectrumView::plotFractionToValue(double fraction) const
{
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (m_yScale == YScale::Linear)
        return kMinLinearVolts + fraction * (kMaxLinearVolts - kMinLinearVolts);
    // Inverts toDb()'s own Log branch, so a level cursor dragged while
    // viewing dBm still ends up storing a plain linear peak-volts value
    // internally, exactly like a trace's own bins do.
    const double db  = kMinDb + fraction * (kMaxDb - kMinDb);
    const double dbv = (m_unit == Unit::DBm) ? db - 10.0 : db;
    return std::pow(10.0, dbv / 20.0);
}

QRectF SpectrumView::plotRect() const
{
    // Gutters swapped from a plain left/right guess to match ScopeView's
    // own convention: a single channel's voltage scale renders on the
    // RIGHT (see drawVoltScale()'s "channel 1, right side" case, which is
    // what actually shows whenever only one channel is on) — the Y scale
    // does the same here, in the common case of a single active trace.
    constexpr double kLeftGutter = 10.0, kRightGutter = 54.0;
    constexpr double kBottomGutter = 20.0;
    return QRectF(kLeftGutter, kTopGutter,
                 std::max(10.0, width() - kLeftGutter - kRightGutter),
                 std::max(10.0, height() - kTopGutter - kBottomGutter));
}

void SpectrumView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath background;
    background.addRoundedRect(rect(), Theme::cornerRadius(), Theme::cornerRadius());
    p.setClipPath(background);
    p.fillPath(background, Theme::plotBackground());

    const QRectF plot = plotRect();
    drawGraticule(p, plot);

    bool any = false;
    for (const Trace &trace : m_traces) {
        if (trace.active && trace.hasSpectrum) {
            drawTrace(p, plot, trace);
            any = true;
        }
    }
    if (!any) {
        p.setPen(Theme::plotAxisText());
        p.drawText(plot, Qt::AlignCenter,
                  QStringLiteral("Waiting for %1 samples...").arg(FftSize));
    }

    drawRulers(p, plot);
}

void SpectrumView::drawGraticule(QPainter &p, const QRectF &plot) const
{
    // Horizontal divisions always match ScopeView's own HorizontalDivs.
    // Vertical stays at ScopeView's own VerticalDivs (10) UNLESS the pane
    // is short enough that 10 evenly spaced rows would be cramped against
    // typical label/font size, in which case 8 stays legible where 10
    // would not — same reasoning a real graticule would use. The 220 px
    // threshold is a plot-height judgement call, not a derived constant:
    // it is roughly where 10 rows' worth of dotted lines start reading as
    // a smear rather than a grid.
    const int divsX = ScopeView::HorizontalDivs;
    constexpr double kShortPlotThreshold = 220.0;
    const int divsY = (plot.height() < kShortPlotThreshold) ? 8 : ScopeView::VerticalDivs;

    // Minor dotted gridlines.
    p.setPen(QPen(Theme::plotGridMinor(), 1.0, Qt::DotLine));
    for (int i = 1; i < divsX; ++i) {
        const double x = plot.left() + plot.width() * i / divsX;
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    for (int i = 1; i < divsY; ++i) {
        const double y = plot.top() + plot.height() * i / divsY;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // Centre axes with 1/5-division ticks, exactly mirroring ScopeView's
    // own drawGraticule() (down to the same colours and tick length) so
    // the two plots' grids read as one visual family rather than two
    // similar-but-not-quite ones.
    p.setPen(QPen(Theme::plotGridAxis(), 1.0, Qt::DashLine));
    const double cx = plot.center().x();
    const double cy = plot.center().y();
    p.drawLine(QPointF(cx, plot.top()), QPointF(cx, plot.bottom()));
    p.drawLine(QPointF(plot.left(), cy), QPointF(plot.right(), cy));

    p.setPen(Theme::plotGridAxis());
    const double dx = plot.width()  / (divsX * 5.0);
    const double dy = plot.height() / (divsY * 5.0);
    for (int i = 1; i < divsX * 5; ++i) {
        const double x = plot.left() + dx * i;
        p.drawLine(QPointF(x, cy - 3), QPointF(x, cy + 3));
    }
    for (int i = 1; i < divsY * 5; ++i) {
        const double y = plot.top() + dy * i;
        p.drawLine(QPointF(cx - 3, y), QPointF(cx + 3, y));
    }

    // Border in the same brighter "major" colour ScopeView's own graticule
    // uses, distinct from the dotted minor lines above.
    p.setPen(Theme::plotGridMajor());
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot);

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);
    const QFontMetrics fm(small);

    // Y scale on the right. Coloured like the sole active trace when there
    // is exactly one — the same way ScopeView colours a channel's voltage
    // numbers in that channel's own colour — or a neutral grey with zero or
    // two active, since the scale is shared by both traces and no single
    // colour would correctly "belong" to it the way a channel's own volts
    // axis belongs to that one channel.
    int activeCount = 0;
    QColor soleColour = Theme::plotAxisText();
    for (const Trace &t : m_traces) {
        if (t.active) {
            ++activeCount;
            soleColour = t.color;
        }
    }
    p.setPen(activeCount == 1 ? soleColour : Theme::plotAxisText());
    for (int i = 0; i <= divsY; ++i) {
        const double frac = 1.0 - double(i) / divsY;
        const double y = plot.top() + plot.height() * i / divsY;
        QString label;
        if (m_yScale == YScale::Linear) {
            const double v = kMinLinearVolts + frac * (kMaxLinearVolts - kMinLinearVolts);
            label = QString::number(v, 'f', 2);
        } else {
            const double db = kMinDb + frac * (kMaxDb - kMinDb);
            label = QString::number(db, 'f', 0);
        }
        p.drawText(QRectF(plot.right() + 4, y - fm.height() / 2.0, 46, fm.height()),
                  Qt::AlignLeft | Qt::AlignVCenter, label);
    }
    // Unit label, given a clean gap above the topmost tick rather than
    // fighting it for the same few pixels — mirrors ScopeView's own
    // drawVoltScale() unit-box sizing (ends well short of the first tick's
    // own vertical centre).
    constexpr double kScalePad = 4.0;
    const QString unitText = (m_yScale == YScale::Linear) ? QStringLiteral("V")
        : (m_unit == Unit::DBV ? QStringLiteral("dBV") : QStringLiteral("dBm"));
    p.drawText(QRectF(plot.right() + kScalePad, 1, 46 - kScalePad, kTopGutter - 14),
              Qt::AlignLeft | Qt::AlignVCenter, unitText);

    // Frequency scale along the bottom, same neutral text colour
    // ScopeView's own time axis uses — Nyquist of whichever active trace
    // has the highest sample rate, so a mixed-rate pairing (uncommon, but
    // possible with Math derived from a different acquisition instant)
    // stays wide enough to show all of them rather than clipping one. End
    // labels aligned to the graticule edge rather than centred on it,
    // exactly mirroring ScopeView's own time axis, so "0 Hz" cannot reach
    // left of the plot and crowd whatever sits beside this widget.
    p.setPen(Theme::plotAxisText());
    double sampleRate = 0.0;
    for (const Trace &trace : m_traces) {
        if (trace.active)
            sampleRate = std::max(sampleRate, trace.sampleRate);
    }
    const double nyquist = sampleRate > 0.0 ? sampleRate / 2.0 : 0.0;
    for (int i = 0; i <= divsX; ++i) {
        const double freq = nyquist * i / divsX;
        const double x    = plot.left() + plot.width() * i / divsX;
        QRectF box(x - 40, plot.bottom() + 2, 80, 16);
        int align = Qt::AlignHCenter | Qt::AlignTop;
        if (i == 0) {
            box = QRectF(plot.left(), plot.bottom() + 2, 80, 16);
            align = Qt::AlignLeft | Qt::AlignTop;
        } else if (i == divsX) {
            box = QRectF(plot.right() - 80, plot.bottom() + 2, 80, 16);
            align = Qt::AlignRight | Qt::AlignTop;
        }
        p.drawText(box, align, formatFrequency(freq));
    }
}

void SpectrumView::drawTrace(QPainter &p, const QRectF &plot, const Trace &trace) const
{
    const QVector<double> &bins =
        (m_displayMode == DisplayMode::Average && !trace.avgBins.isEmpty())
            ? trace.avgBins : trace.bins;
    if (bins.isEmpty() || trace.sampleRate <= 0.0)
        return;

    const double nyquist = trace.sampleRate / 2.0;
    const int n = bins.size();

    // One point per bin rather than per pixel: at FftSize/2+1 = 1025 points
    // across a plot rarely narrower than a few hundred pixels, this is
    // already smoother than the display needs, and far simpler than
    // per-pixel-column bucketing.
    QPolygonF poly;
    poly.reserve(n + 2);
    poly.append(QPointF(plot.left(), plot.bottom()));
    for (int k = 0; k < n; ++k) {
        const double freq = (double(k) / double(n - 1)) * nyquist;
        const double x    = plot.left() + (freq / nyquist) * plot.width();
        const double frac = valueToPlotFraction(bins[k]);
        const double y    = plot.bottom() - frac * plot.height();
        poly.append(QPointF(x, y));
    }
    poly.append(QPointF(plot.right(), plot.bottom()));

    QColor fill = trace.color;
    fill.setAlpha(60);
    p.setPen(Qt::NoPen);
    p.setBrush(fill);
    p.drawPolygon(poly);

    p.setPen(QPen(trace.color, 1.2));
    p.setBrush(Qt::NoBrush);
    // The two baseline-anchor points exist only to close the fill polygon
    // above. Drawn straight from poly's own buffer (skipping index 0 and
    // the last one) rather than via poly.mid(1, n): mid() allocates and
    // copies ~n points on every call, and this runs on every repaint —
    // including every single mouse-move event while dragging a ruler
    // cursor, where that copy was the actual cause of visible lag.
    p.drawPolyline(poly.constData() + 1, n);
}

QString SpectrumView::levelText(double volts) const
{
    // No unit suffix: the axis itself already labels the unit once, at the
    // top of the Y scale (see drawGraticule()) — repeating it on every
    // flag was also what pushed a level flag's width past the right
    // gutter and off the edge of the widget.
    return m_yScale == YScale::Linear
        ? QStringLiteral("%1").arg(volts, 0, 'f', 3)
        : QStringLiteral("%1").arg(toDb(volts), 0, 'f', 1);
}

SpectrumView::RulerLayout SpectrumView::computeRulerLayout(const QRectF &plot, double nyquist) const
{
    RulerLayout out;
    out.x1 = plot.left() + std::clamp(m_freq1 / nyquist, 0.0, 1.0) * plot.width();
    out.x2 = plot.left() + std::clamp(m_freq2 / nyquist, 0.0, 1.0) * plot.width();
    out.y1 = plot.bottom() - valueToPlotFraction(m_level1) * plot.height();
    out.y2 = plot.bottom() - valueToPlotFraction(m_level2) * plot.height();

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    const QFontMetrics fm(small);
    const double flagH = fm.height() + 4.0;

    // Frequency flags below the plot, exactly where ScopeView's own time
    // cursor flags sit (plot.bottom() + 3) — both are vertical cursors on
    // an X axis, so the same placement applies unchanged.
    const QString f1Text = formatFrequency(m_freq1);
    const QString f2Text = formatFrequency(m_freq2);
    const double freqFlagW = std::max(fm.horizontalAdvance(f1Text),
                                      fm.horizontalAdvance(f2Text)) + 10.0;
    out.f1Flag = QRectF(out.x1 - freqFlagW / 2.0, plot.bottom() + 3.0, freqFlagW, flagH);
    out.f2Flag = QRectF(out.x2 - freqFlagW / 2.0, plot.bottom() + 3.0, freqFlagW, flagH);

    // Level flags beside the plot, on the right — this widget's Y scale is
    // always on the right (see plotRect()/drawGraticule()), unlike
    // ScopeView where a voltage flag's side depends on which channel it
    // belongs to.
    constexpr double kPad = 4.0;
    const QString l1Text = levelText(m_level1);
    const QString l2Text = levelText(m_level2);
    const double levelFlagW = std::max(fm.horizontalAdvance(l1Text),
                                       fm.horizontalAdvance(l2Text)) + 10.0;
    out.l1Flag = QRectF(plot.right() + kPad, out.y1 - flagH / 2.0, levelFlagW, flagH);
    out.l2Flag = QRectF(plot.right() + kPad, out.y2 - flagH / 2.0, levelFlagW, flagH);

    return out;
}

void SpectrumView::drawRulers(QPainter &p, const QRectF &plot) const
{
    if (!m_rulersEnabled)
        return;

    double nyquist = 0.0;
    for (const Trace &t : m_traces) {
        if (t.active)
            nyquist = std::max(nyquist, t.sampleRate / 2.0);
    }
    if (nyquist <= 0.0)
        return;   // no active trace has reported a sample rate yet

    p.setRenderHint(QPainter::Antialiasing, true);
    const RulerLayout layout = computeRulerLayout(plot, nyquist);

    p.setPen(QPen(Theme::plotForegroundText(), 1.0, Qt::DashLine));
    p.drawLine(QPointF(layout.x1, plot.top()), QPointF(layout.x1, plot.bottom()));
    p.drawLine(QPointF(layout.x2, plot.top()), QPointF(layout.x2, plot.bottom()));
    p.drawLine(QPointF(plot.left(), layout.y1), QPointF(plot.right(), layout.y1));
    p.drawLine(QPointF(plot.left(), layout.y2), QPointF(plot.right(), layout.y2));

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);

    // Flags: a solid, rounded badge in plotForegroundText()'s own colour
    // with the value centred on top — the same technique ScopeView's own
    // drawFlag() uses for its time cursors specifically (every flag here
    // shares that one fill, unlike ScopeView's voltage cursors, which use
    // each channel's own colour instead), so these read as the same kind
    // of object rather than a different style invented for this plot.
    auto drawFlag = [&](const QRectF &box, const QString &text) {
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::plotForegroundText());
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setPen(Theme::onPlotForeground());
        p.drawText(box, Qt::AlignCenter, text);
    };
    drawFlag(layout.f1Flag, formatFrequency(m_freq1));
    drawFlag(layout.f2Flag, formatFrequency(m_freq2));
    drawFlag(layout.l1Flag, levelText(m_level1));
    drawFlag(layout.l2Flag, levelText(m_level2));

    // Delta-only readout, top-left of the plot — each cursor's own value is
    // already on its flag; this is the one thing a flag does not show.
    // Deliberately plain fixed text, not a draggable/minimizable box like
    // ScopeView's own Ruler readout: two numbers do not need that much
    // chrome.
    const QFontMetrics fm(small);
    QString deltaLevelText;
    if (m_yScale == YScale::Linear) {
        const QString unitSuffix = QStringLiteral("V");
        deltaLevelText = QStringLiteral("\u0394L %1 %2")
            .arg(std::fabs(m_level2 - m_level1), 0, 'f', 3).arg(unitSuffix);
    } else {
        deltaLevelText = QStringLiteral("\u0394L %1 dB")
            .arg(std::fabs(toDb(m_level2) - toDb(m_level1)), 0, 'f', 1);
    }
    const QString deltaFreqText = QStringLiteral("\u0394F %1")
        .arg(formatFrequency(std::fabs(m_freq2 - m_freq1)));

    const double textWidth = std::max(fm.horizontalAdvance(deltaFreqText),
                                      fm.horizontalAdvance(deltaLevelText));
    const QRectF box(plot.left() + 6, plot.top() + 4, textWidth + 10, fm.height() * 2 + 8);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::plotLabelBackdrop());
    p.drawRect(box);
    p.setBrush(Qt::NoBrush);
    p.setPen(Theme::plotForegroundText());
    p.drawText(box.adjusted(5, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop, deltaFreqText);
    p.drawText(box.adjusted(5, fm.height() + 4, 0, 0), Qt::AlignLeft | Qt::AlignTop, deltaLevelText);
}

SpectrumView::RulerDrag SpectrumView::hitTestRulers(const QPointF &pos, const QRectF &plot) const
{
    if (!m_rulersEnabled)
        return RulerDrag::None;

    double nyquist = 0.0;
    for (const Trace &t : m_traces) {
        if (t.active)
            nyquist = std::max(nyquist, t.sampleRate / 2.0);
    }
    if (nyquist <= 0.0)
        return RulerDrag::None;

    const RulerLayout layout = computeRulerLayout(plot, nyquist);

    // Flags first: a real, generously-sized target in their own right, not
    // just the thin dashed line — same priority ScopeView's own Ruler
    // flags get over their line, since a 1 px line is a much harder target
    // to grab precisely than a whole badge.
    if (layout.f1Flag.contains(pos)) return RulerDrag::Freq1;
    if (layout.f2Flag.contains(pos)) return RulerDrag::Freq2;
    if (layout.l1Flag.contains(pos)) return RulerDrag::Level1;
    if (layout.l2Flag.contains(pos)) return RulerDrag::Level2;

    if (!plot.contains(pos))
        return RulerDrag::None;
    constexpr double kHitRadius = 6.0;
    if (std::fabs(pos.x() - layout.x1) <= kHitRadius) return RulerDrag::Freq1;
    if (std::fabs(pos.x() - layout.x2) <= kHitRadius) return RulerDrag::Freq2;
    if (std::fabs(pos.y() - layout.y1) <= kHitRadius) return RulerDrag::Level1;
    if (std::fabs(pos.y() - layout.y2) <= kHitRadius) return RulerDrag::Level2;
    return RulerDrag::None;
}

void SpectrumView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const RulerDrag hit = hitTestRulers(event->position(), plotRect());
        if (hit != RulerDrag::None) {
            m_rulerDrag = hit;
            setCursor(hit == RulerDrag::Freq1 || hit == RulerDrag::Freq2
                          ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void SpectrumView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_rulerDrag != RulerDrag::None) {
        const QRectF plot = plotRect();
        double nyquist = 0.0;
        for (const Trace &t : m_traces) {
            if (t.active)
                nyquist = std::max(nyquist, t.sampleRate / 2.0);
        }
        if (m_rulerDrag == RulerDrag::Freq1 || m_rulerDrag == RulerDrag::Freq2) {
            const double frac = std::clamp(
                (event->position().x() - plot.left()) / plot.width(), 0.0, 1.0);
            const double freq = frac * nyquist;
            if (m_rulerDrag == RulerDrag::Freq1)
                m_freq1 = freq;
            else
                m_freq2 = freq;
        } else {
            const double frac = std::clamp(
                (plot.bottom() - event->position().y()) / plot.height(), 0.0, 1.0);
            const double volts = plotFractionToValue(frac);
            if (m_rulerDrag == RulerDrag::Level1)
                m_level1 = volts;
            else
                m_level2 = volts;
        }
        update();
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void SpectrumView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_rulerDrag != RulerDrag::None) {
        m_rulerDrag = RulerDrag::None;
        unsetCursor();
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}
