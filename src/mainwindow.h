#pragma once

#include <QColor>
#include <QMargins>
#include <QRectF>
#include <QElapsedTimer>
#include <QMainWindow>
#include "cantools.h"
#include "serialbus.h"
#include "sent.h"
#include "diag.h"

#include "probes.h"
#include <QVector>
#include <QMap>
#include <QPair>
#include <QStringList>
#include <array>
#include <deque>
#include <functional>

#include "adcwindow.h"   // AdcWindow in declarations below

class AcquisitionManager;
class StreamEngine;
class CardButton;
class NavigatorView;
class ScopeView;
class SpectrumView;
class XYView;
class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSettings;
class QLayout;
class QLineEdit;
class QShortcut;
class QPropertyAnimation;
class QPushButton;
class QSpinBox;
class QSplitter;
class QTabBar;
class QStackedWidget;
class QThread;
class QTimer;
class QToolButton;
class QWidget;
class DecoderEngine;
class RemoteServer;
class QJsonObject;
class LiveWorker;
struct LiveResult;
class QSoundEffect;
struct DecodeJob;
struct DecodeResult;

/*
 * Layout follows PicoScope 7: a strip of cards along the top for acquisition,
 * timebase and trigger; a rail of channel cards down the left; and a detail
 * panel that opens between them when a card is selected.
 */
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    // Stops the decoder thread before anything it could touch goes away.
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    enum Page { PageScope = 0, PageTrigger, PageChannel, PageInstruments, PageMath, PageRulers, PageMeasure, PageReferences, PageDecoding, PageAutomotive, PageRemote, PageSettings };
    // Which Instruments mode is actually showing something right now — a
    // proper enum rather than a lone "is Spectrum active" bool, since XY
    // needed a second, mutually-exclusive mode and two independent bools
    // can drift into an invalid "both true" state that an enum simply
    // cannot represent.
    enum class InstrumentMode { Scope, Spectrum, XY, Persistence };
    enum AcqMode { ModeCapture = 0, ModeStream, ModeRecord };

    enum ChannelMode { ChannelOff = 0, ChannelAuto, ChannelManual };

    struct ChannelControls {
        QLabel         *adcInfo  = nullptr;   // what the ADC actually covers (updateAdcInfo())
        QLabel         *voltsPageTitle = nullptr;   // "Volts per page", or the probe's unit per page
        QComboBox      *filterHighResN = nullptr;   // High-res: samples averaged
        QWidget        *filterHighResRow = nullptr;
        QButtonGroup   *mode     = nullptr;
        QComboBox      *scale    = nullptr;
        QPushButton    *colour   = nullptr;
        QPushButton    *manageProbes = nullptr;
        QComboBox      *voltsPage = nullptr;
        QComboBox      *coupling = nullptr;
        QComboBox      *probe    = nullptr;
        QDoubleSpinBox *inputOffset = nullptr;   // hardware window centre
        QDoubleSpinBox *offsetV  = nullptr;
        QDoubleSpinBox *offsetPct = nullptr;
        QCheckBox      *invert   = nullptr;
        QComboBox      *filterType = nullptr;     // ScopeView::FilterType
        QDoubleSpinBox *filterLowHz = nullptr;    // FrequencySpinBoxes (mainwindow.cpp):
        QDoubleSpinBox *filterHighHz = nullptr;   // high-pass / band "from", low-pass / band "to"
        QLabel         *filterLowLabel = nullptr;
        QLabel         *filterHighLabel = nullptr;
        QWidget        *filterLowRow = nullptr;
        QWidget        *filterHighRow = nullptr;
    };

    void buildUi();
    QWidget *buildTopBar();
    QWidget *buildChannelRail();
    QWidget *buildToolGrid();
    QWidget *buildDetailPanel();
    QWidget *buildScopePage();
    QWidget *buildTriggerPage();
    QWidget *buildChannelPage();
    QWidget *buildInstrumentsPage();
    QWidget *buildMathPage();
    void applyMathChannel();
    // Math's source lists: only the channels this scope (or opened file) has.
    void refreshMathSources(bool apply = true);
    QWidget *buildRulersPage();
    void refreshRulersChannelList();
    // Rulers > Engine: 720-degree cycle, cylinder overlay, relative
    // compression test. Analysis lives in enginetools.h (tested standalone);
    // this gathers the inputs, runs it and hands the results to ScopeView.
    // Runs from m_engineTimer, only when its inputs changed.
    void updateEngineAnalysis();
    void engineSettingsChanged();
    // Guided battery and charging tests (Automotive page). Each sets the
    // scope up, waits for its capture (cranking: a 10 s recording;
    // charging: one capture with CH1 DC and CH2 AC on the battery), runs
    // batterytools.h and shows a results panel on the plot.
    void startCrankingTest();
    void startChargingTest();
    void finishCrankingTest();
    void finishChargingTest();
    // Puts a channel on, DC or AC, at a page and input offset; false (with
    // a reason) if the probe can't cover it — e.g. 14 V on a x1 probe.
    bool prepareTestChannel(int ch, bool ac, double page, double inputOffset, QString *error);
    // CAN bus health panel: wire levels from the analog channels, load,
    // errors and the frame table from the latest CAN decode. Runs from the
    // engine timer, only when its inputs changed.
    void updateCanHealth();
    // LIN / K-Line health (Automotive page): per decoding slot, what its
    // last decode found (SerialBus), shown in info panel 2.
    void updateSerialHealth();
    struct SerialHealthData {
        bool    valid = false;
        QString message;                       // why nothing is shown, when decoding can't run
        std::vector<SerialBus::LinFrame> lin;
        SerialBus::KLineReport kline;
        double  bitTime = 0.0, sampleRate = 0.0, sampleCount = 0.0;
        int     baud = 0, linVersion = 2;
        std::vector<Sent::Frame> sent;
        std::vector<Sent::SerialMessage> sentMessages;
        double  sentTickUs = 3.0;
        int     sentMode = 0;
    };
    std::array<SerialHealthData, 2> m_serialHealth{};
    quint64         m_serialHealthRev = 0;
    QVector<double> m_serialHealthLastKey;
    QCheckBox      *m_serialHealthEnable = nullptr;
    // OBD-II / UDS summary (Automotive page): per decoding slot, the last
    // decode's diagnostic messages, shown in info panel 3.
    void updateDiagSummary();
    struct DiagData {
        bool    valid = false;
        QString message;
        std::vector<Diag::Message> messages;
        double  sampleRate = 0.0;
    };
    std::array<DiagData, 2> m_diagData{};
    quint64         m_diagRev = 0;
    QVector<double> m_diagLastKey;
    QCheckBox      *m_diagSummaryEnable = nullptr;
    void applyBatterySystemLimits();
    // Software AC: keeps a channel's hardware window on its DC level, from
    // each new frame's raw samples (see applyChannel()).
    void trackSoftwareAc(int ch, const QVector<double> &raw);
    // Record mode, zoomed out past what's filtered directly: a low-passed
    // copy of the whole recording at a reduced rate (block averages at >= 8x
    // the cutoff, at most 8M points), built once per recording / channel /
    // cutoff and drawn from at any zoom. False while recording (the file is
    // still growing) or if nothing could be read.
    bool ensureFilteredOverview(int ch, int type, double lowHz, double highHz);
    void setRulerCheckboxColor(QCheckBox *box, const QColor &colour);
    QWidget *buildMeasurePage();
    QWidget *buildReferencesPage();
    QWidget *buildDecodingPage();
    QWidget *buildAutomotivePage();
    QWidget *buildRemotePage();
    // Remote: the plot streamed to a phone browser, which can also send
    // commands back (see RemoteServer).
    void refreshRemote();
    void sendRemoteFrame();
    void handleRemoteCommand(const QJsonObject &command);
    // Protocol decoding: two slots, each decoded on the decoder thread and
    // shown as a bus row on the plot. onDecodeTick() runs on a timer and
    // posts a job for any enabled slot whose data or settings changed.
    void onDecodeTick();
    void onDecodeFinished(const DecodeResult &result);
    bool buildDecodeJob(int slot, DecodeJob *job, double *indexScale, QString *message);
    void decodeSettingsChanged(int slot);
    void setDecodeProtocol(int slot);
    void applyDecodeInputDefaults(int slot);
    void showDecodeMessage(int slot, const QString &message);
    void refreshReferencesPanel();
    // Disables Math when a reference just added would push the plot past 4
    // gutters total (CH1 + CH2 + Math + each reference) — see its own
    // definition for why this only ever turns Math off, never back on.
    void enforceReferenceGutterBudget();
    void openReferenceFile();
    void saveReferenceToFile(int slot);
    // The top toolbar's Save button: one dialog, format chosen by filter.
    // Capture/Live offers the native .vsxwave alongside PNG/CSV; Record
    // offers PNG/CSV only, since it already persists continuously via
    // .vsxrec and Temporary/Permanent — a native re-save would just be a
    // second, redundant copy of what is already on disk.
    void saveCurrentView();
    // Record mode: the shown recording (Temporary ones too, before they're
    // cleared) copied to <folder>/<name>/<name>.vsxrec + meta.ini, off the GUI thread.
    void saveRecordingAs(const QString &chosen);
    void saveViewAsPng(const QString &path);
    void saveViewAsCsv(const QString &path);
    // Appends extension if path doesn't already end with it (case
    // insensitive) — the native save dialog doesn't reliably do this
    // itself on every platform when the person types a bare name.
    static QString ensureExtension(QString path, const QString &extension);
    // Finds the entry whose itemData() is numerically closest to value and
    // selects it, without blocking signals — letting the combo's own
    // currentIndexChanged handler apply the match normally, the same as an
    // ordinary user selection would. Generalizes a nearest-match pattern
    // already used in three other places in this file (timebase restore at
    // startup, and applyTimebase()'s own per-mode fallback) for the native
    // .vsxwave format's own restoration, which needs the same "closest
    // available value" tolerance for voltsPage and scale as well.
    static void setComboByNearestValue(QComboBox *box, double value);
    void saveViewAsNative(const QString &path);
    // Full: only the plot (and a Spectrum/XY pane if one is open) fills the
    // screen; everything around it is hidden and restored exactly as it was.
    void setFullView(bool full);
    // Default: puts the instrument setup back to first-run defaults — see
    // the definition for what is and is not touched.
    void resetToDefaults();
    void openNativeWaveform(const QString &path);
    // With no scope connected, no device has said how many channels there
    // are (the count stays 0: cards hidden, nothing drawn). Opening a file
    // then takes the count from the file instead; see the definition.
    void adoptChannelCountWithoutDevice(int fileChannels);
    QWidget *buildSettingsPage();
    void refreshSettingsPage();
    // Re-applies the palette, the global style sheet, every cached
    // style-sheet string and baked icon pixmap built from theme colours, and
    // repaints every custom-painted view — called once at construction
    // (after those widgets exist) and again whenever Settings' Dark/Light
    // toggle changes Theme::currentMode() at runtime.
    void applyTheme();
    void refreshLogo();
    void refreshRecordTabs();
    // Keeps each tab's close-button icon dark on the selected tab's bright
    // background and light everywhere else — called whenever the rebuild
    // or the selection itself changes.
    void updateRecordTabCloseIcons();
    // The one place that switches which recording is being viewed — saves
    // the outgoing recording's own view state and restores the incoming
    // one's, rather than the three separate call sites (entering Record
    // mode, clicking a tab, Open) each doing their own version of this.
    void loadRecordTab(const QString &directory);
    void applyRecordTabViewState(const QString &directory);
    // Persists (or clears) a recording's "closed by hand" status across
    // launches — see refreshRecordTabs() for where this is read back.
    void setRecordTabHidden(const QString &directory, bool hidden);
    void refreshMeasureChannelSelector();
    void refreshSpectrumChannelSelector();
    void updateSpectrumSources();
    void feedSpectrum(int ch, const QVector<double> &samples, double rate);
    void refreshXYChannelSelectors();
    void updateXYColors();
    void updateXYRanges();
    void feedXY(int ch, const QVector<double> &samples);
    void setInstrumentMode(InstrumentMode mode);
    void wireUp();

    void openPage(Page page, int channel = -1);
    void closePanel();
    void syncPanelButtons();
    void refreshCards();

    void onDeviceChanged(bool ready, int channels);
    void applyTimebase(double exactOverride = -1.0);
    void refreshTimebaseOptions();
    void applyChannel(int channel);
    void updateOffsetRanges(int channel);
    void reloadProbeList(int channel);
    // The channel page's units follow its probe: the per-page title and
    // items, and the offsets (the values are already in probe units).
    void applyProbeUnits(int channel);
    const Probe &probeFor(int channel) const;
    void pickChannelColour(int channel);
    void autoRange(int channel);
    void setAcquisitionMode(int mode);
    void startAcquisition();
    void stopAcquisition();
    void pullStream();
    void applyStreamSettings();
    void applyRecordRate();
    void refreshRecordView();
    void refreshLiveView();
    void zoomToRect(const QRectF &fractions);
    void resetZoom();
    // Back/forward through zoomToRect()'s own history, one stack pair per
    // acquisition mode (see m_zoomBack/m_zoomForward) — see zoomToRect()
    // for what pushes to it and applyZoomState() for what a stored entry
    // actually restores.
    void pushZoomHistory();
    void navigateZoomHistory(bool forward);
    void updateZoomHistoryButtons();
    struct ZoomState {
        double  captureTimebase = 0.0;
        double  captureCentreFraction = 0.5;
        double  liveTimebase = 0.0;
        quint64 liveViewOffset = 0;
        double  liveCentreFraction = 0.5;   // a loaded Live file pans like a Capture
        quint64 recordViewStart = 0;
        quint64 recordViewLength = 0;
    };
    ZoomState captureZoomState() const;
    void applyZoomState(const ZoomState &state);
    void updateStatusBar();
    void saveSettings() const;
    void restoreSettings();
    void saveSettingsTo(QSettings &settings) const;
    void restoreSettingsFrom(QSettings &settings, bool session);
    // A loaded .vsxwave is a view, not a change to the user's setup: the
    // settings from before it was opened, kept here (an INI file), come back
    // when its view is left — a mode switch, Run, Single — and are what gets
    // saved meanwhile. Colours aren't taken from files at all.
    QString m_fileViewSnapshot;
    bool    m_applyingFileView = false;   // the file's own mode switch isn't "leaving"
    void    takeFileViewSnapshot();
    void    leaveFileView();
    void setChannelPositionPercent(int channel, double percent);
    void applyTrigger();
    void setRunning(bool running);
    void syncNavigator();
    void stepTimebase(int delta);
    void stepTriggerLevel(int delta);
    void stepVoltsPerDiv(int channel, int delta);
    // The user picked a range on a channel in Auto: it goes Manual (Auto
    // would undo the pick on the next frame).
    void leaveAutoRange(int channel);
    void updateTriggerLevelRange();
    void setTriggerLevelToMidpoint();

    AcquisitionManager *m_acq  = nullptr;
    ProbeLibrary       *m_probes = nullptr;
    StreamEngine       *m_stream  = nullptr;
    QColor              m_colour[4];
    ScopeView          *m_view = nullptr;
    NavigatorView      *m_nav  = nullptr;
    SpectrumView       *m_spectrum = nullptr;
    QSplitter          *m_plotSplitter = nullptr;

    CardButton *m_runCard     = nullptr;
    CardButton *m_scopeCard   = nullptr;
    CardButton *m_triggerCard = nullptr;
    CardButton *m_channelCard[4] = {};
    CardButton *m_logicCard   = nullptr;
    CardButton *m_dioCard     = nullptr;

    QStackedWidget *m_panel      = nullptr;
    QWidget        *m_panelHost  = nullptr;
    QWidget        *m_topBar     = nullptr;
    QWidget        *m_channelRail = nullptr;
    QLayout        *m_rootLayout = nullptr;
    // Full view: the Esc shortcut (enabled only while full) and what to put
    // back afterwards.
    QShortcut      *m_fullEscShortcut = nullptr;
    bool            m_fullView = false;
    bool            m_fullWasFullScreen = false;
    bool            m_fullWasMaximized = false;
    QMargins        m_fullRootMargins;
    std::array<bool, 6> m_fullWasVisible{};
    QLabel         *m_panelTitle = nullptr;
    // Slides m_panelHost's own maximumWidth between 0 (closed) and its full
    // width (open) — see openPage()/closePanel(). minimumWidth stays at 0
    // permanently so the layout can actually shrink it that far; maximumWidth
    // is the only property this animates.
    QPropertyAnimation *m_panelAnimation = nullptr;

    // Scope page
    QComboBox *m_timebase   = nullptr;
    QComboBox *m_depth      = nullptr;
    QButtonGroup *m_acqMode = nullptr;
    QComboBox *m_liveRate    = nullptr;
    QComboBox *m_history    = nullptr;
    QComboBox *m_updates    = nullptr;
    QComboBox *m_recordSize = nullptr;
    // Temporary/Permanent choice for the NEXT recording — segment buttons,
    // exclusive, defaulting to Permanent (the safer default: a person has to
    // opt into deletion, not opt out of it).
    QButtonGroup *m_recordPersistGroup = nullptr;
    // What the choice actually WAS for whichever recording's files are
    // currently on disk — captured the moment a recording starts, not read
    // Every recording started this session with Keep set to Temporary,
    // deleted in full on exit — a list, not a single "the last one", since
    // a person can make several recordings in one session and each one's
    // own Keep choice is independent: remembering only the most recent
    // meant an earlier Temporary recording, once a later recording (of
    // either kind) started, was never tracked for deletion again at all,
    // and persisted indefinitely across restarts. Read fresh from
    // m_recordPersistGroup at the moment each recording starts, since the
    // toggle could be flipped afterward without a new recording ever being
    // made, which would otherwise silently change what exit does to files
    // already recorded under the earlier choice.
    QStringList m_temporaryRecordings;
    // Recordings opened (Open) from outside the record directory. The tab bar
    // is built from the record directory, so these had no tab: re-entering
    // Record rebuilt the tabs and switched away from (or cleared) the opened
    // recording. Listed here, they get tabs too; saved as records/opened.
    QStringList m_openedRecordings;
    struct FilteredOverview {
        QString directory;
        int     type = 0;            // ScopeView::FilterType
        double  lowHz = 0.0;
        double  highHz = 0.0;
        quint64 total = 0;
        int     factor = 1;          // raw samples per overview point
        QVector<float> data;         // raw units: the filtered series (low/band-pass),
                                     // or min,max pairs of the exact high-pass output
        int     points = 0;          // overview points (data holds 2 per point for high-pass)
    };
    std::array<FilteredOverview, 4> m_filteredOverview;
    // Live's overview strip, kept up to date incrementally (refreshLiveView()).
    struct LiveOverviewCache {
        quint64 processed = 0;      // written() count already folded in
        quint64 capacity = 0;
        quint64 blockSize = 1;
        quint64 partialCount = 0;
        float   partialMin = 0.0f, partialMax = 0.0f;
        std::deque<std::pair<float, float>> blocks;   // min, max per block, oldest first
    };
    std::array<LiveOverviewCache, 4> m_liveOverview;
    // Live with long windows: the plot gets min/max pairs (see
    // refreshLiveView()); exact measurement data refreshed at most 4x/s.
    bool m_liveReduced = false;
    std::array<QElapsedTimer, 4> m_liveMeasureClock;
    // ... prepared on a worker thread (liveworker.h): one frame at a time.
    QThread    *m_liveThread = nullptr;
    LiveWorker *m_liveWorker = nullptr;
    bool        m_liveJobInFlight = false;
    // Annotations per context: "capture", "live", "record:<directory>" (one
    // set per recording tab). Swapped on mode / tab changes; all saved.
    QHash<QString, QVariantList> m_annotationSets;
    QString m_annotationContext;
    bool    m_annotationsReady = false;   // after the startup restore
    QString annotationContext() const;
    void syncAnnotationContext();
    bool        m_liveJobPending = false;     // asked while one was in flight: run it next
    QElapsedTimer m_livePanClock;             // last pan: readouts wait until it settles
    bool        m_liveMeasureFollowUp = false;
    void applyLiveResult(const LiveResult &result);
    void finishLiveRefresh(qint64 wanted, double rate, quint64 available);
    // Whichever recording is currently loaded for viewing — set by
    // loadRecordTab() (browsing) or directly in startAcquisition() (a
    // fresh recording, actively being written, is "current" too). Kept
    // deliberately separate from m_temporaryRecordings above: that one only
    // ever grows, tracking every Temporary recording made this session for
    // deletion at exit, while this one changes every time the view
    // switches to a different recording for any reason and says nothing
    // about whether that recording is Temporary or Permanent.
    QString m_currentRecordTab;
    // One recording's own saved view state — zoom/pan position within it,
    // and each channel's own vertical scale/position — restored the next
    // time that recording's tab is selected, rather than always resetting
    // to a fresh default view.
    struct RecordTabViewState {
        quint64 viewStart = 0;
        quint64 viewLength = 0;   // 0: not yet saved, use the whole recording
        struct ChannelState {
            double voltsPerDiv = 0.0;    // 0: not yet saved, leave whatever is already set
            double positionVolts = 0.0;
            bool visible = true;
        };
        std::array<ChannelState, 4> channels;
    };
    QMap<QString, RecordTabViewState> m_recordTabStates;   // keyed by recording directory
    // One tab per past recording in the current record directory, shown only
    // in Record mode — see refreshRecordTabs().
    QTabBar *m_recordTabs = nullptr;
    // Settings page: shows the record directory recordDirectory() currently
    // resolves to, kept in sync by refreshSettingsPage() whenever it could
    // plausibly have changed.
    QLabel *m_recordDirLabel = nullptr;
    // Settings page: Dark/Light segments, id 0 and 1 respectively, matching
    // Theme::Mode's own ordering.
    QButtonGroup *m_themeGroup = nullptr;
    // The top-bar logo — swapped between logo-dark.png/logo-light.png by
    // applyTheme() depending on Theme::currentMode(), since each is only
    // legible against the opposite background (dark text on light art, or
    // the reverse).
    QLabel *m_logoLabel = nullptr;
    // Every icon-bearing widget whose icon/style depends on Theme:: colours
    // rebuilds both from scratch when invoked — collected as each is built
    // (most are anonymous locals from a loop, with no member pointer to
    // reach later) so applyTheme() has a way to redo them on a live theme
    // toggle. A style sheet alone re-evaluates on its own once re-applied,
    // but a QIcon is a baked pixmap: nothing short of re-rendering it points
    // at the new colour, which is why this stores a callback rather than
    // just the widget.
    QVector<std::function<void()>> m_themedRefreshers;
    QComboBox *m_recordRate = nullptr;
    QWidget   *m_recordOnly = nullptr;
    QLabel    *m_statusLine = nullptr;
    quint64    m_recordViewStart  = 0;
    quint64    m_recordViewLength = 0;
    // Samples back from the newest one — only ever nonzero while Live is
    // frozen (panning is gated on Freeze; see the panRequested handler).
    // Reset when Freeze comes off or a fresh Live run starts (both mean the
    // ring this offset was measured against is gone or moving again), but
    // NOT when switching to another mode and back — Live's own pan
    // position is part of what persists across a mode switch, the same as
    // its own timebase (m_liveTimebase below).
    quint64    m_liveViewOffset   = 0;
    // Each mode's own timebase, restored exactly when switching back to
    // that mode rather than reusing whatever the combo happens to show —
    // Capture and Live have different, non-overlapping ranges (kTimebases
    // tops out at 100 ms; kRollTimebases starts there and goes up to 5 s),
    // so sharing one "current" value meant leaving Live at 1 s/div and
    // switching to Capture forced a nearest-match substitution down to
    // Capture's own 100 ms maximum — and switching back to Live then found
    // that substituted 100 ms as an exact match in Live's own list,
    // permanently replacing the original 1 s with no way back to it short
    // of resetting it by hand. Record doesn't use this mechanism at all
    // (see applyTimebase()'s own early return), so needs no third value.
    double     m_captureTimebase = 1e-2;   // matches kTimebases[kDefaultTimebaseIndex] in mainwindow.cpp
    double     m_liveTimebase    = 1.0;
    // Capture mode's own last-displayed waveform — updated every time a
    // fresh capture arrives (see the samplesReady handler), restored
    // whenever Capture mode is switched back into. Without this, switching
    // to Live or Record and back showed whatever that other mode had left
    // on screen, since ScopeView's own buffer is shared and gets
    // overwritten by each mode's own refresh the moment it becomes active.
    struct CaptureSnapshot {
        std::array<QVector<double>, 4> samples;
        std::array<bool, 4> valid{};
        double sampleRate = 0.0;
        double triggerPoint = -1.0;
        bool hasData = false;
        // Where the view was panned to, so coming back to the mode lands in
        // the same place (only used when it wasn't simply following the
        // trigger).
        double centreFraction = 0.5;
        bool   followingTrigger = true;
        // This mode's own per-channel vertical scale/position, saved when
        // leaving the mode (see setAcquisitionMode()) so a level dragged
        // in one mode never bleeds into another. voltsPerDiv: 0.0 means
        // "never saved yet" (0 V/div is never a real value, but 0 V
        // position is, so positionVolts has no such gate). Raw volts, not
        // a percentage — matches RecordTabViewState's own convention,
        // and sidesteps the position spinbox's percentage depending on
        // whichever volts/div range happens to be selected right now,
        // which voltsPerDiv here has not necessarily been restored to yet
        // at the point positionVolts is applied.
        std::array<double, 4> voltsPerDiv{};
        std::array<double, 4> positionVolts{};
    };
    CaptureSnapshot m_captureSnapshot;
    // Live mode's own equivalent — updated every time refreshLiveView()
    // actually has data to show, restored (or, with hasData false,
    // explicitly cleared) whenever Live mode is switched back into. Live's
    // own ring buffer state is not trusted to survive a round trip through
    // another mode by itself (attach() is not confirmed to leave existing
    // history intact), so this snapshot is the same explicit belt-and-braces
    // mechanism Capture already has, not a assumption that the ring alone
    // is enough.
    CaptureSnapshot m_liveSnapshot;
    QElapsedTimer m_recordRefresh;
    // A real throttle, not a debounce: the first event in a burst refreshes
    // immediately; further events during the 16 ms cooldown only set the dirty
    // flag; the cooldown's own timeout does one more refresh if anything
    // changed since, so the view is never left stale at the end of a drag.
    QTimer *m_recordPanDebounce = nullptr;
    bool    m_recordPanDirty = false;
    double  m_recordPanRemainder = 0.0;   // sub-sample carry between drag
                                          // events — see panRequested
    // The offsetPct spinbox displays whole percent only (setDecimals(0)),
    // so reading it back as the base for the next drag event — as
    // offsetDragged's handler used to — silently discarded any delta too
    // small to move the rounded, displayed value by a whole percent. A
    // slow drag is exactly a long run of such deltas, each one lost the
    // moment it was read back, which is why only a fast drag (a single
    // delta large enough to survive the rounding by itself) ever moved
    // anything. Kept in exact sync with the spinbox by
    // setChannelPositionPercent() — the one function every caller already
    // goes through, dragging included — so this always holds the true,
    // unrounded value the spinbox's own display is only an approximation
    // of, the same fix already applied to horizontal panning above.
    std::array<double, 4> m_channelOffsetPctExact{};
    // The overview strip's own data spans the WHOLE recording regardless of
    // where the zoomed-in view currently sits, so it never actually changes
    // while panning or zooming within one already-written recording —
    // refreshRecordView() used to re-read and re-decimate the entire file
    // for it on every single call regardless, which for a large recording
    // meant hundreds of extra disk seeks on every drag frame, on top of the
    // (cheap, small) read the zoomed-in view itself needs. Cleared to
    // false — forcing one fresh read — whenever the tab, the recorded
    // length, or a channel's own on/off state changes; left alone
    // otherwise, which is what actually removes the redundant work.
    QString m_overviewCacheTab;
    quint64 m_overviewCacheTotal = 0;
    std::array<bool, 4> m_overviewCacheValid{};
    // The actual decimated points behind m_overviewCacheValid, so a cache
    // hit can still re-feed m_nav (see refreshRecordView()'s own comment on
    // why a hit alone is not enough) without re-reading the file — m_nav is
    // shared with Capture/Live and gets overwritten by whichever of them
    // ran most recently, but the underlying overview data has not actually
    // changed just because the mode was switched away and back.
    std::array<QVector<double>, 4> m_overviewCacheSamples;
    // Live's own equivalent of Record's overview cache: refreshLiveView()
    // runs every 33ms (m_streamTick), decimating the full ring history that
    // often is unnecessary work an overview strip does not need refreshed
    // at — this throttles it to roughly Record's own 400ms cadence instead.
    QElapsedTimer m_liveOverviewRefresh;
    // One independent back/forward pair per acquisition mode, indexed
    // directly by AcqMode (ModeCapture=0/ModeStream=1/ModeRecord=2) — a
    // history built up zooming around in Record mode has no bearing on
    // Capture's own, and switching modes should never silently jump the
    // OTHER mode's zoom around as a side effect of navigating this one's.
    std::array<QVector<ZoomState>, 3> m_zoomBack;
    std::array<QVector<ZoomState>, 3> m_zoomForward;
    // Record mode keeps one history per recording, not one for all of them:
    // m_zoomBack/m_zoomForward[ModeRecord] hold the current tab's, and are
    // parked here (keyed by recording directory) when another tab is shown,
    // restored when it comes back, and dropped when the tab is closed — a
    // shared history let a closed tab's zoom steps bring its waveform back.
    QMap<QString, QPair<QVector<ZoomState>, QVector<ZoomState>>> m_recordZoomHistory;
    QToolButton *m_zoomButton = nullptr;
    QToolButton *m_instrumentsButton = nullptr;
    QToolButton *m_mathToolButton    = nullptr;
    QToolButton *m_rulersButton      = nullptr;
    QToolButton *m_measureButton     = nullptr;
    QToolButton *m_referencesButton  = nullptr;
    QToolButton *m_decodingButton    = nullptr;
    QToolButton *m_automotiveButton  = nullptr;
    QToolButton *m_remoteButton      = nullptr;
    QToolButton *m_settingsButton    = nullptr;
    QWidget   *m_timebaseGroup = nullptr;   // "Time per division" — hidden in Record
    QWidget   *m_captureOnly = nullptr;
    QWidget   *m_streamOnly  = nullptr;
    QToolButton *m_singleButton = nullptr;
    QToolButton *m_forceButton  = nullptr;
    QToolButton *m_freezeButton = nullptr;
    QTimer    *m_streamTick = nullptr;
    QLabel    *m_sampleInfo = nullptr;

    // Trigger page
    QButtonGroup   *m_trigModeGroup = nullptr;
    QComboBox      *m_autoWait = nullptr;      // how long Auto waits for a trigger (Capture)
    QCheckBox      *m_rulersTrackCheck = nullptr;   // Rulers: voltage rulers follow the trace
    // ADC range measurement: the live readout on each channel page, and the
    // Range report (Settings) that steps every page at several input offsets.
    void updateAdcInfo();
    // The filter's lower parameter: N for High-res, else the lower cutoff in Hz.
    double filterLowParam(int ch) const;
    // Capture averaging: a running mean of the last N triggered frames per
    // channel (exponential once N is reached), aligned on the trigger point;
    // restarted whenever a setting changes (m_avgGeneration).
    struct FrameAverage {
        QVector<double> acc;
        int     count = 0;
        double  trigger = -1.0, rate = 0.0;
        quint64 generation = 0;
    };
    std::array<FrameAverage, 4> m_avg{};
    quint64    m_avgGeneration = 1;
    bool       m_avgNoTriggerShown = false;
    QComboBox *m_average = nullptr;
    QVector<double> averageFrame(int ch, const QVector<double> &samples, double fs, double *trigger);
    // Per channel, the stuck setting already reset once (see updateAdcInfo()).
    std::array<QString, 4> m_frontEndResetKey{};
    QString adcWindowText(const AdcWindow &w) const;
    void startRangeReport();
    void rangeReportTick();
    void finishRangeReport();
    struct RangeStep { int ch; int minMv; int maxMv; };
    QVector<RangeStep> m_rrSteps;
    QStringList        m_rrRows;
    int                m_rrIndex = -1, m_rrPhase = 0, m_rrModeBefore = 0;
    bool               m_rrForced = false;
    quint64            m_rrSerial = 0;
    QElapsedTimer      m_rrClock;
    QTimer            *m_rrTimer = nullptr;
    QTimer            *m_adcInfoTimer = nullptr;
    // Live: a stream restart a timebase change needs (rate or history),
    // collapsed into one after the changes stop; and when the stream last
    // started (the SDK is left alone just after).
    QTimer            *m_streamRestartTimer = nullptr;
    QElapsedTimer      m_streamStartClock;
    QPushButton       *m_rangeReportButton = nullptr;
    void updateAutoTriggerWait();
