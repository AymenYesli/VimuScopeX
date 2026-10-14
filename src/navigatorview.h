#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include "theme.h"

#include <array>

/*
 * The band above the plot: the entire capture drawn compressed, with the part
 * currently on screen highlighted. Drag it to move through the capture.
 *
 * Necessary because a capture is far longer than a screen — at 64 KB and
 * 20 us/div the plot shows about 8% of what was acquired.
 */
class NavigatorView : public QWidget {
    Q_OBJECT

public:
    static constexpr int MaxChannels = 4;

    explicit NavigatorView(QWidget *parent = nullptr);

    void setChannelCount(int count);
    void setChannelVisible(int channel, bool visible);
    void setChannelColor(int channel, const QColor &color);
    void setVoltsPerDiv(int channel, double voltsPerDiv);
    void setProbe(int channel, double gradient, double offset);
    void setDisplayScale(int channel, double scale);
    void setOffset(int channel, double volts);
    void setPositionVolts(int channel, double volts);
    void setWindowCentre(int channel, double volts);
    void setInverted(int channel, bool inverted);
    void setSamples(int channel, const QVector<double> &samples);
    void clearAll();

    // Mirrors ScopeView's math channel, computed independently from the same
    // per-channel samples this widget already receives — always shown as a
    // whole-buffer auto-fit, since this is an overview strip, not somewhere
    // volts/div readings matter. op is a MathOps::Op (mathops.h, the same
    // engine the plot uses).
    void setMathChannel(bool enabled, int op, int sourceA, int sourceB);
    void setMathScaleParams(double k, double c);
    void setMathColor(const QColor &color);
    // <= 0 for voltsPerDiv means auto-fit, matching ScopeView's own
    // convention, so the navigator's math trace tracks the same scale the
    // main plot actually uses instead of always auto-fitting independently.
    void setMathScale(double voltsPerDiv, double positionVolts);

    // All fractions are 0..1 of the whole capture.
    void setViewport(double centreFraction, double widthFraction);
    void setTriggerFraction(double fraction);
    void setFollowingTrigger(bool following);

signals:
    void centreRequested(double fraction);
    void followTriggerRequested();       // double click: back to the trigger

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    struct Channel {
        QVector<double> samples;
        QColor color       = Qt::white;
        double voltsPerDiv = 0.5;
        double probeGain   = 1.0;
        double probeOffset = 0.0;
        double scale       = 1.0;
        double offset      = 0.0;
        double positionVolts = 0.0;
        double windowCentre = 0.0;
        bool   inverted    = false;
        bool   visible     = false;
    };

    static constexpr int VerticalDivs = 10;   // same graticule as the plot

    void emitCentreFor(double x);
    void recomputeMath(int changedChannel);

    struct MathState {
        bool   enabled = false;
        int    op       = 0;
        int    sourceA  = 0;
        int    sourceB  = 1;
        QColor color    = Theme::mathColor();
        double voltsPerDiv   = -1.0;   // <= 0 : auto-fit, mirrors ScopeView's own convention
        double positionVolts = 0.0;
        double scaleK = 1.0, scaleC = 0.0;
        QVector<double> samples;
    } m_math;

    std::array<Channel, MaxChannels> m_channels;
    int    m_channelCount    = 0;
    double m_centre          = 0.5;
    double m_width           = 1.0;
    double m_triggerFraction = -1.0;
    bool   m_following       = true;
    bool   m_dragging        = false;
    double m_grabOffset      = 0.0;   // where in the box it was taken hold of (fraction)
};
