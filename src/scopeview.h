#pragma once

#include <QColor>
#include <QImage>
#include <QString>
#include <QJsonObject>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include "theme.h"

#include <array>

class AnnotationBox;

/*
 * One plot, all channels overlaid, laid out the way a bench oscilloscope is:
 *
 *  - 10 x 8 divisions, centre axes ticked at 1/5 division
 *  - each channel has its own volts/div and vertical position, with a ground
 *    marker on the left edge in the channel colour
 *  - the horizontal window is locked to the trigger point rather than to the
 *    start of the buffer, so a triggered trace stands still
 *  - trigger level marker on the right edge, trigger instant marked at the top
 */
class ScopeView : public QWidget {
    Q_OBJECT

public:
    static constexpr int MaxChannels    = 4;
    // Rounds a tick value to as few decimals as the step size (and, if
    // given, a DC-offset-style shift) actually needs, clamped so a wide
    // number never overflows a narrow gutter. Used throughout this class's
    // own axis drawing; public and static so XYView can format its own
    // numbers identically rather than duplicating this logic and risking
    // the two drifting apart.
    static QString axisNumber(double value, double step, double offset = 0.0);
    static constexpr int HorizontalDivs = 10;
    static constexpr int VerticalDivs   = 10;

    explicit ScopeView(QWidget *parent = nullptr);

    void setChannelCount(int count);
    void setChannelVisible(int channel, bool visible);
    void setChannelColor(int channel, const QColor &color);
    void setVoltsPerDiv(int channel, double voltsPerDiv);
    void setOffset(int channel, double volts);      // DC offset: added to the signal
    void setWindowCentre(int channel, double volts);  // input offset: hardware window
    void setPositionVolts(int channel, double volts);
    // Getters matching the setters just above — for saving and restoring a
    // channel's own vertical view state (e.g. per Record tab), not part of
    // the original live-display-only design.
    double voltsPerDiv(int channel) const;
    double positionVolts(int channel) const;
    bool channelVisible(int channel) const;

    // Overrides the trigger-centred time axis with an absolute origin: label(0)
    // reads exactly this many seconds. Used for a record, which has no trigger
    // and should count up from 0 at the start of the record, not from a
    // recentred "now".
    void setAbsoluteTimeOrigin(double seconds);
    void clearAbsoluteTimeOrigin();
    // Live: the right edge is the present, everything left of it the past.
    // Time 0 sits on the rightmost division line even before any data has
    // arrived (it fell back to mid-screen, -5..5 divisions), and the
    // time-zero line isn't drawn — as in Record.
    void setNowAtRightEdge(bool on);
    // Samples that are NaN mean "no data here" (Live's whole-window frames
    // reaching back before the history starts): traces skip them. Off,
    // the drawing loops run exactly as before.
    void setTraceGaps(bool on);
    // What the plot shows, for the Remote page's phone view: each visible
    // channel as min/max per column in divisions (x100, the plot's own
    // mapping), the time axis, the readouts and the info panels.
    QJsonObject remoteFrame(int columns) const;
    bool hasAbsoluteTimeOrigin() const { return m_hasAbsoluteTimeOrigin; }
    double absoluteTimeOrigin() const { return m_absoluteTimeOrigin; }

    // A single computed trace: sourceA <op> sourceB, drawn dashed in its own
    // colour with an auto-scaled vertical range, since a product or sum has no
    // natural volts/div the way a real channel's probe and page do. Recomputed
    // automatically whenever setSamples() updates either source channel.
    // Values match MathOps::Op (mathops.h); Add/Subtract/Multiply keep 0/1/2.
    enum class MathOp { Add, Subtract, Multiply, Divide, Average, Maximum, Minimum,
                        Negate, Absolute, Square, SquareRoot, Reciprocal, Decibel,
                        Derivative, Integral, Scale };
    void setMathChannel(bool enabled, MathOp op, int sourceA, int sourceB);
    bool mathEnabled() const { return m_math.enabled; }
    // Math's own computed samples, for Spectrum's benefit — MainWindow has
    // no other way to reach them, since recomputeMath() keeps them
    // entirely inside this class otherwise.
    const QVector<double> &mathSamples() const { return m_math.samples; }
    // The acquisition's own sample rate — for CSV export's time column and
    // for referenceValueAt()'s own resampling math (see its comment).
    double sampleRate() const { return m_sampleRate; }
    // The trigger's own sample-index position within the current buffer
    // (or -1 if untriggered) — saved by the native .vsxwave format so
    // reopening it can hand the exact same value back to setSamples(),
    // reproducing the same trigger-centred time axis.
    double triggerPoint() const { return m_triggerPoint; }
    // This channel's samples with probe gain/offset, inversion, and DC
    // offset all applied — the same transform valueToY() applies before a
    // channel's own volts/div ever comes into it, and the one
    // addReferenceFromChannel() already bakes into a snapshot. CSV export
    // wants the actual measured value, not the raw ADC reading, so this is
    // the shared place that transform lives rather than duplicating it a
    // third time.
    QVector<double> channelDisplaySamples(int channel) const;
    // The channel's own RAW samples, before channelDisplaySamples()'s probe/
    // offset/inversion transform — what setSamples() itself expects and
    // what the native .vsxwave format saves, so reopening one and reapplying
    // the same probe/offset/inversion settings reproduces the identical
    // display rather than double-applying (or never applying) that transform.
    const QVector<double> &channelSamples(int channel) const;
    // Public wrapper for the otherwise-private sampleIndexToTime() — CSV
    // export's time column uses the same trigger-centred convention as
    // everything else in this app (rulers included) rather than a plain
    // "seconds since the start of the buffer".
    double timeAtSample(int index) const { return sampleIndexToTime(index); }
    QColor mathColor() const { return m_math.color; }
    void setMathColor(const QColor &color) { m_math.color = color; update(); }

