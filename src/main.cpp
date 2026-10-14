#include "mainwindow.h"
#include "acquisitionmanager.h"
#include "perftrace.h"

#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QProcess>
#include <QScreen>
#include <QSplashScreen>
#include <QFile>
#include <QStandardPaths>
#include <QTimer>
#include <QWindow>

#include <algorithm>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

// "[media]" diagnostics straight to stderr: qInfo() can be filtered out by
// logging rules, and these exist to be seen.
#define VSX_MEDIA_LOG(...) (std::fprintf(stderr, "[media] " __VA_ARGS__), std::fputc('\n', stderr), std::fflush(stderr))

#ifdef VSX_HAVE_MULTIMEDIA
#include <QAudioDevice>
#include <QAudioOutput>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QPainter>
#include <QSettings>
#include <QVideoFrame>
#include <QVideoSink>

namespace {
// The startup animation's window. Frames arrive from a QVideoSink and are
// painted here (QVideoWidget's native child window was the first version:
// it stayed on the first frame in this frameless splash). Esc / Space /
// Enter skip the rest of the video; clicks do nothing.
class StartupAnimation : public QWidget
{
public:
    QEventLoop *loop = nullptr;
    QImage frame;          // the latest video frame
    QPixmap still;         // the last frame, shown once the video is over

    void showFrame(const QVideoFrame &videoFrame)
    {
        QImage image = videoFrame.toImage();
        if (image.isNull())
            return;
        frame = std::move(image);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(2, 4, 4));            // the video's own black
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        if (!still.isNull()) {
            p.drawPixmap(rect(), still);
            return;
        }
        if (!frame.isNull()) {
            const QSize fitted = frame.size().scaled(size(), Qt::KeepAspectRatio);
            const QRect target(QPoint((width() - fitted.width()) / 2, (height() - fitted.height()) / 2), fitted);
            p.drawImage(target, frame);
        }
    }
    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space
            || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
            skip();
    }

private:
    void skip()
    {
        if (loop)
            loop->quit();
    }
};

// Plays the logo animation (with its sound) to the end, then shows its last
// frame as a still and returns the window, left up while MainWindow is built
// behind it. Played to the end first because building MainWindow blocks the
// GUI ~2.5 s (the scope SDK starting) and would freeze a video mid-way; the
// still is the same picture, so the hand-over is seamless.
//
// Qt's FFmpeg backend times playback by its audio output: when that can't
// play (on Linux, typically PipeWire without pipewire-pulse), the video sits
// on its first frame. So a stall (no progress for 1.2 s) restarts it without
// sound. nullptr if it can't play at all: the still splash then. "[media]"
// lines on the terminal say what happened.
QWidget *playStartupAnimation(QApplication &app)
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen)
        return nullptr;
    // From the resources to a cache file: playback backends read real files
    // more reliably than qrc. Rewritten when this build's video differs.
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/logo.mp4");
    QFile source(QStringLiteral(":/media/logo.mp4"));
    if (!QFileInfo::exists(path) || QFileInfo(path).size() != source.size()) {
        QFile::remove(path);
        if (!source.copy(path)) {
            VSX_MEDIA_LOG("couldn't write %s", qPrintable(path));
            return nullptr;
        }
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    }

    const QRect area = screen->availableGeometry();
    const int width = std::min(900, int(area.width() * 0.55));
    const int height = width * 9 / 16;
    auto *window = new StartupAnimation;
    window->setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    window->setFixedSize(width, height);
    window->move(area.center() - QPoint(width / 2, height / 2));

    auto *player = new QMediaPlayer(window);
    auto *sink = new QVideoSink(window);
    player->setVideoOutput(sink);
    QObject::connect(sink, &QVideoSink::videoFrameChanged, window,
                     [window](const QVideoFrame &f) { window->showFrame(f); });
    const QAudioDevice speaker = QMediaDevices::defaultAudioOutput();
    VSX_MEDIA_LOG("audio output: %s", speaker.isNull() ? "none found"
                                                      : qPrintable(speaker.description()));
    QAudioOutput *audio = nullptr;
    if (!speaker.isNull()) {
        audio = new QAudioOutput(speaker, window);
        player->setAudioOutput(audio);
    }

    QEventLoop loop;
    window->loop = &loop;
    bool failed = false;
    bool retriedMuted = false;
    QObject::connect(player, &QMediaPlayer::mediaStatusChanged, &loop,
                     [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) {
            loop.quit();
        } else if (status == QMediaPlayer::InvalidMedia) {
            VSX_MEDIA_LOG("the video can't be played: %s", qPrintable(player->errorString()));
            failed = true;
            loop.quit();
        }
    });
    QObject::connect(player, &QMediaPlayer::errorOccurred, &loop,
                     [&](QMediaPlayer::Error, const QString &message) {
        VSX_MEDIA_LOG("playback error: %s", qPrintable(message));
        failed = true;
        loop.quit();
    });
    // Stall watchdog: no progress for 1.2 s restarts without sound once,
    // then gives up.
    qint64 lastPosition = -1;
    int stalledTicks = 0;
    QTimer watchdog;
    watchdog.setInterval(300);
    QObject::connect(&watchdog, &QTimer::timeout, &loop, [&] {
        const qint64 position = player->position();
        stalledTicks = position == lastPosition ? stalledTicks + 1 : 0;
        lastPosition = position;
        if (stalledTicks < 4)
            return;
        stalledTicks = 0;
        if (audio && !retriedMuted) {
            VSX_MEDIA_LOG("playback stalled at %lld ms with sound: restarting without "
                     "(an audio output problem: on Linux check pipewire-pulse)", position);
            retriedMuted = true;
            player->stop();
            player->setAudioOutput(nullptr);
            player->setPosition(0);
            player->play();
        } else {
            VSX_MEDIA_LOG("playback stalled at %lld ms: still banner instead", position);
            failed = true;
            loop.quit();
        }
    });
    QTimer::singleShot(12000, &loop, [&] { loop.quit(); });   // never longer than this

    player->setSource(QUrl::fromLocalFile(path));
    window->show();
    window->raise();
    window->activateWindow();
    window->setFocus();
    player->play();
    watchdog.start();
    loop.exec();
    watchdog.stop();
    window->loop = nullptr;
    player->stop();
    if (failed) {
        delete window;
        return nullptr;
    }
    // The last frame, still, while the window is built.
    const qreal dpr = screen->devicePixelRatio();
    window->still = QPixmap(QStringLiteral(":/images/splash.png"))
                        .scaled(QSize(int(width * dpr), int(height * dpr)),
                                Qt::KeepAspectRatio, Qt::SmoothTransformation);
    window->still.setDevicePixelRatio(dpr);
    window->repaint();
    app.processEvents();
    return window;
}

