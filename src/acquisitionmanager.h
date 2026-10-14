#pragma once

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

// Must be included here: on non-Windows this header is what defines the empty
// CALLBACK / WINAPI macros used in the callback signatures below.
#include <VmMultMsoLib.h>

#include <QElapsedTimer>
#include <QObject>

#include <functional>

#include <array>
#include "adcwindow.h"
#include <QString>
#include <QVector>

class QTimer;

/*
 * Owns the Vimu SDK session.
 *
 * The SDK invokes its notification callbacks from its own internal thread, so
 * every callback here does nothing but marshal a queued call back onto the
 * thread that owns this object. Nothing touches Qt widgets from SDK context.
 */
class AcquisitionManager : public QObject {
    Q_OBJECT

public:
    static constexpr unsigned int InvalidDevice = 0xFFFFFFFFu;

    explicit AcquisitionManager(QObject *parent = nullptr);
    ~AcquisitionManager() override;

    bool initialize();
    // Starts the SDK's InitDll() on a background thread right at launch, so
    // it runs during the startup animation instead of after it (initialize()
    // then just collects the result). Linux only: there the USB layer runs
    // its own hotplug thread; on Windows device notifications can be tied to
    // the thread that registers them, so it stays where it was.
    static void startSdkEarly();
    void shutdown();

    bool ready() const { return m_devId != InvalidDevice; }
    unsigned int deviceId() const { return m_devId; }   // shared with StreamEngine
    int channels() const { return m_channels; }
    unsigned int sampleRate() const { return m_sampleRate; }
    int captureKb() const { return m_captureKb; }
    QVector<unsigned int> supportedRates() const { return m_rates; }
    int rangeMinMv() const { return m_rangeMinMv; }
    int rangeMaxMv() const { return m_rangeMaxMv; }
    bool triggered() const { return m_triggerStyle != 0; }

public slots:
    void start();
    void stop();
    void single();
    // The window the ADC digitised in the last frame (Capture), per channel.
    AdcWindow adcWindow(int channel) const
    {
        return channel >= 0 && channel < 4 ? m_adcWindow[size_t(channel)] : AdcWindow();
    }
    quint64 frameSerial() const { return m_frameSerial; }   // +1 per channel read
    void setSampleRate(unsigned int rate);
    void setCaptureKb(int kb);
    void setChannelEnabled(int channel, bool enabled);
    void setRangeMv(int channel, int minMv, int maxMv);
    // True when the hardware is now coupled as asked (read back with
    // GetAcDc). False when AC was asked for but the device can't switch
    // coupling in software, or didn't take it — the caller then removes DC
    // itself. Without a device there's nothing to confirm: true.
    bool setAcCoupling(int channel, bool ac);
    void setTriggerStyle(unsigned int style);    // TRIGGER_STYLE_* from the SDK header
    // Normal semantics are enforced in software: the hardware always runs in
    // TRIGGER_MODE_AUTO, and frames whose edge cannot be confirmed are held
    // back. See the comment on verifyEdge().
    void setRequireTrigger(bool require);
    void setTriggerSource(unsigned int source);
    void setTriggerLevelMv(int levelMv, int senseMv);
    // Zero calibration: subtracted from every sample of the channel (input
    // volts). The hardware trigger level follows it, so a trigger at 0 V
    // still fires where the corrected trace crosses 0 V.
    void setZeroOffset(int channel, double volts);
    void setAutoTriggerWaitMs(int ms) { m_autoWaitMs = ms; }
    void setPreTriggerPercent(int percent);
    void rearm();          // apply trigger changes to the capture in flight
    void forceTrigger();

signals:
    void deviceChanged(bool ready, int channels);
    void samplesReady(int channel, const QVector<double> &samples,
                      double sampleRate, double triggerPoint);
    void channelOverRange(int channel, bool over);
    void status(const QString &text);
    void triggerWaiting(bool waiting);

private:
    // Called on the SDK's thread. These only post work to the Qt thread.
    static void CALLBACK onDeviceAdded(void *context, unsigned int devId);
    static void CALLBACK onDeviceRemoved(void *context, unsigned int devId);
    static void CALLBACK onDataReady(void *context, unsigned int devId);