    // A saved or loaded waveform overlaid on the live display — up to
    // MaxReferences at once. Unlike Math, a reference is resampled against
    // the CURRENT timebase/zoom on every repaint rather than being tied to
    // whatever view was active when it was captured (see
    // referenceValueAt()'s own comment for how), so it tracks along as the
    // live trace is zoomed or panned instead of drifting out of alignment.
    // Capture/Live only — MainWindow enforces that; this class has no
    // opinion about which acquisition mode is active.
    static constexpr int MaxReferences = 2;
    // Snapshots channel's own current samples, sample rate, and trigger
    // alignment into a new reference slot — this is the "Add" button's
    // entire job, self-contained here since viewAnchor() (what makes the
    // snapshot line up with the live trace at delay = 0) is otherwise
    // private. Returns the slot used, or -1 if both are already occupied.
    int addReferenceFromChannel(int channel);
    // The counterpart for loading a previously-saved .vsxref, where every
    // field was already read back from disk rather than captured live.
    // A time in the time axis's own unit and resolution (cursor flags, Rulers panel).
    QString formatAxisTime(double seconds) const;
    // The Rulers panel's: seconds to a fifth of a division from 1 s up
    // ("10.971 s" at 5 ms/div), the axis's unit below.
    QString formatPanelTime(double seconds) const;
    void    setReferenceUnit(int slot, const QString &unit);
    QString referenceUnit(int slot) const;
    int addReferenceFromData(const QVector<double> &samples, double sampleRate,
                              double triggerIndex, const QString &name,
                              const QColor &color, double voltsPerDiv,
                              double positionVolts, double delaySeconds);
    void removeReference(int slot);
    bool referenceOccupied(int slot) const;
    int referenceCount() const;
    QString referenceName(int slot) const;
    void setReferenceName(int slot, const QString &name);
    QColor referenceColor(int slot) const;
    void setReferenceColor(int slot, const QColor &color);
    // <= 0 means auto-fit the visible window, same convention setMathVoltsPerDiv() uses.
    double referenceVoltsPerDiv(int slot) const;
    void setReferenceVoltsPerDiv(int slot, double voltsPerDiv);
    double referencePositionVolts(int slot) const;
    void setReferencePositionVolts(int slot, double positionVolts);
    // Positive shifts the reference trace later (right) relative to the
    // live view; negative shifts it earlier (left).
    double referenceDelay(int slot) const;
    void setReferenceDelay(int slot, double delaySeconds);
    // For MainWindow's own Save-to-.vsxref — everything else about a
    // reference is already reachable through the getters above.
    const QVector<double> &referenceSamples(int slot) const;
    double referenceSampleRate(int slot) const;
    double referenceTriggerIndex(int slot) const;
    // References make sense only in Capture/Live — MainWindow calls this on
    // every mode switch (false entering Record, true leaving it) so a
    // reference already loaded neither draws nor claims gutter space while
    // Record mode is active, without discarding it: it reappears exactly as
    // it was the moment Capture/Live is returned to.
    void setReferencesVisible(bool visible);

    // Persistence (digital colour): while enabled, every setSamples() also
    // accumulates that channel's visible trace into a per-pixel hit count,
    // and the plot draws that heat map (blue = rare ... red = most frequent)
    // instead of the channels' own traces. Math, references, rulers and
    // everything else still draw on top as normal. Hits are stored in data
    // coordinates, so resizing, vertical dragging and panning keep them;
    // a change of resolution (see persistenceSignature()) starts over.
    // Protocol decoding: one labelled bus row per decoder slot, along the
    // bottom of the plot. Annotation positions are in this view's own
    // sample-index space (MainWindow maps decoder output into it), so rows
    // follow zoom and pan exactly like the traces.
    struct BusAnnotation {
        double      start = 0.0;
        double      end = 0.0;
        QStringList texts;       // longest first; the widest that fits is shown
        bool        error = false;
        QString     group;       // same group = same wording (see drawBusRows())
    };
    static constexpr int MaxBusRows = 2;
    void setBusRow(int slot, bool visible, const QString &label, const QColor &colour,
                   const QVector<BusAnnotation> &annotations, const QString &message);
    // Bumped whenever channel data changes (setSamples/clearChannel/
    // clearAll), so a decoder can tell cheaply whether to run again.
    quint64 dataGeneration() const { return m_dataGeneration; }
    // One raw sample as displayed: probe gain/offset, inversion, DC offset
    // and input offset applied — the per-value form of channelDisplaySamples().
    double displayValue(int channel, double raw) const;
    // Record mode draws a decimated page (min/max pairs, a few thousand
    // points for the whole view), which can't be measured: its "frequency"
    // is how often the page alternates, not the signal's. MainWindow hands
    // over raw samples for the view here instead, and measurements use
    // them. Cleared outside Record mode and by clearChannel().
    void setMeasurementSource(int channel, const QVector<double> &raw, double sampleRate);