// "VimuScopeX --intro": the startup animation as its own process, so it has
// a GUI thread of its own. Building the main window blocks the app's GUI
// thread ~1.8 s (measured: pages, settings, theme), which froze an in-process
// video — so it used to play first and the window was built after it, behind
// a held last frame. Now both run at once. Protocol on stdout: "done" once
// the video has ended (it then holds its last frame) or "failed"; any line
// (or end) on stdin closes it — the app sends one once its window is up.
int runIntro(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VimuScope X"));
    QGuiApplication::setDesktopFileName(QStringLiteral("vimuscopex"));
    QWidget *window = playStartupAnimation(app);
    if (!window) {
        std::printf("failed\n");
        std::fflush(stdout);
        return 1;
    }
    std::printf("done\n");
    std::fflush(stdout);
    // Ends at once when the app says so (or goes away): nothing to tidy here.
    std::thread([] {
        std::string line;
        std::getline(std::cin, line);
        std::_Exit(0);
    }).detach();
    QTimer::singleShot(8000, &app, &QCoreApplication::quit);   // in case nobody says
    app.exec();
    std::_Exit(0);
}
}
#endif

#ifdef Q_OS_LINUX
namespace {
// GNOME (and other Wayland desktops) ignore the icon an app sets on its own
// windows: they look up the installed .desktop file whose name matches the
// app's ID (setDesktopFileName() below) and show that file's icon. So a
// build run straight from its folder installs its own desktop entry and
// icons into ~/.local/share — the usual self-integration AppImages do.
// Rewritten only when missing or when this build lives somewhere else now,
// so the Exec line keeps pointing at the binary actually being run.
void installDesktopIntegration()
{
    const QString home = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (home.isEmpty())
        return;
    const QString exe = QCoreApplication::applicationFilePath();
    const QString entry = QStringLiteral(
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=VimuScope X\n"
        "Comment=Oscilloscope for Vimu MSO instruments\n"
        "Exec=\"%1\"\n"
        "Icon=vimuscopex\n"
        "Terminal=false\n"
        "Categories=Development;Electronics;\n"
        "StartupWMClass=VimuScopeX\n").arg(exe);

    const QString desktopPath = home + QStringLiteral("/applications/vimuscopex.desktop");
    QFile existing(desktopPath);
    if (existing.open(QIODevice::ReadOnly) && QString::fromUtf8(existing.readAll()) == entry)
        return;   // already installed for this build
    existing.close();

    for (int size : { 16, 32, 48, 64, 128, 256, 512 }) {
        const QString dir = home + QStringLiteral("/icons/hicolor/%1x%1/apps").arg(size);
        QDir().mkpath(dir);
        const QString target = dir + QStringLiteral("/vimuscopex.png");
        QFile::remove(target);
        QFile::copy(QStringLiteral(":/images/app-icon-%1.png").arg(size), target);
        QFile::setPermissions(target, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                          | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    }
    QDir().mkpath(home + QStringLiteral("/applications"));
    QFile out(desktopPath);
    if (out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        out.write(entry.toUtf8());
}
}
#endif

int main(int argc, char *argv[])
{
#if defined(__GLIBC__)
    // Large blocks (Live's 32 MB sample windows) are kept for reuse instead
    // of being handed back to the system and re-faulted page by page on the
    // next refresh: measured 99 -> 19 ms per Live refresh of 4M samples. The
    // price: freed big blocks stay with the app rather than going back to
    // the system at once.
    mallopt(M_MMAP_THRESHOLD, 512 * 1024 * 1024);
    mallopt(M_TRIM_THRESHOLD, 1024 * 1024 * 1024);
#endif
#ifdef VSX_HAVE_MULTIMEDIA
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--intro") == 0)
            return runIntro(argc, argv);
    }
#endif
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("VimuScope X"));
    app.setApplicationVersion(QStringLiteral("0.3"));
    app.setStyle(QStringLiteral("Fusion"));
    // The app's ID on Wayland (and to GNOME on X11): must match the installed
    // vimuscopex.desktop for the desktop to show our icon. Set before any
    // window exists.
    QGuiApplication::setDesktopFileName(QStringLiteral("vimuscopex"));
#ifdef Q_OS_LINUX
    installDesktopIntegration();
#endif