    void handleDeviceAdded(unsigned int devId);
    void handleDeviceRemoved(unsigned int devId);
    void applyDefaults();
    std::array<bool, 4> m_acCoupling{};   // as last asked, re-applied by applyDefaults()
    void arm();
    void poll();
    void finish(const QString &reason);
    void handleStall();
    void drainPendingCapture();
    void applyPendingTrigger();
    bool verifyEdge(const QVector<double> &samples, double triggerPoint) const;
    bool recoverSdk();
    void logDeviceInfo(unsigned int devId) const;
    unsigned short activeMask();

    // Settled with tools/maskprobe on an MSO21AL PLUS, SDK 1.24: one bit per
    // channel from bit 0. 0x01 returns CH1 only, 0x02 returns CH2 only, 0x03
    // returns both. The prose in VmMultMsoLib.h claiming ch2=0x20 is wrong;
    // its own worked examples are right. Logic is 0x0100.
    static unsigned short maskBit(int channel) {
        return static_cast<unsigned short>(1u << channel);
    }
    static unsigned short maskOverride();   // VIMU_CAPTURE_MASK, 0 if unset

    unsigned int   m_devId        = InvalidDevice;
    int            m_channels     = 0;
    int            m_adcBits      = 0;
    std::array<AdcWindow, 4> m_adcWindow{};
    quint64        m_frameSerial  = 0;
    unsigned int   m_sampleRate   = 1000000;
    int            m_captureKb    = 32;
    unsigned int   m_capacity     = 32u * 1024u;   // doubles per channel per frame
    unsigned int m_triggerStyle = 0;   // TRIGGER_STYLE_NONE
    bool         m_requireTrigger = false;
    int          m_skipped        = 0;
    unsigned int m_triggerSrc   = 0;
    std::array<double, 4> m_zero{};   // input volts, per channel
    // Capture timing summary, once a second (VSX_TRACE=1): where a frame's
    // time goes — the scope's own wait, the reads, the app's handling, the
    // re-arm, the IsDataReady polling, frames dropped for lacking an edge.
    struct CaptureStats {
        int    frames = 0, readyCalls = 0, discarded = 0;
        double waitMs = 0, readMs = 0, afterMs = 0, armMs = 0, readyMs = 0;
        int    forced = 0;
    };
    CaptureStats m_stats;
    QElapsedTimer m_statsClock;
    qint64 m_tReadyNs = 0, m_tReadStartNs = 0;
    void noteArmDone(qint64 startNs);
    // Auto-trigger wait imposed by the app. The scope's own AUTO timeout is
    // ~190 ms: with nothing crossing the trigger level every frame took
    // 275 ms (3.6 fps) where filling the buffer needs 82 ms (measured, 100
    // kS/s, 8 KB). In continuous Auto mode the capture is forced (TriggerForce)
    // once the pre-trigger part has filled and this many ms have passed
    // without a real trigger. Negative: leave it to the scope.
    void maybeForceAuto();
    int    m_autoWaitMs = -1;
    bool   m_forcedThisArm = false;
    double m_preFraction = 0.5;      // pre-trigger share of the buffer, as armed
    int          m_triggerMv    = 0;
    int          m_triggerSense = 50;
    int          m_preTrigger   = 50;
    int          m_rangeMinMv   = -10000;
    int          m_rangeMaxMv   =  10000;

    unsigned short m_captureMask  = 0x0001;
    unsigned short m_armedMask    = 0x0001;   // mask the in-flight capture used
    QVector<unsigned int> m_rates;

    bool    m_sdkReady   = false;
    bool    m_running    = false;
    bool    m_singleShot = false;
    bool    m_armed      = false;
    bool    m_recovering = false;
    bool    m_waiting    = false;
    bool    m_draining   = false;   // waiting out a capture before reconfiguring
    bool    m_reconfigure = false;
    bool    m_logArm      = true;
    int     m_stalls     = 0;
    int     m_armFailures = 0;
    QElapsedTimer m_sinceArm;
    qint64  m_budgetMs   = 1000;
    QTimer *m_timer      = nullptr;
};