    // Low-pass filter: a zero-phase Butterworth (2nd order, run forward then
    // backward), so edges don't shift in time — trigger, cursors and
    // channel-to-channel timing stay right. cutoffHz is the -3 dB point.
    // Returns the input unchanged when the cutoff is at or above ~0.45 of
    // the sample rate, where there is nothing left to filter.
    static QVector<double> lowPassFilter(const QVector<double> &in, double sampleRate,
                                         double cutoffHz);
    // Per channel and for Math. A channel keeps its raw samples, so turning
    // the filter off or moving the cutoff re-filters from the original data.
    // What's filtered is what's shown: trace, measurements, decoding,
    // persistence, references, CSV. channelSamples() (used to save .vsxwave)
    // stays raw.
    // Per channel: off, low-pass (cutoff = highHz), high-pass (cutoff =
    // lowHz) or band-pass (lowHz..highHz). All zero-phase, -3 dB at each
    // cutoff / edge.
    // FilterHighRes: the mean of N neighbouring samples (N passed as lowHz),
    // centred so edges keep their timing — +0.5 bit per doubling of N where
    // the signal's noise spans a few ADC codes, for 1/N of the bandwidth.
    enum FilterType { FilterOff = 0, FilterLowPass, FilterHighPass, FilterBandPass, FilterBandStop, FilterHighRes };
    void setChannelFilter(int channel, int type, double lowHz, double highHz);
    // The zero-phase high-pass is exactly the input minus a matched low-pass
    // (1 - 1/(1+(f/f0)^4) is the forward-backward Butterworth high-pass);
    // the high-pass's own cutoff sits at -3 dB. Verified: -3.01 dB at the
    // cutoff, -17.7 dB an octave below, DC removed, edges unmoved.
    static QVector<double> highPassFilter(const QVector<double> &in, double sampleRate, double cutoffHz);
    // The slow part a high-pass at cutoffHz removes (input minus this is the
    // high-pass output) — what Record's zoomed-out views subtract.
    static QVector<double> highPassBaseline(const QVector<double> &in, double sampleRate, double cutoffHz);
    // Band-stop: a 2nd-order notch (analog prototype, bilinear with the
    // edges pre-warped), forward + backward, Q set so the squared response
    // is -3 dB at both edges; the null sits at sqrt(low x high). Verified:
    // 1-10 kHz -3.01 dB at both edges, -0.03 dB a decade out; 45-55 Hz
    // -44 dB at 50 Hz, 100 / 150 Hz untouched; DC passes; edges unmoved.
    static QVector<double> bandStopFilter(const QVector<double> &in, double sampleRate,
                                          double lowHz, double highHz);
    static QVector<double> applyFilter(const QVector<double> &in, double sampleRate, int type,
                                       double lowHz, double highHz);
    // What a high-pass or band-stop takes out of the input (input minus this
    // is the output) — what Record's zoomed-out views subtract per sample.
    static QVector<double> removedPart(const QVector<double> &in, double sampleRate, int type,
                                       double lowHz, double highHz);
    // Math's own filter on its result: the channels' filter types and
    // semantics (FilterType, lowHz / highHz).
    void setMathFilter(int type, double lowHz, double highHz);
    void setMathScaleParams(double k, double c);   // for MathOp::Scale (k.A + c)
    QString mathUnit() const;                      // from the sources' probe units
    QString mathLabel() const;                     // e.g. "CH1 \u00d7 CH2", "d(CH1)/dt"
    // Off in Record mode: its samples there are a decimated page, which
    // can't be filtered meaningfully — MainWindow filters the raw recording
    // itself and hands over the result.
    void setLowPassInView(bool apply);
    // Software AC coupling, for a channel whose hardware can't AC-couple:
    // what's drawn and measured has its mean taken off. Raw samples (and so
    // saved files) keep the DC.
    void setChannelRemoveDc(int channel, bool remove);
    void clearMeasurementSources();
    // Any readout measurement switched on for the channel.
    bool anyMeasurementActive(int channel) const;

    // Engine tools (Rulers > Engine). The view draws; MainWindow analyses.
    // A cycle is the span between two consecutive cycleStarts (view sample
    // indices) covering cycleDegrees (720 for a 4-stroke); offsetDegrees is
    // the crank angle at each cycle start (0 = cylinder 1 TDC). Bands are
    // one per cylinder event, centred on its TDC (or its compression peak).
    struct CylinderBand {
        double start = 0.0;
        double end = 0.0;
        double tdc = 0.0;
        int    cylinder = 0;
        bool   excluded = false;   // in a cycle the compression test left out
    };
    // manualMarkers: show the two draggable cycle markers (no sync signal).
    void setEngineOverlay(bool enabled, bool manualMarkers);
    void setEngineCycles(const QVector<double> &cycleStarts, double cycleDegrees,
                         double offsetDegrees);
    void setCylinderBands(const QVector<CylinderBand> &bands);
    // Bar chart: percent[i] for cylinder i + 1; empty hides the bars (the
    // note still shows, e.g. why there is no result).
    void setCompressionResult(const QVector<double> &percent, double flagBelowPercent,
                              const QString &note);
    // The manual markers as view sample indices (0° and the cycle's end).
    double engineMarkerA() const;
    double engineMarkerB() const;
    void resetEngineMarkers();   // one cycle's guess: 20% and 80% of the view
    // Stroke timing chart (as on automotive scopes): one row per cylinder in
    // firing order, each cycle split into intake / compression / power /
    // exhaust (two phases for a 2-stroke), rows offset by the firing
    // interval — the staircase. Off: plain alternating cylinder bands.
    void setEngineTimingChart(bool chart, const QVector<int> &firingOrder);
    // Crank/cam results: info panel lines (warn = red) and tooth-fault
    // positions (view sample indices), marked on the plot. Empty hides them.
    void setCrankInfo(const QStringList &lines, const QVector<bool> &warn,
                      const QVector<double> &faults);
    // Guided-test results (battery cranking, charging): a titled panel shown
    // whatever the engine overlay's state. An empty title hides it.
    void setTestPanel(const QString &title, const QStringList &lines, const QVector<bool> &warn);
    // Titled info panels, stacked top right (top left with the readouts
    // there) in slot order: 0 guided tests, 1 CAN bus health. Lines with
    // tabs line up as columns; a panel taller than the plot is cut off with
    // a "... n more" line. An empty title removes the panel.
    void setInfoPanel(int slot, const QString &title, const QStringList &lines,
                      const QVector<bool> &warn);

    // The measurement readouts: one block, dragged anywhere and snapped to
    // the nearest plot corner on release. 0 top-left, 1 top-right,
    // 2 bottom-left, 3 bottom-right.
    int readoutCorner() const { return m_readoutCorner; }
    void setReadoutCorner(int corner);

    void setPersistenceEnabled(bool enabled);
    bool persistenceEnabled() const { return m_persistEnabled; }
    void clearPersistence();

    // Which of a channel's measurements the Measure panel has turned on.
    // Independent per channel — same "any combination, any channel, all at
    // once" model Rulers already use for their own per-channel cursors —
    // and each channel keeps its set even while off, so it reappears
    // untouched if the channel is turned back on rather than needing to be
    // reconfigured. RulersMathChannel (below) is a valid channel here too.
    enum class Measurement {
        Vpp, Vrms, Mean, Max, Min, Frequency,
        AcRms, Period, DutyCycle, PositiveWidth, NegativeWidth
    };
    static constexpr int MeasurementCount = 11;
    void setChannelMeasurement(int channel, Measurement type, bool enabled);
    bool channelMeasurementEnabled(int channel, Measurement type) const;

    // <= 0 means auto-fit the visible window (the original behaviour);
    // positive is a fixed per-division value, exactly like a real channel's
    // vertical scale, just without a probe or hardware window to speak of.
    void setMathVoltsPerDiv(double perDiv);
    void setMathPositionVolts(double volts);
    double mathPositionVolts() const { return m_math.positionVolts; }
    bool mathIsManual() const { return m_math.voltsPerDiv > 0.0; }
    double mathVoltsPerDiv() const { return m_math.voltsPerDiv; }