    // App icon: title bar, taskbar, Alt-Tab (on Windows the .exe's own icon
    // comes from resources/windows/VimuScopeX.rc). Several sizes so each
    // place gets a crisp one rather than a scaled 512.
    QIcon appIcon;
    for (int size : { 16, 32, 48, 64, 128, 256, 512 })
        appIcon.addFile(QStringLiteral(":/images/app-icon-%1.png").arg(size), QSize(size, size));
    app.setWindowIcon(appIcon);

    // Splash, up before anything heavy is built so it appears at once, and
    // held for kSplashMs from that moment (the window is built behind it).
    // Sized to the screen — 55% of its width, at most 900 px — and rendered
    // at the screen's pixel ratio so it stays sharp on high-DPI displays.
    constexpr int kSplashMs = 3000;
    QElapsedTimer splashClock;
    QSplashScreen *splash = nullptr;
    // The animated logo, when this build has Qt Multimedia and it's switched
    // on (Settings): a second process (runIntro()), so it plays while this
    // one builds the window. Else the still splash.
    QProcess *intro = nullptr;
    // Timings for the freeze hunt: "[slow]" / "[stall]" lines on stderr
    // (perftrace.h).
    PerfTrace::startStallWatch(&app);
    // The SDK starts in the background now, during the animation (Linux).
    AcquisitionManager::startSdkEarly();
#ifdef VSX_HAVE_MULTIMEDIA
    VSX_MEDIA_LOG("built with Qt Multimedia %s", qVersion());
    if (QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
            .value(QStringLiteral("ui/startupAnimation"), true).toBool()) {
        intro = new QProcess(&app);
        intro->setProcessChannelMode(QProcess::ForwardedErrorChannel);   // its [media] lines
        intro->start(QCoreApplication::applicationFilePath(), { QStringLiteral("--intro") });
        if (!intro->waitForStarted(2000)) {
            VSX_MEDIA_LOG("the intro process didn't start: %s", qPrintable(intro->errorString()));
            delete intro;
            intro = nullptr;
        }
    } else {
        VSX_MEDIA_LOG("startup animation is off in Settings");
    }
#else
    VSX_MEDIA_LOG("built WITHOUT Qt Multimedia: no startup animation, no sounds "
                  "(install qt6-multimedia qt6-multimedia-ffmpeg, then rebuild)");
#endif
    if (!intro) {
        const QPixmap banner(QStringLiteral(":/images/splash.png"));
        QScreen *screen = QGuiApplication::primaryScreen();
        if (!banner.isNull() && screen) {
            const int width = std::min(900, int(screen->availableGeometry().width() * 0.55));
            const qreal dpr = screen->devicePixelRatio();
            QPixmap scaled = banner.scaledToWidth(int(width * dpr), Qt::SmoothTransformation);
            scaled.setDevicePixelRatio(dpr);
            splash = new QSplashScreen(scaled);
            splash->show();
            // Wait until it is really on screen before the blocking work
            // below (building MainWindow starts the scope SDK, ~2.5 s). On
            // Wayland a window can't draw until the compositor answers its
            // first request — one round trip — so a single processEvents()
            // left it unmapped and it only appeared once that work was done.
            // Capped at 1 s in case the compositor never exposes it.
            QElapsedTimer wait;
            wait.start();
            while (wait.elapsed() < 1000
                   && !(splash->windowHandle() && splash->windowHandle()->isExposed()))
                app.processEvents(QEventLoop::AllEvents, 20);
            splash->repaint();
            app.processEvents();
        }
    }
    // The 3 s count starts once the splash is actually visible.
    splashClock.start();

