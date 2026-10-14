#include "scopeview.h"
#include "fonts.h"
#include <QApplication>
#include "perftrace.h"
#include <cstdio>
#include "mathops.h"

#include <QByteArray>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QHash>
#include <QJsonArray>
#include <QPainter>
#include <QPainterPath>
#include <QElapsedTimer>
#include <QTimer>

#include "annotationbox.h"
#include "icons.h"
#include "theme.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// Gutters are only reserved when the scale that lives in them is shown; see
// leftGutter()/rightGutter(). The narrow values still leave room for the time
// unit and for a tick label to overhang the edge.
constexpr int kScaleGutter    = 42;   // 20% narrower, then ~17% more
constexpr int kNarrowLeft     = 25;   // ~17% narrower
constexpr int kNarrowRight    = 13;   // ~17% narrower
constexpr int kTopGutter      = 28;
// The same width as CH1's scale gutter, so their ruler flags (each spanning
// its gutter) are the same width too.
constexpr int kMathColumnWidth = kScaleGutter;
constexpr int kMathDrag = -2;          // m_dragChannel sentinel for the math column
// m_dragChannel sentinels for a reference's own column: slot N is
// kReferenceDragBase - N, so both slots (ScopeView::MaxReferences = 2) get
// distinct values below kMathDrag without needing their own separate member.
constexpr int kReferenceDragBase = -10;

// The three places that need "is m_dragChannel currently one of the gutter
// sentinels" all agree on this one definition rather than each re-listing
// kMathDrag/kReferenceDragBase separately, which is what let mouseMoveEvent's
// own check silently miss the reference sentinels the first time this was
// written — a real channel index is always >= 0.
bool isGutterDragActive(int dragChannel)
{
    return dragChannel >= 0 || dragChannel == kMathDrag || dragChannel <= kReferenceDragBase;
}

// m_rulerDrag sentinels — independent of, and checked before, m_dragChannel.
// m_rulerDrag sentinels — independent of, and checked before, m_dragChannel.
// Time cursors and the box are single, fixed IDs; voltage cursors encode
// WHICH channel (0..MaxChannels, the last being math) since every channel can
// have its own pair active at once now, not just one shared selection.
constexpr int kNoRulerDrag   = -1;
constexpr int kRulerT1       = 0;
constexpr int kRulerT2       = 1;
constexpr int kRulerBox      = 2;
constexpr int kRulerMinimize = 3;
constexpr int kRulerVoltsBase = 10;
constexpr int rulerVoltsDragId(int channel, bool isV2)
{
    return kRulerVoltsBase + channel * 2 + (isV2 ? 1 : 0);
}
constexpr bool isRulerVoltsDrag(int dragId) { return dragId >= kRulerVoltsBase; }
constexpr int rulerVoltsChannelOf(int dragId) { return (dragId - kRulerVoltsBase) / 2; }
constexpr bool rulerVoltsIsV2(int dragId) { return (dragId - kRulerVoltsBase) % 2 == 1; }
constexpr int kAxisHeight   = 18;   // time labels under the graticule
constexpr int kStripHeight  = 22;

QString formatEng(double value, const char *unit, int decimals = 2)
{
    const double a = std::fabs(value);
    const char *prefix = "";
    double scaled = value;

    if (a >= 1e6)      { scaled = value / 1e6;  prefix = "M"; }
    else if (a >= 1e3) { scaled = value / 1e3;  prefix = "k"; }
    else if (a >= 1.0) { scaled = value;        prefix = "";  }
    else if (a >= 1e-3){ scaled = value * 1e3;  prefix = "m"; }
    else if (a >= 1e-6){ scaled = value * 1e6;  prefix = "u"; }
    else if (a >= 1e-9){ scaled = value * 1e9;  prefix = "n"; }
    else               { scaled = 0.0;          prefix = "";  }

    // UTF-8: Math's units include \u03a9, \u00b2, \u00b7 ...
    const QString suffix = QString::fromLatin1(prefix) + QString::fromUtf8(unit);
    return QStringLiteral("%1 %2").arg(scaled, 0, 'f', decimals).arg(suffix).trimmed();
}


// %g gives things like "0.05e+03". Pick the decimal count from the tick step
// and never use an exponent.
// Decimals needed to show a value exactly, up to three.
int decimalsFor(double value)
{
    for (int d = 0; d < 3; ++d) {
        const double scaled = value * std::pow(10.0, d);
        if (std::fabs(scaled - std::round(scaled)) < 1e-6)
            return d;
    }
    return 3;
}

}   // namespace

QString ScopeView::axisNumber(double value, double step, double offset)
{
    int decimals = 0;
    const double a = std::fabs(step);
    if (a > 0.0 && a < 1.0)
        decimals = int(std::ceil(-std::log10(a)));

    // With a DC offset the ticks land on values like -1.08, which the step size
    // alone would round away. Only take the precision the offset actually needs,
    // so a whole-number offset does not produce a column of ".00".
    decimals = std::max(decimals, decimalsFor(offset));

    // The gutter cannot hold five characters plus a sign, so wide numbers give
    // up decimals rather than overflow.
    const double magnitude = std::fabs(value);
    const int allowed = magnitude >= 1000.0 ? 0 : magnitude >= 100.0 ? 1 : 2;
    decimals = std::clamp(std::min(decimals, allowed), 0, 2);

    QString text = QString::number(value, 'f', decimals);
    if (text == QLatin1String("-0"))
        text = QStringLiteral("0");
    return text;
}

ScopeView::ScopeView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(640, 380);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    // Without this, ScopeView's own background has no focus policy at all
    // (QWidget defaults to Qt::NoFocus), so clicking it does not take focus
    // away from whatever child widget currently has it — including an
    // annotation's text edit, which would otherwise keep blinking and
    // accepting keystrokes after a click meant to be "away from it".
    setFocusPolicy(Qt::ClickFocus);
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void ScopeView::setChannelCount(int count)
{
    m_channelCount = std::clamp(count, 0, MaxChannels);
    for (int ch = m_channelCount; ch < MaxChannels; ++ch) {
        m_channels[ch].visible = false;
        m_channels[ch].samples.clear();
    }
    update();
}

void ScopeView::setChannelVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].visible = visible;
    if (!visible) {
        m_channels[channel].samples.clear();
        m_channels[channel].rawSamples.clear();
    }
    update();
}

void ScopeView::setChannelColor(int channel, const QColor &color)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].color = color;
    update();
}

void ScopeView::setVoltsPerDiv(int channel, double voltsPerDiv)
{
    if (channel < 0 || channel >= MaxChannels || voltsPerDiv <= 0.0)
        return;
    m_channels[channel].voltsPerDiv = voltsPerDiv;
    update();
}

// DC offset: a voltage added to the signal. With +20 mV set, a trace sitting at
// 0 V reads 20 mV and is drawn 20 mV higher. The graticule itself does not move,
// so the numbers on the scale stay put and the waveform slides against them.
void ScopeView::setOffset(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].offset = volts;
    update();
}

// Input offset: the centre of the hardware input window, so the graticule is
// drawn around it. This is what lets a +/-200 mV page sit on top of a 12 V rail
// and show only the ripple.
void ScopeView::setWindowCentre(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].windowCentre = volts;
    update();
}

// Display shift only. Moves the trace and its scale together without changing
// what the hardware is digitising.
// Volts, not divisions: a fixed voltage shift so the reachable pan range does
// not shrink as the channel is zoomed in. The previous divisions-based version
// meant the same pct offset moved fewer and fewer volts the more you zoomed,
// which is why dragging used to stop dead at the edge of the zoomed window.
void ScopeView::setPositionVolts(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].positionVolts = volts;
    update();
}

double ScopeView::voltsPerDiv(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0.0;
    return m_channels[channel].voltsPerDiv;
}

double ScopeView::positionVolts(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0.0;
    return m_channels[channel].positionVolts;
}

bool ScopeView::channelVisible(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return false;
    return m_channels[channel].visible;
}

void ScopeView::setAbsoluteTimeOrigin(double seconds)
{
    m_absoluteTimeOrigin = seconds;
    m_hasAbsoluteTimeOrigin = true;
    update();
}

QJsonObject ScopeView::remoteFrame(int columns) const
{
    QJsonObject f;
    const QRectF plot = plotRect();
    const double ppd = pixelsPerDiv(plot);
    columns = std::max(16, columns);
    f.insert(QStringLiteral("hdiv"), HorizontalDivs);
    f.insert(QStringLiteral("vdiv"), VerticalDivs);
    f.insert(QStringLiteral("perDiv"), m_secondsPerDiv);
    // Time at division i is (i + offset - H/2) x per-div (time zero sits at
    // division H/2 - offset, as in drawGraticule()).
    f.insert(QStringLiteral("leftTime"), (timeAxisOffsetDivs() - HorizontalDivs / 2.0) * m_secondsPerDiv);

    QJsonArray channels;
    for (int ch = 0; ch < m_channelCount; ++ch) {
        const Channel &c = m_channels[ch];
        const int n = c.samples.size();
        if (!c.visible || n < 2 || ppd <= 0.0)
            continue;
        QJsonArray mins, maxs;
        for (int col = 0; col < columns; ++col) {
            const double x0 = plot.left() + plot.width() * col / columns;
            const double x1 = plot.left() + plot.width() * (col + 1) / columns;
            int i0 = int(std::floor(sampleIndexAtX(plot, x0)));
            int i1 = int(std::ceil(sampleIndexAtX(plot, x1)));
            if (i1 < 0 || i0 > n - 1) {
                mins.append(QJsonValue());   // no data here
                maxs.append(QJsonValue());
                continue;
            }
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0, n - 1);
            double lo = c.samples[i0], hi = lo;
            for (int i = i0 + 1; i <= i1; ++i) {
                lo = std::min(lo, c.samples[i]);
                hi = std::max(hi, c.samples[i]);
            }
            // Divisions from centre, x100; valueToY() applies probe,
            // inversion, offsets and position exactly as the plot does.
            const double dLo = (plot.center().y() - valueToY(plot, c, lo)) / ppd;
            const double dHi = (plot.center().y() - valueToY(plot, c, hi)) / ppd;
            mins.append(int(std::lround(std::min(dLo, dHi) * 100.0)));
            maxs.append(int(std::lround(std::max(dLo, dHi) * 100.0)));
        }
        QJsonObject o;
        o.insert(QStringLiteral("n"), ch + 1);
        o.insert(QStringLiteral("color"), c.color.name());
        o.insert(QStringLiteral("unit"), c.unit);
        o.insert(QStringLiteral("perDiv"), c.voltsPerDiv / c.scale);
        // The value on the centre line, for the voltage scale: from
        // valueToY(), divisions = (signal - windowCentre + position) / perDiv.
        o.insert(QStringLiteral("centre"), c.windowCentre - c.positionVolts);
        o.insert(QStringLiteral("min"), mins);
        o.insert(QStringLiteral("max"), maxs);
        channels.append(o);
    }
    f.insert(QStringLiteral("channels"), channels);

    // Math, on its own (auto-fitted or manual) scale: mathRange().
    if (m_math.enabled && m_math.samples.size() >= 2 && ppd > 0.0) {
        const MathRange range = mathRange(plot);
        const int n = m_math.samples.size();
        QJsonArray mins, maxs;
        for (int col = 0; col < columns; ++col) {
            const double x0 = plot.left() + plot.width() * col / columns;
            const double x1 = plot.left() + plot.width() * (col + 1) / columns;
            int i0 = int(std::floor(sampleIndexAtX(plot, x0)));
            int i1 = int(std::ceil(sampleIndexAtX(plot, x1)));
            if (i1 < 0 || i0 > n - 1) {
                mins.append(QJsonValue());
                maxs.append(QJsonValue());
                continue;
            }
            i0 = std::clamp(i0, 0, n - 1);
            i1 = std::clamp(i1, i0, n - 1);
            double lo = m_math.samples[i0], hi = lo;
            for (int i = i0 + 1; i <= i1; ++i) {
                lo = std::min(lo, m_math.samples[i]);
                hi = std::max(hi, m_math.samples[i]);
            }
            mins.append(int(std::lround((lo - range.mid) * range.scale / ppd * 100.0)));
            maxs.append(int(std::lround((hi - range.mid) * range.scale / ppd * 100.0)));
        }
        QJsonObject o;
        o.insert(QStringLiteral("color"), m_math.color.name());
        o.insert(QStringLiteral("unit"), mathUnit());
        o.insert(QStringLiteral("perDiv"), range.scale > 0.0 ? ppd / range.scale : 0.0);
        o.insert(QStringLiteral("centre"), range.mid);
        o.insert(QStringLiteral("min"), mins);
        o.insert(QStringLiteral("max"), maxs);
        f.insert(QStringLiteral("math"), o);
    }

    // Decoder bus rows: annotations in view, as fractions of the plot width.
    QJsonArray rows;
    for (const BusRow &row : m_busRows) {
        if (!row.visible)
            continue;
        QJsonObject o;
        o.insert(QStringLiteral("label"), row.label);
        o.insert(QStringLiteral("color"), row.colour.name());
        o.insert(QStringLiteral("message"), row.message);
        QJsonArray anns;
        for (const BusAnnotation &a : row.annotations) {
            const double fa = (sampleIndexToX(plot, a.start) - plot.left()) / plot.width();
            const double fb = (sampleIndexToX(plot, a.end) - plot.left()) / plot.width();
            if (fb < 0.0 || fa > 1.0)
                continue;
            QJsonObject ao;
            ao.insert(QStringLiteral("a"), std::max(-0.01, fa));
            ao.insert(QStringLiteral("b"), std::min(1.01, fb));
            ao.insert(QStringLiteral("t"), QJsonArray::fromStringList(a.texts));
            ao.insert(QStringLiteral("e"), a.error);
            anns.append(ao);
            if (anns.size() >= 400)
                break;
        }
        o.insert(QStringLiteral("ann"), anns);
        rows.append(o);
    }
    f.insert(QStringLiteral("bus"), rows);

    QJsonArray readouts;
    for (const ReadoutLine &l : readoutLines()) {
        QJsonObject o;
        o.insert(QStringLiteral("text"), l.text);
        o.insert(QStringLiteral("color"), l.colour.name());
        readouts.append(o);
    }
    f.insert(QStringLiteral("readouts"), readouts);

    QJsonArray panels;
    const auto addPanel = [&](const QString &title, const QStringList &lines, const QVector<bool> &warn) {
        QJsonObject o;
        o.insert(QStringLiteral("title"), title);
        QJsonArray ls, ws;
        for (int i = 0; i < lines.size(); ++i) {
            ls.append(QString(lines[i]).replace(QLatin1Char('\t'), QStringLiteral("   ")));
            ws.append(i < warn.size() && warn[i]);
        }
        o.insert(QStringLiteral("lines"), ls);
        o.insert(QStringLiteral("warn"), ws);
        panels.append(o);
    };
    if (m_engine.enabled && !m_engine.crankLines.isEmpty())
        addPanel(QStringLiteral("Crank and cam"), m_engine.crankLines, m_engine.crankWarn);
    if (m_engine.enabled && (!m_engine.compression.isEmpty() || !m_engine.note.isEmpty())) {
        QStringList lines { m_engine.note };
        QVector<bool> warn { false };
        for (int i = 0; i < m_engine.compression.size(); ++i) {
            lines << QStringLiteral("Cylinder %1: %2 %").arg(i + 1).arg(m_engine.compression[i], 0, 'f', 0);
            warn << (m_engine.compression[i] < 100.0 - m_engine.flagBelow);
        }
        addPanel(QStringLiteral("Relative compression"), lines, warn);
    }
    for (const InfoPanel &p : m_infoPanels)
        addPanel(p.title, p.lines, p.warn);
    f.insert(QStringLiteral("panels"), panels);
    return f;
}

void ScopeView::setTraceGaps(bool on)
{
    if (m_traceGaps == on)
        return;
    m_traceGaps = on;
    update();
}

void ScopeView::setNowAtRightEdge(bool on)
{
    if (m_nowAtRight == on)
        return;
    m_nowAtRight = on;
    update();
}

void ScopeView::clearAbsoluteTimeOrigin()
{
    m_hasAbsoluteTimeOrigin = false;
    update();
}

// The SDK reports the voltage at the BNC. A probe turns that into what the user
// is actually measuring: displayed = raw * gradient + offset, in `unit`.
void ScopeView::setProbe(int channel, double gradient, double offset, const QString &unit)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    Channel &c = m_channels[channel];
    c.probeGain   = qFuzzyIsNull(gradient) ? 1.0 : gradient;
    c.probeOffset = offset;
    c.unit        = unit.isEmpty() ? QStringLiteral("V") : unit;
    recomputeMath(channel);   // Math combines measured (probed) values
    update();
}

// Vertical zoom. Purely a display magnification: it never changes the hardware
// range, so scaling a signal off the page is not an over-range condition.
void ScopeView::setDisplayScale(int channel, double scale)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].scale = scale > 0.0 ? scale : 1.0;
    update();
}

void ScopeView::setInverted(int channel, bool inverted)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].inverted = inverted;
    recomputeMath(channel);
    update();
}

void ScopeView::setChannelMeasurement(int channel, Measurement type, bool enabled)
{
    if (channel == RulersMathChannel)
        m_math.activeMeasurements[size_t(type)] = enabled;
    else if (channel >= 0 && channel < MaxChannels)
        m_channels[channel].activeMeasurements[size_t(type)] = enabled;
    else
        return;
    update();
}

bool ScopeView::channelMeasurementEnabled(int channel, Measurement type) const
{
    if (channel == RulersMathChannel)
        return m_math.activeMeasurements[size_t(type)];
    if (channel >= 0 && channel < MaxChannels)
        return m_channels[channel].activeMeasurements[size_t(type)];
    return false;
}

void ScopeView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }

    // Dragging a voltage scale moves that channel's offset, as on a bench
    // scope. Anywhere else is a plain click.
    const QRectF plot = plotRect();
    const double x = event->position().x();

    // Drawn last, on top of everything else — checked first here for the
    // same reason: whatever else the click might otherwise have hit
    // underneath one of these two small rects, the visible button wins.
    if (m_zoomHistoryCanBack && zoomBackRect(plot).contains(event->position())) {
        emit zoomBackRequested();
        QWidget::mousePressEvent(event);
        return;
    }
    if (m_zoomHistoryCanForward && zoomForwardRect(plot).contains(event->position())) {
        emit zoomForwardRequested();
        QWidget::mousePressEvent(event);
        return;
    }

    // Rulers, when armed, take priority over everything else in the widget:
    // a pan or zoom starting a pixel off a cursor would be far more
    // disruptive to a measurement in progress than a dropped pan gesture is.
    // Engine cycle markers, grabbed by their flags at the top of the plot —
    // checked before the rulers so the two never fight over a press.
    if (m_engine.enabled && m_engine.manual) {
        for (int which : { 1, 2 }) {
            if (engineMarkerFlag(plot, which).contains(event->position())) {
                m_engineDrag = which;
                setCursor(Qt::SizeHorCursor);
                QWidget::mousePressEvent(event);
                return;
            }
        }
    }

    // The measurement readouts: grab anywhere on the block, drop near a
    // corner (see mouseReleaseEvent()).
    {
        const QVector<ReadoutLine> lines = readoutLines();
        if (!lines.isEmpty()) {
            const QRectF block = readoutBlockRect(plot, lines);
            if (block.contains(event->position())) {
                m_readoutDragging = true;
                m_readoutGrab = event->position() - block.topLeft();
                m_readoutDragTopLeft = block.topLeft();
                setCursor(Qt::ClosedHandCursor);
                QWidget::mousePressEvent(event);
                return;
            }
        }
    }

    if (m_rulers.enabled) {
        if (rulerCloseRect(plot).contains(event->position())) {
            setRulersEnabled(false);
            emit rulersClosed();
            QWidget::mousePressEvent(event);
            return;
        }
        if (rulerMinimizeRect(plot).contains(event->position())) {
            setRulersBoxMinimized(!m_rulers.boxMinimized);
            QWidget::mousePressEvent(event);
            return;
        }
        m_rulerDrag = hitTestRulers(event->position(), plot);
        if (m_rulerDrag != kNoRulerDrag) {
            m_rulerDragLast = event->position();
            setCursor(m_rulerDrag == kRulerBox ? Qt::SizeAllCursor
                     : (m_rulerDrag == kRulerT1 || m_rulerDrag == kRulerT2) ? Qt::SizeHorCursor
                     : isRulerVoltsDrag(m_rulerDrag) ? Qt::SizeVerCursor
                     : Qt::ArrowCursor);
            QWidget::mousePressEvent(event);
            return;
        }
    }

    // The selected annotation's external chrome — trash, resize handles,
    // the move handle — takes priority over its own arrow tip below: they
    // are separate, smaller targets that happen to sit near it.
    if (m_selectedAnnotation >= 0 && m_selectedAnnotation < m_annotations.size()) {
        const AnnotationState &sel = m_annotations[m_selectedAnnotation];
        if (annotationTrashRect(sel).contains(event->position())) {
            removeAnnotation(m_selectedAnnotation);
            QWidget::mousePressEvent(event);
            return;
        }
        const int handle = hitTestAnnotationHandles(sel, event->position());
        if (handle >= 0) {
            m_annotationResizeDrag = m_selectedAnnotation;
            m_annotationResizeHandle = handle;
            m_annotationResizeStartGeom = sel.box->geometry();
            m_annotationResizeStartMouse = event->position();
            setCursor(annotationHandleCursor(handle));
            QWidget::mousePressEvent(event);
            return;
        }
        if (annotationMoveRect(sel).contains(event->position())) {
            m_annotationMoveDrag = m_selectedAnnotation;
            m_annotationMoveLast = event->position();
            setCursor(Qt::SizeAllCursor);
            QWidget::mousePressEvent(event);
            return;
        }
    }

    // Annotation tips take priority over panning/zoom-select/channel-drag
    // for the same reason Rulers do above: a small, precise drag target
    // should not be swallowed by a coarser gesture starting a pixel away.
    // Checked after Rulers, which already claim top priority for a
    // measurement in progress.
    m_annotationDrag = hitTestAnnotations(event->position(), plot);
    if (m_annotationDrag >= 0) {
        AnnotationState &a = m_annotations[m_annotationDrag];
        if (!a.active) {
            // First-ever grab of this annotation's tip: it starts tracking
            // a real sample from exactly where it was just clicked, which
            // is right where the default straight-down pointer already was
            // (that is what made the hit test succeed), so this is visually
            // seamless rather than a jump.
            const double anchor = viewAnchor(std::max(m_lastLength, 1));
            a.timeOffset = sampleIndexAtX(plot, event->position().x()) - anchor;
            a.volts = (a.channel == RulersMathChannel)
                ? mathYToValue(plot, event->position().y())
                : yToValue(plot, m_channels[a.channel], event->position().y());
            a.active = true;
        }
        setCursor(Qt::CrossCursor);
        QWidget::mousePressEvent(event);
        return;
    }

    // Math's and each reference's own precise range, from the SAME
    // placement leftGutter()/rightGutter()/the scale-drawing functions
    // already use — checked before CH1/CH2's own wider ranges below, since
    // those (x < plot.left() / x > plot.right()) are deliberately
    // unbounded on the far side and would otherwise catch every extra
    // column beyond them too. This is the actual fix for a reference's own
    // column dragging the adjacent real channel instead: CH2's check previously
    // had no upper bound at all.
    const GutterAllocation alloc = computeGutterAllocation();
    int hitReferenceSlot = -1;
    for (int slot = 0; slot < MaxReferences; ++slot) {
        if (alloc.referenceColumn[slot] < 0)
            continue;
        const double colStart = extraColumnX(alloc.referenceOnLeft[slot], alloc.referenceColumn[slot], plot);
        const double colWidth = extraColumnWidth(alloc.referenceOnLeft[slot], alloc.referenceColumn[slot]);
        if (x >= colStart && x < colStart + colWidth) {
            hitReferenceSlot = slot;
            break;
        }
    }
    bool hitMath = false;
    if (alloc.mathColumn >= 0) {
        const double colStart = extraColumnX(alloc.mathOnLeft, alloc.mathColumn, plot);
        const double colWidth = extraColumnWidth(alloc.mathOnLeft, alloc.mathColumn);
        hitMath = x >= colStart && x < colStart + colWidth;
    }

    if (hitReferenceSlot >= 0)
        m_dragChannel = kReferenceDragBase - hitReferenceSlot;
    else if (hitMath)
        m_dragChannel = kMathDrag;
    else if (x < plot.left() && m_channels[0].visible)
        m_dragChannel = 0;
    else if (x > plot.right() && m_channelCount > 1 && m_channels[1].visible)
        m_dragChannel = 1;
    else
        m_dragChannel = -1;

    if (isGutterDragActive(m_dragChannel)) {
        m_dragLastY = event->position().y();
        setCursor(Qt::SizeVerCursor);
    } else if (m_zoomMode && plot.contains(event->position())) {
        // Selection only happens while zoom is armed, so an ordinary click
        // still just closes the detail panel. The band always spans the full
        // plot height — zooming is a horizontal, time-only operation.
        m_selecting = true;
        m_selectStart = QPointF(event->position().x(), plot.top());
        m_selectEnd   = QPointF(event->position().x(), plot.bottom());
    } else if (plot.contains(event->position())) {
        // Dragging the waveform itself pans the visible window horizontally.
        // Whether this turns out to be a drag or a plain click is decided at
        // release, by how far the mouse actually moved.
        m_panning = true;
        m_panMoved = false;
        m_panLastX = event->position().x();
        m_panPressX = m_panLastX;
    } else {
        emit clicked();
    }
    QWidget::mousePressEvent(event);
}

