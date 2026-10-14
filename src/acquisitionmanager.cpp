#include "acquisitionmanager.h"
#include "perftrace.h"
#include <cmath>
#include <future>
#include <cstdio>
#include <QElapsedTimer>

#include <QByteArray>
#include <QDebug>
#include <QMetaObject>
#include <QScopeGuard>
#include <QStringList>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>

namespace {
// How often a pending capture is checked. 5 ms: IsDataReady() measured at
// 0.0 ms a call, so polling often costs nothing and a finished capture is
// noticed within 5 ms instead of up to 20.
constexpr int kPollIntervalMs = 5;
constexpr qint64 kDrainBudgetMs = 1200;
constexpr int kMaxChannels    = 4;
}

// VIMU_CAPTURE_MASK=0x21 forces one literal mask for every capture, so any
// encoding can be tried without rebuilding. 0 or unset means "follow the
// enabled channels".
unsigned short AcquisitionManager::maskOverride()
{
    static const unsigned short value = [] {
        const QByteArray raw = qgetenv("VIMU_CAPTURE_MASK");
        if (raw.isEmpty())
            return static_cast<unsigned short>(0);
        bool ok = false;
        const uint parsed = QString::fromLatin1(raw).toUInt(&ok, 0);
        return ok ? static_cast<unsigned short>(parsed) : static_cast<unsigned short>(0);
    }();
    return value;
}

unsigned short AcquisitionManager::activeMask()
{
    if (const unsigned short forced = maskOverride())
        return forced;
    return m_captureMask;
}

AcquisitionManager::AcquisitionManager(QObject *parent)
    : QObject(parent)
{
    m_timer = new QTimer(this);
    m_timer->setInterval(kPollIntervalMs);
    connect(m_timer, &QTimer::timeout, this, &AcquisitionManager::poll);
}

AcquisitionManager::~AcquisitionManager()
{
    blockSignals(true);   // no status emissions into half-destroyed widgets
    shutdown();
}

// ---------------------------------------------------------------------------
// Session lifetime
// ---------------------------------------------------------------------------

namespace {
qint64 nowNs()
{
    static QElapsedTimer clock = [] { QElapsedTimer c; c.start(); return c; }();
    return clock.nsecsElapsed();
}

// InitDll() started early (see startSdkEarly()); collected by initialize().
std::future<bool> g_earlyInit;

unsigned int sdkLogFlag() { return qEnvironmentVariableIntValue("VIMU_LOG") ? 1u : 0u; }
unsigned int sdkWatchdogFlag() { return qEnvironmentVariableIntValue("VIMU_WATCHDOG") ? 1u : 0u; }
}

void AcquisitionManager::startSdkEarly()
{
#ifdef Q_OS_LINUX
    if (g_earlyInit.valid())
        return;
    const unsigned int log = sdkLogFlag(), watchdog = sdkWatchdogFlag();
    g_earlyInit = std::async(std::launch::async, [log, watchdog] {
        QElapsedTimer clock;
        clock.start();
        const bool ok = InitDll(log, watchdog) != 0;
        if (PerfTrace::enabled())
            std::fprintf(stderr, "[startup] InitDll %lld ms (in the background)\n",
                     static_cast<long long>(clock.elapsed()));
        return ok;
    });
#endif
}

bool AcquisitionManager::initialize()
{
    VSX_TRACE("acq.initialize");
    if (m_sdkReady)
        return true;
    QElapsedTimer clock;
    clock.start();

    // en_hard_watchdog=1 makes the device reset itself when a transfer stalls,
    // which shows up as a hotplug remove/arrive pair and looks like a crash.
    // Off by default; VIMU_WATCHDOG=1 restores the SDK's original behaviour.
    // VIMU_LOG=1 turns on the SDK's own logging.
    // Started early (Linux): wait for it — usually already done.
    bool initOk;
    if (g_earlyInit.valid()) {
        initOk = g_earlyInit.get();
        if (PerfTrace::enabled())
            std::fprintf(stderr, "[startup] waited %lld ms for InitDll\n",
                     static_cast<long long>(clock.elapsed()));
    } else {
        initOk = InitDll(sdkLogFlag(), sdkWatchdogFlag()) != 0;
        if (PerfTrace::enabled())
            std::fprintf(stderr, "[startup] InitDll %lld ms\n", static_cast<long long>(clock.elapsed()));
    }
    if (!initOk) {
        emit status(QStringLiteral("InitDll() failed — the SDK could not open the USB backend."));
        return false;
    }
    m_sdkReady = true;

    SetDevNoticeCallBack(this, &AcquisitionManager::onDeviceAdded,
                               &AcquisitionManager::onDeviceRemoved);
    SetDataReadyCallBack(this, &AcquisitionManager::onDataReady);

    ScanDevice();

    // Do not rely on the add-callback firing for a device that was already
    // plugged in when we started: enumerate explicitly as well.
    if (!ready()) {
        const int count = GetAvailableDevCount();
        for (int i = 0; i < count && !ready(); ++i)
            handleDeviceAdded(GetOnlyId(static_cast<unsigned int>(i)));
    }

    if (!ready())
        emit status(QStringLiteral("SDK ready — no device found. Plug the scope in."));
    if (PerfTrace::enabled())
        std::fprintf(stderr, "[startup] SDK ready, scope %s, %lld ms in initialize()\n",
                 ready() ? "found" : "not found", static_cast<long long>(clock.elapsed()));
    std::fflush(stderr);
    return true;
}