    // A packaged Windows build carries its own Python (lib/python3.X next
    // to the exe) for the protocol decoders, so the laptop needs no Python
    // installed. Point the interpreter libsigrokdecode embeds at it before
    // anything starts it (the decoder thread does, later). Only when that
    // folder exists — a development build keeps the system's Python.
#ifdef Q_OS_WIN
    {
        const QString appDir = QCoreApplication::applicationDirPath();
        const QStringList pythons = QDir(appDir + QStringLiteral("/lib"))
            .entryList({ QStringLiteral("python3*") }, QDir::Dirs | QDir::NoDotAndDotDot);
        if (!pythons.isEmpty()) {
            qputenv("PYTHONHOME", QDir::toNativeSeparators(appDir).toLocal8Bit());
            qputenv("PYTHONDONTWRITEBYTECODE", "1");   // don't litter the app folder
        }
    }
#endif

    // Registered and applied before any widget is built, so every widget
    // picks it up as its starting font rather than the platform default,
    // with no separate pass afterward to re-apply it everywhere. Left alone
    // (keeping Qt's own platform default) if registration failed for any
    // reason — family() reports that by returning an empty string.
    Fonts::initialise();
    const QString fontFamily = Fonts::family();
    if (!fontFamily.isEmpty()) {
        // Sized off whatever Fusion's own platform default was, so it stays
        // proportionate on a system whose default is larger — but never
        // below 9 pt. Windows' default UI font (Segoe UI 9 pt) is much
        // smaller than GNOME's (~11 pt), and 85% of it left the cards, top
        // bar and side panel noticeably small on Windows. At 9 pt the card
        // titles (base - 1.5 pt, x1.1) come out at 11 px, matching the
        // sidebar buttons' fixed 11 px.
        QFont font(fontFamily);
        const qreal platformSize = app.font().pointSizeF();
        if (platformSize > 0.0)
            font.setPointSizeF(Fonts::scaled(std::max<qreal>(9.0, platformSize * 0.85)));
        app.setFont(font);
    }

    // Theme::currentMode() reads the persisted choice (QSettings' "ui/theme")
    // on its very first call, right here — light or dark, whichever was last
    // set, applied from the moment the window is built rather than always
    // starting dark and flipping a moment later.
    app.setPalette(Theme::palette());

    // Replaces Fusion's own chrome (bevels, gradients, arrow glyphs) for the
    // standard controls that were still wearing it despite the palette —
    // see theme.h for what this does and does not touch.
    app.setStyleSheet(Theme::globalStyleSheet());

    // Registers the embedded Font Awesome OTF before any widget asks for an
    // icon. A no-op when the font was not available at configure time.
    Icons::initialise();

    QElapsedTimer buildClock;
    buildClock.start();
    MainWindow window;
    if (PerfTrace::enabled())
        std::fprintf(stderr, "[startup] main window built in %lld ms\n",
                 static_cast<long long>(buildClock.elapsed()));
    std::fflush(stderr);
    if (intro) {
        // Built while the intro played: wait for it to end ("done"), fail
        // or exit, show the window, then close the intro a moment later —
        // it holds its last frame until the window is on screen, so there's
        // no gap between the two.
        QEventLoop wait;
        bool over = false;
        const auto check = [&] {
            while (intro->canReadLine()) {
                const QByteArray line = intro->readLine().trimmed();
                if (line == "done" || line == "failed")
                    over = true;
            }
            if (over || intro->state() == QProcess::NotRunning)
                wait.quit();
        };
        QObject::connect(intro, &QProcess::readyReadStandardOutput, &wait, check);
        QObject::connect(intro, &QProcess::finished, &wait, [&] { wait.quit(); });
        QTimer::singleShot(12000, &wait, &QEventLoop::quit);
        check();
        if (!over && intro->state() != QProcess::NotRunning)
            wait.exec();
        window.show();
        QTimer::singleShot(250, intro, [intro] {
            if (intro->state() != QProcess::NotRunning) {
                intro->write("close\n");
                intro->closeWriteChannel();
            }
        });
        return app.exec();
    }
    const int remaining = std::max(0, kSplashMs - int(splashClock.elapsed()));
    QTimer::singleShot(remaining, &window, [&window, splash] {
        window.show();
        if (splash) {
            splash->finish(&window);
            splash->deleteLater();
        }
    });
    return app.exec();
}