void ScopeView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_readoutDragging) {
        m_readoutDragTopLeft = event->position() - m_readoutGrab;
        update();
        return;
    }
    if (m_engineDrag != 0) {
        const QRectF plot = plotRect();
        const double x = std::clamp(event->position().x(), plot.left(), plot.right());
        (m_engineDrag == 1 ? m_engine.aTime : m_engine.bTime) =
            sampleIndexToTime(sampleIndexAtX(plot, x));
        m_engine.markersPlaced = true;
        update();
        emit engineMarkersMoved();
        return;
    }
    if (m_rulerDrag != kNoRulerDrag) {
        const QRectF plot = plotRect();
        const QPointF pos = event->position();
        const double anchor = viewAnchor(std::max(m_lastLength, 1));

        if (isRulerVoltsDrag(m_rulerDrag)) {
            const int channel = rulerVoltsChannelOf(m_rulerDrag);
            const bool isV2 = rulerVoltsIsV2(m_rulerDrag);
            const bool isMath = channel == RulersMathChannel;
            RulerState::ChannelCursor &cursor = isMath ? m_rulers.math : m_rulers.channels[channel];
            // Only as far as its flag can follow (centred on the line, kept
            // in the plot): stopping the line at the edge left a few pixels
            // where the line moved and the flag didn't.
            const double half = computeRulerLayout(plot).flagHeight / 2.0;
            const double y = std::clamp(pos.y(), plot.top() + half, std::max(plot.top() + half, plot.bottom() - half));
            const double value = isMath ? mathYToValue(plot, y)
                                        : yToValue(plot, m_channels[channel], y);
            (isV2 ? cursor.v2 : cursor.v1) = value;
        } else switch (m_rulerDrag) {
        case kRulerT1:
        case kRulerT2: {
            // Clamped to the currently visible window — same start/length
            // sampleIndexAtX() itself computes internally, just exposed
            // here so the result can be bounded before it is stored.
            // Annotations use the same underlying sampleIndexAtX() for
            // their own time-drag (see below) and are deliberately left
            // unclamped: an annotation marking something can reasonably
            // point anywhere, including off the currently visible window,
            // in a way a "where is this cursor right now" ruler cannot.
            const int total = std::max(m_lastLength, 1);
            const int start = windowStart(total);
            const int length = std::max(2, std::min(windowLength(), total - start));
            // Likewise only as far as the time flag can follow.
            const double halfW = computeRulerLayout(plot).timeFlagWidth / 2.0;
            const double x = std::clamp(pos.x(), plot.left() + halfW, std::max(plot.left() + halfW, plot.right() - halfW));
            const double clampedIndex = std::clamp(sampleIndexAtX(plot, x),
                                                   double(start), double(start + length));
            (m_rulerDrag == kRulerT1 ? m_rulers.t1Offset : m_rulers.t2Offset)
                = clampedIndex - anchor;
            break;
        }
        case kRulerBox: {
            // The box has no natural "absolute" position the way a cursor
            // does, so this is a relative drag, same pattern as the other
            // relative drags elsewhere in this file.
            if (m_rulers.boxPos.x() < 0.0)
                m_rulers.boxPos = rulerBoxRect(plot).topLeft();
            m_rulers.boxPos += pos - m_rulerDragLast;
            break;
        }
        default:
            break;
        }
        m_rulerDragLast = pos;
        update();
        return;
    }

    if (m_annotationResizeDrag >= 0) {
        AnnotationState &a = m_annotations[m_annotationResizeDrag];
        const QPointF delta = event->position() - m_annotationResizeStartMouse;
        const QRect start = m_annotationResizeStartGeom;
        int left = start.left(), top = start.top(), right = start.right(), bottom = start.bottom();
        switch (m_annotationResizeHandle) {
        case 0: left  += int(delta.x()); top    += int(delta.y()); break;   // top-left
        case 1: top   += int(delta.y());                           break;   // top-mid
        case 2: right += int(delta.x()); top    += int(delta.y()); break;   // top-right
        case 3: right += int(delta.x());                           break;   // right-mid
        case 4: right += int(delta.x()); bottom += int(delta.y()); break;   // bottom-right
        case 5: bottom += int(delta.y());                          break;   // bottom-mid
        case 6: left  += int(delta.x()); bottom += int(delta.y()); break;   // bottom-left
        case 7: left  += int(delta.x());                           break;   // left-mid
        default: break;
        }
        // Clamped without letting an edge cross past its opposite one — the
        // edge actually being dragged gives way, not the fixed one.
        if (right - left < AnnotationBox::kMinWidth) {
            if (m_annotationResizeHandle == 0 || m_annotationResizeHandle == 6 || m_annotationResizeHandle == 7)
                left = right - AnnotationBox::kMinWidth;
            else
                right = left + AnnotationBox::kMinWidth;
        }
        if (bottom - top < AnnotationBox::kMinHeight) {
            if (m_annotationResizeHandle == 0 || m_annotationResizeHandle == 1 || m_annotationResizeHandle == 2)
                top = bottom - AnnotationBox::kMinHeight;
            else
                bottom = top + AnnotationBox::kMinHeight;
        }
        a.box->setGeometry(left, top, right - left, bottom - top);
        update();
        return;
    }

    if (m_annotationMoveDrag >= 0) {
        AnnotationState &a = m_annotations[m_annotationMoveDrag];
        const QPointF delta = event->position() - m_annotationMoveLast;
        m_annotationMoveLast = event->position();
        a.box->move(a.box->pos() + delta.toPoint());
        update();
        return;
    }

    if (m_annotationDrag >= 0) {
        const QRectF plot = plotRect();
        const double anchor = viewAnchor(std::max(m_lastLength, 1));
        AnnotationState &a = m_annotations[m_annotationDrag];
        a.timeOffset = sampleIndexAtX(plot, event->position().x()) - anchor;
        a.volts = (a.channel == RulersMathChannel)
            ? mathYToValue(plot, event->position().y())
            : yToValue(plot, m_channels[a.channel], event->position().y());
        update();
        return;
    }

    if (m_selecting) {
        // Only the X end moves; the band is pinned to full height.
        m_selectEnd = QPointF(event->position().x(), m_selectEnd.y());
        update();
        return;
    }

    if (m_panning) {
        const double x = event->position().x();
        const double dx = x - m_panLastX;
        m_panLastX = x;
        if (std::fabs(dx) > 0.0) {
            if (std::fabs(x - m_panPressX) > 3.0)
                m_panMoved = true;
            const QRectF plot = plotRect();
            if (plot.width() > 0)
                emit panRequested(dx / plot.width());
        }
        return;
    }

    if (!isGutterDragActive(m_dragChannel))
        return;

    const QRectF plot = plotRect();
    const double perDiv = pixelsPerDiv(plot);
    if (perDiv <= 0.0)
        return;

    const double dy = event->position().y() - m_dragLastY;
    m_dragLastY = event->position().y();

    if (m_dragChannel == kMathDrag) {
        // Auto mode has no fixed per-division value to drag against — the
        // range refits to the data every frame regardless, so there is
        // nothing here for a drag to mean.
        if (m_math.voltsPerDiv > 0.0)
            emit mathOffsetDragged((-dy / perDiv) * m_math.voltsPerDiv);
        return;
    }

    if (m_dragChannel <= kReferenceDragBase) {
        const int slot = kReferenceDragBase - m_dragChannel;
        if (referenceOccupied(slot) && m_references[slot].voltsPerDiv > 0.0)
            emit referenceOffsetDragged(slot, (-dy / perDiv) * m_references[slot].voltsPerDiv);
        return;
    }

    // Reported in volts, at this channel's CURRENT (possibly zoomed) scale, so
    // the trace visibly tracks the mouse one-to-one regardless of zoom. Never
    // the DC offset or the hardware window: rewriting either on every mouse
    // move would wedge the capture card.
    const Channel &c = m_channels[m_dragChannel];
    const double perDivision = c.voltsPerDiv / c.scale;
    emit offsetDragged(m_dragChannel, (-dy / perDiv) * perDivision);
}

void ScopeView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const bool onRulerBox = m_rulers.enabled
        && rulerBoxRect(plotRect()).contains(event->position());
    if (event->button() == Qt::LeftButton && !onRulerBox)
        emit zoomResetRequested();
    QWidget::mouseDoubleClickEvent(event);
}

void ScopeView::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_readoutDragging) {
        // Snap to the corner nearest the block's centre.
        const QRectF plot = plotRect();
        const QRectF block = readoutBlockRect(plot, readoutLines());
        const bool right = block.center().x() > plot.center().x();
        const bool bottom = block.center().y() > plot.center().y();
        m_readoutDragging = false;
        setReadoutCorner((bottom ? 2 : 0) + (right ? 1 : 0));
        emit readoutCornerChanged(m_readoutCorner);
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (m_engineDrag != 0) {
        m_engineDrag = 0;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }
    if (m_rulerDrag != kNoRulerDrag) {
        m_rulerDrag = kNoRulerDrag;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_annotationResizeDrag >= 0) {
        m_annotationResizeDrag = -1;
        m_annotationResizeHandle = -1;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_annotationMoveDrag >= 0) {
        m_annotationMoveDrag = -1;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_annotationDrag >= 0) {
        m_annotationDrag = -1;
        unsetCursor();
        QWidget::mouseReleaseEvent(event);
        return;
    }

    if (m_selecting) {
        m_selecting = false;
        const QRectF plot = plotRect();
        const QRectF box = QRectF(m_selectStart, m_selectEnd).normalized()
                               .intersected(plot);

        // Under about a fifth of a division in either direction it was a click.
        const double minWidth  = plot.width()  / (HorizontalDivs * 5.0);
        const double minHeight = plot.height() / (VerticalDivs   * 5.0);
        if (plot.width() > 0 && plot.height() > 0
            && box.width() > minWidth && box.height() > minHeight) {
            emit zoomRequested(QRectF(
                (box.left()   - plot.left()) / plot.width(),
                (box.top()    - plot.top())  / plot.height(),
                box.width()  / plot.width(),
                box.height() / plot.height()));
        }
        update();
    }

    if (m_panning) {
        m_panning = false;
        // A press that never moved is a click, not a pan — preserves closing
        // the detail panel on an ordinary click anywhere on the plot.
        if (!m_panMoved)
            emit clicked();
    }

    if (isGutterDragActive(m_dragChannel)) {
        m_dragChannel = -1;
        unsetCursor();
    }
    QWidget::mouseReleaseEvent(event);
}

void ScopeView::setSamples(int channel, const QVector<double> &samples,
                           double sampleRate, double triggerPoint)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].rawSamples = samples;
    m_channels[channel].visible = true;
    m_sampleRate   = sampleRate > 0.0 ? sampleRate : 0.0;
    {
        VSX_TRACE("view.refilter");
        refilterChannel(channel);   // samples = raw, or raw low-passed
    }
    m_triggerPoint = triggerPoint;
    m_lastLength   = samples.size();
    ++m_dataGeneration;
    {
        VSX_TRACE("view.math");
        recomputeMath(channel);
    }
    if (m_persistEnabled)
        accumulatePersistence(channel);
    update();
}

// Sample index <-> screen X, using exactly the same window mapping the trace
// itself is drawn with, so a cursor always lines up with the sample it claims
// to be at regardless of the current pan or zoom.
double ScopeView::sampleIndexToX(const QRectF &plot, double sampleIndex) const
{
    const int total = std::max(m_lastLength, 1);
    const int start  = windowStart(total);
    const int length = std::max(2, std::min(windowLength(), total - start));
    return plot.left() + plot.width() * (sampleIndex - start) / double(length);
}

double ScopeView::sampleIndexAtX(const QRectF &plot, double x) const
{
    const int total = std::max(m_lastLength, 1);
    const int start  = windowStart(total);
    const int length = std::max(2, std::min(windowLength(), total - start));
    if (plot.width() <= 0.0)
        return double(start);
    return start + (x - plot.left()) / plot.width() * length;
}

// The exact mathematical inverse of valueToY(): given a screen Y, the raw
// (pre-probe) voltage that would have been drawn there. Verified against
// valueToY() by round-tripping random inputs before relying on it here.
double ScopeView::yToValue(const QRectF &plot, const Channel &c, double y) const
{
    const double perDivision = c.voltsPerDiv / c.scale;
    const double divisions = (plot.center().y() - y) / pixelsPerDiv(plot);
    const double signal = divisions * perDivision + c.windowCentre - c.positionVolts;
    const double sign = c.inverted ? -1.0 : 1.0;
    const double probed = (signal - c.offset) * sign;
    if (qFuzzyIsNull(c.probeGain))
        return 0.0;
    return (probed - c.probeOffset) / c.probeGain;
}

// valueToY()/yToValue()'s equivalent for the math trace, whose vertical
// mapping is mathRange()'s (mid, scale) rather than a channel's probe/page —
// extracted here so drawMathTrace() and the ruler code share one definition
// of "where math's zero line sits" instead of two that could drift apart.
double ScopeView::mathValueToY(const QRectF &plot, double value) const
{
    const MathRange r = mathRange(plot);
    const double top = plot.top() - 4.0;
    const double bot = plot.bottom() + 4.0;
    return std::clamp(plot.center().y() - (value - r.mid) * r.scale, top, bot);
}

double ScopeView::mathYToValue(const QRectF &plot, double y) const
{
    const MathRange r = mathRange(plot);
    if (qFuzzyIsNull(r.scale))
        return r.mid;
    return r.mid - (y - plot.center().y()) / r.scale;
}

// The same reference point drawAxes() itself measures from (trigger instant,
// or the absolute record origin), so a cursor's time label always agrees with
// the axis it sits on. Derived and checked algebraically against drawAxes'
// own offsetDivs formula before relying on it here.
double ScopeView::sampleIndexToTime(double sampleIndex) const
{
    if (m_sampleRate <= 0.0)
        return 0.0;
    if (m_hasAbsoluteTimeOrigin)
        return m_absoluteTimeOrigin + sampleIndex / m_sampleRate;
    const double anchor = viewAnchor(std::max(m_lastLength, 1));
    return (sampleIndex - anchor) / m_sampleRate;
}

QRectF ScopeView::rulerBoxRect(const QRectF &plot) const
{
    constexpr double kWidth   = 280.0;
    constexpr double kTitleH  = 20.0;
    constexpr double kHeaderH = 18.0;
    constexpr double kRowH    = 16.0;
    constexpr double kPad     = 6.0;

    QPointF pos = m_rulers.boxPos;
    if (pos.x() < 0.0)
        pos = QPointF(plot.left() + 24.0, plot.top() + 24.0);

    if (m_rulers.boxMinimized) {
        constexpr double kMinimizedWidth = kWidth * 0.8;   // 20% narrower
        pos.setX(std::clamp(pos.x(), plot.left(), std::max(plot.left(), plot.right() - kMinimizedWidth)));
        pos.setY(std::clamp(pos.y(), plot.top(), std::max(plot.top(), plot.bottom() - kTitleH)));
        return QRectF(pos, QSizeF(kMinimizedWidth, kTitleH));
    }

    // Tall enough for every row actually showing — a table, not a fixed
    // handful of lines, since any number of channels can be on at once.
    int rows = m_rulers.timeEnabled ? 2 : 0;   // T row + frequency row
    if (m_rulers.timeEnabled && m_engine.enabled && m_engine.cycleStarts.size() >= 2)
        ++rows;                                 // crank-angle row
    const int perChannel = rulersTracking() ? 2 : 1;   // track mode adds the slope row
    for (int ch = 0; ch < MaxChannels; ++ch) {
        if (m_rulers.channels[ch].enabled && m_channels[ch].visible)
            rows += perChannel;
    }
    if (m_rulers.math.enabled && m_math.enabled)
        rows += perChannel;
    rows = std::max(rows, 1);   // room for "No cursors selected"

    const double height = kTitleH + kHeaderH + rows * kRowH + kPad;

    // Clamped for display/hit-testing only — the stored position is left
    // alone, so a transient window resize does not permanently discard where
    // the user actually put it.
    pos.setX(std::clamp(pos.x(), plot.left(), std::max(plot.left(), plot.right() - kWidth)));
    pos.setY(std::clamp(pos.y(), plot.top(), std::max(plot.top(), plot.bottom() - height)));
    return QRectF(pos, QSizeF(kWidth, height));
}

QRectF ScopeView::rulerMinimizeRect(const QRectF &plot) const
{
    const QRectF box = rulerBoxRect(plot);
    return QRectF(box.right() - 40.0, box.top() + 2.0, 16.0, 16.0);
}

QRectF ScopeView::rulerCloseRect(const QRectF &plot) const
{
    const QRectF box = rulerBoxRect(plot);
    return QRectF(box.right() - 20.0, box.top() + 2.0, 16.0, 16.0);
}

// In the gutter directly above the plot (y between 0 and kTopGutter),
// not inside plotRect() itself — deliberately outside the region a plain
// drag turns into a pan. The previous position, inside the plot's own
// top-right corner, sat squarely inside that same draggable area: a pan
// gesture that happened to start near that corner was being caught by
// these buttons' own click handling instead of ever starting a pan,
// which is what made left/right dragging look like it was "jumping
// between zoom levels" instead of panning. This gutter band is otherwise
// empty — the per-channel "V" axis labels live in the side gutters, to
// the left of plot.left() and to the right of plot.right(), never in the
// strip directly above the plot itself.
QRectF ScopeView::zoomBackRect(const QRectF &plot) const
{
    constexpr double kSize = 22.0, kGap = 4.0, kMargin = 6.0;
    return QRectF(plot.right() - kMargin - kSize * 2.0 - kGap, 3.0, kSize, kSize);
}

QRectF ScopeView::zoomForwardRect(const QRectF &plot) const
{
    constexpr double kSize = 22.0, kMargin = 6.0;
    return QRectF(plot.right() - kMargin - kSize, 3.0, kSize, kSize);
}

void ScopeView::setRulersEnabled(bool enabled)
{
    m_rulers.enabled = enabled;
    if (enabled && !m_rulers.initialised) {
        // Picked from whatever is actually on screen right now, so the
        // cursors appear somewhere visible and useful rather than at a fixed
        // location that might land off-window entirely. Stored relative to
        // the anchor immediately, same as every other use of these offsets.
        const QRectF plot = plotRect();
        const double anchor = viewAnchor(std::max(m_lastLength, 1));
        m_rulers.t1Offset = sampleIndexAtX(plot, plot.left() + plot.width() * 0.3) - anchor;
        m_rulers.t2Offset = sampleIndexAtX(plot, plot.left() + plot.width() * 0.7) - anchor;

        // Only the first visible channel starts with its cursors on — a
        // sensible, immediately-useful default — the rest are added from the
        // settings panel, since every channel can be on at once now.
        for (int ch = 0; ch < MaxChannels; ++ch) {
            if (m_channels[ch].visible) {
                m_rulers.channels[ch].enabled = true;
                m_rulers.channels[ch].v1 = yToValue(plot, m_channels[ch], plot.top() + plot.height() * 0.3);
                m_rulers.channels[ch].v2 = yToValue(plot, m_channels[ch], plot.top() + plot.height() * 0.7);
                break;
            }
        }
        m_rulers.initialised = true;
    }
    m_rulerDrag = kNoRulerDrag;
    update();
}

void ScopeView::setRulersTimeEnabled(bool enabled)
{
    const bool wasEnabled = m_rulers.timeEnabled;
    m_rulers.timeEnabled = enabled;
    if (enabled && !wasEnabled) {
        // Re-centred on every re-enable, not just the very first time ever
        // (see setRulersEnabled()'s own one-shot placement, gated on
        // m_rulers.initialised) — a pair left wherever it was before being
        // turned off could easily no longer even be on screen by the next
        // time they're turned back on, especially after zooming or panning
        // while they were off. Same 30%/70% spread setRulersEnabled() and
        // setRulersChannelEnabled() both already use, not a fixed dead
        // centre for both — symmetric around the middle, but visibly two
        // separate cursors from the moment they appear rather than
        // starting stacked on top of each other.
        const QRectF plot = plotRect();
        const double anchor = viewAnchor(std::max(m_lastLength, 1));
        m_rulers.t1Offset = sampleIndexAtX(plot, plot.left() + plot.width() * 0.3) - anchor;
        m_rulers.t2Offset = sampleIndexAtX(plot, plot.left() + plot.width() * 0.7) - anchor;
    }
    update();
}

void ScopeView::setRulersChannelEnabled(int channel, bool enabled)
{
    const bool isMath = channel == RulersMathChannel;
    RulerState::ChannelCursor &cursor = isMath ? m_rulers.math : m_rulers.channels[channel];
    const bool wasEnabled = cursor.enabled;
    cursor.enabled = enabled;

    if (enabled && !wasEnabled) {
        // Freshly turned on: give it a sensible starting position rather
        // than whatever (0, 0) would map to, same idea as the initial
        // placement when rulers are first enabled at all.
        const QRectF plot = plotRect();
        if (isMath) {
            cursor.v1 = mathYToValue(plot, plot.top() + plot.height() * 0.3);
            cursor.v2 = mathYToValue(plot, plot.top() + plot.height() * 0.7);
        } else {
            cursor.v1 = yToValue(plot, m_channels[channel], plot.top() + plot.height() * 0.3);
            cursor.v2 = yToValue(plot, m_channels[channel], plot.top() + plot.height() * 0.7);
        }
    }
    update();
}

bool ScopeView::rulersChannelEnabled(int channel) const
{
    return channel == RulersMathChannel ? m_rulers.math.enabled
                                        : m_rulers.channels[channel].enabled;
}

void ScopeView::setRulersBoxMinimized(bool minimized)
{
    m_rulers.boxMinimized = minimized;
    update();
}

void ScopeView::setZoomHistoryAvailable(bool canGoBack, bool canGoForward)
{
    if (m_zoomHistoryCanBack == canGoBack && m_zoomHistoryCanForward == canGoForward)
        return;
    m_zoomHistoryCanBack = canGoBack;
    m_zoomHistoryCanForward = canGoForward;
    update();
}

// Adaptive precision, always in volts — no millivolt conversion. Extends
// axisNumber()'s own "fewer decimals as magnitude grows" idea downward: a
// sub-1V reading needs 3 decimals to actually be useful (0.879 vs the 0.88
// that axisNumber's own 2-decimal cap would give), which is exactly why this
// is a dedicated function rather than a call to axisNumber() itself.
static QString rulerVoltsText(double volts)
{
    const double magnitude = std::fabs(volts);
    int decimals;
    if (magnitude >= 100.0)      decimals = 0;
    else if (magnitude >= 10.0)  decimals = 1;
    else if (magnitude >= 1.0)   decimals = 2;
    else                         decimals = 3;
    return QString::number(volts, 'f', decimals);
}

QString ScopeView::ScaleUnit::text(double value) const
{
    return (rulerVoltsText(value * factor) + QLatin1Char(' ') + unit).trimmed();
}

ScopeView::ScaleUnit ScopeView::channelScaleUnit(int channel) const
{
    ScaleUnit su;
    if (channel < 0 || channel >= MaxChannels)
        return su;
    const Channel &c = m_channels[channel];
    su.unit = c.unit;
    // Same rule as drawVoltScale(): millivolts while every tick label stays
    // under 1000 of them.
    const double perDivision = c.voltsPerDiv / c.scale;
    double widest = 0.0;
    for (int i = 0; i <= VerticalDivs; ++i)
        widest = std::max(widest, std::fabs(c.windowCentre - c.positionVolts
                                            + (VerticalDivs / 2.0 - i) * perDivision));
    if (c.unit == QLatin1String("V") && widest < 1.0) {
        su.factor = 1000.0;
        su.unit = QStringLiteral("mV");
    }
    return su;
}

// The channels' rule: millivolts while every tick label stays under 1 V
// (the Math scale stayed in volts: 0.10, 0.08 ... beside channels in mV).
ScopeView::ScaleUnit ScopeView::mathScaleUnit() const
{
    ScaleUnit su;
    su.unit = mathUnit();
    if (su.unit != QLatin1String("V"))
        return su;
    const MathRange r = mathRange(plotRect());
    if (r.scale > 0.0 && std::max(std::fabs(r.lo), std::fabs(r.hi)) < 1.0) {
        su.factor = 1000.0;
        su.unit = QStringLiteral("mV");
    }
    return su;
}

void ScopeView::setRulersTrack(bool on)
{
    if (m_rulers.track == on)
        return;
    m_rulers.track = on;
    update();
}

void ScopeView::setSamplesArePairs(bool pairs)
{
    if (m_samplesArePairs == pairs)
        return;
    m_samplesArePairs = pairs;
    update();
}

bool ScopeView::traceValueAt(int channel, double index, double *raw) const
{
    const QVector<double> &s = channel == MaxChannels ? m_math.samples : m_channels[channel].samples;
    const int n = int(s.size());
    if (n == 0 || !std::isfinite(index) || index < 0.0 || index > double(n - 1))
        return false;
    double v;
    if (m_samplesArePairs) {
        const int p = std::clamp(int(index) & ~1, 0, std::max(0, n - 2));
        v = 0.5 * (s[p] + s[std::min(p + 1, n - 1)]);
    } else {
        const int i0 = int(std::floor(index));
        const int i1 = std::min(i0 + 1, n - 1);
        v = s[i0] + (s[i1] - s[i0]) * (index - double(i0));    // between samples: interpolated
    }
    if (!std::isfinite(v))
        return false;
    *raw = v;
    return true;
}

