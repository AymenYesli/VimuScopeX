#pragma once

#include <QColor>
#include <QVector>
#include <QWidget>

#include <array>

#include <fftw3.h>

class QMouseEvent;

/*
 * A spectrum plot: frequency along X, level along Y (dBV/dBm, or linear
 * volts), up to MaxTraces independent traces overlaid (one per selected
 * source channel or Math). Shown stacked below ScopeView in a QSplitter,
 * only while Instruments' Spectrum mode is active — see
 * MainWindow::buildInstrumentsPage() and the QSplitter built in buildUi().
 *
 * The FFT itself is FFTW3 (linked via pkg-config's fftw3.pc — see
 * CMakeLists.txt), a real, well-tested library, deliberately not a
 * hand-rolled implementation. Window coefficients are precomputed into a
 * table whenever the window TYPE changes (rebuildWindow()), not recomputed
 * on every incoming block of samples, and shared across every trace — one
 * FFTW plan and one pair of scratch buffers serve all of them in turn,
 * since each transform is a complete, self-contained operation.
 *
 * MainWindow feeds this the SAME already-probe-scaled samples it hands
 * ScopeView/NavigatorView at each of their own three call sites (Capture's
 * samplesReady handler, Live's refreshLiveView(), Record's
 * refreshRecordView()), plus ScopeView::mathSamples() for a Math source —
 * this widget has no idea where any of it came from, or what "slot 0" vs
 * "slot 1" actually represents; that mapping is entirely MainWindow's own
 * bookkeeping (see MainWindow::updateSpectrumSources()).
 *
 * The graticule deliberately reuses ScopeView::HorizontalDivs/VerticalDivs
 * rather than its own separate division count, so the two plots' grids
 * always match exactly — see scopeview.h.
 *
 * Also owns its own small ruler-cursor overlay (a frequency pair and a
 * level pair, both draggable, with a small readout) — a separate,
 * self-contained system from ScopeView's own Rulers, not a shared one:
 * frequency/level and time/volts are different units on a different
 * widget, so there was nothing to actually share beyond the on/off toggle
 * living in the same Rulers panel page. See setRulersEnabled().
 */
class SpectrumView : public QWidget {
    Q_OBJECT

public:
    // Named WindowFunction, not Window: X11's Xlib.h defines a global
    // `Window` typedef that Qt's X11 platform plugin can pull in
    // transitively, and this project targets Arch Linux. A nested enum
    // would not actually collide with it (different scopes), but there is
    // no reason to invite the confusion on a name this common.
    enum class WindowFunction { Hann, Hamming, Blackman, FlatTop };
    enum class Unit { DBV, DBm };
    // Linear shows peak-equivalent volts directly, bypassing dB entirely —
    // Unit (dBV/dBm) has nothing to say in that mode, which is why
    // MainWindow disables that combo box whenever this is Linear.
    enum class YScale { Linear, Log };
    // Average is a simple exponential moving average across successive
    // spectra, computed alongside the plain latest one rather than instead
    // of it, so switching back to Normal shows current data immediately
    // rather than a stale average.
    enum class DisplayMode { Normal, Average };

    // Fixed for now rather than user-selectable — see HANDOFF.md for what a
    // size selector would need (mainly: rebuilding the FFTW plan and the
    // window table together whenever it changes, which this class already
    // does correctly for the window table alone).
    static constexpr int FftSize = 2048;
    // How many sources can be shown overlaid at once — MainWindow's own UI
    // enforces this as a hard cap when picking sources, not just a display
    // limit here.
    static constexpr int MaxTraces = 2;

    explicit SpectrumView(QWidget *parent = nullptr);
    ~SpectrumView() override;

    void setWindow(WindowFunction window);
    void setUnit(Unit unit);
    void setYScale(YScale scale);
    void setDisplayMode(DisplayMode mode);
    void setRulersEnabled(bool enabled);
    bool rulersEnabled() const { return m_rulersEnabled; }

    // `slot` is 0 or 1. Turning a slot off (setTraceActive(slot, false))
    // clears whatever it was showing; MainWindow calls this rather than
    // this widget ever deciding for itself which source belongs where.
    void setTraceActive(int slot, bool active);
    void setTraceColor(int slot, const QColor &color);