    // Two time (vertical) and two voltage (horizontal) cursors, each
    // draggable independently, plus a draggable readout box showing their
    // values and the difference between them. Voltage cursors read against
    // whichever channel is enabled first when rulers are turned on.
    void setRulersEnabled(bool enabled);
    bool rulersEnabled() const { return m_rulers.enabled; }
    void setRulersTimeEnabled(bool enabled);
    bool rulersTimeEnabled() const { return m_rulers.timeEnabled; }
    // Track mode: each enabled channel's voltage rulers follow its trace at
    // the two time rulers (they can't be dragged); the box adds the slope
    // between them. Needs the time rulers.
    void setRulersTrack(bool on);
    bool rulersTrack() const { return m_rulers.track; }
    // The channels' samples are min / max pairs (a zoomed-out Record page, a
    // long Live window), not samples: a tracked value is then the middle of
    // the column's envelope rather than one entry of a pair.
    void setSamplesArePairs(bool pairs);

    // -1 turns the voltage cursors off; 0..MaxChannels-1 selects a real
    // channel; MaxChannels selects the math channel.
    // Each channel's voltage cursor pair is independent and can be on at the
    // same time as any other's — 0..MaxChannels-1 selects a real channel;
    // MaxChannels selects the math channel.
    void setRulersChannelEnabled(int channel, bool enabled);
    bool rulersChannelEnabled(int channel) const;
    static constexpr int RulersMathChannel = MaxChannels;

    void setRulersBoxMinimized(bool minimized);
    bool rulersBoxMinimized() const { return m_rulers.boxMinimized; }
    // Shows/hides the back/forward arrows drawn on the plot itself —
    // MainWindow calls this after every change to its own zoom history
    // (a new zoom, a back/forward navigation, or an acquisition mode
    // switch to a mode with its own, different history).
    void setZoomHistoryAvailable(bool canGoBack, bool canGoForward);
    void setProbe(int channel, double gradient, double offset, const QString &unit);
    void setDisplayScale(int channel, double scale);   // vertical zoom, display only
    void setInverted(int channel, bool inverted);

    // Adds a floating, draggable text box with an optional arrow: it starts
    // inactive, hanging straight down from the box, and only begins
    // tracking a specific sample and channel (or Math) — the same way a
    // Ruler voltage cursor tracks its own channel, so panning or zooming
    // afterward slides the tip along the trace — once the user actually
    // drags it onto the trace. The channel it would track defaults to the
    // first currently visible one (Math if that is the only thing showing,
    // channel 0 as a last resort). Session-only, like Rulers' own state —
    // not persisted to QSettings.
    void addAnnotation(bool focus = true);   // focus: ready to type (not when restoring)
    void clearAnnotations();
    // Annotations across sessions: text, box geometry (position as a fraction
    // of this view, so a different window size still places them sensibly)
    // and what the arrow points at.
    QVariantList annotationsState() const;
    void restoreAnnotations(const QVariantList &list);

    void setSamples(int channel, const QVector<double> &samples,
                    double sampleRate, double triggerPoint);
    void setChannelOverRange(int channel, bool over);
    void clearChannel(int channel);
    void clearAll();

    void setSecondsPerDiv(double seconds);
    void setSampleRate(double rate);   // so the strip is current before data arrives

    // The status strip normally just prints the rate passed to setSamples. When
    // that rate has been adjusted for internal windowing math on a decimated
    // buffer (see mainwindow's record view), it no longer means "samples per
    // second" to a person reading it, so the true rate is shown instead here.
    // -1 clears the override.
    void setDisplayRateOverride(double rate);
    void setTrigger(int sourceChannel, double levelVolts, int slope, bool armed);
    void setRunning(bool running);
    void setZoomMode(bool enabled);   // selection only responds when armed

    double secondsPerDiv() const { return m_secondsPerDiv; }

    // Midpoint of a channel's last capture, in displayed volts. Returns false
    // when there is nothing captured to measure.
    bool channelMidpoint(int channel, double *volts) const;
    // The same "+-X V" half-range shown on that channel's own card
    // (voltsPerDiv / display-zoom scale, times half the vertical
    // divisions) — 0.0 for an out-of-range channel. Used by XY mode so its
    // axes track each channel's own configured range exactly, the same
    // fixed-scale convention the main plot and Spectrum both already use,
    // rather than auto-fitting to whatever the data happens to peak at.
    double channelFullScaleVolts(int channel) const;
    bool channelSpan(int channel, double *lo, double *hi) const;

    // Panning is stored as an offset from the trigger, not as a position in the
    // buffer, so a dragged view is just as steady as a centred one.
    void setViewCentreFraction(double fraction);

    // Accumulates directly in samples rather than through a fraction, so nothing
    // is lost to rounding on repeated calls — see the .cpp for why that mattered.
    void panBySamples(double deltaSamples);
    void followTrigger();
    bool isFollowingTrigger() const { return m_viewFollowing; }
    double viewportWidthFraction() const;
    double viewportCentreFraction() const;
    double triggerFraction() const;
    int captureLength() const { return m_lastLength; }

signals:
    void clicked();
    void offsetDragged(int channel, double deltaVolts);
    void mathOffsetDragged(double deltaVolts);
    void referenceOffsetDragged(int slot, double deltaVolts);
    void engineMarkersMoved();
    void readoutCornerChanged(int corner);
    void rulersClosed();   // the box's own close button, so mainwindow can un-check its rail button
    void panRequested(double windowFraction);   // horizontal drag on the plot body

    // A dragged rectangle, in fractions of the plot: x from the left edge,
    // y from the top. The receiver decides what zooming means for the current
    // acquisition mode.
    void zoomRequested(const QRectF &fractions);
    void zoomResetRequested();
    // The two arrows drawn on the plot itself once there is somewhere to
    // go — see setZoomHistoryAvailable(). What "back"/"forward" actually
    // restore is entirely MainWindow's own concern (see its ZoomState);
    // this widget only knows whether to draw each arrow and that it was
    // clicked.
    void zoomBackRequested();
    void zoomForwardRequested();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    struct MathRange { double mid; double scale; double lo; double hi; };
    MathRange mathRange(const QRectF &plot) const;