ScopeView::RulerLayout ScopeView::computeRulerLayout(const QRectF &plot) const
{
    RulerLayout layout;
    if (!m_rulers.enabled)
        return layout;

    QFont f = font();
    f.setPointSizeF(std::max(Fonts::scaled(7.0), f.pointSizeF() - Fonts::scaled(1.5)));
    const QFontMetrics fm(f);
    const double flagH = fm.height() + 4.0;

    // Fixed widths, computed once from a worst-case template rather than the
    // actual text — every time flag shares one width, and every voltage flag
    // sharing a side shares one width, so a flag never resizes as its value's
    // digit count changes while dragging.
    // Time texts are absolute and can be long ("10971.35 ms"): the width is
    // the widest the view can show (its edges, digits as zeros), so it only
    // changes with the view, never while a cursor is dragged.
    double timeFlagW = fm.horizontalAdvance(QStringLiteral("-000.00 ms")) + 12.0;
    if (m_rulers.timeEnabled) {
        for (double x : { plot.left(), plot.right() }) {
            QString t = formatAxisTime(sampleIndexToTime(sampleIndexAtX(plot, x)));
            for (QChar &c : t) {
                if (c.isDigit())
                    c = QLatin1Char('0');
            }
            timeFlagW = std::max(timeFlagW, fm.horizontalAdvance(QStringLiteral("-") + t) + 12.0);
        }
    }
    layout.flagHeight = flagH;
    layout.timeFlagWidth = timeFlagW;
    constexpr double kPad = 4.0;   // matches drawAxes()/drawMathScale()'s own kScalePad
    const double voltsTemplateW = fm.horizontalAdvance(QStringLiteral("-000.000")) + 12.0;
    // CH1/CH3/CH4 and Math share the left side, but Math's column is
    // narrower than CH1's own gutter — using each one's own limit made CH1's
    // flag visibly wider than Math's for the same text. Both use the
    // narrower of the two so they always match.
    const double leftFlagMaxW = std::max(16.0, std::min(double(kScaleGutter), double(kMathColumnWidth)) - 4.0);

    layout.timeOn = m_rulers.timeEnabled;
    if (layout.timeOn) {
        const double anchor = viewAnchor(std::max(m_lastLength, 1));
        layout.t1x = sampleIndexToX(plot, anchor + m_rulers.t1Offset);
        layout.t2x = sampleIndexToX(plot, anchor + m_rulers.t2Offset);
        layout.t1Text = formatAxisTime(sampleIndexToTime(anchor + m_rulers.t1Offset));
        layout.t2Text = formatAxisTime(sampleIndexToTime(anchor + m_rulers.t2Offset));
        layout.t1PanelText = formatPanelTime(sampleIndexToTime(anchor + m_rulers.t1Offset));
        layout.t2PanelText = formatPanelTime(sampleIndexToTime(anchor + m_rulers.t2Offset));
        // Off-screen: the cursor's true position is currently outside the
        // visible plot (zoomed to a region that does not include it). Its
        // flag stays reachable regardless — pinned flush against whichever
        // edge it went past, rather than left at its true, potentially
        // far-off-screen x — so it can always be dragged back into view.
        // Flush against the edge, not centred exactly on it: centring
        // would clip the flag in half rather than keep the whole thing
        // visible.
        layout.t1OffScreen = layout.t1x < plot.left() || layout.t1x > plot.right();
        layout.t2OffScreen = layout.t2x < plot.left() || layout.t2x > plot.right();
        const double t1FlagX = layout.t1x < plot.left()  ? plot.left()  + timeFlagW / 2.0
                              : layout.t1x > plot.right() ? plot.right() - timeFlagW / 2.0
                                                          : layout.t1x;
        const double t2FlagX = layout.t2x < plot.left()  ? plot.left()  + timeFlagW / 2.0
                              : layout.t2x > plot.right() ? plot.right() - timeFlagW / 2.0
                                                          : layout.t2x;
        // Centred on the line but kept within the plot's width (a line at an
        // edge left half its flag outside).
        const auto timeFlagLeft = [&](double x) {
            return std::clamp(x - timeFlagW / 2.0, plot.left(), std::max(plot.left(), plot.right() - timeFlagW));
        };
        layout.t1Flag = QRectF(timeFlagLeft(t1FlagX), plot.bottom() + 3.0, timeFlagW, flagH);
        layout.t2Flag = QRectF(timeFlagLeft(t2FlagX), plot.bottom() + 3.0, timeFlagW, flagH);
    }

    // Every channel (and math) is independent — any subset can be on at once.
    // Each one's flags sit on the same side as that channel's own voltage
    // scale, not lumped together on the left: CH1 (and CH3/CH4, which never
    // get a drawn scale of their own) on the left, CH2 on the right where its
    // own scale actually is, Math in its own column beside CH1's.
    for (int ch = 0; ch <= MaxChannels; ++ch) {
        const bool isMath = ch == MaxChannels;
        const bool enabled = isMath ? m_rulers.math.enabled : m_rulers.channels[ch].enabled;
        const bool available = isMath ? m_math.enabled : m_channels[ch].visible;
        if (!enabled || !available)
            continue;

        RulerChannelLayout &cl = layout.channels[ch];
        cl.on = true;
        double v1raw = isMath ? m_rulers.math.v1 : m_rulers.channels[ch].v1;
        double v2raw = isMath ? m_rulers.math.v2 : m_rulers.channels[ch].v2;
        if (rulersTracking()) {
            // Track mode: the trace's own value under each time ruler.
            const double anchor = viewAnchor(std::max(m_lastLength, 1));
            cl.v1Valid = traceValueAt(ch, anchor + m_rulers.t1Offset, &v1raw);
            cl.v2Valid = traceValueAt(ch, anchor + m_rulers.t2Offset, &v2raw);
        }

        cl.colour = isMath ? m_math.color : m_channels[ch].color;
        cl.v1y = isMath ? mathValueToY(plot, v1raw) : valueToY(plot, m_channels[ch], v1raw);
        cl.v2y = isMath ? mathValueToY(plot, v2raw) : valueToY(plot, m_channels[ch], v2raw);
        // The value the scale shows at the flag's height — probe, inversion
        // and offset included (the probe alone disagreed with the scale on
        // an inverted or offset channel) — in the scale's own unit.
        const auto shown = [&](double raw) {
            if (isMath)
                return raw;
            const Channel &c = m_channels[ch];
            return (raw * c.probeGain + c.probeOffset) * (c.inverted ? -1.0 : 1.0) + c.offset;
        };
        cl.v1Probed = shown(v1raw);
        cl.v2Probed = shown(v2raw);
        const ScaleUnit su = isMath ? mathScaleUnit() : channelScaleUnit(ch);
        cl.unitFactor = su.factor;
        cl.unit = su.unit;
        // The flags carry just the number, in the scale's mV / V (the unit
        // is on the scale beside them); the readout box adds the unit.
        cl.v1Text = rulerVoltsText(cl.v1Probed * su.factor);
        cl.v2Text = rulerVoltsText(cl.v2Probed * su.factor);

        // Each flag spans its own gutter (1 px in from each edge): CH1 the
        // scale gutter, Math its column, CH2 the right gutter. Still fixed per
        // gutter, so a flag never resizes while its value changes.
        double flagX, flagW;
        if (isMath) {
            flagX = kScaleGutter + 1.0;
            flagW = std::max(16.0, double(kMathColumnWidth) - 2.0);
        } else if (ch == 1) {
            flagX = plot.right() + 1.0;
            flagW = std::max(16.0, rightGutter() - 2.0);
        } else {
            flagX = 1.0;
            flagW = std::max(16.0, double(kScaleGutter) - 2.0);
        }
        Q_UNUSED(voltsTemplateW);
        Q_UNUSED(leftFlagMaxW);
        // Centred on the line but kept within the plot's height.
        const auto voltsFlagTop = [&](double y) {
            return std::clamp(y - flagH / 2.0, plot.top(), std::max(plot.top(), plot.bottom() - flagH));
        };
        cl.v1Flag = QRectF(flagX, voltsFlagTop(cl.v1y), flagW, flagH);
        cl.v2Flag = QRectF(flagX, voltsFlagTop(cl.v2y), flagW, flagH);
    }

    layout.box = rulerBoxRect(plot);
    layout.closeRect = rulerCloseRect(plot);
    layout.minimizeRect = rulerMinimizeRect(plot);
    return layout;
}

int ScopeView::hitTestRulers(const QPointF &pos, const QRectF &plot) const
{
    if (!m_rulers.enabled)
        return kNoRulerDrag;

    const RulerLayout layout = computeRulerLayout(plot);

    // The box is a deliberate, filled target — a direct hit there wins
    // outright, before the thin cursor lines or flags are even considered.
    // While minimized it is just the title strip (rulerBoxRect() already
    // accounts for that) — but the cursors themselves stay live either way;
    // minimizing collapses the readout table, not the rulers.
    if (layout.box.contains(pos))
        return kRulerBox;

    // A flag is a real target in its own right, not just the thin line
    // through its centre — clicking anywhere in its rectangle grabs the
    // cursor, which is what "the flag has a tiny clickable area" needed.
    constexpr double kTolerance = 7.0;
    int best = kNoRulerDrag;
    double bestDistance = kTolerance;
    auto consider = [&](int id, double distance) {
        if (distance < bestDistance) {
            bestDistance = distance;
            best = id;
        }
    };
    if (layout.timeOn) {
        if (layout.t1Flag.contains(pos)) return kRulerT1;
        if (layout.t2Flag.contains(pos)) return kRulerT2;
        consider(kRulerT1, std::fabs(pos.x() - layout.t1x));
        consider(kRulerT2, std::fabs(pos.x() - layout.t2x));
    }
    for (int ch = 0; ch <= MaxChannels; ++ch) {
        const RulerChannelLayout &cl = layout.channels[ch];
        if (!cl.on || rulersTracking())     // tracked: they follow the trace, nothing to drag
            continue;
        if (cl.v1Flag.contains(pos)) return rulerVoltsDragId(ch, false);
        if (cl.v2Flag.contains(pos)) return rulerVoltsDragId(ch, true);
        consider(rulerVoltsDragId(ch, false), std::fabs(pos.y() - cl.v1y));
        consider(rulerVoltsDragId(ch, true),  std::fabs(pos.y() - cl.v2y));
    }
    return best;
}

// Where this annotation's arrow tip currently renders — the same
// sample-offset-from-anchor / raw-volts-via-valueToY() math a Ruler voltage
// cursor already uses, so panning or zooming slides the tip along the trace
// exactly the way a ruler cursor would, rather than leaving it behind.
QPointF ScopeView::annotationTipPos(const AnnotationState &a, const QRectF &plot) const
{
    if (!a.active) {
        // Not pointed at anything in particular yet — hangs straight down
        // from wherever the box currently is, recomputed fresh every time
        // rather than stored, so it never "sticks" to whatever arbitrary
        // sample a freshly-placed box happened to land near. Only starts
        // tracking a real sample once the user deliberately drags it onto
        // the trace — see mousePressEvent(). Only ever drawn/hittable while
        // this annotation is selected — see drawAnnotations()/
        // hitTestAnnotations() — so this value is meaningless otherwise.
        constexpr double kDefaultArrowLength = 28.0;
        const QRect boxGeom = a.box->geometry();
        return QPointF(boxGeom.center().x(), boxGeom.bottom() + kDefaultArrowLength);
    }
    const double anchor = viewAnchor(std::max(m_lastLength, 1));
    const double x = sampleIndexToX(plot, anchor + a.timeOffset);
    const double y = (a.channel == RulersMathChannel)
        ? mathValueToY(plot, a.volts)
        : (a.channel >= 0 && a.channel < MaxChannels)
            ? valueToY(plot, m_channels[a.channel], a.volts)
            : plot.center().y();
    return QPointF(x, y);
}

void ScopeView::clearAnnotations()
{
    while (!m_annotations.isEmpty())
        removeAnnotation(m_annotations.size() - 1);
    m_selectedAnnotation = -1;
    update();
}

QVariantList ScopeView::annotationsState() const
{
    QVariantList list;
    const double w = std::max(1, width()), h = std::max(1, height());
    for (const AnnotationState &a : m_annotations) {
        if (!a.box)
            continue;
        QVariantMap m;
        m.insert(QStringLiteral("text"), a.box->text());
        m.insert(QStringLiteral("x"), a.box->x() / w);
        m.insert(QStringLiteral("y"), a.box->y() / h);
        m.insert(QStringLiteral("w"), a.box->width());
        m.insert(QStringLiteral("h"), a.box->height());
        m.insert(QStringLiteral("active"), a.active);
        m.insert(QStringLiteral("channel"), a.channel);
        m.insert(QStringLiteral("timeOffset"), a.timeOffset);
        m.insert(QStringLiteral("volts"), a.volts);
        list.append(m);
    }
    return list;
}

void ScopeView::restoreAnnotations(const QVariantList &list)
{
    for (const QVariant &v : list) {
        const QVariantMap m = v.toMap();
        addAnnotation(false);   // the same box, wiring and bookkeeping, without taking focus
        AnnotationState &a = m_annotations.last();
        a.box->setText(m.value(QStringLiteral("text")).toString());
        const int bw = std::max(AnnotationBox::kMinWidth, m.value(QStringLiteral("w")).toInt());
        const int bh = std::max(AnnotationBox::kMinHeight, m.value(QStringLiteral("h")).toInt());
        a.box->resize(bw, bh);
        const int x = int(m.value(QStringLiteral("x")).toDouble() * width());
        const int y = int(m.value(QStringLiteral("y")).toDouble() * height());
        a.box->move(std::clamp(x, 0, std::max(0, width() - bw)),
                    std::clamp(y, 0, std::max(0, height() - bh)));
        a.active = m.value(QStringLiteral("active")).toBool();
        a.channel = m.value(QStringLiteral("channel")).toInt();
        a.timeOffset = m.value(QStringLiteral("timeOffset")).toDouble();
        a.volts = m.value(QStringLiteral("volts")).toDouble();
    }
    m_selectedAnnotation = -1;
    update();
}

void ScopeView::addAnnotation(bool focus)
{
    // First currently visible channel; Math if that is the only thing
    // showing; channel 0 as a last resort — this is only what the arrow
    // *would* track once activated (see annotationTipPos()); it plays no
    // part in where the box or its default arrow appear.
    int channel = -1;
    for (int ch = 0; ch < m_channelCount; ++ch) {
        if (m_channels[ch].visible) { channel = ch; break; }
    }
    if (channel < 0 && m_math.enabled)
        channel = RulersMathChannel;
    if (channel < 0)
        channel = 0;

    auto *box = new AnnotationBox(this);
    box->setAccentColor(Theme::accent());
    box->setText(QString());

    // Staggers successive drops so repeated clicks build a visible cascade
    // rather than an exact overlapping stack, cycling back to the start
    // rather than walking indefinitely off the plot.
    constexpr int kStaggerStep  = 24;
    constexpr int kStaggerCycle = 6;
    const int stagger = (m_annotations.size() % kStaggerCycle) * kStaggerStep;
    const QRectF plot = plotRect();
    box->move(int(plot.center().x() - box->width() / 2.0) + stagger,
             int(plot.center().y() - box->height() / 2.0) - 20 + stagger);
    box->show();
    if (focus)
        box->focusText();

    AnnotationState state;
    state.box        = box;
    state.active     = false;   // hangs straight down until dragged onto the trace
    state.channel    = channel;
    state.timeOffset = 0.0;
    state.volts      = 0.0;
    m_annotations.append(state);

    connect(box, &AnnotationBox::changed, this, [this] { update(); });
    connect(box, &AnnotationBox::selectionChanged, this, [this, box](bool selected) {
        if (selected) {
            for (int i = 0; i < m_annotations.size(); ++i) {
                if (m_annotations[i].box == box) {
                    m_selectedAnnotation = i;
                    update();
                    break;
                }
            }
            return;
        }
        // Deferred, not immediate: this box's own trash/resize/move chrome
        // is painted by ScopeView outside the box's actual widget
        // rectangle, so clicking it moves Qt's keyboard focus to ScopeView
        // itself first — fully off this text edit — before ScopeView's own
        // mousePressEvent ever gets a chance to read m_selectedAnnotation
        // and act on that same click. Clearing it synchronously here would
        // make that a race the click always loses. Waiting one event-loop
        // turn lets that handling run first; if nothing has since selected
        // a different annotation, only then is this one actually cleared.
        QTimer::singleShot(0, this, [this, box] {
            for (int i = 0; i < m_annotations.size(); ++i) {
                if (m_annotations[i].box == box && m_selectedAnnotation == i) {
                    m_selectedAnnotation = -1;
                    update();
                }
            }
        });
    });

    update();
}

void ScopeView::removeAnnotation(int index)
{
    if (index < 0 || index >= m_annotations.size())
        return;

    AnnotationBox *box = m_annotations[index].box;
    m_annotations.removeAt(index);

    // Every index tracked elsewhere needs the same shift the removal above
    // just applied to the list itself, or it will silently point at the
    // wrong annotation (or a stale one) from here on.
    auto fixup = [index](int &i) {
        if (i == index)
            i = -1;
        else if (i > index)
            --i;
    };
    fixup(m_selectedAnnotation);
    fixup(m_annotationDrag);
    fixup(m_annotationMoveDrag);
    fixup(m_annotationResizeDrag);

    box->hide();
    box->deleteLater();
    update();
}

QRectF ScopeView::annotationTrashRect(const AnnotationState &a) const
{
    const QRect g = a.box->geometry();
    constexpr double kSize = 22.0, kGap = 6.0;
    return QRectF(g.right() + kGap, g.center().y() - kSize / 2.0, kSize, kSize);
}

QRectF ScopeView::annotationMoveRect(const AnnotationState &a) const
{
    const QRect g = a.box->geometry();
    constexpr double kSize = 22.0, kGap = 6.0;   // matches annotationTrashRect() exactly
    return QRectF(g.center().x() - kSize / 2.0, g.top() - kGap - kSize, kSize, kSize);
}

std::array<QPointF, 8> ScopeView::annotationHandlePoints(const AnnotationState &a) const
{
    // Pushed outward from the box's own edge, not centred on it: a handle
    // centred exactly on the border would have its inner half painted over
    // by the box widget itself, which always renders on top of this (its
    // parent's) output — see AnnotationBox's own header comment.
    const QRect g = a.box->geometry();
    constexpr double kOffset = 4.0;
    return {
        QPointF(g.left() - kOffset,  g.top() - kOffset),
        QPointF(g.center().x(),      g.top() - kOffset),
        QPointF(g.right() + kOffset, g.top() - kOffset),
        QPointF(g.right() + kOffset, g.center().y()),
        QPointF(g.right() + kOffset, g.bottom() + kOffset),
        QPointF(g.center().x(),      g.bottom() + kOffset),
        QPointF(g.left() - kOffset,  g.bottom() + kOffset),
        QPointF(g.left() - kOffset,  g.center().y()),
    };
}

int ScopeView::hitTestAnnotationHandles(const AnnotationState &a, const QPointF &pos) const
{
    constexpr double kHandleSize = 9.0;
    const auto points = annotationHandlePoints(a);
    for (int i = 0; i < 8; ++i) {
        const QRectF r(points[i].x() - kHandleSize / 2.0, points[i].y() - kHandleSize / 2.0,
                       kHandleSize, kHandleSize);
        if (r.contains(pos))
            return i;
    }
    return -1;
}

Qt::CursorShape ScopeView::annotationHandleCursor(int handle) const
{
    switch (handle) {
    case 0: case 4: return Qt::SizeFDiagCursor;   // top-left / bottom-right
    case 2: case 6: return Qt::SizeBDiagCursor;   // top-right / bottom-left
    case 1: case 5: return Qt::SizeVerCursor;     // top-mid / bottom-mid
    case 3: case 7: return Qt::SizeHorCursor;     // right-mid / left-mid
    default:        return Qt::ArrowCursor;
    }
}

void ScopeView::drawAnnotations(QPainter &p, const QRectF &plot) const
{
    if (m_annotations.isEmpty())
        return;

    p.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < m_annotations.size(); ++i) {
        const AnnotationState &a = m_annotations[i];
        if (!a.box)
            continue;
        const bool selected = (i == m_selectedAnnotation);

        // The default, not-yet-aimed pointer only shows while selected: it
        // never lingers as a stale, meaningless arrow once you click away
        // from a box you never dragged one out of.
        if (a.active || selected) {
            const QPointF tip   = annotationTipPos(a, plot);
            const QColor colour = a.box->accentColor();
            const QRect boxGeom = a.box->geometry();

            // From the box's own centre, not an edge: the box widget paints
            // on top of this output regardless, so whatever part of the
            // line falls inside its rectangle is simply covered — the
            // visible remainder naturally emerges from whichever edge faces
            // the tip, in any direction, with no separate per-direction
            // anchor-point logic needed.
            const QPointF boxAnchor(boxGeom.center());

            p.setPen(QPen(colour, 1.3));
            p.drawLine(boxAnchor, tip);

            const double angle = std::atan2(tip.y() - boxAnchor.y(), tip.x() - boxAnchor.x());
            constexpr double kHeadAngle = 0.45;   // radians, ~26 degrees either side
            constexpr double kHeadLen   = 8.0;
            p.drawLine(tip, tip - QPointF(std::cos(angle - kHeadAngle), std::sin(angle - kHeadAngle)) * kHeadLen);
            p.drawLine(tip, tip - QPointF(std::cos(angle + kHeadAngle), std::sin(angle + kHeadAngle)) * kHeadLen);
        }

        if (!selected)
            continue;

        // Resize handles: small filled squares at each corner and edge
        // midpoint — the standard "this is selected" affordance, in a
        // neutral selection colour rather than the annotation's own accent,
        // so it always reads as chrome rather than as part of the note.
        const QColor kHandleColour(0x5b, 0xb8, 0xff);
        constexpr double kHandleSize = 7.0;
        p.setPen(QPen(QColor(0x0d, 0x0f, 0x12), 1.0));
        p.setBrush(kHandleColour);
        for (const QPointF &pt : annotationHandlePoints(a)) {
            p.drawRect(QRectF(pt.x() - kHandleSize / 2.0, pt.y() - kHandleSize / 2.0,
                              kHandleSize, kHandleSize));
        }

        // Move handle and trash: the same on-canvas-glyph button, drawn
        // identically (rounded rect, same fill/border treatment) — the same
        // technique the Ruler close/minimize buttons already use (see
        // rulerCloseRect()'s caller), just with vendored Font Awesome
        // glyphs in place of plain Unicode characters. Only the glyph and
        // position differ between the two, so both go through one helper
        // rather than two near-duplicate blocks that could drift apart.
        auto drawGlyphButton = [&](const QRectF &r, const QString &glyphName) {
            p.setBrush([]{ QColor c = Theme::elevatedBackground(); c.setAlpha(235); return c; }());
            p.setPen(QPen(a.box->accentColor(), 1.0));
            p.drawRoundedRect(r, 4, 4);
            if (Icons::hasSvg(glyphName)) {
                const int side = int(std::lround(r.height() * 0.66));
                p.drawPixmap(QPointF(r.center().x() - side / 2.0, r.center().y() - side / 2.0),
                             Icons::pixmap(glyphName, a.box->accentColor(), side));
            } else if (Icons::available()) {
                // Restored right after: everything drawn later in this same
                // paintEvent() (axis labels, readouts, the status strip)
                // shares this one QPainter, so leaving the font switched to
                // FontAwesome here would silently turn all of that into
                // icon glyphs too — this is what made the whole plot's text
                // change font whenever an annotation became selected.
                const QFont previousFont = p.font();
                QFont font(Icons::fontFamily());
                font.setPixelSize(int(r.height() * 0.55));
                p.setFont(font);
                p.setPen(a.box->accentColor());
                p.drawText(r, Qt::AlignCenter, Icons::glyph(glyphName));
                p.setFont(previousFont);
            }
        };
        drawGlyphButton(annotationMoveRect(a), QStringLiteral("arrows-up-down-left-right"));
        drawGlyphButton(annotationTrashRect(a), QStringLiteral("trash"));
        p.setBrush(Qt::NoBrush);
    }
}

int ScopeView::hitTestAnnotations(const QPointF &pos, const QRectF &plot) const
{
    constexpr double kHitRadius = 10.0;
    int best = -1;
    double bestDistance = kHitRadius;
    // Most recently added (topmost, since AnnotationBox children stack in
    // creation order) wins ties, matching how overlapping child widgets
    // would themselves resolve a click.
    for (int i = m_annotations.size() - 1; i >= 0; --i) {
        const AnnotationState &a = m_annotations[i];
        if (!a.active && i != m_selectedAnnotation)
            continue;   // invisible (see drawAnnotations()) — not a valid drag target either
        const double distance = QLineF(pos, annotationTipPos(a, plot)).length();
        if (distance <= bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

void ScopeView::setMathChannel(bool enabled, MathOp op, int sourceA, int sourceB)
{
    m_math.enabled = enabled;
    m_math.op      = op;
    m_math.sourceA = std::clamp(sourceA, 0, MaxChannels - 1);
    m_math.sourceB = std::clamp(sourceB, 0, MaxChannels - 1);
    recomputeMath(m_math.sourceA);
    recomputeMath(m_math.sourceB);
    update();
}

void ScopeView::setMathVoltsPerDiv(double perDiv)
{
    m_math.voltsPerDiv = perDiv;
    update();
}


void ScopeView::setMathPositionVolts(double volts)
{
    m_math.positionVolts = volts;
    update();
}

int ScopeView::addReferenceFromChannel(int channel)
{
    if (channel < 0 || channel >= MaxChannels)
        return -1;
    int slot = -1;
    for (int i = 0; i < MaxReferences; ++i) {
        if (!m_references[i].occupied) { slot = i; break; }
    }
    if (slot < 0)
        return -1;

    const Channel &c = m_channels[channel];
    ReferenceState &r = m_references[slot];
    r.occupied = true;
    r.name = QStringLiteral("CH%1").arg(channel + 1);
    r.color = Theme::referenceColor(slot);
    r.unit = c.unit.isEmpty() ? QStringLiteral("V") : c.unit;   // the probe's unit, like its source
    r.sampleRate = m_sampleRate;
    // What a delay of exactly 0 aligns against — see the field's own
    // comment in scopeview.h.
    r.triggerIndex = viewAnchor(std::max(m_lastLength, 1));

    // Bakes in probe gain/offset, inversion, and DC offset — everything
    // valueToY() applies to a real channel before its own volts/div and
    // position ever come into it — so referenceValueAt()'s own mapping
    // can stay as simple as Math's, needing no per-sample transform of
    // its own at draw time.
    r.samples = channelDisplaySamples(channel);

    // Starts out matching what the channel currently shows (its own
    // effective volts/div, page divided by display zoom), so a freshly
    // added reference looks identical to its source at the instant it is
    // added — everything after that is independent and user-adjustable.
    r.voltsPerDiv = c.scale > 0.0 ? c.voltsPerDiv / c.scale : c.voltsPerDiv;
    r.positionVolts = c.positionVolts;
    r.delaySeconds = 0.0;
    update();
    return slot;
}

// See the declaration's own comment in scopeview.h for why this transform
// lives here rather than being duplicated at each of its call sites.
// windowCentre (the hardware input offset) is subtracted out: it is a
// hardware positioning concept with no meaning once a value is just a
// number in a saved array or an exported row.
QVector<double> ScopeView::channelDisplaySamples(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return {};
    const Channel &c = m_channels[channel];
    QVector<double> result(c.samples.size());
    for (int i = 0; i < c.samples.size(); ++i) {
        const double probed = c.samples[i] * c.probeGain + c.probeOffset;
        const double signal = probed * (c.inverted ? -1.0 : 1.0) + c.offset;
        result[i] = signal - c.windowCentre;
    }
    return result;
}

namespace {
const QVector<double> kEmptyChannelSamples;
}

const QVector<double> &ScopeView::channelSamples(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return kEmptyChannelSamples;
    // Raw, unfiltered — this is what a saved .vsxwave stores.
    const Channel &c = m_channels[channel];
    return c.rawSamples.isEmpty() ? c.samples : c.rawSamples;
}

int ScopeView::addReferenceFromData(const QVector<double> &samples, double sampleRate,
                                     double triggerIndex, const QString &name,
                                     const QColor &color, double voltsPerDiv,
                                     double positionVolts, double delaySeconds)
{
    int slot = -1;
    for (int i = 0; i < MaxReferences; ++i) {
        if (!m_references[i].occupied) { slot = i; break; }
    }
    if (slot < 0)
        return -1;

    ReferenceState &r = m_references[slot];
    r.occupied = true;
    r.name = name;
    r.color = color;
    r.unit = QStringLiteral("V");          // (setReferenceUnit() after, from the file)
    r.samples = samples;
    r.sampleRate = sampleRate;
    r.triggerIndex = triggerIndex;
    r.voltsPerDiv = voltsPerDiv;
    r.positionVolts = positionVolts;
    r.delaySeconds = delaySeconds;
    update();
    return slot;
}

// The time axis's unit (us / ms / s from the window, as drawAxes()) and
// decimals for a hundredth of a division, so a cursor reads to where it can
// be placed: seconds to three figures read "10.97 s" across a whole 50 ms
// page beside an axis in ms.
QString ScopeView::formatAxisTime(double seconds) const
{
    const double window = m_secondsPerDiv * HorizontalDivs;
    double scale = 1.0;
    QString unit = QStringLiteral("s");
    if (window < 1e-3) {
        scale = 1e6;
        unit = QStringLiteral("us");
    } else if (window < 1.0) {
        scale = 1e3;
        unit = QStringLiteral("ms");
    }
    const double resolution = m_secondsPerDiv * scale / 100.0;
    const int decimals = resolution > 0.0 ? std::clamp(int(std::ceil(-std::log10(resolution))), 0, 6) : 2;
    return QStringLiteral("%1 %2").arg(seconds * scale, 0, 'f', decimals).arg(unit);
}

QString ScopeView::formatPanelTime(double seconds) const
{
    if (std::fabs(seconds) < 1.0)
        return formatAxisTime(seconds);
    const double resolution = m_secondsPerDiv / 5.0;
    const int decimals = resolution > 0.0 ? std::clamp(int(std::ceil(-std::log10(resolution))), 0, 9) : 3;
    return QStringLiteral("%1 s").arg(seconds, 0, 'f', decimals);
}

void ScopeView::setReferenceUnit(int slot, const QString &unit)
{
    if (slot < 0 || slot >= MaxReferences)
        return;
    m_references[size_t(slot)].unit = unit.isEmpty() ? QStringLiteral("V") : unit;
    update();
}

QString ScopeView::referenceUnit(int slot) const
{
    return slot >= 0 && slot < MaxReferences ? m_references[size_t(slot)].unit : QStringLiteral("V");
}

void ScopeView::removeReference(int slot)
{
    if (slot < 0 || slot >= MaxReferences)
        return;
    m_references[slot] = ReferenceState();
    update();
}

bool ScopeView::referenceOccupied(int slot) const
{
    return slot >= 0 && slot < MaxReferences && m_references[slot].occupied;
}

int ScopeView::referenceCount() const
{
    int count = 0;
    for (const auto &r : m_references)
        count += r.occupied ? 1 : 0;
    return count;
}

QString ScopeView::referenceName(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].name : QString();
}

void ScopeView::setReferenceName(int slot, const QString &name)
{
    if (!referenceOccupied(slot))
        return;
    m_references[slot].name = name;
    update();
}

QColor ScopeView::referenceColor(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].color : QColor();
}

void ScopeView::setReferenceColor(int slot, const QColor &color)
{
    if (!referenceOccupied(slot))
        return;
    m_references[slot].color = color;
    update();
}

double ScopeView::referenceVoltsPerDiv(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].voltsPerDiv : -1.0;
}

