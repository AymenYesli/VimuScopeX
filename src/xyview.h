#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

/*
 * Classic oscilloscope XY mode: one channel's voltage drives X, another
 * drives Y, and consecutive (x[i], y[i]) sample pairs are connected in
 * order — a Lissajous figure for two related periodic signals, a
 * hysteresis loop for others. Shown in the same second-pane slot Spectrum
 * uses (see MainWindow's m_secondPaneStack), so only one of the two is
 * ever visible at a time; this widget has no idea Spectrum exists.
 *
 * Each axis uses a fixed range set from outside (setRanges()) — the same
 * "the scale is a stable setting, not a fit to whatever the data currently
 * peaks at" convention the main plot (a channel's own volts/div) and
 * Spectrum (a fixed dB span) both already use. MainWindow supplies each
 * axis's assigned channel's own configured full-scale range
 * (ScopeView::channelFullScaleVolts()) so the numbers on screen always
 * match that channel's own card exactly, and stay fixed as the signal
 * itself moves around rather than drifting from frame to frame.
 */
class XYView : public QWidget {
    Q_OBJECT

public:
    explicit XYView(QWidget *parent = nullptr);

    void setTraceColor(const QColor &color);

    // Each axis's half-range in volts — the axis spans -range..+range.
    // Supplied by MainWindow from the assigned channel's own
    // channelFullScaleVolts(), not computed here.
    void setRanges(double xRange, double yRange);

    // Both arrays are already in display volts — probe gain, DC offset,
    // and inversion applied, the same convention Measure/Spectrum use.
    // Paired sample-by-sample; a length mismatch (uncommon, but possible if
    // one channel updated more recently than the other) uses only the
    // overlapping prefix rather than guessing at the rest.
    void setSamples(const QVector<double> &xSamples, const QVector<double> &ySamples);
    void clear();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QRectF plotRect() const;
    void drawGraticule(QPainter &p, const QRectF &plot) const;
    void drawTrace(QPainter &p, const QRectF &plot) const;

    QVector<double> m_xSamples;
    QVector<double> m_ySamples;
    double m_xRange = 1.0;   // symmetric half-range: axis spans -m_xRange..+m_xRange
    double m_yRange = 1.0;
    QColor m_color = Qt::white;
    bool m_hasData = false;
};