    // Math and each reference are "extra" gutters beyond CH1's and CH2's own
    // fixed-side columns — this decides, for the CURRENT state (channels
    // visible, Math on, which reference slots occupied), which side each
    // extra lands on and at what position within that side. Recomputed
    // fresh from current state every call rather than tracked historically:
    // simpler, and means removing then re-adding anything reflows cleanly
    // with no risk of stale placement. leftGutter()/rightGutter() (for
    // width), drawMathScale()/drawReferenceScale() (for where to actually
    // draw), and the gutter mouse-drag hit-test all call this so they can
    // never disagree with each other about where anything is.
    struct GutterAllocation {
        bool mathOnLeft = false;
        int  mathColumn = -1;   // -1: Math not enabled, nothing to place
        std::array<bool, MaxReferences> referenceOnLeft{};
        std::array<int, MaxReferences>  referenceColumn{};   // -1: slot not occupied
        int  leftExtraCount = 0;    // total extra columns beyond CH1's own
        int  rightExtraCount = 0;   // total extra columns beyond CH2's own
    };
    GutterAllocation computeGutterAllocation() const;
    // Where a given extra column (Math's, or one reference's) actually
    // starts, given computeGutterAllocation()'s placement of it — shared by
    // drawMathScale() and drawReferenceScale() so neither can drift out of
    // sync with the other's idea of where things are.
    // The first extra on a side whose own channel (CH1 for left, CH2 for
    // right) is currently OFF takes over that channel's own full-width slot
    // (kScaleGutter) rather than being confined to the narrower "extra"
    // width while that space goes unused — this is what makes Math or a
    // reference actually move into the space a disabled channel leaves
    // behind, rather than squeezing beside a narrow leftover gutter for a
    // channel that isn't even drawing anything there. Any other extra
    // (a later one on the same side, or the first one when that side's
    // channel IS visible) gets the ordinary kMathColumnWidth.
    double extraColumnWidth(bool onLeft, int column) const;
    double extraColumnX(bool onLeft, int column, const QRectF &plot) const;

    void drawMathTrace(QPainter &p, const QRectF &plot) const;
    void drawReferenceTrace(QPainter &p, const QRectF &plot, int slot) const;
    // The reference's own value at whatever live-view time the given
    // screen x maps to, or NaN if that time falls outside the reference's
    // own captured span — resampling against the CURRENT timebase/zoom is
    // what tracks a reference along as the live trace is zoomed or
    // panned, rather than freezing it to whatever view was active when it
    // was captured (see the ReferenceState comment above for the
    // trigger-alignment half of this).
    double referenceValueAt(int slot, const QRectF &plot, double x) const;
    void drawMathScale(QPainter &p, const QRectF &plot) const;
    void drawReferenceScale(QPainter &p, const QRectF &plot, int slot) const;
    void drawRulers(QPainter &p, const QRectF &plot) const;
    double sampleIndexToX(const QRectF &plot, double sampleIndex) const;
    double sampleIndexAtX(const QRectF &plot, double x) const;
    double sampleIndexToTime(double sampleIndex) const;
    double mathValueToY(const QRectF &plot, double value) const;
    double mathYToValue(const QRectF &plot, double y) const;
    QRectF rulerBoxRect(const QRectF &plot) const;
    QRectF rulerCloseRect(const QRectF &plot) const;
    QRectF rulerMinimizeRect(const QRectF &plot) const;
    QRectF zoomBackRect(const QRectF &plot) const;
    QRectF zoomForwardRect(const QRectF &plot) const;

    // Computed once, used by BOTH drawing and hit-testing, so a click is
    // always evaluated against exactly the geometry that was actually drawn —
    // including each flag's full rectangle, not just a thin line through its
    // centre, which is what made the flags hard to grab by anywhere but a
    // handful of pixels.
    struct RulerChannelLayout {
        bool    on = false;
        double  v1y = 0.0, v2y = 0.0;
        QRectF  v1Flag, v2Flag;
        QString v1Text, v2Text;
        QColor  colour;
        double  v1Probed = 0.0, v2Probed = 0.0;   // for the box's own delta
        bool    v1Valid = true, v2Valid = true;    // track mode: the trace has a value there
        double  unitFactor = 1.0;                  // the scale's own unit (see ScaleUnit)
        QString unit;
    };
    // The unit a channel's scale shows its numbers in — mV while every label
    // stays under 1 V, else V (or the probe's unit); Math in its own unit —
    // so the cursor flags read exactly like the scale beside them.
    struct ScaleUnit {
        double  factor = 1.0;    // value x factor = the number shown
        QString unit;
        QString text(double value) const;
    };
    ScaleUnit channelScaleUnit(int channel) const;
    ScaleUnit mathScaleUnit() const;
    struct RulerLayout {
        double  flagHeight = 0.0, timeFlagWidth = 0.0;   // for drags: lines stop where flags do
        bool    timeOn = false;
        double  t1x = 0.0, t2x = 0.0;
        QRectF  t1Flag, t2Flag;
        QString t1Text, t2Text;              // flags: the axis's unit
        QString t1PanelText, t2PanelText;    // Rulers panel: seconds (10.971 s)
        // True when this cursor's actual position is currently outside the
        // visible plot — its flag is still drawn, pinned to the nearest
        // edge, but styled differently (see drawRulers()) and with no text,
        // since the position that flag sits at no longer corresponds to
        // where the cursor actually is.
        bool    t1OffScreen = false, t2OffScreen = false;

        // Index MaxChannels is the math channel — every real channel can have
        // its own cursor pair active at the same time, same as the reference
        // app's own "channel rulers" (one row per channel, all independent).
        std::array<RulerChannelLayout, MaxChannels + 1> channels;

        QRectF box;
        QRectF closeRect;
        QRectF minimizeRect;
    };
    RulerLayout computeRulerLayout(const QRectF &plot) const;
    bool rulersTracking() const { return m_rulers.track && m_rulers.timeEnabled; }
    // The trace's value (raw, as the channel's samples hold it; Math in its
    // own unit) at a fractional sample index. False outside the data or in a
    // gap.
    bool traceValueAt(int channel, double index, double *raw) const;
    bool m_samplesArePairs = false;
    int hitTestRulers(const QPointF &pos, const QRectF &plot) const;
    void recomputeMath(int changedChannel);