void AcquisitionManager::shutdown()
{
    VSX_TRACE("acq.shutdown");
    if (!m_sdkReady)
        return;

    if (m_running)
        stop();

    // Detach callbacks before tearing the session down, so nothing can call
    // back into a half-destroyed object.
    SetDevNoticeCallBack(nullptr, nullptr, nullptr);
    SetDataReadyCallBack(nullptr, nullptr);

    if (ready())
        CloseDevice(m_devId);

    FinishDll();

    m_devId    = InvalidDevice;
    m_channels = 0;
    m_sdkReady = false;
}

// ---------------------------------------------------------------------------
// SDK callbacks (foreign thread — marshal only)
// ---------------------------------------------------------------------------

void CALLBACK AcquisitionManager::onDeviceAdded(void *context, unsigned int devId)
{
    auto *self = static_cast<AcquisitionManager *>(context);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self, devId] { self->handleDeviceAdded(devId); },
                              Qt::QueuedConnection);
}

void CALLBACK AcquisitionManager::onDeviceRemoved(void *context, unsigned int devId)
{
    auto *self = static_cast<AcquisitionManager *>(context);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self, devId] { self->handleDeviceRemoved(devId); },
                              Qt::QueuedConnection);
}

void CALLBACK AcquisitionManager::onDataReady(void *context, unsigned int devId)
{
    auto *self = static_cast<AcquisitionManager *>(context);
    if (!self)
        return;
    QMetaObject::invokeMethod(self, [self, devId] {
        if (self->m_devId == devId)
            self->poll();
    }, Qt::QueuedConnection);
}

// ---------------------------------------------------------------------------
// Device setup
// ---------------------------------------------------------------------------

void AcquisitionManager::handleDeviceAdded(unsigned int devId)
{
    VSX_TRACE("acq.handleDeviceAdded");
    if (ready() || devId == InvalidDevice)
        return;
    if (!IsDevAvailable(devId))
        return;

    m_devId    = devId;
    m_channels = std::clamp(GetOscChannelNum(devId), 1, kMaxChannels);
    m_sampleRate = GetOscSampleRate(devId);

    m_rates.clear();
    const int rateCount = GetOscSupportSampleRateNum(devId);
    if (rateCount > 0) {
        m_rates.resize(rateCount);
        const int got = GetOscSupportSampleRates(devId, m_rates.data(), rateCount);
        m_rates.resize(got > 0 ? got : 0);
        std::sort(m_rates.begin(), m_rates.end());
    }

    m_rangeMinMv = GetOscRangeMinmV(devId);
    m_rangeMaxMv = GetOscRangeMaxmV(devId);

    const unsigned int memoryKb = GetMemoryLength(devId);
    if (memoryKb > 0 && m_captureKb > static_cast<int>(memoryKb))
        m_captureKb = static_cast<int>(memoryKb);

    applyDefaults();

    char description[256] = {0};
    GetDeviceDesString(devId, description, static_cast<int>(sizeof(description) - 1));

    logDeviceInfo(devId);

    emit deviceChanged(true, m_channels);
    m_adcBits = GetOscAdcBit(devId);
    emit status(QStringLiteral("Connected: %1 (id %2, %3 ch, %4-bit ADC, %5 KB memory)")
                    .arg(description[0] ? QString::fromLatin1(description)
                                        : QStringLiteral("Vimu MSO"))
                    .arg(devId)
                    .arg(m_channels)
                    .arg(GetOscAdcBit(devId))
                    .arg(memoryKb));
}

void AcquisitionManager::handleDeviceRemoved(unsigned int devId)
{
    VSX_TRACE("acq.handleDeviceRemoved");
    if (m_devId != devId)
        return;

    m_running = false;
    m_armed   = false;
    m_timer->stop();
    m_devId    = InvalidDevice;
    m_channels = 0;
    m_armFailures = 0;

    emit deviceChanged(false, 0);
    emit status(QStringLiteral("Device disconnected."));
}