void ScopeView::setReferenceVoltsPerDiv(int slot, double voltsPerDiv)
{
    if (!referenceOccupied(slot))
        return;
    m_references[slot].voltsPerDiv = voltsPerDiv;
    update();
}

double ScopeView::referencePositionVolts(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].positionVolts : 0.0;
}

void ScopeView::setReferencePositionVolts(int slot, double positionVolts)
{
    if (!referenceOccupied(slot))
        return;
    m_references[slot].positionVolts = positionVolts;
    update();
}

double ScopeView::referenceDelay(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].delaySeconds : 0.0;
}

void ScopeView::setReferenceDelay(int slot, double delaySeconds)
{
    if (!referenceOccupied(slot))
        return;
    m_references[slot].delaySeconds = delaySeconds;
    update();
}

namespace {
const QVector<double> kEmptyReferenceSamples;
}

const QVector<double> &ScopeView::referenceSamples(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].samples : kEmptyReferenceSamples;
}

double ScopeView::referenceSampleRate(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].sampleRate : 0.0;
}

double ScopeView::referenceTriggerIndex(int slot) const
{
    return referenceOccupied(slot) ? m_references[slot].triggerIndex : 0.0;
}

double ScopeView::displayValue(int channel, double raw) const
{
    if (channel < 0 || channel >= MaxChannels)
        return raw;
    const Channel &c = m_channels[channel];
    const double probed = raw * c.probeGain + c.probeOffset;
    return probed * (c.inverted ? -1.0 : 1.0) + c.offset - c.windowCentre;
}

void ScopeView::setBusRow(int slot, bool visible, const QString &label, const QColor &colour,
                          const QVector<BusAnnotation> &annotations, const QString &message)
{
    if (slot < 0 || slot >= MaxBusRows)
        return;
    BusRow &row = m_busRows[slot];
    row.visible = visible;
    row.label = label;
    row.colour = colour;
    row.message = message;

    // Decoders often report a field AND the parts it is made of (CAN's full
    // extended identifier over its two halves). One row can only show one
    // box per span, so a box wholly inside an earlier, wider one is dropped
    // in favour of the wider. Errors are always kept and drawn on top.
    QVector<BusAnnotation> sorted = annotations;
    std::sort(sorted.begin(), sorted.end(), [](const BusAnnotation &a, const BusAnnotation &b) {
        return a.start < b.start || (a.start == b.start && a.end > b.end);
    });
    row.annotations.clear();
    row.annotations.reserve(sorted.size());
    double coveredTo = -std::numeric_limits<double>::infinity();
    for (const BusAnnotation &a : sorted) {
        if (!a.error && a.end <= coveredTo)
            continue;
        row.annotations.append(a);
        if (!a.error)
            coveredTo = std::max(coveredTo, a.end);
    }
    update();
}

void ScopeView::drawBusRows(QPainter &p, const QRectF &plot) const
{
    int visibleRows = 0;
    for (const BusRow &row : m_busRows)
        visibleRows += row.visible ? 1 : 0;
    if (visibleRows == 0)
        return;

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.save();
    p.setFont(small);
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QFontMetricsF fm(small);
    const double rowH = fm.height() + 8.0;
    const QColor errorColour(255, 70, 70);

    int index = 0;
    for (const BusRow &row : m_busRows) {
        if (!row.visible)
            continue;
        const double top = plot.bottom() - (visibleRows - index) * (rowH + 2.0) - 2.0;
        ++index;
        const QRectF band(plot.left(), top, plot.width(), rowH);
        QColor bg = Theme::plotBackground();
        bg.setAlpha(215);
        p.fillRect(band, bg);

        const double tagW = fm.horizontalAdvance(row.label) + 12.0;
        const QRectF tag(band.left() + 2.0, band.top() + 2.0, tagW, rowH - 4.0);
        const double areaLeft = tag.right() + 4.0;

        // One wording per annotation class across the whole row: each box
        // would otherwise take the longest text that fits it alone, so
        // neighbouring bytes of slightly different widths read "DB 0: 0xc8"
        // next to "Data byte 1: 0x7f". The level used for a class is the
        // one its narrowest visible box can fit (boxes that fit no text at
        // all don't vote); a box that still can't fit it falls back to the
        // next shorter text.
        QHash<QString, int> groupLevel;
        for (const BusAnnotation &a : row.annotations) {
            // Only boxes shown whole decide their group's label length: a
            // frame cut by the plot's edge leaves a sliver of a box, which
            // forced every data box down to "DB". A clipped box still shows
            // the longest label its own visible part can hold (below).
            const double fx0 = sampleIndexToX(plot, a.start);
            const double fx1 = sampleIndexToX(plot, a.end);
            if (fx0 < areaLeft || fx1 > plot.right())
                continue;
            const double x0 = fx0;
            const double x1 = fx1;
            if (x1 <= x0 || a.texts.isEmpty())
                continue;
            int fit = -1;
            for (int i = 0; i < a.texts.size(); ++i) {
                if (fm.horizontalAdvance(a.texts[i]) <= (x1 - x0) - 6.0) {
                    fit = i;
                    break;
                }
            }
            if (fit >= 0)
                groupLevel[a.group] = std::max(groupLevel.value(a.group, 0), fit);
        }

        if (!row.message.isEmpty()) {
            p.setPen(Theme::textDisabled());
            p.drawText(QRectF(areaLeft, band.top(), band.right() - areaLeft, rowH),
                       Qt::AlignCenter, row.message);
        } else {
            // Ordinary fields first, errors over them.
            for (int pass = 0; pass < 2; ++pass) {
                for (const BusAnnotation &a : row.annotations) {
                    if (a.error != (pass == 1))
                        continue;
                    double x0 = sampleIndexToX(plot, a.start);
                    double x1 = sampleIndexToX(plot, a.end);
                    if (x1 < areaLeft || x0 > plot.right())
                        continue;
                    x0 = std::max(x0, areaLeft);
                    x1 = std::min(x1, plot.right());
                    const QColor c = a.error ? errorColour : row.colour;
                    if (x1 - x0 < 3.0) {
                        // Too narrow for a box at this zoom: a tick keeps it
                        // visible that something is there.
                        p.setPen(QPen(c, 1.0));
                        p.drawLine(QPointF(x0, band.top() + 3.0), QPointF(x0, band.bottom() - 3.0));
                        continue;
                    }
                    const QRectF box(x0, band.top() + 2.0, x1 - x0, rowH - 4.0);
                    QColor fill = c;
                    fill.setAlpha(60);
                    p.setPen(QPen(c, 1.0));
                    p.setBrush(fill);
                    p.drawRoundedRect(box, 3.0, 3.0);
                    for (int i = groupLevel.value(a.group, 0); i < a.texts.size(); ++i) {
                        if (fm.horizontalAdvance(a.texts[i]) <= box.width() - 6.0) {
                            p.setPen(Theme::text());
                            p.drawText(box, Qt::AlignCenter, a.texts[i]);
                            break;
                        }
                    }
                }
            }
        }

        p.setPen(Qt::NoPen);
        p.setBrush(row.colour);
        p.drawRoundedRect(tag, 3.0, 3.0);
        p.setPen(QColor(16, 24, 10));
        p.drawText(tag, Qt::AlignCenter, row.label);
    }
    p.restore();
}

namespace {
QColor engineFlagColour() { return Theme::accent(); }
QColor compressionBadColour() { return QColor(255, 70, 70); }
}

double ScopeView::timeToSampleIndex(double seconds) const
{
    if (m_sampleRate <= 0.0)
        return 0.0;
    if (m_hasAbsoluteTimeOrigin)
        return (seconds - m_absoluteTimeOrigin) * m_sampleRate;
    return viewAnchor(std::max(m_lastLength, 1)) + seconds * m_sampleRate;
}

void ScopeView::setEngineOverlay(bool enabled, bool manualMarkers)
{
    if (enabled && !m_engine.markersPlaced)
        resetEngineMarkers();
    m_engine.enabled = enabled;
    m_engine.manual = manualMarkers;
    update();
}

void ScopeView::setEngineCycles(const QVector<double> &cycleStarts, double cycleDegrees,
                                double offsetDegrees)
{
    m_engine.cycleStarts = cycleStarts;
    m_engine.cycleDeg = cycleDegrees > 0.0 ? cycleDegrees : 720.0;
    m_engine.offsetDeg = offsetDegrees;
    update();
}

void ScopeView::setCylinderBands(const QVector<CylinderBand> &bands)
{
    m_engine.bands = bands;
    update();
}

void ScopeView::setCompressionResult(const QVector<double> &percent, double flagBelowPercent,
                                     const QString &note)
{
    m_engine.compression = percent;
    m_engine.flagBelow = flagBelowPercent;
    m_engine.note = note;
    update();
}

void ScopeView::setTestPanel(const QString &title, const QStringList &lines,
                             const QVector<bool> &warn)
{
    setInfoPanel(0, title, lines, warn);
}

void ScopeView::setInfoPanel(int slot, const QString &title, const QStringList &lines,
                             const QVector<bool> &warn)
{
    if (title.isEmpty())
        m_infoPanels.remove(slot);
    else
        m_infoPanels[slot] = { title, lines, warn };
    update();
}

void ScopeView::drawTestPanel(QPainter &p, const QRectF &plot) const
{
    if (m_infoPanels.isEmpty())
        return;
    p.save();
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);
    const QFontMetricsF fm(small);
    const double rowH = fm.height() + 4.0;
    const double colGap = 12.0;
    const double bottomLimit = plot.bottom() - bottomReservedHeight() - 6.0;
    double y = m_engine.enabled && m_enginePanelsBottom > 0.0
        ? m_enginePanelsBottom : plot.top() + 10.0 + topReservedHeight();

    for (const InfoPanel &panelInfo : m_infoPanels) {
        // Column widths across this panel's tabbed lines.
        QVector<double> cols;
        double plainWidth = fm.horizontalAdvance(panelInfo.title);
        for (const QString &line : panelInfo.lines) {
            if (!line.contains(QLatin1Char('\t'))) {
                plainWidth = std::max(plainWidth, fm.horizontalAdvance(line));
                continue;
            }
            const QStringList cells = line.split(QLatin1Char('\t'));
            if (cols.size() < cells.size())
                cols.resize(cells.size());
            for (int c = 0; c < cells.size(); ++c)
                cols[c] = std::max(cols[c], fm.horizontalAdvance(cells[c]));
        }
        double tableWidth = 0.0;
        for (double c : cols)
            tableWidth += c + colGap;
        double width = std::max(plainWidth, tableWidth - colGap) + 16.0;
        // Never wider than ~60 % of the plot: the last column gives way (its
        // text is cut with an ellipsis). It was unlimited, so one long row
        // pushed the right-anchored panel off the plot's left edge.
        const double maxWidth = std::max(160.0, plot.width() * 0.62);
        if (width > maxWidth) {
            if (!cols.isEmpty()) {
                cols.last() = std::max(30.0, cols.last() - (width - maxWidth));
                double tw = 0.0;
                for (double c : cols)
                    tw += c + colGap;
                width = std::max(plainWidth, tw - colGap) + 16.0;
            }
            width = std::min(width, maxWidth);
        }

        // As many lines as fit above the bottom; the rest summarised.
        int shown = int(panelInfo.lines.size());
        const int fit = int((bottomLimit - y - 8.0) / rowH) - 1;   // minus the title
        if (fit < 1)
            break;
        const bool cut = shown > fit;
        if (cut)
            shown = fit - 1;
        const int rows = shown + (cut ? 1 : 0);
        const double height = rowH * (rows + 1) + 8.0;
        const double x = m_readoutCorner == 1 ? plot.left() + 8.0 : plot.right() - width - 8.0;
        const QRectF panel(x, y, width, height);
        QColor bg = Theme::plotBackground();
        bg.setAlpha(225);
        p.setPen(QPen(Theme::plotGridMajor(), 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(panel, 6.0, 6.0);
        p.setPen(Theme::plotForegroundText());
        p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0, width - 16.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(panelInfo.title, Qt::ElideRight, int(width - 16.0)));
        for (int i = 0; i < shown; ++i) {
            const bool warn = i < panelInfo.warn.size() && panelInfo.warn[i];
            p.setPen(warn ? compressionBadColour() : Theme::plotAxisText());
            const double ly = panel.top() + 4.0 + rowH * (i + 1);
            const QString &line = panelInfo.lines[i];
            if (line.contains(QLatin1Char('\t'))) {
                const QStringList cells = line.split(QLatin1Char('\t'));
                double cx = panel.left() + 8.0;
                for (int c = 0; c < cells.size(); ++c) {
                    p.drawText(QRectF(cx, ly, cols[c] + 2.0, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                               fm.elidedText(cells[c], Qt::ElideRight, int(cols[c] + 2.0)));
                    cx += cols[c] + colGap;
                }
            } else {
                p.drawText(QRectF(panel.left() + 8.0, ly, width - 16.0, rowH),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           fm.elidedText(line, Qt::ElideRight, int(width - 16.0)));
            }
        }
        if (cut) {
            p.setPen(Theme::plotAxisText());
            p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0 + rowH * (shown + 1), width - 16.0, rowH),
                       Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("\u2026 %1 more").arg(panelInfo.lines.size() - shown));
        }
        y = panel.bottom() + 8.0;
    }
    p.restore();
}

void ScopeView::setCrankInfo(const QStringList &lines, const QVector<bool> &warn,
                             const QVector<double> &faults)
{
    m_engine.crankLines = lines;
    m_engine.crankWarn = warn;
    m_engine.crankFaults = faults;
    update();
}

void ScopeView::setEngineTimingChart(bool chart, const QVector<int> &firingOrder)
{
    m_engine.timingChart = chart;
    m_engine.order = firingOrder;
    update();
}

namespace {
// Stroke colours for the timing chart, in stroke order after TDC.
QColor strokeColour(int stroke)
{
    switch (stroke) {
    case 0:  return QColor(0xef, 0x53, 0x50);   // power
    case 1:  return QColor(0xa1, 0x88, 0x7f);   // exhaust
    case 2:  return QColor(0x4f, 0xc3, 0xf7);   // intake
    default: return QColor(0xff, 0xd5, 0x4f);   // compression
    }
}
const char *strokeName(int stroke)
{
    static const char *names[] = { "Power", "Exhaust", "Intake", "Compression" };
    return names[stroke & 3];
}
}

// One row per cylinder, in firing order (so each row starts one firing
// interval after the one above — the staircase). Cylinder c's compression
// TDC sits at its firing position x (720 / n); from there: power, exhaust,
// intake, compression, 180 degrees each. A 2-stroke has two phases (power,
// compression) of 180 degrees. Semi-transparent, under the traces.
void ScopeView::drawTimingChart(QPainter &p, const QRectF &plot) const
{
    const QVector<double> &s = m_engine.cycleStarts;
    const int n = m_engine.order.size();
    if (n == 0 || s.size() < 2)
        return;
    const double cyc = m_engine.cycleDeg;
    const bool twoStroke = cyc < 540.0;
    const int strokes = twoStroke ? 2 : 4;
    const double strokeDeg = cyc / strokes;
    const double top = plot.top() + topReservedHeight();
    const double bottom = plot.bottom() - bottomReservedHeight();
    if (bottom - top < n * 4.0)
        return;
    const double rowH = (bottom - top) / n;
    const int alpha = Theme::isDark() ? 70 : 95;

    p.save();
    p.setClipRect(plot);
    for (int k = 0; k + 1 < s.size(); ++k) {
        const double len = s[k + 1] - s[k];
        if (len <= 0.0)
            continue;
        const double x0 = sampleIndexToX(plot, s[k]);
        const double x1 = sampleIndexToX(plot, s[k + 1]);
        if (x1 < plot.left() || x0 > plot.right())
            continue;
        const auto xAt = [&](double rel) { return x0 + (x1 - x0) * rel / cyc; };
        for (int r = 0; r < n; ++r) {
            const double tdc = r * cyc / n;
            const double y = top + r * rowH;
            for (int st = 0; st < strokes; ++st) {
                // A 2-stroke's second phase is its compression stroke.
                QColor c = strokeColour(twoStroke && st == 1 ? 3 : st);
                c.setAlpha(alpha);
                double rel0 = std::fmod(tdc + st * strokeDeg - m_engine.offsetDeg, cyc);
                if (rel0 < 0.0)
                    rel0 += cyc;
                const double rel1 = rel0 + strokeDeg;
                // A stroke running past the cycle's end wraps to its start.
                const double a = xAt(rel0), b = xAt(std::min(rel1, cyc));
                p.fillRect(QRectF(QPointF(a, y + 1.0), QPointF(b, y + rowH - 1.0)), c);
                if (rel1 > cyc)
                    p.fillRect(QRectF(QPointF(x0, y + 1.0), QPointF(xAt(rel1 - cyc), y + rowH - 1.0)), c);
            }
        }
    }
    p.restore();
}

double ScopeView::engineMarkerA() const { return timeToSampleIndex(m_engine.aTime); }
double ScopeView::engineMarkerB() const { return timeToSampleIndex(m_engine.bTime); }

void ScopeView::resetEngineMarkers()
{
    // Not before there is data: with no sample rate both markers would land
    // on time 0, on top of each other. setEngineOverlay() retries until
    // they're placed.
    if (m_sampleRate <= 0.0 || m_lastLength < 2)
        return;
    const QRectF plot = plotRect();
    m_engine.aTime = sampleIndexToTime(sampleIndexAtX(plot, plot.left() + plot.width() * 0.2));
    m_engine.bTime = sampleIndexToTime(sampleIndexAtX(plot, plot.left() + plot.width() * 0.8));
    m_engine.markersPlaced = true;
    update();
}

bool ScopeView::engineAngleAt(double sampleIndex, double *degrees, double *unwrapped,
                              int *cylinder) const
{
    const QVector<double> &s = m_engine.cycleStarts;
    if (s.size() < 2)
        return false;
    const auto it = std::upper_bound(s.cbegin(), s.cend(), sampleIndex);
    const int k = int(it - s.cbegin()) - 1;
    if (k < 0 || k + 1 >= s.size() || s[k + 1] <= s[k])
        return false;
    const double frac = (sampleIndex - s[k]) / (s[k + 1] - s[k]);
    const double cyc = m_engine.cycleDeg;
    *unwrapped = (double(k) + frac) * cyc + m_engine.offsetDeg;
    double deg = std::fmod(frac * cyc + m_engine.offsetDeg, cyc);
    if (deg < 0.0)
        deg += cyc;
    *degrees = deg;
    *cylinder = 0;
    for (const CylinderBand &b : m_engine.bands) {
        if (sampleIndex >= b.start && sampleIndex < b.end) {
            *cylinder = b.cylinder;
            break;
        }
    }
    return true;
}

QRectF ScopeView::engineMarkerFlag(const QRectF &plot, int which) const
{
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    const QFontMetricsF fm(small);
    const QString text = which == 1 ? QStringLiteral("0\u00b0")
                                    : QStringLiteral("%1\u00b0").arg(m_engine.cycleDeg, 0, 'f', 0);
    const double w = fm.horizontalAdvance(text) + 12.0;
    const double x = sampleIndexToX(plot, which == 1 ? engineMarkerA() : engineMarkerB());
    const double left = std::clamp(x - w / 2.0, plot.left(), plot.right() - w);
    return QRectF(left, plot.top() + 2.0, w, fm.height() + 4.0);
}

// Alternating light shading, one band per cylinder event — under the traces
// so it never hides them. Bands the compression test left out get a red tint.
void ScopeView::drawEngineBands(QPainter &p, const QRectF &plot) const
{
    if (!m_engine.enabled)
        return;
    if (m_engine.timingChart) {
        drawTimingChart(p, plot);
        return;
    }
    if (m_engine.bands.isEmpty())
        return;
    p.save();
    p.setClipRect(plot);
    QColor even = Theme::plotForegroundText();
    even.setAlpha(Theme::isDark() ? 16 : 22);
    QColor bad = compressionBadColour();
    bad.setAlpha(28);
    for (int i = 0; i < m_engine.bands.size(); ++i) {
        const CylinderBand &b = m_engine.bands[i];
        const double x0 = sampleIndexToX(plot, b.start);
        const double x1 = sampleIndexToX(plot, b.end);
        if (x1 < plot.left() || x0 > plot.right())
            continue;
        if (b.excluded)
            p.fillRect(QRectF(QPointF(x0, plot.top()), QPointF(x1, plot.bottom())), bad);
        else if (i % 2 == 0)
            p.fillRect(QRectF(QPointF(x0, plot.top()), QPointF(x1, plot.bottom())), even);
    }
    p.restore();
}