    // One entry per annotation added via addAnnotation(). The box itself
    // (position, size, text, its own trash button) is entirely the child
    // widget's own business; this only tracks what the arrow's tip points
    // at, in the same waveform-relative terms a Ruler voltage cursor uses,
    // so it tracks the trace correctly across pans and zooms.
    struct AnnotationState {
        AnnotationBox *box = nullptr;
        bool   active      = false;   // false until dragged onto the trace — see annotationTipPos()
        int    channel     = 0;       // 0..MaxChannels-1, or RulersMathChannel
        double timeOffset  = 0.0;     // sample offset from viewAnchor(), like ruler cursors
        double volts       = 0.0;     // raw, pre-probe volts in that channel's own space
    };
    QVector<AnnotationState> m_annotations;
    int m_annotationDrag = -1;        // index whose arrow tip is being dragged, or -1

    // At most one annotation can hold keyboard focus at a time (that is what
    // "selected" means here), so a single index — not a per-annotation flag
    // — is enough. Only the selected annotation shows its external chrome
    // (trash, move handle, resize handles) and its default, not-yet-aimed
    // arrow if it has not been activated yet.
    int m_selectedAnnotation = -1;

    // The move handle and the 8 resize handles are dragged the same way
    // panning/Ruler-dragging are: by index while pressed, cleared on
    // release. Only one of these, m_annotationDrag above, or the ordinary
    // pan/zoom/channel-drag state can be active at a time — mousePressEvent
    // picks at most one per click.
    int      m_annotationMoveDrag = -1;
    QPointF  m_annotationMoveLast;
    int      m_annotationResizeDrag   = -1;
    int      m_annotationResizeHandle = -1;   // 0..7, see annotationHandlePoints()
    QRect    m_annotationResizeStartGeom;
    QPointF  m_annotationResizeStartMouse;

    void drawAnnotations(QPainter &p, const QRectF &plot) const;
    int hitTestAnnotations(const QPointF &pos, const QRectF &plot) const;
    QPointF annotationTipPos(const AnnotationState &a, const QRectF &plot) const;
    void removeAnnotation(int index);

    // External chrome layout, shared between drawing and hit-testing so the
    // two can never drift apart — same principle as the Ruler layout struct
    // above. All positioned clear of the box's own rectangle: a resize
    // handle centred exactly on the box's border would be half-covered by
    // the box widget itself, which always paints on top of this (its
    // parent's) output.
    QRectF annotationTrashRect(const AnnotationState &a) const;
    QRectF annotationMoveRect(const AnnotationState &a) const;
    std::array<QPointF, 8> annotationHandlePoints(const AnnotationState &a) const;
    int hitTestAnnotationHandles(const AnnotationState &a, const QPointF &pos) const;
    Qt::CursorShape annotationHandleCursor(int handle) const;

    struct Channel {
        QVector<double> samples;       // what is drawn/measured: filtered if a filter is on
        QVector<double> rawSamples;    // as delivered by setSamples(), unfiltered
        int    filter       = 0;       // FilterType
        double filterLowHz  = 10.0;    // high-pass cutoff / band-pass lower edge
        double filterHighHz = 100e3;   // low-pass cutoff / band-pass upper edge
        bool   removeDc  = false;   // software AC coupling (see setChannelRemoveDc())
        // Raw samples to measure instead of `samples` (Record mode, where
        // `samples` is a decimated page; see setMeasurementSource()).
        QVector<double> measureSamples;
        double measureRate = 0.0;
        QColor color       = Qt::white;
        double voltsPerDiv = 0.5;
        double offset       = 0.0;  // DC offset, volts added to the signal
        double windowCentre = 0.0;  // input offset, centre of the hardware window
        double positionVolts = 0.0;   // display shift, volts — independent of zoom,
                                       // so panning range does not shrink when the
                                       // channel is magnified
        double probeGain   = 1.0;
        double probeOffset = 0.0;
        double scale       = 1.0;   // display zoom
        QString unit       = QStringLiteral("V");
        bool   inverted    = false;
        bool   visible     = false;
        bool   overRange   = false;
        // Indexed by Measurement — which of this channel's measurements the
        // Measure panel has turned on. Kept even while the channel itself is
        // off (see setChannelMeasurement()'s own comment).
        std::array<bool, MeasurementCount> activeMeasurements{};
    };

public:
    struct Measurements {
        double vpp = 0.0, vrms = 0.0, mean = 0.0, max = 0.0, min = 0.0, frequency = 0.0;
        // acRms is valid whenever the others are (needs nothing beyond what
        // they already use); the rest default to 0.0 meaning "not
        // measurable" (too few edges, or a DC signal) — same convention
        // frequency already used before these existed.
        double acRms = 0.0, period = 0.0, dutyCycle = 0.0;
        double positiveWidth = 0.0, negativeWidth = 0.0;
        bool   valid = false;
    };
    // The measurement maths on any samples (thread-safe: no state) — used by
    // Live's worker thread to measure full-rate data off the interface thread.
    // gain / bias as measure() derives them from the probe, inversion, offset.
    static Measurements measureSamples(const QVector<double> &samples, double gain, double bias,
                                       double sampleRate)
    { return measureUncached(samples, gain, bias, sampleRate); }
    // Measurements computed elsewhere (Live's worker, on the full-rate data
    // while the plot shows reduced pairs): measure() returns these instead.
    void setMeasurementOverride(int channel, const Measurements &m);
    void clearMeasurementOverrides();
    // What measure() multiplies / adds for the channel (probe, inversion, offset).
    double measureGain(int channel) const;
    double measureBias(int channel) const;

private:
    std::array<bool, MaxChannels> m_hasMeasureOverride{};
    std::array<Measurements, MaxChannels> m_measureOverride{};

