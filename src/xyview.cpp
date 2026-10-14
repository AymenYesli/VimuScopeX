#include "xyview.h"
#include "fonts.h"

#include "scopeview.h"
#include "theme.h"

#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

namespace {


// A reasonable minimum half-range so a flat (or all-zero) signal on either
// axis does not collapse that axis to zero width.
constexpr double kMinRange = 0.05;

// Y numbers sit on the right, mirroring ScopeView's own single-channel
// voltage scale placement (see drawVoltScale()'s "channel on the right"
// case) — kTopGutter leaves room for a unit label sitting cleanly above
// the topmost tick, the same way ScopeView's own kTopGutter (28) does.
constexpr double kLeftGutter = 10.0, kRightGutter = 54.0;
constexpr double kTopGutter = 28.0, kBottomGutter = 20.0;

QString formatVolts(double v)
{
    if (std::fabs(v) < 1.0)
        return QStringLiteral("%1 mV").arg(v * 1000.0, 0, 'f', 0);
    return QStringLiteral("%1 V").arg(v, 0, 'f', 2);
}

}   // namespace

XYView::XYView(QWidget *parent)
    : QWidget(parent)
{
    setMinimumSize(300, 140);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

void XYView::setTraceColor(const QColor &color)
{
    m_color = color;
    update();
}

void XYView::setRanges(double xRange, double yRange)
{
    // Guards against a degenerate 0 (an invalid channel, or one with no
    // configured scale yet) rather than dividing by zero later in
    // drawGraticule()/drawTrace().
    m_xRange = std::max(kMinRange, xRange);
    m_yRange = std::max(kMinRange, yRange);
    update();
}

void XYView::clear()
{
    m_hasData = false;
    m_xSamples.clear();
    m_ySamples.clear();
    update();
}

void XYView::setSamples(const QVector<double> &xSamples, const QVector<double> &ySamples)
{
    // The overlapping prefix only — a length mismatch is uncommon (it means
    // one channel's update reached here before the other's did) but using
    // only what both actually have avoids reading past either array's end
    // rather than guessing at the missing tail.
    const int n = std::min(xSamples.size(), ySamples.size());
    if (n < 2) {
        m_hasData = false;
        update();
        return;
    }

    m_xSamples = xSamples.mid(0, n);
    m_ySamples = ySamples.mid(0, n);

    m_hasData = true;
    update();
}

QRectF XYView::plotRect() const
{
    return QRectF(kLeftGutter, kTopGutter,
                 std::max(10.0, width() - kLeftGutter - kRightGutter),
                 std::max(10.0, height() - kTopGutter - kBottomGutter));
}

void XYView::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath background;
    background.addRoundedRect(rect(), Theme::cornerRadius(), Theme::cornerRadius());
    p.setClipPath(background);
    p.fillPath(background, Theme::plotBackground());

    const QRectF plot = plotRect();
    drawGraticule(p, plot);

    if (m_hasData) {
        drawTrace(p, plot);
    } else {
        p.setPen(Theme::plotAxisText());
        p.drawText(plot, Qt::AlignCenter, QStringLiteral("Waiting for samples..."));
    }
}

