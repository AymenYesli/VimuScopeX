#include "navigatorview.h"
#include "perftrace.h"
#include "mathops.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include "theme.h"

#include <algorithm>
#include <cmath>

namespace {
QColor viewportFill() { QColor c = Theme::accent(); c.setAlpha(48); return c; }
QColor viewportEdge() { return Theme::accent(); }
}

NavigatorView::NavigatorView(QWidget *parent)
    : QWidget(parent)
{
    setFixedHeight(49);   // 15% shorter
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral(
        "The whole capture. Drag to move the view; double click to re-centre "
        "on the trigger."));
}

void NavigatorView::setChannelCount(int count)
{
    m_channelCount = std::clamp(count, 0, MaxChannels);
    update();
}

void NavigatorView::setChannelVisible(int channel, bool visible)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].visible = visible;
    if (!visible)
        m_channels[channel].samples.clear();
    update();
}

void NavigatorView::setChannelColor(int channel, const QColor &color)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].color = color;
    update();
}

void NavigatorView::setVoltsPerDiv(int channel, double voltsPerDiv)
{
    if (channel < 0 || channel >= MaxChannels || voltsPerDiv <= 0.0)
        return;
    m_channels[channel].voltsPerDiv = voltsPerDiv;
    update();
}

void NavigatorView::setProbe(int channel, double gradient, double offset)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].probeGain   = qFuzzyIsNull(gradient) ? 1.0 : gradient;
    m_channels[channel].probeOffset = offset;
    recomputeMath(channel);   // Math combines measured values
    update();
}

void NavigatorView::setDisplayScale(int channel, double scale)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].scale = scale > 0.0 ? scale : 1.0;
    update();
}

void NavigatorView::setOffset(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].offset = volts;
    update();
}

void NavigatorView::setInverted(int channel, bool inverted)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].inverted = inverted;
    recomputeMath(channel);
    update();
}

void NavigatorView::setPositionVolts(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].positionVolts = volts;
    update();
}

void NavigatorView::setWindowCentre(int channel, double volts)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].windowCentre = volts;
    update();
}

void NavigatorView::setSamples(int channel, const QVector<double> &samples)
{
    if (channel < 0 || channel >= MaxChannels)
        return;
    m_channels[channel].samples = samples;
    recomputeMath(channel);
    update();
}

void NavigatorView::setMathChannel(bool enabled, int op, int sourceA, int sourceB)
{
    m_math.enabled = enabled;
    m_math.op      = op;
    m_math.sourceA = std::clamp(sourceA, 0, MaxChannels - 1);
    m_math.sourceB = std::clamp(sourceB, 0, MaxChannels - 1);
    recomputeMath(m_math.sourceA);
    recomputeMath(m_math.sourceB);
    update();
}

void NavigatorView::setMathColor(const QColor &color)
{
    m_math.color = color;
    update();
}

void NavigatorView::setMathScale(double voltsPerDiv, double positionVolts)
{
    m_math.voltsPerDiv = voltsPerDiv;
    m_math.positionVolts = positionVolts;
    update();
}

void NavigatorView::setMathScaleParams(double k, double c)
{
    m_math.scaleK = k;
    m_math.scaleC = c;
    recomputeMath(m_math.sourceA);
    update();
}

// The plot's engine on the measured (probed, inverted) values. The strip is
// auto-scaled, so dA/dt and the integral only need their shape: any rate
// constant will do (this widget doesn't know the sample rate).
void NavigatorView::recomputeMath(int changedChannel)
{
    if (!m_math.enabled)
        return;
    const bool twoSources = MathOps::usesB(m_math.op);
    if (changedChannel != m_math.sourceA && (!twoSources || changedChannel != m_math.sourceB))
        return;
    const auto measured = [](const Channel &c) {
        std::vector<double> v(size_t(c.samples.size()));
        const double sign = c.inverted ? -1.0 : 1.0;
        for (int i = 0; i < c.samples.size(); ++i)
            v[size_t(i)] = (c.samples[i] * c.probeGain + c.probeOffset) * sign;
        return v;
    };
    const std::vector<double> a = measured(m_channels[m_math.sourceA]);
    const std::vector<double> b = twoSources ? measured(m_channels[m_math.sourceB]) : std::vector<double>();
    const std::vector<double> r = MathOps::compute(m_math.op, a, b, 1.0, m_math.scaleK, m_math.scaleC);
    m_math.samples = QVector<double>(r.begin(), r.end());
}