// TDC lines and cylinder numbers, degree ticks, the manual markers, and the
// compression bar chart — over the traces.
void ScopeView::drawEngineOverlay(QPainter &p, const QRectF &plot) const
{
    m_enginePanelsBottom = -1.0;
    if (!m_engine.enabled)
        return;
    p.save();
    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);
    const QFontMetricsF fm(small);
    const double rowH = fm.height() + 4.0;

    // Degree ticks every 90 degrees along the top edge, labelled where
    // there's room.
    const QVector<double> &s = m_engine.cycleStarts;
    QColor tick = Theme::plotAxisText();
    for (int k = 0; k + 1 < s.size(); ++k) {
        const double len = s[k + 1] - s[k];
        if (len <= 0.0)
            continue;
        const double pxPerDeg = (sampleIndexToX(plot, s[k + 1]) - sampleIndexToX(plot, s[k]))
                                / m_engine.cycleDeg;
        for (double d = 0.0; d < m_engine.cycleDeg - 1e-6; d += 90.0) {
            // Angle d sits (d - offset) into the cycle.
            double rel = std::fmod(d - m_engine.offsetDeg, m_engine.cycleDeg);
            if (rel < 0.0)
                rel += m_engine.cycleDeg;
            const double x = sampleIndexToX(plot, s[k] + rel / m_engine.cycleDeg * len);
            if (x < plot.left() || x > plot.right())
                continue;
            p.setPen(QPen(tick, 1.0));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.top() + 5.0));
            if (pxPerDeg * 90.0 >= fm.horizontalAdvance(QStringLiteral("540\u00b0")) + 8.0) {
                p.drawText(QRectF(x + 2.0, plot.top() + 2.0, 60.0, rowH),
                           Qt::AlignLeft | Qt::AlignVCenter,
                           QStringLiteral("%1\u00b0").arg(d, 0, 'f', 0));
            }
        }
    }

    // TDC lines and cylinder numbers (second row, under the degree labels).
    QColor tdc = Theme::plotForegroundText();
    tdc.setAlpha(110);
    for (const CylinderBand &b : m_engine.bands) {
        const double x = sampleIndexToX(plot, b.tdc);
        if (x < plot.left() || x > plot.right())
            continue;
        p.setPen(QPen(tdc, 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, plot.top() + rowH + 2.0), QPointF(x, plot.bottom()));
        const QString label = QString::number(b.cylinder);
        const double w = fm.horizontalAdvance(label) + 10.0;
        const QRectF pill(x - w / 2.0, plot.top() + rowH + 4.0, w, rowH);
        p.setPen(Qt::NoPen);
        p.setBrush(b.excluded ? compressionBadColour() : Theme::plotForegroundText());
        p.drawRoundedRect(pill, 4.0, 4.0);
        p.setPen(Theme::plotBackground());
        p.drawText(pill, Qt::AlignCenter, label);
    }

    // Timing chart: cylinder number at the left of each row, and a legend.
    if (m_engine.timingChart && !m_engine.order.isEmpty() && s.size() >= 2) {
        const int n = m_engine.order.size();
        const double top = plot.top() + topReservedHeight();
        const double bottom = plot.bottom() - bottomReservedHeight();
        const double rowHeight = (bottom - top) / n;
        if (rowHeight >= 4.0) {
            for (int r = 0; r < n; ++r) {
                const QString label = QString::number(m_engine.order[r]);
                const double w = fm.horizontalAdvance(label) + 10.0;
                const double h = std::min(rowH, rowHeight - 2.0);
                const QRectF pill(plot.left() + 3.0, top + r * rowHeight + (rowHeight - h) / 2.0, w, h);
                p.setPen(Qt::NoPen);
                p.setBrush(Theme::plotForegroundText());
                p.drawRoundedRect(pill, 4.0, 4.0);
                p.setPen(Theme::plotBackground());
                p.drawText(pill, Qt::AlignCenter, label);
            }
            const bool twoStroke = m_engine.cycleDeg < 540.0;
            const QList<int> shown = twoStroke ? QList<int>{ 0, 3 } : QList<int>{ 2, 3, 0, 1 };
            double legendW = 0.0;
            for (int st : shown)
                legendW += 14.0 + fm.horizontalAdvance(QString::fromLatin1(strokeName(st))) + 12.0;
            double x = plot.center().x() - legendW / 2.0;
            const double y = bottom + 2.0;
            for (int st : shown) {
                QColor c = strokeColour(st);
                c.setAlpha(200);
                p.setPen(Qt::NoPen);
                p.setBrush(c);
                p.drawRoundedRect(QRectF(x, y + (rowH - 10.0) / 2.0, 10.0, 10.0), 2.0, 2.0);
                p.setPen(Theme::plotAxisText());
                const QString name = QString::fromLatin1(strokeName(st));
                p.drawText(QRectF(x + 14.0, y, fm.horizontalAdvance(name) + 4.0, rowH),
                           Qt::AlignLeft | Qt::AlignVCenter, name);
                x += 14.0 + fm.horizontalAdvance(name) + 12.0;
            }
        }
    }

    // The manual cycle markers: solid lines, flags at the top to drag them.
    if (m_engine.manual) {
        for (int which : { 1, 2 }) {
            const double x = sampleIndexToX(plot, which == 1 ? engineMarkerA() : engineMarkerB());
            if (x >= plot.left() && x <= plot.right()) {
                p.setPen(QPen(engineFlagColour(), 1.5));
                p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            }
            const QRectF flag = engineMarkerFlag(plot, which);
            p.setPen(Qt::NoPen);
            p.setBrush(engineFlagColour());
            p.drawRoundedRect(flag, 4.0, 4.0);
            p.setPen(Theme::onAccent());
            p.drawText(flag, Qt::AlignCenter, which == 1
                ? QStringLiteral("0\u00b0")
                : QStringLiteral("%1\u00b0").arg(m_engine.cycleDeg, 0, 'f', 0));
        }
    }

    // Tooth faults: a red line and "!" at each, along the bottom.
    const double faultBase = plot.bottom() - bottomReservedHeight();
    for (double f : m_engine.crankFaults) {
        const double x = sampleIndexToX(plot, f);
        if (x < plot.left() || x > plot.right())
            continue;
        p.setPen(QPen(compressionBadColour(), 1.5));
        p.drawLine(QPointF(x, plot.top() + topReservedHeight()), QPointF(x, faultBase));
        const QRectF pill(x - 7.0, faultBase - rowH - 2.0, 14.0, rowH);
        p.setPen(Qt::NoPen);
        p.setBrush(compressionBadColour());
        p.drawRoundedRect(pill, 4.0, 4.0);
        p.setPen(Qt::white);
        p.drawText(pill, Qt::AlignCenter, QStringLiteral("!"));
    }

    // Info panels, top right (top left if the readouts are there): the
    // crank/cam results, then the compression chart under them.
    double panelTop = plot.top() + topReservedHeight() + 10.0;
    if (!m_engine.crankLines.isEmpty()) {
        double width = fm.horizontalAdvance(QStringLiteral("Crank and cam"));
        for (const QString &line : m_engine.crankLines)
            width = std::max(width, fm.horizontalAdvance(line));
        width += 16.0;
        const double height = rowH * (m_engine.crankLines.size() + 1) + 8.0;
        const double x = m_readoutCorner == 1 ? plot.left() + 8.0 : plot.right() - width - 8.0;
        const QRectF panel(x, panelTop, width, height);
        QColor bg = Theme::plotBackground();
        bg.setAlpha(225);
        p.setPen(QPen(Theme::plotGridMajor(), 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(panel, 6.0, 6.0);
        p.setPen(Theme::plotForegroundText());
        p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0, width - 16.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Crank and cam"));
        for (int i = 0; i < m_engine.crankLines.size(); ++i) {
            const bool warn = i < m_engine.crankWarn.size() && m_engine.crankWarn[i];
            p.setPen(warn ? compressionBadColour() : Theme::plotAxisText());
            p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0 + rowH * (i + 1), width - 16.0, rowH),
                       Qt::AlignLeft | Qt::AlignVCenter, m_engine.crankLines[i]);
        }
        panelTop = panel.bottom() + 8.0;
        m_enginePanelsBottom = panelTop;
    }

    // Compression bar chart.
    if (!m_engine.compression.isEmpty() || !m_engine.note.isEmpty()) {
        const int n = m_engine.compression.size();
        const double barW = 26.0, gap = 8.0, chartH = 90.0;
        const double titleW = std::max(fm.horizontalAdvance(QStringLiteral("Relative compression")),
                                       fm.horizontalAdvance(m_engine.note));
        const double width = std::max(titleW + 16.0, n * (barW + gap) + gap);
        const double height = rowH * 2.0 + (n > 0 ? chartH + rowH * 2.0 : 0.0) + 8.0;
        // Top right, unless the readouts were dropped there.
        const double panelX = m_readoutCorner == 1 ? plot.left() + 8.0
                                                   : plot.right() - width - 8.0;
        const QRectF panel(panelX, panelTop, width, height);
        m_enginePanelsBottom = panel.bottom() + 8.0;
        QColor bg = Theme::plotBackground();
        bg.setAlpha(225);
        p.setPen(QPen(Theme::plotGridMajor(), 1.0));
        p.setBrush(bg);
        p.drawRoundedRect(panel, 6.0, 6.0);
        p.setPen(Theme::plotForegroundText());
        p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0, panel.width() - 16.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Relative compression"));
        p.setPen(Theme::plotAxisText());
        p.drawText(QRectF(panel.left() + 8.0, panel.top() + 4.0 + rowH, panel.width() - 16.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, m_engine.note);
        const double baseY = panel.top() + 4.0 + rowH * 3.0 + chartH;
        for (int i = 0; i < n; ++i) {
            const double pct = std::clamp(m_engine.compression[i], 0.0, 100.0);
            const bool weak = pct < 100.0 - m_engine.flagBelow;
            const double x = panel.left() + gap + i * (barW + gap);
            const double h = chartH * pct / 100.0;
            p.setPen(Qt::NoPen);
            p.setBrush(weak ? compressionBadColour() : Theme::accent());
            p.drawRoundedRect(QRectF(x, baseY - h, barW, h), 3.0, 3.0);
            p.setPen(weak ? compressionBadColour() : Theme::plotForegroundText());
            p.drawText(QRectF(x - 6.0, baseY - h - rowH, barW + 12.0, rowH),
                       Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("%1").arg(pct, 0, 'f', 0));
            p.setPen(Theme::plotAxisText());
            p.drawText(QRectF(x, baseY + 2.0, barW, rowH), Qt::AlignCenter, QString::number(i + 1));
        }
    }
    p.restore();
}

void ScopeView::setPersistenceEnabled(bool enabled)
{
    if (m_persistEnabled == enabled)
        return;
    m_persistEnabled = enabled;
    clearPersistence();
}

void ScopeView::clearPersistence()
{
    for (PersistGrid &g : m_persist)
        g = PersistGrid();
    m_persistDirty = true;
    update();
}

// What the grids' own RESOLUTION depends on — a change here makes old bins
// the wrong size or the wrong quantity, so accumulation starts over. Not in
// here, on purpose: plot size (side panels), each channel's position
// (vertical drag) and the pan offset. Hits are stored in data coordinates,
// so those only change how the grids are drawn, not what they hold.
QVector<double> ScopeView::persistenceSignature() const
{
    QVector<double> sig;
    sig.reserve(3 + MaxChannels * 8);
    sig << m_secondsPerDiv << m_sampleRate << (m_triggerArmed ? 1.0 : 0.0);
    for (int ch = 0; ch < MaxChannels; ++ch) {
        const Channel &c = m_channels[ch];
        sig << (c.visible ? 1.0 : 0.0) << c.voltsPerDiv << c.scale
            << c.offset << c.windowCentre
            << c.probeGain << c.probeOffset << (c.inverted ? 1.0 : 0.0);
    }
    return sig;
}

bool ScopeView::persistenceUsable(const QRectF &) const
{
    if (!m_persistEnabled || m_persistSignature != persistenceSignature())
        return false;
    for (const PersistGrid &g : m_persist) {
        if (g.cols > 0 && g.max > 0)
            return true;
    }
    return false;
}

void ScopeView::accumulatePersistence(int channel)
{
    const QRectF plot = plotRect();
    const int w = int(plot.width());
    const int h = int(plot.height());
    if (w < 2 || h < 2)
        return;

    const QVector<double> sig = persistenceSignature();
    if (sig != m_persistSignature) {
        m_persistSignature = sig;
        for (PersistGrid &g : m_persist)
            g = PersistGrid();
    }

    const Channel &c = m_channels[channel];
    const int n = c.samples.size();
    if (!c.visible || n < 2)
        return;

    const int total  = std::max(m_lastLength, 1);
    const double anchor = viewAnchor(total);
    const double perDivision = c.voltsPerDiv / c.scale;
    const double ppd = pixelsPerDiv(plot);
    if (perDivision <= 0.0 || ppd <= 0.0)
        return;

    PersistGrid &g = m_persist[channel];
    if (g.cols == 0) {
        // Bin sizes from the view as it is right now — about one bin per
        // pixel — covering the whole buffer horizontally (so panning shows
        // accumulated data outside the current window too) and three screen
        // heights vertically, centred on what is mid-screen now (so a trace
        // dragged up or down by up to a screen keeps its history).
        const int start  = windowStart(total);
        const int length = std::max(2, std::min(windowLength(), total - start));
        g.bx   = std::max(1e-6, double(length) / w);
        g.by   = perDivision / ppd;
        g.rows = 3 * h;
        g.xMin = -anchor;
        g.yMin = -c.positionVolts - g.rows / 2.0 * g.by;
        double cols = std::ceil(double(n) / g.bx);
        // A hard ceiling on memory (16 MB per channel): deep zoom on a long
        // capture would otherwise ask for an enormous grid; past it the bins
        // simply get wider than a pixel.
        constexpr double kMaxCells = 4.0 * 1024 * 1024;
        if (cols * g.rows > kMaxCells) {
            cols = std::floor(kMaxCells / g.rows);
            g.bx = double(n) / cols;
        }
        g.cols = std::max(1, int(cols));
        g.hits.fill(0, g.cols * g.rows);
        g.max = 0;
    }

    // Volts as drawn, minus the channel's own position — what the grid's y
    // axis holds, so vertical dragging never touches stored hits.
    const auto volts = [&c](double raw) {
        const double probed = raw * c.probeGain + c.probeOffset;
        return probed * (c.inverted ? -1.0 : 1.0) + c.offset - c.windowCentre;
    };

    // One vertical run per column over that column's samples plus the next
    // one, so neighbouring columns join up — dense or sparse, like before.
    // Each bin counts at most once per channel per frame.
    for (int gx = 0; gx < g.cols; ++gx) {
        const double r0 = g.xMin + gx * g.bx + anchor;   // absolute sample
        const double r1 = r0 + g.bx;
        if (r1 < 0.0 || r0 > n - 1)
            continue;                                     // not in this capture
        const int a = std::clamp(int(std::floor(r0)), 0, n - 1);
        const int b = std::clamp(int(std::ceil(r1)), a, n - 1);
        double mn = c.samples[a], mx = c.samples[a];
        for (int i = a + 1; i <= b; ++i) {
            mn = std::min(mn, c.samples[i]);
            mx = std::max(mx, c.samples[i]);
        }
        double v0 = volts(mn), v1 = volts(mx);
        if (v0 > v1)
            std::swap(v0, v1);                            // inverted channel
        int gy0 = int(std::floor((v0 - g.yMin) / g.by));
        int gy1 = int(std::floor((v1 - g.yMin) / g.by));
        if (gy1 < 0 || gy0 >= g.rows)
            continue;                                     // off the grid entirely
        gy0 = std::max(gy0, 0);
        gy1 = std::min(gy1, g.rows - 1);
        for (int gy = gy0; gy <= gy1; ++gy) {
            quint32 &hit = g.hits[gy * g.cols + gx];
            ++hit;
            g.max = std::max(g.max, hit);
        }
    }
    m_persistDirty = true;
}

namespace {
// Least to most frequent. CH1 (and CH3) use the usual digital-colour
// ordering, blue, cyan, green, yellow, red; CH2 (and CH4) use violet,
// magenta, pink, white, which shares no colours with the first, so the two
// channels stay distinguishable even where both are dense. Built once each.
struct PaletteStop { double t; int r, g, b; };

std::array<QRgb, 256> buildPalette(const PaletteStop (&stops)[5])
{
        std::array<QRgb, 256> out{};
        for (int i = 0; i < 256; ++i) {
            const double t = i / 255.0;
            int k = 0;
            while (k < 3 && t > stops[k + 1].t)
                ++k;
            const double f = (t - stops[k].t) / (stops[k + 1].t - stops[k].t);
            const auto mix = [f](int x, int y) { return int(std::lround(x + (y - x) * f)); };
            out[i] = qRgb(mix(stops[k].r, stops[k + 1].r),
                          mix(stops[k].g, stops[k + 1].g),
                          mix(stops[k].b, stops[k + 1].b));
        }
        return out;
}

const std::array<QRgb, 256> &persistencePalette(int channel)
{
    static const PaletteStop kRainbow[5] = {
        { 0.00,  40,  80, 255 },
        { 0.25,   0, 200, 255 },
        { 0.50,  40, 230,  80 },
        { 0.75, 255, 230,   0 },
        { 1.00, 255,  40,  30 },
    };
    static const PaletteStop kViolet[5] = {
        { 0.00, 110,  40, 200 },
        { 0.25, 170,  50, 230 },
        { 0.50, 230,  60, 210 },
        { 0.75, 255, 140, 210 },
        { 1.00, 255, 245, 250 },
    };
    static const std::array<QRgb, 256> rainbow = buildPalette(kRainbow);
    static const std::array<QRgb, 256> violet = buildPalette(kViolet);
    return (channel % 2 == 0) ? rainbow : violet;
}
}

void ScopeView::drawPersistence(QPainter &p, const QRectF &plot) const
{
    const int w = int(plot.width());
    const int h = int(plot.height());
    if (w < 2 || h < 2)
        return;

    const int total  = std::max(m_lastLength, 1);
    const int start  = windowStart(total);
    const int length = std::max(2, std::min(windowLength(), total - start));
    const double anchor = viewAnchor(total);

    // Everything that decides where a grid lands on screen right now.
    QVector<double> key;
    key << w << h << start << length << anchor << plot.center().y()
        << pixelsPerDiv(plot);
    for (int ch = 0; ch < MaxChannels; ++ch)
        key << m_channels[ch].positionVolts;

    if (m_persistDirty || key != m_persistViewKey || m_persistImage.size() != QSize(w, h)) {
        m_persistViewKey = key;
        m_persistImage = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
        m_persistImage.fill(Qt::transparent);
        const double ppd = pixelsPerDiv(plot);

        QVector<int> colLo(w), colHi(w), rowLo(h), rowHi(h);
        // Channels in order, so a higher channel draws over a lower one.
        for (int ch = 0; ch < MaxChannels; ++ch) {
            const PersistGrid &g = m_persist[ch];
            const Channel &c = m_channels[ch];
            if (g.cols == 0 || g.max == 0 || !c.visible)
                continue;
            const double perDivision = c.voltsPerDiv / c.scale;
            if (perDivision <= 0.0 || ppd <= 0.0)
                continue;

            // Which bins each screen column / row covers. Taking the busiest
            // bin across that span (rather than one sample point) keeps thin
            // edges solid when a narrower plot squeezes several bins into a
            // pixel.
            for (int col = 0; col < w; ++col) {
                const double rel0 = start + double(length) * col / w - anchor;
                const double rel1 = start + double(length) * (col + 1) / w - anchor;
                int lo = int(std::floor((rel0 - g.xMin) / g.bx));
                int hi = int(std::ceil((rel1 - g.xMin) / g.bx)) - 1;
                hi = std::max(hi, lo);
                if (hi < 0 || lo >= g.cols) { colLo[col] = -1; continue; }
                colLo[col] = std::max(lo, 0);
                colHi[col] = std::min(hi, g.cols - 1);
            }
            // Screen row r spans y = top + r .. top + r + 1; the inverse of
            // valueToY() without position gives the stored volts there.
            const auto voltsAtY = [&](double y) {
                return (plot.center().y() - y) / ppd * perDivision - c.positionVolts;
            };
            for (int r = 0; r < h; ++r) {
                const double vTop = voltsAtY(plot.top() + r);
                const double vBot = voltsAtY(plot.top() + r + 1);
                int lo = int(std::floor((vBot - g.yMin) / g.by));
                int hi = int(std::ceil((vTop - g.yMin) / g.by)) - 1;
                hi = std::max(hi, lo);
                if (hi < 0 || lo >= g.rows) { rowLo[r] = -1; continue; }
                rowLo[r] = std::max(lo, 0);
                rowHi[r] = std::min(hi, g.rows - 1);
            }

            // Log scale: steady edges would otherwise drown out the rare
            // events persistence exists to show.
            const double norm = std::log1p(double(g.max));
            const auto &lut = persistencePalette(ch);
            for (int r = 0; r < h; ++r) {
                if (rowLo[r] < 0)
                    continue;
                QRgb *line = reinterpret_cast<QRgb *>(m_persistImage.scanLine(r));
                for (int col = 0; col < w; ++col) {
                    if (colLo[col] < 0)
                        continue;
                    quint32 m = 0;
                    for (int gy = rowLo[r]; gy <= rowHi[r]; ++gy) {
                        const quint32 *row = g.hits.constData() + gy * g.cols;
                        for (int gx = colLo[col]; gx <= colHi[col]; ++gx)
                            m = std::max(m, row[gx]);
                    }
                    if (m == 0)
                        continue;
                    const double tt = norm > 0.0 ? std::log1p(double(m)) / norm : 1.0;
                    line[col] = lut[std::clamp(int(tt * 255.0), 0, 255)];
                }
            }
        }
        m_persistDirty = false;
    }
    p.drawImage(QPointF(plot.left(), plot.top()), m_persistImage);
}

void ScopeView::setReferencesVisible(bool visible)
{
    if (m_referencesVisible == visible)
        return;
    m_referencesVisible = visible;
    update();
}

// Recomputed from the RAW voltage samples (before probe scaling, offset, or
// display zoom — the same values valueToY() starts from for a real channel),
// so "CH1 - CH2" subtracts what was actually measured rather than some
// already-scaled display quantity. Only does the work when the channel that
// just changed is actually one of the two math sources.
void ScopeView::recomputeMath(int changedChannel)
{
    if (!m_math.enabled)
        return;
    const bool twoSources = MathOps::usesB(int(m_math.op));
    if (changedChannel != m_math.sourceA && (!twoSources || changedChannel != m_math.sourceB))
        return;

    // What each source measures — probe applied, inverted if the channel
    // is, in the probe's unit (combining raw pre-probe values was wrong as
    // soon as a probe wasn't x1). See mathops.h.
    const auto measured = [](const Channel &c) {
        std::vector<double> v(size_t(c.samples.size()));
        const double sign = c.inverted ? -1.0 : 1.0;
        for (int i = 0; i < c.samples.size(); ++i)
            v[size_t(i)] = (c.samples[i] * c.probeGain + c.probeOffset) * sign;
        return v;
    };
    const std::vector<double> a = measured(m_channels[m_math.sourceA]);
    const std::vector<double> b = twoSources ? measured(m_channels[m_math.sourceB]) : std::vector<double>();
    const std::vector<double> r = MathOps::compute(int(m_math.op), a, b, m_sampleRate,
                                                   m_math.scaleK, m_math.scaleC);
    m_math.samples = QVector<double>(r.begin(), r.end());
    // Math's own filter, on its result (its sources are already filtered by
    // their own settings).
    if (m_math.filter != FilterOff && m_lowPassInView)
        m_math.samples = applyFilter(m_math.samples, m_sampleRate, m_math.filter,
                                     m_math.filterLowHz, m_math.filterHighHz);
    ++m_dataGeneration;   // Math's measurements (cache) and decoding follow it
}

void ScopeView::setChannelOverRange(int channel, bool over)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    if (m_channels[channel].overRange == over)
        return;
    m_channels[channel].overRange = over;
    update();
}

void ScopeView::clearChannel(int channel)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].samples.clear();
    m_channels[channel].rawSamples.clear();
    m_channels[channel].measureSamples.clear();
    m_channels[channel].measureRate = 0.0;
    m_channels[channel].visible = false;
    ++m_dataGeneration;
    update();
}

void ScopeView::clearAll()
{
    ++m_dataGeneration;
    for (Channel &c : m_channels) {
        c.samples.clear();
        c.rawSamples.clear();
    }
    m_lastLength = 0;
    update();
}

void ScopeView::setSampleRate(double rate)
{
    if (rate <= 0.0 || qFuzzyCompare(rate, m_sampleRate))
        return;
    m_sampleRate = rate;
    update();
}

void ScopeView::setDisplayRateOverride(double rate)
{
    m_displayRateOverride = rate;
    update();
}

void ScopeView::setSecondsPerDiv(double seconds)
{
    if (seconds > 0.0) {
        m_secondsPerDiv = seconds;
        update();
    }
}

void ScopeView::setTrigger(int sourceChannel, double levelVolts, int slope, bool armed)
{
    m_triggerSource = sourceChannel;
    m_triggerLevel  = levelVolts;
    m_triggerSlope  = slope;
    m_triggerArmed  = armed;
    update();
}

void ScopeView::setRunning(bool running)
{
    m_running = running;
    update();
}