    // Takes the newest FftSize samples from `samples` (already in display
    // volts — probe gain, DC offset, and inversion applied, exactly what
    // Measure and the plot's own readouts use) and `sampleRate` in Hz,
    // computes that slot's spectrum, and repaints. Fewer than FftSize
    // samples, or a non-positive rate, clears just that slot rather than
    // guessing. Has no effect on a slot that is not active.
    void setTraceSamples(int slot, const QVector<double> &samples, double sampleRate);

    void clearAll();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    struct WindowTable {
        QVector<double> coefficients;
        // The window's own DC response — the mean of its coefficients —
        // computed from what rebuildWindow() actually generated rather than
        // a remembered textbook constant, so it is exactly right regardless
        // of FftSize or any edge effects. Dividing by this in
        // setTraceSamples() is what makes a bin-centred tone read its true
        // amplitude regardless of which window is applied.
        double coherentGain = 1.0;
    };
    struct Trace {
        bool   active      = false;
        bool   hasSpectrum = false;
        QColor color       = Qt::white;
        double sampleRate  = 0.0;
        QVector<double> bins;      // linear volts, peak-equivalent; latest computation
        QVector<double> avgBins;   // exponential moving average of the same, for DisplayMode::Average
    };

    void rebuildWindow();
    double toDb(double volts) const;
    // Maps a linear peak-volts value to a 0..1 fraction of the plot height
    // (0 = bottom), honouring the current YScale — the one place both
    // drawTrace() and the ruler-cursor code convert a value to a Y
    // position, so the two can never disagree about where "0 dB" or
    // "1.5 V" actually sits.
    double valueToPlotFraction(double volts) const;
    // The inverse — a plot Y coordinate back to linear peak volts, for
    // dragging a level cursor with the mouse.
    double plotFractionToValue(double fraction) const;

    QRectF plotRect() const;
    void drawGraticule(QPainter &p, const QRectF &plot) const;
    void drawTrace(QPainter &p, const QRectF &plot, const Trace &trace) const;
    void drawRulers(QPainter &p, const QRectF &plot) const;

    // Which of the four ruler cursors (if any) the mouse is currently
    // dragging — a plain enum rather than reusing ScopeView's own ruler-drag
    // sentinels, since this is a separate, unrelated cursor system.
    enum class RulerDrag { None, Freq1, Freq2, Level1, Level2 };

    // A cursor's value formatted for its flag/the delta readout — dB or
    // dBm text in Log, plain volts in Linear, matching whatever
    // valueToPlotFraction() itself currently honours.
    QString levelText(double volts) const;

    // Cursor pixel positions plus their flag rectangles, computed once and
    // shared between drawRulers() and hitTestRulers() — the same "shared
    // layout computation" ScopeView's own computeRulerLayout() already
    // uses for its Rulers, so drawing and hit-testing can never drift
    // apart from each other.
    struct RulerLayout {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        QRectF f1Flag, f2Flag, l1Flag, l2Flag;
    };
    RulerLayout computeRulerLayout(const QRectF &plot, double nyquist) const;
    RulerDrag hitTestRulers(const QPointF &pos, const QRectF &plot) const;

    WindowFunction m_window      = WindowFunction::Hann;
    Unit           m_unit        = Unit::DBV;
    YScale         m_yScale      = YScale::Log;
    DisplayMode    m_displayMode = DisplayMode::Normal;
    WindowTable    m_windowTable;
    std::array<Trace, MaxTraces> m_traces;

    // Ruler cursors: frequency pair in Hz, level pair in linear peak volts
    // (matching how a trace's own bins are stored — converted to dB/linear
    // only for display, exactly like Trace::bins is). Seeded lazily, on
    // first paint after being enabled, once an active trace's sample rate
    // is actually known — there is nothing sensible to seed a frequency
    // cursor to before that.
    bool   m_rulersEnabled = false;
    bool   m_rulersSeeded  = false;
    double m_freq1 = 0.0, m_freq2 = 0.0;
    double m_level1 = 0.0, m_level2 = 0.0;
    RulerDrag m_rulerDrag = RulerDrag::None;

    // Allocated once in the constructor and freed in the destructor: FFTW's
    // own types, not Qt-managed, and FftSize is fixed so the plan never
    // needs rebuilding after the first call. Shared by every trace in turn.
    double       *m_fftIn  = nullptr;
    fftw_complex *m_fftOut = nullptr;
    fftw_plan     m_plan   = nullptr;
};