void AcquisitionManager::applyDefaults()
{
    VSX_TRACE("acq.applyDefaults");
    if (!ready())
        return;

    SetOscCaptureMode(m_devId, 0);   // block mode, not streaming

    for (int ch = 0; ch < m_channels; ++ch) {
        // As last asked (DC until then) — this runs on every (re)connection
        // and used to force DC, losing an AC setting.
        SetAcDc(m_devId, static_cast<unsigned int>(ch),
                ch < 4 && m_acCoupling[size_t(ch)] ? 1 : 0);
        SetOscChannelRangemV(m_devId, ch, -1000, 1000);
    }

    SetTriggerMode(m_devId, 0 /* TRIGGER_MODE_AUTO */);
    SetTriggerStyle(m_devId, m_triggerStyle);
    SetTriggerSource(m_devId, m_triggerSrc);
    SetTriggerLevelmV(m_devId, m_triggerMv, m_triggerSense);
    if (IsSupportPreTriggerPercent(m_devId))
        SetPreTriggerPercent(m_devId, m_preTrigger);
}

// ---------------------------------------------------------------------------
// Acquisition
// ---------------------------------------------------------------------------

// Continuous Auto only: Normal waits for a real edge, Single for a real
// trigger event (as before: the scope's own timeout then ends the wait).
void AcquisitionManager::maybeForceAuto()
{
    if (m_autoWaitMs < 0 || m_forcedThisArm || m_requireTrigger || m_singleShot || m_draining
        || m_reconfigure || !triggered() || !m_armed || !m_sinceArm.isValid() || m_sampleRate == 0)
        return;
    // Not before the pre-trigger part of the buffer has filled (a trigger
    // earlier than that has no history to show), plus a little margin.
    const double fillMs = double(m_capacity) / double(m_sampleRate) * 1000.0;
    const double dueMs = fillMs * m_preFraction + 10.0 + double(m_autoWaitMs);
    if (double(m_sinceArm.elapsed()) < dueMs || !IsSupportTriggerForce(m_devId))
        return;
    m_forcedThisArm = true;
    ++m_stats.forced;
    TriggerForce(m_devId);
}

void AcquisitionManager::noteArmDone(qint64 startNs)
{
    const qint64 now = nowNs();
    m_stats.armMs += double(now - startNs) / 1e6;
    if (!m_statsClock.isValid()) {
        m_statsClock.start();
        return;
    }
    const qint64 elapsed = m_statsClock.elapsed();
    if (elapsed < 1000)
        return;
    const double seconds = double(elapsed) / 1000.0;
    const int n = std::max(1, m_stats.frames);
    const double fill = m_sampleRate > 0 ? double(m_capacity) / double(m_sampleRate) * 1000.0 : 0.0;
    std::fprintf(stderr,
                 "[capture] %d frames in %.1f s = %.1f fps | per frame: scope wait %.0f ms (filling %u samples "
                 "at %u S/s takes %.0f ms), read %.0f ms, app after data %.0f ms, arm %.0f ms | "
                 "IsDataReady %d calls, %.1f ms each | %d dropped (no edge), %d forced after %d ms wait | trigger %s\n",
                 m_stats.frames, seconds, double(m_stats.frames) / seconds, m_stats.waitMs / n, m_capacity,
                 m_sampleRate, fill, m_stats.readMs / n, m_stats.afterMs / n, m_stats.armMs / n,
                 m_stats.readyCalls, m_stats.readyCalls ? m_stats.readyMs / m_stats.readyCalls : 0.0,
                 m_stats.discarded, m_stats.forced, m_autoWaitMs, m_requireTrigger ? "Normal" : "Auto");
    std::fflush(stderr);
    m_stats = CaptureStats();
    m_statsClock.start();
}