void ScopeView::setZoomMode(bool enabled)
{
    m_zoomMode = enabled;
    m_selecting = false;
    setCursor(enabled ? Qt::CrossCursor : Qt::ArrowCursor);
    update();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

bool ScopeView::channelSpan(int channel, double *lo, double *hi) const
{
    if (channel < 0 || channel >= MaxChannels)
        return false;
    const Channel &c = m_channels[channel];
    if (c.samples.isEmpty())
        return false;

    const auto [mn, mx] = std::minmax_element(c.samples.begin(), c.samples.end());
    if (lo) *lo = *mn * c.probeGain + c.probeOffset;
    if (hi) *hi = *mx * c.probeGain + c.probeOffset;
    return true;
}

bool ScopeView::channelMidpoint(int channel, double *volts) const
{
    double lo = 0.0, hi = 0.0;
    if (!channelSpan(channel, &lo, &hi))
        return false;
    if (volts)
        *volts = (lo + hi) / 2.0;
    return true;
}

double ScopeView::channelFullScaleVolts(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0.0;
    const Channel &c = m_channels[channel];
    // Identical to drawReadouts()'s own "page" — deliberately the same
    // formula, not just a similar one, so a channel's XY axis always
    // matches the +-X V its own card shows.
    return c.voltsPerDiv / c.scale * (VerticalDivs / 2.0);
}

// The navigator hands us a position in the capture; we keep it as a distance
// from the trigger instead. The trigger does not land on the same buffer index
// every frame, so a fixed buffer position drifts against the signal by exactly
// that jitter — which is the wobble you see when panning away from centre.
void ScopeView::setViewCentreFraction(double fraction)
{
    const int n = m_lastLength;
    if (n <= 0)
        return;

    const double requested = std::clamp(fraction, 0.0, 1.0) * n;
    m_viewOffsetSamples = requested - viewAnchor(n);
    m_viewFollowing = false;
    update();
}

// Panning accumulates directly into m_viewOffsetSamples (a double) rather than
// going through setViewCentreFraction()/viewportCentreFraction(), which round-
// trips through windowStart()'s integer sample index on every call. Reading
// that rounded position back and writing a new fraction derived from it, once
// per mouse-move event during a drag, silently drops the sub-sample remainder
// every single time — and because C++'s int() truncates toward zero rather
// than to the nearest integer, that loss is not symmetric between positive and
// negative deltas, which is what made one drag direction consistently feel
// faster than the other at a tight zoom (where each event's raw delta is only
// a few samples, so the rounding loss is a large fraction of it).
void ScopeView::panBySamples(double deltaSamples)
{
    if (m_lastLength <= 0)
        return;

    if (m_viewFollowing) {
        m_viewOffsetSamples = 0.0;   // start the pan from where it visually is
        m_viewFollowing = false;
    }
    m_viewOffsetSamples += deltaSamples;

    // Clamped to what windowStart() would use anyway, so dragging past the
    // edge does not require dragging back through the same distance before
    // the view starts moving again.
    const int length = std::min(windowLength(), m_lastLength);
    const double anchor = viewAnchor(m_lastLength);
    const double minOffset = length / 2.0 - anchor;
    const double maxOffset = double(m_lastLength) - length / 2.0 - anchor;
    m_viewOffsetSamples = std::clamp(m_viewOffsetSamples, minOffset, maxOffset);

    update();
}

void ScopeView::followTrigger()
{
    m_viewOffsetSamples = 0.0;
    m_viewFollowing = true;
    update();
}

// Whatever the view is measured from: the trigger instant when one is armed and
// reported, the middle of the buffer otherwise.
double ScopeView::viewAnchor(int sampleCount) const
{
    if (m_triggerArmed && m_triggerPoint >= 0.0 && m_triggerPoint <= sampleCount)
        return m_triggerPoint;
    return sampleCount / 2.0;
}

double ScopeView::viewportWidthFraction() const
{
    if (m_lastLength <= 0)
        return 1.0;
    return std::clamp(double(windowLength()) / double(m_lastLength), 0.0005, 1.0);
}

double ScopeView::viewportCentreFraction() const
{
    if (m_lastLength <= 0)
        return 0.5;
    const int length = std::min(windowLength(), m_lastLength);
    const int start  = windowStart(m_lastLength);
    return std::clamp((start + length / 2.0) / double(m_lastLength), 0.0, 1.0);
}

double ScopeView::triggerFraction() const
{
    if (m_lastLength <= 0 || !m_triggerArmed
        || m_triggerPoint < 0.0 || m_triggerPoint > m_lastLength)
        return -1.0;
    return m_triggerPoint / double(m_lastLength);
}

// See the struct's own comment in scopeview.h for why this is recomputed
// fresh from current state rather than tracked historically.
ScopeView::GutterAllocation ScopeView::computeGutterAllocation() const
{
    GutterAllocation alloc;
    const bool leftHasChannel  = m_channels[0].visible;
    const bool rightHasChannel = m_channelCount > 1 && m_channels[1].visible;
    // How many MORE columns fit on each side, beyond that side's own
    // channel if it has one — each side holds at most 2 total.
    const int leftCap  = leftHasChannel  ? 1 : 2;
    const int rightCap = rightHasChannel ? 1 : 2;
    int leftUsed = 0, rightUsed = 0;

    // Left first, then right once the left side's own cap is reached —
    // exactly the "left first, then overflow right" placement asked for.
    // Math is offered the first slot ahead of either reference: the common
    // case is Math already on before a reference is added, so this matches
    // that order without needing to track actual add-order historically.
    auto place = [&](bool active, bool &onLeftOut, int &columnOut) {
        if (!active) { columnOut = -1; return; }
        if (leftUsed < leftCap) {
            onLeftOut = true;
            columnOut = leftUsed++;
        } else {
            onLeftOut = false;
            columnOut = rightUsed++;
        }
    };

    place(m_math.enabled, alloc.mathOnLeft, alloc.mathColumn);
    for (int slot = 0; slot < MaxReferences; ++slot) {
        place(m_referencesVisible && m_references[slot].occupied,
              alloc.referenceOnLeft[slot], alloc.referenceColumn[slot]);
    }

    alloc.leftExtraCount  = leftUsed;
    alloc.rightExtraCount = rightUsed;
    return alloc;
}

// A hidden channel's scale gives its gutter back to the plot.
double ScopeView::leftGutter() const
{
    const GutterAllocation alloc = computeGutterAllocation();
    if (!m_channels[0].visible && alloc.leftExtraCount == 0)
        return kNarrowLeft;
    double width = m_channels[0].visible ? kScaleGutter : 0.0;
    for (int i = 0; i < alloc.leftExtraCount; ++i)
        width += extraColumnWidth(true, i);
    return width;
}

double ScopeView::rightGutter() const
{
    const GutterAllocation alloc = computeGutterAllocation();
    const bool ch2Visible = m_channelCount > 1 && m_channels[1].visible;
    if (!ch2Visible && alloc.rightExtraCount == 0)
        return kNarrowRight;
    double width = ch2Visible ? kScaleGutter : 0.0;
    for (int i = 0; i < alloc.rightExtraCount; ++i)
        width += extraColumnWidth(false, i);
    return width;
}


QRectF ScopeView::plotRect() const
{
    const double w = double(width())  - leftGutter() - rightGutter();
    const double h = double(height()) - kTopGutter - kAxisHeight - kStripHeight - 4;
    return QRectF(leftGutter(), kTopGutter, std::max(10.0, w), std::max(10.0, h));
}

double ScopeView::pixelsPerDiv(const QRectF &plot) const
{
    return plot.height() / VerticalDivs;
}

double ScopeView::valueToY(const QRectF &plot, const Channel &c, double volts) const
{
    const double probed = volts * c.probeGain + c.probeOffset;
    const double signal = probed * (c.inverted ? -1.0 : 1.0) + c.offset;
    const double perDivision = c.voltsPerDiv / c.scale;
    const double divisions = (signal - c.windowCentre + c.positionVolts) / perDivision;
    return plot.center().y() - divisions * pixelsPerDiv(plot);
}

// How many samples one screen width covers at the current timebase.
int ScopeView::windowLength() const
{
    if (m_sampleRate <= 0.0)
        return m_lastLength;
    const double samples = m_secondsPerDiv * HorizontalDivs * m_sampleRate;
    return std::clamp(int(samples), 2, std::max(2, m_lastLength));
}

// Where that window starts, centred on the trigger instant. This is what stops
// a triggered trace from walking: the buffer may begin anywhere, but the view
// is anchored to the edge that caused the capture.
int ScopeView::windowStart(int sampleCount) const
{
    const int length = std::min(windowLength(), sampleCount);

    // Always measured from the anchor, so panning moves the window without
    // unlatching it from the trigger. Free running the anchor is the buffer
    // centre: whatever ReadVoltageDatasTriggerPoint() returns then is not a
    // trigger instant, and following it makes the drift look erratic.
    const double centre = viewAnchor(sampleCount)
                        + (m_viewFollowing ? 0.0 : m_viewOffsetSamples);

    int start = int(centre) - length / 2;
    return std::clamp(start, 0, std::max(0, sampleCount - length));
}

// ---------------------------------------------------------------------------
// Measurements
// ---------------------------------------------------------------------------

void ScopeView::setMeasurementOverride(int channel, const Measurements &m)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_measureOverride[size_t(channel)] = m;
    m_hasMeasureOverride[size_t(channel)] = true;
    update();
}

void ScopeView::clearMeasurementOverrides()
{
    if (std::none_of(m_hasMeasureOverride.begin(), m_hasMeasureOverride.end(), [](bool b) { return b; }))
        return;
    m_hasMeasureOverride.fill(false);
    update();
}

double ScopeView::measureGain(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 1.0;
    const Channel &c = m_channels[channel];
    return c.probeGain * (c.inverted ? -1.0 : 1.0);
}

double ScopeView::measureBias(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return 0.0;
    const Channel &c = m_channels[channel];
    return c.probeOffset * (c.inverted ? -1.0 : 1.0) + c.offset;
}

ScopeView::Measurements ScopeView::measure(const Channel &c) const
{
    // Measured elsewhere (Live's worker, full-rate data): that result.
    const ptrdiff_t index = &c - m_channels.data();
    if (index >= 0 && index < MaxChannels && m_hasMeasureOverride[size_t(index)])
        return m_measureOverride[size_t(index)];
    // Measurements report what is displayed: probe scaling and DC offset
    // included, display zoom excluded (it magnifies the drawing, not the
    // signal).
    const double sign = c.inverted ? -1.0 : 1.0;
    const double gain = c.probeGain * sign;
    const double bias = c.probeOffset * sign + c.offset;
    if (!c.measureSamples.isEmpty() && c.measureRate > 0.0)
        return measureCore(c.measureSamples, gain, bias, c.measureRate);
    return measureCore(c.samples, gain, bias, m_sampleRate);
}

// Verified numerically before use (1 MS/s, 10 kHz cutoff): -3.01 dB at the
// cutoff, -17.7 dB an octave above, -48.6 dB at 5x; a step's 50% point stays
// on its original sample (zero phase); DC unchanged at both ends.
namespace {
// Zero-phase 2nd-order Butterworth low-pass at design frequency fc: run
// forward then backward, so the response is 1 / (1 + (f/fc)^4).
QVector<double> zeroPhaseLowPass(const QVector<double> &in, double sampleRate, double fc);
}

QVector<double> ScopeView::lowPassFilter(const QVector<double> &in, double sampleRate,
                                         double cutoffHz)
{
    // Forward-backward squares the response, so design above the requested
    // point: -3 dB where (f/fc)^4 = sqrt(2) - 1, i.e. fc = f / 0.802.
    if (cutoffHz <= 0.0)
        return in;
    return zeroPhaseLowPass(in, sampleRate, cutoffHz / std::pow(std::sqrt(2.0) - 1.0, 0.25));
}

QVector<double> ScopeView::highPassBaseline(const QVector<double> &in, double sampleRate,
                                            double cutoffHz)
{
    // -3 dB for the high-pass 1 - 1/(1+(f/fc)^4) is where (f/fc)^4 =
    // sqrt(2) + 1, i.e. fc = f / 1.2465.
    if (cutoffHz <= 0.0)
        return QVector<double>(in.size(), 0.0);
    const double fc = cutoffHz / std::pow(std::sqrt(2.0) + 1.0, 0.25);
    if (fc >= 0.45 * sampleRate)
        return in;   // cutoff beyond what's sampled: nothing passes
    return zeroPhaseLowPass(in, sampleRate, fc);
}

QVector<double> ScopeView::highPassFilter(const QVector<double> &in, double sampleRate,
                                          double cutoffHz)
{
    const QVector<double> base = highPassBaseline(in, sampleRate, cutoffHz);
    QVector<double> out(in.size());
    for (int i = 0; i < in.size(); ++i)
        out[i] = in[i] - base[i];
    return out;
}

QVector<double> ScopeView::bandStopFilter(const QVector<double> &in, double sampleRate,
                                          double lowHz, double highHz)
{
    const int n = in.size();
    if (lowHz > highHz)
        std::swap(lowHz, highHz);
    if (n < 3 || sampleRate <= 0.0 || lowHz <= 0.0 || lowHz >= 0.45 * sampleRate)
        return in;
    highHz = std::min(highHz, 0.45 * sampleRate);
    const double w1 = std::tan(M_PI * lowHz / sampleRate);
    const double w2 = std::tan(M_PI * highHz / sampleRate);
    const double w0 = std::sqrt(w1 * w2);
    const double r = std::sqrt(w2 / w1);
    if (r - 1.0 / r < 1e-9)
        return in;
    // Forward + backward gives |H|^2: -3 dB at an edge needs |Q u| =
    // sqrt(sqrt(2) + 1), where u = r - 1/r there.
    const double q = std::sqrt(std::sqrt(2.0) + 1.0) / (r - 1.0 / r);
    const double a0 = 1.0 + w0 / q + w0 * w0;
    const double b0 = (1.0 + w0 * w0) / a0;
    const double b1 = (2.0 * w0 * w0 - 2.0) / a0;
    const double b2 = b0;
    const double a1 = b1;
    const double a2 = (1.0 - w0 / q + w0 * w0) / a0;
    QVector<double> out(in);
    const auto pass = [&](int from, int to, int dir) {
        double x1 = out[from], x2 = out[from], y1 = out[from], y2 = out[from];
        for (int i = from; i != to; i += dir) {
            const double x = out[i];
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            out[i] = y;
        }
    };
    pass(0, n, +1);
    pass(n - 1, -1, -1);
    return out;
}

QVector<double> ScopeView::removedPart(const QVector<double> &in, double sampleRate, int type,
                                       double lowHz, double highHz)
{
    if (type == FilterHighPass)
        return highPassBaseline(in, sampleRate, lowHz);
    const QVector<double> kept = applyFilter(in, sampleRate, type, lowHz, highHz);
    QVector<double> out(in.size());
    for (int i = 0; i < in.size(); ++i)
        out[i] = in[i] - kept[i];
    return out;
}

QVector<double> ScopeView::applyFilter(const QVector<double> &in, double sampleRate, int type,
                                       double lowHz, double highHz)
{
    if (lowHz > highHz && (type == FilterBandPass || type == FilterBandStop))
        std::swap(lowHz, highHz);
    switch (type) {
    case FilterLowPass:  return lowPassFilter(in, sampleRate, highHz);
    case FilterHighPass: return highPassFilter(in, sampleRate, lowHz);
    case FilterBandPass: return lowPassFilter(highPassFilter(in, sampleRate, lowHz), sampleRate, highHz);
    case FilterBandStop: return bandStopFilter(in, sampleRate, lowHz, highHz);
    case FilterHighRes: {
        // Centred mean of N samples via running sums (N from lowHz); at the
        // ends, the mean of what there is.
        const int n = in.size();
        const int len = std::clamp(int(std::lround(lowHz)), 1, std::max(1, n));
        if (len <= 1 || n < 2)
            return in;
        QVector<double> prefix(n + 1, 0.0);
        for (int i = 0; i < n; ++i)
            prefix[i + 1] = prefix[i] + in[i];
        QVector<double> out(n);
        const int before = (len - 1) / 2, after = len - 1 - before;
        for (int i = 0; i < n; ++i) {
            const int a = std::max(0, i - before), e = std::min(n, i + after + 1);
            out[i] = (prefix[e] - prefix[a]) / double(e - a);
        }
        return out;
    }
    default:             return in;
    }
}

namespace {
QVector<double> zeroPhaseLowPass(const QVector<double> &in, double sampleRate, double fc)
{
    const int n = in.size();
    if (n < 3 || sampleRate <= 0.0 || fc <= 0.0 || fc >= 0.45 * sampleRate)
        return in;
    const double w0 = 2.0 * M_PI * fc / sampleRate;
    const double cs = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * std::sqrt(0.5));   // Q = 1/sqrt(2)
    const double a0 = 1.0 + alpha;
    const double b0 = (1.0 - cs) / 2.0 / a0;
    const double b1 = (1.0 - cs) / a0;
    const double b2 = b0;
    const double a1 = -2.0 * cs / a0;
    const double a2 = (1.0 - alpha) / a0;

    QVector<double> out(in);
    // Each pass starts settled on its first value, so the ends don't ring.
    const auto pass = [&](int from, int to, int dir) {
        double x1 = out[from], x2 = out[from], y1 = out[from], y2 = out[from];
        for (int i = from; i != to; i += dir) {
            const double x = out[i];
            const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
            x2 = x1; x1 = x;
            y2 = y1; y1 = y;
            out[i] = y;
        }
    };
    pass(0, n, +1);
    pass(n - 1, -1, -1);
    return out;
}
}   // namespace

void ScopeView::refilterChannel(int channel)
{
    Channel &c = m_channels[channel];
    c.samples = (c.filter != FilterOff && m_lowPassInView)
        ? applyFilter(c.rawSamples, m_sampleRate, c.filter, c.filterLowHz, c.filterHighHz)
        : c.rawSamples;
    if (c.removeDc && !c.samples.isEmpty()) {
        double mean = 0.0;
        for (double v : c.samples)
            mean += v;
        mean /= double(c.samples.size());
        for (double &v : c.samples)
            v -= mean;
    }
}

void ScopeView::setChannelRemoveDc(int channel, bool remove)
{
    if (channel < 0 || channel >= MaxChannels || m_channels[channel].removeDc == remove)
        return;
    m_channels[channel].removeDc = remove;
    refilterChannel(channel);
    recomputeMath(channel);
    ++m_dataGeneration;
    update();
}

void ScopeView::setChannelFilter(int channel, int type, double lowHz, double highHz)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    Channel &c = m_channels[channel];
    if (c.filter == type && qFuzzyCompare(c.filterLowHz, lowHz) && qFuzzyCompare(c.filterHighHz, highHz))
        return;
    c.filter = type;
    c.filterLowHz = lowHz;
    c.filterHighHz = highHz;
    refilterChannel(channel);
    recomputeMath(channel);
    ++m_dataGeneration;     // decoding re-runs on the filtered data
    update();
}

void ScopeView::setMathFilter(int type, double lowHz, double highHz)
{
    if (m_math.filter == type && qFuzzyCompare(m_math.filterLowHz, lowHz)
        && qFuzzyCompare(m_math.filterHighHz, highHz))
        return;
    m_math.filter = type;
    m_math.filterLowHz = lowHz;
    m_math.filterHighHz = highHz;
    recomputeMath(m_math.sourceA);
    ++m_dataGeneration;
    update();
}

void ScopeView::setMathScaleParams(double k, double c)
{
    if (qFuzzyCompare(m_math.scaleK, k) && qFuzzyCompare(m_math.scaleC + 1.0, c + 1.0))
        return;
    m_math.scaleK = k;
    m_math.scaleC = c;
    recomputeMath(m_math.sourceA);
    ++m_dataGeneration;
    update();
}

QString ScopeView::mathUnit() const
{
    const QString ua = m_channels[m_math.sourceA].unit;
    const QString ub = m_channels[m_math.sourceB].unit;
    return QString::fromStdString(MathOps::unit(int(m_math.op), ua.toStdString(), ub.toStdString()));
}

QString ScopeView::mathLabel() const
{
    return QString::fromStdString(MathOps::label(int(m_math.op),
        QStringLiteral("CH%1").arg(m_math.sourceA + 1).toStdString(),
        QStringLiteral("CH%1").arg(m_math.sourceB + 1).toStdString()));
}

void ScopeView::setLowPassInView(bool apply)
{
    if (m_lowPassInView == apply)
        return;
    m_lowPassInView = apply;
    for (int ch = 0; ch < MaxChannels; ++ch)
        refilterChannel(ch);
    recomputeMath(m_math.sourceA);
    ++m_dataGeneration;
    update();
}

void ScopeView::setMeasurementSource(int channel, const QVector<double> &raw, double sampleRate)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].measureSamples = raw;
    m_channels[channel].measureRate = sampleRate;
    ++m_dataGeneration;   // the measurement cache keys on it
    update();
}

bool ScopeView::anyMeasurementActive(int channel) const
{
    if (channel < 0 || channel >= MaxChannels)
        return false;
    for (bool on : m_channels[channel].activeMeasurements)
        if (on)
            return true;
    return false;
}

void ScopeView::clearMeasurementSources()
{
    for (Channel &c : m_channels) {
        c.measureSamples.clear();
        c.measureRate = 0.0;
    }
    ++m_dataGeneration;
    update();
}

ScopeView::Measurements ScopeView::measureMath() const
{
    return measureCore(m_math.samples, 1.0, 0.0, m_sampleRate);
}

ScopeView::Measurements ScopeView::measureCore(const QVector<double> &samples, double gain, double bias,
                                              double sampleRate) const
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    const qint64 now = clock.elapsed();
    MeasureCacheEntry &e = m_measureCache[&samples];
    const bool sameSetup = e.valid && e.gain == gain && e.bias == bias && e.rate == sampleRate;
    if (sameSetup && e.generation == m_dataGeneration && e.data == samples.constData()
        && e.size == samples.size())
        return e.result;                                   // nothing changed
    // At least 200 ms between measurements of streaming data, and at least
    // 4x what the last one cost — readouts never take more than a fifth of
    // the interface's time, however large the buffer. Timed from when the
    // last one *finished*: from its start (the first version), a 200 ms
    // measurement had always "expired" by the next paint and ran every time.
    const qint64 kIntervalMs = std::max<qint64>(200, 4 * e.costMs);
    if (sameSetup && !samples.isEmpty() && now - e.atMs < kIntervalMs) {
        // New data arriving faster than readouts are worth refreshing: the
        // last result for now, and a repaint once the interval is up so the
        // readouts always catch up.
        if (!m_measureRefreshPending) {
            m_measureRefreshPending = true;
            auto *self = const_cast<ScopeView *>(this);
            QTimer::singleShot(int(kIntervalMs - (now - e.atMs)) + 5, self, [self] {
                self->m_measureRefreshPending = false;
                self->update();
            });
        }
        return e.result;
    }
    {
        VSX_TRACE("view.measure");
        e.result = measureUncached(samples, gain, bias, sampleRate);
    }
    const qint64 finished = clock.elapsed();
    e.costMs = finished - now;
    if (e.costMs >= 50) {
        if (PerfTrace::enabled())
            std::fprintf(stderr, "[measure] %d samples took %lld ms\n", int(samples.size()),
                     static_cast<long long>(e.costMs));
        std::fflush(stderr);
    }
    e.valid = true;
    e.generation = m_dataGeneration;
    e.data = samples.constData();
    e.size = samples.size();
    e.gain = gain;
    e.bias = bias;
    e.rate = sampleRate;
    e.atMs = finished;
    return e.result;
}

ScopeView::Measurements ScopeView::measureUncached(const QVector<double> &samples, double gain, double bias,
                                               double sampleRate)
{
    Measurements m;
    const int n = samples.size();
    if (n < 2)
        return m;

    const auto [lo, hi] = std::minmax_element(samples.begin(), samples.end());
    double sum = 0.0, sumSq = 0.0;
    for (double v : samples) {
        sum   += v;
        sumSq += v * v;
    }

    const double meanRaw = sum / n;

    m.vpp  = std::fabs((*hi - *lo) * gain);
    m.mean = meanRaw * gain + bias;
    m.vrms = std::sqrt(std::max(0.0,
                 (sumSq / n) * gain * gain
                 + 2.0 * gain * bias * meanRaw
                 + bias * bias));
    // Not simply *hi/*lo scaled: when inverted, gain is negative, so the raw
    // maximum maps to the smaller displayed value, not the larger one.
    const double displayedLo = *lo * gain + bias;
    const double displayedHi = *hi * gain + bias;
    m.max = std::max(displayedLo, displayedHi);
    m.min = std::min(displayedLo, displayedHi);
    // The DC/mean component drops out of a variance entirely (it is a
    // constant shift, not spread), so this needs no bias term at all — only
    // gain's magnitude, since AC RMS is inherently non-negative regardless
    // of inversion.
    m.acRms = std::sqrt(std::max(0.0, sumSq / n - meanRaw * meanRaw)) * std::fabs(gain);
    m.valid = true;

    // Frequency, period, duty cycle, and pulse widths all come from the same
    // single pass over mid-level crossings (with hysteresis, ignoring
    // anything indistinguishable from noise) — one shared source of truth
    // rather than separate passes that could disagree with each other.
    if (sampleRate <= 0.0 || (*hi - *lo) < 5e-3)
        return m;

    const double mid  = (*hi + *lo) / 2.0;
    const double hyst = (*hi - *lo) * 0.1;
    bool above = samples[0] > mid;
    int  firstRising = -1, lastRising = -1, crossings = 0;

    // Every pulse is the span between one edge and the next of the opposite
    // direction — tracked as we go, not by storing every crossing, since a
    // long multi-cycle capture could otherwise mean a lot of them.
    int    lastEdge = -1;
    bool   lastEdgeWasRising = false;
    double sumHighTime = 0.0, sumLowTime = 0.0;
    int    highPulses = 0, lowPulses = 0;

    for (int i = 1; i < n; ++i) {
        const double v = samples[i];
        if (!above && v > mid + hyst) {
            above = true;
            if (firstRising < 0)
                firstRising = i;
            lastRising = i;
            ++crossings;
            if (lastEdge >= 0 && !lastEdgeWasRising) {
                sumLowTime += double(i - lastEdge);
                ++lowPulses;
            }
            lastEdge = i;
            lastEdgeWasRising = true;
        } else if (above && v < mid - hyst) {
            above = false;
            if (lastEdge >= 0 && lastEdgeWasRising) {
                sumHighTime += double(i - lastEdge);
                ++highPulses;
            }
            lastEdge = i;
            lastEdgeWasRising = false;
        }
    }

    if (crossings >= 2 && lastRising > firstRising)
        m.frequency = (crossings - 1) * sampleRate / double(lastRising - firstRising);
    if (m.frequency > 0.0)
        m.period = 1.0 / m.frequency;
    if (highPulses > 0)
        m.positiveWidth = (sumHighTime / highPulses) / sampleRate;
    if (lowPulses > 0)
        m.negativeWidth = (sumLowTime / lowPulses) / sampleRate;
    // Duty cycle is self-consistent with the two widths above (built from
    // the same paired transitions) rather than dividing by the separately-
    // derived period, which a less regular signal could disagree with.
    if (highPulses > 0 && lowPulses > 0) {
        const double avgHigh = sumHighTime / highPulses;
        const double avgLow  = sumLowTime  / lowPulses;
        if (avgHigh + avgLow > 0.0)
            m.dutyCycle = avgHigh / (avgHigh + avgLow) * 100.0;
    }

    return m;
}