void NavigatorView::clearAll()
{
    for (Channel &c : m_channels)
        c.samples.clear();
    update();
}

void NavigatorView::setViewport(double centreFraction, double widthFraction)
{
    m_centre = std::clamp(centreFraction, 0.0, 1.0);
    m_width  = std::clamp(widthFraction, 0.0005, 1.0);
    update();
}

void NavigatorView::setTriggerFraction(double fraction)
{
    m_triggerFraction = fraction;
    update();
}

void NavigatorView::setFollowingTrigger(bool following)
{
    if (m_following == following)
        return;
    m_following = following;
    update();
}

// ---------------------------------------------------------------------------

void NavigatorView::paintEvent(QPaintEvent *)
{
    VSX_TRACE("nav.paintEvent");
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);

    const QRectF band = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    QPainterPath background;
    background.addRoundedRect(band, Theme::cornerRadius(), Theme::cornerRadius());
    p.setClipPath(background);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillPath(background, Theme::plotBackground());
    p.setRenderHint(QPainter::Antialiasing, false);

    // Channels use the SAME vertical scale as the plot, not a per-channel
    // normalisation. Normalising made an unconnected input's few millivolts of
    // noise fill the whole band and look like a large signal.
    const int columns = int(band.width());
    for (int ch = 0; ch < m_channelCount && columns > 1; ++ch) {
        const Channel &c = m_channels[ch];
        if (!c.visible || c.samples.size() < 2)
            continue;

        const int n = c.samples.size();
        const double top    = band.top() + 2;
        const double bottom = band.bottom() - 2;
        const double centre = (top + bottom) / 2.0;
        const double perDiv = (bottom - top) / VerticalDivs;

        const auto toY = [&](double v) {
            const double probed = v * c.probeGain + c.probeOffset;
            const double signal = probed * (c.inverted ? -1.0 : 1.0) + c.offset;
            const double divisions =
                (signal - c.windowCentre + c.positionVolts) / (c.voltsPerDiv / c.scale);
            return std::clamp(centre - divisions * perDiv, top, bottom);
        };

        QVector<QLineF> bars;
        bars.reserve(columns);
        for (int col = 0; col < columns; ++col) {
            const int a = int(qint64(n) * col / columns);
            // At least two points per column: an overview fed min/max pairs
            // (Record's readDecimated(), Live's own decimation) often has
            // fewer points than the strip has pixels, and a column holding a
            // single point draws a dot at either the min or the max, so the
            // band broke up into two dotted lines (and columns with no point
            // at all were skipped). Widening to the next point keeps each
            // column's bar spanning a real envelope.
            int b = int(qint64(n) * (col + 1) / columns);
            if (b - a < 2)
                b = std::min(n, a + 2);
            if (b <= a)
                continue;
            const auto [cl, ch2] = std::minmax_element(c.samples.begin() + a,
                                                       c.samples.begin() + b);
            bars.append(QLineF(band.left() + col, toY(*ch2),
                               band.left() + col, toY(*cl)));
        }
        p.setPen(QPen(c.color, 1.0));
        p.drawLines(bars);
    }

    if (m_math.enabled && m_math.samples.size() >= 2 && columns > 1) {
        const QVector<double> &s = m_math.samples;
        const int n = s.size();
        const double top    = band.top() + 2;
        const double bottom = band.bottom() - 2;
        const double centre = (top + bottom) / 2.0;

        double mid, scale;
        if (m_math.voltsPerDiv > 0.0) {
            // Manual: mirrors ScopeView's own math mapping exactly (same
            // mid/scale shape as valueToY()), so math shows at the same
            // scale here as in the main plot — an independent auto-fit is
            // what let it dominate the strip regardless of how it was
            // actually scaled.
            const double perDiv = (bottom - top) / VerticalDivs;
            scale = perDiv / m_math.voltsPerDiv;
            mid = -m_math.positionVolts;
        } else {
            // Auto: fit the whole buffer's min/max to most of the band —
            // unchanged from before, since Auto has no fixed scale to match.
            double lo = s[0], hi = s[0];
            for (int i = 1; i < n; ++i) {
                lo = std::min(lo, s[i]);
                hi = std::max(hi, s[i]);
            }
            const double span = std::max(hi - lo, 1e-12);
            scale = (bottom - top) * 0.8 / span;
            mid = (hi + lo) / 2.0;
        }
        const auto toY = [&](double v) {
            return std::clamp(centre - (v - mid) * scale, top, bottom);
        };

        QVector<QLineF> bars;
        bars.reserve(columns);
        for (int col = 0; col < columns; ++col) {
            const int a = int(qint64(n) * col / columns);
            // At least two points per column: an overview fed min/max pairs
            // (Record's readDecimated(), Live's own decimation) often has
            // fewer points than the strip has pixels, and a column holding a
            // single point draws a dot at either the min or the max, so the
            // band broke up into two dotted lines (and columns with no point
            // at all were skipped). Widening to the next point keeps each
            // column's bar spanning a real envelope.
            int b = int(qint64(n) * (col + 1) / columns);
            if (b - a < 2)
                b = std::min(n, a + 2);
            if (b <= a)
                continue;
            const auto [cl, ch2] = std::minmax_element(s.begin() + a, s.begin() + b);
            bars.append(QLineF(band.left() + col, toY(*ch2),
                               band.left() + col, toY(*cl)));
        }
        p.setPen(QPen(m_math.color, 1.0, Qt::DashLine));
        p.drawLines(bars);
    }

    if (m_triggerFraction >= 0.0 && m_triggerFraction <= 1.0) {
        const double x = band.left() + band.width() * m_triggerFraction;
        p.setPen(QPen(Theme::triggerColor(), 1.0, Qt::DashLine));
        p.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
    }

    // The part currently on screen.
    const double w = std::max(3.0, band.width() * m_width);
    double x = band.left() + band.width() * m_centre - w / 2.0;
    x = std::clamp(x, band.left(), band.right() - w);
    const QRectF viewport(x, band.top(), w, band.height());

    p.setBrush(viewportFill());
    p.setPen(QPen(m_following ? Theme::triggerColor() : viewportEdge(), 1.0));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.drawRoundedRect(viewport.adjusted(0.5, 0.5, -0.5, -0.5),
                       Theme::cornerRadius(), Theme::cornerRadius());
}

void NavigatorView::emitCentreFor(double x)
{
    const double width = std::max(1.0, double(rect().width()));
    emit centreRequested(std::clamp(x / width, 0.0, 1.0));
}

void NavigatorView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    m_dragging = true;
    setCursor(Qt::ClosedHandCursor);
    // On the box: take hold of it where it was pressed — nothing moves until
    // it's dragged (a click used to re-centre it on the pointer). Elsewhere
    // on the strip: jump there, as before. A tiny box is grabbable as 8 px.
    const double width = std::max(1.0, double(rect().width()));
    const double at = event->position().x() / width;
    const double half = std::max(m_width / 2.0, 4.0 / width);
    if (std::fabs(at - m_centre) <= half) {
        m_grabOffset = at - m_centre;
    } else {
        m_grabOffset = 0.0;
        emitCentreFor(event->position().x());
    }
}

void NavigatorView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const double width = std::max(1.0, double(rect().width()));
        emitCentreFor(event->position().x() - m_grabOffset * width);
    }
}

void NavigatorView::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    setCursor(Qt::OpenHandCursor);
}

void NavigatorView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        emit followTriggerRequested();
}