void AcquisitionManager::arm()
{
    VSX_TRACE("acq.arm");
    const qint64 armStartNs = PerfTrace::enabled() ? nowNs() : 0;
    if (PerfTrace::enabled() && m_tReadyNs) {
        m_stats.afterMs += double(armStartNs - m_tReadyNs) / 1e6;   // data ready -> next arm
        m_tReadyNs = 0;
    }
    const auto armGuard = qScopeGuard([this, armStartNs] {
        if (PerfTrace::enabled())
            noteArmDone(armStartNs);
    });
    m_forcedThisArm = false;
    if (!ready())
        return;

    unsigned short mask = activeMask();

    // maskprobe on an MSO21AL Plus: a mask bit for a channel the device does
    // not have makes libvmmultmso raise SIGFPE rather than return an error.
    // Never let a bit outside the device's channel count reach Capture().
    if (!maskOverride()) {
        unsigned short valid = 0;
        for (int ch = 0; ch < m_channels && ch < kMaxChannels; ++ch)
            valid = static_cast<unsigned short>(valid | maskBit(ch));
        mask = static_cast<unsigned short>(mask & valid);

        // The trigger source has to be in the capture, otherwise the hardware
        // has nothing to find an edge on and waits forever.
        if (triggered() && m_triggerSrc < unsigned(m_channels)
            && m_triggerSrc < unsigned(kMaxChannels)) {
            mask = static_cast<unsigned short>(mask | maskBit(int(m_triggerSrc)));
        }
        if (mask == 0)
            mask = maskBit(0);
    }

    // The vendor demo re-applies the trigger settings immediately before every
    // Capture() rather than setting them once. Do the same: it removes any
    // question of whether a change reached the hardware in time.
    SetTriggerStyle(m_devId, m_triggerStyle);
    SetTriggerMode(m_devId, 0 /* TRIGGER_MODE_AUTO */);
    SetTriggerSource(m_devId, m_triggerSrc);
    SetTriggerLevelmV(m_devId, m_triggerMv, m_triggerSense);
    const bool preSupported = IsSupportPreTriggerPercent(m_devId);
    if (preSupported)
        SetPreTriggerPercent(m_devId, m_preTrigger);
    m_preFraction = preSupported ? std::clamp(m_preTrigger, 5, 95) / 100.0 : 0.5;

    // The 8 KB triggered-depth limit came from vimu_driver.py, which is not part
    // of the vendor SDK and was written while fighting the LIANXU bug we no
    // longer trigger. So the requested depth is used as-is, and handleStall()
    // drops back to 8 KB if a deep triggered capture actually does wedge.
    const int requestedKb = m_captureKb;

    const int kb = Capture(m_devId, requestedKb, mask, 1);
    if (kb <= 0) {
        m_armed = false;
        ++m_armFailures;

        // A refusal usually means the device went away — unplugging the USB
        // cable is the common case — so check before blaming the request.
        if (!IsDevAvailable(m_devId)) {
            emit status(QStringLiteral("Device disconnected."));
            handleDeviceRemoved(m_devId);
            return;
        }

        // Otherwise retry a few times: an isolated refusal is usually
        // transient. Retries are paced by poll(), not spun here.
        if (m_armFailures >= 6) {
            finish(QStringLiteral(
                "Capture refused %1 KB on mask 0x%2 six times — stopping. "
                "Check the USB connection.")
                    .arg(requestedKb).arg(mask, 4, 16, QLatin1Char('0')));
        }
        m_sinceArm.start();
        return;
    }
    m_armFailures = 0;

    m_captureKb = kb;

    // The SDK reports capture length in KB of raw ADC memory. One sample is one
    // byte on an 8-bit ADC and two on a 12-bit one, and the header does not say
    // whether the length is per channel or shared. Sizing the buffer at one
    // sample per byte is therefore the safe upper bound; ReadVoltageDatas()
    // reports how many samples it actually wrote.
    // vimu_driver.py, cross-checked against GetMemoryLength(): one KB of
    // capture memory holds 1024 samples.
    m_capacity  = static_cast<unsigned int>(kb) * 1024u;
    m_armedMask = mask;            // poll() must read what THIS capture holds
    m_armed     = true;

    // How long this capture should plausibly take, plus room for USB latency.
    const double seconds = m_sampleRate > 0
        ? double(m_capacity) / double(m_sampleRate)
        : 0.0;
    m_budgetMs = std::max<qint64>(750, qint64(seconds * 1000.0 * 3.0) + 500);
    m_sinceArm.start();

    // Read the settings back once per configuration change: the only reliable
    // way to know the hardware accepted what we asked for.
    if (m_logArm) {
        m_logArm = false;
        qInfo("vimu arm: mask=0x%04x kb=%d rate=%u | style=0x%04x mode=%u src=%u "
              "level=%dmV sense=%dmV pre=%d%%",
              mask, kb, m_sampleRate,
              GetTriggerStyle(m_devId), GetTriggerMode(m_devId),
              GetTriggerSource(m_devId), GetTriggerLevelmV(m_devId),
              GetTriggerSensemV(m_devId), GetPreTriggerPercent(m_devId));
    }
}