QString ScopeView::formatMeasurements(const Measurements &m,
                                      const std::array<bool, MeasurementCount> &active,
                                      const char *unit) const
{
    if (!m.valid)
        return QString();

    QString extra;
    auto appendRaw = [&](const QString &text) {
        if (!extra.isEmpty())
            extra += QStringLiteral("   ");
        extra += text;
    };
    auto append = [&](const QString &label, double value, const char *u = nullptr, int decimals = 2) {
        appendRaw(label + QStringLiteral(" ") + formatEng(value, u ? u : unit, decimals));
    };

    if (active[size_t(Measurement::Vpp)])
        append(QStringLiteral("Vpp"), m.vpp);
    if (active[size_t(Measurement::Vrms)])
        append(QStringLiteral("Vrms"), m.vrms);
    if (active[size_t(Measurement::Mean)])
        append(QStringLiteral("Mean"), m.mean);
    if (active[size_t(Measurement::Max)])
        append(QStringLiteral("Max"), m.max);
    if (active[size_t(Measurement::Min)])
        append(QStringLiteral("Min"), m.min);
    if (active[size_t(Measurement::AcRms)])
        append(QStringLiteral("ACrms"), m.acRms);
    if (active[size_t(Measurement::Frequency)] && m.frequency > 0.0)
        append(QStringLiteral("f"), m.frequency, "Hz", 3);
    if (active[size_t(Measurement::Period)] && m.period > 0.0)
        append(QStringLiteral("T"), m.period, "s", 3);
    if (active[size_t(Measurement::DutyCycle)] && m.dutyCycle > 0.0)
        appendRaw(QStringLiteral("Duty %1%").arg(m.dutyCycle, 0, 'f', 1));
    if (active[size_t(Measurement::PositiveWidth)] && m.positiveWidth > 0.0)
        append(QStringLiteral("+W"), m.positiveWidth, "s", 3);
    if (active[size_t(Measurement::NegativeWidth)] && m.negativeWidth > 0.0)
        append(QStringLiteral("-W"), m.negativeWidth, "s", 3);

    return extra;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void ScopeView::paintEvent(QPaintEvent *)
{
    VSX_TRACE("view.paintEvent");
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    // Rounded to match the cards' own radius, so the plot reads as part of
    // the same design instead of a sharp-cornered rectangle sitting among
    // rounded ones. Clipping to this path — not just filling it — keeps
    // the grid, traces, and every later layer from poking out past the
    // rounded corners; antialiasing is switched on only for this fill and
    // switched back off right after, since the grid/trace drawing below
    // depends on the crisp, unantialiased lines that setting produces.
    QPainterPath background;
    background.addRoundedRect(rect(), Theme::cornerRadius(), Theme::cornerRadius());
    p.setClipPath(background);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillPath(background, Theme::plotBackground());
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF plot = plotRect();

    drawGraticule(p, plot);
    drawGroundMarkers(p, plot);
    drawTriggerMarkers(p, plot);
    drawEngineBands(p, plot);

    // Persistence replaces the channels' own traces with the accumulated
    // heat map — but only while that map matches the current mapping; right
    // after a settings change (and before the next capture rebuilds it) the
    // plain traces show instead of a blank plot.
    if (persistenceUsable(plot)) {
        drawPersistence(p, plot);
    } else {
        for (int ch = 0; ch < m_channelCount; ++ch) {
            if (m_channels[ch].visible && !m_channels[ch].samples.isEmpty()) {
                VSX_TRACE("view.drawTrace");
                drawTrace(p, plot, m_channels[ch]);
            }
        }
    }
    drawMathTrace(p, plot);
    for (int slot = 0; slot < MaxReferences; ++slot)
        drawReferenceTrace(p, plot, slot);
    drawBusRows(p, plot);
    drawEngineOverlay(p, plot);
    drawTestPanel(p, plot);

    if (m_selecting) {
        const QRectF box = QRectF(m_selectStart, m_selectEnd).normalized()
                               .intersected(plot);
        QColor fill = Theme::triggerColor();
        fill.setAlpha(50);
        p.fillRect(box, fill);
        p.setPen(QPen(Theme::triggerColor(), 1.0, Qt::DashLine));
        p.drawRect(box);
    }

    drawAxes(p, plot);
    drawRulers(p, plot);
    drawAnnotations(p, plot);
    {
        VSX_TRACE("view.drawReadouts");
        drawReadouts(p, plot);
    }
    drawStatusStrip(p, plot);
    drawZoomHistoryButtons(p, plot);
}

double ScopeView::timeAxisOffsetDivs() const
{
    // Live with nothing to place yet: the present at the right edge.
    if (m_nowAtRight && (m_sampleRate <= 0.0 || m_lastLength <= 0 || !m_hasAbsoluteTimeOrigin))
        return -HorizontalDivs / 2.0;
    if (m_sampleRate <= 0.0 || m_lastLength <= 0)
        return 0.0;
    if (m_hasAbsoluteTimeOrigin) {
        // A record has no trigger: time counts up from the start of the
        // record, at the left edge of the plot, rather than being centred
        // on anything. The left edge is sample windowStart(), not sample 0:
        // for a Live ring window or a Record slice the two are the same
        // (the buffer IS the visible window), but a loaded Live file holds
        // far more than one screen and pans within it, and leaving the
        // start out is what kept the grid still while the trace moved.
        const double start = windowStart(m_lastLength);
        return HorizontalDivs / 2.0
               + (m_absoluteTimeOrigin + start / m_sampleRate) / m_secondsPerDiv;
    }
    // Zero on the time axis is the trigger instant, which is only
    // mid-screen while the view is following it.
    const int length = std::min(windowLength(), m_lastLength);
    const int start  = windowStart(m_lastLength);
    if (length > 1 && m_triggerPoint >= 0.0) {
        const double centreSample = start + length / 2.0;
        return (centreSample - m_triggerPoint) / (m_sampleRate * m_secondsPerDiv);
    }
    return 0.0;
}

void ScopeView::drawGraticule(QPainter &p, const QRectF &plot) const
{
    // Vertical (time) grid lines sit at "nice" time values — whole
    // multiples of one division's worth of time from the same reference
    // point drawAxes() labels against — rather than at fixed screen
    // positions, so the whole time grid slides together with the waveform
    // during a pan: a ruler printed on the same paper as the trace, not a
    // fixed overlay the trace moves independently beneath. Horizontal
    // (voltage) lines are unaffected — voltage has nothing to pan — and
    // stay at the ordinary fixed division positions, same as before.
    const double offsetDivs = timeAxisOffsetDivs();
    // A division's worth of slack on either side of the visible range, so
    // a line just outside it at one extreme of its own fractional shift is
    // still drawn rather than popping in a frame late as the pan continues.
    const int firstK = int(std::floor(offsetDivs - HorizontalDivs / 2.0)) - 1;
    const int lastK  = int(std::ceil(offsetDivs + HorizontalDivs / 2.0)) + 1;

    p.setPen(QPen(Theme::plotGridMinor(), 1.0, Qt::DotLine));
    for (int k = firstK; k <= lastK; ++k) {
        if (k == 0)
            continue;   // drawn separately below, as the dashed time-zero reference
        const double i = k - offsetDivs + HorizontalDivs / 2.0;
        if (i <= 0.0 || i >= HorizontalDivs)
            continue;
        const double x = plot.left() + plot.width() * i / HorizontalDivs;
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    for (int i = 1; i < VerticalDivs; ++i) {
        const double y = plot.top() + plot.height() * i / VerticalDivs;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // The dashed vertical reference marks time zero (the trigger instant,
    // or a record's own start) wherever it actually falls, rather than
    // always at the screen's physical centre — it only stayed centred
    // while unpanned before too, but now actually follows a pan instead of
    // staying frozen in place while the rest of the grid (once this redesign)
    // moves around it. Drawn only when time zero is actually somewhere in
    // view; the horizontal reference (voltage zero) has no pan to follow
    // and keeps its own fixed centre unconditionally.
    p.setPen(QPen(Theme::plotGridAxis(), 1.0, Qt::DashLine));
    const double zeroI = -offsetDivs + HorizontalDivs / 2.0;
    const bool zeroInView = !m_nowAtRight && zeroI > 0.0 && zeroI < HorizontalDivs;
    double cx = plot.center().x();
    if (zeroInView) {
        cx = plot.left() + plot.width() * zeroI / HorizontalDivs;
        p.drawLine(QPointF(cx, plot.top()), QPointF(cx, plot.bottom()));
    }
    const double cy = plot.center().y();
    p.drawLine(QPointF(plot.left(), cy), QPointF(plot.right(), cy));

    // Fine 1/5-division ticks along both centre references — the
    // horizontal ones (time) spaced out from wherever the vertical
    // reference above actually landed, so they slide together with it;
    // the vertical ones (voltage) are unaffected, same fixed centre as
    // always. Both sets only drawn when the vertical reference itself is
    // actually in view, since otherwise there is nothing meaningful for
    // either set to be measured from.
    p.setPen(Theme::plotGridAxis());
    const double dx = plot.width()  / (HorizontalDivs * 5.0);
    const double dy = plot.height() / (VerticalDivs   * 5.0);
    if (zeroInView) {
        for (int i = -HorizontalDivs * 5; i <= HorizontalDivs * 5; ++i) {
            if (i == 0)
                continue;
            const double x = cx + dx * i;
            if (x <= plot.left() || x >= plot.right())
                continue;
            p.drawLine(QPointF(x, cy - 3), QPointF(x, cy + 3));
        }
        for (int i = 1; i < VerticalDivs * 5; ++i) {
            const double y = plot.top() + dy * i;
            p.drawLine(QPointF(cx - 3, y), QPointF(cx + 3, y));
        }
    }

    p.setPen(Theme::plotGridMajor());
    p.drawRect(plot);
}

// Min and max of s from a up to (not including) b: the plain fast path, or skipping NaN ("no data")
// when gaps are on. False if the column holds no data at all.
bool ScopeView::columnRange(const QVector<double> &s, int a, int b, double *lo, double *hi) const
{
    if (!m_traceGaps) {
        const auto [mn, mx] = std::minmax_element(s.begin() + a, s.begin() + b);
        *lo = *mn;
        *hi = *mx;
        return true;
    }
    bool found = false;
    for (int i = a; i < b; ++i) {
        const double v = s[i];
        if (!std::isfinite(v))
            continue;
        if (!found) {
            *lo = *hi = v;
            found = true;
        } else {
            *lo = std::min(*lo, v);
            *hi = std::max(*hi, v);
        }
    }
    return found;
}

void ScopeView::drawTrace(QPainter &p, const QRectF &plot, const Channel &c) const
{
    const QVector<double> &s = c.samples;
    const int total  = s.size();
    const int start  = windowStart(total);
    const int length = std::min(windowLength(), total - start);
    if (length < 2)
        return;

    const double top = plot.top() - 4.0;
    const double bot = plot.bottom() + 4.0;
    const auto toY = [&](double v) { return std::clamp(valueToY(plot, c, v), top, bot); };

    p.setClipRect(plot);

    const int columns = int(plot.width());

    if (columns <= 0 || length <= columns * 2) {
        // Few enough samples per pixel that we are drawing an actual polyline
        // rather than a solid min/max wall — this is where real antialiasing
        // is visible and worth its cost. Round joins avoid the small notches
        // a hand-rolled miter would show at sharp direction changes.
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(c.color, 1.3);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        QPainterPath path;
        bool penDown = false;
        for (int i = 0; i < length; ++i) {
            const double v = s[start + i];
            if (m_traceGaps && !std::isfinite(v)) {
                penDown = false;          // no data here: lift the pen
                continue;
            }
            const double x = plot.left() + plot.width() * i / double(length - 1);
            const double y = toY(v);
            if (!penDown)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
            penDown = true;
        }
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setClipping(false);
        return;
    }

    // More samples than pixels: one vertical min/max bar per column, which
    // keeps deep captures cheap to repaint and preserves narrow glitches that
    // plain subsampling would drop. Left unantialiased on purpose: a vertical
    // line landing on a pixel column is already crisp, and softening its
    // edges would only blur the very peaks this technique exists to keep.
    p.setPen(QPen(c.color, 1.3));
    QVector<QLineF> bars;
    bars.reserve(columns * 2);
    double previousMid = 0.0;
    bool havePrevious = false;

    for (int col = 0; col < columns; ++col) {
        const int a = start + int(qint64(length) * col / columns);
        const int b = start + int(qint64(length) * (col + 1) / columns);
        if (b <= a)
            continue;

        double lo = 0.0, hi = 0.0;
        if (!columnRange(s, a, b, &lo, &hi)) {
            havePrevious = false;          // no data in this column
            continue;
        }
        const double x    = plot.left() + col;
        const double yTop = toY(hi);
        const double yBot = toY(lo);
        const double mid  = (yTop + yBot) / 2.0;

        if (havePrevious)
            bars.append(QLineF(x - 1, previousMid, x, mid));
        bars.append(QLineF(x, yTop, x, yBot));
        previousMid  = mid;
        havePrevious = true;
    }

    p.drawLines(bars);
    p.setClipping(false);
}

// A simplified, self-contained sibling of drawTrace(): same windowing and the
// same min/max-per-column decimation for a deep buffer, but with an
// independent, auto-scaled vertical range rather than a channel's probe/page,
// since a sum or product has no natural volts/div of its own. Kept as its own
// function rather than folded into drawTrace() so the existing, working
// channel rendering is not touched by this at all.
// Shared by drawMathTrace() and drawMathScale(), so the numbers in the gutter
// always describe exactly the line actually drawn — computed independently by
// each caller rather than cached, since a linear scan of one window's worth of
// samples is cheap and this avoids any ordering dependency between them.
ScopeView::MathRange ScopeView::mathRange(const QRectF &plot) const
{
    if (m_math.voltsPerDiv > 0.0) {
        // Manual: a fixed volts/division and a user-draggable position, the
        // same shape as a real channel's vertical scale, just without a probe
        // or hardware window — math has neither. "scale" here is kept in the
        // same units (pixels per volt) as the auto branch below, so
        // drawMathTrace()'s toY needs no branch of its own.
        const double scale = pixelsPerDiv(plot) / m_math.voltsPerDiv;
        const double mid   = -m_math.positionVolts;
        const double half  = m_math.voltsPerDiv * (VerticalDivs / 2.0);
        return { mid, scale, mid - half, mid + half };
    }

    // Auto: fit whatever is in the visible window to most of the graticule.
    const QVector<double> &s = m_math.samples;
    const int total  = s.size();
    const int start  = windowStart(total);
    const int length = std::min(windowLength(), total - start);
    if (length < 2)
        return { 0.0, 0.0, 0.0, 0.0 };

    double lo = s[start], hi = s[start];
    for (int i = 1; i < length; ++i) {
        lo = std::min(lo, s[start + i]);
        hi = std::max(hi, s[start + i]);
    }
    const double span = std::max(hi - lo, 1e-12);
    return { (hi + lo) / 2.0, (plot.height() * 0.8) / span, lo, hi };
}

void ScopeView::drawMathTrace(QPainter &p, const QRectF &plot) const
{
    if (!m_math.enabled || m_math.samples.isEmpty())
        return;

    const QVector<double> &s = m_math.samples;
    const int total  = s.size();
    const int start  = windowStart(total);
    const int length = std::min(windowLength(), total - start);
    if (length < 2)
        return;

    const MathRange r = mathRange(plot);
    if (r.scale <= 0.0)
        return;
    const double mid   = r.mid;
    const double scale = r.scale;

    const double top = plot.top() - 4.0;
    const double bot = plot.bottom() + 4.0;
    const auto toY = [&](double v) {
        return std::clamp(plot.center().y() - (v - mid) * scale, top, bot);
    };

    p.setClipRect(plot);

    // Solid rather than dashed: a dash pattern cuts across the line at
    // arbitrary points, and each cut edge is its own small aliasing problem —
    // worst on a thin, saturated trace. Thinner and slightly translucent
    // reads as "a computed aid" without needing a dash to say so, matching
    // how the better scope UIs draw a math trace.
    QColor dimmed = m_math.color;
    dimmed.setAlphaF(0.85f);

    const int columns = int(plot.width());
    if (columns <= 0 || length <= columns * 2) {
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(dimmed, 1.0);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);

        QPainterPath path;
        bool penDown = false;
        for (int i = 0; i < length; ++i) {
            const double v = s[start + i];
            if (m_traceGaps && !std::isfinite(v)) {
                penDown = false;          // no data here: lift the pen
                continue;
            }
            const double x = plot.left() + plot.width() * i / double(length - 1);
            const double y = toY(v);
            if (!penDown)
                path.moveTo(x, y);
            else
                path.lineTo(x, y);
            penDown = true;
        }
        p.drawPath(path);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setClipping(false);
        return;
    }

    p.setPen(QPen(dimmed, 1.0));
    QVector<QLineF> bars;
    bars.reserve(columns * 2);
    double previousMid = 0.0;
    bool havePrevious = false;
    for (int col = 0; col < columns; ++col) {
        const int a = start + int(qint64(length) * col / columns);
        const int b = start + int(qint64(length) * (col + 1) / columns);
        if (b <= a)
            continue;
        double smin = 0.0, smax = 0.0;
        if (!columnRange(s, a, b, &smin, &smax)) {
            havePrevious = false;
            continue;
        }
        const double x    = plot.left() + col;
        const double yTop = toY(smax);
        const double yBot = toY(smin);
        const double barMid = (yTop + yBot) / 2.0;
        if (havePrevious)
            bars.append(QLineF(x - 1, previousMid, x, barMid));
        bars.append(QLineF(x, yTop, x, yBot));
        previousMid  = barMid;
        havePrevious = true;
    }
    p.drawLines(bars);
    p.setClipping(false);
}

// Resampled against the CURRENT timebase/zoom via sampleIndexAtX()/
// sampleIndexToTime() — the same conversion the live trace and rulers
// already use — rather than whatever view was active when the reference
// was captured, which is what keeps it tracking along as the live trace
// is zoomed or panned instead of drifting out of alignment. Linear
// interpolation between the two nearest of the reference's own samples;
// NaN when the mapped time falls entirely outside what was captured,
// which drawReferenceTrace() reads as "no line here" rather than
// clamping to an edge value that was never actually measured there.
double ScopeView::referenceValueAt(int slot, const QRectF &plot, double x) const
{
    if (!referenceOccupied(slot))
        return std::numeric_limits<double>::quiet_NaN();
    const ReferenceState &r = m_references[slot];
    if (r.sampleRate <= 0.0 || r.samples.isEmpty())
        return std::numeric_limits<double>::quiet_NaN();

    // Anchor-relative, deliberately not sampleIndexToTime(): in Live (and a
    // loaded Live file) that returns absolute "time before newest", while a
    // reference's triggerIndex is relative to viewAnchor() — mixing the two
    // put a reference added in Live about half a screen off to the right.
    if (m_sampleRate <= 0.0)
        return std::numeric_limits<double>::quiet_NaN();
    const double liveTime = (sampleIndexAtX(plot, x) - viewAnchor(std::max(m_lastLength, 1)))
                            / m_sampleRate;
    const double refIndex = r.triggerIndex + (liveTime - r.delaySeconds) * r.sampleRate;
    if (refIndex < 0.0 || refIndex > double(r.samples.size() - 1))
        return std::numeric_limits<double>::quiet_NaN();

    const int lo = int(std::floor(refIndex));
    const int hi = std::min(lo + 1, int(r.samples.size()) - 1);
    const double frac = refIndex - lo;
    return r.samples[lo] * (1.0 - frac) + r.samples[hi] * frac;
}

void ScopeView::drawReferenceTrace(QPainter &p, const QRectF &plot, int slot) const
{
    if (!m_referencesVisible || !referenceOccupied(slot))
        return;
    const ReferenceState &r = m_references[slot];
    if (r.voltsPerDiv <= 0.0)
        return;   // nothing meaningful to scale against

    const double scale = pixelsPerDiv(plot) / r.voltsPerDiv;
    const double mid = -r.positionVolts;
    const double top = plot.top() - 4.0;
    const double bot = plot.bottom() + 4.0;
    const auto toY = [&](double v) {
        return std::clamp(plot.center().y() - (v - mid) * scale, top, bot);
    };

    p.setClipRect(plot);
    p.setRenderHint(QPainter::Antialiasing, true);
    // Solid, like the live traces (dashed was the user's call to drop); a
    // reference is told apart by its own colour.
    QPen pen(r.color, 1.2, Qt::SolidLine);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);

    // One sample per screen column: unlike drawTrace()'s own min/max-bar
    // technique for a dense capture, a reference is resampled from
    // whatever rate it actually holds, so a point per pixel already
    // matches what the display can show regardless of that rate.
    const int columns = std::max(1, int(plot.width()));
    QPainterPath path;
    bool havePoint = false;
    for (int col = 0; col <= columns; ++col) {
        const double x = plot.left() + col;
        const double v = referenceValueAt(slot, plot, x);
        if (std::isnan(v)) {
            havePoint = false;   // a gap: nothing captured at this time
            continue;
        }
        const double y = toY(v);
        if (!havePoint) {
            path.moveTo(x, y);
            havePoint = true;
        } else {
            path.lineTo(x, y);
        }
    }
    p.drawPath(path);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setClipping(false);
}

// A second, narrower column immediately to the right of CH1's own numbers —
// "beside" them rather than replacing anything there. Ticks are read off the
// same mapping drawMathTrace() draws with, inverted, so the numbers always
// describe exactly the line on screen regardless of the current auto-scale.
// See the declaration's own comment for why this is shared between
// drawMathScale() and drawReferenceScale().
double ScopeView::extraColumnWidth(bool onLeft, int column) const
{
    const bool sideHasChannel = onLeft ? m_channels[0].visible
                                        : (m_channelCount > 1 && m_channels[1].visible);
    if (column == 0 && !sideHasChannel)
        return kScaleGutter;
    return kMathColumnWidth;
}

double ScopeView::extraColumnX(bool onLeft, int column, const QRectF &plot) const
{
    constexpr double kScalePad = 4.0;
    if (onLeft) {
        double x = m_channels[0].visible ? kScaleGutter : 0.0;
        for (int i = 0; i < column; ++i)
            x += extraColumnWidth(true, i);
        return x;
    }
    const bool ch2Visible = m_channelCount > 1 && m_channels[1].visible;
    double x = plot.right() + kScalePad + (ch2Visible ? kScaleGutter : 0.0);
    for (int i = 0; i < column; ++i)
        x += extraColumnWidth(false, i);
    return x;
}

void ScopeView::drawMathScale(QPainter &p, const QRectF &plot) const
{
    if (!m_math.enabled || m_math.samples.isEmpty())
        return;

    const MathRange r = mathRange(plot);
    if (r.scale <= 0.0)
        return;

    const GutterAllocation alloc = computeGutterAllocation();
    if (alloc.mathColumn < 0)
        return;   // shouldn't happen (m_math.enabled implies placed), but be safe

    const QFontMetrics fm(p.font());
    constexpr double kScalePad = 4.0;
    const double colX = extraColumnX(alloc.mathOnLeft, alloc.mathColumn, plot);
    const double colW = extraColumnWidth(alloc.mathOnLeft, alloc.mathColumn) - kScalePad;
    const auto align = alloc.mathOnLeft ? (Qt::AlignRight | Qt::AlignVCenter)
                                         : (Qt::AlignLeft | Qt::AlignVCenter);

    p.setPen(m_math.color);
    const ScaleUnit su = mathScaleUnit();          // V or mV, as the channels
    p.drawText(QRectF(colX, 1, colW, kTopGutter - 14), align, su.unit);

    const double step = (r.hi - r.lo) / VerticalDivs;
    for (int i = 0; i <= VerticalDivs; ++i) {
        const double y = plot.top() + plot.height() * i / VerticalDivs;
        // Inverse of drawMathTrace()'s toY: y = centerY - (v - mid) * scale.
        const double v = r.mid - (y - plot.center().y()) / r.scale;
        const QRectF box(colX, y - fm.height() / 2.0, colW, fm.height());
        p.drawText(box, align, axisNumber(v * su.factor, step * su.factor, r.mid * su.factor));
    }
}

void ScopeView::drawReferenceScale(QPainter &p, const QRectF &plot, int slot) const
{
    if (!m_referencesVisible || !referenceOccupied(slot))
        return;
    const ReferenceState &r = m_references[slot];
    if (r.voltsPerDiv <= 0.0)
        return;

    const GutterAllocation alloc = computeGutterAllocation();
    if (alloc.referenceColumn[slot] < 0)
        return;   // shouldn't happen (occupied implies placed), but be safe

    const double scale = pixelsPerDiv(plot) / r.voltsPerDiv;
    const double mid = -r.positionVolts;

    const QFontMetrics fm(p.font());
    constexpr double kScalePad = 4.0;
    const double colX = extraColumnX(alloc.referenceOnLeft[slot], alloc.referenceColumn[slot], plot);
    const double colW = extraColumnWidth(alloc.referenceOnLeft[slot], alloc.referenceColumn[slot]) - kScalePad;
    const auto align = alloc.referenceOnLeft[slot] ? (Qt::AlignRight | Qt::AlignVCenter)
                                                    : (Qt::AlignLeft | Qt::AlignVCenter);

    // Its source's unit, in milli- while every label stays under 1 (the
    // channels' rule) — it read "V" for everything.
    double widest = 0.0;
    for (int i = 0; i <= VerticalDivs; ++i) {
        const double y = plot.top() + plot.height() * i / VerticalDivs;
        widest = std::max(widest, std::fabs(mid - (y - plot.center().y()) / scale));
    }
    const bool milli = widest < 1.0;
    const double factor = milli ? 1000.0 : 1.0;
    const QString unit = (milli ? QStringLiteral("m") : QString()) + r.unit;
    p.setPen(r.color);
    p.drawText(QRectF(colX, 1, colW, kTopGutter - 14), align, unit);

    const double step = r.voltsPerDiv;
    for (int i = 0; i <= VerticalDivs; ++i) {
        const double y = plot.top() + plot.height() * i / VerticalDivs;
        // Inverse of drawReferenceTrace()'s toY: y = centerY - (v - mid) * scale.
        const double v = mid - (y - plot.center().y()) / scale;
        const QRectF box(colX, y - fm.height() / 2.0, colW, fm.height());
        p.drawText(box, align, axisNumber(v * factor, step * factor, mid * factor));
    }
}

void ScopeView::drawRulers(QPainter &p, const QRectF &plot) const
{
    if (!m_rulers.enabled)
        return;

    const RulerLayout layout = computeRulerLayout(plot);

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);

    // Flags are allowed to draw in the axis/gutter area outside the plot
    // itself, so the clip is the whole widget here rather than just the plot.
    p.setClipRect(rect());

    auto drawFlag = [&](const QRectF &box, const QColor &colour, const QColor &textColour,
                         const QString &text) {
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawRoundedRect(box, 4.0, 4.0);
        p.setPen(textColour);
        p.drawText(box, Qt::AlignCenter, text);
    };

    // The off-screen state's own style: nothing here can meaningfully show
    // a value, since the flag's own position no longer corresponds to
    // where the cursor actually is — an outline, not a filled value badge,
    // makes that visually obvious rather than showing a number that would
    // otherwise look just as authoritative as an on-screen one.
    auto drawPinnedFlag = [&](const QRectF &box, const QColor &borderColour) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(borderColour, 1.0));
        p.drawRoundedRect(box, 4.0, 4.0);
    };

    if (layout.timeOn) {
        p.setPen(QPen(Theme::plotForegroundText(), 1.0, Qt::DashLine));
        if (layout.t1x >= plot.left() - 1.0 && layout.t1x <= plot.right() + 1.0)
            p.drawLine(QPointF(layout.t1x, plot.top()), QPointF(layout.t1x, plot.bottom()));
        if (layout.t2x >= plot.left() - 1.0 && layout.t2x <= plot.right() + 1.0)
            p.drawLine(QPointF(layout.t2x, plot.top()), QPointF(layout.t2x, plot.bottom()));
        if (layout.t1OffScreen)
            drawPinnedFlag(layout.t1Flag, Theme::plotForegroundText());
        else
            drawFlag(layout.t1Flag, Theme::plotForegroundText(), Theme::onPlotForeground(),
                     layout.t1Text);
        if (layout.t2OffScreen)
            drawPinnedFlag(layout.t2Flag, Theme::plotForegroundText());
        else
            drawFlag(layout.t2Flag, Theme::plotForegroundText(), Theme::onPlotForeground(),
                     layout.t2Text);
    }

    // Every enabled channel gets its own pair, all visible at once — dashed
    // in that channel's own colour, same convention as the reference app's
    // own coloured cursors.
    for (int ch = 0; ch <= MaxChannels; ++ch) {
        const RulerChannelLayout &cl = layout.channels[ch];
        if (!cl.on)
            continue;
        p.setPen(QPen(cl.colour, 1.0, Qt::DashLine));
        if (cl.v1Valid && cl.v1y >= plot.top() - 1.0 && cl.v1y <= plot.bottom() + 1.0)
            p.drawLine(QPointF(plot.left(), cl.v1y), QPointF(plot.right(), cl.v1y));
        if (cl.v2Valid && cl.v2y >= plot.top() - 1.0 && cl.v2y <= plot.bottom() + 1.0)
            p.drawLine(QPointF(plot.left(), cl.v2y), QPointF(plot.right(), cl.v2y));
        if (cl.v1Valid)
            drawFlag(cl.v1Flag, cl.colour, Theme::onAccent(), cl.v1Text);
        if (cl.v2Valid)
            drawFlag(cl.v2Flag, cl.colour, Theme::onAccent(), cl.v2Text);
        if (rulersTracking()) {
            // Where each voltage ruler meets its time ruler, on the trace.
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(cl.colour, 1.5));
            p.setBrush(Theme::plotBackground());
            const auto marker = [&](bool valid, double x, double y) {
                if (valid && x >= plot.left() && x <= plot.right() && y >= plot.top() && y <= plot.bottom())
                    p.drawEllipse(QPointF(x, y), 4.0, 4.0);
            };
            marker(cl.v1Valid, layout.t1x, cl.v1y);
            marker(cl.v2Valid, layout.t2x, cl.v2y);
            p.setBrush(Qt::NoBrush);
        }
    }

    // The draggable readout box.
    const QRectF box = layout.box;
    p.setPen(Qt::NoPen);
    p.setBrush([]{ QColor c = Theme::elevatedBackground(); c.setAlpha(235); return c; }());
    p.drawRoundedRect(box, 6.0, 6.0);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(Theme::border(), 1.0));
    p.drawRoundedRect(box, 6.0, 6.0);

    // Title strip in the accent colour, clipped to the box's own rounded
    // corners so it does not square them off — corners are only at the top
    // when minimized (a shorter rect), so this stays correct either way.
    constexpr double kTitleH = 20.0;
    QPainterPath titleClip;
    titleClip.addRoundedRect(box, 6.0, 6.0);
    p.setClipPath(titleClip, Qt::IntersectClip);
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::accent());
    p.drawRect(QRectF(box.left(), box.top(), box.width(), kTitleH));
    p.setClipRect(rect());

    p.setPen(Theme::onAccent());
    p.drawText(QRectF(box.left() + 8.0, box.top(), box.width() - 46.0, kTitleH),
               Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Rulers"));
    p.drawText(layout.minimizeRect, Qt::AlignCenter,
               m_rulers.boxMinimized ? QStringLiteral("\u25a1") : QStringLiteral("\u2212"));
    p.drawText(layout.closeRect, Qt::AlignCenter, QStringLiteral("\u2715"));

    if (m_rulers.boxMinimized) {
        p.setClipRect(plot);
        return;
    }

    // A real table: a swatch column, then "1" / "2" / "delta", exactly like
    // the reference app's own readout — one row per active cursor, not a
    // fixed handful of lines, since any number of channels can be active.
    constexpr double kSwatchW = 18.0;
    const double colW = (box.width() - kSwatchW - 12.0) / 3.0;
    const double col0 = box.left() + 6.0 + kSwatchW;
    const double col1 = col0 + colW;
    const double col2 = col1 + colW;
    const double rowH = 16.0;

    double rowY = box.top() + kTitleH + 2.0;
    p.setPen(Theme::plotAxisText());
    p.drawText(QRectF(col0, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("1"));
    p.drawText(QRectF(col1, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("2"));
    p.drawText(QRectF(col2, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("\u0394"));
    rowY += rowH + 2.0;

    auto swatchCentre = [&](double y) { return QPointF(box.left() + 6.0 + kSwatchW / 2.0, y + rowH / 2.0); };
    auto dataRow = [&](const QString &v1, const QString &v2, const QString &d) {
        p.setPen(Theme::plotForegroundText());
        p.drawText(QRectF(col0, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, v1);
        p.drawText(QRectF(col1, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, v2);
        p.drawText(QRectF(col2, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, d);
    };

    const double anchor = viewAnchor(std::max(m_lastLength, 1));
    const double t1time = sampleIndexToTime(anchor + m_rulers.t1Offset);
    const double t2time = sampleIndexToTime(anchor + m_rulers.t2Offset);
    const double dt = layout.timeOn ? std::fabs(t2time - t1time) : 0.0;

    if (layout.timeOn) {
        p.setPen(Theme::plotForegroundText());
        p.drawLine(QPointF(swatchCentre(rowY).x() - 5.0, swatchCentre(rowY).y()),
                   QPointF(swatchCentre(rowY).x() + 5.0, swatchCentre(rowY).y()));
        dataRow(layout.t1PanelText, layout.t2PanelText, formatEng(dt, "s"));
        rowY += rowH;
    }

    for (int ch = 0; ch <= MaxChannels; ++ch) {
        const RulerChannelLayout &cl = layout.channels[ch];
        if (!cl.on)
            continue;
        p.setPen(Qt::NoPen);
        p.setBrush(cl.colour);
        p.drawRect(QRectF(swatchCentre(rowY).x() - 4.0, swatchCentre(rowY).y() - 4.0, 8.0, 8.0));
        p.setBrush(Qt::NoBrush);
        const ScaleUnit su{ cl.unitFactor, cl.unit };
        const QString dash = QStringLiteral("\u2013");
        dataRow(cl.v1Valid ? su.text(cl.v1Probed) : dash, cl.v2Valid ? su.text(cl.v2Probed) : dash,
                cl.v1Valid && cl.v2Valid ? su.text(std::fabs(cl.v2Probed - cl.v1Probed)) : dash);
        rowY += rowH;
        if (rulersTracking()) {
            // Slope between the two tracked points: rise over run, signed
            // (from ruler 1 to ruler 2), in the channel's own unit per second.
            const double run = t2time - t1time;
            QString slope = dash;
            if (cl.v1Valid && cl.v2Valid && run != 0.0) {
                const QString base = ch == MaxChannels ? mathUnit() : m_channels[ch].unit;
                const QByteArray unit = (base + QStringLiteral("/s")).toUtf8();
                slope = formatEng((cl.v2Probed - cl.v1Probed) / run, unit.constData());
            }
            p.setPen(Theme::plotAxisText());
            p.drawText(QRectF(col0, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("\u0394V/\u0394t"));
            p.setPen(Theme::plotForegroundText());
            p.drawText(QRectF(col1, rowY, colW * 2.0, rowH), Qt::AlignLeft | Qt::AlignVCenter, slope);
            rowY += rowH;
        }
    }

    if (layout.timeOn) {
        const double freq = dt > 0.0 ? 1.0 / dt : 0.0;
        p.setPen(Theme::plotAxisText());
        p.drawText(QRectF(box.left() + 6.0, rowY, kSwatchW + colW, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("1/\u0394"));
        p.setPen(Theme::plotForegroundText());
        p.drawText(QRectF(col1, rowY, colW, rowH), Qt::AlignLeft | Qt::AlignVCenter, formatEng(freq, "Hz"));
        rowY += rowH;
    }

    // Crank angle of each time cursor (and the cylinder it falls in), from
    // the engine overlay's cycles; the difference counts across cycles.
    if (layout.timeOn && m_engine.enabled && m_engine.cycleStarts.size() >= 2) {
        double d1 = 0.0, u1 = 0.0, d2 = 0.0, u2 = 0.0;
        int c1 = 0, c2 = 0;
        const bool ok1 = engineAngleAt(anchor + m_rulers.t1Offset, &d1, &u1, &c1);
        const bool ok2 = engineAngleAt(anchor + m_rulers.t2Offset, &d2, &u2, &c2);
        const auto angleText = [](bool ok, double deg, int cyl) {
            if (!ok)
                return QStringLiteral("\u2013");
            return cyl > 0 ? QStringLiteral("%1\u00b0 c%2").arg(deg, 0, 'f', 1).arg(cyl)
                           : QStringLiteral("%1\u00b0").arg(deg, 0, 'f', 1);
        };
        p.setPen(Theme::plotAxisText());
        p.drawText(QRectF(box.left() + 6.0, rowY, kSwatchW + colW, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("\u03b8"));
        p.setPen(Theme::plotForegroundText());
        dataRow(angleText(ok1, d1, c1), angleText(ok2, d2, c2),
                ok1 && ok2 ? QStringLiteral("%1\u00b0").arg(std::fabs(u2 - u1), 0, 'f', 1)
                           : QStringLiteral("\u2013"));
        rowY += rowH;
    }

    bool anyChannel = false;
    for (int ch = 0; ch <= MaxChannels; ++ch)
        anyChannel |= layout.channels[ch].on;
    if (!layout.timeOn && !anyChannel) {
        p.setPen(Theme::textDisabled());
        p.drawText(QRectF(box.left() + 8.0, rowY, box.width() - 16.0, rowH),
                   Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("No cursors selected"));
    }

    p.setClipRect(plot);
}

void ScopeView::drawGroundMarkers(QPainter &p, const QRectF &plot) const
{
    for (int ch = 0; ch < m_channelCount; ++ch) {
        const Channel &c = m_channels[ch];
        if (!c.visible)
            continue;

        const double y = valueToY(plot, c, 0.0);
        if (y < plot.top() - 1 || y > plot.bottom() + 1)
            continue;

        // On the same side as this channel's scale, and inside the graticule so
        // it cannot collide with the numbers in the gutter.
        const bool right = (ch == 1);
        const double tip  = right ? plot.right() - 1  : plot.left() + 1;
        const double back = right ? plot.right() - 10 : plot.left() + 10;

        QPainterPath arrow;
        arrow.moveTo(back, y - 5);
        arrow.lineTo(tip,  y);
        arrow.lineTo(back, y + 5);
        arrow.closeSubpath();

        p.setPen(Qt::NoPen);
        p.setBrush(c.color);
        p.drawPath(arrow);
        p.setBrush(Qt::NoBrush);
    }
}

void ScopeView::drawTriggerMarkers(QPainter &p, const QRectF &plot) const
{
    if (!m_triggerArmed)
        return;

    // Level marker, scaled by the source channel's own volts/div.
    if (m_triggerSource >= 0 && m_triggerSource < m_channelCount) {
        const Channel &c = m_channels[m_triggerSource];
        const double raw = valueToY(plot, c,
            (m_triggerLevel - c.probeOffset) / (qFuzzyIsNull(c.probeGain) ? 1.0 : c.probeGain));
        const bool offScreen = raw < plot.top() || raw > plot.bottom();
        const double y = std::clamp(raw, plot.top() + 1, plot.bottom() - 1);

        if (!offScreen) {
            p.setPen(QPen(Theme::triggerColor(), 1.0, Qt::DashLine));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }

        QPainterPath arrow;
        arrow.moveTo(plot.right() - 12, y - 5);
        arrow.lineTo(plot.right() - 2,  y);
        arrow.lineTo(plot.right() - 12, y + 5);
        arrow.closeSubpath();

        if (offScreen) {
            // Threshold is outside what this channel can show: hollow marker
            // parked at the edge, so it is obviously unreachable rather than
            // just missing.
            p.setPen(QPen(Theme::triggerColor(), 1.0));
            p.setBrush(Qt::NoBrush);
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(Theme::triggerColor());
        }
        p.drawPath(arrow);
        p.setBrush(Qt::NoBrush);
    }

    // Trigger instant. Once the view can be dragged this is no longer always
    // mid-screen, so place it from the window rather than assuming the centre.
    if (m_lastLength <= 0)
        return;
    const int length = std::min(windowLength(), m_lastLength);
    const int start  = windowStart(m_lastLength);
    if (length < 2 || m_triggerPoint < start || m_triggerPoint > start + length)
        return;

    const double x = plot.left() + plot.width() * (m_triggerPoint - start) / double(length);
    p.setPen(QPen(Theme::triggerColor(), 1.0, Qt::DashLine));
    p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));

    QPainterPath tag;
    tag.moveTo(x - 6, plot.top() - 8);
    tag.lineTo(x + 6, plot.top() - 8);
    tag.lineTo(x,     plot.top() - 1);
    tag.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(Theme::triggerColor());
    p.drawPath(tag);
    p.setBrush(Qt::NoBrush);
}

// Numeric scales: volts on the left in the colour of the channel the axis is
// following, time along the bottom with zero at the trigger instant.
// A channel's voltage scale, in the gutter on its own side: CH1 left, CH2
// right. Shown in millivolts up to a +/-500 mV page, so the largest number on
// the scale is 500 rather than a row of 0.1-style fractions.
// A channel's voltage scale, in the gutter on its own side: CH1 left, CH2
// right. Shown in millivolts up to a +/-500 mV page, so the largest number on
// the scale is 500 rather than a row of 0.1-style fractions.
void ScopeView::drawVoltScale(QPainter &p, const QRectF &plot,
                              int channel, bool leftSide) const
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    const Channel &c = m_channels[channel];
    if (!c.visible)
        return;

    const QFontMetrics fm(p.font());
    const double perDivision = c.voltsPerDiv / c.scale;

    // Millivolts only while every label still fits under 1000 of them: the
    // scale switches to volts as soon as the largest tick would reach 1000 mV.
    const bool inMillivolts = channelScaleUnit(channel).factor > 1.0;   // shared with the cursor flags
    const double step = inMillivolts ? perDivision * 1000.0 : perDivision;

    // The unit sits above the topmost number with clear air between them.
    p.setPen(c.color);
    // Sat closer to the graticule than the gutter width suggests: the numbers
    // belong to the plot, not to the window edge.
    constexpr double kScalePad = 4.0;
    const QRectF unitBox = leftSide
        ? QRectF(0, 1, kScaleGutter - kScalePad, kTopGutter - 14)
        : QRectF(plot.right() + kScalePad, 1, rightGutter() - kScalePad, kTopGutter - 14);
    p.drawText(unitBox, (leftSide ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
               inMillivolts ? QStringLiteral("m%1").arg(c.unit) : c.unit);

    for (int i = 0; i <= VerticalDivs; ++i) {
        const double y = plot.top() + plot.height() * i / VerticalDivs;
        const double divisionsFromCentre = VerticalDivs / 2.0 - i;
        const double volts = c.windowCentre - c.positionVolts
                           + divisionsFromCentre * perDivision;
        const double shown = inMillivolts ? volts * 1000.0 : volts;

        const QRectF box = leftSide
            ? QRectF(0, y - fm.height() / 2.0, kScaleGutter - kScalePad, fm.height())
            : QRectF(plot.right() + kScalePad, y - fm.height() / 2.0,
                     rightGutter() - kScalePad, fm.height());
        // The percentage offset shifts the ticks off round numbers, so the
        // label precision has to follow it.
        const double shift = (c.windowCentre - c.positionVolts)
                           * (inMillivolts ? 1000.0 : 1.0);
        p.drawText(box, (leftSide ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter,
                   axisNumber(shown, step, shift));
    }
}

void ScopeView::drawAxes(QPainter &p, const QRectF &plot) const
{
    QFont small = p.font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);

    drawVoltScale(p, plot, 0, true);
    if (m_channelCount > 1)
        drawVoltScale(p, plot, 1, false);
    drawMathScale(p, plot);
    for (int slot = 0; slot < MaxReferences; ++slot)
        drawReferenceScale(p, plot, slot);

    if (m_sampleRate <= 0.0 || m_lastLength <= 0)
        return;

    // Time axis: one unit for the whole scale rather than per label.
    const double window = m_secondsPerDiv * HorizontalDivs;
    double scale = 1.0;
    const char *unit = "s";
    if (window < 1e-3)      { scale = 1e6; unit = "us"; }
    else if (window < 1.0)  { scale = 1e3; unit = "ms"; }

    p.setPen(Theme::plotTimeText());
    const double step = m_secondsPerDiv * scale;
    const double offsetDivs = timeAxisOffsetDivs();

    // Labels sit at the same "nice" time-value positions drawGraticule()'s
    // own vertical lines just used, so a label is always directly under
    // its own line rather than the two disagreeing about where a given
    // time value actually is — and both slide together during a pan.
    const int firstK = int(std::floor(offsetDivs - HorizontalDivs / 2.0)) - 1;
    const int lastK  = int(std::ceil(offsetDivs + HorizontalDivs / 2.0)) + 1;
    for (int k = firstK; k <= lastK; ++k) {
        const double i = k - offsetDivs + HorizontalDivs / 2.0;
        if (i < -0.05 || i > HorizontalDivs + 0.05)
            continue;
        const double x = plot.left() + plot.width() * i / HorizontalDivs;
        const double t = k * step;
        const QString text = axisNumber(t, step);

        // The end labels are aligned to the graticule edge rather than centred
        // on it, so they cannot reach into the gutters and collide with the
        // voltage scale or the unit. Keyed on position now, not a fixed
        // i == 0 / i == HorizontalDivs index, since a label may or may not
        // land exactly on either true edge once the whole grid can slide.
        QRectF box(x - 30, plot.bottom() + 2, 60, kAxisHeight);
        int align = Qt::AlignCenter;
        if (i < 0.5) {
            box = QRectF(plot.left(), plot.bottom() + 2, 60, kAxisHeight);
            align = Qt::AlignLeft | Qt::AlignVCenter;
        } else if (i > HorizontalDivs - 0.5) {
            box = QRectF(plot.right() - 60, plot.bottom() + 2, 60, kAxisHeight);
            align = Qt::AlignRight | Qt::AlignVCenter;
        }
        p.drawText(box, align, text);
    }

    // The unit goes in the left gutter, clear of the end label now that the
    // end label starts at the graticule edge.
    p.drawText(QRectF(0, plot.bottom() + 2, leftGutter() - 10, kAxisHeight),
               Qt::AlignRight | Qt::AlignVCenter, QString::fromLatin1(unit));
}

QFont ScopeView::readoutFont() const
{
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.5), small.pointSizeF() - Fonts::scaled(1.0)) * 1.10);
    return small;
}

// One line per visible channel (scale, INV, active measurements) plus Math.
QVector<ScopeView::ReadoutLine> ScopeView::readoutLines() const
{
    QVector<ReadoutLine> lines;
    for (int ch = 0; ch < m_channelCount; ++ch) {
        const Channel &c = m_channels[ch];
        if (!c.visible)
            continue;
        bool anyMeasurementActive = false;
        for (bool v : c.activeMeasurements) {
            if (v) { anyMeasurementActive = true; break; }
        }
        const Measurements m = anyMeasurementActive ? measure(c) : Measurements{};
        const double page = c.voltsPerDiv / c.scale * (VerticalDivs / 2.0);
        QString line = QStringLiteral("CH%1  \u00b1%2")
            .arg(ch + 1)
            .arg(formatEng(page, c.unit.toUtf8().constData()));
        if (c.inverted)
            line += QStringLiteral("  INV");
        {
            const QByteArray unit = c.unit.toUtf8();
            const QString extra = formatMeasurements(m, c.activeMeasurements, unit.constData());
            if (!extra.isEmpty())
                line += QStringLiteral("   ") + extra;
        }
        lines.append({ line, c.color, c.overRange });
    }
    if (m_math.enabled && !m_math.samples.isEmpty()) {
        QString line = QStringLiteral("M  %1").arg(mathLabel());
        bool anyMathMeasurementActive = false;
        for (bool v : m_math.activeMeasurements) {
            if (v) { anyMathMeasurementActive = true; break; }
        }
        if (anyMathMeasurementActive) {
            const QByteArray mathUnitUtf8 = mathUnit().toUtf8();
            const QString extra = formatMeasurements(measureMath(), m_math.activeMeasurements,
                                                     mathUnitUtf8.constData());
            if (!extra.isEmpty())
                line += QStringLiteral("   ") + extra;
        }
        lines.append({ line, m_math.color, false });
    }
    return lines;
}

// Room the engine overlay's label rows take along the top, and the bus rows
// and timing-chart legend along the bottom — the readouts' corners sit
// clear of both.
double ScopeView::topReservedHeight() const
{
    if (!m_engine.enabled)
        return 0.0;
    // Only rows that are actually drawn: the first holds the degree scale
    // and the manual markers' flags, the second the cylinder numbers.
    // Reserving both regardless left an empty strip at the top whenever no
    // cycle was found.
    int rows = 0;
    if (m_engine.manual || m_engine.cycleStarts.size() >= 2)
        rows = 1;
    if (!m_engine.bands.isEmpty())
        rows = 2;
    if (rows == 0)
        return 0.0;
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    const double rowH = QFontMetricsF(small).height() + 4.0;
    return rowH * rows + 6.0;
}

double ScopeView::timingLegendHeight() const
{
    if (!m_engine.enabled || !m_engine.timingChart || m_engine.order.isEmpty()
        || m_engine.cycleStarts.size() < 2)
        return 0.0;
    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    return QFontMetricsF(small).height() + 8.0;
}

double ScopeView::bottomReservedHeight() const
{
    int rows = 0;
    for (const BusRow &row : m_busRows)
        rows += row.visible ? 1 : 0;
    double height = timingLegendHeight();
    if (rows > 0) {
        QFont small = font();
        small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
        height += rows * (QFontMetricsF(small).height() + 8.0 + 2.0) + 2.0;
    }
    return height;
}

QRectF ScopeView::readoutBlockRect(const QRectF &plot, const QVector<ReadoutLine> &lines) const
{
    const QFontMetrics fm(readoutFont());
    double width = 0.0;
    for (const ReadoutLine &l : lines) {
        double w = fm.horizontalAdvance(l.text) + 10.0;
        if (l.overRange)
            w += fm.horizontalAdvance(QStringLiteral("OVER RANGE")) + 16.0;
        width = std::max(width, w);
    }
    const double height = lines.size() * (fm.height() + 6.0) - 2.0;
    QPointF topLeft;
    if (m_readoutDragging) {
        topLeft = m_readoutDragTopLeft;
    } else {
        const bool right = m_readoutCorner == 1 || m_readoutCorner == 3;
        const bool bottom = m_readoutCorner == 2 || m_readoutCorner == 3;
        topLeft.setX(right ? plot.right() - 6.0 - width : plot.left() + 6.0);
        topLeft.setY(bottom ? plot.bottom() - 6.0 - bottomReservedHeight() - height
                            : plot.top() + 6.0 + topReservedHeight());
    }
    topLeft.setX(std::clamp(topLeft.x(), plot.left(), std::max(plot.left(), plot.right() - width)));
    topLeft.setY(std::clamp(topLeft.y(), plot.top(), std::max(plot.top(), plot.bottom() - height)));
    return QRectF(topLeft, QSizeF(width, height));
}

void ScopeView::setReadoutCorner(int corner)
{
    m_readoutCorner = std::clamp(corner, 0, 3);
    update();
}

void ScopeView::drawReadouts(QPainter &p, const QRectF &plot) const
{
    const QVector<ReadoutLine> lines = readoutLines();
    if (lines.isEmpty())
        return;
    const QFont small = readoutFont();
    p.setFont(small);
    const QFontMetrics fm(small);
    const QRectF block = readoutBlockRect(plot, lines);
    // Lines hug the block's outer edge: right-aligned in the right corners.
    const bool right = !m_readoutDragging && (m_readoutCorner == 1 || m_readoutCorner == 3);
    const QString warn = QStringLiteral("OVER RANGE");
    const double warnW = fm.horizontalAdvance(warn) + 10.0;
    double y = block.top();
    for (const ReadoutLine &l : lines) {
        const double w = fm.horizontalAdvance(l.text) + 10.0;
        const double h = fm.height() + 4.0;
        double x = right ? block.right() - w : block.left();
        if (right && l.overRange)
            x -= warnW + 6.0;
        const QRectF box(x, y, w, h);
        p.fillRect(box, Theme::plotLabelBackdrop());
        p.setPen(l.colour);
        p.drawText(box.adjusted(5, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop, l.text);
        if (l.overRange) {
            const QRectF wbox(box.right() + 6.0, y, warnW, h);
            p.fillRect(wbox, QColor(0x60, 0x10, 0x10));
            p.setPen(QColor(0xff, 0x5a, 0x5a));
            p.drawText(wbox.adjusted(5, 2, 0, 0), Qt::AlignLeft | Qt::AlignTop, warn);
        }
        y += fm.height() + 6.0;
    }
}

void ScopeView::drawStatusStrip(QPainter &p, const QRectF &plot) const
{
    const QRectF strip(plot.left(), plot.bottom() + kAxisHeight + 2,
                       plot.width(), kStripHeight);

    QString state;
    if (!m_running)
        state = QStringLiteral("STOP");
    else if (m_triggerArmed)
        state = QStringLiteral("TRIG");
    else
        state = QStringLiteral("AUTO");

    const QString slope = m_triggerSlope == 0 ? QStringLiteral("Rising")
                        : m_triggerSlope == 1 ? QStringLiteral("Falling")
                                              : QStringLiteral("Either");

    QString left = QStringLiteral("%1   %2/div")
        .arg(state, formatEng(m_secondsPerDiv, "s"));
    const double shownRate = m_displayRateOverride > 0.0
        ? m_displayRateOverride : m_sampleRate;
    if (shownRate > 0.0)
        left += QStringLiteral("   %1").arg(formatEng(shownRate, "S/s"));
    if (m_lastLength > 0)
        left += QStringLiteral("   %1 pts").arg(m_lastLength);

    const QString right = m_triggerArmed
        ? QStringLiteral("Trig CH%1  %2  %3")
              .arg(m_triggerSource + 1).arg(slope, formatEng(m_triggerLevel, "V"))
        : QStringLiteral("Trig off (free run)");

    p.setPen(Theme::plotAxisText());
    p.drawText(strip, Qt::AlignLeft  | Qt::AlignVCenter, left);
    p.setPen(m_triggerArmed ? Theme::triggerColor() : Theme::plotAxisText());
    p.drawText(strip, Qt::AlignRight | Qt::AlignVCenter, right);
}

// Drawn last, on top of everything else, so the buttons are always
// reachable regardless of what trace or overlay happens to sit underneath
// them. Each arrow only appears while MainWindow's own zoom history
// actually has something in that direction — see setZoomHistoryAvailable().
void ScopeView::drawZoomHistoryButtons(QPainter &p, const QRectF &plot) const
{
    if (!m_zoomHistoryCanBack && !m_zoomHistoryCanForward)
        return;

    // Above the plot: drawRulers() leaves the clip on the plot (for what it
    // draws after), which hid these whenever cursors were on.
    p.setClipping(false);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont glyphFont = font();
    glyphFont.setPointSizeF(std::max(Fonts::scaled(9.0), glyphFont.pointSizeF()));
    glyphFont.setBold(true);
    p.setFont(glyphFont);

    const auto drawButton = [&](const QRectF &rect, const QString &glyph, const QString &iconName) {
        p.setPen(Qt::NoPen);
        p.setBrush(Theme::elevatedBackground());
        p.drawRoundedRect(rect, 5.0, 5.0);
        if (Icons::hasSvg(iconName)) {
            const int side = int(std::lround(std::min(rect.width(), rect.height()) * 0.72));
            const QPixmap px = Icons::pixmap(iconName, Theme::accent(), side);
            p.drawPixmap(QPointF(rect.center().x() - side / 2.0, rect.center().y() - side / 2.0), px);
        } else {
            p.setPen(Theme::accent());
            p.drawText(rect, Qt::AlignCenter, glyph);
        }
    };

    if (m_zoomHistoryCanBack)
        drawButton(zoomBackRect(plot), QStringLiteral("\u2190"), QStringLiteral("zoom-back"));
    if (m_zoomHistoryCanForward)
        drawButton(zoomForwardRect(plot), QStringLiteral("\u2192"), QStringLiteral("zoom-forward"));
}