    QRectF plotRect() const;
    double leftGutter() const;
    double rightGutter() const;
    double pixelsPerDiv(const QRectF &plot) const;
    double valueToY(const QRectF &plot, const Channel &c, double volts) const;
    bool columnRange(const QVector<double> &s, int a, int b, double *lo, double *hi) const;
    double yToValue(const QRectF &plot, const Channel &c, double y) const;
    int    windowLength() const;
    int    windowStart(int sampleCount) const;
    double viewAnchor(int sampleCount) const;
    Measurements measure(const Channel &c) const;
    // Math has no probe gain, DC offset, or inversion of its own — its
    // samples are already final display-scale values — so this is measure()
    // with gain=1, bias=0, rather than needing a second parallel
    // implementation of the actual signal analysis.
    Measurements measureMath() const;
    Measurements measureCore(const QVector<double> &samples, double gain, double bias,
                             double sampleRate) const;
    // measureCore()'s cache, one entry per source vector (a channel's
    // samples, its Record measurement source, Math). The readouts are
    // measured from paintEvent() — several times per paint — over the whole
    // buffer: millions of samples in Live, 87-130 ms per refresh measured,
    // the main Live stall. Same inputs: the stored result. New data within
    // 200 ms of the last measurement: the stored result, and a repaint when
    // the 200 ms are up (readouts at 5/s while data streams in).
    static Measurements measureUncached(const QVector<double> &samples, double gain, double bias,
                                        double sampleRate);
    struct MeasureCacheEntry {
        bool    valid = false;
        quint64 generation = 0;
        const double *data = nullptr;
        int     size = 0;
        double  gain = 0.0, bias = 0.0, rate = 0.0;
        qint64  atMs = 0;           // when the last measurement finished
        qint64  costMs = 0;         // how long it took
        Measurements result;
    };
    mutable QHash<const void *, MeasureCacheEntry> m_measureCache;
    mutable bool m_measureRefreshPending = false;
    // The "Vpp 1.2 V   Freq 1.0 kHz" suffix drawReadouts() appends after a
    // channel's or Math's own line — shared since both need exactly the same
    // "only what is checked, in this fixed order" formatting.
    QString formatMeasurements(const Measurements &m,
                               const std::array<bool, MeasurementCount> &active,
                               const char *unit) const;

    void drawGraticule(QPainter &p, const QRectF &plot) const;
    // How many divisions the visible window's time reference (the trigger
    // instant, or a record's own absolute time origin) sits from the left
    // edge of the plot — shared by the graticule (to slide the whole time
    // grid with a pan) and the axis labels (to know which time value each
    // slid line corresponds to), so the two always agree with each other
    // about exactly where every gridline actually is.
    double timeAxisOffsetDivs() const;
    void drawTrace(QPainter &p, const QRectF &plot, const Channel &c) const;
    void drawBusRows(QPainter &p, const QRectF &plot) const;
    void drawEngineBands(QPainter &p, const QRectF &plot) const;     // under the traces
    void drawTimingChart(QPainter &p, const QRectF &plot) const;     // under the traces
    void drawTestPanel(QPainter &p, const QRectF &plot) const;
    struct ReadoutLine {
        QString text;
        QColor  colour;
        bool    overRange = false;
    };
    QVector<ReadoutLine> readoutLines() const;
    QFont readoutFont() const;
    QRectF readoutBlockRect(const QRectF &plot, const QVector<ReadoutLine> &lines) const;
    double topReservedHeight() const;                       // engine label rows
    double bottomReservedHeight() const;                    // bus rows + chart legend
    double timingLegendHeight() const;
    void drawEngineOverlay(QPainter &p, const QRectF &plot) const;   // over them
    QRectF engineMarkerFlag(const QRectF &plot, int which) const;    // 1 = A, 2 = B
    double timeToSampleIndex(double seconds) const;                  // inverse of sampleIndexToTime()
    // Crank angle and cylinder at a view sample index; false outside every
    // cycle. unwrapped keeps counting across cycles (for differences).
    bool engineAngleAt(double sampleIndex, double *degrees, double *unwrapped, int *cylinder) const;
    void accumulatePersistence(int channel);
    QVector<double> persistenceSignature() const;
    bool persistenceUsable(const QRectF &plot) const;
    void drawPersistence(QPainter &p, const QRectF &plot) const;
    void drawGroundMarkers(QPainter &p, const QRectF &plot) const;
    void drawTriggerMarkers(QPainter &p, const QRectF &plot) const;
    void drawReadouts(QPainter &p, const QRectF &plot) const;
    void drawStatusStrip(QPainter &p, const QRectF &plot) const;
    void drawZoomHistoryButtons(QPainter &p, const QRectF &plot) const;
    void drawAxes(QPainter &p, const QRectF &plot) const;
    void drawVoltScale(QPainter &p, const QRectF &plot, int channel, bool leftSide) const;

    std::array<Channel, MaxChannels> m_channels;
    int    m_channelCount  = 0;
    int    m_lastLength    = 0;
    double m_sampleRate    = 0.0;
    double m_displayRateOverride = -1.0;

    struct MathState {
        bool    enabled = false;
        int     filter = 0;             // FilterType, as the channels
        double  filterLowHz = 10.0;
        double  filterHighHz = 100e3;
        double  scaleK = 1.0;           // MathOp::Scale
        double  scaleC = 0.0;
        MathOp  op       = MathOp::Add;
        int     sourceA  = 0;
        int     sourceB  = 1;
        QColor  color    = Theme::mathColor();   // distinct from any channel default
        double  voltsPerDiv   = -1.0;   // <= 0 : auto-fit
        double  positionVolts = 0.0;    // manual mode only
        QVector<double> samples;
        std::array<bool, MeasurementCount> activeMeasurements{};
    } m_math;

    struct ReferenceState {
        bool    occupied = false;
        QString name;
        QColor  color;
        QVector<double> samples;
        double  sampleRate = 0.0;
        // The live channel's own viewAnchor() at the moment this was
        // captured — what a delay of exactly 0 aligns against, so a
        // freshly-added reference starts out drawn exactly where the
        // source channel was at that instant, not at some arbitrary offset.
        double  triggerIndex = 0.0;
        double  voltsPerDiv   = -1.0;   // <= 0 : auto-fit
        double  positionVolts = 0.0;
        double  delaySeconds  = 0.0;
        QString unit = QStringLiteral("V");   // its source channel's (probe's) unit
    };
    std::array<ReferenceState, MaxReferences> m_references;
    bool m_referencesVisible = true;