void XYView::drawGraticule(QPainter &p, const QRectF &plot) const
{
    // Same division counts as ScopeView's own graticule, and the same
    // "drop to 8 rows once short" rule Spectrum already applies, so all
    // three plots' grids read as one visual family regardless of which
    // pane happens to be showing.
    const int divsX = ScopeView::HorizontalDivs;
    constexpr double kShortPlotThreshold = 220.0;
    const int divsY = (plot.height() < kShortPlotThreshold) ? 8 : ScopeView::VerticalDivs;

    p.setPen(QPen(Theme::plotGridMinor(), 1.0, Qt::DotLine));
    for (int i = 1; i < divsX; ++i) {
        const double x = plot.left() + plot.width() * i / divsX;
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
    }
    for (int i = 1; i < divsY; ++i) {
        const double y = plot.top() + plot.height() * i / divsY;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }

    // Centre axes with 1/5-division ticks, exactly mirroring
    // ScopeView's/SpectrumView's own graticule.
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

    p.setPen(Theme::plotGridMajor());
    p.setBrush(Qt::NoBrush);
    p.drawRect(plot);

    QFont small = font();
    small.setPointSizeF(std::max(Fonts::scaled(7.0), small.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    p.setFont(small);
    const QFontMetrics fm(small);

    // Y axis (right) — redesigned to mirror ScopeView's own drawVoltScale()
    // exactly, not a generic math-plot left-side layout: millivolts below
    // 1 V (assumes a plain voltage unit — unlike a real channel, XY has no
    // per-channel probe unit of its own to check), the unit sitting above
    // the topmost tick with the same clean gap drawVoltScale() leaves,
    // numbers via the same ScopeView::axisNumber() rounding every other
    // axis in this app already uses, and the whole thing in the Y
    // channel's own colour — exactly how that channel's voltage scale
    // looks on the time-domain plot.
    const double perDivision  = (2.0 * m_yRange) / divsY;
    const bool inMillivolts   = m_yRange < 1.0;
    const double step         = inMillivolts ? perDivision * 1000.0 : perDivision;
    constexpr double kScalePad = 4.0;

    p.setPen(m_color);
    p.drawText(QRectF(plot.right() + kScalePad, 1, kRightGutter - kScalePad, plot.top() - 14),
              Qt::AlignLeft | Qt::AlignVCenter,
              inMillivolts ? QStringLiteral("mV") : QStringLiteral("V"));
    for (int i = 0; i <= divsY; ++i) {
        const double y = plot.top() + plot.height() * i / divsY;
        const double divisionsFromCentre = divsY / 2.0 - i;
        const double volts = divisionsFromCentre * perDivision;
        const double shown = inMillivolts ? volts * 1000.0 : volts;
        p.drawText(QRectF(plot.right() + kScalePad, y - fm.height() / 2.0,
                          kRightGutter - kScalePad, fm.height()),
                  Qt::AlignLeft | Qt::AlignVCenter, ScopeView::axisNumber(shown, step));
    }

    // X axis (bottom), symmetric about 0 at the horizontal centre. End
    // labels aligned to the graticule edge rather than centred on it,
    // mirroring ScopeView's/SpectrumView's own end-label handling so
    // neither one crowds whatever sits beside this widget. Left as its own
    // simpler formatting (not axisNumber()) — only the vertical scale was
    // asked to match the time-domain design.
    p.setPen(Theme::plotAxisText());
    for (int i = 0; i <= divsX; ++i) {
        const double frac = double(i) / divsX;
        const double v = (frac * 2.0 - 1.0) * m_xRange;
        const double x = plot.left() + plot.width() * i / divsX;
        QRectF box(x - 40, plot.bottom() + 2, 80, 16);
        int align = Qt::AlignHCenter | Qt::AlignTop;
        if (i == 0) {
            box = QRectF(plot.left(), plot.bottom() + 2, 80, 16);
            align = Qt::AlignLeft | Qt::AlignTop;
        } else if (i == divsX) {
            box = QRectF(plot.right() - 80, plot.bottom() + 2, 80, 16);
            align = Qt::AlignRight | Qt::AlignTop;
        }
        p.drawText(box, align, formatVolts(v));
    }
}

void XYView::drawTrace(QPainter &p, const QRectF &plot) const
{
    const int n = std::min(m_xSamples.size(), m_ySamples.size());
    if (n < 2)
        return;

    QPolygonF poly;
    poly.reserve(n);
    for (int i = 0; i < n; ++i) {
        const double xFrac = (m_xSamples[i] + m_xRange) / (2.0 * m_xRange);
        const double yFrac = (m_ySamples[i] + m_yRange) / (2.0 * m_yRange);
        const double x = plot.left() + std::clamp(xFrac, 0.0, 1.0) * plot.width();
        const double y = plot.bottom() - std::clamp(yFrac, 0.0, 1.0) * plot.height();
        poly.append(QPointF(x, y));
    }

    p.setPen(QPen(m_color, 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(poly.constData(), n);
}