void AcquisitionManager::poll()
{
    VSX_TRACE("acq.poll");
    if (!ready() || m_recovering)
        return;

    // Nothing is armed but we are meant to be running: a previous Capture() was
    // refused. Retry, paced so a persistent refusal cannot spin.
    if (!m_armed) {
        if (m_running && !m_reconfigure
            && (!m_sinceArm.isValid() || m_sinceArm.elapsed() > 200)) {
            arm();
        }
        return;
    }

    bool dataReady;
    if (PerfTrace::enabled()) {
        const qint64 t0 = nowNs();
        dataReady = IsDataReady(m_devId) != 0;
        m_stats.readyMs += double(nowNs() - t0) / 1e6;
        ++m_stats.readyCalls;
    } else {
        dataReady = IsDataReady(m_devId) != 0;
    }
    if (!dataReady) {
        maybeForceAuto();
        if (!m_sinceArm.isValid() || m_sinceArm.elapsed() <= m_budgetMs)
            return;

        // A drain that does not land has to be escapable, or a threshold the
        // signal never reaches locks the application permanently. AbortCapture
        // is the SDK guide's documented fallback for exactly this case.
        if (m_draining && m_sinceArm.elapsed() > kDrainBudgetMs) {
            qWarning("vimu: AUTO drain did not complete in %lld ms; aborting capture",
                     qint64(kDrainBudgetMs));
            AbortCapture(m_devId);
            applyPendingTrigger();
            return;
        }

        // The device is always in TRIGGER_MODE_AUTO, so it sweeps whether or
        // not an edge arrives. A capture that does not complete therefore means
        // the device is wedged, in every mode — there is no longer such a thing
        // as a legitimate indefinite hardware wait. (Waiting for an edge is now
        // detected after the data arrives, by verifyEdge().)
        if (m_draining)
            return;
        handleStall();
        return;
    }
    m_waiting = false;

    m_armed  = false;
    m_stalls = 0;
    if (m_waiting) {
        m_waiting = false;
        emit triggerWaiting(false);
    }

    // This capture only completed so that the pending reconfiguration could be
    // applied cleanly. Its data belongs to the old settings; drop it.
    if (m_reconfigure) {
        applyPendingTrigger();
        return;
    }
    if (PerfTrace::enabled()) {
        m_tReadStartNs = nowNs();
        m_tReadyNs = m_tReadStartNs;
        m_stats.waitMs += double(m_sinceArm.isValid() ? m_sinceArm.elapsed() : 0);
        ++m_stats.frames;
    }
    const double triggerPoint = ReadVoltageDatasTriggerPoint(m_devId);

    // Read every requested channel before publishing any of them: in Normal
    // mode the source channel decides whether this frame is shown at all.
    QVector<double> captured[kMaxChannels];
    bool over[kMaxChannels] = {};

    for (int ch = 0; ch < m_channels && ch < kMaxChannels; ++ch) {
        // Deliberately m_armedMask, not m_captureMask: if the user toggled a
        // channel after this capture was armed, the new channel has no data in
        // it and asking the SDK for it is what crashed.
        //
        // With a literal VIMU_CAPTURE_MASK the bit-to-channel mapping is
        // exactly what is in question, so read every channel and let the
        // caller see which ones came back with data.
        if (!maskOverride() && !(m_armedMask & maskBit(ch)))
            continue;

        QVector<double> samples(static_cast<int>(m_capacity));
        unsigned int read = ReadVoltageDatas(m_devId, static_cast<char>(ch),
                                             samples.data(), m_capacity);
        if (read == 0)
            continue;
        if (read > m_capacity)
            read = m_capacity;          // never trust a length back into a resize

        samples.resize(static_cast<int>(read));
        over[ch] = IsVoltageDatasOutRange(m_devId, static_cast<char>(ch)) != 0;
        captured[ch] = samples;
        if (ch < 4) {
            // The conversion the SDK used for this very frame: the window the
            // ADC actually covered (see adcwindow.h).
            AdcWindow &w = m_adcWindow[size_t(ch)];
            w.bits = m_adcBits > 0 ? m_adcBits : 12;
            w.zoom = ReadADCToVoltageZoom(m_devId, static_cast<char>(ch));
            w.bias = ReadADCToVoltageBias(m_devId, static_cast<char>(ch));
            w.sdkResolution = GetVoltageResolution(m_devId, static_cast<char>(ch));
            w.complete();
        }
        ++m_frameSerial;
    }

    // Normal mode: hold the frame unless the edge is really there. The hardware
    // is in AUTO, so it sweeps regardless; this is what turns that back into
    // Normal behaviour.
    if (PerfTrace::enabled())
        m_stats.readMs += double(nowNs() - m_tReadStartNs) / 1e6;
    if (m_requireTrigger && triggered()) {
        const int src = int(m_triggerSrc);
        const bool ok = src >= 0 && src < kMaxChannels
                        && !captured[src].isEmpty()
                        && verifyEdge(captured[src], triggerPoint);
        if (!ok) {
            ++m_skipped;
            ++m_stats.discarded;
            if (m_skipped > 20 && !m_waiting) {
                m_waiting = true;
                emit triggerWaiting(true);
                emit status(QStringLiteral(
                    "Waiting for trigger — no edge at this threshold on CH%1.")
                        .arg(src + 1));
            }
            if (m_running && !m_singleShot) {
                arm();
                return;
            }
        } else if (m_skipped > 0 || m_waiting) {
            m_skipped = 0;
            if (m_waiting) {
                m_waiting = false;
                emit triggerWaiting(false);
                emit status(QStringLiteral("Triggered"));
            }
        }
    }

    for (int ch = 0; ch < m_channels && ch < kMaxChannels; ++ch) {
        if (captured[ch].isEmpty())
            continue;
        // Zero calibration, after the trigger check (which works on what the
        // hardware saw).
        if (ch < 4 && m_zero[size_t(ch)] != 0.0) {
            for (double &v : captured[ch])
                v -= m_zero[size_t(ch)];
        }
        emit channelOverRange(ch, over[ch]);
        emit samplesReady(ch, captured[ch], static_cast<double>(m_sampleRate), triggerPoint);
    }

    if (m_singleShot)
        finish(QStringLiteral("SINGLE complete"));
    else if (m_running)
        arm();
}