    struct BusRow {
        bool    visible = false;
        QString label;
        QColor  colour;
        QVector<BusAnnotation> annotations;
        QString message;
    };
    std::array<BusRow, MaxBusRows> m_busRows;
    quint64 m_dataGeneration = 0;

    struct EngineState {
        bool    enabled = false;
        bool    manual = true;
        bool    markersPlaced = false;
        // Stored as times, not indices: in Record the buffer is a page that
        // is rebuilt on every pan, and an index would stay put on screen
        // while the recording slides underneath. Trigger-relative time in
        // Capture, time within the recording in Record.
        double  aTime = 0.0;
        double  bTime = 0.0;
        QVector<double> cycleStarts;
        double  cycleDeg = 720.0;
        double  offsetDeg = 0.0;
        QVector<CylinderBand> bands;
        QVector<double> compression;
        double  flagBelow = 15.0;
        QString note;
        bool    timingChart = true;
        QVector<int> order;   // firing order, for the timing chart rows
        QStringList    crankLines;
        QVector<bool>  crankWarn;
        QVector<double> crankFaults;
    } m_engine;
    struct InfoPanel {
        QString       title;
        QStringList   lines;
        QVector<bool> warn;
    };
    QMap<int, InfoPanel> m_infoPanels;
    mutable double m_enginePanelsBottom = -1.0;   // where the test panel stacks below
    int     m_readoutCorner = 0;
    bool    m_readoutDragging = false;
    QPointF m_readoutDragTopLeft;
    QPointF m_readoutGrab;
    int m_engineDrag = 0;   // 1 = marker A, 2 = marker B
    bool    m_lowPassInView = true;
    bool    m_nowAtRight = false;
    bool    m_traceGaps = false;
    void    refilterChannel(int channel);

    bool              m_persistEnabled = false;
    // One hit grid per channel, in DATA coordinates rather than screen
    // pixels: x is samples relative to viewAnchor() (the trigger instant),
    // y is volts before the channel's own position offset. Resizing the
    // plot (opening a side panel), dragging a channel up/down and panning
    // only change how the grid maps onto the screen, so the accumulation
    // survives them; only a change of resolution (see
    // persistenceSignature()) starts it over. Each grid's bin size is taken
    // from the view at the moment it starts, so one bin is about one pixel.
    struct PersistGrid {
        QVector<quint32> hits;      // rows x cols, row 0 = lowest volts
        int     cols = 0;           // 0: not started yet
        int     rows = 0;
        double  xMin = 0.0;         // anchor-relative sample of column 0
        double  bx   = 1.0;         // samples per column
        double  yMin = 0.0;         // volts at the bottom of row 0
        double  by   = 1.0;         // volts per row
        quint32 max  = 0;
    };
    std::array<PersistGrid, MaxChannels> m_persist;
    QVector<double>   m_persistSignature;
    mutable QImage    m_persistImage;       // rebuilt from the grids when dirty
    mutable bool      m_persistDirty = true;
    // How the grids were last mapped onto the screen; a different mapping
    // (resize, drag, pan) redraws the image from the same grids.
    mutable QVector<double> m_persistViewKey;

    struct RulerState {
        bool   enabled     = false;
        bool   timeEnabled = true;
        // Relative to the SAME anchor viewAnchor() uses (trigger instant, or
        // buffer centre free-running) — never an absolute sample index. The
        // anchor itself moves a little from one capture to the next (real
        // trigger jitter), and the window re-centres on it each time so the
        // TRACE stays visually still; a cursor stored as a fixed absolute
        // index would not get that same correction and would drift relative
        // to the now-correctly-repositioned trace by exactly the jitter each
        // frame — which is what "the cursor keeps wobbling" was.
        double t1Offset = -400.0, t2Offset = 400.0;

        // Every channel can have its own pair active at once, same as the
        // reference app's per-channel rulers — not a single shared selection.
        struct ChannelCursor {
            bool   enabled = false;
            double v1 = 0.0, v2 = 0.0;   // raw volts — this channel's own
                                        // pre-probe space, matching
                                        // valueToY()/yToValue()/mathValueToY()
        };
        std::array<ChannelCursor, MaxChannels> channels;   // CH1..CH4
        ChannelCursor math;                                 // the math channel

        bool    track        = false;   // voltage rulers follow the trace (see setRulersTrack())
        bool    initialised  = false;   // defaults picked once, on first enable
        QPointF boxPos{-1.0, -1.0};      // plot-relative; negative x means "not yet placed"
        bool    boxMinimized = false;
    } m_rulers;

    // Set by MainWindow via setZoomHistoryAvailable() — each arrow draws
    // (and can be clicked) only while its own flag is true.
    bool m_zoomHistoryCanBack = false;
    bool m_zoomHistoryCanForward = false;

    int     m_rulerDrag = -1;           // kNoRulerDrag or a kRuler* sentinel
    QPointF m_rulerDragLast;            // for the box's relative drag only
    double m_triggerPoint  = -1.0;
    double m_secondsPerDiv = 1e-4;
    double m_viewOffsetSamples = 0.0;   // from the trigger, not from the buffer
    bool   m_viewFollowing     = true;
    int    m_dragChannel  = -1;
    double m_dragLastY    = 0.0;
    bool    m_zoomMode    = false;
    bool    m_selecting   = false;
    QPointF m_selectStart;
    QPointF m_selectEnd;
    bool    m_panning     = false;
    bool    m_panMoved    = false;
    double  m_panLastX    = 0.0;
    double  m_panPressX   = 0.0;
    // An explicit flag, not the sign of m_absoluteTimeOrigin itself: a
    // negative origin is a legitimate value (Live mode panned back before
    // "now" is exactly that), and the previous "<0 means not set"
    // convention silently discarded it, treating a real value as if it had
    // never been set at all.
    bool    m_hasAbsoluteTimeOrigin = false;
    double  m_absoluteTimeOrigin = 0.0;   // seconds; meaningful only when m_hasAbsoluteTimeOrigin

    int    m_triggerSource = 0;
    double m_triggerLevel  = 0.0;
    int    m_triggerSlope  = 0;      // 0 rising, 1 falling, 2 either
    bool   m_triggerArmed  = false;
    bool   m_running       = false;
};