protected:
    // Left / Right arrow keys pan a recording (Record mode).
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    QButtonGroup   *m_trigDirGroup  = nullptr;
    QComboBox      *m_trigSource    = nullptr;
    QDoubleSpinBox *m_trigLevel     = nullptr;
    QSpinBox       *m_preTrigger    = nullptr;
    QPushButton    *m_levelToMid    = nullptr;

    // Channel page (rebound to whichever channel is selected)
    ChannelControls m_channel[4];
    QStackedWidget *m_channelPages = nullptr;
    QButtonGroup   *m_instrumentMode = nullptr;
    // Second pane, shared by Spectrum and XY: only one of the two is ever
    // visible at a time (m_currentInstrumentMode says which, if either),
    // shown in the same QSplitter slot below m_view via a QStackedWidget
    // so the splitter itself never has to know which content it holds —
    // see MainWindow::setInstrumentMode() and the QSplitter built in
    // buildUi().
    InstrumentMode  m_currentInstrumentMode = InstrumentMode::Scope;
    QStackedWidget *m_secondPaneStack = nullptr;

    // Spectrum (Instruments -> Spectrum). Up to SpectrumView::MaxTraces (2)
    // sources at once, real channels or Math — m_spectrumSources[slot] is
    // -1 (nothing) or a channel index 0-3, or ScopeView::RulersMathChannel
    // for Math; that is the ONLY place the "which slot is which source"
    // mapping lives, SpectrumView itself has no idea.
    std::array<int, 2> m_spectrumSources{-1, -1};
    QWidget        *m_spectrumControls = nullptr;
    QButtonGroup   *m_spectrumChannelGroup = nullptr;
    std::array<QPushButton *, 4> m_spectrumChannelSegments{};
    QPushButton    *m_spectrumMathSegment = nullptr;
    QComboBox      *m_spectrumWindowCombo = nullptr;
    QComboBox      *m_spectrumUnitCombo   = nullptr;
    QComboBox      *m_spectrumDisplayModeCombo = nullptr;
    QComboBox      *m_spectrumYScaleCombo      = nullptr;

    // XY (Instruments -> XY). Real channels only, one each for X and Y —
    // an exclusive single choice per axis, unlike Spectrum's up-to-2
    // multi-select, since an XY plot inherently needs exactly one source
    // per axis, never zero or more than one.
    XYView         *m_xy = nullptr;
    QWidget        *m_xyControls = nullptr;
    QWidget        *m_persistenceControls = nullptr;
    QButtonGroup   *m_xyXGroup = nullptr;
    QButtonGroup   *m_xyYGroup = nullptr;
    std::array<QPushButton *, 4> m_xyXSegments{};
    std::array<QPushButton *, 4> m_xySegments{};
    int             m_xyChannelX = 0;
    int             m_xyChannelY = 1;
    // Every channel's most recent samples, cached here so that whichever
    // one of X/Y updates second still has the other's latest data on hand
    // to pair with — MainWindow otherwise only ever sees one channel's
    // samples at a time, at the moment they arrive. Kept for all four
    // channels unconditionally (cheap relative to a capture buffer itself,
    // and means XY mode has something to show immediately on activation
    // rather than waiting for the next sample tick).
    std::array<QVector<double>, 4> m_lastChannelSamples;

    QCheckBox      *m_mathEnabled = nullptr;
    QComboBox      *m_mathOp      = nullptr;
    QComboBox      *m_mathSourceA = nullptr;
    QComboBox      *m_mathSourceB  = nullptr;
    QComboBox      *m_mathVoltsPage = nullptr;
    QCheckBox      *m_rulersTimeCheck = nullptr;
    std::array<QCheckBox *, 4> m_rulersChannelChecks{};
    QCheckBox      *m_rulersMathCheck = nullptr;
    QCheckBox      *m_rulersFftCheck  = nullptr;
    QCheckBox      *m_engineEnable = nullptr;
    QSpinBox       *m_engineCylinders = nullptr;
    QLineEdit      *m_engineFiringOrder = nullptr;
    QComboBox      *m_engineStroke = nullptr;
    QComboBox      *m_engineStyle = nullptr;
    QComboBox      *m_engineSource = nullptr;
    QPushButton    *m_engineResetMarkers = nullptr;
    QWidget        *m_engineSyncBox = nullptr;
    QDoubleSpinBox *m_engineSyncThreshold = nullptr;
    QComboBox      *m_engineSyncEdge = nullptr;
    QDoubleSpinBox *m_engineSyncOffset = nullptr;
    QWidget        *m_compressionBox = nullptr;
    // Crank + cam sensors (cycle source 2).
    QWidget        *m_crankBox = nullptr;
    QComboBox      *m_crankChannel = nullptr;
    QComboBox      *m_camChannel = nullptr;
    QComboBox      *m_crankPattern = nullptr;
    QDoubleSpinBox *m_crankTdc = nullptr;      // cylinder 1 TDC, degrees after the gap
    QPushButton    *m_crankCalibrate = nullptr;
    QDoubleSpinBox *m_camSpec = nullptr;       // expected cam edge angle (-1: off)
    QDoubleSpinBox *m_camTolerance = nullptr;
    QPushButton    *m_camRemember = nullptr;
    QPushButton    *m_camForget = nullptr;
    double          m_camKnownGood = -1.0;     // < 0: none remembered
    double          m_lastCamAngle = -1.0;     // < 0: none measured
    double          m_lastTdcCalibration = -1.0;   // < 0: none available

    // Software AC coupling, per channel: on when AC is selected but the
    // hardware can't (or didn't) switch. The hardware window is then centred
    // on the tracked DC level (absolute volts) and the plot takes the DC off.
    std::array<bool, 4>   m_softwareAc{};
    std::array<double, 4> m_softAcCentre{};
    std::array<bool, 4>   m_softAcUnreachable{};
    // One frame with the window opened to the probe's full range, to read
    // the true DC level after the signal was found clipped (see
    // trackSoftwareAc()).
    std::array<bool, 4>   m_softAcSeeking{};
    int                   m_chargePass = 0;   // charging test: 0 finds the DC, 1 measures

    // Guided battery / charging tests.
    enum GuidedTest { GuidedNone = 0, GuidedCranking, GuidedCharging, GuidedDrain };
    int             m_guidedTest = GuidedNone;
    bool            m_guidedWaiting = false;
    QTimer         *m_guidedStopTimer = nullptr;
    QComboBox      *m_batterySystem = nullptr;     // 12 or 24 (volts)
    QDoubleSpinBox *m_batteryTemperature = nullptr;   // C: shifts the cranking limit
    QCheckBox      *m_batteryClamp = nullptr;          // current clamp on CH2: internal resistance
    QDoubleSpinBox *m_batteryRatedCca = nullptr;       // A, 0 = not set
    QDoubleSpinBox *m_limitRestingGood = nullptr;
    QDoubleSpinBox *m_limitCrankMin = nullptr;
    QDoubleSpinBox *m_limitChargeLow = nullptr;
    QDoubleSpinBox *m_limitChargeHigh = nullptr;
    QDoubleSpinBox *m_limitRipple = nullptr;
    QPushButton    *m_crankTestButton = nullptr;
    QPushButton    *m_chargeTestButton = nullptr;
    QPushButton    *m_clearTestButton = nullptr;

    // Parasitic drain test (current clamp, long Record): batterytools.h.
    void startDrainTest();
    void finishDrainTest();
    void updateDrainProgress();
    QComboBox      *m_drainChannel = nullptr;
    QComboBox      *m_drainDuration = nullptr;    // seconds
    QDoubleSpinBox *m_drainCapacity = nullptr;    // Ah
    QDoubleSpinBox *m_drainLimit = nullptr;       // mA
    QPushButton    *m_drainButton = nullptr;
    QTimer         *m_drainTimer = nullptr;       // ends the test
    QTimer         *m_drainTick = nullptr;        // progress, every second
    qint64          m_drainStartMs = 0;
    int             m_drainSeconds = 0;

    // Zero calibration: per data path (0 Capture, 1 Live/Record streaming),
    // channel, coupling and hardware half-range, the offset measured with
    // the inputs open; subtracted where data enters (both engines).
    struct CalStep { int path; int ch; int coupling; int halfMv; };
    // Full calibration: per path / channel / coupling, the largest hardware
    // range measured so far (mV, read back from the ADC). Requests ascend, so
    // one at or below it lands on a range already done and is skipped.
    QHash<QString, int> m_calCoverage;
    int                 m_calCoveredSkips = 0;
    void startCalibration(bool quick);
    void runCalibrationStep();
    void calibrationMeasured(double volts, double peakToPeak);
    void saveZeroTable() const;
    // An open input reads within a few mV of zero and is quiet. Steps that
    // see more are rejected (a probe still on the scope's calibration output
    // read as a 1 V "offset" and recentred its 0-2 V square wave on 0 V).
    std::array<int, 4>    m_calRejectCount{};
    std::array<double, 4> m_calRejectMean{}, m_calRejectPtp{};
    QPushButton          *m_clearCalButton = nullptr;
    void finishCalibration();
    void applyZeroOffsets(int ch);
    double zeroOffsetFor(int path, int ch, int coupling, int halfMv) const;
    void refreshCalibrationStatus();
    QHash<QString, double> m_zeroTable;
    QVector<CalStep> m_calSteps;
    int             m_calIndex = 0;
    bool            m_calibrating = false;
    bool            m_calQuick = false;
    int             m_calPath = -1;           // the path the hardware is set up for
    int             m_calModeBefore = 0;
    std::array<int, 4> m_calChannelModes{};
    QHash<QString, double> m_calResults;
    QTimer         *m_calTimer = nullptr;     // settle, then measure
    QTimer         *m_calTimeout = nullptr;   // a step that never answers is skipped
    QCheckBox      *m_zeroEnable = nullptr;
    QPushButton    *m_quickZeroButton = nullptr;
    QPushButton    *m_fullCalButton = nullptr;
    QLabel         *m_calStatus = nullptr;
    std::array<int, 4> m_appliedHalfMv{};     // what applyChannel last set
    std::array<int, 4> m_appliedCoupling{};

    // Plug in / out sounds (Qt Multimedia builds only) and the startup
    // animation switch (read by main.cpp before this window exists).
    QSoundEffect   *m_plugInSound = nullptr;
    QSoundEffect   *m_plugOutSound = nullptr;
    QCheckBox      *m_soundsEnable = nullptr;
    QCheckBox      *m_startupAnimation = nullptr;
    bool            m_deviceStateKnown = false;   // the state at launch plays nothing
    bool            m_lastDeviceReady = false;

    // Remote.
    RemoteServer   *m_remote = nullptr;
    QTimer         *m_remoteTimer = nullptr;
    QCheckBox      *m_remoteEnable = nullptr;
    QSpinBox       *m_remotePort = nullptr;
    QComboBox      *m_remoteAddress = nullptr;
    QComboBox      *m_remoteRate = nullptr;
    QLabel         *m_remoteQr = nullptr;
    QLabel         *m_remoteUrl = nullptr;
    QLabel         *m_remoteStatus = nullptr;
    QPushButton    *m_remoteNewKey = nullptr;
    double          m_remoteSeq = 0;   // last phone drag applied (echoed in frames)

    // CAN bus health.
    QCheckBox      *m_canHealthEnable = nullptr;
    std::vector<CanTools::CanFrame> m_canFrames;   // latest CAN decode
    int             m_canStrayErrors = 0;
    double          m_canSampleCount = 0.0;
    double          m_canSampleRate = 0.0;
    bool            m_canDecoded = false;
    quint64         m_canHealthRev = 0;
    QVector<double> m_canHealthLastKey;
    QCheckBox      *m_compressionEnable = nullptr;
    QComboBox      *m_compressionChannel = nullptr;
    QComboBox      *m_compressionSignal = nullptr;
    QDoubleSpinBox *m_compressionFlag = nullptr;
    QTimer         *m_engineTimer = nullptr;
    quint64         m_engineRev = 0;
    QVector<double> m_engineLastKey;

    // Measure page — one exclusive channel selector (which channel's own
    // independent measurement set the checkboxes below are currently
    // showing/editing), not a per-channel list like Rulers': six checkboxes
    // per channel would be a lot of simultaneous UI, so only one channel's
    // is visible at a time.
    QButtonGroup   *m_measureChannelGroup = nullptr;
    std::array<QPushButton *, 4> m_measureChannelSegments{};
    QPushButton    *m_measureMathSegment = nullptr;
    int             m_measureChannel = 0;   // ScopeView::RulersMathChannel selects Math
    std::array<QCheckBox *, 11> m_measureChecks{};   // mirrors ScopeView::MeasurementCount
    QPushButton    *m_mathColour  = nullptr;
    QWidget        *m_mathSourceBRow = nullptr;   // hidden for one-source operations
    QWidget        *m_mathScaleRow = nullptr;     // k and c, for k.A + c
    QDoubleSpinBox *m_mathScaleK = nullptr;
    QDoubleSpinBox *m_mathScaleC = nullptr;
    QComboBox      *m_mathFilterType = nullptr;   // as the channels' Filter
    QDoubleSpinBox *m_mathFilterLowHz = nullptr;
    QDoubleSpinBox *m_mathFilterHighHz = nullptr;
    QLabel         *m_mathFilterLowLabel = nullptr;
    QLabel         *m_mathFilterHighLabel = nullptr;
    QWidget        *m_mathFilterLowRow = nullptr;
    QWidget        *m_mathFilterHighRow = nullptr;
    CardButton     *m_mathCard    = nullptr;

    // References page. "Add" row buttons are built once for MaxChannels and
    // shown/hidden per active channel, same as m_channelCard[] itself.
    // "Manage" row is one toggle button per slot (ScopeView::MaxReferences),
    // showing whichever's currently selected for editing below — there are
    // never more than two, so a full list widget would be overhead for
    // nothing a couple of toggle buttons do not already say.
    std::array<QPushButton *, 4> m_referenceAddButtons{};
    std::array<QPushButton *, 2> m_referenceSlotButtons{};
    QPushButton    *m_referenceFileButton = nullptr;
    QPushButton    *m_referenceSaveButton = nullptr;
    QPushButton    *m_referenceRemoveButton = nullptr;
    QLineEdit      *m_referenceNameEdit = nullptr;
    QPushButton    *m_referenceColourButton = nullptr;
    QDoubleSpinBox *m_referenceVoltsPerDiv = nullptr;
    QDoubleSpinBox *m_referencePositionPct = nullptr;
    QDoubleSpinBox *m_referenceDelay = nullptr;
    // Which slot the Manage controls below are currently showing/editing —
    // not necessarily the most recently added one, since clicking either
    // toggle button switches which slot's fields are being edited.
    int             m_referenceSelectedSlot = 0;

    Page m_currentPage    = PageScope;
    int  m_currentChannel = 0;
    bool m_panelOpen      = false;
    bool m_running        = false;
    bool m_waitingTrigger = false;
    bool m_inChannelApply = false;
    int  m_acquisitionMode = ModeCapture;
    // Set by openNativeWaveform() for the rest of its own restoration and
    // left set afterward — reconfiguring a channel's mode/range through its
    // own normal widget signals, which is deliberate (see that function's
    // own comment on why: it's what makes Run afterward acquire fresh data
    // under the exact restored settings), also reconfigures the real
    // hardware, and that can provoke an AcquisitionManager::samplesReady
    // the device sends on its own timing rather than synchronously within
    // the call that triggered it. Left unguarded, that stale/empty frame
    // arriving after the loaded samples were already injected is exactly
    // what overwrote them with flat lines. Cleared by startAcquisition()
    // instead of a timer: acquisition stays stopped after a load, so
    // nothing should legitimately produce a frame until a new one is
    // explicitly requested, and that request is a well-defined moment to
    // resume normal processing rather than a guess at how long hardware
    // reconfiguration might still take to settle. See the samplesReady
    // handler in wireUp() for where this is actually checked.
    bool m_suppressSamplesReady = false;

    // Protocol decoding (see buildDecodingPage()).
    struct DecodeSlotUi {
        QCheckBox      *enable = nullptr;
        QComboBox      *protocol = nullptr;
        QComboBox      *input = nullptr;
        QDoubleSpinBox *threshold = nullptr;
        QCheckBox      *invert = nullptr;
        QWidget        *canBox = nullptr;
        QComboBox      *canRate = nullptr;
        QComboBox      *canFdRate = nullptr;
        QDoubleSpinBox *samplePoint = nullptr;
        QWidget        *linBox = nullptr;
        QComboBox      *linBaud = nullptr;
        QComboBox      *linVersion = nullptr;
        QWidget        *klineBox = nullptr;
        QComboBox      *klineBaud = nullptr;
        QComboBox      *klineFraming = nullptr;
        QWidget        *sentBox = nullptr;
        QComboBox      *sentTick = nullptr;
        QComboBox      *sentNibbles = nullptr;
        QComboBox      *sentMode = nullptr;      // fast / slow / SPC
        QWidget        *diagBox = nullptr;
        QComboBox      *diagIds = nullptr;       // which CAN IDs carry diagnostics
    };
    std::array<DecodeSlotUi, 2> m_decodeUi{};
    QThread       *m_decodeThread = nullptr;
    DecoderEngine *m_decoder = nullptr;
    QTimer        *m_decodeTimer = nullptr;
    quint64        m_decodeJobCounter = 0;
    std::array<quint64, 2> m_decodeRev{};         // bumped on any settings change
    std::array<quint64, 2> m_decodeInFlightId{};
    std::array<bool, 2>    m_decodeInFlight{};
    std::array<QString, 2> m_decodeRateWarning{};   // last sample-rate warning shown
    std::array<bool, 2>    m_decodeRowShown{};
    std::array<double, 2>  m_decodeIndexScale{};  // decoded sample -> view index
    std::array<double, 2>  m_decodeLead{};        // samples decoded before the window (Record, CAN)
    // Decoded blocks in their own positions (Record: recording samples;
    // otherwise the buffer's), placed on the current page by
    // projectBusRow() whenever the page changes — they follow the trace at
    // once while dragging instead of waiting for the next decode, and a
    // result that lands after the view moved goes where it belongs.
    struct BusBlock {
        double      start = 0.0, end = 0.0;
        QStringList texts;
        bool        error = false;
        QString     group;
    };
    struct BusRowData {
        bool    valid = false;
        QString label, message;
        QColor  colour;
        QVector<BusBlock> blocks;
    };
    std::array<BusRowData, 2> m_busRows{};
    std::array<double, 2>     m_decodeOrigin{};   // the job's sample 0, in those positions
    void projectBusRow(int slot);
    QString                m_canMessage;          // why no CAN frames are shown, when decoding can't run
    std::array<QVector<double>, 2> m_decodeLastKey;
    // True while Live mode is showing a .vsxwave loaded from disk rather than
    // the stream's own ring buffer. Every Live navigation path (drag,
    // navigator click, box zoom, refreshLiveView() itself) normally re-reads
    // a window from that ring, which does not contain the loaded data; with
    // this set they treat the loaded buffer the way Capture treats its own
    // static capture instead. Cleared only when Live itself starts acquiring.
    bool   m_liveLoaded = false;
    // The loaded Live buffer's own time axis and pan position, re-applied
    // when Live mode is switched back into — the regular Live restore
    // assumes a single ring-buffer window, which a loaded buffer is not.
    bool   m_liveLoadedHasOrigin = false;
    double m_liveLoadedOrigin = 0.0;
    double m_liveLoadedCentre = 0.5;
    qint64 m_lastFrameMs  = 0;
    double m_framesPerSec = 0.0;
    int  m_channelCount   = 0;
};