void AcquisitionManager::start()
{
    VSX_TRACE("acq.start");
    if (!ready() || m_running)
        return;
    m_armFailures = 0;
    m_logArm     = true;
    m_running    = true;
    m_singleShot = false;
    arm();
    m_timer->start();
    emit status(QStringLiteral("RUN"));
}

void AcquisitionManager::single()
{
    VSX_TRACE("acq.single");
    if (!ready() || m_running)
        return;
    m_running    = true;
    m_singleShot = true;
    arm();
    m_timer->start();
    emit status(QStringLiteral("SINGLE armed"));
}

void AcquisitionManager::stop()
{
    VSX_TRACE("acq.stop");
    // The device is always in AUTO, so a pending capture completes on its own
    // and there is nothing to abort.
    m_armed = false;
    finish(QStringLiteral("STOP"));
}

void AcquisitionManager::finish(const QString &reason)
{
    VSX_TRACE("acq.finish");
    m_running    = false;
    m_singleShot = false;
    m_armed      = false;
    if (m_timer)
        m_timer->stop();
    emit status(reason);
}

// ---------------------------------------------------------------------------
// Stall handling
//
// vimu_driver.py: "A wedged device fails EVERY capture, free run included, and
// only a full FinishDll/InitDll restart clears it." So a stalled capture is not
// something to retry — the whole SDK session has to be torn down and rebuilt,
// or every later attempt fails for reasons unrelated to whatever caused it.
// ---------------------------------------------------------------------------

void AcquisitionManager::handleStall()
{
    VSX_TRACE("acq.handleStall");
    m_armed = false;
    ++m_stalls;

    // If a deep triggered capture is what wedged us, the 8 KB folklore was real
    // after all. Back off once and say so, rather than looping on it.
    if (triggered() && m_captureKb > 8) {
        emit status(QStringLiteral(
            "Capture stalled at %1 KB with a trigger armed — falling back to 8 KB.")
                .arg(m_captureKb));
        qWarning("vimu: triggered capture stalled at %d KB; capping depth at 8 KB",
                 m_captureKb);
        m_captureKb = 8;
    }

    emit status(QStringLiteral("No data after %1 ms on mask 0x%2 (%3 KB) — resetting the SDK session.")
                    .arg(m_budgetMs)
                    .arg(m_armedMask, 4, 16, QLatin1Char('0'))
                    .arg(m_captureKb));
    qWarning("vimu: capture stalled; mask=0x%04x kb=%d rate=%u stalls=%d",
             m_armedMask, m_captureKb, m_sampleRate, m_stalls);

    const bool wasRunning = m_running;
    const bool wasSingle  = m_singleShot;
    finish(QStringLiteral("Recovering..."));

    if (!recoverSdk()) {
        emit status(QStringLiteral("SDK reset failed — unplug and replug the scope."));
        return;
    }

    if (m_stalls >= 3) {
        emit status(QStringLiteral(
            "Capture keeps stalling on mask 0x%1. Try fewer channels or a "
            "smaller capture.").arg(m_armedMask, 4, 16, QLatin1Char('0')));
        return;
    }

    if (wasRunning) {
        QTimer::singleShot(250, this, [this, wasSingle] {
            if (!ready())
                return;
            if (wasSingle)
                single();
            else
                start();
        });
    }
}

bool AcquisitionManager::recoverSdk()
{
    VSX_TRACE("acq.recoverSdk");
    if (m_recovering)
        return false;
    m_recovering = true;

    SetDevNoticeCallBack(nullptr, nullptr, nullptr);
    SetDataReadyCallBack(nullptr, nullptr);
    if (ready())
        CloseDevice(m_devId);
    FinishDll();

    m_devId    = InvalidDevice;
    m_channels = 0;
    m_sdkReady = false;

    const bool ok = initialize();
    m_recovering = false;
    return ok && ready();
}

void AcquisitionManager::logDeviceInfo(unsigned int devId) const
{
    QStringList rates;
    for (unsigned int r : m_rates)
        rates << QString::number(r);

    qInfo().noquote() << QStringLiteral(
        "vimu: channels=%1 adcBits=%2 memoryKB=%3 hardTrigger=%4 preTrigger=%5\n"
        "vimu: rates=[%6]  maskOverride=0x%7")
        .arg(m_channels)
        .arg(GetOscAdcBit(devId))
        .arg(GetMemoryLength(devId))
        .arg(IsSupportHardTrigger(devId))
        .arg(IsSupportPreTriggerPercent(devId) ? 1 : 0)
        .arg(rates.join(QLatin1String(", ")))
        .arg(maskOverride(), 4, 16, QLatin1Char('0'));
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

void AcquisitionManager::setSampleRate(unsigned int rate)
{
    VSX_TRACE("acq.setSampleRate");
    if (!ready())
        return;
    const unsigned int actual = SetOscSampleRate(m_devId, rate);
    if (actual) {
        m_sampleRate = actual;
        emit status(QStringLiteral("Sample rate %1 S/s").arg(actual));
    }
}

// Settings are not applied by aborting and re-arming. arm() reads them at the
// start of the next acquisition, which happens within a frame anyway, so a
// change costs nothing and never disturbs a capture in flight.
void AcquisitionManager::setCaptureKb(int kb)
{
    VSX_TRACE("acq.setCaptureKb");
    m_captureKb = std::max(1, kb);
}

void AcquisitionManager::setChannelEnabled(int channel, bool enabled)
{
    VSX_TRACE("acq.setChannelEnabled");
    if (channel < 0 || channel >= kMaxChannels)
        return;
    if (ready() && channel >= m_channels)
        return;   // the device does not have this channel; see maskGuard() below

    if (enabled)
        m_captureMask |= maskBit(channel);
    else
        m_captureMask &= static_cast<unsigned short>(~maskBit(channel));

    if (m_captureMask == 0)
        m_captureMask = maskBit(0);   // the hardware needs at least one channel
}

void AcquisitionManager::setRangeMv(int channel, int minMv, int maxMv)
{
    VSX_TRACE("acq.setRangeMv");
    if (channel >= 0 && channel < 4) {
        m_adcWindow[size_t(channel)].requestedMinMv = minMv;
        m_adcWindow[size_t(channel)].requestedMaxMv = maxMv;
        m_adcWindow[size_t(channel)].sentMinMv = minMv;     // block mode doesn't retry
        m_adcWindow[size_t(channel)].sentMaxMv = maxMv;
    }
    if (ready() && channel >= 0 && channel < m_channels)
        SetOscChannelRangemV(m_devId, channel, minMv, maxMv);
}

bool AcquisitionManager::setAcCoupling(int channel, bool ac)
{
    VSX_TRACE("acq.setAcCoupling");
    if (channel >= 0 && channel < 4)
        m_acCoupling[size_t(channel)] = ac;
    if (!ready() || channel < 0 || channel >= m_channels)
        return true;
    // Sent whatever IsSupportAcDc() says: on this scope it answers "not
    // supported" although AC works (the vendor's app uses it), and gating on
    // it was why AC never reached the hardware. The read-back decides.
    SetAcDc(m_devId, static_cast<unsigned int>(channel), ac ? 1 : 0);
    return (GetAcDc(m_devId, static_cast<unsigned int>(channel)) != 0) == ac;
}

void AcquisitionManager::setTriggerStyle(unsigned int style)
{
    VSX_TRACE("acq.setTriggerStyle");
    m_triggerStyle = style;
    if (style == 0 && m_waiting) {
        m_waiting = false;
        emit triggerWaiting(false);
    }
    if (ready())
        SetTriggerStyle(m_devId, style);
}

// TRIGGER_MODE_LIANXU ("normal") does not work on this hardware — the vendor's
// own application fails to lock with it too, while AUTO works. So the device is
// always left in AUTO and Normal is reconstructed here: confirm that the source
// channel really crosses the threshold in the right direction near the reported
// trigger point, and hold the frame back when it does not.
void AcquisitionManager::setRequireTrigger(bool require)
{
    VSX_TRACE("acq.setRequireTrigger");
    m_requireTrigger = require;
    m_skipped = 0;
    if (!require && m_waiting) {
        m_waiting = false;
        emit triggerWaiting(false);
    }
}

bool AcquisitionManager::verifyEdge(const QVector<double> &samples,
                                    double triggerPoint) const
{
    VSX_TRACE("acq.verifyEdge");
    const int n = samples.size();
    if (n < 4)
        return false;

    const double level = m_triggerMv / 1000.0;      // samples are volts at the BNC
    const double hyst  = std::max(1e-4, m_triggerSense / 1000.0);
    const bool rising  = (m_triggerStyle & 0x0002u) == 0;   // FALL_EDGE is 0x0002

    // The SDK places the trigger in the middle of the capture, but not to the
    // sample. Search a window around it rather than a single point.
    int centre = int(triggerPoint);
    if (centre <= 0 || centre >= n)
        centre = n / 2;
    const int half  = std::max(32, n / 50);
    const int begin = std::clamp(centre - half, 1, n - 1);
    const int end   = std::clamp(centre + half, 1, n);

    for (int i = begin; i < end; ++i) {
        const double a = samples[i - 1];
        const double b = samples[i];
        if (rising) {
            if (a <= level && b > level + hyst)
                return true;
        } else {
            if (a >= level && b < level - hyst)
                return true;
        }
    }
    return false;
}

void AcquisitionManager::setTriggerSource(unsigned int source)
{
    VSX_TRACE("acq.setTriggerSource");
    m_triggerSrc = source;
    if (ready())
        SetTriggerSource(m_devId, source);
}

void AcquisitionManager::setTriggerLevelMv(int levelMv, int senseMv)
{
    VSX_TRACE("acq.setTriggerLevelMv");
    m_triggerMv    = levelMv;
    m_triggerSense = std::max(1, senseMv);
    if (ready()) {
        // The hardware compares against the uncorrected signal: shift by the
        // source's zero offset so the level means the same on the corrected trace.
        const double zero = m_triggerSrc < 4 ? m_zero[m_triggerSrc] : 0.0;
        SetTriggerLevelmV(m_devId, m_triggerMv + int(std::lround(zero * 1000.0)), m_triggerSense);
    }
}

void AcquisitionManager::setZeroOffset(int channel, double volts)
{
    if (channel < 0 || channel >= 4 || m_zero[size_t(channel)] == volts)
        return;
    m_zero[size_t(channel)] = volts;
    if (unsigned(channel) == m_triggerSrc)
        setTriggerLevelMv(m_triggerMv, m_triggerSense);
}

void AcquisitionManager::setPreTriggerPercent(int percent)
{
    VSX_TRACE("acq.setPreTriggerPercent");
    m_preTrigger = std::clamp(percent, 5, 95);   // documented range
    if (ready() && IsSupportPreTriggerPercent(m_devId))
        SetPreTriggerPercent(m_devId, m_preTrigger);
}

// A trigger setting only reaches the hardware through the next Capture(), and a
// LIANXU capture waiting for an edge that never comes has no next Capture().
//
// The SDK guide, section 7.5.7, is explicit about how to get out of that:
// switch the trigger mode to AUTO, wait for the data notification, and only
// then reconfigure. AbortCapture() is listed as the fallback, and in practice
// it wedges the device — the capture card audibly resets and every later
// capture fails. So this drains rather than aborts.
void AcquisitionManager::rearm()
{
    VSX_TRACE("acq.rearm");
    if (!ready() || !m_running)
        return;

    m_reconfigure = true;
    if (m_armed)
        drainPendingCapture();
    else
        applyPendingTrigger();
}

void AcquisitionManager::drainPendingCapture()
{
    VSX_TRACE("acq.drainPendingCapture");
    m_draining = true;

    // AUTO sweeps on its own timeout, so the in-flight capture completes and
    // poll() picks the data up on the next tick.
    SetTriggerMode(m_devId, 0 /* TRIGGER_MODE_AUTO */);
    m_sinceArm.start();
}

void AcquisitionManager::applyPendingTrigger()
{
    VSX_TRACE("acq.applyPendingTrigger");
    m_draining    = false;
    m_reconfigure = false;

    SetTriggerStyle(m_devId, m_triggerStyle);
    SetTriggerMode(m_devId, 0 /* TRIGGER_MODE_AUTO */);
    SetTriggerSource(m_devId, m_triggerSrc);
    SetTriggerLevelmV(m_devId, m_triggerMv, m_triggerSense);
    if (IsSupportPreTriggerPercent(m_devId))
        SetPreTriggerPercent(m_devId, m_preTrigger);

    if (m_waiting) {
        m_waiting = false;
        emit triggerWaiting(false);
    }
    m_logArm = true;
    if (m_running)
        arm();
}

void AcquisitionManager::forceTrigger()
{
    VSX_TRACE("acq.forceTrigger");
    if (ready() && IsSupportTriggerForce(m_devId))
        TriggerForce(m_devId);
}
