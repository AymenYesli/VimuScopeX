#include "mainwindow.h"
#include "perftrace.h"
#include "decoderengine.h"
#include "enginetools.h"
#include "batterytools.h"
#include "mathops.h"
#include "liveworker.h"
#ifdef VSX_HAVE_MULTIMEDIA
#include <QFontDatabase>
#include <QGuiApplication>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QDialog>
#include <QKeyEvent>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <climits>
#include <QSoundEffect>
#endif
#include <cstdio>
#define VSX_MEDIA_LOG(...) (std::fprintf(stderr, "[media] " __VA_ARGS__), std::fputc('\n', stderr), std::fflush(stderr))
#include "qrcode.h"
#include "remoteserver.h"

#include "acquisitionmanager.h"
#include "annotationbox.h"
#include "cardbutton.h"
#include "panelcontrols.h"
#include "vsxdialog.h"
#include "icons.h"
#include "navigatorview.h"
#include "probedialog.h"
#include "probes.h"
#include "scopeview.h"
#include "recordwriter.h"
#include "spectrumview.h"
#include "streamengine.h"
#include "theme.h"
#include "xyview.h"

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QColorDialog>
#include <QDataStream>
#include <QFile>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QTemporaryFile>
#include <thread>
#include <QPointer>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QGridLayout>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QPixmap>
#include <QTextStream>
#include <QHBoxLayout>
#include <QLabel>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QPainter>
#include <QSpinBox>
#include <QScrollArea>
#include <QStyledItemDelegate>
#include <QSettings>
#include <QSet>
#include <QDateTime>
#include <QDir>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSplitter>
#include <QTabBar>
#include <QStackedWidget>
#include <QStyle>
#include <QStatusBar>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QKeySequence>
#include <QMessageBox>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {

const double kTimebases[] = {
    1e-6, 2e-6, 5e-6, 1e-5, 2e-5, 5e-5, 1e-4, 2e-4, 5e-4,
    1e-3, 2e-3, 5e-3, 1e-2, 2e-2, 5e-2, 1e-1
};
constexpr int kDefaultTimebaseIndex = 9;

// Live is this app's rolling-display mode (no trigger, continuous) — the
// slow end of a real scope's own roll-mode range, where a continuously
// scrolling display is more useful than a triggered snapshot. Capture and
// Record keep the faster-focused range above; picking a timebase this slow
// there would mean waiting a very long time for one triggered buffer to
// fill, which isn't what Capture is for.
const double kRollTimebases[] = { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0 };

// Record's own rate list, matching the vendor's fixed selector. Chosen
// directly here rather than derived from time/div: Record has nothing to
// navigate before it runs, so there is no "screen window" to derive a rate
// from the way Capture and Live have.
const unsigned int kSampleRates[] = {
    1000, 2000, 5000, 10000, 20000, 50000, 100000, 200000, 500000,
    1000000, 2500000, 5000000, 12500000, 25000000, 31250000, 62500000
};
constexpr int kSampleRateCount = int(sizeof(kSampleRates) / sizeof(kSampleRates[0]));
constexpr int kDefaultSampleRateIndex = 9;   // 1 MS/s

// The fixed list is a convenience; the hardware's own table for the current
// mode is authoritative. Snaps to whichever entry is actually closest.
unsigned int nearestRate(const QVector<unsigned int> &rates, unsigned int wanted)
{
    unsigned int best = rates.first();
    qint64 bestDiff = std::llabs(qint64(best) - qint64(wanted));
    for (unsigned int r : rates) {
        const qint64 diff = std::llabs(qint64(r) - qint64(wanted));
        if (diff < bestDiff) {
            bestDiff = diff;
            best = r;
        }
    }
    return best;
}

// Volts per page: the half-span of the graticule. +/-5 V spans -5 V to +5 V,
// which over 10 divisions is 1 V per division.
const double kVoltsPerPage[] = { 0.05, 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0 };
constexpr int kVoltsCount = int(sizeof(kVoltsPerPage) / sizeof(kVoltsPerPage[0]));
constexpr int kDefaultVoltsIndex = 4;   // +/-1 V

double divsFromPage(double page)
{
    return page / (ScopeView::VerticalDivs / 2.0);
}

// Round up to the nearest 1-2-5 step, so the DC offset increments in round
// numbers like 10 mV or 0.2 V rather than whatever a division happens to be.
double niceStep(double value)
{
    if (value <= 0.0)
        return 0.01;
    const double decade = std::pow(10.0, std::floor(std::log10(value)));
    const double norm = value / decade;
    const double mantissa = norm <= 1.0 ? 1.0 : norm <= 2.0 ? 2.0 : norm <= 5.0 ? 5.0 : 10.0;
    return mantissa * decade;
}

// Display zoom steps for the Scale control.
const double kDisplayScales[] = { 0.1, 0.2, 0.5, 1.0, 2.0, 5.0, 10.0 };

// TRIGGER_MODE_LIANXU is broken on this hardware — the vendor's own software
// cannot lock with it either — so the device is always left in AUTO. Auto shows
// every sweep; Normal holds frames whose edge cannot be confirmed in software.
enum TriggerMode { ModeNone = 0, ModeAuto = 1, ModeNormal = 2 };

// TRIGGER_STYLE_* from VmMultMsoLib.h.
constexpr unsigned int kStyleNone    = 0x0000;
constexpr unsigned int kStyleRising  = 0x0001;
constexpr unsigned int kStyleFalling = 0x0002;

QString formatVolts(double v, const QString &unit = QStringLiteral("V"))
{
    if (std::fabs(v) < 1.0)
        return QStringLiteral("%1 m%2").arg(v * 1000.0, 0, 'g', 3).arg(unit);
    return QStringLiteral("%1 %2").arg(v, 0, 'g', 3).arg(unit);
}

QString formatSeconds(double s)
{
    if (s < 1e-3)
        return QStringLiteral("%1 us").arg(s * 1e6, 0, 'g', 3);
    if (s < 1.0)
        return QStringLiteral("%1 ms").arg(s * 1e3, 0, 'g', 3);
    return QStringLiteral("%1 s").arg(s, 0, 'g', 3);
}

QString formatRate(double r)
{
    if (r >= 1e6)
        return QStringLiteral("%1 MS/s").arg(r / 1e6, 0, 'g', 3);
    if (r >= 1e3)
        return QStringLiteral("%1 kS/s").arg(r / 1e3, 0, 'g', 3);
    return QStringLiteral("%1 S/s").arg(r, 0, 'f', 0);
}

QPushButton *segment(const QString &text)
{
    auto *b = new QPushButton(text);
    b->setCheckable(true);
    b->setCursor(Qt::PointingHandCursor);
    b->setMinimumWidth(72);
    return b;
}

// Font Awesome where it is available, Qt's stock icon where it is not, so the
// app is usable either way.
QIcon uiIcon(QWidget *owner, const QString &faName,
             QStyle::StandardPixmap fallback, const QColor &colour = Theme::accentSoft())
{
    QIcon icon = Icons::icon(faName, colour, 30);
    if (icon.isNull())
        return owner->style()->standardIcon(fallback);
    // Qt's own auto-generated Disabled pixmap lightens the icon toward the
    // palette — the right call for a pale icon on a dark surface, but it
    // makes a disabled icon nearly invisible against a light one. An
    // explicit Disabled pixmap in the same colour disabled text already
    // uses replaces that guess outright, in both modes.
    const QIcon disabledSource = Icons::icon(faName, Theme::textDisabled(), 30);
    if (!disabledSource.isNull())
        icon.addPixmap(disabledSource.pixmap(30, 30), QIcon::Disabled);
    return icon;
}

// Padding-top lifts the glyph off the top edge; the label under it takes the
// secondary accent so icon and caption read as one unit. A function, not a
// file-scope const: Theme::accentSoftHex()/accentHex() must be read fresh at
// each call (see applyTheme()), not baked in once at static-init time.
// The sliding panel: the cards' background, and its buttons and dropdowns
// resting on the top / left sections' colour. A parent's sheet overrides the
// application's whatever the specificity, so every state that sets a
// background is restated: hover / pressed as neutral steps from the resting
// colour, checked and disabled exactly as the application has them.
QString panelHostStyle()
{
    const QColor rest = Theme::windowBackground();
    const QColor hover = Theme::isDark() ? QColor(0x23, 0x23, 0x23) : QColor(0xdb, 0xe0, 0xe5);
    const QColor press = Theme::isDark() ? QColor(0x14, 0x14, 0x14) : QColor(0xd2, 0xd8, 0xde);
    // Disabled: plain grey (the elevated colour read as dark blue).
    const QColor disabled = Theme::isDark() ? QColor(0x30, 0x30, 0x30) : QColor(0xe2, 0xe2, 0xe2);
    const QColor disabledBorder = Theme::isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0);
    return QStringLiteral(
        // (0 px: no border; %2 stays so the .arg() numbering below holds)
        "QFrame#panelHost { background: %1; border: 0px solid %2; border-radius: %3px; }"
        "QFrame#panelHost QPushButton { background: %4; border-color: %7; }"
        "QFrame#panelHost QPushButton:hover { background: %5; }"
        "QFrame#panelHost QPushButton:pressed { background: %6; }"
        "QFrame#panelHost QPushButton:checked,"
        " QFrame#panelHost QPushButton:checked:hover { background: %7; }"
        "QFrame#panelHost QPushButton:disabled { background: %8; border-color: %2; }"
        "QFrame#panelHost QComboBox { background: %4; font-size: %14pt; }"
        "QFrame#panelHost QLineEdit { background: %4; border: none; border-radius: 5px; padding: 0px 8px; }"
        "QFrame#panelHost QComboBox:disabled { background: %8; }"
        // PicoScope: groups in a row join into one outlined control...
        "QFrame#panelHost QPushButton[segment=\"left\"] { border-top-right-radius: 0px; border-bottom-right-radius: 0px; }"
        "QFrame#panelHost QPushButton[segment=\"middle\"] { border-radius: 0px; border-left: none; }"
        "QFrame#panelHost QPushButton[segment=\"right\"] { border-top-left-radius: 0px; border-bottom-left-radius: 0px;"
        " border-left: none; }"
        "QFrame#panelHost QPushButton { padding: 0px 12px; }"
        // The selected segment: the secondary accent, dark ink on it in dark
        // mode, white in light mode (as on the cards' bars).
        "QFrame#panelHost QPushButton[segment=\"left\"]:checked,"
        " QFrame#panelHost QPushButton[segment=\"middle\"]:checked,"
        " QFrame#panelHost QPushButton[segment=\"right\"]:checked"
        " { background: %12; border-color: %12; color: %13; }"
        // ...choices in a grid are flat, the selected one filled...
        "QFrame#panelHost QPushButton[flatChoice=\"true\"] { background: transparent; border-color: transparent; }"
        "QFrame#panelHost QPushButton[flatChoice=\"true\"]:hover { background: %5; }"
        "QFrame#panelHost QPushButton[flatChoice=\"true\"]:checked { background: %7; border-color: %7; }"
        // ...check boxes in a grid are toggle buttons...
        "QFrame#panelHost QCheckBox[toggle=\"true\"] { background: %4; border: 1px solid %7; border-radius: 5px;"
        " padding: 0px 8px; spacing: 0px; }"
        "QFrame#panelHost QCheckBox[toggle=\"true\"]::indicator { width: 0px; height: 0px; }"
        "QFrame#panelHost QCheckBox[toggle=\"true\"]:hover { background: %5; }"
        "QFrame#panelHost QCheckBox[toggle=\"true\"]:checked { background: %7; color: %9; }"
        "QFrame#panelHost QCheckBox[toggle=\"true\"]:disabled { background: %8; border-color: %8; }"
        // ...and dropdowns / number fields are compact cards.
        "QFrame#panelHost QFrame#stepField { background: %4; border: none; border-radius: 5px; }"
        "QFrame#panelHost QFrame#stepField QComboBox, QFrame#panelHost QFrame#stepField QAbstractSpinBox"
        " { background: transparent; border: none; min-height: 0px; padding: 0px 6px; }"
        "QFrame#panelHost QToolButton#stepMinus { background: %7; border: none;"
        " border-top-left-radius: 5px; border-bottom-left-radius: 5px; }"
        "QFrame#panelHost QToolButton#stepPlus { background: %7; border: none;"
        " border-top-right-radius: 5px; border-bottom-right-radius: 5px; }"
        "QFrame#panelHost QToolButton#stepMinus:pressed, QFrame#panelHost QToolButton#stepPlus:pressed"
        " { background: %10; }"
        "QFrame#panelHost QToolButton#stepMinus:disabled, QFrame#panelHost QToolButton#stepPlus:disabled"
        " { background: %11; }")
        .arg(Theme::cardBackground().name(), disabledBorder.name())
        .arg(Theme::cornerRadius())
        .arg(rest.name(), hover.name(), press.name(), Theme::accentHex(),
             disabled.name(), Theme::onAccent().name())
        // Selected segments: the main accent again.
        .arg(Theme::accent().darker(115).name(), Theme::textDisabled().name(),
             Theme::accent().name(), Theme::onAccent().name(),
             // The panel's one text size (the global sheet gives combos 110 %).
             QString::number(QApplication::font().pointSizeF() * 0.9, 'f', 1));
}

// The status bar: the top / left sections' neutral colour (it read as the old
// blue-grey with a blue-grey rule), no rule, 28 px, the panel's 90 % text;
// the readout muted, messages in the text colour.
QString statusBarStyle()
{
    const double pt = QApplication::font().pointSizeF() * 0.9;
    return QStringLiteral(
        "QStatusBar { background: %1; border: none; min-height: 28px; max-height: 28px;"
        " padding: 0px 10px; color: %2; font-size: %4pt; }"
        "QStatusBar::item { border: none; }"
        "QStatusBar QLabel { color: %3; background: transparent; font-size: %4pt; padding: 0px 2px; }")
        .arg(Theme::windowBackground().name(), Theme::text().name(), Theme::textMuted().name())
        .arg(pt, 0, 'f', 1);
}

QString gridButtonStyle()
{
    return QStringLiteral(
        "QToolButton { padding-top: 5px; padding-bottom: 3px; font-size: 12px;"
        "              color: %1; border: none; background: transparent;"
        "              border-radius: 5px; }"
        "QToolButton:hover { background: rgba(115, 255, 0, 26); }"
        "QToolButton:checked { background: rgba(115, 255, 0, 60);"
        "                      border: 1px solid %2; color: %3; }")
        // Selected text: the accent on dark; on light the bright accent on
        // its own pale tint was ~1.7:1, so the icon's darker green instead.
        .arg(Theme::accentSoftHex(), Theme::accentHex(),
             Theme::isDark() ? Theme::accentHex() : Theme::accentSoftHex());
}

QString toolButtonQss()
{
    return QStringLiteral(
        "QToolButton { padding-top: 8px; padding-bottom: 4px; color: %1;"
        "              border: none; background: transparent; }"
        "QToolButton:hover { background: rgba(115, 255, 0, 26); border-radius: 6px; }")
        .arg(Theme::accentSoftHex());
}

// QTabBar carries no styling of its own anywhere else in this app — without
// this it renders in plain, unstyled Fusion chrome, out of place next to
// everything around it. Colours matched exactly to QComboBox/QPushButton's
// own background/border/disabled/checked values above, not just similar
// ones, so the tab bar reads as the same design rather than a different one
// that happens to be nearby. A function for the same reason as the two above.
QString recordTabStyle()
{
    return QStringLiteral(
        "QTabBar::tab {"
        "    background: %1; color: %2;"
        "    border: 1px solid %3; border-bottom: none;"
        "    border-top-left-radius: %11px; border-top-right-radius: %11px;"
        "    padding: 0px 20px 0px 10px; height: 29px; margin-right: 2px; }"
        "QTabBar::tab:hover:!selected { background: %4; border-color: %5; }"
        "QTabBar::tab:selected { background: %6; color: %7; border-color: %6; }"
        "QTabBar::tab:disabled { background: %8; color: %9; border-color: %10; }")
        // Unselected tabs: the cards' neutral greys (the surface colours read as dark blue).
        .arg(Theme::cardBackground().name(), Theme::textMuted().name(),
             (Theme::isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0)).name(),
             Theme::cardHoverBackground().name(),
             (Theme::isDark() ? QColor(0x4a, 0x4a, 0x4a) : QColor(0xb8, 0xb8, 0xb8)).name(),
             Theme::accentHex(), Theme::onAccent().name(),
             (Theme::isDark() ? QColor(0x30, 0x30, 0x30) : QColor(0xe2, 0xe2, 0xe2)).name())
        .arg(Theme::textDisabled().name(),
             (Theme::isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0)).name())
        .arg(Theme::cornerRadius());
}

// Every card is built to these, so they match exactly rather than by eye. The
// scope and trigger cards are the one exception: they carry more text.
constexpr int kCardWidth     = 178;
constexpr int kWideCardWidth = 224;
constexpr int kCardHeight    = 62;
constexpr int kHalfCardHeight = 31;

// The detail panel's own open width — was a literal in setFixedWidth(),
// now shared with the slide-open/closed animation in openPage()/closePanel().
constexpr int kPanelWidth = 250;
// The panel's title bar and the record tabs: one height (tabs: 29 px + their
// 1 px top border), so their tops and bottoms line up.
constexpr int kTabStripHeight = 30;

// No vertical tab shift: the style drew unselected tabs 2 px lower than the
// selected one, so the record tabs never sat level with the panel's title.
class NoTabShiftStyle : public QProxyStyle
{
public:
    using QProxyStyle::QProxyStyle;
    int pixelMetric(PixelMetric metric, const QStyleOption *option, const QWidget *widget) const override
    {
        if (metric == PM_TabBarTabShiftVertical || metric == PM_TabBarTabShiftHorizontal)
            return 0;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }
};

// One definition of where records live, created on first use. Reads a
// user-chosen override from Settings (see buildSettingsPage()) each time,
// rather than caching it, so a directory change takes effect on the very
// next record without anything else needing to know it happened.
QString recordDirectory()
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    QString path = settings.value(QStringLiteral("settings/recordDirectory")).toString();
    if (path.isEmpty()) {
        QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (base.isEmpty())
            base = QDir::homePath() + QStringLiteral("/.vimuscopex");
        path = base + QStringLiteral("/records");
    }
    QDir().mkpath(path);
    return path;
}

// Frequency counter for the low-pass cutoffs: the arrows, wheel and Page
// keys step through 1-2-5 values (... 1 k, 2 k, 5 k, 10 k ...), it shows
// Hz / kHz / MHz, and typed values like "250k", "1.5 MHz" or "800" work.
// Only overrides virtuals, so it needs no Q_OBJECT of its own.
class FrequencySpinBox : public QDoubleSpinBox
{
public:
    explicit FrequencySpinBox(QWidget *parent = nullptr) : QDoubleSpinBox(parent)
    {
        setDecimals(3);
        setRange(1.0, 100e6);
        setKeyboardTracking(false);   // apply on Enter / focus-out, not per keystroke
        setValue(100e3);
    }

    void stepBy(int steps) override { setValue(step125(value(), steps, minimum(), maximum())); }

    QString textFromValue(double v) const override
    {
        if (v >= 1e6)
            return QStringLiteral("%1 MHz").arg(QString::number(v / 1e6, 'g', 4));
        if (v >= 1e3)
            return QStringLiteral("%1 kHz").arg(QString::number(v / 1e3, 'g', 4));
        return QStringLiteral("%1 Hz").arg(QString::number(v, 'g', 4));
    }

    double valueFromText(const QString &text) const override
    {
        bool ok = false;
        const double v = parse(text, &ok);
        return ok ? v : value();
    }

    QValidator::State validate(QString &text, int &) const override
    {
        bool ok = false;
        const double v = parse(text, &ok);
        return ok && v >= minimum() && v <= maximum() ? QValidator::Acceptable
                                                       : QValidator::Intermediate;
    }

private:
    static double parse(QString text, bool *ok)
    {
        text = text.trimmed();
        text.replace(QLatin1Char(','), QLatin1Char('.'));
        if (text.endsWith(QLatin1String("hz"), Qt::CaseInsensitive))
            text.chop(2);
        text = text.trimmed();
        double mult = 1.0;
        if (text.endsWith(QLatin1Char('k')) || text.endsWith(QLatin1Char('K'))) {
            mult = 1e3;
            text.chop(1);
        } else if (text.endsWith(QLatin1Char('M')) || text.endsWith(QLatin1Char('m'))) {
            mult = 1e6;   // nothing here is ever milli-hertz
            text.chop(1);
        }
        return text.trimmed().toDouble(ok) * mult;
    }

    // Tested standalone: 1 2 5 10 20 ... both ways, off-sequence values
    // snap to the next 1-2-5 step, clamped to the range.
    static double step125(double v, int steps, double lo, double hi)
    {
        const double mant[] = { 1.0, 2.0, 5.0 };
        for (int s = 0; s < std::abs(steps); ++s) {
            const double decade = std::pow(10.0, std::floor(std::log10(v) + 1e-9));
            const double m = v / decade;
            double next = 0.0;
            if (steps > 0) {
                next = 10.0 * decade;
                for (double c : mant) {
                    if (c > m + 1e-9) { next = c * decade; break; }
                }
            } else {
                next = 0.5 * decade;
                for (int i = 2; i >= 0; --i) {
                    if (mant[i] < m - 1e-9) { next = mant[i] * decade; break; }
                }
            }
            v = std::min(hi, std::max(lo, next));
        }
        return v;
    }
};

// What an open input can plausibly read at a hardware half-range (volts): an
// offset within max(30 mV, 2 % of the half-range) and a noise swing within
// max(20 mV, 10 % of it). The scope's own offsets measure a few mV. (Used by
// the zero calibration and when its stored table is loaded.)
double plausibleOffsetV(int halfMv) { return std::max(0.030, 0.02 * halfMv / 1000.0); }
double plausibleSwingV(int halfMv)  { return std::max(0.020, 0.10 * halfMv / 1000.0); }

// A group of settings (Automotive: Engine, Battery and charging...):
// stronger than the titles over settings, with a rule above all but the first.
QString groupHeadingStyle(bool first)
{
    const QColor rule = Theme::isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0);
    return first ? QStringLiteral("color: %1; margin-top: 2px;").arg(Theme::text().name())
                 : QStringLiteral("color: %1; margin-top: 12px; padding-top: 10px; border-top: 1px solid %2;")
                       .arg(Theme::text().name(), rule.name());
}

QLabel *groupHeading(const QString &text, bool first = false)
{
    auto *l = new QLabel(text);
    l->setObjectName(QStringLiteral("groupHeading"));
    l->setProperty("firstGroup", first);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet(groupHeadingStyle(first));
    return l;
}

QLabel *sectionLabel(const QString &text)
{
    auto *l = new QLabel(text);
    // Tagged so applyTheme() can find and recolour it: the colour is baked
    // into this label's own style sheet, which a theme switch doesn't touch.
    l->setObjectName(QStringLiteral("sectionLabel"));
    l->setStyleSheet(QStringLiteral("color: %1; margin-top: 6px;")
                          .arg(Theme::textMuted().name()));
    return l;
}

}   // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    // Before the first tab build (see m_openedRecordings).
    m_openedRecordings = QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
                             .value(QStringLiteral("records/opened")).toStringList();
    setWindowTitle(QStringLiteral("VimuScope X"));
    resize(1400, 820);

    m_acq = new AcquisitionManager(this);
    m_probes = new ProbeLibrary(this);
    m_stream = new StreamEngine(this);
    // Live's long-window preparation runs here (see refreshLiveView()).
    m_liveThread = new QThread(this);
    m_liveWorker = new LiveWorker(m_stream);
    m_liveWorker->moveToThread(m_liveThread);
    connect(m_liveThread, &QThread::finished, m_liveWorker, &QObject::deleteLater);
    m_liveThread->start();

    // Pulls the visible window out of the ring buffer. Deliberately independent
    // of the callback rate: the SDK thread only ever copies and counts.
    m_streamTick = new QTimer(this);
    m_streamTick->setInterval(33);
    connect(m_streamTick, &QTimer::timeout, this, &MainWindow::pullStream);

    // Throttles the expensive part of dragging a record to roughly 60 Hz.
    // start() on an already-running single-shot timer RESETS its countdown, so
    // calling it on every drag event would be a debounce (fire only once
    // dragging stops) rather than a steady-cadence throttle — a real problem
    // under a fast, continuous drag, which is exactly when this matters most.
    // Instead the first event in a burst fires immediately and starts a 16 ms
    // cooldown; further events during the cooldown only set a dirty flag;
    // the cooldown's own timeout performs one more refresh if anything
    // changed, so the last position is never left undrawn.
    m_recordPanDebounce = new QTimer(this);
    m_recordPanDebounce->setSingleShot(true);
    m_recordPanDebounce->setInterval(16);
    connect(m_recordPanDebounce, &QTimer::timeout, this, [this] {
        if (m_recordPanDirty) {
            m_recordPanDirty = false;
            refreshRecordView();
            m_recordPanDebounce->start();   // keep the cadence while still dirty
        }
    });
    for (int ch = 0; ch < 4; ++ch)
        m_colour[ch] = Theme::channelColor(ch);

    // The theme before the interface exists, so every widget is styled once
    // as it's created: applying the global style sheet after building made
    // Qt restyle every widget (all panel pages) — ~950 ms measured. The
    // stored light / dark choice is read on first use (theme.h).
    qApp->setPalette(Theme::palette());
    qApp->setStyleSheet(Theme::globalStyleSheet());
    buildUi();
    for (int ch = 0; ch < 4; ++ch)
        reloadProbeList(ch);
    wireUp();

    // The cards show live acquisition state, which changes without any user
    // action. Repaint them on a timer rather than only when something is
    // clicked.
    auto *tick = new QTimer(this);
    tick->setInterval(250);
    connect(tick, &QTimer::timeout, this, [this] {
        refreshCards();
        updateStatusBar();
    });
    tick->start();

    restoreSettings();

    // Unconditional, not left to restoreSettings() alone: on a fresh
    // install, with no settings file yet to restore, restoreSettings()
    // returns before ever reaching its own applyChannel() loop — leaving a
    // channel that starts on by default (CH1's initial Auto mode) with no
    // path to ever call setChannelVisible(true) at all, since that is
    // otherwise only ever set here or by a person actually touching that
    // channel's own mode combo. Redundant, not harmful, on a normal launch
    // where restoreSettings() already did this itself.
    for (int ch = 0; ch < 4; ++ch)
        applyChannel(ch);

    m_acq->initialize();
    m_adcInfoTimer = new QTimer(this);
    m_adcInfoTimer->setInterval(1000);
    connect(m_adcInfoTimer, &QTimer::timeout, this, [this] { updateAdcInfo(); });
    m_adcInfoTimer->start();
    m_streamRestartTimer = new QTimer(this);
    m_streamRestartTimer->setSingleShot(true);
    m_streamRestartTimer->setInterval(300);
    connect(m_streamRestartTimer, &QTimer::timeout, this, [this] {
        if (m_running && m_acquisitionMode == ModeStream && !m_liveLoaded) {
            stopAcquisition();
            startAcquisition();
        }
    });
    m_rrTimer = new QTimer(this);
    m_rrTimer->setInterval(30);
    connect(m_rrTimer, &QTimer::timeout, this, [this] { rangeReportTick(); });
    qApp->installEventFilter(this);   // Left / Right panning (see eventFilter())
    // The state at launch is now known, scope or no scope: the first plug-in
    // after starting without one is an event (it used to become the starting
    // state, silently, since no report had come before it).
    m_deviceStateKnown = true;
    m_lastDeviceReady = m_acq->ready();
    applyTheme();
    refreshCards();
}

// ---------------------------------------------------------------------------
// Shell
// ---------------------------------------------------------------------------

void MainWindow::buildUi()
{
    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(6);
    m_rootLayout = root;

    m_topBar = buildTopBar();
    root->addWidget(m_topBar);

    auto *body = new QHBoxLayout;
    body->setSpacing(6);
    m_channelRail = buildChannelRail();
    body->addWidget(m_channelRail);
    body->addWidget(buildDetailPanel());

    // Navigator above the plot, sharing its column.
    auto *plotColumn = new QWidget;
    auto *plotLayout = new QVBoxLayout(plotColumn);
    plotLayout->setContentsMargins(0, 0, 0, 0);
    plotLayout->setSpacing(4);

    // Record mode's own tab bar — one tab per past recording in the current
    // record directory (see refreshRecordTabs()), shown only in Record mode
    // (setAcquisitionMode()) and disabled while a recording is actively
    // running (loading a different one mid-write would mean pulling its
    // files out from under the writer thread). Sits above the navigator
    // strip, matching where the vendor's own app puts its equivalent tabs.
    m_recordTabs = new QTabBar;
    m_recordTabs->setExpanding(false);
    m_recordTabs->setVisible(false);
    m_recordTabs->setStyleSheet(recordTabStyle());
    {
        QFont tabFont = m_recordTabs->font();
        tabFont.setPointSizeF(tabFont.pointSizeF() * 1.10);
        m_recordTabs->setFont(tabFont);
    }
    // Suppresses the connecting line QTabBar draws beneath its tabs by
    // default (meant to visually join a QTabWidget's tab bar to its page
    // below) — with nothing here for it to connect to, it just showed as a
    // stray light line between the tabs and the navigator strip right
    // underneath them.
    m_recordTabs->setDrawBase(false);
    connect(m_recordTabs, &QTabBar::currentChanged, this, [this](int index) {
        if (!m_recordTabs)
            return;
        if (index < 0) {
            // No tabs left at all (most commonly: the last one was just
            // closed) — show nothing, rather than leaving whichever
            // recording was on screen before still sitting there.
            // refreshRecordView() itself cannot be used for this: it
            // treats m_recordViewLength == 0 as "show the whole of
            // whatever m_stream still has loaded", not "show nothing".
            for (int ch = 0; ch < 4; ++ch)
                m_view->clearChannel(ch);
            m_nav->clearAll();
            m_recordViewStart = 0;
            m_recordViewLength = 0;
            m_currentRecordTab.clear();
            syncAnnotationContext();
            // Nothing left to zoom: the arrows would otherwise replay the
            // closed recording's zoom steps and bring its waveform back.
            m_zoomBack[ModeRecord].clear();
            m_zoomForward[ModeRecord].clear();
            updateZoomHistoryButtons();
            return;
        }
        loadRecordTab(m_recordTabs->tabData(index).toString());
        updateRecordTabCloseIcons();
    });
    {
        auto *style = new NoTabShiftStyle(QStyleFactory::create(QApplication::style()->name()));
        style->setParent(m_recordTabs);
        m_recordTabs->setStyle(style);
    }
    plotLayout->addWidget(m_recordTabs);

    m_nav = new NavigatorView;
    for (int ch = 0; ch < 4; ++ch)
        m_nav->setChannelColor(ch, Theme::channelColor(ch));
    plotLayout->addWidget(m_nav);

    m_view = new ScopeView;
    for (int ch = 0; ch < 4; ++ch) {
        m_view->setChannelColor(ch, Theme::channelColor(ch));
        m_view->setVoltsPerDiv(ch, divsFromPage(kVoltsPerPage[kDefaultVoltsIndex]));
    }
    m_view->setSecondsPerDiv(kTimebases[kDefaultTimebaseIndex]);

    // Split, not just stacked: PicoScope's own Spectrum view is a
    // user-resizable pane below the time-domain plot (see the reference
    // screenshot this was built against), and QSplitter is what gets that
    // for free. XY mode shares this same second-pane slot — only one of
    // Spectrum/XY is ever meaningful at a time (Instruments' own mode
    // selector is exclusive), so a QStackedWidget holds both and the
    // splitter itself only ever sees two children, never three; it has no
    // idea which content is currently showing. The whole stack starts
    // hidden — buildInstrumentsPage()'s mode handling (setInstrumentMode())
    // is what shows it, via a plain setVisible(), which a QSplitter
    // handles correctly (the hidden widget just takes no space).
    m_spectrum = new SpectrumView;
    m_xy = new XYView;
    m_secondPaneStack = new QStackedWidget;
    m_secondPaneStack->addWidget(m_spectrum);
    m_secondPaneStack->addWidget(m_xy);
    m_secondPaneStack->hide();

    // ScopeView's own 380 px minimum (set in its constructor, sized for
    // filling the whole plot column alone) otherwise caps how far the
    // splitter below can ever be dragged toward the second pane — in a
    // typical window that floor alone can eat most of the available
    // height, well short of the 60% snap-to-maximized threshold. Only the
    // height is relaxed; its 640 px width minimum is unaffected by a
    // vertical split.
    m_view->setMinimumHeight(150);

    m_plotSplitter = new QSplitter(Qt::Vertical);
    m_plotSplitter->addWidget(m_view);
    m_plotSplitter->addWidget(m_secondPaneStack);
    m_plotSplitter->setStretchFactor(0, 1);
    m_plotSplitter->setStretchFactor(1, 1);
    plotLayout->addWidget(m_plotSplitter, 1);

    // Dragging the spectrum pane past 60% of the total height snaps it the
    // rest of the way to fully maximized (the scope pane collapses to 0)
    // rather than leaving it sitting at some arbitrary point past that —
    // a "drag past a threshold, snap to full" affordance, asked for
    // explicitly. Dragging back up always works normally: the splitter
    // handle stays at the very top, grabbable, even with the scope pane's
    // own size at 0.
    connect(m_plotSplitter, &QSplitter::splitterMoved, this, [this](int, int) {
        const QList<int> sizes = m_plotSplitter->sizes();
        if (sizes.size() != 2)
            return;
        const int total = sizes[0] + sizes[1];
        if (total <= 0)
            return;

        // Dragging the second pane away to nothing is treated as "turn
        // Instruments back to Scope mode", not just "shrink it very
        // small" — the opposite of the maximize snap below, and for the
        // same reason: the gesture of resizing a pane out of existence is
        // itself the signal, not something to leave sitting at a sliver of
        // a pixel. Mode-agnostic: works whichever of Spectrum/XY was
        // showing, since both share this same slot.
        if (sizes[1] <= 1) {
            setInstrumentMode(InstrumentMode::Scope);
            if (auto *scope = m_instrumentMode->button(0))
                scope->setChecked(true);
            return;
        }

        const double spectrumFraction = double(sizes[1]) / double(total);
        if (spectrumFraction > 0.60 && spectrumFraction < 1.0)
            m_plotSplitter->setSizes({0, total});
    });

    body->addWidget(plotColumn, 1);

    root->addLayout(body, 1);
    setCentralWidget(central);

    m_statusLine = new QLabel(QStringLiteral("Starting..."));
    m_statusLine->setTextFormat(Qt::RichText);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->addPermanentWidget(m_statusLine);
    statusBar()->setStyleSheet(statusBarStyle());
}

QWidget *MainWindow::buildTopBar()
{
    auto *bar = new QWidget;
    auto *layout = new QHBoxLayout(bar);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    m_runCard = new CardButton(QString());
    m_runCard->setSteppersInteractive(false);   // bars for symmetry, nothing to step
    m_runCard->setFixedSize(kCardWidth, kCardHeight);
    m_runCard->setAccent(QColor(0xe0, 0x5c, 0x5c));
    layout->addWidget(m_runCard);

    m_scopeCard = new CardButton(QStringLiteral("Scope"));
    m_scopeCard->setFixedSize(kWideCardWidth, kCardHeight);
    m_scopeCard->setAccent(QColor(0x8a, 0x9a, 0xa8));
    layout->addWidget(m_scopeCard);

    m_triggerCard = new CardButton(QStringLiteral("Trigger"));
    m_triggerCard->setFixedSize(kWideCardWidth, kCardHeight);
    m_triggerCard->setAccent(QColor(0xd8, 0x92, 0x3a));
    layout->addWidget(m_triggerCard);

    auto *single = new QToolButton;
    m_singleButton = single;
    single->setText(QStringLiteral("Single"));
    single->setIconSize(QSize(22, 22));
    single->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    single->setFixedHeight(kCardHeight);
    single->setFixedWidth(62);
    auto refreshSingle = [this, single] {
        single->setIcon(uiIcon(this, QStringLiteral("forward-step"),
                               QStyle::SP_MediaSkipForward));
        single->setStyleSheet(toolButtonQss());
    };
    refreshSingle();
    m_themedRefreshers.append(refreshSingle);
    layout->addWidget(single);
    connect(single, &QToolButton::clicked, this, [this] {
        leaveFileView();
        // Single starts a capture without going through startAcquisition(),
        // so it has to lift a loaded file's samplesReady guard itself —
        // otherwise its result was silently dropped after opening a file.
        m_suppressSamplesReady = false;
        m_acq->single();
        setRunning(true);
    });

    auto *force = new QToolButton;
    m_forceButton = force;
    force->setText(QStringLiteral("Force"));
    force->setIconSize(QSize(22, 22));
    force->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    force->setFixedHeight(kCardHeight);
    force->setFixedWidth(62);
    auto refreshForce = [this, force] {
        force->setIcon(uiIcon(this, QStringLiteral("bolt"), QStyle::SP_MediaSeekForward));
        force->setStyleSheet(toolButtonQss());
    };
    refreshForce();
    m_themedRefreshers.append(refreshForce);
    layout->addWidget(force);
    connect(force, &QToolButton::clicked, m_acq, &AcquisitionManager::forceTrigger);

    m_freezeButton = new QToolButton;
    m_freezeButton->setText(QStringLiteral("Freeze"));
    m_freezeButton->setIconSize(QSize(22, 22));
    m_freezeButton->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    m_freezeButton->setCheckable(true);
    m_freezeButton->setFixedHeight(kCardHeight);
    m_freezeButton->setFixedWidth(62);
    auto refreshFreeze = [this] {
        m_freezeButton->setIcon(uiIcon(this, QStringLiteral("grip-lines"),
                                       QStyle::SP_MediaPause));
        m_freezeButton->setStyleSheet(toolButtonQss());
    };
    refreshFreeze();
    m_themedRefreshers.append(refreshFreeze);
    m_freezeButton->setEnabled(false);
    m_freezeButton->setToolTip(QStringLiteral(
        "Stops the display while acquisition keeps running. Drag the plot "
        "while frozen to look back through the buffered history."));
    layout->addWidget(m_freezeButton);
    connect(m_freezeButton, &QToolButton::toggled, this, [this](bool on) {
        m_stream->setFrozen(on);
        if (!on) {
            // Un-freezing resumes pullStream()'s normal "show the newest"
            // tick; a pan offset left over from before freezing would
            // otherwise keep it showing a fixed distance behind instead of
            // catching back up to the newest sample.
            m_liveViewOffset = 0;
        }
        statusBar()->showMessage(on
            ? QStringLiteral("Display frozen — drag the plot to pan through it")
            : QStringLiteral("Display live"), on ? 3000 : 2000);
    });

    layout->addSpacing(10);

    // Shell only for now: these carry icons and open their panel, but nothing
    // behind them is implemented yet.
    const struct {
        const char *text;
        const char *fa;
        QStyle::StandardPixmap fallback;
    } kTools[] = {
        { "Open",          "folder-open",  QStyle::SP_DialogOpenButton },
        { "Save",          "floppy-disk",  QStyle::SP_DialogSaveButton },
        { "Full",          "expand",       QStyle::SP_TitleBarMaxButton },
    };

    for (const auto &tool : kTools) {
        auto *button = new QToolButton;
        button->setText(QString::fromLatin1(tool.text));
        button->setIconSize(QSize(22, 22));
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setFixedHeight(kCardHeight);
        button->setFixedWidth(66);
        button->setAutoRaise(true);
        auto refreshTool = [this, button, fa = QString::fromLatin1(tool.fa),
                            fallback = tool.fallback] {
            button->setIcon(uiIcon(this, fa, fallback));
            button->setStyleSheet(toolButtonQss());
        };
        refreshTool();
        m_themedRefreshers.append(refreshTool);
        layout->addWidget(button);

        const QString name = QString::fromLatin1(tool.text);
        if (name == QLatin1String("Open")) {
            button->setToolTip(QStringLiteral(
                "Open a recording (.vsxrec, switching to Record mode) or a "
                "saved Capture/Live waveform (.vsxwave)."));
            connect(button, &QToolButton::clicked, this, [this] {
                // A recording is a directory internally, but asking the
                // person to pick a FOLDER is what left every .vsxrec file
                // shown greyed out and unselectable — a getExistingDirectory
                // dialog cannot select a file at all, matching neither what
                // it looked like nor what "open a .vsxrec file" suggests.
                // Picking the file directly, then taking its parent
                // directory, gives the natural interaction while keeping
                // the same directory-based loading underneath.
                const QString file = QFileDialog::getOpenFileName(
                    this, QStringLiteral("Open"), recordDirectory(),
                    QStringLiteral("VimuScopeX files (*.%1 *.vsxwave);;"
                                    "VimuScopeX recordings (*.%1);;"
                                    "VimuScopeX waveform (*.vsxwave)")
                        .arg(QLatin1String(RecordWriter::kFileExtension)));
                if (file.isEmpty())
                    return;   // cancelled

                // Auto-detected from the chosen file itself — one dialog,
                // two entirely independent mechanisms underneath (Open/Save
                // for Capture/Live is deliberately independent of Record
                // mode's own .vsxrec persistence).
                if (QFileInfo(file).suffix().compare(
                        QStringLiteral("vsxwave"), Qt::CaseInsensitive) == 0) {
                    openNativeWaveform(file);
                    return;
                }

                const QString dir = QFileInfo(file).absolutePath();
                if (m_acquisitionMode != ModeRecord) {
                    // setChecked() alone does not emit idClicked (only an
                    // actual click does), so setAcquisitionMode() needs
                    // calling explicitly right alongside it, exactly like
                    // the "not implemented" mode fallback elsewhere does.
                    if (m_acqMode) {
                        if (auto *recordButton = m_acqMode->button(ModeRecord))
                            recordButton->setChecked(true);
                    }
                    setAcquisitionMode(ModeRecord);
                }
                // Explicitly opening a recording is exactly the kind of
                // deliberate action that should undo an earlier close.
                setRecordTabHidden(dir, false);
                // A recording from outside the record directory gets a tab
                // of its own too (see m_openedRecordings).
                {
                    const QString path = QFileInfo(dir).absoluteFilePath();
                    bool inRecordDir = false;
                    for (const RecordWriter::RecordInfo &info :
                         RecordWriter::listRecordings(recordDirectory()))
                        inRecordDir = inRecordDir || QFileInfo(info.directory).absoluteFilePath() == path;
                    if (!inRecordDir && !m_openedRecordings.contains(path)) {
                        m_openedRecordings.append(path);
                        QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
                            .setValue(QStringLiteral("records/opened"), m_openedRecordings);
                    }
                }
                loadRecordTab(dir);
                refreshRecordTabs();
            });
        } else if (name == QLatin1String("Save")) {
            button->setToolTip(QStringLiteral(
                "Save the current display as an image or data file \u2014 or, "
                "in Capture/Live, a waveform file that can be reopened later."));
            connect(button, &QToolButton::clicked, this, [this] { saveCurrentView(); });
        } else if (name == QLatin1String("Full")) {
            button->setToolTip(QStringLiteral(
                "Show only the plot, full screen. Esc returns."));
            connect(button, &QToolButton::clicked, this, [this] { setFullView(true); });
        } else {
            button->setToolTip(QStringLiteral("%1 — not implemented yet.").arg(name));
            connect(button, &QToolButton::clicked, this, [this, name] {
                statusBar()->showMessage(
                    QStringLiteral("%1 is not implemented yet.").arg(name), 3000);
            });
        }
    }

    layout->addStretch();

    m_logoLabel = new QLabel;
    m_logoLabel->setAlignment(Qt::AlignCenter);
    m_logoLabel->setContentsMargins(14, 0, 6, 0);
    refreshLogo();
    layout->addWidget(m_logoLabel, 0, Qt::AlignVCenter);

    return bar;
}

QWidget *MainWindow::buildChannelRail()
{
    auto *rail = new QWidget;
    rail->setFixedWidth(kCardWidth);
    auto *layout = new QVBoxLayout(rail);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(6);

    for (int ch = 0; ch < 4; ++ch) {
        m_channelCard[ch] = new CardButton(QStringLiteral("CH%1").arg(ch + 1));
        m_channelCard[ch]->setAccent(Theme::channelColor(ch));
        m_channelCard[ch]->setFixedSize(kCardWidth, kCardHeight);
        m_channelCard[ch]->setVisible(false);
        layout->addWidget(m_channelCard[ch]);
    }

    // Half-height: neither is implemented, so neither earns a full card.
    m_logicCard = new CardButton(QStringLiteral("Logic"));
    m_logicCard->setCompact(true);
    m_logicCard->setAccent(QColor(0x9b, 0x8c, 0xff));
    m_logicCard->setSteppersInteractive(false);
    m_logicCard->setStepperGlyphs(true);   // signs like every card; inactive until Logic exists
    m_logicCard->setMuted(true);
    m_logicCard->setFixedSize(kCardWidth, kHalfCardHeight);
    m_logicCard->setToolTip(QStringLiteral("Logic analyser — not implemented yet."));
    layout->addWidget(m_logicCard);

    m_dioCard = new CardButton(QStringLiteral("DIO"));
    m_dioCard->setCompact(true);
    m_dioCard->setAccent(QColor(0x6f, 0xc3, 0xb8));
    m_dioCard->setSteppersInteractive(false);
    m_dioCard->setStepperGlyphs(true);
    m_dioCard->setMuted(true);
    m_dioCard->setFixedSize(kCardWidth, kHalfCardHeight);
    m_dioCard->setToolTip(QStringLiteral("Digital IO — not implemented yet."));
    layout->addWidget(m_dioCard);

    for (CardButton *card : { m_logicCard, m_dioCard }) {
        connect(card, &CardButton::clicked, this, [this] {
            statusBar()->showMessage(
                QStringLiteral("Not implemented yet."), 3000);
        });
    }

    // Only appears once a math channel is actually turned on — see
    // applyMathChannel() — so an idle rail does not carry a card for a
    // feature that is not doing anything.
    m_mathCard = new CardButton(QStringLiteral("Math"));
    m_mathCard->setCompact(true);
    // - / + step Math's range like its page's range control (Auto first).
    m_mathCard->setSteppersInteractive(true);
    m_mathCard->setFixedSize(kCardWidth, kHalfCardHeight);
    m_mathCard->setVisible(false);
    connect(m_mathCard, &CardButton::clicked, this, [this] { openPage(PageMath); });
    connect(m_mathCard, &CardButton::stepUp, this, [this] {
        if (m_mathVoltsPage)
            m_mathVoltsPage->setCurrentIndex(std::min(m_mathVoltsPage->count() - 1,
                                                      m_mathVoltsPage->currentIndex() + 1));
    });
    connect(m_mathCard, &CardButton::stepDown, this, [this] {
        if (m_mathVoltsPage)
            m_mathVoltsPage->setCurrentIndex(std::max(0, m_mathVoltsPage->currentIndex() - 1));
    });
    layout->addWidget(m_mathCard);

    layout->addSpacing(6);
    layout->addWidget(buildToolGrid());
    layout->addStretch();
    return rail;
}

// Two per row, icon above the label. Zoom, Annotations, and Measure are
// real; most of the rest are still shells that just say so when clicked
// (see HANDOFF.md).
QWidget *MainWindow::buildToolGrid()
{
    const struct { const char *text; const char *fa; QStyle::StandardPixmap fallback; }
    kTools[] = {
        { "Math",        "calculator",       QStyle::SP_FileDialogDetailedView },
        { "Measure",     "ruler-combined",   QStyle::SP_FileDialogInfoView },
        { "Rulers",      "ruler-horizontal", QStyle::SP_FileDialogListView },
        { "Decoding",    "square-binary",    QStyle::SP_FileDialogContentsView },
        { "Annotations", "note-sticky",      QStyle::SP_FileDialogDetailedView },
        { "Settings",    "gear",             QStyle::SP_FileDialogInfoView },
        { "Instruments", "toolbox",          QStyle::SP_DesktopIcon },
        { "Default",     "rotate-left",      QStyle::SP_BrowserReload },
        { "Zoom",        "magnifying-glass", QStyle::SP_FileDialogContentsView },
        // Labelled "References" rather than "Reference Waveforms": a grid
        // cell is ~87 px wide at a 10 px font, and the full phrase does not
        // fit without clipping. The tooltip carries the full name.
        { "References",  "clone",            QStyle::SP_FileDialogDetailedView },
        { "Automotive",  "car",              QStyle::SP_DriveHDIcon },
        { "Remote",      "mobile-screen-button", QStyle::SP_ComputerIcon },
    };

    auto *grid = new QWidget;
    auto *layout = new QGridLayout(grid);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    int index = 0;
    for (const auto &tool : kTools) {
        auto *button = new QToolButton;
        button->setText(QString::fromLatin1(tool.text));
        button->setIconSize(QSize(25, 25));   // 15% up on 22
        button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setMinimumHeight(52);
        button->setAutoRaise(true);
        auto refreshTool = [this, button, fa = QString::fromLatin1(tool.fa),
                            fallback = tool.fallback] {
            button->setIcon(uiIcon(this, fa, fallback));
            button->setStyleSheet(gridButtonStyle());
        };
        refreshTool();
        m_themedRefreshers.append(refreshTool);
        button->setToolTip(QStringLiteral("%1 — not implemented yet.")
                               .arg(QString::fromLatin1(tool.text)));

        const QString name = QString::fromLatin1(tool.text);
        if (name == QLatin1String("Instruments")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral("Scope, spectrum, XY, persistence."));
            m_instrumentsButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageInstruments); });
        } else if (name == QLatin1String("Math")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral("Add a computed CH A +/-/x CH B trace."));
            m_mathToolButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageMath); });
        } else if (name == QLatin1String("Rulers")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Two time and two voltage cursors, each draggable, plus a "
                "draggable readout showing their values and the difference "
                "between them."));
            m_rulersButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageRulers); });
        } else if (name == QLatin1String("Measure")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Pick a channel and which of its measurements (Vpp, Vrms, "
                "mean, max, min, frequency) to show on the plot."));
            m_measureButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageMeasure); });
        } else if (name == QLatin1String("Settings")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral("Choose where recordings are saved."));
            m_settingsButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageSettings); });
        } else if (name == QLatin1String("Zoom")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Drag a box on the plot to zoom into it. Double click the plot "
                "to go back."));
            m_zoomButton = button;
            connect(button, &QToolButton::toggled, this, [this](bool on) {
                m_view->setZoomMode(on);
                statusBar()->showMessage(on
                    ? QStringLiteral("Zoom: drag a box on the plot")
                    : QStringLiteral("Zoom off"), 2500);
            });
        } else if (name == QLatin1String("Annotations")) {
            // A plain action button, not checkable: each click drops one
            // more note near the middle of the plot, rather than arming a
            // placement mode. Deleting one is done from its own trash icon,
            // not from here.
            button->setToolTip(QStringLiteral(
                "Add a text note with a draggable arrow pointing at the trace."));
            connect(button, &QToolButton::clicked, this, [this] {
                m_view->addAnnotation();
            });
        } else if (name == QLatin1String("References")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Reference waveforms: keep a copy of a trace and overlay it on "
                "the live display. Capture/Live only."));
            m_referencesButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageReferences); });
        } else if (name == QLatin1String("Default")) {
            button->setToolTip(QStringLiteral(
                "Reset the instrument setup (timebase, channels, trigger, "
                "Math, zoom) to its defaults."));
            connect(button, &QToolButton::clicked, this, [this] { resetToDefaults(); });
        } else if (name == QLatin1String("Remote")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Show the plot on a phone or tablet on the same network, and control "
                "the scope from it."));
            m_remoteButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageRemote); });
        } else if (name == QLatin1String("Automotive")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Engine cycle and cylinder timing, relative compression, crank and cam "
                "sensors, and charging and battery tests."));
            m_automotiveButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageAutomotive); });
        } else if (name == QLatin1String("Decoding")) {
            button->setCheckable(true);
            button->setToolTip(QStringLiteral(
                "Protocol decoding: CAN / CAN FD, LIN and K-Line, shown as "
                "bus rows on the plot."));
            m_decodingButton = button;
            connect(button, &QToolButton::clicked, this,
                    [this] { openPage(PageDecoding); });
        } else {
            connect(button, &QToolButton::clicked, this, [this, name] {
                statusBar()->showMessage(
                    QStringLiteral("%1 is not implemented yet.").arg(name), 3000);
            });
        }

        layout->addWidget(button, index / 2, index % 2);
        ++index;
    }
    return grid;
}

QWidget *MainWindow::buildDetailPanel()
{
    m_panelHost = new QFrame;
    m_panelHost->setObjectName(QStringLiteral("panelHost"));
    // minimumWidth stays at 0 permanently — maximumWidth is what
    // openPage()/closePanel() animate, between 0 (closed) and kPanelWidth
    // (open), so the layout can actually shrink the panel down to nothing
    // rather than clipping a still-250px-wide frame.
    m_panelHost->setMinimumWidth(0);
    m_panelHost->setMaximumWidth(0);
    // Scoped by object name: QLabel derives from QFrame, so a bare QFrame rule
    // would draw a border around every label in the panel.
    m_panelHost->setStyleSheet(panelHostStyle());

    m_panelAnimation = new QPropertyAnimation(m_panelHost, "maximumWidth", this);
    m_panelAnimation->setDuration(220);
    m_panelAnimation->setEasingCurve(QEasingCurve::OutCubic);
    // Only a fully-finished CLOSE should hide the panel — a finished OPEN
    // should obviously leave it showing. Checking the width actually landed
    // on, right when this fires, distinguishes the two without needing two
    // separate signal connections for the same slot.
    connect(m_panelAnimation, &QPropertyAnimation::finished, this, [this] {
        if (m_panelHost->maximumWidth() == 0)
            m_panelHost->setVisible(false);
    });

    auto *layout = new QVBoxLayout(m_panelHost);
    // No right margin at panel level: each page carries its own 10 px
    // instead (set below), except a scrolling page, whose scroll bar then
    // sits right at the panel's edge rather than inset over its content.
    // No margins at the top and sides: the title bar runs edge to edge across
    // the panel's top, the content below keeps its own 10 px inset (body).
    layout->setContentsMargins(0, 0, 0, 10);
    layout->setSpacing(8);

    // The title as a bar filled with the accent colour, the title centred and
    // the close button at its right end (PicoScope's panel headers). A space
    // the button's width on the left keeps the title centred on the bar.
    auto *titleBar = new QFrame;
    titleBar->setObjectName(QStringLiteral("panelTitleBar"));
    titleBar->setFixedHeight(kTabStripHeight);   // level with the record tabs beside it
    auto *header = new QHBoxLayout(titleBar);
    header->setContentsMargins(2, 2, 2, 2);   // close button 2 px nearer the edge; even both sides keep the title centred
    header->setSpacing(0);
    m_panelTitle = new QLabel(QStringLiteral("Scope"));
    QFont f = m_panelTitle->font();
    f.setBold(true);
    m_panelTitle->setFont(f);
    m_panelTitle->setAlignment(Qt::AlignCenter);
    header->addSpacing(26);
    header->addWidget(m_panelTitle, 1);

    auto *close = new QToolButton;
    close->setCursor(Qt::PointingHandCursor);
    close->setFixedSize(26, 26);
    close->setIconSize(QSize(15, 15));
    close->setToolTip(QStringLiteral("Close"));
    // Plain Fusion chrome otherwise — nothing else in this app leaves a
    // QToolButton unstyled, and this one showed it. A soft red tint on
    // hover/press is the familiar "this dismisses something" convention
    // (independent of theme, the same way the over-range warning colours
    // are: a self-contained pairing rather than one read against the
    // surrounding chrome), scaled up a little further when pressed so the
    // click itself has visible feedback.
    close->setStyleSheet(QStringLiteral(
        "QToolButton { border: none; border-radius: 6px; background: transparent; }"
        "QToolButton:hover   { background: rgba(255, 70, 70, 40); }"
        "QToolButton:pressed { background: rgba(255, 70, 70, 85); }"));
    auto refreshPanelClose = [this, close, titleBar] {
        const QColor ink = Theme::onAccentBar();
        // Rounded on top, nested inside the panel's rounded corners (1 px
        // less: the panel's border), square below — like the cards' side bars,
        // turned to the top.
        const int radius = Theme::cornerRadius();   // the panel has no border
        titleBar->setStyleSheet(QStringLiteral(
            "QFrame#panelTitleBar { background: %1; border-top-left-radius: %2px;"
            " border-top-right-radius: %2px; border-bottom-left-radius: 0px;"
            " border-bottom-right-radius: 0px; }").arg(Theme::accent().name()).arg(radius));
        m_panelTitle->setStyleSheet(QStringLiteral("color: %1; background: transparent;").arg(ink.name()));
        const QIcon closeIcon = Icons::icon(QStringLiteral("xmark"), ink, 16);
        if (closeIcon.isNull())
            close->setText(QStringLiteral("\u00d7"));
        else
            close->setIcon(closeIcon);
    };
    refreshPanelClose();
    m_themedRefreshers.append(refreshPanelClose);
    connect(close, &QToolButton::clicked, this, &MainWindow::closePanel);
    header->addWidget(close);
    layout->addWidget(titleBar);

    m_panel = new QStackedWidget;
    m_panel->addWidget(buildScopePage());
    m_panel->addWidget(buildTriggerPage());
    m_panel->addWidget(buildChannelPage());
    m_panel->addWidget(buildInstrumentsPage());
    m_panel->addWidget(buildMathPage());
    m_panel->addWidget(buildRulersPage());
    m_panel->addWidget(buildMeasurePage());
    m_panel->addWidget(buildReferencesPage());
    m_panel->addWidget(buildDecodingPage());
    m_panel->addWidget(buildAutomotivePage());
    m_panel->addWidget(buildRemotePage());
    m_panel->addWidget(buildSettingsPage());
    for (int i = 0; i < m_panel->count(); ++i) {
        if (i != PageDecoding && i != PageChannel && i != PageRulers && i != PageAutomotive
            && i != PageRemote)
            m_panel->widget(i)->setContentsMargins(0, 0, 10, 0);
    }
    // PicoScope-style controls: segments, Off | On, toggle grids, compact-card
    // fields with a keypad (panelcontrols.h). The controls keep their code.
    // Decoding's dropdowns without their parenthesised notes ("Auto (from the
    // sync byte)" -> "Auto"): the fields have one width on every page.
    // (Item data drives everything; no decoding code looks items up by text.)
    for (QComboBox *box : m_panel->widget(PageDecoding)->findChildren<QComboBox *>()) {
        static const QRegularExpression note(QStringLiteral("\\s*\\([^)]*\\)"));
        for (int i = 0; i < box->count(); ++i) {
            QString text = box->itemText(i);
            text.remove(note);
            if (!text.trimmed().isEmpty())
                box->setItemText(i, text.trimmed());
        }
    }
    PanelControls::adopt(m_panel, this);
    auto *body = new QHBoxLayout;
    body->setContentsMargins(10, 0, 0, 0);       // pages keep their own 10 px on the right
    body->addWidget(m_panel);
    layout->addLayout(body, 1);

    m_panelHost->setVisible(false);
    return m_panelHost;
}

QWidget *MainWindow::buildScopePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    layout->addWidget(sectionLabel(QStringLiteral("Acquisition")));
    auto *modeRow = new QHBoxLayout;
    m_acqMode = new QButtonGroup(this);
    auto *capture = segment(QStringLiteral("Capture"));
    auto *live    = segment(QStringLiteral("Live"));
    auto *record  = segment(QStringLiteral("Record"));
    m_acqMode->addButton(capture, ModeCapture);
    m_acqMode->addButton(live,    ModeStream);
    m_acqMode->addButton(record,  ModeRecord);
    capture->setChecked(true);
    capture->setToolTip(QStringLiteral("Triggered snapshots of a fixed length."));
    live->setToolTip(QStringLiteral(
        "Continuous acquisition with a rolling display. No trigger."));
    record->setToolTip(QStringLiteral(
        "Streams until the chosen size is filled, then keeps the whole record "
        "to navigate and zoom."));
    for (QPushButton *b : { capture, live, record }) {
        b->setMinimumWidth(0);
        modeRow->addWidget(b);
    }
    modeRow->addStretch();
    layout->addLayout(modeRow);

    // Wrapped in its own widget purely so it can be hidden in Record mode.
    // Record still drives its rate through this same control and the shared
    // applyTimebase() mechanism below, unchanged — only its VISIBILITY is
    // mode-dependent.
    m_timebaseGroup = new QWidget;
    auto *timebaseLayout = new QVBoxLayout(m_timebaseGroup);
    timebaseLayout->setContentsMargins(0, 0, 0, 0);
    timebaseLayout->setSpacing(4);
    timebaseLayout->addWidget(sectionLabel(QStringLiteral("Time per division")));
    m_timebase = new QComboBox;
    for (double t : kTimebases)
        m_timebase->addItem(formatSeconds(t), t);
    m_timebase->setCurrentIndex(kDefaultTimebaseIndex);
    timebaseLayout->addWidget(m_timebase);
    layout->addWidget(m_timebaseGroup);

    // Depth belongs to capture; history and update rate belong to stream.
    m_captureOnly = new QWidget;
    auto *captureLayout = new QVBoxLayout(m_captureOnly);
    captureLayout->setContentsMargins(0, 0, 0, 0);
    captureLayout->setSpacing(4);
    layout->addWidget(m_captureOnly);
    auto *capturePage = captureLayout;
    capturePage->addWidget(sectionLabel(QStringLiteral("Capture depth")));
    m_depth = new QComboBox;
    for (int kb : { 8, 32, 64, 128, 256, 1024 })
        m_depth->addItem(QStringLiteral("%1 KB").arg(kb), kb);
    m_depth->setCurrentIndex(2);   // 64 KB
    m_depth->setToolTip(QStringLiteral(
        "Samples per acquisition. Deeper captures span more of the signal and "
        "allow a higher sample rate at a given timebase, but each one takes "
        "longer to arrive: measured roughly 290 ms fixed plus 3 ms per KB. "
        "64 KB keeps the display live; 256 KB and above suit Single shots."));
    capturePage->addWidget(m_depth);
    capturePage->addWidget(sectionLabel(QStringLiteral("Averaging")));
    m_average = new QComboBox;
    m_average->addItem(QStringLiteral("Off"), 1);
    for (int n : { 2, 4, 8, 16, 32, 64, 128 })
        m_average->addItem(QStringLiteral("%1 frames").arg(n), n);
    m_average->setToolTip(QStringLiteral(
        "Each channel shows the mean of its last N triggered frames: noise falls by the square "
        "root of N, giving detail finer than one ADC step (16 frames: about 2 bits more) at full "
        "bandwidth. Needs a stable trigger (Normal mode is best); starts over when a setting "
        "changes."));
    capturePage->addWidget(m_average);
    connect(m_average, &QComboBox::currentIndexChanged, this, [this](int) {
        ++m_avgGeneration;
        m_avgNoTriggerShown = false;
        refreshCards();
    });

    m_streamOnly = new QWidget;
    auto *streamLayout = new QVBoxLayout(m_streamOnly);
    streamLayout->setContentsMargins(0, 0, 0, 0);
    streamLayout->setSpacing(4);
    layout->addWidget(m_streamOnly);

    streamLayout->addWidget(sectionLabel(QStringLiteral("Sample rate")));
    m_liveRate = new QComboBox;
    for (unsigned int r : kSampleRates)
        m_liveRate->addItem(formatRate(r), r);
    m_liveRate->setCurrentIndex(kDefaultSampleRateIndex);
    m_liveRate->setToolTip(QStringLiteral(
        "Chosen directly, matching the device's own rate list — Live's own "
        "rate, independent of Capture's (which derives its own from its own "
        "timebase instead)."));
    streamLayout->addWidget(m_liveRate);

    streamLayout->addWidget(sectionLabel(QStringLiteral("History")));
    m_history = new QComboBox;
    for (double seconds : { 1.0, 2.0, 5.0, 10.0, 30.0, 60.0 })
        m_history->addItem(QStringLiteral("%1 s").arg(seconds, 0, 'g', 3), seconds);
    m_history->setCurrentIndex(2);   // 5 s
    m_history->setToolTip(QStringLiteral(
        "How much of the past is kept in memory. Stored as float, so a second "
        "costs four bytes per sample per channel."));
    streamLayout->addWidget(m_history);

    streamLayout->addWidget(sectionLabel(QStringLiteral("Update rate")));
    m_updates = new QComboBox;
    for (int hz : { 10, 20, 30, 60 })
        m_updates->addItem(QStringLiteral("%1 /s").arg(hz), hz);
    m_updates->setCurrentIndex(2);   // 30/s
    m_updates->setToolTip(QStringLiteral(
        "Blocks per second from the library. It holds four blocks whatever the "
        "rate, so this sets the delay between signal and screen: 30/s is about "
        "130 ms, 60/s about 65 ms."));
    streamLayout->addWidget(m_updates);
    m_streamOnly->setVisible(false);

    m_recordOnly = new QWidget;
    auto *recordLayout = new QVBoxLayout(m_recordOnly);
    recordLayout->setContentsMargins(0, 0, 0, 0);
    recordLayout->setSpacing(4);
    layout->addWidget(m_recordOnly);

    recordLayout->addWidget(sectionLabel(QStringLiteral("Sample rate")));
    m_recordRate = new QComboBox;
    for (unsigned int r : kSampleRates)
        m_recordRate->addItem(formatRate(r), r);
    m_recordRate->setCurrentIndex(kDefaultSampleRateIndex);
    m_recordRate->setToolTip(QStringLiteral(
        "Chosen directly, matching the device's own rate list — this is the "
        "rate the NEXT recording runs at. An already-finished record keeps "
        "whatever rate it was actually captured with."));
    recordLayout->addWidget(m_recordRate);

    recordLayout->addWidget(sectionLabel(QStringLiteral("Record size")));
    m_recordSize = new QComboBox;
    for (int mb : { 64, 128, 256, 512, 1024, 2048, 3072, 5120, 7168, 10240 })
        m_recordSize->addItem(mb < 1024 ? QStringLiteral("%1 MB").arg(mb)
                                        : QStringLiteral("%1 GB").arg(mb / 1024), mb);
    m_recordSize->setCurrentIndex(1);
    m_recordSize->setToolTip(QStringLiteral(
        "Total raw record size at two bytes per sample, as the vendor's app "
        "counts it. Written straight to disk (reserved when recording starts); "
        "drives formatted FAT32, as many USB sticks are, can't hold files over 4 GB."));
    recordLayout->addWidget(m_recordSize);

    recordLayout->addWidget(sectionLabel(QStringLiteral("Keep")));
    auto *persistRow = new QHBoxLayout;
    m_recordPersistGroup = new QButtonGroup(this);
    auto *tempSeg = segment(QStringLiteral("Temporary"));
    auto *permSeg = segment(QStringLiteral("Permanent"));
    m_recordPersistGroup->addButton(tempSeg, 0);
    m_recordPersistGroup->addButton(permSeg, 1);
    permSeg->setChecked(true);   // deletion is opt-in, not opt-out
    tempSeg->setToolTip(QStringLiteral(
        "Deleted from disk when the app closes \u2014 not when the recording "
        "itself stops, so it is still there to review until then."));
    permSeg->setToolTip(QStringLiteral(
        "Stays on disk after the app closes, same as any other file."));
    persistRow->addWidget(tempSeg);
    persistRow->addWidget(permSeg);
    recordLayout->addLayout(persistRow);

    m_recordOnly->setVisible(false);

    layout->addWidget(sectionLabel(QStringLiteral("Acquisition")));
    m_sampleInfo = new QLabel;
    m_sampleInfo->setWordWrap(true);
    m_sampleInfo->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    layout->addWidget(m_sampleInfo);

    auto *note = new QLabel(QStringLiteral(
        "Capture and Live pick time/div directly; Record picks its rate "
        "directly instead, since there is nothing to navigate before it runs."));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    layout->addWidget(note);

    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildTriggerPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    layout->addWidget(sectionLabel(QStringLiteral("Mode")));
    auto *modeRow = new QHBoxLayout;
    m_trigModeGroup = new QButtonGroup(this);
    auto *none   = segment(QStringLiteral("None"));
    auto *automatic = segment(QStringLiteral("Auto"));
    auto *normal = segment(QStringLiteral("Normal"));
    m_trigModeGroup->addButton(none,      ModeNone);
    m_trigModeGroup->addButton(automatic, ModeAuto);
    m_trigModeGroup->addButton(normal,    ModeNormal);
    none->setToolTip(QStringLiteral("Free run. No trigger, the trace will drift."));
    automatic->setToolTip(QStringLiteral(
        "Triggers on the edge, and sweeps anyway when no edge arrives."));
    normal->setToolTip(QStringLiteral(
        "Only shows frames where the edge is confirmed. Holds the last trace "
        "otherwise."));
    // None by default: an armed mode with the default 0 V threshold on an
    // unconnected channel shows nothing, and the app looks broken on launch.
    none->setChecked(true);
    modeRow->addWidget(none);
    modeRow->addWidget(automatic);
    modeRow->addWidget(normal);
    modeRow->addStretch();
    layout->addLayout(modeRow);

    layout->addWidget(sectionLabel(QStringLiteral("Auto wait")));
    m_autoWait = new QComboBox;
    m_autoWait->addItem(QStringLiteral("Short (1 screen)"), 0);
    m_autoWait->addItem(QStringLiteral("Medium (2 screens)"), 1);
    m_autoWait->addItem(QStringLiteral("Scope default (slowest)"), 2);
    m_autoWait->setToolTip(QStringLiteral(
        "In Auto mode, how long to wait for a trigger before sweeping anyway. The scope's "
        "own wait is about 0.2 s, which limits the frame rate to 3-4 per second when no "
        "edge arrives. Short keeps it lively; for signals slower than about one cycle per "
        "screen, choose Medium or the scope default."));
    layout->addWidget(m_autoWait);
    connect(m_autoWait, &QComboBox::currentIndexChanged, this, [this](int) { updateAutoTriggerWait(); });

    layout->addWidget(sectionLabel(QStringLiteral("Source")));
    m_trigSource = new QComboBox;
    layout->addWidget(m_trigSource);

    layout->addWidget(sectionLabel(QStringLiteral("Direction")));
    auto *dirRow = new QHBoxLayout;
    m_trigDirGroup = new QButtonGroup(this);
    auto *rising  = segment(QStringLiteral("Rising"));
    auto *falling = segment(QStringLiteral("Falling"));
    m_trigDirGroup->addButton(rising,  0);
    m_trigDirGroup->addButton(falling, 1);
    rising->setChecked(true);
    dirRow->addWidget(rising);
    dirRow->addWidget(falling);
    dirRow->addStretch();
    layout->addLayout(dirRow);

    layout->addWidget(sectionLabel(QStringLiteral("Threshold")));
    m_trigLevel = new QDoubleSpinBox;
    m_trigLevel->setRange(-100.0, 100.0);
    m_trigLevel->setDecimals(3);
    m_trigLevel->setSingleStep(0.05);
    m_trigLevel->setSuffix(QStringLiteral(" V"));
    layout->addWidget(m_trigLevel);

    m_levelToMid = new QPushButton(QStringLiteral("Set to signal midpoint"));
    m_levelToMid->setCursor(Qt::PointingHandCursor);
    m_levelToMid->setToolTip(QStringLiteral(
        "Move the threshold to the middle of the source channel's last capture."));
    layout->addWidget(m_levelToMid);

    layout->addWidget(sectionLabel(QStringLiteral("Pre-trigger")));
    m_preTrigger = new QSpinBox;
    m_preTrigger->setRange(5, 95);   // documented range in the SDK guide
    m_preTrigger->setValue(50);
    m_preTrigger->setSuffix(QStringLiteral(" %"));
    layout->addWidget(m_preTrigger);

    layout->addStretch();
    return page;
}

// Mode selector only. Selecting anything other than Scope marks the choice and
// says so; none of the modes are implemented.
// A single computed trace: sourceA <op> sourceB, drawn in its own colour with
// an automatically-scaled vertical range. Enable/disable plus which channels
// and operation feed it.
QWidget *MainWindow::buildMathPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_mathEnabled = new QCheckBox(QStringLiteral("Show math channel"));
    layout->addWidget(m_mathEnabled);

    m_mathColour = new QPushButton(QStringLiteral("Trace colour"));
    m_mathColour->setIcon(uiIcon(this, QStringLiteral("eye"), QStyle::SP_DesktopIcon, Theme::text()));
    m_mathColour->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_mathColour);

    layout->addWidget(sectionLabel(QStringLiteral("Volts per page")));
    m_mathVoltsPage = new QComboBox;
    m_mathVoltsPage->addItem(QStringLiteral("Auto"), -1.0);
    for (double v : kVoltsPerPage)
        m_mathVoltsPage->addItem(QStringLiteral("\u00b1%1").arg(formatVolts(v)), v);
    // Fixed by default, same as a real channel — "Auto" refits every frame as
    // the signal changes, which is exactly what looked like a scale that
    // would not hold still, and is why it should be something the user
    // opts into rather than the default.
    m_mathVoltsPage->setCurrentIndex(kDefaultVoltsIndex + 1);   // +1: "Auto" occupies index 0 here
    m_mathVoltsPage->setToolTip(QStringLiteral(
        "Auto fits whatever is on screen to the graticule every frame. Picking "
        "a fixed value instead lets you drag the scale beside CH1's to "
        "reposition it, the same as a real channel."));
    layout->addWidget(m_mathVoltsPage);

    layout->addWidget(sectionLabel(QStringLiteral("Operation")));
    m_mathOp = new QComboBox;
    // Two sources first, then one; the units follow the probes (V x A = W,
    // V / A = ohms, dA/dt in V/s ...). See mathops.h.
    for (int op : { MathOps::Add, MathOps::Subtract, MathOps::Multiply, MathOps::Divide,
                    MathOps::Average, MathOps::Maximum, MathOps::Minimum })
        m_mathOp->addItem(QString::fromUtf8(MathOps::menuText(op)), op);
    m_mathOp->insertSeparator(m_mathOp->count());
    for (int op : { MathOps::Negate, MathOps::Absolute, MathOps::Square, MathOps::SquareRoot,
                    MathOps::Reciprocal, MathOps::Decibel, MathOps::Derivative,
                    MathOps::Integral, MathOps::Scale })
        m_mathOp->addItem(QString::fromUtf8(MathOps::menuText(op)), op);
    m_mathOp->setToolTip(QStringLiteral(
        "Works on what each channel measures (probe applied). Units follow the "
        "probes: a voltage channel times a current channel is power in W, "
        "divided is resistance in \u03a9. dA/dt amplifies noise: Math's filter helps."));
    layout->addWidget(m_mathOp);

    m_mathScaleRow = new QWidget;
    {
        auto *grid = new QGridLayout(m_mathScaleRow);
        grid->setContentsMargins(0, 0, 0, 0);
        m_mathScaleK = new QDoubleSpinBox;
        m_mathScaleC = new QDoubleSpinBox;
        for (QDoubleSpinBox *box : { m_mathScaleK, m_mathScaleC }) {
            box->setRange(-1e6, 1e6);
            box->setDecimals(4);
            box->setMinimumWidth(40);
        }
        m_mathScaleK->setValue(1.0);
        m_mathScaleK->setSingleStep(0.1);
        m_mathScaleC->setSingleStep(0.1);
        grid->addWidget(new QLabel(QStringLiteral("k")), 0, 0);
        grid->addWidget(m_mathScaleK, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("c")), 1, 0);
        grid->addWidget(m_mathScaleC, 1, 1);
        grid->setColumnStretch(1, 1);
    }
    layout->addWidget(m_mathScaleRow);

    layout->addWidget(sectionLabel(QStringLiteral("Source A")));
    m_mathSourceA = new QComboBox;
    layout->addWidget(m_mathSourceA);

    m_mathSourceBRow = new QWidget;
    {
        auto *bLayout = new QVBoxLayout(m_mathSourceBRow);
        bLayout->setContentsMargins(0, 0, 0, 0);
        bLayout->setSpacing(4);
        bLayout->addWidget(sectionLabel(QStringLiteral("Source B")));
        m_mathSourceB = new QComboBox;
        bLayout->addWidget(m_mathSourceB);
    }
    layout->addWidget(m_mathSourceBRow);

    refreshMathSources(false);   // CH2 for B by default, so A and B differ

    connect(m_mathVoltsPage, &QComboBox::currentIndexChanged, this, [this](int) {
        // A fresh scale starts recentred — carrying over a position meant for
        // a different per-division value would put the trace somewhere
        // arbitrary rather than where the new scale actually centres it.
        m_view->setMathPositionVolts(0.0);
        applyMathChannel();
    });
    connect(m_mathColour, &QPushButton::clicked, this, [this] {
        const QColor current = m_view->mathColor();
        const QColor picked = VsxDialogs::pickColour(current, this,
                                                      QStringLiteral("Math channel colour"));
        if (!picked.isValid())
            return;
        m_view->setMathColor(picked);
        m_nav->setMathColor(picked);
        QPalette pal = m_mathColour->palette();
        pal.setColor(QPalette::ButtonText, picked);
        m_mathColour->setPalette(pal);
        if (m_rulersMathCheck)
            setRulerCheckboxColor(m_rulersMathCheck, picked);
        m_mathCard->setAccent(picked);
        updateSpectrumSources();   // a Math trace in Spectrum should pick this up immediately
        refreshCards();
    });
    layout->addWidget(sectionLabel(QStringLiteral("Filter")));
    {
        // The channels' filters, on Math's result (its sources keep their own).
        m_mathFilterType = new QComboBox;
        m_mathFilterType->addItem(QStringLiteral("Off"), int(ScopeView::FilterOff));
        m_mathFilterType->addItem(QStringLiteral("Low-pass"), int(ScopeView::FilterLowPass));
        m_mathFilterType->addItem(QStringLiteral("High-pass"), int(ScopeView::FilterHighPass));
        m_mathFilterType->addItem(QStringLiteral("Band-pass"), int(ScopeView::FilterBandPass));
        m_mathFilterType->addItem(QStringLiteral("Band-stop"), int(ScopeView::FilterBandStop));
        m_mathFilterType->setToolTip(QStringLiteral(
            "Filters the Math result (its sources keep their own filters). Zero-phase; "
            "cutoffs are -3 dB; the counters step 1-2-5."));
        layout->addWidget(m_mathFilterType);
        const auto counterRow = [&](QLabel **label, QDoubleSpinBox **box, double value) {
            auto *row = new QWidget;
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            *label = new QLabel;
            *box = new FrequencySpinBox;
            (*box)->setValue(value);
            rowLayout->addWidget(*label);
            rowLayout->addWidget(*box, 1);
            layout->addWidget(row);
            return row;
        };
        m_mathFilterLowRow = counterRow(&m_mathFilterLowLabel, &m_mathFilterLowHz, 10.0);
        m_mathFilterHighRow = counterRow(&m_mathFilterHighLabel, &m_mathFilterHighHz, 100e3);
        const auto apply = [this] {
            const int type = m_mathFilterType->currentData().toInt();
            const bool band = type == ScopeView::FilterBandPass || type == ScopeView::FilterBandStop;
            m_mathFilterLowRow->setVisible(type == ScopeView::FilterHighPass || band);
            m_mathFilterHighRow->setVisible(type == ScopeView::FilterLowPass || band);
            m_mathFilterLowLabel->setText(band ? QStringLiteral("From") : QStringLiteral("Cutoff"));
            m_mathFilterHighLabel->setText(band ? QStringLiteral("To") : QStringLiteral("Cutoff"));
            if (m_view)
                m_view->setMathFilter(type, m_mathFilterLowHz->value(), m_mathFilterHighHz->value());
        };
        connect(m_mathFilterType, &QComboBox::currentIndexChanged, this, [apply](int) { apply(); });
        connect(m_mathFilterLowHz, &QDoubleSpinBox::valueChanged, this, [apply](double) { apply(); });
        connect(m_mathFilterHighHz, &QDoubleSpinBox::valueChanged, this, [apply](double) { apply(); });
        apply();
    }
    connect(m_mathScaleK, &QDoubleSpinBox::valueChanged, this, [this](double) { applyMathChannel(); });
    connect(m_mathScaleC, &QDoubleSpinBox::valueChanged, this, [this](double) { applyMathChannel(); });

    connect(m_mathEnabled, &QCheckBox::toggled, this, [this](bool) { applyMathChannel(); });
    connect(m_mathOp,      &QComboBox::currentIndexChanged, this, [this](int) { applyMathChannel(); });
    connect(m_mathSourceA, &QComboBox::currentIndexChanged, this, [this](int) { applyMathChannel(); });
    connect(m_mathSourceB, &QComboBox::currentIndexChanged, this, [this](int) { applyMathChannel(); });

    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildReferencesPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    layout->addWidget(sectionLabel(QStringLiteral("Add")));
    auto *addRow = new QWidget;
    auto *addLayout = new QHBoxLayout(addRow);
    addLayout->setContentsMargins(0, 0, 0, 0);
    addLayout->setSpacing(4);
    for (int ch = 0; ch < 4; ++ch) {
        auto *button = new QPushButton(QStringLiteral("CH%1").arg(ch + 1));
        button->setCursor(Qt::PointingHandCursor);
        m_referenceAddButtons[ch] = button;
        addLayout->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, ch] {
            const int slot = m_view->addReferenceFromChannel(ch);
            if (slot < 0) {
                statusBar()->showMessage(QStringLiteral(
                    "Both reference slots are already in use \u2014 remove one first."), 3000);
                return;
            }
            enforceReferenceGutterBudget();
            m_referenceSelectedSlot = slot;
            refreshReferencesPanel();
        });
    }
    layout->addWidget(addRow);

    m_referenceFileButton = new QPushButton(QStringLiteral("File"));
    m_referenceFileButton->setCursor(Qt::PointingHandCursor);
    m_referenceFileButton->setToolTip(QStringLiteral(
        "Load a previously saved .vsxref reference waveform."));
    layout->addWidget(m_referenceFileButton);
    connect(m_referenceFileButton, &QPushButton::clicked, this, [this] {
        openReferenceFile();
    });

    layout->addWidget(sectionLabel(QStringLiteral("Manage")));
    auto *manageRow = new QWidget;
    auto *manageLayout = new QHBoxLayout(manageRow);
    manageLayout->setContentsMargins(0, 0, 0, 0);
    manageLayout->setSpacing(4);
    auto *slotGroup = new QButtonGroup(page);
    slotGroup->setExclusive(true);
    for (int slot = 0; slot < ScopeView::MaxReferences; ++slot) {
        auto *button = new QPushButton(QStringLiteral("Ref %1").arg(slot + 1));
        button->setCheckable(true);
        button->setCursor(Qt::PointingHandCursor);
        m_referenceSlotButtons[slot] = button;
        slotGroup->addButton(button, slot);
        manageLayout->addWidget(button);
    }
    layout->addWidget(manageRow);
    connect(slotGroup, &QButtonGroup::idClicked, this, [this](int slot) {
        m_referenceSelectedSlot = slot;
        refreshReferencesPanel();
    });

    auto *saveRemoveRow = new QWidget;
    auto *saveRemoveLayout = new QHBoxLayout(saveRemoveRow);
    saveRemoveLayout->setContentsMargins(0, 0, 0, 0);
    saveRemoveLayout->setSpacing(4);
    m_referenceSaveButton = new QPushButton(QStringLiteral("Save"));
    m_referenceRemoveButton = new QPushButton(QStringLiteral("Remove"));
    for (QPushButton *b : { m_referenceSaveButton, m_referenceRemoveButton })
        b->setCursor(Qt::PointingHandCursor);
    saveRemoveLayout->addWidget(m_referenceSaveButton);
    saveRemoveLayout->addWidget(m_referenceRemoveButton);
    layout->addWidget(saveRemoveRow);
    connect(m_referenceSaveButton, &QPushButton::clicked, this, [this] {
        saveReferenceToFile(m_referenceSelectedSlot);
    });
    connect(m_referenceRemoveButton, &QPushButton::clicked, this, [this] {
        m_view->removeReference(m_referenceSelectedSlot);
        // Deliberately does NOT bring Math back even if removing this
        // reference frees up a gutter — see enforceReferenceGutterBudget()'s
        // own comment for why staying off until turned back on by hand is
        // the wanted behaviour, not an oversight here.
        refreshReferencesPanel();
    });

    layout->addWidget(sectionLabel(QStringLiteral("Name")));
    m_referenceNameEdit = new QLineEdit;
    layout->addWidget(m_referenceNameEdit);
    connect(m_referenceNameEdit, &QLineEdit::editingFinished, this, [this] {
        m_view->setReferenceName(m_referenceSelectedSlot, m_referenceNameEdit->text());
        refreshReferencesPanel();
    });

    m_referenceColourButton = new QPushButton(QStringLiteral("Trace colour"));   // as the channels' and Math's
    m_referenceColourButton->setIcon(uiIcon(this, QStringLiteral("eye"), QStyle::SP_DesktopIcon, Theme::text()));
    m_referenceColourButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_referenceColourButton);
    connect(m_referenceColourButton, &QPushButton::clicked, this, [this] {
        const int slot = m_referenceSelectedSlot;
        if (!m_view->referenceOccupied(slot))
            return;
        const QColor current = m_view->referenceColor(slot);
        const QColor picked = VsxDialogs::pickColour(current, this,
                                                      QStringLiteral("Reference colour"));
        if (!picked.isValid())
            return;
        m_view->setReferenceColor(slot, picked);
        refreshReferencesPanel();
    });

    layout->addWidget(sectionLabel(QStringLiteral("Vertical scale")));
    m_referenceVoltsPerDiv = new QDoubleSpinBox;
    m_referenceVoltsPerDiv->setRange(0.001, 1000.0);
    m_referenceVoltsPerDiv->setDecimals(3);
    m_referenceVoltsPerDiv->setSuffix(QStringLiteral(" V/div"));
    layout->addWidget(m_referenceVoltsPerDiv);
    connect(m_referenceVoltsPerDiv, &QDoubleSpinBox::valueChanged, this, [this](double v) {
        m_view->setReferenceVoltsPerDiv(m_referenceSelectedSlot, v);
    });

    layout->addWidget(sectionLabel(QStringLiteral("Vertical offset")));
    m_referencePositionPct = new QDoubleSpinBox;
    m_referencePositionPct->setRange(-900.0, 900.0);
    m_referencePositionPct->setDecimals(0);
    m_referencePositionPct->setSuffix(QStringLiteral(" %"));
    m_referencePositionPct->setToolTip(QStringLiteral(
        "Percent of this reference's own volts/division \u2014 independent of "
        "any channel's own offset."));
    layout->addWidget(m_referencePositionPct);
    connect(m_referencePositionPct, &QDoubleSpinBox::valueChanged, this, [this](double pct) {
        const double perDiv = m_view->referenceVoltsPerDiv(m_referenceSelectedSlot);
        if (perDiv > 0.0)
            m_view->setReferencePositionVolts(m_referenceSelectedSlot, pct / 100.0 * perDiv);
    });

    layout->addWidget(sectionLabel(QStringLiteral("Delay")));
    m_referenceDelay = new QDoubleSpinBox;
    m_referenceDelay->setRange(-10.0, 10.0);
    m_referenceDelay->setDecimals(6);
    m_referenceDelay->setSuffix(QStringLiteral(" s"));
    m_referenceDelay->setToolTip(QStringLiteral(
        "Shifts this reference later (positive) or earlier (negative) "
        "relative to the live trace."));
    layout->addWidget(m_referenceDelay);
    connect(m_referenceDelay, &QDoubleSpinBox::valueChanged, this, [this](double delay) {
        m_view->setReferenceDelay(m_referenceSelectedSlot, delay);
    });

    layout->addStretch();
    refreshReferencesPanel();
    return page;
}

QWidget *MainWindow::buildInstrumentsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    layout->addWidget(sectionLabel(QStringLiteral("Mode")));

    m_instrumentMode = new QButtonGroup(this);
    const struct { const char *text; const char *fa; QStyle::StandardPixmap fallback; }
    kModes[] = {
        { "Scope",       "wave-square",  QStyle::SP_FileDialogDetailedView },
        { "Spectrum",    "chart-simple", QStyle::SP_FileDialogInfoView },
        { "XY",          "chart-line",   QStyle::SP_FileDialogContentsView },
        { "Persistence", "layer-group",  QStyle::SP_FileDialogListView },
    };
    int id = 0;
    for (const auto &mode : kModes) {
        auto *button = segment(QString::fromLatin1(mode.text));
        button->setMinimumWidth(0);
        m_instrumentMode->addButton(button, id);
        if (id != 0) {
            button->setToolTip(QStringLiteral(
                "Only available in Capture mode \u2014 this analyzes a "
                "single, settled capture, not a continuously streaming or "
                "recording signal."));
        }
        // Coloured to match this button's own text in each state, not the
        // icon's usual default: an ordinary QPushButton's text here is
        // Theme::text() (nothing in globalStyleSheet()'s QPushButton rule
        // sets an explicit colour, so it falls back to the palette), and
        // QPushButton:checked's text is explicitly onAccent() — matching
        // the icon to whichever is showing right now instead of leaving it
        // at accentSoft() by default. Held by shared_ptr, not by value, so
        // the refresher below (run again by applyTheme() on a live theme
        // toggle) can regenerate both in place and have the toggled
        // handler's own already-captured copies see the change too — a
        // plain by-value QIcon capture there would keep referring to
        // whatever colour was current when the connection was first made.
        // Null when Font Awesome is unavailable — the toggled handler
        // falls back to normalIcon in that case, same as segment icons
        // already do everywhere else in this app.
        auto normalIcon = std::make_shared<QIcon>();
        auto checkedIcon = std::make_shared<QIcon>();
        auto refreshModeIcon = [this, button, normalIcon, checkedIcon,
                                 fa = QString::fromLatin1(mode.fa), fallback = mode.fallback] {
            *normalIcon = uiIcon(this, fa, fallback, Theme::text());
            *checkedIcon = Icons::icon(fa, Theme::onAccent(), 30);
            button->setIcon(button->isChecked() && !checkedIcon->isNull()
                                 ? *checkedIcon : *normalIcon);
        };
        refreshModeIcon();
        m_themedRefreshers.append(refreshModeIcon);
        connect(button, &QPushButton::toggled, this, [button, normalIcon, checkedIcon](bool checked) {
            button->setIcon(checked && !checkedIcon->isNull() ? *checkedIcon : *normalIcon);
        });
        if (id == 0)
            button->setChecked(true);   // toggled() fires here, swapping in the black icon
        layout->addWidget(button);
        ++id;
    }

    // Spectrum's own controls — up to SpectrumView::MaxTraces (2) source
    // channels at once, real channels or Math, a window function, and a
    // unit — hidden outside Spectrum mode so Scope mode stays as
    // uncluttered as it always was.
    m_spectrumControls = new QWidget;
    auto *specLayout = new QVBoxLayout(m_spectrumControls);
    specLayout->setContentsMargins(0, 4, 0, 0);
    specLayout->setSpacing(4);

    specLayout->addWidget(sectionLabel(QStringLiteral("Source (up to 2)")));
    auto *specChannelRow = new QHBoxLayout;
    // Not exclusive: up to two of these can be checked at once, enforced by
    // hand in the idToggled handler below rather than by QButtonGroup
    // itself, which only ever knows how to enforce "at most one".
    m_spectrumChannelGroup = new QButtonGroup(this);
    m_spectrumChannelGroup->setExclusive(false);
    for (int ch = 0; ch < 4; ++ch) {
        auto *seg = segment(QStringLiteral("CH%1").arg(ch + 1));
        m_spectrumChannelGroup->addButton(seg, ch);
        m_spectrumChannelSegments[ch] = seg;
        specChannelRow->addWidget(seg);
    }
    m_spectrumMathSegment = segment(QStringLiteral("Math"));
    m_spectrumChannelGroup->addButton(m_spectrumMathSegment, ScopeView::RulersMathChannel);
    specChannelRow->addWidget(m_spectrumMathSegment);
    specChannelRow->addStretch();
    specLayout->addLayout(specChannelRow);
    if (auto *ch1 = m_spectrumChannelGroup->button(0))
        ch1->setChecked(true);   // a sensible starting point, one source already selected
    connect(m_spectrumChannelGroup, &QButtonGroup::idToggled, this,
            [this](int toggledId, bool checked) {
        if (checked) {
            int activeCount = 0;
            for (QAbstractButton *b : m_spectrumChannelGroup->buttons()) {
                if (b->isChecked())
                    ++activeCount;
            }
            if (activeCount > SpectrumView::MaxTraces) {
                // This button just became the (MaxTraces+1)-th — refuse it
                // rather than silently evicting whichever was checked first.
                if (auto *b = m_spectrumChannelGroup->button(toggledId)) {
                    b->blockSignals(true);
                    b->setChecked(false);
                    b->blockSignals(false);
                }
                statusBar()->showMessage(QStringLiteral(
                    "Spectrum shows at most %1 sources at once — turn one off first.")
                    .arg(SpectrumView::MaxTraces), 3000);
                return;
            }
        }
        updateSpectrumSources();
    });

    specLayout->addWidget(sectionLabel(QStringLiteral("Window")));
    m_spectrumWindowCombo = new QComboBox;
    m_spectrumWindowCombo->addItem(QStringLiteral("Hann"));
    m_spectrumWindowCombo->addItem(QStringLiteral("Hamming"));
    m_spectrumWindowCombo->addItem(QStringLiteral("Blackman"));
    m_spectrumWindowCombo->addItem(QStringLiteral("Flat Top (amplitude accuracy)"));
    specLayout->addWidget(m_spectrumWindowCombo);
    connect(m_spectrumWindowCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        static const SpectrumView::WindowFunction kWindows[] = {
            SpectrumView::WindowFunction::Hann,     SpectrumView::WindowFunction::Hamming,
            SpectrumView::WindowFunction::Blackman, SpectrumView::WindowFunction::FlatTop,
        };
        if (index >= 0 && index < 4)
            m_spectrum->setWindow(kWindows[index]);
    });

    specLayout->addWidget(sectionLabel(QStringLiteral("Unit")));
    m_spectrumUnitCombo = new QComboBox;
    m_spectrumUnitCombo->addItem(QStringLiteral("dBV"));
    m_spectrumUnitCombo->addItem(QStringLiteral("dBm (50 \u03a9)"));
    specLayout->addWidget(m_spectrumUnitCombo);
    connect(m_spectrumUnitCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_spectrum->setUnit(index == 1 ? SpectrumView::Unit::DBm : SpectrumView::Unit::DBV);
    });

    specLayout->addWidget(sectionLabel(QStringLiteral("Display")));
    m_spectrumDisplayModeCombo = new QComboBox;
    m_spectrumDisplayModeCombo->addItem(QStringLiteral("Normal"));
    m_spectrumDisplayModeCombo->addItem(QStringLiteral("Average"));
    specLayout->addWidget(m_spectrumDisplayModeCombo);
    connect(m_spectrumDisplayModeCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_spectrum->setDisplayMode(index == 1 ? SpectrumView::DisplayMode::Average
                                              : SpectrumView::DisplayMode::Normal);
    });

    specLayout->addWidget(sectionLabel(QStringLiteral("Scale")));
    m_spectrumYScaleCombo = new QComboBox;
    m_spectrumYScaleCombo->addItem(QStringLiteral("Logarithmic"));
    m_spectrumYScaleCombo->addItem(QStringLiteral("Linear"));
    specLayout->addWidget(m_spectrumYScaleCombo);
    connect(m_spectrumYScaleCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        const bool linear = (index == 1);
        m_spectrum->setYScale(linear ? SpectrumView::YScale::Linear : SpectrumView::YScale::Log);
        // Linear shows plain volts, bypassing dB entirely — the unit choice
        // has nothing left to act on until Logarithmic is picked again.
        m_spectrumUnitCombo->setEnabled(!linear);
    });

    m_spectrumControls->setVisible(false);
    layout->addWidget(m_spectrumControls);

    // XY's own controls — one exclusive channel choice per axis (never
    // zero, never more than one, unlike Spectrum's up-to-2 multi-select),
    // hidden outside XY mode.
    m_xyControls = new QWidget;
    auto *xyLayout = new QVBoxLayout(m_xyControls);
    xyLayout->setContentsMargins(0, 4, 0, 0);
    xyLayout->setSpacing(4);

    xyLayout->addWidget(sectionLabel(QStringLiteral("X channel")));
    auto *xRow = new QHBoxLayout;
    m_xyXGroup = new QButtonGroup(this);
    for (int ch = 0; ch < 4; ++ch) {
        auto *seg = segment(QStringLiteral("CH%1").arg(ch + 1));
        m_xyXGroup->addButton(seg, ch);
        m_xyXSegments[ch] = seg;
        xRow->addWidget(seg);
    }
    xRow->addStretch();
    xyLayout->addLayout(xRow);
    if (auto *b = m_xyXGroup->button(0))
        b->setChecked(true);
    connect(m_xyXGroup, &QButtonGroup::idClicked, this, [this](int ch) {
        m_xyChannelX = ch;
        m_xy->clear();   // the old pairing would otherwise linger
        updateXYColors();
        updateXYRanges();
    });

    xyLayout->addWidget(sectionLabel(QStringLiteral("Y channel")));
    auto *yRow = new QHBoxLayout;
    m_xyYGroup = new QButtonGroup(this);
    for (int ch = 0; ch < 4; ++ch) {
        auto *seg = segment(QStringLiteral("CH%1").arg(ch + 1));
        m_xyYGroup->addButton(seg, ch);
        m_xySegments[ch] = seg;
        yRow->addWidget(seg);
    }
    yRow->addStretch();
    xyLayout->addLayout(yRow);
    if (auto *b = m_xyYGroup->button(1))
        b->setChecked(true);   // CH2 by default, so X and Y differ from the start
    connect(m_xyYGroup, &QButtonGroup::idClicked, this, [this](int ch) {
        m_xyChannelY = ch;
        m_xy->clear();
        updateXYColors();
        updateXYRanges();
    });

    m_xyControls->setVisible(false);
    layout->addWidget(m_xyControls);

    // Persistence draws on the main plot itself, so its only control is a
    // way to start the accumulation over by hand; it also restarts on its
    // own whenever a setting moves where samples land on screen.
    m_persistenceControls = new QWidget;
    {
        auto *pl = new QVBoxLayout(m_persistenceControls);
        pl->setContentsMargins(0, 6, 0, 0);
        pl->setSpacing(4);
        auto *clear = new QPushButton(QStringLiteral("Clear persistence"));
        clear->setCursor(Qt::PointingHandCursor);
        clear->setToolTip(QStringLiteral(
            "Start accumulating again from the next capture. Also happens "
            "automatically when the timebase, volts/div, position, probe or "
            "zoom changes."));
        pl->addWidget(clear);
        connect(clear, &QPushButton::clicked, this, [this] { m_view->clearPersistence(); });
    }
    m_persistenceControls->setVisible(false);
    layout->addWidget(m_persistenceControls);

    connect(m_instrumentMode, &QButtonGroup::idClicked, this, [this](int id) {
        if (id == 0) {
            setInstrumentMode(InstrumentMode::Scope);
            return;
        }
        if (id == 1) {
            setInstrumentMode(InstrumentMode::Spectrum);
            return;
        }
        if (id == 2) {
            setInstrumentMode(InstrumentMode::XY);
            return;
        }
        if (id == 3) {
            setInstrumentMode(InstrumentMode::Persistence);
            return;
        }
        setInstrumentMode(InstrumentMode::Scope);
        statusBar()->showMessage(
            QStringLiteral("That mode is not implemented yet."), 3000);
        if (auto *scope = m_instrumentMode->button(0))
            scope->setChecked(true);
    });

    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildChannelPage()
{
    m_channelPages = new QStackedWidget;

    for (int ch = 0; ch < 4; ++ch) {
        auto *page = new QWidget;
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(4);

        ChannelControls &c = m_channel[ch];

        auto *modeRow = new QHBoxLayout;
        c.mode = new QButtonGroup(this);
        auto *off    = segment(QStringLiteral("Off"));
        auto *autoB  = segment(QStringLiteral("Auto"));
        auto *manual = segment(QStringLiteral("Manual"));
        c.mode->addButton(off,    ChannelOff);
        c.mode->addButton(autoB,  ChannelAuto);
        c.mode->addButton(manual, ChannelManual);
        (ch == 0 ? manual : off)->setChecked(true);
        autoB->setToolTip(QStringLiteral(
            "Picks the volts per page automatically from the measured signal."));
        for (QPushButton *b : { off, autoB, manual })
            modeRow->addWidget(b);
        modeRow->addStretch();
        layout->addLayout(modeRow);

        c.colour = new QPushButton(QStringLiteral("Trace colour"));
        c.colour->setIcon(uiIcon(this, QStringLiteral("eye"), QStyle::SP_DesktopIcon, Theme::text()));
        c.colour->setCursor(Qt::PointingHandCursor);
        layout->addWidget(c.colour);

        c.voltsPageTitle = sectionLabel(QStringLiteral("Volts per page"));
        layout->addWidget(c.voltsPageTitle);
        c.voltsPage = new QComboBox;
        for (double v : kVoltsPerPage)
            c.voltsPage->addItem(QStringLiteral("\u00b1%1").arg(formatVolts(v)), v);
        c.voltsPage->setCurrentIndex(kDefaultVoltsIndex);
        c.voltsPage->setToolTip(QStringLiteral(
            "Half the graticule height. +/-5 V shows -5 V to +5 V, so 1 V per "
            "division across 10 divisions."));
        layout->addWidget(c.voltsPage);

        layout->addWidget(sectionLabel(QStringLiteral("Scale")));
        c.scale = new QComboBox;
        for (double v : kDisplayScales)
            c.scale->addItem(QStringLiteral("x%1").arg(v, 0, 'g', 2), v);
        c.scale->setCurrentIndex(3);   // x1
        c.scale->setToolTip(QStringLiteral(
            "Vertical zoom of the display. It magnifies the drawing, not the "
            "signal, so zooming a trace off the page is not an over-range."));
        layout->addWidget(c.scale);

        layout->addWidget(sectionLabel(QStringLiteral("Coupling")));
        c.coupling = new QComboBox;
        c.coupling->addItem(QStringLiteral("DC"), 0);
        c.coupling->addItem(QStringLiteral("AC"), 1);
        c.coupling->setProperty("asSegments", true);   // DC | AC segments in the panel
        layout->addWidget(c.coupling);

        layout->addWidget(sectionLabel(QStringLiteral("Probe")));
        c.probe = new QComboBox;
        c.probe->setToolTip(QStringLiteral(
            "The SDK reports the voltage at the BNC. The probe converts that "
            "into what you are actually measuring."));
        layout->addWidget(c.probe);

        c.manageProbes = new QPushButton(QStringLiteral("Manage probes..."));
        c.manageProbes->setIcon(uiIcon(this, QStringLiteral("wrench"),
                                       QStyle::SP_FileDialogDetailedView, Theme::text()));
        c.manageProbes->setCursor(Qt::PointingHandCursor);
        layout->addWidget(c.manageProbes);

        c.invert = new QCheckBox(QStringLiteral("Invert"));
        c.invert->setToolTip(QStringLiteral(
            "Flip the trace vertically. Display only — the hardware is untouched."));
        layout->addWidget(c.invert);

        layout->addWidget(sectionLabel(QStringLiteral("Input offset")));
        c.inputOffset = new QDoubleSpinBox;
        c.inputOffset->setDecimals(3);
        c.inputOffset->setSuffix(QStringLiteral(" V"));
        c.inputOffset->setToolTip(QStringLiteral(
            "Centre of the hardware input window. Set this to 12 V with a "
            "\u00b1200 mV page to put the whole ADC range across the ripple on a "
            "12 V rail instead of wasting it on the DC level."));
        layout->addWidget(c.inputOffset);

        layout->addWidget(sectionLabel(QStringLiteral("DC offset")));
        c.offsetV = new QDoubleSpinBox;
        c.offsetV->setDecimals(3);
        c.offsetV->setSuffix(QStringLiteral(" V"));
        c.offsetV->setToolTip(QStringLiteral(
            "Adds a DC voltage to the waveform. With +20 mV set, a trace at 0 V "
            "reads 20 mV. The graticule stays where it is and the waveform moves "
            "against it."));
        layout->addWidget(c.offsetV);

        c.offsetPct = new QDoubleSpinBox;
        c.offsetPct->setRange(-900.0, 900.0);
        c.offsetPct->setDecimals(1);
        c.offsetPct->setSingleStep(5.0);
        c.offsetPct->setSuffix(QStringLiteral(" %"));
        c.offsetPct->setSingleStep(1.0);
        c.offsetPct->setDecimals(0);
        c.offsetPct->setToolTip(QStringLiteral(
            "Shifts the trace up or down on screen without changing what the "
            "hardware measures. Sized to this channel's page, so the reachable "
            "range does not shrink when you zoom in. Drag the voltage scale to "
            "adjust it directly."));
        layout->addWidget(c.offsetPct);

        layout->addWidget(sectionLabel(QStringLiteral("Filter")));
        {
            c.filterType = new QComboBox;
            c.filterType->addItem(QStringLiteral("Off"), int(ScopeView::FilterOff));
            c.filterType->addItem(QStringLiteral("Low-pass"), int(ScopeView::FilterLowPass));
            c.filterType->addItem(QStringLiteral("High-pass"), int(ScopeView::FilterHighPass));
            c.filterType->addItem(QStringLiteral("Band-pass"), int(ScopeView::FilterBandPass));
            c.filterType->addItem(QStringLiteral("Band-stop"), int(ScopeView::FilterBandStop));
            c.filterType->addItem(QStringLiteral("High-res"), int(ScopeView::FilterHighRes));
            c.filterType->setToolTip(QStringLiteral(
                "Zero-phase filters: edges keep their timing. Cutoffs are -3 dB; the "
                "counters step 1-2-5 and take typed values like 250k or 1.5 MHz."));
            layout->addWidget(c.filterType);
            // Two counters: the lower frequency (high-pass cutoff, band-pass
            // "from") and the upper one (low-pass cutoff, band-pass "to").
            const auto counterRow = [&](QLabel **label, QDoubleSpinBox **box, double value) {
                auto *row = new QWidget;
                auto *rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                *label = new QLabel;
                *box = new FrequencySpinBox;
                (*box)->setValue(value);
                rowLayout->addWidget(*label);
                rowLayout->addWidget(*box, 1);
                layout->addWidget(row);
                return row;
            };
            c.filterLowRow = counterRow(&c.filterLowLabel, &c.filterLowHz, 10.0);
            c.filterHighRow = counterRow(&c.filterHighLabel, &c.filterHighHz, 100e3);
            {
                // High-res: how many samples each point averages, and what it buys.
                auto *row = new QWidget;
                auto *rowLayout = new QHBoxLayout(row);
                rowLayout->setContentsMargins(0, 0, 0, 0);
                rowLayout->addWidget(new QLabel(QStringLiteral("Average")));
                c.filterHighResN = new QComboBox;
                c.filterHighResN->addItem(QStringLiteral("\u00d74  (+1 bit)"), 4);
                c.filterHighResN->addItem(QStringLiteral("\u00d716  (+2 bits)"), 16);
                c.filterHighResN->addItem(QStringLiteral("\u00d764  (+3 bits)"), 64);
                c.filterHighResN->addItem(QStringLiteral("\u00d7256  (+4 bits)"), 256);
                c.filterHighResN->setCurrentIndex(1);
                c.filterHighResN->setToolTip(QStringLiteral(
                    "Each point becomes the mean of this many neighbouring samples: finer than one "
                    "ADC step where the signal's noise spans a few steps (on small ranges it does), "
                    "for a bandwidth of about sample rate / (2 x N)."));
                rowLayout->addWidget(c.filterHighResN, 1);
                layout->addWidget(row);
                c.filterHighResRow = row;
            }
            const auto apply = [this, ch] {
                ChannelControls &cc = m_channel[ch];
                const int type = cc.filterType->currentData().toInt();
                const bool band = type == ScopeView::FilterBandPass || type == ScopeView::FilterBandStop;
                cc.filterLowRow->setVisible(type == ScopeView::FilterHighPass || band);
                cc.filterHighRow->setVisible(type == ScopeView::FilterLowPass || band);
                cc.filterHighResRow->setVisible(type == ScopeView::FilterHighRes);
                cc.filterLowLabel->setText(band ? QStringLiteral("From") : QStringLiteral("Cutoff"));
                cc.filterHighLabel->setText(band ? QStringLiteral("To") : QStringLiteral("Cutoff"));
                if (!m_view)
                    return;
                m_view->setChannelFilter(ch, type, filterLowParam(ch), cc.filterHighHz->value());
                // Record mode filters the raw recording itself (see
                // refreshRecordView()); the view only filters elsewhere.
                if (m_acquisitionMode == ModeRecord)
                    refreshRecordView();
                refreshCards();
            };
            connect(c.filterType, &QComboBox::currentIndexChanged, this, [apply](int) { apply(); });
            connect(c.filterLowHz, &QDoubleSpinBox::valueChanged, this, [apply](double) { apply(); });
            connect(c.filterHighHz, &QDoubleSpinBox::valueChanged, this, [apply](double) { apply(); });
            connect(c.filterHighResN, &QComboBox::currentIndexChanged, this, [apply](int) { apply(); });
            apply();
        }

        layout->addStretch();

        // What the ADC actually covers for this channel (updateAdcInfo()).
        c.adcInfo = new QLabel;
        c.adcInfo->setWordWrap(true);
        c.adcInfo->setObjectName(QStringLiteral("hintLabel"));
        c.adcInfo->setToolTip(QStringLiteral(
            "The input window the ADC really digitises, from the SDK's own conversion of the "
            "last data (code x zoom + bias), and how many of its codes the screen gets."));
        // Not shown (kept for its readback): the ADC line is diagnostics.
        c.adcInfo->setVisible(false);
        layout->addWidget(c.adcInfo);

        // Same arrangement as the Decoding page: transparent all the way
        // down (else the viewport shows as a differently tinted block), the
        // scroll bar at the panel's right edge, content 10 px in from it.
        layout->setContentsMargins(0, 0, 10, 0);
        auto *scroll = new QScrollArea;
        scroll->setObjectName(QStringLiteral("panelScroll"));
        page->setObjectName(QStringLiteral("panelScrollPage"));
        scroll->setWidget(page);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll->setStyleSheet(QStringLiteral(
            "QScrollArea#panelScroll, QWidget#qt_scrollarea_viewport,"
            " QWidget#panelScrollPage { background: transparent; }"));
        scroll->viewport()->setAutoFillBackground(false);
        page->setAutoFillBackground(false);
        m_channelPages->addWidget(scroll);
    }

    return m_channelPages;
}

// ---------------------------------------------------------------------------
// Wiring
// ---------------------------------------------------------------------------

void MainWindow::wireUp()
{
    connect(m_runCard, &CardButton::clicked, this, [this] {
        if (m_running)
            stopAcquisition();
        else
            startAcquisition();
    });

    connect(m_acqMode, &QButtonGroup::idClicked, this,
            [this](int mode) { setAcquisitionMode(mode); });
    connect(m_history, &QComboBox::currentIndexChanged, this,
            [this](int) { applyStreamSettings(); });
    connect(m_updates, &QComboBox::currentIndexChanged, this,
            [this](int) { applyStreamSettings(); });

    connect(m_stream, &StreamEngine::status, this, [this](const QString &text) {
        statusBar()->showMessage(text, 4000);
    });
    connect(m_stream, &StreamEngine::failed, this, [this](const QString &reason) {
        statusBar()->showMessage(reason, 6000);
        setRunning(false);
    });

    connect(m_scopeCard, &CardButton::clicked, this, [this] { openPage(PageScope); });
    connect(m_scopeCard, &CardButton::stepUp,   this, [this] { stepTimebase(+1); });
    connect(m_scopeCard, &CardButton::stepDown, this, [this] { stepTimebase(-1); });

    connect(m_triggerCard, &CardButton::clicked, this, [this] { openPage(PageTrigger); });
    connect(m_triggerCard, &CardButton::stepUp,   this, [this] { stepTriggerLevel(+1); });
    connect(m_triggerCard, &CardButton::stepDown, this, [this] { stepTriggerLevel(-1); });

    for (int ch = 0; ch < 4; ++ch) {
        connect(m_channelCard[ch], &CardButton::clicked, this,
                [this, ch] { openPage(PageChannel, ch); });
        connect(m_channelCard[ch], &CardButton::stepUp, this,
                [this, ch] { stepVoltsPerDiv(ch, +1); });
        connect(m_channelCard[ch], &CardButton::stepDown, this,
                [this, ch] { stepVoltsPerDiv(ch, -1); });

        ChannelControls &c = m_channel[ch];
        connect(c.mode, &QButtonGroup::idClicked, this, [this, ch](int mode) {
            const bool on = mode != ChannelOff;
            m_channel[ch].voltsPage->setEnabled(mode != ChannelOff);   // usable in Auto too (picking leaves Auto)
            m_nav->setChannelVisible(ch, on);
            m_view->setChannelVisible(ch, on);
            m_acq->setChannelEnabled(ch, on);
            refreshCards();
        });

        connect(c.scale, &QComboBox::currentIndexChanged, this, [this, ch](int) {
            applyChannel(ch);
        });
        connect(c.colour, &QPushButton::clicked, this, [this, ch] { pickChannelColour(ch); });
        connect(c.manageProbes, &QPushButton::clicked, this, [this, ch] {
            ProbeManagerDialog dialog(m_probes, this);
            dialog.exec();
            for (int i = 0; i < 4; ++i)
                reloadProbeList(i);
            applyChannel(ch);
        });
        connect(c.voltsPage, &QComboBox::currentIndexChanged, this, [this, ch](int) {
            applyChannel(ch);
            updateTriggerLevelRange();
        });
        // Picked by the user (the list or the field's bars; Auto's own changes
        // and settings restore don't emit this): leave Auto.
        connect(c.voltsPage, &QComboBox::activated, this, [this, ch](int) { leaveAutoRange(ch); });
        connect(c.invert, &QCheckBox::toggled, this, [this, ch](bool on) {
            m_view->setInverted(ch, on);
            m_nav->setInverted(ch, on);
            refreshCards();
        });
        connect(c.coupling, &QComboBox::currentIndexChanged, this,
                [this, ch](int) { applyChannel(ch); });
        connect(c.probe, &QComboBox::currentIndexChanged, this, [this, ch](int) {
            applyProbeUnits(ch);
            applyChannel(ch);
            updateTriggerLevelRange();   // bounds are in probe units
            applyTrigger();
        });
        connect(c.inputOffset, &QDoubleSpinBox::valueChanged, this, [this, ch](double) {
            applyChannel(ch);          // moves the hardware input window
            updateTriggerLevelRange();
        });
        connect(c.offsetV, &QDoubleSpinBox::valueChanged, this, [this, ch](double) {
            applyChannel(ch);          // added to the displayed signal
        });
        connect(c.offsetPct, &QDoubleSpinBox::valueChanged, this, [this, ch](double pct) {
            setChannelPositionPercent(ch, pct);   // display only
        });
    }

    connect(m_timebase, &QComboBox::currentIndexChanged, this, [this](int) { applyTimebase(); });
    connect(m_recordRate, &QComboBox::currentIndexChanged, this, [this](int) { applyRecordRate(); });
    connect(m_liveRate, &QComboBox::currentIndexChanged, this, [this](int) { applyTimebase(); });
    connect(m_depth,    &QComboBox::currentIndexChanged, this, [this](int) { applyTimebase(); });

    connect(m_trigModeGroup, &QButtonGroup::idClicked, this, [this](int) { applyTrigger(); });
    connect(m_trigDirGroup,  &QButtonGroup::idClicked, this, [this](int) { applyTrigger(); });
    connect(m_trigSource, &QComboBox::currentIndexChanged, this, [this](int) {
        updateTriggerLevelRange();
        applyTrigger();
    });
    connect(m_trigLevel, &QDoubleSpinBox::valueChanged, this, [this](double) { applyTrigger(); });
    connect(m_levelToMid, &QPushButton::clicked, this, &MainWindow::setTriggerLevelToMidpoint);

    connect(m_acq, &AcquisitionManager::triggerWaiting, this, [this](bool waiting) {
        m_waitingTrigger = waiting;
        refreshCards();
    });
    connect(m_preTrigger, &QSpinBox::valueChanged, this, [this](int v) {
        m_acq->setPreTriggerPercent(v);
        refreshCards();                           // the trigger card shows it
    });

    connect(m_acq, &AcquisitionManager::samplesReady, this,
            [this](int ch, const QVector<double> &samples, double fs, double trigger) {
                VSX_TRACE("capture.frame");
                // Zero calibration measures these frames instead of showing them.
                if (m_calibrating) {
                    if (m_calIndex < m_calSteps.size() && m_calSteps[m_calIndex].path == 0
                        && m_calSteps[m_calIndex].ch == ch && !samples.isEmpty()) {
                        double sum = 0.0, lo = samples[0], hi = samples[0];
                        for (double v : samples) {
                            sum += v;
                            lo = std::min(lo, v);
                            hi = std::max(hi, v);
                        }
                        calibrationMeasured(sum / samples.size(), hi - lo);
                    }
                    return;
                }
                // See m_suppressSamplesReady's own declaration comment —
                // a stray frame arriving any time after openNativeWaveform()
                // starts restoring settings, and before a new acquisition is
                // explicitly started, would otherwise overwrite what it
                // just loaded.
                if (m_suppressSamplesReady)
                    return;
                // Averaging (Capture): what's shown from here on is the
                // running mean of the triggered frames, aligned on the trigger.
                double shownTrigger = trigger;
                const QVector<double> shown = averageFrame(ch, samples, fs, &shownTrigger);
                // Captured here regardless of which mode is currently
                // active — a capture in progress when the mode changes
                // still finishes and its data is still worth keeping for
                // when Capture mode is switched back into, the same as if
                // the switch had never happened.
                if (ch >= 0 && ch < 4) {
                    m_captureSnapshot.samples[ch] = shown;
                    m_captureSnapshot.valid[ch] = true;
                    m_captureSnapshot.sampleRate = fs;
                    m_captureSnapshot.triggerPoint = shownTrigger;
                    m_captureSnapshot.hasData = true;
                }
                // Only drawn if the channel is actually on — matching the
                // same check refreshLiveView() and refreshRecordView()
                // already make before their own equivalent calls. Without
                // it, whatever the SDK happens to still deliver for a
                // disabled channel would draw anyway: setSamples() always
                // marks a channel visible now (a recent, deliberate fix
                // for a different bug), and this was the one call site
                // that never checked whether the channel was supposed to
                // be shown at all before calling it.
                if (m_channel[ch].mode->checkedId() != ChannelOff) {
                    // Never an empty array: setSamples() sets the view-wide
                    // buffer length from it, and 0 collapses the window to
                    // two samples (the flat-trace bug). Keep the last frame.
                    if (!shown.isEmpty()) {
                        m_view->setSamples(ch, shown, fs, shownTrigger);
                        QTimer::singleShot(0, this, &MainWindow::onDecodeTick);   // decode this frame now
                        if (m_softwareAc[ch])
                            trackSoftwareAc(ch, shown);
                        m_nav->setSamples(ch, shown);
                    }
                } else {
                    m_view->clearChannel(ch);
                }
                feedSpectrum(ch, shown, fs);
                feedXY(ch, shown);
                syncNavigator();
                if (m_channel[ch].mode->checkedId() == ChannelAuto)
                    autoRange(ch);

                // Depth trades latency for resolution, so show what it costs.
                if (ch == 0 || m_framesPerSec == 0.0) {
                    static QElapsedTimer clock;
                    if (!clock.isValid())
                        clock.start();
                    const qint64 now = clock.elapsed();
                    if (m_lastFrameMs > 0 && now > m_lastFrameMs) {
                        const double instant = 1000.0 / double(now - m_lastFrameMs);
                        m_framesPerSec = m_framesPerSec > 0.0
                            ? m_framesPerSec * 0.8 + instant * 0.2 : instant;
                    }
                    m_lastFrameMs = now;
                }
                if (m_guidedWaiting && m_guidedTest == GuidedCharging)
                    QTimer::singleShot(0, this, [this] { finishChargingTest(); });
            });
    // (StreamEngine::rangeReport: no longer shown in the status bar — it was
    // diagnostics; the Range report in development builds covers it.)
    connect(m_acq, &AcquisitionManager::channelOverRange, this, [this](int ch, bool over) {
        m_view->setChannelOverRange(ch, over);
    });
    connect(m_acq, &AcquisitionManager::status, this, [this](const QString &text) {
        statusBar()->showMessage(text, 4000);
    });
    connect(m_acq, &AcquisitionManager::deviceChanged, this, &MainWindow::onDeviceChanged);

    connect(m_view, &ScopeView::clicked, this, [this] {
        if (m_panelOpen)
            closePanel();
    });

    connect(m_view, &ScopeView::zoomRequested, this,
            [this](const QRectF &box) { zoomToRect(box); });
    connect(m_view, &ScopeView::zoomResetRequested, this, [this] { resetZoom(); });
    connect(m_view, &ScopeView::zoomBackRequested, this, [this] { navigateZoomHistory(false); });
    connect(m_view, &ScopeView::zoomForwardRequested, this, [this] { navigateZoomHistory(true); });
    connect(m_view, &ScopeView::rulersClosed, this, [this] {
        if (m_rulersButton)
            m_rulersButton->setChecked(false);   // ScopeView already turned itself off
        if (m_panelOpen && m_currentPage == PageRulers)
            closePanel();
    });
    connect(m_view, &ScopeView::panRequested, this, [this](double dxFraction) {
        if (m_acquisitionMode == ModeRecord && !m_currentRecordTab.isEmpty()
            && m_stream->recorded() > 0) {
            // Updating m_recordViewStart here is cheap; refreshRecordView() is
            // not — it reads off disk and decimates. A drag delivers a mouse
            // move roughly every frame, and doing that work on every single one
            // was slow enough that Qt started coalescing/dropping intermediate
            // move events waiting for the handler to return, which is what
            // made dragging feel choppy and occasionally jump. Updating the
            // (cheap) position on every event but deferring the (expensive)
            // redraw to the next spin of the event loop collapses a whole
            // burst of drag events into a single disk read.
            const quint64 total = m_stream->recorded();
            const quint64 span = m_recordViewLength > 0
                ? std::min(m_recordViewLength, total) : total;

            // Each drag event covers only a few screen pixels, which at a
            // small span (deep zoom) is a small FRACTION of one sample —
            // computing and truncating that fraction independently on every
            // event throws it away completely rather than rounding it, so a
            // slow, smooth drag did nothing at all until an occasional larger,
            // faster motion happened to cross a whole sample by itself. The
            // remainder is carried forward instead, so nothing is lost — it
            // just takes a few more events to add up to one sample at extreme
            // zoom, exactly as it should.
            const double shiftF = -dxFraction * double(span) + m_recordPanRemainder;
            const qint64 shift = qint64(shiftF);
            m_recordPanRemainder = shiftF - double(shift);

            qint64 newStart = qint64(m_recordViewStart) + shift;
            const qint64 maxStart = qint64(total > span ? total - span : 0);
            newStart = std::clamp<qint64>(newStart, 0, maxStart);
            m_recordViewStart = quint64(newStart);
            m_recordViewLength = span;

            if (m_recordPanDebounce->isActive()) {
                m_recordPanDirty = true;
            } else {
                refreshRecordView();
                m_recordPanDebounce->start();
            }
        } else if (m_acquisitionMode == ModeCapture
                   || (m_acquisitionMode == ModeStream && m_liveLoaded)) {
            // Loaded Live data (m_liveLoaded) is a static buffer just like a
            // capture, so it pans the same way rather than through the ring.
            // panBySamples() accumulates directly rather than round-tripping
            // through viewportCentreFraction() on every event — see ScopeView
            // for why that round-trip made one drag direction feel faster
            // than the other at a tight zoom.
            const double windowSamples =
                m_view->viewportWidthFraction() * m_view->captureLength();
            m_view->panBySamples(-dxFraction * windowSamples);
            syncNavigator();
        } else if (m_acquisitionMode == ModeStream) {
            // Panning only does anything once the ring has stopped changing
            // underneath it — either Freeze (ingest() itself stops
            // appending while frozen — see StreamEngine) or Stop (nothing
            // is running to append at all) both leave it static; only
            // actively running and unfrozen means the very next tick's
            // "show the newest" pull would immediately overwrite whatever
            // a drag just revealed.
            if (!m_stream->frozen() && m_running)
                return;

            // ScopeView's own current secondsPerDiv, not the timebase
            // dropdown's nominal value — see refreshLiveView() for why
            // these two can disagree after a drag-a-box zoom, and what
            // reading the wrong one did here specifically: windowSamples
            // ending up far larger than the window actually on screen
            // could push maxOffset to (or below) 0, clamping every drag's
            // result to the same fixed 0 regardless of how far the mouse
            // moved — panning that silently did nothing at all, no matter
            // how it was dragged.
            const double perDiv = m_view->secondsPerDiv();
            const double windowSamples =
                perDiv * ScopeView::HorizontalDivs * m_stream->sampleRate();
            if (windowSamples <= 0.0)
                return;

            quint64 available = 0;
            for (int ch = 0; ch < 4; ++ch) {
                if (m_channel[ch].mode->checkedId() != ChannelOff)
                    available = std::max(available, m_stream->historyAvailable(ch));
            }

            const qint64 maxOffset = std::max<qint64>(0, qint64(available) - qint64(windowSamples));
            // No negation: dragging left (dxFraction < 0) should decrease
            // the offset, moving toward "now" — physically, dragging
            // content left brings whatever is to its right into view, and
            // "right" on this timeline is newer (offset 0). The previous
            // negated version moved the offset the opposite way, which is
            // what made the drag feel reversed.
            const qint64 shift = qint64(std::lround(dxFraction * windowSamples));
            m_liveViewOffset = quint64(std::clamp<qint64>(
                qint64(m_liveViewOffset) + shift, 0, maxOffset));
            m_livePanClock.restart();
            refreshLiveView();
        }
    });

    connect(m_view, &ScopeView::mathOffsetDragged, this, [this](double deltaVolts) {
        m_view->setMathPositionVolts(m_view->mathPositionVolts() + deltaVolts);
        m_nav->setMathScale(m_view->mathVoltsPerDiv(), m_view->mathPositionVolts());
    });
    connect(m_view, &ScopeView::offsetDragged, this, [this](int ch, double deltaVolts) {
        if (ch < 0 || ch >= 4)
            return;
        // ScopeView now reports the drag in volts at the channel's current
        // (possibly zoomed) scale, so this converts straight to a page-relative
        // percentage without any divisions in between.
        const double page = m_channel[ch].voltsPage->currentData().toDouble();
        if (page <= 0.0)
            return;
        const double deltaPct = deltaVolts / page * 100.0;
        setChannelPositionPercent(ch, m_channelOffsetPctExact[ch] + deltaPct);
    });
    connect(m_view, &ScopeView::referenceOffsetDragged, this, [this](int slot, double deltaVolts) {
        if (!m_view->referenceOccupied(slot))
            return;
        // referencePositionVolts() is the exact value ScopeView stores
        // internally, not a rounded UI spinbox read back — same reason
        // Math's own drag above reads mathPositionVolts() directly rather
        // than a spinbox, avoiding the slow-drag rounding bug fixed earlier
        // this session for channels (setChannelPositionPercent()'s own
        // m_channelOffsetPctExact accumulator).
        m_view->setReferencePositionVolts(slot, m_view->referencePositionVolts(slot) + deltaVolts);
        // Keeps the panel's own Vertical offset spinbox showing the current
        // (rounded-for-display) percentage as the drag proceeds — the
        // underlying value driving the trace itself stays exact regardless.
        refreshReferencesPanel();
    });

    connect(m_nav, &NavigatorView::centreRequested, this, [this](double f) {
        if (m_acquisitionMode == ModeRecord && !m_currentRecordTab.isEmpty()
            && m_stream->recorded() > 0) {
            // In a record the navigator spans the whole thing, so a drag picks
            // a position in the record rather than in a single capture. This
            // is a SEPARATE signal from the plot's own panRequested — dragging
            // the navigator strip was calling refreshRecordView() (a disk read
            // plus decimation) directly on every single mouse-move event, with
            // none of the throttling panRequested already has. That made
            // scrubbing the navigator strip specifically the laggiest thing in
            // the app, since it hit the disk on every pixel of movement.
            const quint64 total = m_stream->recorded();
            const quint64 centre = quint64(std::clamp(f, 0.0, 1.0) * total);
            quint64 start = centre > m_recordViewLength / 2
                ? centre - m_recordViewLength / 2 : 0;
            // Clamped so the window keeps its full configured width right up
            // to the end of the recording, rather than shrinking as the
            // requested centre approaches it — an unclamped start here let
            // `total - start` (the actual span refreshRecordView() can show)
            // narrow the closer the drag got to the right edge, which is
            // what looked like an unexpected zoom rather than a plain pan,
            // and let the viewport rectangle itself extend past the strip's
            // own right edge.
            if (total > m_recordViewLength)
                start = std::min(start, total - m_recordViewLength);
            else
                start = 0;
            m_recordViewStart = start;

            if (m_recordPanDebounce->isActive()) {
                m_recordPanDirty = true;
            } else {
                refreshRecordView();
                m_recordPanDebounce->start();
            }
            return;
        }
        if (m_acquisitionMode == ModeStream && !m_liveLoaded) {
            // Same gate as the plot's own panRequested handler just above,
            // for the identical reason: the ring has to have actually
            // stopped changing (Freeze, or nothing running to append at
            // all) for a drag to mean anything rather than being
            // overwritten by the very next "show the newest" tick.
            if (!m_stream->frozen() && m_running)
                return;
            // ScopeView's own current secondsPerDiv, not the timebase
            // dropdown's nominal value — the two can disagree after a
            // drag-a-box zoom (see refreshLiveView()), and windowSamples
            // computed from the wrong one can push maxOffset to 0,
            // clamping every drag here to the same fixed position
            // regardless of where it targets — which is exactly the
            // "rectangle snaps to maximized, drags nothing" this was
            // reported as.
            const double perDiv = m_view->secondsPerDiv();
            const double windowSamples =
                perDiv * ScopeView::HorizontalDivs * m_stream->sampleRate();
            if (windowSamples <= 0.0)
                return;
            quint64 available = 0;
            for (int ch = 0; ch < 4; ++ch) {
                if (m_channel[ch].mode->checkedId() != ChannelOff)
                    available = std::max(available, m_stream->historyAvailable(ch));
            }
            if (available <= 0)
                return;
            const qint64 maxOffset = std::max<qint64>(0, qint64(available) - qint64(windowSamples));
            // Inverse of refreshLiveView()'s own centre computation
            // (centre = (available - offset - span/2) / available), so a
            // drag to fraction f lands the viewport exactly where it
            // visually points rather than merely in the right direction.
            const double offset = double(available) * (1.0 - std::clamp(f, 0.0, 1.0))
                                 - windowSamples / 2.0;
            m_liveViewOffset = quint64(std::clamp<qint64>(
                qint64(std::lround(offset)), 0, maxOffset));
            m_livePanClock.restart();
            refreshLiveView();
            return;
        }
        m_view->setViewCentreFraction(f);
        syncNavigator();
    });
    connect(m_nav, &NavigatorView::followTriggerRequested, this, [this] {
        // Record has no trigger, and syncNavigator() would report its page
        // as the whole recording — the same box-stretching bug as above.
        if (m_acquisitionMode == ModeRecord)
            return;
        m_view->followTrigger();
        syncNavigator();
    });
}

// ---------------------------------------------------------------------------
// Panel
// ---------------------------------------------------------------------------

void MainWindow::openPage(Page page, int channel)
{
    // Clicking the card that is already showing closes the panel again.
    if (m_panelOpen && m_currentPage == page
        && (page != PageChannel || m_currentChannel == channel)) {
        closePanel();
        return;
    }

    m_currentPage = page;
    if (page == PageChannel && channel >= 0) {
        m_currentChannel = channel;
        m_channelPages->setCurrentIndex(channel);
        m_panelTitle->setText(QStringLiteral("Channel %1").arg(channel + 1));
    } else if (page == PageScope) {
        m_panelTitle->setText(QStringLiteral("Scope"));
    } else if (page == PageInstruments) {
        m_panelTitle->setText(QStringLiteral("Instruments"));
    } else if (page == PageMath) {
        m_panelTitle->setText(QStringLiteral("Math"));
    } else if (page == PageRulers) {
        m_panelTitle->setText(QStringLiteral("Rulers"));
        // Opening the settings makes little sense with nothing to configure
        // showing yet — enabling here matches Zoom's own "the rail button
        // arms the feature" behaviour, just routed through the panel first.
        m_view->setRulersEnabled(true);
        refreshRulersChannelList();
    } else if (page == PageMeasure) {
        m_panelTitle->setText(QStringLiteral("Measure"));
        refreshMeasureChannelSelector();
    } else if (page == PageReferences) {
        m_panelTitle->setText(QStringLiteral("References"));
    } else if (page == PageDecoding) {
        m_panelTitle->setText(QStringLiteral("Decoding"));
    } else if (page == PageAutomotive) {
        m_panelTitle->setText(QStringLiteral("Automotive"));
    } else if (page == PageRemote) {
        m_panelTitle->setText(QStringLiteral("Remote"));
        refreshRemote();   // addresses may have changed (joined a hotspot since)
    } else if (page == PageSettings) {
        m_panelTitle->setText(QStringLiteral("Settings"));
        refreshSettingsPage();
    } else {
        m_panelTitle->setText(QStringLiteral("Trigger"));
    }

    m_panel->setCurrentIndex(int(page));
    if (!m_panelOpen) {
        m_panelHost->setVisible(true);
        m_panelAnimation->stop();
        m_panelAnimation->setStartValue(m_panelHost->maximumWidth());
        m_panelAnimation->setEndValue(kPanelWidth);
        m_panelAnimation->start();
    }
    m_panelOpen = true;
    syncPanelButtons();
    refreshCards();
}

// Keeps the rail buttons that open a panel page showing "checked" for exactly
// as long as their page is the one actually open — driven by panel state
// rather than the button's own click-toggle, since openPage() also closes the
// panel when you click an already-open page's button, which the button's own
// checked state would not otherwise reflect.
void MainWindow::syncPanelButtons()
{
    if (m_instrumentsButton)
        m_instrumentsButton->setChecked(m_panelOpen && m_currentPage == PageInstruments);
    if (m_mathToolButton)
        m_mathToolButton->setChecked(m_panelOpen && m_currentPage == PageMath);
    if (m_rulersButton)
        m_rulersButton->setChecked(m_panelOpen && m_currentPage == PageRulers);
    if (m_measureButton)
        m_measureButton->setChecked(m_panelOpen && m_currentPage == PageMeasure);
    if (m_referencesButton)
        m_referencesButton->setChecked(m_panelOpen && m_currentPage == PageReferences);
    if (m_decodingButton)
        m_decodingButton->setChecked(m_panelOpen && m_currentPage == PageDecoding);
    if (m_automotiveButton)
        m_automotiveButton->setChecked(m_panelOpen && m_currentPage == PageAutomotive);
    if (m_remoteButton)
        m_remoteButton->setChecked(m_panelOpen && m_currentPage == PageRemote);
    if (m_settingsButton)
        m_settingsButton->setChecked(m_panelOpen && m_currentPage == PageSettings);
}

void MainWindow::closePanel()
{
    m_panelAnimation->stop();
    m_panelAnimation->setStartValue(m_panelHost->maximumWidth());
    m_panelAnimation->setEndValue(0);
    m_panelAnimation->start();
    m_panelOpen = false;
    syncPanelButtons();
    refreshCards();
}

void MainWindow::refreshCards()
{
    VSX_TRACE("refreshCards");
    m_runCard->setTitle(m_running ? QStringLiteral("Acquiring")
                                  : QStringLiteral("Idle"));
    m_runCard->setValue(m_running ? QStringLiteral("Running") : QStringLiteral("Stopped"));
    const QColor runColour = m_running ? Theme::accent() : QColor(0xff, 0x3b, 0x30);
    m_runCard->setAccent(runColour);
    m_runCard->setSideColor(runColour);
    m_runCard->setIcon(m_running
        ? uiIcon(const_cast<MainWindow *>(this), QStringLiteral("stop"),
                 QStyle::SP_MediaStop, runColour)
        : uiIcon(const_cast<MainWindow *>(this), QStringLiteral("play"),
                 QStyle::SP_MediaPlay, runColour));
    m_runCard->setSelected(m_running);

    if (m_acquisitionMode == ModeRecord) {
        m_scopeCard->setValue(formatRate(m_recordRate->currentData().toUInt()));
        m_scopeCard->setNote(QStringLiteral("next recording"));
    } else {
        m_scopeCard->setValue(QStringLiteral("%1/div")
                                  .arg(formatSeconds(m_timebase->currentData().toDouble())));
        QString note = formatRate(m_acq->sampleRate());
        if (m_acquisitionMode == ModeCapture && m_average && m_average->currentData().toInt() > 1)
            note += QStringLiteral(" \u00b7 avg %1").arg(m_average->currentData().toInt());
        m_scopeCard->setNote(note);
    }
    m_scopeCard->setSelected(m_panelOpen && m_currentPage == PageScope);

    const int mode = m_trigModeGroup->checkedId();
    const bool armed = mode != ModeNone;
    const int trigSource = m_trigSource->currentIndex() < 0
        ? 0 : std::clamp(m_trigSource->currentData().toInt(), 0, 3);
    m_triggerCard->setValue(armed
        ? formatVolts(m_trigLevel->value(), probeFor(trigSource).unit)
        : QStringLiteral("None"));
    if (!armed) {
        m_triggerCard->setNote(QStringLiteral("free run"));
    } else if (m_running && m_waitingTrigger) {
        m_triggerCard->setNote(QStringLiteral("waiting..."));
    } else {
        m_triggerCard->setNote(QStringLiteral("%1 %2 %3")
            .arg(m_trigSource->currentText(),
                 m_trigDirGroup->checkedId() == 0 ? QStringLiteral("Rising")
                                                  : QStringLiteral("Falling"),
                 mode == ModeNormal ? QStringLiteral("Nrm") : QStringLiteral("Auto")));
    }
    // Pre-trigger (Capture: where the trigger sits in the frame), bottom right.
    m_triggerCard->setDetail(m_acquisitionMode == ModeCapture && m_preTrigger
                                 ? QStringLiteral("%1 %").arg(m_preTrigger->value()) : QString());
    m_triggerCard->setSelected(m_panelOpen && m_currentPage == PageTrigger);

    for (int ch = 0; ch < 4; ++ch) {
        const int mode = m_channel[ch].mode->checkedId();
        const bool on = mode != ChannelOff;
        m_channelCard[ch]->setValue(on
            ? QStringLiteral("\u00b1%1")
                  .arg(formatVolts(m_channel[ch].voltsPage->currentData().toDouble(),
                                   probeFor(ch).unit))
            : QStringLiteral("Off"));
        QString note = QStringLiteral("%1 %2")
            .arg(m_softwareAc[ch] ? QStringLiteral("AC (sw)") : m_channel[ch].coupling->currentText(),
                 m_channel[ch].probe->currentText());
        if (mode == ChannelAuto)
            note += QStringLiteral(" AUTO");
        if (m_channel[ch].invert->isChecked())
            note += QStringLiteral(" INV");
        if (m_channel[ch].filterType) {
            static const char *kFilterNote[] = { "", " LPF", " HPF", " BPF", " BSF" };
            const int type = m_channel[ch].filterType->currentData().toInt();
            if (type == ScopeView::FilterHighRes)
                note += QStringLiteral(" HR\u00d7%1").arg(int(filterLowParam(ch)));
            else
                note += QLatin1String(kFilterNote[std::clamp(type, 0, 4)]);
        }
        m_channelCard[ch]->setNote(note);
        m_channelCard[ch]->setMuted(!on);
        m_channelCard[ch]->setSelected(m_panelOpen && m_currentPage == PageChannel
                                       && m_currentChannel == ch);
    }

    if (m_sampleInfo) {
        // m_view->secondsPerDiv() rather than a locally-declared perDiv: the
        // card section above reads m_timebase directly (Capture/Live) or
        // m_recordRate (Record), and neither is a stand-in for "the window
        // actually on screen right now" once panning or zooming has moved
        // away from it.
        const double rate = m_acq->sampleRate();
        const double window = m_view->secondsPerDiv() * ScopeView::HorizontalDivs;
        const double capture = rate > 0.0
            ? double(m_acq->captureKb()) * 1024.0 / rate : 0.0;
        QString info = QStringLiteral("Sample rate %1\nScreen %2\nCapture %3 (%4 KB)")
                .arg(formatRate(rate), formatSeconds(window), formatSeconds(capture))
                .arg(m_acq->captureKb());

        if (m_acquisitionMode == ModeRecord) {
            const quint64 done   = m_stream->recorded();
            const quint64 target = m_stream->recordTarget();
            const int active = std::max(1, m_stream->recordedChannels());
            const double doneMb   = m_stream->recordMegabytes();
            const double targetMb = double(target) * 2.0 * active / 1048576.0;
            // GB for the large sizes ("5120 MB" reads poorly).
            const bool gb = targetMb >= 1024.0;
            m_sampleInfo->setText(QStringLiteral(
                "Sample rate %1\nRecorded %2 / %3 %4")
                    .arg(formatRate(m_acq->sampleRate()))
                    .arg(gb ? doneMb / 1024.0 : doneMb, 0, 'f', gb ? 2 : 1)
                    .arg(gb ? targetMb / 1024.0 : targetMb, 0, 'f', gb ? 1 : 0)
                    .arg(gb ? QStringLiteral("GB") : QStringLiteral("MB")));
            return;
        }

        if (m_acquisitionMode == ModeStream) {
            const StreamEngine::Stats st = m_stream->stats();
            info = QStringLiteral("Sample rate %1\nScreen %2\nHistory %3")
                       .arg(formatRate(rate), formatSeconds(window),
                            m_history->currentText());
            // What's actually held, if memory capped it.
            const double asked = m_history->currentData().toDouble();
            for (int ch = 0; ch < m_stream->channels() && ch < 4; ++ch) {
                if (m_channel[ch].mode->checkedId() == ChannelOff || rate <= 0.0)
                    continue;
                const double held = double(m_stream->historyCapacity(ch)) / rate;
                if (held > 1.0 && asked > 0.0 && held < asked * 0.99)
                    info += QStringLiteral(" (%1 held: memory limit)").arg(formatSeconds(held));
                break;
            }
            if (st.blocks > 0) {
                info += QStringLiteral("\nBlocks %1/s of %2 samples"
                                       "\nLatency %3 ms\nDropped %4")
                            .arg(st.blocksPerSecond, 0, 'f', 1)
                            .arg(st.blockSize)
                            .arg(st.latencyMs, 0, 'f', 0)
                            .arg(st.drops);
            }
            m_sampleInfo->setText(info);
            return;
        }

        // Where the device actually put the trigger. Asking for 50% pre-trigger
        // does not guarantee the trigger lands mid-capture, and that offset is
        // what moves the navigator viewport off the middle of the strip.
        const double trigFraction = m_view->triggerFraction();
        if (trigFraction >= 0.0) {
            info += QStringLiteral("\nTrigger at %1% of capture")
                        .arg(trigFraction * 100.0, 0, 'f', 1);
        }
        if (m_running && m_framesPerSec > 0.0)
            info += QStringLiteral("\nFrames %1/s").arg(m_framesPerSec, 0, 'f', 1);
        m_sampleInfo->setText(info);
    }
    enforceReferenceGutterBudget();
    refreshReferencesPanel();
}

// The plot has room for 4 gutter columns at once: CH1, CH2, Math, and each
// reference all compete for that budget (see rightGutter()/leftGutter()).
// With CH1+CH2 fixed and up to 2 references allowed, a second reference
// alongside Math would need a 5th — so Math loses that fight, not the
// reference, since the reference is the thing the user just explicitly
// asked for. Called after every channel/Math/reference change via
// refreshCards(), so it is really an invariant check rather than a
// one-shot reaction to any single action — but per the user's own
// specification, this only ever turns Math OFF; removing a reference
// afterward does not bring it back on its own; the user re-enables it by
// hand once there is room again.
void MainWindow::enforceReferenceGutterBudget()
{
    // m_view guarded explicitly here rather than relying on the
    // m_mathEnabled check below happening to also be null at the same
    // construction-time moment — see refreshReferencesPanel()'s own
    // comment for the exact ordering quirk this is protecting against.
    if (!m_view || !m_mathEnabled || !m_mathEnabled->isChecked())
        return;
    int total = 0;
    for (int ch = 0; ch < 4; ++ch)
        total += m_view->channelVisible(ch) ? 1 : 0;
    total += 1;   // Math itself, already confirmed on above
    total += m_view->referenceCount();
    if (total <= 4)
        return;

    m_mathEnabled->setChecked(false);   // applyMathChannel() runs from its own toggled signal
    statusBar()->showMessage(QStringLiteral(
        "Math disabled to make room for a reference waveform \u2014 the plot "
        "allows 4 gutters at once."), 4000);
}

// Called at the end of refreshCards() (so any channel on/off change anywhere
// keeps the Add row in sync for free) and after every add/remove/select
// inside the References panel itself.
void MainWindow::refreshReferencesPanel()
{
    // Both guards needed: m_referenceNameEdit for calls before this panel
    // itself is built, m_view for the panel's own end-of-build call — see
    // buildRulersPage()'s identical comment on m_rulersMathCheck for why:
    // buildDetailPanel() (and so every buildXxxPage(), this one included)
    // runs before m_view = new ScopeView actually executes, even though
    // that line sits earlier in this file than buildReferencesPage()'s own
    // code. Every OTHER m_view reference in this function is safely inside
    // a deferred lambda; only this function's own immediate, top-level
    // work needs the guard.
    if (!m_referenceNameEdit || !m_view)
        return;

    const bool referencesUsable = m_acquisitionMode != ModeRecord;

    for (int ch = 0; ch < 4; ++ch) {
        QPushButton *button = m_referenceAddButtons[ch];
        if (!button)
            continue;
        const bool channelOn = m_view->channelVisible(ch);
        button->setVisible(channelOn);
        // Disabled rather than hidden when both slots are full: a channel
        // that IS on but cannot currently be added stays visible so it is
        // obvious the slots, not the channel, are what is blocking it.
        button->setEnabled(channelOn && referencesUsable
                            && m_view->referenceCount() < ScopeView::MaxReferences);
    }
    if (m_referenceFileButton)
        m_referenceFileButton->setEnabled(referencesUsable
                                           && m_view->referenceCount() < ScopeView::MaxReferences);

    // If the previously-selected slot just emptied out (removed, or never
    // occupied to begin with) but the other one is occupied, follow it
    // rather than leave the Manage controls pointed at nothing while a
    // real reference sits one click away.
    if (!m_view->referenceOccupied(m_referenceSelectedSlot)) {
        for (int slot = 0; slot < ScopeView::MaxReferences; ++slot) {
            if (m_view->referenceOccupied(slot)) {
                m_referenceSelectedSlot = slot;
                break;
            }
        }
    }

    for (int slot = 0; slot < ScopeView::MaxReferences; ++slot) {
        QPushButton *button = m_referenceSlotButtons[slot];
        if (!button)
            continue;
        const bool occupied = m_view->referenceOccupied(slot);
        button->setEnabled(occupied);
        button->setChecked(occupied && slot == m_referenceSelectedSlot);
        button->setText(occupied ? m_view->referenceName(slot)
                                  : QStringLiteral("Ref %1").arg(slot + 1));
        if (occupied) {
            QPalette pal = button->palette();
            pal.setColor(QPalette::ButtonText, m_view->referenceColor(slot));
            button->setPalette(pal);
        }
    }

    const bool haveSelection = m_view->referenceOccupied(m_referenceSelectedSlot);
    for (QWidget *w : { static_cast<QWidget *>(m_referenceNameEdit),
                        static_cast<QWidget *>(m_referenceColourButton),
                        static_cast<QWidget *>(m_referenceVoltsPerDiv),
                        static_cast<QWidget *>(m_referencePositionPct),
                        static_cast<QWidget *>(m_referenceDelay),
                        static_cast<QWidget *>(m_referenceSaveButton),
                        static_cast<QWidget *>(m_referenceRemoveButton) }) {
        if (w)
            w->setEnabled(haveSelection);
    }
    if (!haveSelection)
        return;

    const int slot = m_referenceSelectedSlot;
    if (m_referenceNameEdit->text() != m_view->referenceName(slot))
        m_referenceNameEdit->setText(m_view->referenceName(slot));

    QPalette pal = m_referenceColourButton->palette();
    pal.setColor(QPalette::ButtonText, m_view->referenceColor(slot));
    m_referenceColourButton->setPalette(pal);

    const double perDiv = m_view->referenceVoltsPerDiv(slot);
    if (!qFuzzyCompare(m_referenceVoltsPerDiv->value(), perDiv)) {
        m_referenceVoltsPerDiv->blockSignals(true);
        m_referenceVoltsPerDiv->setValue(perDiv);
        m_referenceVoltsPerDiv->blockSignals(false);
    }

    const double positionVolts = m_view->referencePositionVolts(slot);
    const double positionPct = perDiv > 0.0 ? positionVolts / perDiv * 100.0 : 0.0;
    if (!qFuzzyCompare(m_referencePositionPct->value(), positionPct)) {
        m_referencePositionPct->blockSignals(true);
        m_referencePositionPct->setValue(positionPct);
        m_referencePositionPct->blockSignals(false);
    }

    const double delay = m_view->referenceDelay(slot);
    if (!qFuzzyCompare(m_referenceDelay->value(), delay)) {
        m_referenceDelay->blockSignals(true);
        m_referenceDelay->setValue(delay);
        m_referenceDelay->blockSignals(false);
    }
}

namespace {
// A reference is a small, self-contained blob — one channel's worth of
// samples plus a handful of scalar fields — so unlike a recording's own
// .vsxrec (samples file + a separate meta.ini, built for a file that grows
// incrementally while being written) this is one single QDataStream-framed
// file with everything in it, written and read in one pass.
constexpr quint32 kReferenceMagic   = 0x56535852;   // "VSXR"
constexpr quint32 kReferenceVersion = 2;   // 2: + the unit (1 = volts)

}

void MainWindow::saveReferenceToFile(int slot)
{
    if (!m_view->referenceOccupied(slot)) {
        statusBar()->showMessage(QStringLiteral("No reference selected to save."), 3000);
        return;
    }
    QString path = QFileDialog::getSaveFileName(this,
        QStringLiteral("Save reference waveform"), QString(),
        QStringLiteral("VimuScopeX reference (*.vsxref)"));
    if (path.isEmpty())
        return;
    // The native dialog doesn't reliably append the filter's own extension
    // when the person types a bare name — without this, a file saved as
    // e.g. "ch1" would silently fail to appear in File's own *.vsxref
    // filtered listing later.
    if (!path.endsWith(QStringLiteral(".vsxref"), Qt::CaseInsensitive))
        path += QStringLiteral(".vsxref");

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 4000);
        return;
    }
    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_5_15);
    out << kReferenceMagic << kReferenceVersion;
    out << m_view->referenceName(slot);
    out << m_view->referenceColor(slot);
    out << m_view->referenceSampleRate(slot);
    out << m_view->referenceTriggerIndex(slot);
    out << m_view->referenceVoltsPerDiv(slot);
    out << m_view->referencePositionVolts(slot);
    out << m_view->referenceDelay(slot);
    out << m_view->referenceSamples(slot);
    out << m_view->referenceUnit(slot);       // version 2
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
}

void MainWindow::openReferenceFile()
{
    if (m_view->referenceCount() >= ScopeView::MaxReferences) {
        statusBar()->showMessage(QStringLiteral(
            "Both reference slots are already in use \u2014 remove one first."), 3000);
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this,
        QStringLiteral("Open reference waveform"), QString(),
        QStringLiteral("VimuScopeX reference (*.vsxref)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(QStringLiteral("Could not read %1").arg(path), 4000);
        return;
    }
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_15);
    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (magic != kReferenceMagic) {
        statusBar()->showMessage(
            QStringLiteral("%1 is not a reference waveform file.").arg(path), 4000);
        return;
    }
    if (version > kReferenceVersion) {
        statusBar()->showMessage(QStringLiteral(
            "%1 was saved by a newer VimuScope X; update to open it.").arg(path), 5000);
        return;
    }
    QString name;
    QColor color;
    double sampleRate = 0.0, triggerIndex = 0.0, voltsPerDiv = -1.0;
    double positionVolts = 0.0, delaySeconds = 0.0;
    QVector<double> samples;
    QString unit = QStringLiteral("V");       // version 1 files: volts
    in >> name >> color >> sampleRate >> triggerIndex >> voltsPerDiv
       >> positionVolts >> delaySeconds >> samples;
    if (version >= 2)
        in >> unit;
    if (in.status() != QDataStream::Ok) {
        statusBar()->showMessage(QStringLiteral("%1 is corrupt or unreadable.").arg(path), 4000);
        return;
    }

    const int slot = m_view->addReferenceFromData(samples, sampleRate, triggerIndex, name,
                                                   color, voltsPerDiv, positionVolts,
                                                   delaySeconds);
    if (slot >= 0)
        m_view->setReferenceUnit(slot, unit);
    if (slot < 0) {
        statusBar()->showMessage(QStringLiteral(
            "Both reference slots are already in use \u2014 remove one first."), 3000);
        return;
    }
    enforceReferenceGutterBudget();
    m_referenceSelectedSlot = slot;
    refreshReferencesPanel();
    statusBar()->showMessage(QStringLiteral("Loaded %1").arg(path), 3000);
}

namespace {
// Magic/version for the native .vsxwave format — a full-state snapshot of
// Capture or Live, distinct from a reference's own .vsxref (a single
// channel's worth of data plus display settings) and from a recording's
// .vsxrec+meta.ini (built for a file growing incrementally while being
// written). One QDataStream-framed file, written and read in one pass
// each way, the same shape as .vsxref but with everything a full
// Capture/Live view needs rather than just one trace.
constexpr quint32 kWaveMagic   = 0x56535857;   // "VSXW"
// Version 2 appends the view centre (fraction of the saved buffer) and the
// Live time axis origin; version 1 files remain readable without them.
constexpr quint32 kWaveVersion = 3;   // 3: filters (channels and Math), Math k / c
}

// Without a scope connected, no device has reported how many channels it
// has, so the count is still 0: the channel cards stay hidden, the plot
// draws no channel at all and decoding reports every channel off — a file
// loaded but showed nothing. Opening a .vsxwave or .vsxrec then takes the
// count from the file (highest channel it holds). A connected scope's own
// count always wins, and replaces this one when it appears
// (onDeviceChanged()). Nothing here touches hardware: every
// AcquisitionManager setter is already a no-op without a device.
void MainWindow::adoptChannelCountWithoutDevice(int fileChannels)
{
    fileChannels = std::min(fileChannels, 4);
    if (m_acq->ready() || fileChannels <= m_channelCount)
        return;
    m_channelCount = fileChannels;
    for (int ch = 0; ch < 4; ++ch)
        m_channelCard[ch]->setVisible(ch < fileChannels);
    m_view->setChannelCount(m_channelCount);
    m_nav->setChannelCount(m_channelCount);
    refreshMathSources();
}

void MainWindow::openNativeWaveform(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        statusBar()->showMessage(QStringLiteral("Could not read %1").arg(path), 4000);
        return;
    }
    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_5_15);
    quint32 magic = 0, version = 0;
    in >> magic >> version;
    if (magic != kWaveMagic) {
        statusBar()->showMessage(
            QStringLiteral("%1 is not a VimuScopeX waveform file.").arg(path), 4000);
        return;
    }
    if (version > kWaveVersion) {
        statusBar()->showMessage(
            QStringLiteral("%1 was saved by a newer VimuScopeX: update to open it.").arg(path), 5000);
        return;
    }

    quint32 modeRaw = 0;
    double timebase = 0.0, sampleRate = 0.0, triggerPoint = -1.0;
    quint32 trigMode = 0, trigDir = 0;
    int trigSourceIndex = 0, preTrigger = 50;
    double trigLevel = 0.0;
    in >> modeRaw >> timebase >> sampleRate >> triggerPoint;
    in >> trigMode >> trigDir >> trigSourceIndex >> trigLevel >> preTrigger;

    struct ChannelData {
        quint32 mode = 0, coupling = 0;
        double page = 0.0, scale = 1.0;
        QString probeName;
        bool invert = false;
        double inputOffset = 0.0, dcOffset = 0.0, offsetPct = 0.0;
        QColor colour;
        QVector<double> samples;
    };
    std::array<ChannelData, 4> channels;
    for (int ch = 0; ch < 4; ++ch) {
        ChannelData &d = channels[ch];
        in >> d.mode >> d.page >> d.scale >> d.coupling >> d.probeName
           >> d.invert >> d.inputOffset >> d.dcOffset >> d.offsetPct
           >> d.colour >> d.samples;
    }

    bool mathEnabled = false;
    quint32 mathOp = 0, mathSourceA = 0, mathSourceB = 0;
    double mathVoltsPage = -1.0;
    QColor mathColour;
    in >> mathEnabled >> mathOp >> mathSourceA >> mathSourceB >> mathVoltsPage >> mathColour;

    double savedCentre = -1.0;   // < 0: not saved (version 1)
    bool   hasOrigin = false;
    double origin = 0.0;
    if (version >= 2)
        in >> savedCentre >> hasOrigin >> origin;
    // Version 3: filters and Math k / c (older files: as they are now).
    struct FilterData { quint32 type = 0; double low = 10.0, high = 100e3; };
    std::array<FilterData, 4> channelFilters;
    FilterData mathFilter;
    double mathK = 1.0, mathC = 0.0;
    const bool hasFilters = version >= 3;
    if (hasFilters) {
        for (FilterData &f : channelFilters)
            in >> f.type >> f.low >> f.high;
        in >> mathFilter.type >> mathFilter.low >> mathFilter.high >> mathK >> mathC;
    }

    if (in.status() != QDataStream::Ok) {
        statusBar()->showMessage(QStringLiteral("%1 is corrupt or unreadable.").arg(path), 4000);
        return;
    }
    const int mode = int(modeRaw);
    if (mode != ModeCapture && mode != ModeStream) {
        statusBar()->showMessage(
            QStringLiteral("%1 has an unrecognised mode.").arg(path), 4000);
        return;
    }

    // A view of the file, not a change to the setup: the setup kept first
    // (once — another file opened while viewing one keeps the original).
    if (m_fileViewSnapshot.isEmpty())
        takeFileViewSnapshot();
    m_applyingFileView = true;
    struct ApplyingGuard { bool &flag; ~ApplyingGuard() { flag = false; } } applying { m_applyingFileView };

    // Replaces the current display, same as Open already does for a
    // .vsxrec — switching mode first if needed, same pattern that
    // branch already uses.
    stopAcquisition();
    // Raised before anything below that reconfigures a channel (mode,
    // volts/div, scale, coupling all do) — see m_suppressSamplesReady's own
    // comment for why. Cleared further down, after the loaded samples are
    // in place, with a short deferred grace period rather than
    // immediately.
    m_suppressSamplesReady = true;
    // Set before any restoration below, so a widget handler that calls
    // refreshLiveView() along the way leaves the loaded data alone rather
    // than reading the ring over it (see m_liveLoaded).
    if (mode == ModeStream)
        m_liveLoaded = true;
    if (m_acquisitionMode != mode) {
        if (m_acqMode) {
            if (auto *button = m_acqMode->button(mode))
                button->setChecked(true);
        }
        setAcquisitionMode(mode);
    }

    if (auto *b = m_trigModeGroup->button(int(trigMode)))
        b->setChecked(true);
    if (auto *b = m_trigDirGroup->button(int(trigDir)))
        b->setChecked(true);
    if (trigSourceIndex >= 0 && trigSourceIndex < m_trigSource->count())
        m_trigSource->setCurrentIndex(trigSourceIndex);
    m_trigLevel->setValue(trigLevel);
    m_preTrigger->setValue(preTrigger);
    // Restoring the widgets above never actually tells ScopeView its own
    // trigger is armed — that only happens via setTrigger(), which none of
    // those widgets' own signal handlers call on their own. Left unset,
    // m_triggerArmed stays false, and viewAnchor() (what windowStart()
    // centres the visible window on) falls back to the middle of the whole
    // buffer instead of the actual, correctly-restored trigger point —
    // still a valid, in-bounds window, just not necessarily the one
    // actually saved. Armed unconditionally, regardless of mode: a loaded
    // .vsxwave always represents a completed capture with a known trigger
    // instant, never an active, still-waiting-to-trigger state.
    //
    // applyTrigger() first: the mode/edge buttons above only listen to
    // idClicked, which setChecked() doesn't emit, so without it the
    // hardware kept its old trigger mode/edge and Run used those. Unlike
    // applyTimebase() it touches no rates or buffers. It also sets the
    // view's armed state (false in Live), which is why the view-only call
    // comes after it.
    applyTrigger();
    m_view->setTrigger(trigSourceIndex, trigLevel, int(trigDir), triggerPoint >= 0.0);

    // Widgets set normally here, signals firing rather than blocked, so
    // each one's own existing handler reconfigures the hardware to match —
    // exactly as if the person had clicked through these settings
    // themselves, which is also what makes pressing Run afterward acquire
    // fresh data under these exact restored settings rather than whatever
    // the instrument happened to be configured for beforehand. Sample data
    // is injected afterward (below), overwriting whatever those handlers'
    // own side effects would otherwise show, since acquisition is stopped
    // throughout.
    //
    // Without a scope connected, no device has reported how many channels
    // it has, so the count is still 0: the channel cards stay hidden, the
    // plot draws no channel at all and decoding reports every channel off —
    // the file loaded but showed nothing. Take the count from the file
    // instead (the highest channel it has on or carrying data). A connected
    // scope's own count always wins, and replaces this one when it appears
    // (onDeviceChanged()). Nothing here touches hardware: every
    // AcquisitionManager setter is already a no-op without a device.
    {
        int fileChannels = 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (!channels[ch].samples.isEmpty() || int(channels[ch].mode) != ChannelOff)
                fileChannels = ch + 1;
        }
        adoptChannelCountWithoutDevice(fileChannels);
    }

    for (int ch = 0; ch < 4; ++ch) {
        const ChannelData &d = channels[ch];
        ChannelControls &c = m_channel[ch];
        if (auto *b = c.mode->button(int(d.mode)))
            b->setChecked(true);
        // setChecked() alone skips the on/off handler (hardware enable,
        // overview visibility, range control) — it only listens to
        // idClicked. Without this, a channel the file had on stayed
        // disabled in hardware and Run never acquired it.
        emit c.mode->idClicked(int(d.mode));
        setComboByNearestValue(c.voltsPage, d.page);
        setComboByNearestValue(c.scale, d.scale);
        const int couplingIndex = c.coupling->findData(int(d.coupling));
        if (couplingIndex >= 0)
            c.coupling->setCurrentIndex(couplingIndex);
        const int probeIndex = c.probe->findText(d.probeName);
        if (probeIndex >= 0)
            c.probe->setCurrentIndex(probeIndex);
        c.invert->setChecked(d.invert);
        c.inputOffset->setValue(d.inputOffset);
        c.offsetV->setValue(d.dcOffset);
        setChannelPositionPercent(ch, d.offsetPct);
        // (Colours stay the user's: a file's trace colours aren't applied.)
    }

    m_mathEnabled->setChecked(mathEnabled);
    if (const int i = m_mathOp->findData(int(mathOp)); i >= 0)
        m_mathOp->setCurrentIndex(i);
    if (const int i = m_mathSourceA->findData(int(mathSourceA)); i >= 0)
        m_mathSourceA->setCurrentIndex(i);
    if (const int i = m_mathSourceB->findData(int(mathSourceB)); i >= 0)
        m_mathSourceB->setCurrentIndex(i);
    setComboByNearestValue(m_mathVoltsPage, mathVoltsPage);
    if (mathColour.isValid())
        m_view->setMathColor(mathColour);
    if (hasFilters) {
        // Frequencies first: the type change applies them.
        for (int ch = 0; ch < 4; ++ch) {
            ChannelControls &c = m_channel[ch];
            if (!c.filterType)
                continue;
            if (int(channelFilters[ch].type) == ScopeView::FilterHighRes) {
                const int n = c.filterHighResN->findData(int(std::lround(channelFilters[ch].low)));
                c.filterHighResN->setCurrentIndex(n >= 0 ? n : 1);
            } else {
                c.filterLowHz->setValue(channelFilters[ch].low);
            }
            c.filterHighHz->setValue(channelFilters[ch].high);
            const int i = c.filterType->findData(int(channelFilters[ch].type));
            c.filterType->setCurrentIndex(i >= 0 ? i : 0);
        }
        m_mathFilterLowHz->setValue(mathFilter.low);
        m_mathFilterHighHz->setValue(mathFilter.high);
        const int mi = m_mathFilterType->findData(int(mathFilter.type));
        m_mathFilterType->setCurrentIndex(mi >= 0 ? mi : 0);
        m_mathScaleK->setValue(mathK);
        m_mathScaleC->setValue(mathC);
    }

    // Applied last, deliberately after every other restoration step above:
    // setSecondsPerDiv() silently ignores a non-positive value, so if
    // anything else touches the timebase during channel/trigger/Math
    // restoration (several of those widget signals reconfigure real
    // hardware and are not fully audited for this), this is what makes
    // sure the loaded value is still the one actually on screen once
    // restoration finishes, not whatever the view happened to be showing
    // beforehand. Falls back to the current combo's own value if the saved
    // one is somehow invalid, rather than silently leaving a stale
    // pre-load zoom in place.
    const double safeTimebase = timebase > 0.0 ? timebase : m_timebase->currentData().toDouble();
    // Deliberately NOT applyTimebase(): that function is built for "the
    // person just changed the timebase dropdown" and unconditionally
    // reconfigures the sample rate too — from m_liveRate's current
    // selection in Live, from m_acq's currently configured rate in
    // Capture — regardless of exactOverride, which only affects
    // secondsPerDiv. Calling it here would overwrite the correctly-loaded
    // sample rate with whatever the current combo/device state happens to
    // be, moments before setSamples() below sets it correctly again — a
    // race that should not exist rather than one to try to win. The
    // combo's own currentIndexChanged handler also calls applyTimebase()
    // with no override at all, which is why signals are blocked here:
    // only the displayed selection should change, not trigger that
    // handler's own side effects a second time.
    m_timebase->blockSignals(true);
    setComboByNearestValue(m_timebase, safeTimebase);
    m_timebase->blockSignals(false);
    m_view->setSecondsPerDiv(safeTimebase);
    if (m_acquisitionMode == ModeStream)
        m_liveTimebase = safeTimebase;
    else
        m_captureTimebase = safeTimebase;
    syncNavigator();

    // Last: every widget change above has already had its own side effects
    // run, so this is what actually ends up on screen rather than being
    // immediately overwritten by one of them. Feeds m_nav too — the
    // overview strip at the top — since the normal samplesReady path
    // always does both together and a loaded waveform should look no
    // different there.
    for (int ch = 0; ch < 4; ++ch) {
        // Only channels that actually carry data: ScopeView::setSamples()
        // sets the view-wide m_lastLength (what windowLength() and
        // windowStart() are computed against) to THIS call's sample count.
        // On 2-channel hardware CH3/CH4 are saved as empty arrays, and
        // feeding them last reset m_lastLength to 0, clamping the visible
        // window to windowLength()'s 2-sample minimum — the flat line seen
        // on every load. The normal acquisition path never hits this since
        // the device only reports real channels. Empty or switched-off
        // channels are cleared instead, which leaves m_lastLength alone.
        if (channels[ch].samples.isEmpty() || int(channels[ch].mode) == ChannelOff) {
            m_view->clearChannel(ch);
            continue;
        }
        m_view->setSamples(ch, channels[ch].samples, sampleRate, triggerPoint);
        m_nav->setSamples(ch, channels[ch].samples);
    }
    // Where the saved view was within the buffer, and its time axis: Capture
    // files are trigger-centred (no origin), Live files carry the origin that
    // makes their axis read as time before the newest sample, as live does.
    m_view->followTrigger();
    if (savedCentre >= 0.0)
        m_view->setViewCentreFraction(savedCentre);
    if (hasOrigin)
        m_view->setAbsoluteTimeOrigin(origin);
    else
        m_view->clearAbsoluteTimeOrigin();
    if (mode == ModeStream) {
        m_liveViewOffset = 0;
        m_liveLoadedHasOrigin = hasOrigin;
        m_liveLoadedOrigin = origin;
        m_liveLoadedCentre = m_view->viewportCentreFraction();
    }

    // Mode switching restores each mode from its own snapshot, so the loaded
    // data has to live there too — otherwise leaving this mode and coming
    // back showed whatever was acquired before the file was opened.
    CaptureSnapshot &snap = mode == ModeStream ? m_liveSnapshot : m_captureSnapshot;
    snap.hasData = false;
    for (int ch = 0; ch < 4; ++ch) {
        const bool loaded = !channels[ch].samples.isEmpty()
                            && int(channels[ch].mode) != ChannelOff;
        snap.samples[ch] = loaded ? channels[ch].samples : QVector<double>();
        snap.valid[ch] = loaded;
        snap.hasData = snap.hasData || loaded;
    }
    snap.sampleRate = sampleRate;
    snap.triggerPoint = triggerPoint;

    // Again now that m_lastLength reflects the loaded buffer — the viewport
    // box on the overview strip is a fraction of that length.
    syncNavigator();

    // m_suppressSamplesReady is deliberately left set here rather than
    // cleared on a timer — acquisition stays stopped after a load, so
    // nothing should legitimately produce a new sample frame until the
    // person explicitly starts one; startAcquisition() clears it at that
    // well-defined point instead (see its own comment). A fixed delay here
    // would have been a guess at how long the channel reconfiguration
    // above might still take to settle on the hardware side, and guessing
    // wrong in either direction is exactly what caused this to need fixing
    // twice: too short and a delayed frame still slips through, too long
    // and a person pressing Run quickly after loading would have their own
    // first real acquisition silently swallowed too.
    refreshCards();
    statusBar()->showMessage(QStringLiteral("Loaded %1").arg(path), 3000);
}

QString MainWindow::ensureExtension(QString path, const QString &extension)
{
    if (!path.endsWith(extension, Qt::CaseInsensitive))
        path += extension;
    return path;
}

void MainWindow::setComboByNearestValue(QComboBox *box, double value)
{
    if (!box || box->count() == 0)
        return;
    int index = box->findData(value);
    if (index < 0) {
        index = 0;
        double bestDiff = std::fabs(box->itemData(0).toDouble() - value);
        for (int i = 1; i < box->count(); ++i) {
            const double diff = std::fabs(box->itemData(i).toDouble() - value);
            if (diff < bestDiff) {
                bestDiff = diff;
                index = i;
            }
        }
    }
    box->setCurrentIndex(index);
}

void MainWindow::saveCurrentView()
{
    // Capture/Live gets the native format alongside PNG/CSV; Record gets
    // PNG only. Record's CSV export was deliberately dropped rather than
    // built as the full-file, background-threaded export it would
    // actually need — refreshRecordView()'s own comment explains why: a
    // recording can be hundreds of millions of samples, far more than
    // what's ever loaded into memory for display, so exporting from what's
    // loaded would silently produce a file missing almost everything.
    // Record already persists continuously via .vsxrec and Temporary/
    // Permanent regardless, so a native re-save doesn't apply to it either.
    // Record: the recording itself (.vsxrec — keeps a Temporary one) or a picture.
    const bool nativeAvailable = m_acquisitionMode != ModeRecord;
    const QString filters = nativeAvailable
        ? QStringLiteral("VimuScopeX waveform (*.vsxwave);;PNG image (*.png);;CSV data (*.csv)")
        : QStringLiteral("VimuScopeX recording (*.vsxrec);;PNG image (*.png)");

    QString selectedFilter;
    const QString chosen = QFileDialog::getSaveFileName(
        this, QStringLiteral("Save"), QString(), filters, &selectedFilter);
    if (chosen.isEmpty())
        return;

    if (selectedFilter.contains(QStringLiteral("*.vsxrec"))) {
        saveRecordingAs(ensureExtension(chosen, QStringLiteral(".vsxrec")));
    } else if (selectedFilter.contains(QStringLiteral("*.vsxwave"))) {
        saveViewAsNative(ensureExtension(chosen, QStringLiteral(".vsxwave")));
    } else if (selectedFilter.contains(QStringLiteral("*.png"))) {
        saveViewAsPng(ensureExtension(chosen, QStringLiteral(".png")));
    } else {
        saveViewAsCsv(ensureExtension(chosen, QStringLiteral(".csv")));
    }
}

void MainWindow::saveRecordingAs(const QString &chosen)
{
    const QString source = m_currentRecordTab;
    if (source.isEmpty() || !QDir(source).exists()) {
        statusBar()->showMessage(QStringLiteral("No recording to save: open or make one first."), 5000);
        return;
    }
    if (m_stream->recording() && QDir(m_stream->recordDirectory()) == QDir(source)) {
        statusBar()->showMessage(QStringLiteral("Still recording: save it once it has finished."), 5000);
        return;
    }
    // <folder>/<name>/ holding <name>.vsxrec and meta.ini: a recording is a
    // folder; opening <name>.vsxrec opens it.
    const QFileInfo target(chosen);
    const QString name = target.completeBaseName();
    QDir parent = target.absoluteDir();
    QString folderName = name;
    for (int n = 2; parent.exists(folderName); ++n)
        folderName = QStringLiteral("%1 (%2)").arg(name, QString::number(n));
    if (!parent.mkdir(folderName)) {
        statusBar()->showMessage(QStringLiteral("Could not create %1").arg(parent.filePath(folderName)), 5000);
        return;
    }
    const QString destination = parent.filePath(folderName);
    QList<QPair<QString, QString>> files;   // from, to
    quint64 total = 0;
    for (const QFileInfo &f : QDir(source).entryInfoList(QDir::Files)) {
        const bool data = f.suffix().compare(QLatin1String(RecordWriter::kFileExtension), Qt::CaseInsensitive) == 0;
        files.append({ f.absoluteFilePath(),
                       QDir(destination).filePath(data ? QStringLiteral("%1.%2").arg(folderName,
                                                             QLatin1String(RecordWriter::kFileExtension))
                                                       : f.fileName()) });
        total += quint64(f.size());
    }
    statusBar()->showMessage(QStringLiteral("Saving recording to %1\u2026").arg(destination));

    // Recordings run to gigabytes: copied in chunks on a thread of its own,
    // progress in the status bar.
    QPointer<MainWindow> self(this);
    std::thread([self, files, total, destination] {
        constexpr qint64 kChunk = 8 * 1024 * 1024;
        QByteArray buffer;
        quint64 done = 0;
        int lastPercent = -1;
        QString error;
        for (const auto &[from, to] : files) {
            QFile in(from), out(to);
            if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly)) {
                error = QStringLiteral("Could not copy %1").arg(QFileInfo(from).fileName());
                break;
            }
            while (!in.atEnd()) {
                buffer = in.read(kChunk);
                if (buffer.isEmpty() || out.write(buffer) != buffer.size()) {
                    error = QStringLiteral("Could not write %1 (disk full?)").arg(to);
                    break;
                }
                done += quint64(buffer.size());
                const int percent = total > 0 ? int(done * 100 / total) : 100;
                if (percent != lastPercent) {
                    lastPercent = percent;
                    QMetaObject::invokeMethod(qApp, [self, percent] {
                        if (self)
                            self->statusBar()->showMessage(QStringLiteral("Saving recording\u2026 %1 %").arg(percent));
                    }, Qt::QueuedConnection);
                }
            }
            if (!error.isEmpty())
                break;
        }
        QMetaObject::invokeMethod(qApp, [self, error, destination] {
            if (!self)
                return;
            self->statusBar()->showMessage(error.isEmpty()
                ? QStringLiteral("Recording saved to %1").arg(destination) : error, 8000);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::saveViewAsNative(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 4000);
        return;
    }

    // What ScopeView holds is right for Capture (the whole capture) and for
    // a Live view that is itself a loaded file. A running/stopped Live view,
    // though, only ever holds the one screen refreshLiveView() last read, so
    // saving that alone left nothing to pan to once reopened. For Live the
    // stream's own history is saved instead (capped like refreshLiveView()'s
    // own read), positioned so the saved view is still the one on screen.
    std::array<QVector<double>, 4> liveHistory;
    bool   useHistory    = false;
    double rate          = m_view->sampleRate();
    double trigger       = m_view->triggerPoint();
    double centreFraction = m_view->viewportCentreFraction();
    bool   hasOrigin     = m_view->hasAbsoluteTimeOrigin();
    double origin        = m_view->absoluteTimeOrigin();
    if (m_acquisitionMode == ModeStream && !m_liveLoaded && m_stream->channels() > 0) {
        constexpr quint64 kMaxSaved = 4ull * 1024 * 1024;
        const double streamRate = m_stream->sampleRate();
        quint64 available = 0;
        for (int ch = 0; ch < m_stream->channels(); ++ch) {
            if (m_channel[ch].mode->checkedId() != ChannelOff)
                available = std::max(available, m_stream->historyAvailable(ch));
        }
        if (streamRate > 0.0 && available > 0) {
            const quint64 count = std::min(available, kMaxSaved);
            const double window = std::min<double>(
                m_view->secondsPerDiv() * ScopeView::HorizontalDivs * streamRate,
                double(available));
            // Samples back from newest, matching rangeFromNewest()'s own
            // offsetFromNewest convention and m_liveViewOffset's.
            const double centreBack = double(m_liveViewOffset) + window / 2.0;
            const qint64 startBack = std::clamp<qint64>(
                qint64(std::llround(centreBack - double(count) / 2.0)),
                0, qint64(available - count));

            QVector<float> block;
            for (int ch = 0; ch < m_stream->channels(); ++ch) {
                if (m_channel[ch].mode->checkedId() == ChannelOff)
                    continue;
                const int got = m_stream->rangeFromNewest(ch, quint64(startBack),
                                                          int(count), &block);
                if (got <= 0)
                    continue;
                liveHistory[ch].resize(got);
                for (int i = 0; i < got; ++i)
                    liveHistory[ch][i] = double(block[i]);
                useHistory = true;
            }
            if (useHistory) {
                rate = streamRate;
                trigger = -1.0;
                // Oldest sample first, so index i is (startBack + count - i)
                // samples back from newest — the same axis refreshLiveView()
                // gives its own window, just longer.
                centreFraction = std::clamp(
                    (double(startBack) + double(count) - centreBack) / double(count), 0.0, 1.0);
                hasOrigin = true;
                origin = -(double(startBack) + double(count)) / streamRate;
            }
        }
    }

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_5_15);
    out << kWaveMagic << kWaveVersion;
    out << quint32(m_acquisitionMode);   // ModeCapture or ModeStream only — Save never offers this in Record
    out << (m_acquisitionMode == ModeStream ? m_liveTimebase : m_captureTimebase);
    out << rate;
    out << trigger;

    out << quint32(m_trigModeGroup->checkedId());
    out << quint32(m_trigDirGroup->checkedId());
    out << m_trigSource->currentIndex();
    out << m_trigLevel->value();
    out << m_preTrigger->value();

    for (int ch = 0; ch < 4; ++ch) {
        const ChannelControls &c = m_channel[ch];
        out << quint32(c.mode->checkedId());
        out << c.voltsPage->currentData().toDouble();
        out << c.scale->currentData().toDouble();
        out << quint32(c.coupling->currentData().toInt());
        out << c.probe->currentText();
        out << c.invert->isChecked();
        out << c.inputOffset->value();
        out << c.offsetV->value();
        out << c.offsetPct->value();
        out << m_colour[ch];
        out << (useHistory ? liveHistory[ch] : m_view->channelSamples(ch));
    }

    out << m_mathEnabled->isChecked();
    out << quint32(m_mathOp->currentData().toInt());
    out << quint32(m_mathSourceA->currentData().toInt());
    out << quint32(m_mathSourceB->currentData().toInt());
    out << m_mathVoltsPage->currentData().toDouble();
    out << m_view->mathColor();
    out << centreFraction << hasOrigin << origin;   // version 2
    // Version 3: each channel's filter, Math's filter, Math's k / c — a
    // filtered capture used to reopen unfiltered.
    for (int ch = 0; ch < 4; ++ch) {
        const ChannelControls &c = m_channel[ch];
        out << quint32(c.filterType ? c.filterType->currentData().toInt() : 0)
            << (c.filterLowHz ? filterLowParam(ch) : 10.0)   // N for High-res
            << (c.filterHighHz ? c.filterHighHz->value() : 100e3);
    }
    out << quint32(m_mathFilterType->currentData().toInt())
        << m_mathFilterLowHz->value() << m_mathFilterHighHz->value()
        << m_mathScaleK->value() << m_mathScaleC->value();

    if (out.status() != QDataStream::Ok) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 4000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
}

MainWindow::~MainWindow()
{
    // Live's worker reads the stream engine: stopped before anything goes.
    if (m_liveThread) {
        m_liveThread->quit();
        m_liveThread->wait();
    }
    // The decoder thread may be mid-decode; let it finish and stop before
    // the window (its QThread's parent) is torn down.
    if (m_decodeTimer)
        m_decodeTimer->stop();
    if (m_decodeThread) {
        m_decodeThread->quit();
        m_decodeThread->wait();
    }
}

namespace {
// Bus row colours per decoder slot — apart from every channel default.
QColor decodeSlotColour(int slot)
{
    return slot == 0 ? QColor(0x4f, 0xc3, 0xf7) : QColor(0xff, 0xb7, 0x4d);
}

// Input choices. CAN picks a wire and a channel (or the difference);
// LIN and K-Line are one wire on one channel.
enum DecodeInput {
    InCanHCh1 = 0, InCanHCh2, InCanLCh1, InCanLCh2, InCanDiff,
    InCh1 = 10, InCh2
};

// The channel a decode input reads (the difference: CH1 first).
int decodeInputChannel(int input)
{
    switch (input) {
    case InCanHCh1: case InCanLCh1: case InCh1: case InCanDiff: return 0;
    case InCanHCh2: case InCanLCh2: case InCh2: return 1;
    default: return -1;
    }
}

// Longest a Record view may span and still be decoded from the file
// (samples per channel); beyond it the row asks to zoom in instead.
constexpr quint64 kMaxRecordDecode = 4ull * 1024 * 1024;
}

QWidget *MainWindow::buildDecodingPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    if (!DecoderEngine::available()) {
        auto *note = new QLabel(QStringLiteral(
            "This build has no protocol decoding. Install libsigrokdecode "
            "(Arch: sudo pacman -S libsigrokdecode), then reconfigure and "
            "rebuild."));
        note->setWordWrap(true);
        layout->addWidget(note);
    }

    for (int slot = 0; slot < 2; ++slot) {
        DecodeSlotUi &u = m_decodeUi[slot];
        layout->addWidget(sectionLabel(QStringLiteral("Decoder %1").arg(slot + 1)));

        u.enable = new QCheckBox(QStringLiteral("Show bus row"));
        u.enable->setEnabled(DecoderEngine::available());
        layout->addWidget(u.enable);

        u.protocol = new QComboBox;
        u.protocol->addItem(QStringLiteral("CAN / CAN FD"), QStringLiteral("can"));
        u.protocol->addItem(QStringLiteral("LIN"), QStringLiteral("lin"));
        u.protocol->addItem(QStringLiteral("K-Line"), QStringLiteral("kline"));
        u.protocol->addItem(QStringLiteral("SENT (SAE J2716)"), QStringLiteral("sent"));
        u.protocol->addItem(QStringLiteral("OBD-II / UDS (on CAN)"), QStringLiteral("obd"));
        u.protocol->setCurrentIndex(slot == 0 ? 0 : 1);
        layout->addWidget(u.protocol);

        layout->addWidget(sectionLabel(QStringLiteral("Input")));
        u.input = new QComboBox;
        layout->addWidget(u.input);

        layout->addWidget(sectionLabel(QStringLiteral("Threshold")));
        auto *thrRow = new QWidget;
        auto *thrLayout = new QHBoxLayout(thrRow);
        thrLayout->setContentsMargins(0, 0, 0, 0);
        u.threshold = new QDoubleSpinBox;
        u.threshold->setRange(-50.0, 50.0);
        u.threshold->setDecimals(2);
        u.threshold->setSingleStep(0.1);
        u.threshold->setSuffix(QStringLiteral(" V"));
        u.invert = new QCheckBox(QStringLiteral("Invert"));
        u.invert->setToolTip(QStringLiteral(
            "Treat the level below the threshold as logic 1. Set automatically "
            "for CAN-H and the differential input, where dominant is high."));
        thrLayout->addWidget(u.threshold, 1);
        layout->addWidget(thrRow);
        // Own line: beside the threshold box the pair is wider than the
        // panel and the label got clipped.
        layout->addWidget(u.invert);

        // CAN settings.
        u.canBox = new QWidget;
        {
            auto *l = new QVBoxLayout(u.canBox);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(4);
            l->addWidget(sectionLabel(QStringLiteral("Bit rate")));
            u.canRate = new QComboBox;
            for (int kbps : { 125, 250, 500, 1000 })
                u.canRate->addItem(kbps >= 1000 ? QStringLiteral("1 Mbit/s")
                                                : QStringLiteral("%1 kbit/s").arg(kbps),
                                   qlonglong(kbps) * 1000);
            u.canRate->setCurrentIndex(2);
            l->addWidget(u.canRate);
            l->addWidget(sectionLabel(QStringLiteral("CAN FD data rate")));
            u.canFdRate = new QComboBox;
            for (int mbps : { 1, 2, 4, 5, 8 })
                u.canFdRate->addItem(QStringLiteral("%1 Mbit/s").arg(mbps), qlonglong(mbps) * 1000000);
            u.canFdRate->setCurrentIndex(1);
            u.canFdRate->setToolTip(QStringLiteral(
                "Used for the data phase of CAN FD frames with bit-rate switch. "
                "Classic CAN frames ignore it."));
            l->addWidget(u.canFdRate);
            l->addWidget(sectionLabel(QStringLiteral("Sample point")));
            u.samplePoint = new QDoubleSpinBox;
            u.samplePoint->setRange(50.0, 90.0);
            u.samplePoint->setDecimals(1);
            u.samplePoint->setValue(70.0);
            u.samplePoint->setSuffix(QStringLiteral(" %"));
            l->addWidget(u.samplePoint);
        }
        layout->addWidget(u.canBox);

        // LIN settings.
        u.linBox = new QWidget;
        {
            auto *l = new QVBoxLayout(u.linBox);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(4);
            l->addWidget(sectionLabel(QStringLiteral("Baud rate")));
            u.linBaud = new QComboBox;
            // Auto: measured from each frame's sync byte (the 19200 default
            // misread a 9600 bus: sync errors, data bytes taken for breaks).
            u.linBaud->addItem(QStringLiteral("Auto (from the sync byte)"), qlonglong(0));
            for (int baud : { 2400, 4800, 9600, 10417, 19200, 20000 })
                u.linBaud->addItem(QString::number(baud), qlonglong(baud));
            u.linBaud->setCurrentIndex(0);
            l->addWidget(u.linBaud);
            l->addWidget(sectionLabel(QStringLiteral("Version")));
            u.linVersion = new QComboBox;
            u.linVersion->addItem(QStringLiteral("LIN 2.x (enhanced checksum)"), 2);
            u.linVersion->addItem(QStringLiteral("LIN 1.x (classic checksum)"), 1);
            l->addWidget(u.linVersion);
        }
        layout->addWidget(u.linBox);

        // K-Line settings.
        u.klineBox = new QWidget;
        {
            auto *l = new QVBoxLayout(u.klineBox);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(4);
            l->addWidget(sectionLabel(QStringLiteral("Baud rate")));
            u.klineBaud = new QComboBox;
            for (int baud : { 10400, 9600 })
                u.klineBaud->addItem(QString::number(baud), qlonglong(baud));
            l->addWidget(u.klineBaud);
            l->addWidget(sectionLabel(QStringLiteral("Framing")));
            u.klineFraming = new QComboBox;
            u.klineFraming->addItem(QStringLiteral("KWP2000 (ISO 14230)"), false);
            u.klineFraming->addItem(QStringLiteral("ISO 9141-2 (idle gaps)"), true);
            l->addWidget(u.klineFraming);
        }
        layout->addWidget(u.klineBox);

        // SENT settings.
        u.sentBox = new QWidget;
        {
            auto *l = new QVBoxLayout(u.sentBox);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(4);
            l->addWidget(sectionLabel(QStringLiteral("Tick")));
            u.sentTick = new QComboBox;
            for (double us : { 3.0, 4.0, 5.0, 6.0, 10.0, 20.0, 30.0, 45.0, 60.0, 90.0 })
                u.sentTick->addItem(QStringLiteral("%1 \u00b5s").arg(us), us);
            u.sentTick->setToolTip(QStringLiteral(
                "The sensor's nominal tick (3 \u00b5s for most). Its real tick is measured from every "
                "sync pulse, so a sensor clock up to 25 % off still decodes."));
            l->addWidget(u.sentTick);
            l->addWidget(sectionLabel(QStringLiteral("Data nibbles")));
            u.sentNibbles = new QComboBox;
            for (int n = 6; n >= 1; --n)
                u.sentNibbles->addItem(QString::number(n), n);
            u.sentNibbles->setToolTip(QStringLiteral(
                "Data nibbles per frame (6 for most sensors: two 12-bit values, shown as FC1 and FC2)."));
            l->addWidget(u.sentNibbles);
            l->addWidget(sectionLabel(QStringLiteral("Show")));
            u.sentMode = new QComboBox;
            u.sentMode->addItem(QStringLiteral("Fast channel (data)"), 0);
            u.sentMode->addItem(QStringLiteral("Slow channel (serial messages)"), 1);
            u.sentMode->addItem(QStringLiteral("SPC (with master trigger)"), 2);
            u.sentMode->setToolTip(QStringLiteral(
                "Fast: each frame's status, data nibbles and CRC. Slow: the short (16-frame) and "
                "enhanced (18-frame) serial messages carried in status bits 2 and 3. SPC: frames "
                "requested by the ECU's master trigger pulse, the trigger shown. Use both decoding "
                "slots on the same input to see two at once."));
            l->addWidget(u.sentMode);
        }
        layout->addWidget(u.sentBox);

        // OBD-II / UDS: CAN's own settings (above) plus which IDs to read.
        u.diagBox = new QWidget;
        {
            auto *l = new QVBoxLayout(u.diagBox);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(4);
            l->addWidget(sectionLabel(QStringLiteral("Diagnostic IDs")));
            u.diagIds = new QComboBox;
            u.diagIds->addItem(QStringLiteral("OBD-II / UDS standard"), 0);
            u.diagIds->addItem(QStringLiteral("Also 600\u20137FF"), 1);
            u.diagIds->addItem(QStringLiteral("Every ID that looks like ISO-TP"), 2);
            u.diagIds->setToolTip(QStringLiteral(
                "Standard: 7DF, 7E0\u20137EF and 29-bit 18DA / 18DB IDs (OBD-II and most UDS). Many "
                "makers also put UDS on IDs in 600\u20137FF; the last choice tries every ID."));
            l->addWidget(u.diagIds);
        }
        layout->addWidget(u.diagBox);

        setDecodeProtocol(slot);

        connect(u.enable, &QCheckBox::toggled, this, [this, slot](bool) { decodeSettingsChanged(slot); });
        connect(u.protocol, &QComboBox::currentIndexChanged, this, [this, slot](int) {
            setDecodeProtocol(slot);
            decodeSettingsChanged(slot);
        });
        connect(u.input, &QComboBox::currentIndexChanged, this, [this, slot](int) {
            applyDecodeInputDefaults(slot);
            decodeSettingsChanged(slot);
        });
        connect(u.threshold, &QDoubleSpinBox::valueChanged, this, [this, slot](double) { decodeSettingsChanged(slot); });
        connect(u.invert, &QCheckBox::toggled, this, [this, slot](bool) { decodeSettingsChanged(slot); });
        for (QComboBox *box : { u.canRate, u.canFdRate, u.linBaud, u.linVersion,
                                u.klineBaud, u.klineFraming, u.sentTick, u.sentNibbles, u.sentMode,
                                u.diagIds })
            connect(box, &QComboBox::currentIndexChanged, this, [this, slot](int) { decodeSettingsChanged(slot); });
        connect(u.samplePoint, &QDoubleSpinBox::valueChanged, this, [this, slot](double) { decodeSettingsChanged(slot); });
    }
    layout->addStretch();

    // The decoder runs on its own thread for the app's whole life, so
    // libsigrokdecode and its embedded Python only ever see that thread.
    qRegisterMetaType<DecodeResult>("DecodeResult");
    m_decodeThread = new QThread(this);
    m_decoder = new DecoderEngine;
    m_decoder->moveToThread(m_decodeThread);
    connect(m_decodeThread, &QThread::finished, m_decoder, &QObject::deleteLater);
    connect(m_decoder, &DecoderEngine::finished, this, &MainWindow::onDecodeFinished);
    m_decodeThread->start();

    m_decodeTimer = new QTimer(this);
    m_decodeTimer->setInterval(250);
    connect(m_decodeTimer, &QTimer::timeout, this, &MainWindow::onDecodeTick);
    m_decodeTimer->start();

    // Two full decoder slots are taller than the panel on most screens.
    // The scroll area reaches the panel's right edge (see buildDetailPanel())
    // so its bar sits there; the content keeps the usual 10 px gap from it.
    // Transparent all the way down, or the viewport fills itself with the
    // default window colour and shows as a differently tinted block.
    layout->setContentsMargins(0, 0, 10, 0);
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("panelScroll"));
    page->setObjectName(QStringLiteral("panelScrollPage"));
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#panelScroll, QWidget#qt_scrollarea_viewport,"
        " QWidget#panelScrollPage { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    return scroll;
}

// Protocol changed: which settings box shows and which inputs make sense.
void MainWindow::setDecodeProtocol(int slot)
{
    DecodeSlotUi &u = m_decodeUi[slot];
    const QString proto = u.protocol->currentData().toString();
    const bool canLike = proto == QLatin1String("can") || proto == QLatin1String("obd");   // OBD-II / UDS rides on CAN
    u.canBox->setVisible(canLike);
    u.diagBox->setVisible(proto == QLatin1String("obd"));
    // CAN's polarity is detected from the signal (see DecoderEngine), so
    // Invert would only be a way to get it wrong there.
    u.invert->setVisible(!canLike);
    u.linBox->setVisible(proto == QLatin1String("lin"));
    u.klineBox->setVisible(proto == QLatin1String("kline"));
    u.sentBox->setVisible(proto == QLatin1String("sent"));

    u.input->blockSignals(true);
    u.input->clear();
    if (canLike) {
        u.input->addItem(QStringLiteral("CAN-H on CH1"), int(InCanHCh1));
        u.input->addItem(QStringLiteral("CAN-H on CH2"), int(InCanHCh2));
        u.input->addItem(QStringLiteral("CAN-L on CH1"), int(InCanLCh1));
        u.input->addItem(QStringLiteral("CAN-L on CH2"), int(InCanLCh2));
        u.input->addItem(QStringLiteral("CAN-H \u2212 CAN-L (CH1 \u2212 CH2)"), int(InCanDiff));
    } else {
        u.input->addItem(QStringLiteral("CH1"), int(InCh1));
        u.input->addItem(QStringLiteral("CH2"), int(InCh2));
        u.input->setCurrentIndex(slot == 1 ? 1 : 0);
    }
    u.input->blockSignals(false);
    applyDecodeInputDefaults(slot);
}

// The usual threshold and polarity for the chosen wire, so a new setup
// decodes without touching them. CAN at transceiver RX polarity is 1 when
// recessive: CAN-L rests high and drops when dominant; CAN-H and the
// difference rise when dominant, so those are inverted.
void MainWindow::applyDecodeInputDefaults(int slot)
{
    DecodeSlotUi &u = m_decodeUi[slot];
    const int input = u.input->currentData().toInt();
    double threshold = 6.0;       // LIN / K-Line: half of a 12 V bus
    bool invert = false;
    if (u.protocol->currentData().toString() == QLatin1String("sent"))
        threshold = 2.5;          // SENT: a 5 V signal
    switch (input) {
    case InCanHCh1: case InCanHCh2: threshold = 3.0; invert = true;  break;
    case InCanLCh1: case InCanLCh2: threshold = 2.0; invert = false; break;
    case InCanDiff:                 threshold = 0.9; invert = true;  break;
    default: break;
    }
    u.threshold->blockSignals(true);
    u.invert->blockSignals(true);
    u.threshold->setValue(threshold);
    u.invert->setChecked(invert);
    u.threshold->blockSignals(false);
    u.invert->blockSignals(false);
}

void MainWindow::decodeSettingsChanged(int slot)
{
    ++m_decodeRev[slot];
    const DecodeSlotUi &u = m_decodeUi[slot];
    if (u.enable && !u.enable->isChecked() && m_view && m_decodeRowShown[slot]) {
        m_view->setBusRow(slot, false, QString(), QColor(), {}, QString());
        m_busRows[size_t(slot)].valid = false;
        m_decodeRowShown[slot] = false;
    }
    onDecodeTick();
}

void MainWindow::showDecodeMessage(int slot, const QString &message)
{
    const DecodeSlotUi &u = m_decodeUi[slot];
    {
        const QString proto = u.protocol->currentData().toString();
        if (proto == QLatin1String("obd")) {
            m_diagData[size_t(slot)] = DiagData();
            m_diagData[size_t(slot)].message = message;
            ++m_diagRev;
        }
        if (proto == QLatin1String("lin") || proto == QLatin1String("kline") || proto == QLatin1String("sent")) {
            m_serialHealth[size_t(slot)] = SerialHealthData();   // stale otherwise, like CAN's
            m_serialHealth[size_t(slot)].message = message;
            ++m_serialHealthRev;
        }
    }
    if (u.protocol->currentData().toString() == QLatin1String("can")) {
        // Nothing is being decoded: the frames of the last decode no longer
        // describe what's on screen (a stale table stayed on the plot).
        m_canFrames.clear();
        m_canStrayErrors = 0;
        m_canSampleCount = 0.0;
        m_canDecoded = false;
        m_canMessage = message;
        ++m_canHealthRev;
    }
    m_busRows[size_t(slot)].valid = false;   // a message, nothing to re-place
    m_view->setBusRow(slot, true, u.protocol->currentText().section(QLatin1Char(' '), 0, 0),
                      decodeSlotColour(slot), {}, message);
    m_decodeRowShown[slot] = true;
}

void MainWindow::onDecodeTick()
{
    if (!m_view || !m_decoder)
        return;
    for (int slot = 0; slot < 2; ++slot) {
        const DecodeSlotUi &u = m_decodeUi[slot];
        if (!u.enable || !u.enable->isChecked())
            continue;
        // One job per slot at a time: while one runs, newer data just waits
        // for the next tick, so a slow decode never builds a backlog.
        if (m_decodeInFlight[slot])
            continue;
        const QVector<double> key {
            double(m_view->dataGeneration()), double(m_decodeRev[slot]),
            double(m_acquisitionMode), double(m_recordViewStart), double(m_recordViewLength)
        };
        if (key == m_decodeLastKey[slot])
            continue;
        m_decodeLastKey[slot] = key;

        DecodeJob job;
        double scale = 1.0;
        QString message;
        if (!buildDecodeJob(slot, &job, &scale, &message)) {
            showDecodeMessage(slot, message);
            continue;
        }
        job.slot = slot;
        job.id = ++m_decodeJobCounter;
        m_decodeInFlight[slot] = true;
        m_decodeInFlightId[slot] = job.id;
        m_decodeIndexScale[slot] = scale;
        DecoderEngine *engine = m_decoder;
        QMetaObject::invokeMethod(engine, [engine, job] { engine->decode(job); },
                                  Qt::QueuedConnection);
    }
}

// Gathers the analog trace a slot decodes, in display volts. Capture and
// Live decode exactly what ScopeView holds (full rate); Record's view is a
// decimated page, so its raw samples are read from the file for the
// visible span instead, and mapped back onto the page by indexScale.
bool MainWindow::buildDecodeJob(int slot, DecodeJob *job, double *indexScale, QString *message)
{
    const DecodeSlotUi &u = m_decodeUi[slot];
    // OBD-II / UDS is CAN with the diagnostic layer on top.
    const QString chosen = u.protocol->currentData().toString();
    const bool diagnostics = chosen == QLatin1String("obd");
    const QString proto = diagnostics ? QStringLiteral("can") : chosen;
    job->protocol = proto;
    job->diagnostics = diagnostics;
    job->diagIds = u.diagIds ? u.diagIds->currentData().toInt() : 0;
    job->threshold = u.threshold->value();
    job->invert = u.invert->isChecked();
    double bitRate = 0.0;
    if (proto == QLatin1String("can")) {
        job->bitrate = u.canRate->currentData().toLongLong();
        job->fastBitrate = u.canFdRate->currentData().toLongLong();
        job->samplePoint = u.samplePoint->value();
        bitRate = double(job->bitrate);
    } else if (proto == QLatin1String("lin")) {
        job->baud = u.linBaud->currentData().toLongLong();       // 0: Auto
        job->linVersion = u.linVersion->currentData().toInt();
        bitRate = job->baud > 0 ? double(job->baud) : 9600.0;   // Auto: margins sized for the slowest common rate
    } else if (proto == QLatin1String("sent")) {
        job->sentTickUs = u.sentTick->currentData().toDouble();
        job->sentNibbles = u.sentNibbles->currentData().toInt();
        job->sentMode = u.sentMode ? u.sentMode->currentData().toInt() : 0;
        bitRate = 1e6 / std::max(1.0, job->sentTickUs);         // margins and rate checks count in ticks
    } else {
        job->baud = u.klineBaud->currentData().toLongLong();
        job->gapFraming = u.klineFraming->currentData().toBool();
        // ISO 9141 / 14230 messages are at least 25 ms apart (P2 minimum).
        job->gapBits = std::max<qint64>(1, qint64(std::llround(0.025 * double(job->baud))));
        bitRate = double(job->baud);
    }

    int chA = 0, chB = -1;
    switch (u.input->currentData().toInt()) {
    case InCanHCh1: case InCanLCh1: case InCh1: chA = 0; break;
    case InCanHCh2: case InCanLCh2: case InCh2: chA = 1; break;
    case InCanDiff: chA = 0; chB = 1; break;
    default: break;
    }
    for (int ch : { chA, chB }) {
        if (ch < 0)
            continue;
        if (ch >= m_channelCount || m_channel[ch].mode->checkedId() == ChannelOff) {
            *message = QStringLiteral("CH%1 is off").arg(ch + 1);
            return false;
        }
    }

    double rate = 0.0;
    QVector<double> a, b;
    *indexScale = 1.0;
    m_decodeLead[size_t(slot)] = 0.0;
    m_decodeOrigin[size_t(slot)] = 0.0;
    if (m_acquisitionMode == ModeRecord) {
        if (m_currentRecordTab.isEmpty() || m_stream->recorded() == 0 || m_recordViewLength == 0) {
            *message = QStringLiteral("No recording to decode");
            return false;
        }
        const quint64 span = m_recordViewLength;
        if (span > kMaxRecordDecode) {
            *message = QStringLiteral("Zoom in to decode (view spans %1 M samples, limit %2 M)")
                           .arg(double(span) / 1e6, 0, 'f', 1)
                           .arg(double(kMaxRecordDecode) / 1e6, 0, 'f', 1);
            return false;
        }
        rate = m_stream->sampleRate();
        // CAN: 1000 bit times of context each side of the window (a classic
        // frame is at most ~130 bits, an FD one ~650), so a frame cut by an
        // edge decodes whole. The decoder starts at an idle gap and reports
        // only the window (DecodeJob::visibleFrom / visibleTo).
        // LIN and K-Line the same way: a LIN frame is ~130 bit times with its
        // break; a K-Line message can run to hundreds of bytes with gaps
        // between them, so it gets 4000.
        quint64 lead = 0, trail = 0;
        {
            const double marginBits = proto == QLatin1String("kline") ? 4000.0 : 1000.0;
            // ...but never more than half the decode limit each side (4000
            // K-Line bits at 125 MS/s would be ~48 M samples).
            const quint64 margin = std::min<quint64>(quint64(kMaxRecordDecode / 2),
                quint64(std::llround(marginBits * rate / std::max(1.0, bitRate))));
            lead = std::min<quint64>(m_recordViewStart, margin);
            const quint64 viewEnd = m_recordViewStart + span;
            const quint64 recorded = m_stream->recorded();
            trail = recorded > viewEnd ? std::min<quint64>(recorded - viewEnd, margin) : 0;
            job->visibleFrom = qint64(lead);
            job->visibleTo = qint64(lead + span);
            m_decodeLead[size_t(slot)] = double(lead);
        }
        const quint64 readFrom = m_recordViewStart - lead;
        m_decodeOrigin[size_t(slot)] = double(readFrom);
        const int readCount = int(lead + span + trail);
        QVector<float> raw;
        const int gotA = m_stream->read(chA, readFrom, readCount, &raw);
        a.resize(std::max(gotA, 0));
        for (int i = 0; i < a.size(); ++i)
            a[i] = m_view->displayValue(chA, raw[i]);
        if (chB >= 0) {
            const int gotB = m_stream->read(chB, readFrom, readCount, &raw);
            b.resize(std::max(gotB, 0));
            for (int i = 0; i < b.size(); ++i)
                b[i] = m_view->displayValue(chB, raw[i]);
        }
        const int pageLength = m_view->channelSamples(chA).size();
        if (pageLength > 0 && span > 0)
            *indexScale = double(pageLength) / double(span);
    } else {
        rate = m_view->sampleRate();
        a = m_view->channelDisplaySamples(chA);
        if (chB >= 0)
            b = m_view->channelDisplaySamples(chB);
    }

    int n = a.size();
    if (chB >= 0)
        n = std::min(n, int(b.size()));
    if (n < 16 || rate <= 0.0) {
        *message = QStringLiteral("No signal to decode");
        return false;
    }

    // Too few samples per bit can't be decoded reliably; say so rather
    // than show frames that are guesses.
    const double samplesPerBit = rate / bitRate;
    if (samplesPerBit < 4.0) {
        *message = QStringLiteral("Sample rate too low: %1 samples per bit (needs 4 or more)")
                       .arg(samplesPerBit, 0, 'f', 1);
        return false;
    }

    job->sampleRate = rate;
    job->volts.resize(n);
    for (int i = 0; i < n; ++i)
        job->volts[i] = float(chB >= 0 ? a[i] - b[i] : a[i]);
    return true;
}

// Decoded blocks onto the page shown now. Record: the page starts at
// m_recordViewStart and holds pageLength points for m_recordViewLength
// samples; elsewhere positions are already the buffer's own.
void MainWindow::projectBusRow(int slot)
{
    if (!m_view || slot < 0 || slot >= 2 || !m_busRows[size_t(slot)].valid)
        return;
    const BusRowData &row = m_busRows[size_t(slot)];
    double start = 0.0, scale = 1.0;
    if (m_acquisitionMode == ModeRecord && m_recordViewLength > 0) {
        int pageLength = 0;
        for (int ch = 0; ch < std::min(m_channelCount, 4); ++ch)
            pageLength = std::max(pageLength, int(m_view->channelSamples(ch).size()));
        start = double(m_recordViewStart);
        if (pageLength > 0)
            scale = double(pageLength) / double(m_recordViewLength);
    }
    QVector<ScopeView::BusAnnotation> annotations;
    annotations.reserve(row.blocks.size());
    for (const BusBlock &b : row.blocks) {
        ScopeView::BusAnnotation a;
        a.start = (b.start - start) * scale;
        a.end = (b.end - start) * scale;
        a.texts = b.texts;
        a.error = b.error;
        a.group = b.group;
        annotations.append(a);
    }
    m_view->setBusRow(slot, true, row.label, row.colour, annotations, row.message);
}

void MainWindow::onDecodeFinished(const DecodeResult &result)
{
    const int slot = result.slot;
    if (slot < 0 || slot >= 2 || result.id != m_decodeInFlightId[slot])
        return;
    m_decodeInFlight[slot] = false;
    const DecodeSlotUi &u = m_decodeUi[slot];
    if (!m_view || !u.enable || !u.enable->isChecked())
        return;

    const double origin = m_decodeOrigin[size_t(slot)];
    QVector<BusBlock> blocks;
    blocks.reserve(result.annotations.size());
    for (const DecodeAnnotation &d : result.annotations) {
        BusBlock b;
        b.start = origin + d.start;
        b.end = origin + d.end;
        b.texts = d.texts;
        b.error = d.error;
        b.group = d.group;
        blocks.append(b);
    }
    const QVector<BusBlock> &annotations = blocks;   // (emptiness checks below)
    QString message = result.message;
    if (message.isEmpty() && annotations.isEmpty())
        message = QStringLiteral("No frames found \u2014 check input, threshold and bit rate");

    // Enough samples per bit? Below 8 the decoder misreads bits (CAN FD's
    // fast data phase most of all). Checked against what this decode ran at;
    // the FD data rate only once FD traffic is seen. Shown as a mark by the
    // row's name (putting it in the row's message would hide the frames) and
    // explained once in the status bar.
    QString rateWarning;
    {
        const QString protocol = u.protocol->currentData().toString();
        const double rate = result.sampleRate;
        const auto check = [&](double bitRate, const QString &what) {
            if (bitRate <= 0.0 || rate <= 0.0 || !rateWarning.isEmpty())
                return;
            const double perBit = rate / bitRate;
            if (perBit < 8.0)
                rateWarning = QStringLiteral("%1: %2 samples per bit at %3 \u2014 at least 8 are needed; "
                                             "raise the sample rate to %4 or more")
                                  .arg(what).arg(perBit, 0, 'f', 1).arg(formatRate(rate))
                                  .arg(formatRate(8.0 * bitRate));
        };
        if (protocol == QLatin1String("can") || protocol == QLatin1String("obd")) {
            check(u.canRate ? u.canRate->currentData().toDouble() : 0.0, QStringLiteral("CAN"));
            bool fdSeen = false;
            for (const DecodeAnnotation &d : result.annotations) {
                for (const QString &text : d.texts) {
                    if (text.contains(QLatin1String("CRC-17")) || text.contains(QLatin1String("CRC-21"))
                        || text.contains(QLatin1String("BRS")) || text.contains(QLatin1String("FDF"))) {
                        fdSeen = true;
                        break;
                    }
                }
                if (fdSeen)
                    break;
            }
            if (fdSeen)
                check(u.canFdRate ? u.canFdRate->currentData().toDouble() : 0.0,
                      QStringLiteral("CAN FD data phase"));
        } else if (protocol == QLatin1String("lin")) {
            const double chosen = u.linBaud ? u.linBaud->currentData().toDouble() : 0.0;
            check(chosen > 0.0 ? chosen : double(result.detectedBaud), QStringLiteral("LIN"));
        } else if (protocol == QLatin1String("kline")) {
            check(u.klineBaud ? u.klineBaud->currentData().toDouble() : 0.0, QStringLiteral("K-Line"));
        } else if (protocol == QLatin1String("sent") && u.sentTick && rate > 0.0) {
            // Nibbles differ by one tick: ~4 samples per tick tell them apart.
            const double tickUs = u.sentTick->currentData().toDouble();
            const double perTick = rate * tickUs * 1e-6;
            if (perTick < 4.0)
                rateWarning = QStringLiteral("SENT: %1 samples per tick at %2 \u2014 at least 4 are needed; "
                                             "raise the sample rate to %3 or more")
                                  .arg(perTick, 0, 'f', 1).arg(formatRate(rate)).arg(formatRate(4.0e6 / tickUs));
        }
        if (!rateWarning.isEmpty() && rateWarning != m_decodeRateWarning[size_t(slot)])
            statusBar()->showMessage(QStringLiteral("Decoding %1").arg(rateWarning), 10000);
        m_decodeRateWarning[size_t(slot)] = rateWarning;
    }
    QString rowName = u.protocol->currentText().section(QLatin1Char(' '), 0, 0);
    // LIN on Auto: the rate it measured, or that it couldn't.
    if (u.protocol->currentData().toString() == QLatin1String("lin") && u.linBaud
        && u.linBaud->currentData().toLongLong() == 0 && result.message.isEmpty()) {
        if (result.detectedBaud > 0) {
            rowName += QStringLiteral(" %1").arg(result.detectedBaud);
        } else {
            rowName += QStringLiteral(" ?");
            if (message.isEmpty())
                statusBar()->showMessage(QStringLiteral(
                    "LIN: no sync byte in view to measure the bit rate (decoded at 19200); zoom out to "
                    "include a whole frame, or pick the rate in Decoding"), 8000);
        }
    }
    if (!rateWarning.isEmpty())
        rowName += QStringLiteral(" \u26a0");
    BusRowData &row = m_busRows[size_t(slot)];
    row.valid = true;
    row.label = rowName;
    row.colour = decodeSlotColour(slot);
    row.message = message;
    row.blocks = blocks;
    projectBusRow(slot);
    // A newer view may already be waiting: decode it now, not on the next tick.
    QTimer::singleShot(0, this, &MainWindow::onDecodeTick);
    m_decodeRowShown[slot] = true;
    if (u.protocol->currentData().toString() == QLatin1String("obd")) {
        DiagData &d = m_diagData[size_t(slot)];
        d.valid = result.message.isEmpty() || !result.diagMessages.empty();
        d.message = result.message;
        d.messages = result.diagMessages;
        d.sampleRate = result.sampleRate;
        ++m_diagRev;
    }
    {
        const QString proto = u.protocol->currentData().toString();
        if (proto == QLatin1String("lin") || proto == QLatin1String("kline") || proto == QLatin1String("sent")) {
            SerialHealthData &d = m_serialHealth[size_t(slot)];
            d.valid = result.message.isEmpty() || !result.linFrames.empty() || !result.kline.messages.empty()
                      || !result.sentFrames.empty();
            d.sent = result.sentFrames;
            d.sentMessages = result.sentMessages;
            d.sentMode = u.sentMode ? u.sentMode->currentData().toInt() : 0;
            d.sentTickUs = u.sentTick ? u.sentTick->currentData().toDouble() : 3.0;
            d.message = result.message;
            d.lin = result.linFrames;
            d.kline = result.kline;
            d.bitTime = result.busBitTime;
            d.sampleRate = result.sampleRate;
            d.sampleCount = double(result.sampleCount);
            QComboBox *baud = proto == QLatin1String("lin") ? u.linBaud : u.klineBaud;
            d.baud = baud ? baud->currentData().toInt() : 0;
            if (d.baud <= 0)
                d.baud = int(result.detectedBaud);          // LIN on Auto: what the decoder measured
            d.linVersion = u.linVersion ? u.linVersion->currentData().toInt() : 2;
            ++m_serialHealthRev;
        }
    }
    if (u.protocol->currentData().toString() == QLatin1String("can")) {
        m_canFrames = result.canFrames;
        m_canStrayErrors = result.strayErrors;
        m_canSampleCount = double(result.sampleCount);
        m_canSampleRate = result.sampleRate;
        m_canDecoded = result.message.isEmpty() || !result.canFrames.empty();
        m_canMessage.clear();
        ++m_canHealthRev;
    }
    // Data that changed while this ran is picked up by the next tick.
}

void MainWindow::engineSettingsChanged()
{
    ++m_engineRev;
    if (m_engineSource) {
        const int source = m_engineSource->currentData().toInt();
        m_engineSyncBox->setVisible(source == 1);
        m_crankBox->setVisible(source == 2);
        m_engineResetMarkers->setVisible(source == 0);
    }
    if (m_compressionBox)
        m_compressionBox->setVisible(m_compressionEnable->isChecked());
    updateEngineAnalysis();
}

void MainWindow::updateEngineAnalysis()
{
    VSX_TRACE("updateEngineAnalysis");
    if (!m_view || !m_engineEnable)
        return;
    const bool on = m_engineEnable->isChecked();
    const int cycleSource = m_engineSource->currentData().toInt();
    const bool manual = cycleSource == 0;
    const QVector<double> key {
        on ? 1.0 : 0.0, double(m_engineRev), double(m_view->dataGeneration()),
        double(m_acquisitionMode),
        on && manual ? m_view->engineMarkerA() : 0.0,
        on && manual ? m_view->engineMarkerB() : 0.0
    };
    if (key == m_engineLastKey)
        return;
    m_engineLastKey = key;

    m_view->setEngineOverlay(on, manual);
    if (!on) {
        m_view->setEngineCycles({}, 720.0, 0.0);
        m_view->setCylinderBands({});
        m_view->setCompressionResult({}, m_compressionFlag->value(), QString());
        m_view->setCrankInfo({}, {}, {});
        return;
    }

    // Firing order: a bad entry is outlined red and treated as 1..n so the
    // overlay still shows something sensible meanwhile.
    const int cylinders = m_engineCylinders->value();
    std::vector<int> order;
    const bool orderOk = EngineTools::parseFiringOrder(
        m_engineFiringOrder->text().toStdString(), cylinders, &order);
    m_engineFiringOrder->setStyleSheet(orderOk
        ? QString() : QStringLiteral("QLineEdit { border: 1px solid rgb(255, 70, 70); }"));
    if (!orderOk) {
        order.clear();
        for (int c = 1; c <= cylinders; ++c)
            order.push_back(c);
    }
    const double cycleDeg = m_engineStroke->currentData().toDouble();
    const double rate = m_view->sampleRate();
    const double length = double(m_view->captureLength());
    m_view->setEngineTimingChart(m_engineStyle->currentData().toInt() == 1,
                                 QVector<int>(order.cbegin(), order.cend()));

    // Cycles: repeated from the markers, one per CH2 sync edge, or from the
    // crank and cam sensors.
    EngineTools::CrankAnalysis crank;
    EngineTools::CamAnalysis cam;
    std::vector<double> compressionPeaks;
    std::vector<double> starts;
    double offset = 0.0;
    QString problem;
    if (manual) {
        starts = EngineTools::manualCycles(m_view->engineMarkerA(), m_view->engineMarkerB(), length);
        if (starts.size() < 2)
            problem = QStringLiteral("Drag the 0\u00b0 marker onto cylinder 1 and the end marker one cycle later");
    } else if (cycleSource == 2) {
        // Crank + cam: the gap gives 0 degrees each revolution, the cam the
        // revolution each 720-degree cycle starts on. The cycle starts at the
        // gap, so its angle relative to cylinder 1 TDC is minus the TDC
        // setting.
        const int crankCh = m_crankChannel->currentData().toInt();
        const int camCh = m_camChannel->currentData().toInt();
        if (crankCh >= m_channelCount || m_channel[crankCh].mode->checkedId() == ChannelOff) {
            problem = QStringLiteral("Crank sensor: CH%1 is off").arg(crankCh + 1);
        } else {
            const QVector<double> cs = m_view->channelDisplaySamples(crankCh);
            const int pattern = m_crankPattern->currentData().toInt();
            crank = EngineTools::analyseCrank(std::vector<double>(cs.cbegin(), cs.cend()), rate,
                                              pattern / 100, pattern % 100);
            if (crank.tooFewSamples) {
                problem = QStringLiteral("Crank teeth too close together here \u2014 zoom in or "
                                         "capture at a higher sample rate");
            } else if (!crank.valid) {
                problem = QStringLiteral("No crank sensor signal on CH%1 \u2014 connect it, or set "
                                         "Cycle from to Markers").arg(crankCh + 1);
            } else {
                if (camCh >= 0 && camCh != crankCh && camCh < m_channelCount
                    && m_channel[camCh].mode->checkedId() != ChannelOff) {
                    const QVector<double> ms = m_view->channelDisplaySamples(camCh);
                    cam = EngineTools::analyseCam(std::vector<double>(ms.cbegin(), ms.cend()), crank);
                }
                if (cam.valid) {
                    starts = cam.cycleStarts;
                } else {
                    // No cam: every other revolution — right angle, phase a guess.
                    for (size_t k = 0; k < crank.refs.size(); k += 2)
                        starts.push_back(crank.refs[k]);
                }
                offset = -m_crankTdc->value();
                if (starts.size() < 2)
                    problem = QStringLiteral("Needs at least two full crank revolutions");
            }
        }
    } else if (m_channelCount < 2 || m_channel[1].mode->checkedId() == ChannelOff) {
        problem = QStringLiteral("Sync needs CH2 on");
    } else {
        const QVector<double> sync = m_view->channelDisplaySamples(1);
        const std::vector<double> x(sync.cbegin(), sync.cend());
        double lo = 0.0, hi = 0.0;
        if (!x.empty()) {
            const auto mm = std::minmax_element(x.cbegin(), x.cend());
            lo = *mm.first;
            hi = *mm.second;
        }
        starts = EngineTools::syncEdges(x, m_engineSyncThreshold->value(),
                                        m_engineSyncEdge->currentData().toBool(),
                                        0.1 * (hi - lo), size_t(std::max(1.0, rate * 0.002)));
        offset = m_engineSyncOffset->value();
        if (starts.size() < 2)
            problem = QStringLiteral("No sync: fewer than two CH2 edges cross %1 V")
                          .arg(m_engineSyncThreshold->value(), 0, 'f', 2);
    }
    std::vector<EngineTools::Band> bands =
        EngineTools::bandsFromCycles(starts, cycleDeg, offset, order);

    // Compression test: peaks assigned by counting from cylinder 1, so the
    // bands follow the real strokes even as cranking speed wobbles.
    QVector<double> percent;
    QString note;
    if (m_compressionEnable->isChecked()) {
        const int source = m_compressionChannel->currentData().toInt();
        if (m_acquisitionMode == ModeStream) {
            note = QStringLiteral("Runs on a Capture or a Recording");
        } else if (!problem.isEmpty()) {
            note = problem;
        } else if (source >= m_channelCount || m_channel[source].mode->checkedId() == ChannelOff) {
            note = QStringLiteral("CH%1 is off").arg(source + 1);
        } else {
            // Smoothed first: cranking ripple is well under 150 Hz, noise isn't.
            QVector<double> sig = ScopeView::lowPassFilter(
                m_view->channelDisplaySamples(source), rate, 150.0);
            std::vector<double> x(sig.cbegin(), sig.cend());
            if (m_compressionSignal->currentData().toInt() == 1) {
                for (double &v : x)
                    v = -v;   // battery voltage dips at each compression
            }
            std::vector<double> lengths;
            for (size_t k = 0; k + 1 < starts.size(); ++k)
                lengths.push_back(starts[k + 1] - starts[k]);
            std::nth_element(lengths.begin(), lengths.begin() + long(lengths.size() / 2), lengths.end());
            const double cycleLength = lengths[lengths.size() / 2];
            // Where cylinder 1's TDC falls in the first cycle.
            double rel = std::fmod(0.0 - offset, cycleDeg);
            if (rel < 0.0)
                rel += cycleDeg;
            const double cylinder1 = starts[0] + rel / cycleDeg * (starts[1] - starts[0]);

            const EngineTools::Compression r = EngineTools::relativeCompression(
                x, cycleLength / cylinders, cylinder1, order);
            for (const EngineTools::Band &b : r.bands)
                compressionPeaks.push_back(b.tdc);
            // With the crank sensor the angle comes from the crank itself;
            // otherwise the strokes define the cycles.
            if (!r.bands.empty() && cycleSource != 2) {
                bands = r.bands;
                starts = r.cycleStarts;
                offset = 0.0;
            }
            if (!r.percent.empty()) {
                percent = QVector<double>(r.percent.cbegin(), r.percent.cend());
                note = QStringLiteral("Average of %1 cycle%2").arg(r.cyclesUsed)
                           .arg(r.cyclesUsed == 1 ? QString() : QStringLiteral("s"));
                if (r.cyclesTotal > r.cyclesUsed)
                    note += QStringLiteral(", %1 left out").arg(r.cyclesTotal - r.cyclesUsed);
            } else if (r.cyclesTotal > 0) {
                note = QStringLiteral("No complete cycles \u2014 check the markers and signal");
            } else {
                note = QStringLiteral("No compression strokes found \u2014 check channel and signal type");
            }
        }
    } else if (!problem.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Engine overlay: %1").arg(problem), 3000);
    }

    // Crank and cam panel.
    QStringList crankLines;
    QVector<bool> crankWarn;
    QVector<double> crankFaults;
    m_lastCamAngle = -1.0;
    m_lastTdcCalibration = -1.0;
    if (cycleSource == 2) {
        if (!crank.valid) {
            crankLines << problem;
            crankWarn << true;
        } else {
            double lo = crank.rpm.front(), hi = crank.rpm.front(), sum = 0.0;
            for (double r : crank.rpm) {
                lo = std::min(lo, r);
                hi = std::max(hi, r);
                sum += r;
            }
            const double mean = sum / double(crank.rpm.size());
            QString line = QStringLiteral("%1-%2 wheel%3 \u00b7 %4 rpm")
                               .arg(crank.teeth).arg(crank.missing)
                               .arg(m_crankPattern->currentData().toInt() == 0 ? QStringLiteral(" (detected)") : QString())
                               .arg(mean, 0, 'f', 0);
            if (hi - lo > 0.02 * mean)
                line += QStringLiteral(" (%1\u2013%2)").arg(lo, 0, 'f', 0).arg(hi, 0, 'f', 0);
            crankLines << line;
            crankWarn << false;
            crankLines << (crank.faults.empty()
                ? QStringLiteral("No tooth faults")
                : QStringLiteral("%1 tooth fault%2, marked red").arg(crank.faults.size())
                      .arg(crank.faults.size() == 1 ? QString() : QStringLiteral("s")));
            crankWarn << !crank.faults.empty();
            for (double f : crank.faults)
                crankFaults.append(f);

            if (cam.valid) {
                m_lastCamAngle = cam.angle;
                const double tol = m_camTolerance->value();
                const auto deviation = [](double a, double b) {   // a - b, wrapped to +-180
                    return std::fmod(a - b + 540.0, 360.0) - 180.0;
                };
                QString camLine = QStringLiteral("Cam edge %1\u00b0 after the gap").arg(cam.angle, 0, 'f', 1);
                if (cam.spread > 1.0)
                    camLine += QStringLiteral(" (\u00b1%1\u00b0)").arg(cam.spread, 0, 'f', 1);
                crankLines << camLine;
                crankWarn << false;
                if (m_camSpec->value() >= 0.0) {
                    const double dev = deviation(cam.angle, m_camSpec->value());
                    crankLines << QStringLiteral("Spec %1\u00b0: %2%3\u00b0 %4")
                                      .arg(m_camSpec->value(), 0, 'f', 1)
                                      .arg(dev >= 0.0 ? QStringLiteral("+") : QString())
                                      .arg(dev, 0, 'f', 1)
                                      .arg(std::fabs(dev) <= tol ? QStringLiteral("OK")
                                                                 : QStringLiteral("OUT OF TOLERANCE"));
                    crankWarn << (std::fabs(dev) > tol);
                }
                if (m_camKnownGood >= 0.0) {
                    const double dev = deviation(cam.angle, m_camKnownGood);
                    crankLines << QStringLiteral("Known good %1\u00b0: %2%3\u00b0")
                                      .arg(m_camKnownGood, 0, 'f', 1)
                                      .arg(dev >= 0.0 ? QStringLiteral("+") : QString())
                                      .arg(dev, 0, 'f', 1);
                    crankWarn << (std::fabs(dev) > tol);
                }
            } else if (m_camChannel->currentData().toInt() >= 0) {
                crankLines << QStringLiteral("No cam reference edge found");
                crankWarn << true;
            } else {
                crankLines << QStringLiteral("No cam: which revolution is cylinder 1's is a guess");
                crankWarn << false;
            }

            if (!compressionPeaks.empty()) {
                const double cal = EngineTools::tdcFromCompression(
                    crank, compressionPeaks, cycleDeg / cylinders);
                if (!std::isnan(cal)) {
                    m_lastTdcCalibration = cal;
                    crankLines << QStringLiteral("Compression TDC at %1\u00b0 (mod %2\u00b0) \u2014 Calibrate")
                                      .arg(cal, 0, 'f', 1).arg(cycleDeg / cylinders, 0, 'f', 0);
                    crankWarn << false;
                }
            }
        }
    }
    m_view->setCrankInfo(crankLines, crankWarn, crankFaults);

    QVector<ScopeView::CylinderBand> viewBands;
    viewBands.reserve(int(bands.size()));
    for (const EngineTools::Band &b : bands) {
        ScopeView::CylinderBand v;
        v.start = b.start;
        v.end = b.end;
        v.tdc = b.tdc;
        v.cylinder = b.cylinder;
        v.excluded = b.excluded;
        viewBands.append(v);
    }
    m_view->setEngineCycles(QVector<double>(starts.cbegin(), starts.cend()), cycleDeg, offset);
    m_view->setCylinderBands(viewBands);
    m_view->setCompressionResult(percent, m_compressionFlag->value(), note);
}

// 12 V defaults, doubled for 24 V: charged from 12.4 V resting, at least
// 9.6 V while cranking, 13.5-14.8 V charging, ripple at most 0.5 Vpp.
void MainWindow::applyBatterySystemLimits()
{
    const double k = m_batterySystem->currentData().toDouble() / 12.0;
    m_limitRestingGood->setValue(12.4 * k);
    m_limitCrankMin->setValue(9.6 * k);
    m_limitChargeLow->setValue(13.5 * k);
    m_limitChargeHigh->setValue(14.8 * k);
    m_limitRipple->setValue(0.5 * k);
}

bool MainWindow::prepareTestChannel(int ch, bool ac, double page, double inputOffset, QString *error)
{
    if (ch >= m_channelCount) {
        *error = QStringLiteral("CH%1 isn't available on this scope").arg(ch + 1);
        return false;
    }
    ChannelControls &c = m_channel[ch];
    const int probeIndex = std::clamp(c.probe->currentIndex(), 0, std::max(0, m_probes->count() - 1));
    const Probe &spec = m_probes->at(probeIndex);
    const double gain = spec.gradient > 0.0 ? spec.gradient : 1.0;
    // The same mapping applyChannel() uses: a window the hardware can't
    // reach would be silently clamped, and the battery voltage clipped.
    const double needMv = (std::fabs(inputOffset - spec.offset) + page) * 1000.0 / gain;
    if (needMv > double(m_acq->rangeMaxMv()) + 0.5) {
        *error = QStringLiteral("CH%1's probe can't reach %2 V \u2014 use the x10 probe and "
                                "set CH%1's probe to x10").arg(ch + 1).arg(inputOffset + page, 0, 'f', 0);
        return false;
    }
    if (c.mode->checkedId() == ChannelOff) {
        if (auto *manual = c.mode->button(ChannelManual))
            manual->setChecked(true);
        emit c.mode->idClicked(ChannelManual);
    }
    c.invert->setChecked(false);
    c.offsetV->setValue(0.0);
    c.filterType->setCurrentIndex(0);
    c.coupling->setCurrentIndex(std::max(0, c.coupling->findData(ac ? 1 : 0)));
    c.inputOffset->setValue(ac ? 0.0 : inputOffset);
    const int pageIndex = c.voltsPage->findData(page);
    if (pageIndex >= 0)
        c.voltsPage->setCurrentIndex(pageIndex);
    return true;
}

namespace {
// Absolute volts from a channel's raw samples: the probe's own scaling only.
// Display values have the window centre (input offset) taken off, which
// for a 24 V test centred at 15 V would read 15 V low.
std::vector<double> absoluteVolts(const QVector<double> &raw, const Probe &probe)
{
    std::vector<double> v(size_t(raw.size()));
    for (int i = 0; i < raw.size(); ++i)
        v[size_t(i)] = raw[i] * probe.gradient + probe.offset;
    return v;
}
QString volts(double v) { return QStringLiteral("%1 V").arg(v, 0, 'f', 2); }
}

void MainWindow::startCrankingTest()
{
    const double system = m_batterySystem->currentData().toDouble();
    const QString title = QStringLiteral("Battery cranking test (%1 V)").arg(system, 0, 'f', 0);
    if (!m_acq->ready()) {
        m_view->setTestPanel(title, { QStringLiteral("Connect the scope first") }, { true });
        return;
    }
    // Cleared before stopping: stopAcquisition() queues the end of a waiting
    // cranking test, which would otherwise end this new test as it starts.
    m_guidedWaiting = false;
    if (m_running)
        stopAcquisition();
    if (m_acquisitionMode != ModeRecord) {
        // Through the mode selector too, as opening a file does: setting the
        // mode alone left the Scope page still showing the old mode.
        if (m_acqMode) {
            if (auto *button = m_acqMode->button(ModeRecord))
                button->setChecked(true);
        }
        setAcquisitionMode(ModeRecord);
    }
    // 12 V: +-20 V. 24 V: centred at 15 V, so -5..35 V.
    QString error;
    if (!prepareTestChannel(0, false, 20.0, system > 12.0 ? 15.0 : 0.0, &error)) {
        m_view->setTestPanel(title, { error }, { true });
        return;
    }
    // CH2: the current clamp (left as set up — its range depends on the
    // clamp), or off for a small recording and a simple setup.
    if (m_batteryClamp->isChecked()) {
        const Probe &clamp = m_probes->at(std::clamp(m_channel[1].probe->currentIndex(), 0,
                                                     std::max(0, m_probes->count() - 1)));
        if (m_channelCount < 2 || clamp.unit != QLatin1String("A")) {
            m_view->setTestPanel(title, { QStringLiteral(
                "Set CH2's probe to your current clamp (a probe in amps), or untick "
                "Current clamp") }, { true });
            return;
        }
        if (m_channel[1].mode->checkedId() == ChannelOff) {
            if (auto *manual = m_channel[1].mode->button(ChannelManual))
                manual->setChecked(true);
            emit m_channel[1].mode->idClicked(ChannelManual);
        }
        m_channel[1].coupling->setCurrentIndex(std::max(0, m_channel[1].coupling->findData(0)));
    } else if (m_channelCount > 1 && m_channel[1].mode->checkedId() != ChannelOff) {
        if (auto *off = m_channel[1].mode->button(ChannelOff))
            off->setChecked(true);
        emit m_channel[1].mode->idClicked(ChannelOff);
    }
    // Battery voltage moves on a millisecond scale; 100 kS/s is plenty.
    const int rateIndex = m_recordRate->findData(QVariant::fromValue(100000u));
    if (rateIndex >= 0)
        m_recordRate->setCurrentIndex(rateIndex);

    m_guidedTest = GuidedCranking;
    m_guidedWaiting = true;
    m_view->setTestPanel(title, { QStringLiteral("Recording 10 s \u2014 crank the engine now"),
                                  QStringLiteral("(stop early once it runs, if you like)") },
                         { false, false });
    startAcquisition();
    m_guidedStopTimer->start(10000);
}

void MainWindow::finishCrankingTest()
{
    if (!m_guidedWaiting || m_guidedTest != GuidedCranking)
        return;
    m_guidedWaiting = false;
    m_guidedStopTimer->stop();
    const double system = m_batterySystem->currentData().toDouble();
    const QString title = QStringLiteral("Battery cranking test (%1 V)").arg(system, 0, 'f', 0);
    const quint64 total = m_stream->recorded();
    const double rate = m_stream->sampleRate();
    if (total < 1000 || rate <= 0.0) {
        m_view->setTestPanel(title, { QStringLiteral("Nothing recorded to analyse") }, { true });
        return;
    }
    QVector<float> raw;
    m_stream->read(0, 0, int(std::min<quint64>(total, 50000000ull)), &raw);
    QVector<double> rawD(raw.size());
    for (int i = 0; i < raw.size(); ++i)
        rawD[i] = double(raw[i]);
    const ChannelControls &c = m_channel[0];
    const Probe &probe = m_probes->at(std::clamp(c.probe->currentIndex(), 0, std::max(0, m_probes->count() - 1)));
    const std::vector<double> volts = absoluteVolts(rawD, probe);
    const BatteryTools::Cranking r = BatteryTools::analyseCranking(volts, rate);
    // With the clamp: CH2's recording in amps, and the battery's internal
    // resistance from the two together.
    BatteryTools::Resistance resistance;
    const bool withClamp = m_batteryClamp->isChecked() && m_channelCount > 1;
    if (withClamp) {
        QVector<float> rawI;
        m_stream->read(1, 0, int(std::min<quint64>(total, 50000000ull)), &rawI);
        QVector<double> rawID(rawI.size());
        for (int i = 0; i < rawI.size(); ++i)
            rawID[i] = double(rawI[i]);
        const Probe &clamp = m_probes->at(std::clamp(m_channel[1].probe->currentIndex(), 0,
                                                     std::max(0, m_probes->count() - 1)));
        resistance = BatteryTools::internalResistance(volts, absoluteVolts(rawID, clamp), rate, r);
    }
    // Judged on the whole cranking curve (batterytools.h): state of charge,
    // the voltage held under load against a temperature-adjusted limit, the
    // engagement dip, sag, cranking speed (cylinders from the engine setup)
    // and whether it started — then one verdict.
    const BatteryTools::BatteryVerdict verdict = BatteryTools::judgeBattery(
        r, system, m_batteryTemperature->value(), m_engineCylinders->value(),
        m_limitRestingGood->value(), m_limitCrankMin->value(),
        m_limitChargeLow->value(), m_limitChargeHigh->value(),
        withClamp ? &resistance : nullptr, m_batteryRatedCca->value());
    QStringList lines;
    QVector<bool> warn;
    lines << QString::fromStdString(verdict.headline);
    warn << verdict.warn;
    for (const BatteryTools::VerdictLine &line : verdict.lines) {
        lines << QString::fromStdString(line.text);
        warn << line.warn;
    }
    m_view->setTestPanel(title, lines, warn);
}

void MainWindow::startChargingTest()
{
    const double system = m_batterySystem->currentData().toDouble();
    const QString title = QStringLiteral("Charging test (%1 V)").arg(system, 0, 'f', 0);
    if (!m_acq->ready()) {
        m_view->setTestPanel(title, { QStringLiteral("Connect the scope first") }, { true });
        return;
    }
    // Cleared before stopping: stopAcquisition() queues the end of a waiting
    // cranking test, which would otherwise end this new test as it starts.
    m_guidedWaiting = false;
    if (m_running)
        stopAcquisition();
    if (m_acquisitionMode != ModeCapture) {
        if (m_acqMode) {
            if (auto *button = m_acqMode->button(ModeCapture))
                button->setChecked(true);
        }
        setAcquisitionMode(ModeCapture);
    }
    // CH1 DC for the voltage (as in the cranking test); CH2 AC on a small
    // range for the ripple, which is a fraction of a volt riding on 14 V.
    QString error;
    if (!prepareTestChannel(0, false, 20.0, system > 12.0 ? 15.0 : 0.0, &error)
        || !prepareTestChannel(1, true, system > 12.0 ? 2.0 : 1.0, 0.0, &error)) {
        m_view->setTestPanel(title, { error }, { true });
        return;
    }
    // 5 ms/div: 50 ms, several dozen ripple humps at idle.
    for (int i = 0; i < m_timebase->count(); ++i) {
        if (std::fabs(m_timebase->itemData(i).toDouble() - 5e-3) < 1e-9) {
            m_timebase->setCurrentIndex(i);
            break;
        }
    }
    if (auto *automatic = m_trigModeGroup->button(ModeAuto)) {
        automatic->setChecked(true);
        emit m_trigModeGroup->idClicked(ModeAuto);
    }
    m_guidedTest = GuidedCharging;
    m_guidedWaiting = true;
    // CH2 on software AC (the hardware didn't switch): a first capture finds
    // the battery's DC level on CH1 so CH2's window can be centred there,
    // then a second measures the ripple. Hardware AC needs only one.
    m_chargePass = m_softwareAc[1] ? 0 : 1;
    m_view->setTestPanel(title, { QStringLiteral("Capturing\u2026") }, { false });
    // As the Single button does.
    m_suppressSamplesReady = false;
    m_acq->single();
    setRunning(true);
}

void MainWindow::finishChargingTest()
{
    if (!m_guidedWaiting || m_guidedTest != GuidedCharging)
        return;
    const double system = m_batterySystem->currentData().toDouble();
    const QString title = QStringLiteral("Charging test (%1 V)").arg(system, 0, 'f', 0);
    const double rate = m_view->sampleRate();
    const auto probeOf = [this](int ch) -> const Probe & {
        return m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                       std::max(0, m_probes->count() - 1)));
    };
    if (m_chargePass == 0) {
        const std::vector<double> first = absoluteVolts(m_view->channelSamples(0), probeOf(0));
        if (!first.empty()) {
            double mean = 0.0;
            for (double v : first)
                mean += v;
            m_softAcCentre[1] = mean / double(first.size());
            applyChannel(1);
        }
        m_chargePass = 1;
        m_view->setTestPanel(title, { QStringLiteral("Capturing the ripple\u2026") }, { false });
        m_suppressSamplesReady = false;
        m_acq->single();
        setRunning(true);
        return;   // still waiting, for the second capture
    }
    m_guidedWaiting = false;
    const std::vector<double> dc = absoluteVolts(m_view->channelSamples(0), probeOf(0));
    // A light filter first: ignition and injector switching put sharp
    // spikes on the supply that aren't ripple.
    const QVector<double> acFiltered = ScopeView::lowPassFilter(m_view->channelSamples(1), rate, 20000.0);
    const std::vector<double> ac = absoluteVolts(acFiltered, probeOf(1));

    QStringList lines;
    QVector<bool> warn;
    if (dc.empty() || ac.empty() || rate <= 0.0) {
        m_view->setTestPanel(title, { QStringLiteral("Nothing captured to analyse") }, { true });
        return;
    }
    double mean = 0.0;
    for (double v : dc)
        mean += v;
    mean /= double(dc.size());
    const bool low = mean < m_limitChargeLow->value();
    const bool high = mean > m_limitChargeHigh->value();
    lines << QStringLiteral("Charging %1 \u2014 %2").arg(volts(mean),
        low ? QStringLiteral("low (engine running? belt? regulator?)")
            : high ? QStringLiteral("high: regulator overcharging") : QStringLiteral("OK"));
    warn << (low || high);

    const BatteryTools::Ripple r = BatteryTools::analyseRipple(ac, rate);
    if (!r.valid) {
        lines << QStringLiteral("No alternator ripple found on CH2 (AC, on battery +?)");
        warn << true;
    } else {
        const bool rippleHigh = r.vpp > m_limitRipple->value();
        lines << QStringLiteral("Ripple %1 mV p-p \u2014 %2").arg(r.vpp * 1000.0, 0, 'f', 0)
                     .arg(rippleHigh ? QStringLiteral("high") : QStringLiteral("OK"));
        warn << rippleHigh;
        // A bad diode or stator phase leaves one hump in every 3 or 6 off
        // (found within runs of regular humps, so ignition / injector dips
        // don't hide it), or humps uneven by robust spread. The plain spreads
        // called a healthy alternator with ignition dips uneven (user's car at
        // idle: dips at the firing rate) and missed an open diode without
        // them. Simulated: healthy (with or without dips) robust 0.08-0.09 /
        // 0.01, no pattern; open diode pattern 0.62-0.64, weak phase 0.34-0.37.
        // Lines kept short: the panel cuts longer ones off.
        if (r.faultEvery > 0) {
            lines << QStringLiteral("Every %1 hump %2 % off: check diodes / stator")
                         .arg(r.faultEvery == 3 ? QStringLiteral("3rd") : QStringLiteral("6th"))
                         .arg(r.faultDepth * 100.0, 0, 'f', 0);
            warn << true;
        } else if (r.humpSpreadRobust > 0.30 || r.timingSpreadRobust > 0.15) {
            lines << QStringLiteral("Uneven ripple: check diodes / stator");
            warn << true;
        } else {
            lines << QStringLiteral("Ripple even: diodes look OK");
            warn << false;
        }
        lines << QStringLiteral("%1 ripple humps per second").arg(r.frequency, 0, 'f', 0);
        warn << false;
    }
    m_view->setTestPanel(title, lines, warn);
}

// ISO 11898-2 levels (5 V transceivers): recessive 2.0-3.0 V on both wires;
// dominant CAN-H 2.75-4.5 V, CAN-L 0.5-2.25 V; dominant difference
// 1.5-3.0 V, recessive difference -0.5..+0.05 V (+0.15 here, for probe and
// measurement tolerance). The difference is what the transceivers read, and
// what termination faults move.
void MainWindow::updateDiagSummary()
{
    if (!m_view || !m_diagSummaryEnable)
        return;
    if (!m_diagSummaryEnable->isChecked()) {
        if (!m_diagLastKey.isEmpty()) {
            m_view->setInfoPanel(3, QString(), {}, {});
            m_diagLastKey.clear();
        }
        return;
    }
    const QVector<double> key { double(m_diagRev), double(m_acquisitionMode) };
    if (key == m_diagLastKey)
        return;
    m_diagLastKey = key;
    QStringList lines;
    QVector<bool> warn;
    const auto add = [&](const QString &line, bool bad) {
        lines << line;
        warn << bad;
    };
    bool any = false;
    for (int slot = 0; slot < 2; ++slot) {
        const DecodeSlotUi &u = m_decodeUi[slot];
        if (!u.enable || !u.enable->isChecked() || u.protocol->currentData().toString() != QLatin1String("obd"))
            continue;
        any = true;
        const DiagData &d = m_diagData[size_t(slot)];
        if (!d.valid) {
            add(d.message.isEmpty() ? QStringLiteral("Nothing decoded yet") : d.message, false);
            continue;
        }
        const auto ecu = [](const Diag::Message &m) {
            return QString::fromStdString(Diag::idText(m.id, m.extended));
        };
        // Answer times, and what went wrong.
        int requests = 0, answered = 0, negative = 0, pending = 0;
        double sumMs = 0.0, maxMs = 0.0;
        QString lastNegative;
        for (size_t i = 0; i < d.messages.size(); ++i) {
            const Diag::Message &m = d.messages[i];
            if (m.request) {
                ++requests;
                for (size_t j = i + 1; j < d.messages.size(); ++j) {
                    if (Diag::answers(m, d.messages[j])) {
                        if (d.sampleRate > 0.0) {
                            const double ms = double(d.messages[j].start - m.end) / d.sampleRate * 1000.0;
                            sumMs += ms;
                            maxMs = std::max(maxMs, ms);
                        }
                        ++answered;
                        break;
                    }
                }
            }
            if (m.meaning.negative) {
                ++negative;
                lastNegative = QString::fromStdString(m.meaning.text);
            }
            pending += m.meaning.pending ? 1 : 0;
        }
        add(QStringLiteral("%1 messages \u00b7 %2 of %3 requests answered%4").arg(d.messages.size()).arg(answered).arg(requests)
                .arg(answered > 0 ? QStringLiteral(" \u00b7 response time %1 ms (max %2)")
                                        .arg(sumMs / answered, 0, 'f', 1).arg(maxMs, 0, 'f', 1)
                                  : QString()),
            false);
        if (negative > 0)
            add(QStringLiteral("%1 negative response%2 \u2014 last: %3").arg(negative).arg(negative == 1 ? QString() : QStringLiteral("s"))
                    .arg(lastNegative), true);
        if (pending > 0)
            add(QStringLiteral("%1 \u201cresponse pending\u201d (the ECU asked for more time)").arg(pending), false);
        // VIN, fault codes, live data: the latest of each.
        QString vin;
        QStringList dtcs;
        QMap<QString, QPair<QString, QString>> values;   // ECU + name -> (name, value)
        for (const Diag::Message &m : d.messages) {
            if (!m.meaning.vin.empty())
                vin = QString::fromStdString(m.meaning.vin);
            if (!m.request && !m.meaning.dtcs.empty()) {
                for (const std::string &code : m.meaning.dtcs) {
                    const QString line = ecu(m) + QStringLiteral("  ") + QString::fromStdString(code);
                    if (!dtcs.contains(line))
                        dtcs << line;
                }
            }
            for (const auto &[name, value] : m.meaning.values)
                values[ecu(m) + QLatin1Char('/') + QString::fromStdString(name)] =
                    { QString::fromStdString(name), QString::fromStdString(value) };
        }
        if (!vin.isEmpty())
            add(QStringLiteral("VIN %1").arg(vin), false);
        if (!dtcs.isEmpty()) {
            add(QStringLiteral("Fault codes (%1):").arg(dtcs.size()), true);
            for (const QString &line : dtcs)
                add(line, true);
        }
        if (!values.isEmpty()) {
            add(QStringLiteral("ECU\tValue\tLatest"), false);
            for (auto it = values.constBegin(); it != values.constEnd(); ++it)
                add(QStringLiteral("%1\t%2\t%3").arg(it.key().section(QLatin1Char('/'), 0, 0), it.value().first,
                                                     it.value().second), false);
        }
    }
    if (!any)
        add(QStringLiteral("Turn on an OBD-II / UDS decoder in Decoding"), false);
    m_view->setInfoPanel(3, QStringLiteral("OBD-II / UDS"), lines, warn);
}

void MainWindow::updateSerialHealth()
{
    VSX_TRACE("updateSerialHealth");
    if (!m_view || !m_serialHealthEnable)
        return;
    if (!m_serialHealthEnable->isChecked()) {
        if (!m_serialHealthLastKey.isEmpty()) {
            m_view->setInfoPanel(2, QString(), {}, {});
            m_serialHealthLastKey.clear();
        }
        return;
    }
    const QVector<double> key { double(m_view->dataGeneration()), double(m_serialHealthRev),
                                double(m_acquisitionMode) };
    if (key == m_serialHealthLastKey)
        return;
    m_serialHealthLastKey = key;

    QStringList lines;
    QVector<bool> warn;
    const auto add = [&](const QString &line, bool bad) {
        lines << line;
        warn << bad;
    };
    const auto plural = [](int n, const char *one, const char *many) {
        return QStringLiteral("%1 %2").arg(n).arg(QLatin1String(n == 1 ? one : many));
    };
    const auto hexBytes = [](const std::vector<std::uint8_t> &bytes) {
        QStringList out;
        for (std::uint8_t b : bytes) {
            if (out.size() == 8) {
                out << QStringLiteral("\u2026");
                break;
            }
            out << QStringLiteral("%1").arg(uint(b), 2, 16, QLatin1Char('0')).toUpper();
        }
        return out.isEmpty() ? QStringLiteral("\u2014") : out.join(QLatin1Char(' '));
    };
    QStringList kinds;
    for (int slot = 0; slot < 2; ++slot) {
        const DecodeSlotUi &u = m_decodeUi[slot];
        if (!u.enable || !u.enable->isChecked())
            continue;
        const QString proto = u.protocol->currentData().toString();
        if (proto != QLatin1String("lin") && proto != QLatin1String("kline") && proto != QLatin1String("sent"))
            continue;
        const bool lin = proto == QLatin1String("lin");
        const bool sentBus = proto == QLatin1String("sent");
        const QString name = lin ? QStringLiteral("LIN") : sentBus ? QStringLiteral("SENT") : QStringLiteral("K-Line");
        kinds << name;

        // The wire: recessive near the battery, dominant pulled low. Receivers
        // read dominant below 40 % of the supply (LIN) / 30 % (K-Line, ISO 9141).
        const int ch = u.input ? decodeInputChannel(u.input->currentData().toInt()) : -1;
        if (ch >= 0 && ch < std::min(m_channelCount, 4) && m_channel[ch].mode->checkedId() != ChannelOff) {
            const Probe &probe = m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                                         std::max(0, m_probes->count() - 1)));
            const SerialBus::WireLevels w =
                SerialBus::analyseWire(absoluteVolts(m_view->channelSamples(ch), probe));
            if (!w.active) {
                add(QStringLiteral("%1 (CH%2): no traffic (steady at %3)").arg(name).arg(ch + 1).arg(volts(w.idle)), true);
            } else if (sentBus) {
                // SENT transmitters: high at least 4.1 V, low at most 0.5 V.
                const bool ok = w.recessive >= 4.1 && w.dominant <= 0.5;
                add(QStringLiteral("SENT (CH%1): high %2, low %3 \u2014 %4").arg(ch + 1)
                        .arg(volts(w.recessive), volts(w.dominant))
                        .arg(ok ? QStringLiteral("OK") : QStringLiteral("out of spec")), !ok);
                if (w.recessive < 4.1)
                    add(QStringLiteral("High low: the 5 V supply or the pull-up?"), true);
                if (w.dominant > 0.5)
                    add(QStringLiteral("Low high: a ground offset or a weak driver?"), true);
            } else {
                const double ratio = w.recessive > 0.1 ? w.dominant / w.recessive : 1.0;
                const bool domOk = ratio <= (lin ? 0.4 : 0.3);
                const bool recOk = w.recessive >= 7.0;
                add(QStringLiteral("%1 (CH%2): recessive %3, dominant %4 (%5 %) \u2014 %6")
                        .arg(name).arg(ch + 1).arg(volts(w.recessive), volts(w.dominant))
                        .arg(ratio * 100.0, 0, 'f', 0)
                        .arg(domOk && recOk ? QStringLiteral("OK") : QStringLiteral("out of spec")),
                    !(domOk && recOk));
                if (!recOk)
                    add(QStringLiteral("Recessive low: a weak battery, or the bus's pull-up / supply"), true);
                else if (!domOk)
                    add(QStringLiteral("Dominant too high for receivers to read: a ground offset or a weak driver"), true);
                else if (ratio > 0.2)
                    add(QStringLiteral("Dominant on the high side: a ground offset or a weak driver?"), false);
            }
        }

        const SerialHealthData &d = m_serialHealth[size_t(slot)];
        if (!d.valid) {
            add(d.message.isEmpty() ? QStringLiteral("%1 decoder: nothing decoded yet").arg(name)
                                    : QStringLiteral("%1 decoder: %2").arg(name, d.message), false);
            continue;
        }
        const double seconds = d.sampleRate > 0.0 ? d.sampleCount / d.sampleRate : 0.0;
        const double nominal = double(d.baud);
        const auto every = [&](double period) {
            if (period <= 0.0 || d.sampleRate <= 0.0)
                return QStringLiteral("once");
            const double ms = period / d.sampleRate * 1000.0;
            return QStringLiteral("%1 ms").arg(ms, 0, 'f', ms < 10.0 ? 1 : 0);
        };
        if (sentBus) {
            double tickSum = 0.0, worstDrift = 0.0;
            int crc = 0, legacy = 0, nibble = 0, shortLow = 0, drift = 0, errors = 0, pauses = 0;
            int fc1Min = 1 << 30, fc1Max = -1, fc2Min = 1 << 30, fc2Max = -1;
            for (const Sent::Frame &f : d.sent) {
                tickSum += f.tick;
                worstDrift = std::max(worstDrift, std::fabs(f.tickDrift));
                crc += (!f.crcOk && !f.nibbleError) ? 1 : 0;     // a bad nibble isn't counted twice
                legacy += f.legacyCrc ? 1 : 0;
                nibble += f.nibbleError ? 1 : 0;
                shortLow += f.shortLow ? 1 : 0;
                drift += f.driftError ? 1 : 0;
                errors += f.error() ? 1 : 0;
                pauses += f.pauseTicks > 0.0 ? 1 : 0;
                if (!f.nibbleError && f.fastChannel1() >= 0) {
                    fc1Min = std::min(fc1Min, f.fastChannel1());
                    fc1Max = std::max(fc1Max, f.fastChannel1());
                    fc2Min = std::min(fc2Min, f.fastChannel2());
                    fc2Max = std::max(fc2Max, f.fastChannel2());
                }
            }
            const int count = int(d.sent.size());
            if (count > 0 && d.sampleRate > 0.0) {
                const double tickUs = tickSum / count / d.sampleRate * 1e6;
                const double dev = (tickUs - d.sentTickUs) / d.sentTickUs * 100.0;
                add(QStringLiteral("Tick %1 \u00b5s, %2 % from %3 \u00b5s (SENT allows 20 %)")
                        .arg(tickUs, 0, 'f', 3).arg(dev, 0, 'f', 1).arg(d.sentTickUs), std::fabs(dev) > 20.0);
                add(QStringLiteral("Largest tick change between frames: %1 % (1.56 % allowed)")
                        .arg(worstDrift * 100.0, 0, 'f', 2), worstDrift > 0.015625);
                double period = 0.0;
                if (count > 1)
                    period = double(d.sent.back().start - d.sent.front().start) / double(count - 1) / d.sampleRate;
                add(QStringLiteral("%1 in %2%3").arg(plural(count, "frame", "frames")).arg(formatSeconds(seconds))
                        .arg(period > 0.0 ? QStringLiteral(" \u00b7 one every %1").arg(formatSeconds(period)) : QString()),
                    false);
            } else {
                add(QStringLiteral("No frames found"), true);
            }
            QStringList kinds2;
            if (crc) kinds2 << plural(crc, "CRC", "CRCs");
            if (nibble) kinds2 << plural(nibble, "bad nibble", "bad nibbles");
            if (shortLow) kinds2 << plural(shortLow, "short low pulse", "short low pulses");
            if (drift) kinds2 << plural(drift, "tick jump", "tick jumps");
            add(errors > 0 ? QStringLiteral("%1 with errors: %2").arg(plural(errors, "frame", "frames"), kinds2.join(QStringLiteral(", ")))
                           : QStringLiteral("No errors"), errors > 0);
            if (legacy > 0)
                add(QStringLiteral("%1 the legacy (2008) CRC: an older sensor, not an error").arg(legacy == count ? QStringLiteral("All frames use")
                                                                                                    : plural(legacy, "frame uses", "frames use")), false);
            if (d.sentMode == 2) {
                int triggers = 0;
                double shortest = 1e9, longest = 0.0;
                for (const Sent::Frame &f : d.sent) {
                    if (f.triggerTicks > 0.0) {
                        ++triggers;
                        shortest = std::min(shortest, f.triggerTicks);
                        longest = std::max(longest, f.triggerTicks);
                    }
                }
                add(triggers > 0 ? QStringLiteral("Master triggers: %1, %2 \u2013 %3 ticks low").arg(triggers)
                                       .arg(shortest, 0, 'f', 1).arg(longest, 0, 'f', 1)
                                 : QStringLiteral("No master trigger seen (not SPC?)"), triggers == 0);
            } else {
                add(pauses > 0 ? QStringLiteral("Pause pulses: yes (constant frame length)") : QStringLiteral("Pause pulses: none seen"), false);
            }
            if (d.sentMode == 1) {
                int shorts = 0, enhanced = 0, bad = 0, unverified = 0;
                for (const Sent::SerialMessage &m : d.sentMessages) {
                    (m.enhanced ? enhanced : shorts) += 1;
                    bad += m.error() ? 1 : 0;
                    unverified += (!m.crcOk && !m.crcVerified) ? 1 : 0;
                }
                add(QStringLiteral("Serial messages: %1 short, %2 enhanced%3").arg(shorts).arg(enhanced)
                        .arg(bad ? QStringLiteral(" \u00b7 %1").arg(plural(bad, "CRC error", "CRC errors")) : QString()),
                    bad > 0);
                if (unverified > 0)
                    add(QStringLiteral("%1 with an enhanced CRC not verified (its bit order isn't confirmed here)")
                            .arg(plural(unverified, "message", "messages")), false);
                if (shorts + enhanced == 0)
                    add(QStringLiteral("No serial messages yet: they need 16-18 frames in view"), false);
                const std::vector<SerialBus::Row> rows = SerialBus::table(
                    d.sentMessages,
                    [](const Sent::SerialMessage &m) { return (m.enhanced ? 1 << 16 : 0) | m.id; },
                    [](const Sent::SerialMessage &m) {
                        std::vector<std::uint8_t> v;           // most significant byte first
                        for (int i = (m.dataBits + 7) / 8 - 1; i >= 0; --i)
                            v.push_back(std::uint8_t((m.data >> (8 * i)) & 0xFF));
                        return v;
                    });
                if (!rows.empty()) {
                    add(QStringLiteral("Message\tCount\tEvery\tData"), false);
                    for (const SerialBus::Row &r : rows)
                        add(QStringLiteral("%1 %2\t%3\t%4\t%5").arg((r.key >> 16) ? QStringLiteral("E") : QStringLiteral("S"))
                                .arg(QString::number(r.key & 0xFFFF, 16).toUpper()).arg(r.count).arg(every(r.period))
                                .arg(hexBytes(r.lastData)), r.errors > 0);
                }
            }
            if (fc1Max >= 0)
                add(QStringLiteral("FC1 %1 \u2013 %2 \u00b7 FC2 %3 \u2013 %4 (12-bit fast channels)")
                        .arg(fc1Min).arg(fc1Max).arg(fc2Min).arg(fc2Max), false);
            continue;
        }
        if (lin) {
            // Bit rate from each sync byte, as LIN slaves themselves measure it.
            double sum = 0.0, worst = 0.0;
            int measured = 0;
            for (const SerialBus::LinFrame &f : d.lin) {
                if (f.bitTime > 0.0 && nominal > 0.0) {
                    const double dev = (d.sampleRate / f.bitTime - nominal) / nominal * 100.0;
                    sum += dev;
                    worst = std::max(worst, std::fabs(dev));
                    ++measured;
                }
            }
            if (measured > 0)
                add(QStringLiteral("Bit rate (sync bytes): %1 bit/s, %2 % (worst %3 %)")
                        .arg(qlonglong(std::llround(nominal * (1.0 + sum / measured / 100.0))))
                        .arg(sum / measured, 0, 'f', 2).arg(worst, 0, 'f', 2),
                    worst > 1.5);
            double busy = 0.0;
            int parity = 0, checksum = 0, otherType = 0, framing = 0, sync = 0, shortBreak = 0, noResponse = 0, errors = 0;
            for (const SerialBus::LinFrame &f : d.lin) {
                busy += double(f.end - f.start);
                parity += f.parityOk ? 0 : 1;
                checksum += (!f.checksumOk && !f.otherTypeMatches) ? 1 : 0;
                otherType += f.otherTypeMatches ? 1 : 0;
                framing += f.framingError ? 1 : 0;
                sync += f.syncOk ? 0 : 1;
                shortBreak += f.breakBits < 12.5 ? 1 : 0;
                noResponse += f.noResponse ? 1 : 0;
                errors += f.error() ? 1 : 0;
            }
            const double load = d.sampleCount > 0.0 ? busy / d.sampleCount : 0.0;
            add(QStringLiteral("%1 in %2 \u00b7 bus load %3 %").arg(plural(int(d.lin.size()), "frame", "frames"))
                    .arg(formatSeconds(seconds)).arg(load * 100.0, 0, 'f', 1), false);
            QStringList kinds2;
            if (parity) kinds2 << plural(parity, "ID parity", "ID parity");
            if (checksum) kinds2 << plural(checksum, "checksum", "checksums");
            if (framing) kinds2 << plural(framing, "stop bit", "stop bits");
            if (sync) kinds2 << plural(sync, "sync", "syncs");
            add(errors > 0 ? QStringLiteral("%1 with errors: %2").arg(plural(errors, "frame", "frames"), kinds2.join(QStringLiteral(", ")))
                           : QStringLiteral("No errors"), errors > 0);
            if (otherType > 0)
                add(QStringLiteral("%1 match the other checksum type: set LIN version to %2 in Decoding")
                        .arg(plural(otherType, "checksum", "checksums"))
                        .arg(d.linVersion >= 2 ? QStringLiteral("1.x (classic)") : QStringLiteral("2.x (enhanced)")), true);
            if (shortBreak > 0)
                add(QStringLiteral("%1 shorter than 13 bits").arg(plural(shortBreak, "break", "breaks")), true);
            if (noResponse > 0)
                add(QStringLiteral("%1 without a response (normal for unused IDs)").arg(plural(noResponse, "header", "headers")), false);
            const std::vector<SerialBus::Row> rows = SerialBus::table(
                d.lin, [](const SerialBus::LinFrame &f) { return f.id; },
                [](const SerialBus::LinFrame &f) { return f.data; });
            if (!rows.empty()) {
                add(QStringLiteral("ID\tCount\tEvery\tData"), false);
                for (const SerialBus::Row &r : rows)
                    add(QStringLiteral("%1\t%2\t%3\t%4")
                            .arg(r.key < 0 ? QStringLiteral("?") : QStringLiteral("%1").arg(r.key, 2, 16, QLatin1Char('0')).toUpper())
                            .arg(r.count).arg(every(r.period)).arg(hexBytes(r.lastData)),
                        r.errors > 0);
            }
        } else {
            if (d.bitTime > 0.0 && nominal > 0.0) {
                const double measuredBaud = d.sampleRate / d.bitTime;
                const double dev = (measuredBaud - nominal) / nominal * 100.0;
                add(QStringLiteral("Baud rate: %1, %2 %").arg(qlonglong(std::llround(measuredBaud))).arg(dev, 0, 'f', 2),
                    std::fabs(dev) > 2.0);
            }
            double busy = 0.0;
            int checksum = 0, framing = 0, length = 0, errors = 0;
            for (const SerialBus::KLineMessage &m : d.kline.messages) {
                busy += double(m.end - m.start);
                checksum += m.checksumOk ? 0 : 1;
                framing += m.framingError ? 1 : 0;
                length += m.lengthOk ? 0 : 1;
                errors += m.error() ? 1 : 0;
            }
            const double load = d.sampleCount > 0.0 ? busy / d.sampleCount : 0.0;
            add(QStringLiteral("%1 in %2 \u00b7 bus load %3 %").arg(plural(int(d.kline.messages.size()), "message", "messages"))
                    .arg(formatSeconds(seconds)).arg(load * 100.0, 0, 'f', 1), false);
            QStringList kinds2;
            if (checksum) kinds2 << plural(checksum, "checksum", "checksums");
            if (framing) kinds2 << plural(framing, "stop bit", "stop bits");
            if (length) kinds2 << plural(length, "short message", "short messages");
            add(errors > 0 ? QStringLiteral("%1 with errors: %2").arg(plural(errors, "message", "messages"), kinds2.join(QStringLiteral(", ")))
                           : QStringLiteral("No errors"), errors > 0);
            if (d.sampleRate > 0.0 && !d.kline.messages.empty()) {
                const double ms = d.kline.maxInterByte / d.sampleRate * 1000.0;
                add(QStringLiteral("Longest gap between bytes: %1 ms (KWP2000 allows 20)").arg(ms, 0, 'f', 1), ms > 20.0);
            }
            if (d.kline.inits > 0)
                add(QStringLiteral("%1 (low for 11+ bit times)").arg(plural(d.kline.inits, "wake-up / init pulse", "wake-up / init pulses")), false);
            const std::vector<SerialBus::Row> rows = SerialBus::table(
                d.kline.messages,
                [](const SerialBus::KLineMessage &m) {
                    if (m.target >= 0)
                        return m.target << 8 | m.source;
                    int key = 0;
                    for (size_t i = 0; i < 3 && i < m.bytes.size(); ++i)
                        key = key << 8 | m.bytes[i];
                    return key | 1 << 30;                   // headerless: the first bytes
                },
                [](const SerialBus::KLineMessage &m) { return m.data; });
            if (!rows.empty()) {
                add(QStringLiteral("From \u2192 to\tCount\tEvery\tData"), false);
                for (const SerialBus::Row &r : rows) {
                    QString label;
                    if (r.key & (1 << 30))
                        label = QStringLiteral("%1").arg(r.key & 0xFFFFFF, 6, 16, QLatin1Char('0')).toUpper();
                    else
                        label = QStringLiteral("%1 \u2192 %2").arg(r.key & 0xFF, 2, 16, QLatin1Char('0'))
                                    .arg((r.key >> 8) & 0xFF, 2, 16, QLatin1Char('0')).toUpper();
                    add(QStringLiteral("%1\t%2\t%3\t%4").arg(label).arg(r.count).arg(every(r.period)).arg(hexBytes(r.lastData)),
                        r.errors > 0);
                }
            }
        }
    }
    if (kinds.isEmpty())
        add(QStringLiteral("Turn on a LIN, K-Line or SENT decoder in Decoding"), false);
    m_view->setInfoPanel(2, kinds.isEmpty() ? QStringLiteral("LIN / K-Line / SENT health")
                                            : kinds.join(QStringLiteral(" / ")) + QStringLiteral(" health"),
                         lines, warn);
}

void MainWindow::updateCanHealth()
{
    VSX_TRACE("updateCanHealth");
    if (!m_view || !m_canHealthEnable)
        return;
    if (!m_canHealthEnable->isChecked()) {
        if (!m_canHealthLastKey.isEmpty()) {
            m_view->setInfoPanel(1, QString(), {}, {});
            m_canHealthLastKey.clear();
        }
        return;
    }
    const QVector<double> key { double(m_view->dataGeneration()), double(m_canHealthRev),
                                double(m_acquisitionMode) };
    if (key == m_canHealthLastKey)
        return;
    m_canHealthLastKey = key;

    QStringList lines;
    QVector<bool> warn;
    const auto add = [&](const QString &line, bool bad) {
        lines << line;
        warn << bad;
    };
    const auto differenceHint = [&](double dominant) {
        if (dominant > 3.0)
            add(QStringLiteral("Difference high: a terminating resistor may be missing "
                               "(expect 60 \u03a9 between CAN-H and CAN-L, ignition off)"), true);
        else if (dominant < 1.5)
            add(QStringLiteral("Difference low: extra load \u2014 a short, extra termination "
                               "or a failing node"), true);
    };

    // --- Levels, per channel that's on; each recognised from its own signal.
    std::vector<double> highV, lowV;
    int highCh = -1, lowCh = -1;
    for (int ch = 0; ch < std::min(m_channelCount, 2); ++ch) {
        if (m_channel[ch].mode->checkedId() == ChannelOff)
            continue;
        const Probe &probe = m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                                     std::max(0, m_probes->count() - 1)));
        const std::vector<double> v = absoluteVolts(m_view->channelSamples(ch), probe);
        const CanTools::WireLevels w = CanTools::analyseWire(v);
        if (!w.active) {
            add(QStringLiteral("CH%1: no CAN traffic (steady at %2)").arg(ch + 1).arg(volts(w.idle)), true);
            continue;
        }
        if (w.kind == 2) {
            const bool ok = w.dominant >= 1.5 && w.dominant <= 3.0
                            && w.recessive >= -0.5 && w.recessive <= 0.15;
            add(QStringLiteral("CH%1 differential: recessive %2, dominant %3 \u2014 %4")
                    .arg(ch + 1).arg(volts(w.recessive), volts(w.dominant),
                                     ok ? QStringLiteral("OK") : QStringLiteral("out of spec")), !ok);
            differenceHint(w.dominant);
            continue;
        }
        const bool isHigh = w.kind == 1;
        const QString name = isHigh ? QStringLiteral("CAN-H") : QStringLiteral("CAN-L");
        const bool recOk = w.recessive >= 2.0 && w.recessive <= 3.0;
        const bool domOk = isHigh ? (w.dominant >= 2.75 && w.dominant <= 4.5)
                                  : (w.dominant >= 0.5 && w.dominant <= 2.25);
        add(QStringLiteral("%1 (CH%2): recessive %3, dominant %4 \u2014 %5")
                .arg(name).arg(ch + 1).arg(volts(w.recessive), volts(w.dominant),
                                           recOk && domOk ? QStringLiteral("OK") : QStringLiteral("out of spec")),
            !(recOk && domOk));
        if (!recOk) {
            add(QStringLiteral("%1 doesn't rest near 2.5 V \u2014 shorted to %2?")
                    .arg(name, w.recessive < 2.0 ? QStringLiteral("ground") : QStringLiteral("battery")), true);
        }
        if (isHigh && highCh < 0) {
            highCh = ch;
            highV = v;
        } else if (!isHigh && lowCh < 0) {
            lowCh = ch;
            lowV = v;
        }
    }
    if (highCh >= 0 && lowCh >= 0 && highV.size() == lowV.size()) {
        std::vector<double> d(highV.size());
        for (size_t i = 0; i < d.size(); ++i)
            d[i] = highV[i] - lowV[i];
        const CanTools::WireLevels w = CanTools::analyseWire(d);
        if (w.active) {
            const bool ok = w.dominant >= 1.5 && w.dominant <= 3.0
                            && w.recessive >= -0.5 && w.recessive <= 0.15;
            add(QStringLiteral("Difference: recessive %1, dominant %2 \u2014 %3")
                    .arg(volts(w.recessive), volts(w.dominant),
                         ok ? QStringLiteral("OK") : QStringLiteral("out of spec")), !ok);
            differenceHint(w.dominant);
        }
    }
    if (lines.isEmpty())
        add(QStringLiteral("Turn on CH1 and/or CH2 on CAN-H / CAN-L"), true);

    // --- Load, errors and frames, from the CAN decoder.
    int canSlot = -1;
    for (int slot = 0; slot < 2 && canSlot < 0; ++slot) {
        const DecodeSlotUi &u = m_decodeUi[slot];
        if (u.enable && u.enable->isChecked()
            && u.protocol->currentData().toString() == QLatin1String("can"))
            canSlot = slot;
    }
    if (canSlot < 0) {
        add(QStringLiteral("Turn on a CAN decoder in Decoding for load, errors and frames"), false);
    } else if (!m_canDecoded) {
        add(m_canMessage.isEmpty() ? QStringLiteral("CAN decoder: nothing decoded yet")
                                   : QStringLiteral("CAN decoder: %1").arg(m_canMessage), false);
    } else {
        const double load = CanTools::busLoad(m_canFrames, m_canSampleCount);
        const double seconds = m_canSampleRate > 0.0 ? m_canSampleCount / m_canSampleRate : 0.0;
        int errors = m_canStrayErrors;
        for (const CanTools::CanFrame &f : m_canFrames)
            errors += f.error ? 1 : 0;
        // formatSeconds(): a window under a millisecond read "0.000 s".
        add(QStringLiteral("%1 frame%2 in %3 \u00b7 bus load %4 %")
                .arg(qulonglong(m_canFrames.size())).arg(m_canFrames.size() == 1 ? QString() : QStringLiteral("s"))
                .arg(formatSeconds(seconds)).arg(load * 100.0, 0, 'f', 1),
            load > 0.8);
        add(errors > 0 ? QStringLiteral("%1 error%2").arg(errors).arg(errors == 1 ? QString() : QStringLiteral("s"))
                       : QStringLiteral("No errors"),
            errors > 0);
        const std::vector<CanTools::IdStats> rows = CanTools::frameTable(m_canFrames);
        if (!rows.empty()) {
            add(QStringLiteral("ID\tCount\tEvery\tData"), false);
            for (const CanTools::IdStats &r : rows) {
                const QString id = QStringLiteral("%1").arg(r.id, r.extended ? 8 : 3, 16, QLatin1Char('0')).toUpper();
                QString every = QStringLiteral("once");
                if (r.period > 0.0 && m_canSampleRate > 0.0) {
                    const double ms = r.period / m_canSampleRate * 1000.0;
                    every = QStringLiteral("%1 ms").arg(ms, 0, 'f', ms < 10.0 ? 1 : 0);
                }
                QStringList bytes;
                for (uint8_t b : r.lastData) {
                    if (bytes.size() == 8) {            // a 64-byte FD frame made the panel wider than the plot
                        bytes << QStringLiteral("\u2026");
                        break;
                    }
                    bytes << QStringLiteral("%1").arg(uint(b), 2, 16, QLatin1Char('0')).toUpper();
                }
                add(QStringLiteral("%1\t%2\t%3\t%4").arg(id).arg(r.count).arg(every)
                        .arg(bytes.isEmpty() ? QStringLiteral("\u2014") : bytes.join(QLatin1Char(' '))),
                    r.errors > 0);
            }
        }
    }
    m_view->setInfoPanel(1, QStringLiteral("CAN bus health"), lines, warn);
}

void MainWindow::trackSoftwareAc(int ch, const QVector<double> &raw)
{
    if (raw.isEmpty() || ch < 0 || ch >= 4)
        return;
    const ChannelControls &c = m_channel[ch];
    const Probe &probe = m_probes->at(std::clamp(c.probe->currentIndex(), 0,
                                                 std::max(0, m_probes->count() - 1)));
    const double gain = probe.gradient != 0.0 ? probe.gradient : 1.0;
    double mn = raw[0], mx = raw[0], sum = 0.0;
    for (double r : raw) {
        mn = std::min(mn, r);
        mx = std::max(mx, r);
        sum += r;
    }
    const double mean = sum / double(raw.size()) * gain + probe.offset;
    double lo = mn * gain + probe.offset, hi = mx * gain + probe.offset;
    if (lo > hi)
        std::swap(lo, hi);
    const double page = c.voltsPage->currentData().toDouble();
    const double centre = m_softAcCentre[ch];
    const double fullHalf = double(m_acq->rangeMaxMv()) * gain / 1000.0;   // probe's whole reach
    const double reach = std::max(0.0, fullHalf - page);   // furthest the narrow window can centre

    // Stepping the narrow window toward a clipped signal can bounce forever
    // when the DC sits on a window edge (simulated: 13 V on a +-1 V page,
    // with two-page steps). Instead, a clipped signal opens the window to
    // the probe's whole range for one frame — that frame's average is the
    // true DC level — and the narrow window then centres on it: two frames,
    // whatever the level.
    if (m_softAcSeeking[ch]) {
        m_softAcSeeking[ch] = false;
        double level = mean;
        if (hi >= probe.offset + 0.97 * fullHalf || lo <= probe.offset - 0.97 * fullHalf
            || std::fabs(level - probe.offset) > reach) {
            level = std::clamp(level, probe.offset - reach, probe.offset + reach);
            if (!m_softAcUnreachable[ch]) {
                m_softAcUnreachable[ch] = true;
                statusBar()->showMessage(QStringLiteral(
                    "CH%1: the DC level is beyond this probe's range for software AC "
                    "\u2014 use the x10 probe and set CH%1's probe to x10").arg(ch + 1), 8000);
            }
        }
        m_softAcCentre[ch] = level;
        applyChannel(ch);
        return;
    }
    const bool clipped = hi >= centre + 0.97 * page || lo <= centre - 0.97 * page;
    if (clipped) {
        m_softAcSeeking[ch] = true;
        applyChannel(ch);
        return;
    }
    // Inside the window: follow slow drift (charging voltage rising, say).
    if (std::fabs(mean - centre) <= 0.2 * page)
        return;
    m_softAcCentre[ch] = std::clamp(mean, probe.offset - reach, probe.offset + reach);
    applyChannel(ch);
}

// Block averages first (an average is itself a low-pass), down to at least
// 8x the cutoff, then the same zero-phase filter at that reduced rate.
// Checked against filtering at the full rate (1 MS/s, 100 Hz cutoff, 1250x
// reduction): within ~1 % of the signal (1.5 % worst) — indistinguishable on
// screen. Reads the recording once per recording / channel / cutoff; pans
// and zooms then cost nothing.
bool MainWindow::ensureFilteredOverview(int ch, int type, double lowHz, double highHz)
{
    if (ch < 0 || ch >= 4 || m_stream->recording() || m_currentRecordTab.isEmpty()
        || type == ScopeView::FilterOff)
        return false;
    const quint64 total = m_stream->recorded();
    const double rate = m_stream->sampleRate();
    // The frequency the reduced rate must stay 8x above: the low-pass /
    // band-pass upper edge, or the high-pass cutoff (whose removed slow part
    // is what's kept here).
    const double cutoff = type == ScopeView::FilterHighPass ? lowHz : std::max(lowHz, highHz);
    // High-pass and band-stop keep fast content a reduced-rate copy can't
    // hold: those take the exact two-pass route below.
    const bool keepsFast = type == ScopeView::FilterHighPass || type == ScopeView::FilterBandStop;
    if (total < 2 || rate <= 0.0 || cutoff <= 0.0)
        return false;
    FilteredOverview &ov = m_filteredOverview[size_t(ch)];
    if (ov.directory == m_currentRecordTab && ov.type == type && qFuzzyCompare(ov.lowHz, lowHz)
        && qFuzzyCompare(ov.highHz, highHz) && ov.total == total && !ov.data.isEmpty())
        return true;

    constexpr quint64 kMaxPoints = 8ull * 1024 * 1024;
    const int forCutoff = std::max(1, int(rate / (8.0 * cutoff)));
    const int forMemory = int((total + kMaxPoints - 1) / kMaxPoints);
    const int factor = std::max(forCutoff, forMemory);
    const quint64 points = total / quint64(factor);
    if (points < 2)
        return false;

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QVector<double> reduced(static_cast<int>(points));   // not reduced(int(points)): that parses as a function declaration
    // Whole blocks per read, about 4M raw samples at a time.
    const quint64 blocksPerRead = std::max<quint64>(1, (4ull * 1024 * 1024) / quint64(factor));
    QVector<float> raw;
    quint64 done = 0;
    while (done < points) {
        const quint64 blocks = std::min(blocksPerRead, points - done);
        const int got = m_stream->read(ch, done * quint64(factor), int(blocks * quint64(factor)), &raw);
        if (got <= 0)
            break;
        const quint64 full = quint64(got) / quint64(factor);
        for (quint64 b = 0; b < full; ++b) {
            double sum = 0.0;
            const float *p = raw.constData() + b * quint64(factor);
            for (int j = 0; j < factor; ++j)
                sum += double(p[j]);
            reduced[int(done + b)] = sum / factor;
        }
        done += full;
        if (full < blocks)
            break;
    }
    reduced.resize(int(done));
    if (!keepsFast) {
        reduced = ScopeView::applyFilter(reduced, rate / factor, type, lowHz, highHz);
        QApplication::restoreOverrideCursor();
        if (reduced.size() < 2)
            return false;
        ov.data.resize(reduced.size());
        for (int i = 0; i < reduced.size(); ++i)
            ov.data[i] = float(reduced[i]);
        ov.points = reduced.size();
    } else {
        // High-pass keeps the fast content a reduced-rate copy can't hold.
        // A second read subtracts the slow part (found at the reduced rate,
        // nothing in it faster than the cutoff, so interpolating it to every
        // raw sample is accurate) and keeps the exact output's min/max per
        // overview point. Checked against the full-rate high-pass: 0.3 mV
        // worst column error on a 0.6 V result (one level per column instead
        // was 180 mV off: the slow part drifts within a column).
        const QVector<double> base = ScopeView::removedPart(reduced, rate / factor, type, lowHz, highHz);
        const int count = base.size();
        if (count < 2) {
            QApplication::restoreOverrideCursor();
            return false;
        }
        ov.data.resize(2 * count);
        quint64 point = 0;
        while (point < quint64(count)) {
            const quint64 blocks = std::min(blocksPerRead, quint64(count) - point);
            const int got = m_stream->read(ch, point * quint64(factor), int(blocks * quint64(factor)), &raw);
            if (got <= 0)
                break;
            const quint64 full = quint64(got) / quint64(factor);
            for (quint64 b = 0; b < full; ++b) {
                const quint64 k = point + b;
                float mn = 0.0f, mx = 0.0f;
                for (int j = 0; j < factor; ++j) {
                    const double pos = (double(j) + 0.5) / factor - 0.5 + double(k);
                    const int k0 = std::clamp(int(std::floor(pos)), 0, count - 1);
                    const int k1 = std::min(count - 1, k0 + 1);
                    const double f = std::clamp(pos - k0, 0.0, 1.0);
                    const float h = float(double(raw[int(b * quint64(factor)) + j])
                                          - (base[k0] * (1.0 - f) + base[k1] * f));
                    if (j == 0 || h < mn) mn = h;
                    if (j == 0 || h > mx) mx = h;
                }
                ov.data[int(2 * k)] = mn;
                ov.data[int(2 * k + 1)] = mx;
            }
            point += full;
            if (full < blocks)
                break;
        }
        QApplication::restoreOverrideCursor();
        ov.points = int(point);
        if (ov.points < 2)
            return false;
    }

    ov.directory = m_currentRecordTab;
    ov.type = type;
    ov.lowHz = lowHz;
    ov.highHz = highHz;
    ov.total = total;
    ov.factor = factor;
    return true;
}

void MainWindow::setFullView(bool full)
{
    if (full == m_fullView)
        return;

    // Everything around the plot. The plot splitter itself (and with it a
    // Spectrum/XY pane, if one is open) is what stays.
    QWidget *chrome[6] = { m_topBar, m_channelRail, m_panelHost,
                           m_recordTabs, m_nav, statusBar() };

    if (full) {
        m_fullWasFullScreen = isFullScreen();
        m_fullWasMaximized  = isMaximized();
        for (int i = 0; i < 6; ++i) {
            m_fullWasVisible[i] = chrome[i] && chrome[i]->isVisible();
            if (chrome[i])
                chrome[i]->setVisible(false);
        }
        if (m_rootLayout) {
            m_fullRootMargins = m_rootLayout->contentsMargins();
            m_rootLayout->setContentsMargins(0, 0, 0, 0);
        }

        if (!m_fullEscShortcut) {
            m_fullEscShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
            connect(m_fullEscShortcut, &QShortcut::activated, this,
                    [this] { setFullView(false); });
        }
        // Only while full, so Esc keeps meaning whatever it means elsewhere.
        m_fullEscShortcut->setEnabled(true);

        m_fullView = true;
        showFullScreen();
        return;
    }

    m_fullView = false;
    if (m_fullEscShortcut)
        m_fullEscShortcut->setEnabled(false);
    for (int i = 0; i < 6; ++i) {
        if (chrome[i])
            chrome[i]->setVisible(m_fullWasVisible[i]);
    }
    if (m_rootLayout)
        m_rootLayout->setContentsMargins(m_fullRootMargins);
    if (!m_fullWasFullScreen) {
        if (m_fullWasMaximized)
            showMaximized();
        else
            showNormal();
    }
}

// The same values restoreSettings() falls back to on a first run, applied
// through the widgets with their signals firing, so each handler reconfigures
// the hardware exactly as if the person had set them by hand. Deliberately
// left alone: reference waveforms, annotations, recordings, the probe
// library, theme and record directory — those are the person's data and
// preferences, not the instrument setup.
void MainWindow::resetToDefaults()
{
    if (!VsxDialogs::question(this, QStringLiteral("Default setup"),
            QStringLiteral("Reset timebase, channels, trigger, Math and zoom to "
                           "their defaults?\n\nReference waveforms, annotations "
                           "and recordings are kept.")))
        return;

    stopAcquisition();

    if (m_instrumentMode) {
        if (auto *b = m_instrumentMode->button(0))
            b->setChecked(true);
    }
    setInstrumentMode(InstrumentMode::Scope);
    m_view->setRulersEnabled(false);

    // Rates, depth, record options.
    m_liveRate->setCurrentIndex(kDefaultSampleRateIndex);
    m_recordRate->setCurrentIndex(kDefaultSampleRateIndex);
    m_recordSize->setCurrentIndex(1);
    m_depth->setCurrentIndex(2);   // 64 KB, as built
    if (m_recordPersistGroup) {
        if (auto *b = m_recordPersistGroup->button(1))
            b->setChecked(true);
    }

    // Trigger. Mode/direction buttons: setChecked() does not emit
    // idClicked, so applyTrigger() runs explicitly below.
    if (auto *b = m_trigModeGroup->button(ModeNone))
        b->setChecked(true);
    if (auto *b = m_trigDirGroup->button(0))
        b->setChecked(true);
    m_trigSource->setCurrentIndex(0);
    m_trigLevel->setValue(0.0);
    m_preTrigger->setValue(50);

    // Channels: CH1 on in manual range, the rest off.
    for (int ch = 0; ch < 4; ++ch) {
        ChannelControls &c = m_channel[ch];
        const int mode = ch == 0 ? ChannelManual : ChannelOff;
        if (auto *b = c.mode->button(mode))
            b->setChecked(true);
        // setChecked() alone skips the on/off handler (visibility, hardware
        // enable, cards) — it only listens to idClicked.
        emit c.mode->idClicked(mode);
        c.voltsPage->setCurrentIndex(kDefaultVoltsIndex);
        c.scale->setCurrentIndex(3);
        c.coupling->setCurrentIndex(0);
        c.probe->setCurrentIndex(0);
        c.invert->setChecked(false);
        c.filterType->setCurrentIndex(0);
        c.inputOffset->setValue(0.0);
        c.offsetV->setValue(0.0);
        setChannelPositionPercent(ch, 0.0);

        const QColor colour = Theme::channelColor(ch);
        m_colour[ch] = colour;
        m_view->setChannelColor(ch, colour);
        m_nav->setChannelColor(ch, colour);
        m_channelCard[ch]->setAccent(colour);
        QPalette pal = c.colour->palette();
        pal.setColor(QPalette::ButtonText, colour);
        c.colour->setPalette(pal);
        if (m_rulersChannelChecks[ch])
            setRulerCheckboxColor(m_rulersChannelChecks[ch], colour);

        for (int i = 0; i < ScopeView::MeasurementCount; ++i)
            m_view->setChannelMeasurement(ch, ScopeView::Measurement(i), false);
    }

    // Math: off, A + B of CH1 and CH2, default page and colour.
    m_mathEnabled->setChecked(false);
    m_mathOp->setCurrentIndex(0);
    m_mathSourceA->setCurrentIndex(0);
    m_mathSourceB->setCurrentIndex(1);
    m_mathVoltsPage->setCurrentIndex(kDefaultVoltsIndex + 1);
    m_mathFilterType->setCurrentIndex(0);
    m_mathScaleK->setValue(1.0);
    m_mathScaleC->setValue(0.0);
    m_engineEnable->setChecked(false);
    m_compressionEnable->setChecked(false);
    const QColor mathColour = Theme::mathColor();
    m_view->setMathColor(mathColour);
    m_nav->setMathColor(mathColour);
    QPalette mathPal = m_mathColour->palette();
    mathPal.setColor(QPalette::ButtonText, mathColour);
    m_mathColour->setPalette(mathPal);
    if (m_rulersMathCheck)
        setRulerCheckboxColor(m_rulersMathCheck, mathColour);
    for (int i = 0; i < ScopeView::MeasurementCount; ++i)
        m_view->setChannelMeasurement(ScopeView::RulersMathChannel,
                                      ScopeView::Measurement(i), false);
    applyMathChannel();

    // Timebases last, once the channels they size captures for are set.
    m_captureTimebase = kTimebases[kDefaultTimebaseIndex];
    m_liveTimebase = 1.0;
    if (m_acquisitionMode != ModeRecord) {
        m_timebase->blockSignals(true);
        setComboByNearestValue(m_timebase, m_acquisitionMode == ModeStream
                                               ? m_liveTimebase : m_captureTimebase);
        m_timebase->blockSignals(false);
        applyTimebase();
    }

    applyTrigger();
    resetZoom();
    refreshMeasureChannelSelector();
    refreshCards();
    statusBar()->showMessage(QStringLiteral("Default setup restored."), 3000);
}

void MainWindow::saveViewAsPng(const QString &path)
{
    // The plot area exactly as rendered at this moment — grid, traces,
    // cursors, annotations, and any reference waveforms currently visible,
    // since this is a straight visual snapshot of whatever is on screen
    // rather than anything reconstructed from the underlying data.
    const QPixmap pixmap = m_view->grab();
    if (pixmap.isNull() || !pixmap.save(path, "PNG")) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 4000);
        return;
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
}

void MainWindow::saveViewAsCsv(const QString &path)
{
    // Record mode is never routed here at all — saveCurrentView() doesn't
    // offer CSV as a filter for it (see that function's own comment for
    // why: a full, correct export would need to read the .vsxrec file
    // directly rather than the few thousand downsampled points ever
    // loaded into memory for display, and was dropped for simplicity
    // rather than built as the background-threaded job it would need).

    // One column per enabled physical channel, in probe-scaled, display-
    // ready units (channelDisplaySamples() bakes in probe gain/offset,
    // inversion, and DC offset) — the actual measured value, not the raw
    // ADC reading. References are deliberately excluded: Open/Save and
    // References are independent systems, and a reference is not part of
    // what this capture actually measured.
    struct Column { QString header; QVector<double> values; };
    QVector<Column> columns;
    for (int ch = 0; ch < 4; ++ch) {
        if (!m_view->channelVisible(ch))
            continue;
        columns.append({ QStringLiteral("CH%1 (V)").arg(ch + 1),
                          m_view->channelDisplaySamples(ch) });
    }
    if (m_mathEnabled && m_mathEnabled->isChecked()) {
        // The plot's own Math: computed from what each channel measured
        // (probe applied), with Math's filter, in its unit.
        const QString unit = m_view->mathUnit();
        columns.append({ QStringLiteral("Math %1 (%2)").arg(m_view->mathLabel(),
                                                           unit.isEmpty() ? QStringLiteral("ratio") : unit),
                         m_view->mathSamples() });
    }

    if (columns.isEmpty()) {
        statusBar()->showMessage(QStringLiteral(
            "Nothing is enabled to export \u2014 turn on a channel or Math first."), 4000);
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(path), 4000);
        return;
    }

    int rowCount = 0;
    for (const auto &col : columns)
        rowCount = std::max(rowCount, int(col.values.size()));

    QTextStream out(&file);
    out << "Time (s)";
    for (const auto &col : columns)
        out << ',' << col.header;
    out << '\n';

    // The same trigger-centred time convention rulers and everything else
    // in this app already use, not a plain "seconds since the start of
    // the buffer" — see timeAtSample()'s own comment.
    for (int i = 0; i < rowCount; ++i) {
        out << QString::number(m_view->timeAtSample(i), 'g', 9);
        for (const auto &col : columns) {
            out << ',';
            if (i < col.values.size())
                out << QString::number(col.values[i], 'g', 9);
        }
        out << '\n';
    }
    statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
}

// ---------------------------------------------------------------------------
// Steppers
// ---------------------------------------------------------------------------

void MainWindow::stepTimebase(int delta)
{
    VSX_TRACE("stepTimebase");
    // The Scope card's steppers act on whichever control is actually primary
    // for the current mode.
    if (m_acquisitionMode == ModeRecord) {
        const int index = std::clamp(m_recordRate->currentIndex() + delta,
                                     0, kSampleRateCount - 1);
        m_recordRate->setCurrentIndex(index);
    } else {
        // Clamped against the combo's own current size — Live mode loads a
        // shorter roll-mode list, so a fixed constant sized for the full
        // range would let this silently do nothing near the end of it.
        const int index = std::clamp(m_timebase->currentIndex() + delta,
                                     0, m_timebase->count() - 1);
        m_timebase->setCurrentIndex(index);
    }
}

void MainWindow::stepTriggerLevel(int delta)
{
    m_trigLevel->setValue(m_trigLevel->value() + delta * m_trigLevel->singleStep());
}

void MainWindow::leaveAutoRange(int channel)
{
    if (channel < 0 || channel >= 4 || m_channel[channel].mode->checkedId() != ChannelAuto)
        return;
    if (QAbstractButton *manual = m_channel[channel].mode->button(ChannelManual))
        manual->click();                          // the mode handler does the rest
}

void MainWindow::stepVoltsPerDiv(int channel, int delta)
{
    if (channel < 0 || channel >= 4)
        return;
    leaveAutoRange(channel);                      // card +/- and the phone remote
    QComboBox *box = m_channel[channel].voltsPage;
    box->setCurrentIndex(std::clamp(box->currentIndex() + delta, 0, kVoltsCount - 1));
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

void MainWindow::onDeviceChanged(bool ready, int channels)
{
    VSX_TRACE("onDeviceChanged");
    // Plugged in / out: a sound on every change after launch (whatever was
    // connected when the app started is the starting point, not an event).
#ifdef VSX_HAVE_MULTIMEDIA
    if (m_deviceStateKnown && ready != m_lastDeviceReady) {
        QSoundEffect *sound = ready ? m_plugInSound : m_plugOutSound;
        const bool on = m_soundsEnable && m_soundsEnable->isChecked();
        VSX_MEDIA_LOG("scope %s: %s", ready ? "plugged in" : "unplugged",
              !on ? "sounds are off in Settings"
              : !sound || sound->status() != QSoundEffect::Ready ? "sound not loaded"
              : "playing");
        if (on && sound)
            sound->play();
    }
#endif
    m_deviceStateKnown = true;
    m_lastDeviceReady = ready;
    setRunning(false);

    if (!ready) {
        // The device vanished (almost always an unplug). Capture's last
        // completed frame and Live's last pulled window already live
        // entirely in ScopeView/NavigatorView's own per-channel sample
        // buffers, and a Record's data is safe on disk in RecordWriter's
        // files — none of that needs the device to keep existing, and zoom,
        // pan, rulers and measurements all read from those buffers, never
        // from the SDK. So, unlike the "device has fewer channels than
        // this" case below, disconnect must NOT touch channel visibility,
        // channel count, or call clearAll() — every one of those actively
        // DISCARDS sample data (setChannelVisible(false) and
        // setChannelCount() both clear the channels they turn off; clearAll()
        // clears everything), which is exactly what used to erase the trace
        // on unplug in all three modes. AcquisitionManager has already
        // stopped polling on its own (poll()/arm() no-op once ready() is
        // false); pullStream()'s own disconnect check stops Live/Record on
        // its next tick and, for a Record, re-shows the full completed span
        // from disk via refreshRecordView() — untouched by any of this,
        // since it reads the record files directly rather than the device.
        statusBar()->showMessage(
            QStringLiteral("Device disconnected — showing the last data captured."), 6000);
        refreshCards();
        return;
    }

    m_channelCount = channels;
    refreshMathSources();

    for (int ch = 0; ch < 4; ++ch) {
        const bool present = ch < channels;
        m_channelCard[ch]->setVisible(present);
        if (!present) {
            if (auto *off = m_channel[ch].mode->button(ChannelOff))
                off->setChecked(true);
            m_view->setChannelVisible(ch, false);
            m_nav->setChannelVisible(ch, false);
        }
    }

    m_view->setChannelCount(m_channelCount);
    m_nav->setChannelCount(m_channelCount);

    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    const int savedSource = settings.value(QStringLiteral("trigger/source"), 0).toInt();
    const double savedLevel = settings.value(QStringLiteral("trigger/level"), 0.0).toDouble();

    m_trigSource->blockSignals(true);
    m_trigSource->clear();
    for (int ch = 0; ch < channels; ++ch)
        m_trigSource->addItem(QStringLiteral("CH%1").arg(ch + 1), ch);
    if (savedSource >= 0 && savedSource < m_trigSource->count())
        m_trigSource->setCurrentIndex(savedSource);
    m_trigSource->blockSignals(false);

    m_acq->setPreTriggerPercent(m_preTrigger->value());
    updateTriggerLevelRange();

    // updateTriggerLevelRange() bounds the level to the page and window, so the
    // saved value can only be applied once those are known.
    m_trigLevel->blockSignals(true);
    m_trigLevel->setValue(savedLevel);
    m_trigLevel->blockSignals(false);

    applyTimebase();
    for (int ch = 0; ch < channels; ++ch)
        applyChannel(ch);
    applyTrigger();

    for (int ch = 0; ch < channels; ++ch) {
        const bool on = m_channel[ch].mode->checkedId() != ChannelOff;
        m_channel[ch].voltsPage->setEnabled(
            m_channel[ch].mode->checkedId() != ChannelOff);
        m_view->setChannelVisible(ch, on);
        m_nav->setChannelVisible(ch, on);
        m_acq->setChannelEnabled(ch, on);
    }
    bool anyOn = false;
    for (int ch = 0; ch < channels; ++ch)
        anyOn = anyOn || m_channel[ch].mode->checkedId() != ChannelOff;

    if (channels > 0 && !anyOn) {
        if (auto *manual = m_channel[0].mode->button(ChannelManual))
            manual->setChecked(true);
        m_channel[0].voltsPage->setEnabled(true);
        m_view->setChannelVisible(0, true);
        m_nav->setChannelVisible(0, true);
        m_acq->setChannelEnabled(0, true);
    }

    refreshCards();
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // A recording still running when the window closes needs to be
    // properly finished first — flushed and closed on the writer thread —
    // whether it is about to be deleted (Temporary) or not: deleting files
    // still open for writing races the writer thread, and a Permanent
    // recording left mid-write would not read back correctly next launch
    // either. stopAcquisition() finishes it via the exact same path
    // pressing Stop does.
    if (m_running && m_acquisitionMode == ModeRecord)
        stopAcquisition();

    saveSettings();
    for (const QString &dir : m_temporaryRecordings)
        RecordWriter::deleteRecording(dir);
    QMainWindow::closeEvent(event);
}

namespace {
QString channelKey(int ch, const char *field)
{
    return QStringLiteral("channel%1/%2").arg(ch + 1).arg(QLatin1String(field));
}
}

void MainWindow::saveSettings() const
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    // Viewing a loaded waveform: what's on screen is the file's, not the
    // user's setup — keep the setup from before it was opened.
    if (!m_fileViewSnapshot.isEmpty()) {
        QSettings snapshot(m_fileViewSnapshot, QSettings::IniFormat);
        for (const QString &key : snapshot.allKeys())
            settings.setValue(key, snapshot.value(key));
        settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
        return;
    }
    saveSettingsTo(settings);
}

void MainWindow::saveSettingsTo(QSettings &settings) const
{

    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    // Saved from the two members themselves, not from the combo — each
    // mode's own value, so both survive being quit while the other mode
    // was the active one; see restoreSettings() for the matching read, and
    // m_captureTimebase's own comment in mainwindow.h for the bug this
    // replaces (a single shared value, silently overwritten by whichever
    // mode ran last and substituted into the other's different range).
    settings.setValue(QStringLiteral("scope/captureTimebase"), m_captureTimebase);
    settings.setValue(QStringLiteral("scope/liveTimebase"), m_liveTimebase);
    settings.setValue(QStringLiteral("scope/recordRate"), m_recordRate->currentIndex());
    settings.setValue(QStringLiteral("scope/liveRate"), m_liveRate->currentIndex());
    settings.setValue(QStringLiteral("scope/recordSize"), m_recordSize->currentIndex());
    settings.setValue(QStringLiteral("scope/recordPersist"),
                      m_recordPersistGroup ? m_recordPersistGroup->checkedId() : 1);
    settings.setValue(QStringLiteral("scope/depth"), m_depth->currentIndex());

    settings.setValue(QStringLiteral("trigger/mode"), m_trigModeGroup->checkedId());
    settings.setValue(QStringLiteral("trigger/direction"), m_trigDirGroup->checkedId());
    settings.setValue(QStringLiteral("trigger/source"), m_trigSource->currentIndex());
    settings.setValue(QStringLiteral("trigger/level"), m_trigLevel->value());
    settings.setValue(QStringLiteral("trigger/pretrigger"), m_preTrigger->value());

    for (int ch = 0; ch < 4; ++ch) {
        const ChannelControls &c = m_channel[ch];
        settings.setValue(channelKey(ch, "mode"), c.mode->checkedId());
        settings.setValue(channelKey(ch, "page"), c.voltsPage->currentIndex());
        settings.setValue(channelKey(ch, "scale"), c.scale->currentIndex());
        settings.setValue(channelKey(ch, "coupling"), c.coupling->currentIndex());
        settings.setValue(channelKey(ch, "probe"), c.probe->currentText());
        settings.setValue(channelKey(ch, "invert"), c.invert->isChecked());
        settings.setValue(channelKey(ch, "inputOffset"), c.inputOffset->value());
        settings.setValue(channelKey(ch, "dcOffset"), c.offsetV->value());
        settings.setValue(channelKey(ch, "offsetPct"), c.offsetPct->value());
        settings.setValue(channelKey(ch, "filter"), c.filterType->currentData());
        settings.setValue(channelKey(ch, "filterLowHz"), c.filterLowHz->value());
        settings.setValue(channelKey(ch, "filterHighResN"), c.filterHighResN->currentData());
        settings.setValue(channelKey(ch, "filterHighHz"), c.filterHighHz->value());
        settings.setValue(channelKey(ch, "colour"), m_colour[ch].name());

        // One bitmask rather than eleven separate keys — cheap to read back
        // as a whole, and the bit order only has to agree with itself
        // (Measurement's own declaration order) since nothing outside this
        // pair of functions ever looks at it directly.
        quint32 measurementMask = 0;
        for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
            if (m_view->channelMeasurementEnabled(ch, ScopeView::Measurement(i)))
                measurementMask |= (1u << i);
        }
        settings.setValue(channelKey(ch, "measurements"), measurementMask);
    }

    settings.setValue(QStringLiteral("math/enabled"), m_mathEnabled->isChecked());
    // By operation, not menu position (the menu is grouped).
    settings.setValue(QStringLiteral("math/operation"), m_mathOp->currentData());
    settings.setValue(QStringLiteral("math/k"), m_mathScaleK->value());
    settings.setValue(QStringLiteral("math/c"), m_mathScaleC->value());
    settings.setValue(QStringLiteral("math/sourceA"), m_mathSourceA->currentIndex());
    settings.setValue(QStringLiteral("math/sourceB"), m_mathSourceB->currentIndex());
    settings.setValue(QStringLiteral("math/voltsPage"), m_mathVoltsPage->currentIndex());
    settings.setValue(QStringLiteral("math/colour"), m_view->mathColor().name());
    settings.setValue(QStringLiteral("math/filter"), m_mathFilterType->currentData());
    settings.setValue(QStringLiteral("engine/cylinders"), m_engineCylinders->value());
    settings.setValue(QStringLiteral("engine/firingOrder"), m_engineFiringOrder->text());
    settings.setValue(QStringLiteral("engine/cycleDegrees"), m_engineStroke->currentData());
    settings.setValue(QStringLiteral("engine/style"), m_engineStyle->currentData());
    settings.setValue(QStringLiteral("battery/system"), m_batterySystem->currentData());
    settings.setValue(QStringLiteral("remote/port"), m_remotePort->value());
    settings.setValue(QStringLiteral("remote/rate"), m_remoteRate->currentData());
    settings.setValue(QStringLiteral("battery/temperature"), m_batteryTemperature->value());
    settings.setValue(QStringLiteral("battery/clamp"), m_batteryClamp->isChecked());
    settings.setValue(QStringLiteral("battery/ratedCca"), m_batteryRatedCca->value());
    settings.setValue(QStringLiteral("ui/sounds"), m_soundsEnable->isChecked());
    settings.setValue(QStringLiteral("zero/enabled"), m_zeroEnable->isChecked());
    settings.setValue(QStringLiteral("trigger/autoWait"), m_autoWait->currentData());
    settings.setValue(QStringLiteral("rulers/track"), m_rulersTrackCheck->isChecked());
    settings.setValue(QStringLiteral("capture/average"), m_average->currentData());
    {
        // A copy with the current context's annotations as they are now
        // (saveSettings() is const: the stored sets stay untouched).
        QHash<QString, QVariantList> current = m_annotationSets;
        if (m_annotationsReady)
            current[m_annotationContext] = m_view->annotationsState();
        QVariantMap sets;
        for (auto it = current.constBegin(); it != current.constEnd(); ++it) {
            if (it.value().isEmpty())
                continue;
            if (it.key().startsWith(QLatin1String("record:")) && it.key().size() > 7
                && !QFileInfo::exists(it.key().mid(7)))
                continue;                                    // that recording is gone
            sets.insert(it.key(), it.value());
        }
        settings.setValue(QStringLiteral("annotations/sets"), sets);
        settings.remove(QStringLiteral("annotations"));       // the old single set
    }
    // Decoding slots (they used to reset every session).
    for (int s = 0; s < int(m_decodeUi.size()); ++s) {
        const DecodeSlotUi &u = m_decodeUi[size_t(s)];
        if (!u.enable)
            continue;
        const QString key = QStringLiteral("decode/%1/").arg(s);
        settings.setValue(key + QStringLiteral("enabled"), u.enable->isChecked());
        for (auto [name, box] : { std::pair<const char *, QComboBox *>{ "protocol", u.protocol },
                                  { "input", u.input }, { "canRate", u.canRate },
                                  { "canFdRate", u.canFdRate }, { "linBaud", u.linBaud },
                                  { "linVersion", u.linVersion }, { "klineBaud", u.klineBaud },
                                  { "klineFraming", u.klineFraming }, { "sentTick", u.sentTick },
                                  { "sentNibbles", u.sentNibbles }, { "sentMode", u.sentMode },
                                  { "diagIds", u.diagIds } }) {
            if (box)
                settings.setValue(key + QLatin1String(name), box->currentData());
        }
        if (u.threshold)
            settings.setValue(key + QStringLiteral("threshold"), u.threshold->value());
        if (u.samplePoint)
            settings.setValue(key + QStringLiteral("samplePoint"), u.samplePoint->value());
        if (u.invert)
            settings.setValue(key + QStringLiteral("invert"), u.invert->isChecked());
    }
    settings.setValue(QStringLiteral("ui/startupAnimation"), m_startupAnimation->isChecked());
    settings.setValue(QStringLiteral("drain/channel"), m_drainChannel->currentIndex());
    settings.setValue(QStringLiteral("drain/seconds"), m_drainDuration->currentData());
    settings.setValue(QStringLiteral("drain/capacity"), m_drainCapacity->value());
    settings.setValue(QStringLiteral("drain/limit"), m_drainLimit->value());
    settings.setValue(QStringLiteral("battery/restingGood"), m_limitRestingGood->value());
    settings.setValue(QStringLiteral("battery/crankMin"), m_limitCrankMin->value());
    settings.setValue(QStringLiteral("battery/chargeLow"), m_limitChargeLow->value());
    settings.setValue(QStringLiteral("battery/chargeHigh"), m_limitChargeHigh->value());
    settings.setValue(QStringLiteral("battery/ripple"), m_limitRipple->value());
    settings.setValue(QStringLiteral("engine/crankChannel"), m_crankChannel->currentData());
    settings.setValue(QStringLiteral("engine/camChannel"), m_camChannel->currentData());
    settings.setValue(QStringLiteral("engine/crankPattern"), m_crankPattern->currentData());
    settings.setValue(QStringLiteral("engine/crankTdc"), m_crankTdc->value());
    settings.setValue(QStringLiteral("engine/camSpec"), m_camSpec->value());
    settings.setValue(QStringLiteral("engine/camTolerance"), m_camTolerance->value());
    settings.setValue(QStringLiteral("view/readoutCorner"), m_view->readoutCorner());
    settings.setValue(QStringLiteral("engine/source"), m_engineSource->currentData());
    settings.setValue(QStringLiteral("engine/syncThreshold"), m_engineSyncThreshold->value());
    settings.setValue(QStringLiteral("engine/syncRising"), m_engineSyncEdge->currentData());
    settings.setValue(QStringLiteral("engine/syncOffset"), m_engineSyncOffset->value());
    settings.setValue(QStringLiteral("engine/compressionChannel"), m_compressionChannel->currentData());
    settings.setValue(QStringLiteral("engine/compressionSignal"), m_compressionSignal->currentData());
    settings.setValue(QStringLiteral("engine/compressionFlag"), m_compressionFlag->value());
    settings.setValue(QStringLiteral("math/filterLowHz"), m_mathFilterLowHz->value());
    settings.setValue(QStringLiteral("math/filterHighHz"), m_mathFilterHighHz->value());

    quint32 mathMeasurementMask = 0;
    for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
        if (m_view->channelMeasurementEnabled(ScopeView::RulersMathChannel, ScopeView::Measurement(i)))
            mathMeasurementMask |= (1u << i);
    }
    settings.setValue(QStringLiteral("math/measurements"), mathMeasurementMask);
}

// Everything is restored with signals blocked, then applied in one pass: a
// cascade of currentIndexChanged during restore would fight itself, and the
// device may not even be connected yet.
void MainWindow::takeFileViewSnapshot()
{
    QTemporaryFile file(QDir::temp().filePath(QStringLiteral("vsx-view-XXXXXX.ini")));
    file.setAutoRemove(false);
    if (!file.open())
        return;
    m_fileViewSnapshot = file.fileName();
    file.close();
    QSettings snapshot(m_fileViewSnapshot, QSettings::IniFormat);
    saveSettingsTo(snapshot);
    snapshot.sync();
}

void MainWindow::leaveFileView()
{
    if (m_fileViewSnapshot.isEmpty() || m_applyingFileView)
        return;
    const QString path = m_fileViewSnapshot;
    m_fileViewSnapshot.clear();
    {
        QSettings snapshot(path, QSettings::IniFormat);
        restoreSettingsFrom(snapshot, false);
    }
    QFile::remove(path);
    for (int ch = 0; ch < 4; ++ch)
        applyChannel(ch);
    applyTimebase();
    applyTrigger();
    applyMathChannel();
    refreshCards();
}

void MainWindow::restoreSettings()
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    restoreSettingsFrom(settings, true);
}

void MainWindow::restoreSettingsFrom(QSettings &settings, bool session)
{
    VSX_TRACE("restoreSettings");
    if (!settings.contains(QStringLiteral("scope/captureTimebase"))
        && !settings.contains(QStringLiteral("scope/timebase")))
        return;                                    // first run: keep the defaults

    if (session)                                   // not when leaving a loaded waveform's view
        restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());

    const auto setCombo = [](QComboBox *box, const QVariant &value) {
        const int index = value.toInt();
        if (index >= 0 && index < box->count()) {
            box->blockSignals(true);
            box->setCurrentIndex(index);
            box->blockSignals(false);
        }
    };

    setCombo(m_recordRate, settings.value(QStringLiteral("scope/recordRate"), kDefaultSampleRateIndex));
    setCombo(m_liveRate, settings.value(QStringLiteral("scope/liveRate"), kDefaultSampleRateIndex));
    setCombo(m_recordSize, settings.value(QStringLiteral("scope/recordSize"), 1));
    if (m_recordPersistGroup) {
        const int persistId = settings.value(QStringLiteral("scope/recordPersist"), 1).toInt();
        if (auto *b = m_recordPersistGroup->button(persistId)) {
            b->blockSignals(true);
            b->setChecked(true);
            b->blockSignals(false);
        }
    }
    setCombo(m_depth,    settings.value(QStringLiteral("scope/depth")));

    // Matched by VALUE, not index — the combo at this point always holds
    // Capture/Record's full kTimebases list (mode itself is not persisted,
    // so every launch starts in Capture). findData() also cannot be trusted
    // to hit exactly: QSettings round-trips doubles through text on the
    // ini-format backend, so the restored value is not always bit-identical
    // to the compile-time constant it started as. Nearest match handles
    // both cases — snapping back to the exact entry when it matches, and
    // picking the closest available one (same policy as
    // refreshTimebaseOptions()) when it does not.
    //
    // "scope/timebase" (no mode in the name) is the old, single shared key
    // from before each mode got its own — read here only as the fallback
    // default for Capture's own value, for an existing settings file that
    // predates this split; a fresh save from this version onward always has
    // both new keys and never touches this old one again.
    const double legacyTimebase =
        settings.value(QStringLiteral("scope/timebase"), m_captureTimebase).toDouble();
    const double savedCaptureTimebase =
        settings.value(QStringLiteral("scope/captureTimebase"), legacyTimebase).toDouble();
    m_liveTimebase =
        settings.value(QStringLiteral("scope/liveTimebase"), m_liveTimebase).toDouble();
    if (m_timebase->count() > 0) {
        int index = 0;
        double bestDiff = std::fabs(m_timebase->itemData(0).toDouble() - savedCaptureTimebase);
        for (int i = 1; i < m_timebase->count(); ++i) {
            const double diff = std::fabs(m_timebase->itemData(i).toDouble() - savedCaptureTimebase);
            if (diff < bestDiff) {
                bestDiff = diff;
                index = i;
            }
        }
        m_timebase->blockSignals(true);
        m_timebase->setCurrentIndex(index);
        m_timebase->blockSignals(false);
        // The actual matched value, not the raw saved one — keeps
        // m_captureTimebase consistent with whatever the combo now shows,
        // the same invariant applyTimebase() maintains during normal use.
        m_captureTimebase = m_timebase->itemData(index).toDouble();
    }

    if (auto *b = m_trigModeGroup->button(
            settings.value(QStringLiteral("trigger/mode"), ModeNone).toInt()))
        b->setChecked(true);
    if (auto *b = m_trigDirGroup->button(
            settings.value(QStringLiteral("trigger/direction"), 0).toInt()))
        b->setChecked(true);

    m_preTrigger->blockSignals(true);
    m_preTrigger->setValue(settings.value(QStringLiteral("trigger/pretrigger"), 50).toInt());
    m_preTrigger->blockSignals(false);

    for (int ch = 0; ch < 4; ++ch) {
        ChannelControls &c = m_channel[ch];

        if (auto *b = c.mode->button(
                settings.value(channelKey(ch, "mode"),
                               ch == 0 ? ChannelManual : ChannelOff).toInt()))
            b->setChecked(true);

        setCombo(c.voltsPage, settings.value(channelKey(ch, "page"), kDefaultVoltsIndex));
        setCombo(c.scale,     settings.value(channelKey(ch, "scale"), 3));
        setCombo(c.coupling,  settings.value(channelKey(ch, "coupling"), 0));

        const QString probeName = settings.value(channelKey(ch, "probe")).toString();
        const int probeIndex = c.probe->findText(probeName);
        if (probeIndex >= 0) {
            c.probe->blockSignals(true);
            c.probe->setCurrentIndex(probeIndex);
            c.probe->blockSignals(false);
            applyProbeUnits(ch);                    // (signals were blocked)
        }

        c.invert->blockSignals(true);
        c.invert->setChecked(settings.value(channelKey(ch, "invert"), false).toBool());
        c.invert->blockSignals(false);

        for (auto *box : { c.inputOffset, c.offsetV, c.offsetPct })
            box->blockSignals(true);
        c.inputOffset->setValue(settings.value(channelKey(ch, "inputOffset"), 0.0).toDouble());
        c.offsetV->setValue(settings.value(channelKey(ch, "dcOffset"), 0.0).toDouble());
        c.offsetPct->setValue(settings.value(channelKey(ch, "offsetPct"), 0.0).toDouble());
        for (auto *box : { c.inputOffset, c.offsetV, c.offsetPct })
            box->blockSignals(false);
        // Blocked above specifically so restoring these doesn't trigger
        // their own valueChanged side effects — but that also means
        // setChannelPositionPercent() never ran, which is the only other
        // place m_channelOffsetPctExact[ch] gets kept in sync. Without
        // this, a restored non-zero offsetPct would leave the exact
        // accumulator sitting at its default 0 until the next drag reset
        // it back to the wrong starting point.
        m_channelOffsetPctExact[ch] = c.offsetPct->value();
        // Filters (settings from before band/high-pass carry over as low-pass).
        const bool oldLowPass = settings.value(channelKey(ch, "lowPass"), false).toBool();
        c.filterLowHz->setValue(settings.value(channelKey(ch, "filterLowHz"), 10.0).toDouble());
        {
            const int n = c.filterHighResN->findData(settings.value(channelKey(ch, "filterHighResN"), 16));
            c.filterHighResN->setCurrentIndex(n >= 0 ? n : 1);
        }
        c.filterHighHz->setValue(settings.value(channelKey(ch, "filterHighHz"),
            settings.value(channelKey(ch, "lowPassHz"), 100e3)).toDouble());
        {
            const int type = settings.value(channelKey(ch, "filter"),
                                            oldLowPass ? int(ScopeView::FilterLowPass) : 0).toInt();
            const int i = c.filterType->findData(type);
            c.filterType->setCurrentIndex(i >= 0 ? i : 0);
        }
        m_view->setChannelFilter(ch, c.filterType->currentData().toInt(),
                                 filterLowParam(ch), c.filterHighHz->value());

        const QColor colour(settings.value(channelKey(ch, "colour"),
                                           Theme::channelColor(ch).name()).toString());
        if (colour.isValid()) {
            m_colour[ch] = colour;
            m_view->setChannelColor(ch, colour);
            m_nav->setChannelColor(ch, colour);
            m_channelCard[ch]->setAccent(colour);
            QPalette pal = c.colour->palette();
            pal.setColor(QPalette::ButtonText, colour);
            c.colour->setPalette(pal);
            if (m_rulersChannelChecks[ch])
                setRulerCheckboxColor(m_rulersChannelChecks[ch], colour);
        }

        const quint32 measurementMask = settings.value(channelKey(ch, "measurements"), 0u).toUInt();
        for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
            m_view->setChannelMeasurement(ch, ScopeView::Measurement(i),
                                          (measurementMask & (1u << i)) != 0);
        }

        const bool on = c.mode->checkedId() != ChannelOff;
        c.voltsPage->setEnabled(c.mode->checkedId() != ChannelOff);
        m_view->setChannelVisible(ch, on);
        m_nav->setChannelVisible(ch, on);
    }

    m_mathEnabled->blockSignals(true);
    m_mathEnabled->setChecked(settings.value(QStringLiteral("math/enabled"), false).toBool());
    m_mathEnabled->blockSignals(false);
    {
        // Older settings saved the menu position, whose 0/1/2 were exactly
        // the operations Add/Subtract/Multiply.
        const int op = settings.value(QStringLiteral("math/operation"),
                                      settings.value(QStringLiteral("math/op"), 0)).toInt();
        const int i = m_mathOp->findData(op);
        m_mathOp->setCurrentIndex(i >= 0 ? i : 0);
    }
    m_mathScaleK->setValue(settings.value(QStringLiteral("math/k"), 1.0).toDouble());
    m_mathScaleC->setValue(settings.value(QStringLiteral("math/c"), 0.0).toDouble());
    setCombo(m_mathSourceA, settings.value(QStringLiteral("math/sourceA"), 0));
    setCombo(m_mathSourceB, settings.value(QStringLiteral("math/sourceB"), 1));
    setCombo(m_mathVoltsPage, settings.value(QStringLiteral("math/voltsPage"), kDefaultVoltsIndex + 1));

    const QColor mathColour(settings.value(QStringLiteral("math/colour"),
                                           Theme::mathColor().name()).toString());
    if (mathColour.isValid()) {
        m_view->setMathColor(mathColour);
        m_nav->setMathColor(mathColour);
        QPalette mathPal = m_mathColour->palette();
        mathPal.setColor(QPalette::ButtonText, mathColour);
        m_mathColour->setPalette(mathPal);
        if (m_rulersMathCheck)
            setRulerCheckboxColor(m_rulersMathCheck, mathColour);
    }
    applyMathChannel();   // one pass, same reasoning as the per-channel restore above
    // (A Math low-pass saved before the full filter set carries over.)
    m_mathFilterLowHz->setValue(settings.value(QStringLiteral("math/filterLowHz"), 10.0).toDouble());
    m_mathFilterHighHz->setValue(settings.value(QStringLiteral("math/filterHighHz"),
        settings.value(QStringLiteral("math/lowPassHz"), 100e3)).toDouble());
    {
        const bool oldLowPass = settings.value(QStringLiteral("math/lowPass"), false).toBool();
        const int type = settings.value(QStringLiteral("math/filter"),
                                        oldLowPass ? int(ScopeView::FilterLowPass) : 0).toInt();
        const int i = m_mathFilterType->findData(type);
        m_mathFilterType->setCurrentIndex(i >= 0 ? i : 0);
    }
    m_view->setMathFilter(m_mathFilterType->currentData().toInt(),
                          m_mathFilterLowHz->value(), m_mathFilterHighHz->value());

    // Engine setup (the overlay and test themselves start off each session).
    m_engineCylinders->setValue(settings.value(QStringLiteral("engine/cylinders"), 4).toInt());
    m_engineFiringOrder->setText(settings.value(QStringLiteral("engine/firingOrder"),
                                                QStringLiteral("1-3-4-2")).toString());
    const auto selectData = [](QComboBox *box, const QVariant &value) {
        const int i = box->findData(value);
        if (i >= 0)
            box->setCurrentIndex(i);
    };
    selectData(m_engineStroke, settings.value(QStringLiteral("engine/cycleDegrees"), 720.0));
    selectData(m_engineStyle, settings.value(QStringLiteral("engine/style"), 1));
    // System first (it resets the limits to its defaults), then any saved limits.
    selectData(m_batterySystem, settings.value(QStringLiteral("battery/system"), 12.0));
    m_remotePort->setValue(settings.value(QStringLiteral("remote/port"), 8765).toInt());
    selectData(m_remoteRate, settings.value(QStringLiteral("remote/rate"), 15));
    m_batteryTemperature->setValue(settings.value(QStringLiteral("battery/temperature"), 20.0).toDouble());
    m_batteryClamp->setChecked(settings.value(QStringLiteral("battery/clamp"), false).toBool());
    m_batteryRatedCca->setValue(settings.value(QStringLiteral("battery/ratedCca"), 0.0).toDouble());
    m_soundsEnable->setChecked(settings.value(QStringLiteral("ui/sounds"), true).toBool());
    m_zeroEnable->setChecked(settings.value(QStringLiteral("zero/enabled"), true).toBool());
    {
        const int i = m_autoWait->findData(settings.value(QStringLiteral("trigger/autoWait"), 0));
        m_autoWait->setCurrentIndex(i >= 0 ? i : 0);
    }
    m_rulersTrackCheck->setChecked(settings.value(QStringLiteral("rulers/track"), false).toBool());
    {
        const int i = m_average->findData(settings.value(QStringLiteral("capture/average"), 1));
        m_average->setCurrentIndex(i >= 0 ? i : 0);
    }
    // Decoding slots: settings first, then the switch, so a slot starts
    // decoding with its own settings already in place.
    for (int s = 0; s < int(m_decodeUi.size()); ++s) {
        DecodeSlotUi &u = m_decodeUi[size_t(s)];
        const QString key = QStringLiteral("decode/%1/").arg(s);
        if (!u.enable || !settings.contains(key + QStringLiteral("enabled")))
            continue;
        for (auto [name, box] : { std::pair<const char *, QComboBox *>{ "protocol", u.protocol },
                                  { "input", u.input }, { "canRate", u.canRate },
                                  { "canFdRate", u.canFdRate }, { "linBaud", u.linBaud },
                                  { "linVersion", u.linVersion }, { "klineBaud", u.klineBaud },
                                  { "klineFraming", u.klineFraming }, { "sentTick", u.sentTick },
                                  { "sentNibbles", u.sentNibbles }, { "sentMode", u.sentMode },
                                  { "diagIds", u.diagIds } }) {
            if (!box || !settings.contains(key + QLatin1String(name)))
                continue;
            const int i = box->findData(settings.value(key + QLatin1String(name)));
            if (i >= 0)
                box->setCurrentIndex(i);
        }
        if (u.threshold && settings.contains(key + QStringLiteral("threshold")))
            u.threshold->setValue(settings.value(key + QStringLiteral("threshold")).toDouble());
        if (u.samplePoint && settings.contains(key + QStringLiteral("samplePoint")))
            u.samplePoint->setValue(settings.value(key + QStringLiteral("samplePoint")).toDouble());
        if (u.invert)
            u.invert->setChecked(settings.value(key + QStringLiteral("invert"), false).toBool());
        u.enable->setChecked(settings.value(key + QStringLiteral("enabled"), false).toBool());
    }
    // Annotations once the window is up and laid out (their positions are
    // fractions of the plot's size). Startup only.
    if (session) {
        const QVariantMap sets = settings.value(QStringLiteral("annotations/sets")).toMap();
        for (auto it = sets.constBegin(); it != sets.constEnd(); ++it)
            m_annotationSets.insert(it.key(), it.value().toList());
        // From before annotations were per mode: into the context we start in.
        const QVariantList single = settings.value(QStringLiteral("annotations")).toList();
        QTimer::singleShot(200, this, [this, single] {
            m_annotationContext = annotationContext();
            if (!single.isEmpty() && !m_annotationSets.contains(m_annotationContext))
                m_annotationSets.insert(m_annotationContext, single);
            m_view->clearAnnotations();
            m_view->restoreAnnotations(m_annotationSets.value(m_annotationContext));
            m_annotationsReady = true;
        });
    }
    m_zeroTable.clear();
    settings.beginGroup(QStringLiteral("zero/table"));
    for (const QString &key : settings.childKeys())
        m_zeroTable.insert(QString(key).replace(QLatin1Char('_'), QLatin1Char('/')), settings.value(key).toDouble());
    settings.endGroup();
    {
        // Offsets no open input could have (calibrated with a signal
        // connected) are dropped, and the clean table kept.
        int dropped = 0;
        for (auto it = m_zeroTable.begin(); it != m_zeroTable.end();) {
            const int halfMv = it.key().section(QLatin1Char('/'), 3, 3).toInt();
            if (std::fabs(it.value()) > plausibleOffsetV(halfMv)) {
                it = m_zeroTable.erase(it);
                ++dropped;
            } else {
                ++it;
            }
        }
        if (dropped > 0) {
            saveZeroTable();
            statusBar()->showMessage(QStringLiteral(
                "%1 stored zero offsets were far too large for an open input (calibrated with a "
                "probe connected?) and were removed: calibrate again with the inputs disconnected")
                    .arg(dropped), 15000);
        }
    }
    refreshCalibrationStatus();
    m_startupAnimation->setChecked(settings.value(QStringLiteral("ui/startupAnimation"), true).toBool());
    m_drainChannel->setCurrentIndex(std::clamp(settings.value(QStringLiteral("drain/channel"), 1).toInt(), 0, 1));
    selectData(m_drainDuration, settings.value(QStringLiteral("drain/seconds"), 3600));
    m_drainCapacity->setValue(settings.value(QStringLiteral("drain/capacity"), 60.0).toDouble());
    m_drainLimit->setValue(settings.value(QStringLiteral("drain/limit"), 50.0).toDouble());
    if (settings.contains(QStringLiteral("battery/restingGood"))) {
        m_limitRestingGood->setValue(settings.value(QStringLiteral("battery/restingGood")).toDouble());
        m_limitCrankMin->setValue(settings.value(QStringLiteral("battery/crankMin")).toDouble());
        m_limitChargeLow->setValue(settings.value(QStringLiteral("battery/chargeLow")).toDouble());
        m_limitChargeHigh->setValue(settings.value(QStringLiteral("battery/chargeHigh")).toDouble());
        m_limitRipple->setValue(settings.value(QStringLiteral("battery/ripple")).toDouble());
    }
    selectData(m_crankChannel, settings.value(QStringLiteral("engine/crankChannel"), 0));
    selectData(m_camChannel, settings.value(QStringLiteral("engine/camChannel"), 1));
    selectData(m_crankPattern, settings.value(QStringLiteral("engine/crankPattern"), 0));
    m_crankTdc->setValue(settings.value(QStringLiteral("engine/crankTdc"), 0.0).toDouble());
    m_camSpec->setValue(settings.value(QStringLiteral("engine/camSpec"), -1.0).toDouble());
    m_camTolerance->setValue(settings.value(QStringLiteral("engine/camTolerance"), 3.0).toDouble());
    m_camKnownGood = settings.value(QStringLiteral("engine/camKnownGood"), -1.0).toDouble();
    m_view->setReadoutCorner(settings.value(QStringLiteral("view/readoutCorner"), 0).toInt());
    selectData(m_engineSource, settings.value(QStringLiteral("engine/source"), 0));
    m_engineSyncThreshold->setValue(settings.value(QStringLiteral("engine/syncThreshold"), 2.5).toDouble());
    selectData(m_engineSyncEdge, settings.value(QStringLiteral("engine/syncRising"), true));
    m_engineSyncOffset->setValue(settings.value(QStringLiteral("engine/syncOffset"), 0.0).toDouble());
    selectData(m_compressionChannel, settings.value(QStringLiteral("engine/compressionChannel"), 0));
    selectData(m_compressionSignal, settings.value(QStringLiteral("engine/compressionSignal"), 0));
    m_compressionFlag->setValue(settings.value(QStringLiteral("engine/compressionFlag"), 15.0).toDouble());

    const quint32 mathMeasurementMask = settings.value(QStringLiteral("math/measurements"), 0u).toUInt();
    for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
        m_view->setChannelMeasurement(ScopeView::RulersMathChannel, ScopeView::Measurement(i),
                                      (mathMeasurementMask & (1u << i)) != 0);
    }

    // Level last: its bounds depend on the page and input offset restored above.
    m_trigLevel->blockSignals(true);
    m_trigLevel->setValue(settings.value(QStringLiteral("trigger/level"), 0.0).toDouble());
    m_trigLevel->blockSignals(false);
}

void MainWindow::syncNavigator()
{
    m_nav->setViewport(m_view->viewportCentreFraction(),
                       m_view->viewportWidthFraction());
    m_nav->setTriggerFraction(m_view->triggerFraction());
    m_nav->setFollowingTrigger(m_view->isFollowingTrigger());
}

// Capture and stream cannot coexist: SetOscCaptureMode switches the whole
// device, and both engines share one InitDll session. Switching therefore stops
// whatever is running first.
void MainWindow::setAcquisitionMode(int mode)
{
    leaveFileView();     // a loaded waveform's settings stay with it
    ++m_avgGeneration;   // a running Capture average starts over
    VSX_TRACE("setAcquisitionMode");
    if (m_acquisitionMode == mode)
        return;

    const int previousMode = m_acquisitionMode;
    stopAcquisition();
    m_acquisitionMode = mode;
    // References make sense only in Capture/Live — see setReferencesVisible()'s
    // own comment for why this suppresses drawing/gutter space rather than
    // discarding anything already loaded.
    m_view->setReferencesVisible(mode != ModeRecord);
    m_view->setLowPassInView(mode != ModeRecord);   // Record filters the raw recording itself
    // Live's reduction (refreshLiveView()) sets itself up again from scratch:
    // the line above just changed what it had switched off.
    m_liveReduced = false;
    m_view->clearMeasurementOverrides();
    m_view->setTraceGaps(false);
    m_view->setSamplesArePairs(false);   // Record / Live set it again for their own data

    // Save the outgoing mode's own vertical scale/position before anything
    // else runs — a level or scale adjusted here should stay exactly as
    // left, not silently reappear as whatever another mode happens to
    // leave in ScopeView's own (shared) state when this mode is entered
    // again later. Record's own branch below saves per current tab, not
    // per mode — the same tab, revisited later, needs its own state back,
    // not just "whatever Record mode last had" generically.
    if (previousMode == ModeCapture) {
        for (int ch = 0; ch < 4; ++ch) {
            m_captureSnapshot.voltsPerDiv[ch] = m_view->voltsPerDiv(ch);
            m_captureSnapshot.positionVolts[ch] = m_view->positionVolts(ch);
        }
        m_captureSnapshot.centreFraction = m_view->viewportCentreFraction();
        m_captureSnapshot.followingTrigger = m_view->isFollowingTrigger();
    } else if (previousMode == ModeStream) {
        for (int ch = 0; ch < 4; ++ch) {
            m_liveSnapshot.voltsPerDiv[ch] = m_view->voltsPerDiv(ch);
            m_liveSnapshot.positionVolts[ch] = m_view->positionVolts(ch);
        }
        if (m_liveLoaded)
            m_liveLoadedCentre = m_view->viewportCentreFraction();
    } else if (previousMode == ModeRecord && !m_currentRecordTab.isEmpty()) {
        // Same save loadRecordTab() already does on an in-mode tab switch,
        // run here too so it also happens on leaving Record mode entirely —
        // otherwise a tab viewed only once, never switched away from
        // in-mode, has nothing in m_recordTabStates yet the first time
        // applyRecordTabViewState() looks it up on the way back in.
        RecordTabViewState &state = m_recordTabStates[m_currentRecordTab];
        state.viewStart = m_recordViewStart;
        state.viewLength = m_recordViewLength;
        for (int ch = 0; ch < 4; ++ch) {
            state.channels[ch].voltsPerDiv = m_view->voltsPerDiv(ch);
            state.channels[ch].positionVolts = m_view->positionVolts(ch);
            state.channels[ch].visible = m_view->channelVisible(ch);
        }
    }

    const bool streaming = mode != ModeCapture;

    // Spectrum, XY, and Persistence only make sense against a single,
    // settled capture, not a continuously streaming or recording signal —
    // available only in Capture mode. Disabled rather than hidden, so the
    // choice is visibly still there, just not usable from here; forced back
    // to Scope if one of them was active when leaving Capture, since a mode
    // that just became unusable cannot stay the current one.
    if (m_instrumentMode) {
        for (int id = 1; id <= 3; ++id) {
            if (auto *button = m_instrumentMode->button(id))
                button->setEnabled(!streaming);
        }
        if (streaming && m_currentInstrumentMode != InstrumentMode::Scope) {
            // setChecked() alone does not emit idClicked (only an actual
            // click does), so setInstrumentMode() needs calling explicitly
            // too — the same two-step every other programmatic mode switch
            // in this file already needs.
            if (auto *scopeButton = m_instrumentMode->button(0))
                scopeButton->setChecked(true);
            setInstrumentMode(InstrumentMode::Scope);
        }
    }

    m_timebaseGroup->setVisible(mode != ModeRecord);
    m_captureOnly->setVisible(mode == ModeCapture);
    m_streamOnly->setVisible(mode == ModeStream);
    m_recordOnly->setVisible(mode == ModeRecord);

    if (m_recordTabs) {
        // A hard, temporary height constraint, not the previous pass's
        // "measure the size, then resize back afterward" approach — the
        // screenshots proved that one insufficient: the window still grew
        // by roughly the tab bar's own height, meaning whatever Qt layout
        // recalculation actually causes this had not yet happened by the
        // time the deferred check ran, so there was nothing to undo yet.
        // Pinning the height outright prevents the growth from happening
        // at all: with the window's own height fixed, showing the tab bar
        // has nowhere to grow into, so the layout is forced to fit it by
        // giving the plot area — the one flexible, stretchable element in
        // this column — slightly less room instead.
        const int heightBeforeTabsToggle = height();
        setFixedHeight(heightBeforeTabsToggle);
        m_recordTabs->setVisible(mode == ModeRecord);
        setMinimumHeight(0);
        setMaximumHeight(QWIDGETSIZE_MAX);
        if (mode == ModeRecord) {
            refreshRecordTabs();
            // Load whatever refreshRecordTabs() just selected — the most
            // recent past recording, if any exist and none is actively
            // being written right now (nothing has started a recording yet
            // just by switching modes, so this is always safe here).
            const int index = m_recordTabs->currentIndex();
            const QString dir = index >= 0 ? m_recordTabs->tabData(index).toString() : QString();
            if (!dir.isEmpty()) {
                if (dir == m_currentRecordTab) {
                    // Same recording as before this trip through another
                    // mode — loadRecordTab() would treat this as a no-op
                    // and skip everything, but ScopeView's shared state
                    // may still hold whatever Capture or Live last put
                    // there regardless, so this tab's own display needs
                    // reapplying even though the tab itself never changed.
                    applyRecordTabViewState(dir);
                } else {
                    loadRecordTab(dir);
                }
            } else {
                // No recordings in the current record directory at all —
                // an empty plot, not whatever Capture or Live happened to
                // leave on screen.
                for (int ch = 0; ch < 4; ++ch)
                    m_view->clearChannel(ch);
                m_nav->clearAll();
                m_currentRecordTab.clear();
                syncAnnotationContext();
            }
        }
    }

    // Nothing about triggering applies to a continuous record.
    m_singleButton->setEnabled(!streaming && m_acq->ready());
    m_forceButton->setEnabled(!streaming && m_acq->ready());
    m_triggerCard->setMuted(streaming);
    m_freezeButton->setEnabled(false);
    m_freezeButton->setChecked(false);

    if (streaming && m_acq->ready())
        m_stream->attach(m_acq->deviceId(), m_acq->channels());
    if (mode != ModeRecord) {
        m_recordViewStart = 0;
        m_recordViewLength = 0;
        m_view->setDisplayRateOverride(-1.0);
        m_view->clearAbsoluteTimeOrigin();
        m_view->clearMeasurementSources();   // Record-only; see refreshRecordView()
    }
    m_view->setNowAtRightEdge(mode == ModeStream);

    if (mode == ModeRecord)
        applyRecordRate();
    else {
        refreshTimebaseOptions();
        applyTimebase(mode == ModeStream ? m_liveTimebase : m_captureTimebase);
    }

    if (mode == ModeCapture) {
        if (m_captureSnapshot.hasData) {
            // Restores exactly what was last captured, rather than leaving
            // whatever Live or Record most recently left in ScopeView's own
            // (shared) buffer on screen — see the samplesReady handler for
            // where this snapshot is captured, continuously, regardless of
            // which mode happens to be active at the time. A channel
            // currently off on the rail is cleared regardless of whether
            // it has snapshot data — an explicit off is a deliberate
            // choice that should never be silently overridden by old data
            // from while it was on.
            for (int ch = 0; ch < 4; ++ch) {
                const bool channelOn = m_channel[ch].mode->checkedId() != ChannelOff;
                if (m_captureSnapshot.valid[ch] && channelOn) {
                    m_view->setSamples(ch, m_captureSnapshot.samples[ch],
                                       m_captureSnapshot.sampleRate,
                                       m_captureSnapshot.triggerPoint);
                    m_nav->setSamples(ch, m_captureSnapshot.samples[ch]);
                    feedSpectrum(ch, m_captureSnapshot.samples[ch], m_captureSnapshot.sampleRate);
                    feedXY(ch, m_captureSnapshot.samples[ch]);
                } else {
                    m_view->clearChannel(ch);
                }
                if (m_captureSnapshot.voltsPerDiv[ch] > 0.0)
                    m_view->setVoltsPerDiv(ch, m_captureSnapshot.voltsPerDiv[ch]);
                // Through setChannelPositionPercent(), not a direct
                // setPositionVolts() call — that would leave the offsetPct
                // spinbox showing whatever the previous mode left it at,
                // and offsetDragged's handler reads that stale spinbox
                // value as the base for the very next drag, making the
                // trace instantly jump to the wrong level the moment the
                // user tried to adjust it in the newly-entered mode.
                {
                    const double page = m_channel[ch].voltsPage->currentData().toDouble();
                    const double pct = page > 0.0
                        ? m_captureSnapshot.positionVolts[ch] / page * 100.0 : 0.0;
                    setChannelPositionPercent(ch, pct);
                }
            }
            // Back where it was panned to (before, the view re-centred on the
            // trigger every time Capture was re-entered).
            if (!m_captureSnapshot.followingTrigger)
                m_view->setViewCentreFraction(m_captureSnapshot.centreFraction);
            syncNavigator();
        } else {
            // Never captured anything yet this session — an empty plot,
            // not whatever another mode happened to leave on screen.
            for (int ch = 0; ch < 4; ++ch)
                m_view->clearChannel(ch);
            m_nav->clearAll();
        }
    } else if (mode == ModeStream) {
        if (m_liveSnapshot.hasData) {
            // Same reasoning as Capture's own restore just above — Live's
            // last-displayed data, not whatever Capture or Record most
            // recently left in the shared buffer. No feedSpectrum()/feedXY()
            // needed here: Instruments' Spectrum/XY are Capture-only, so
            // Live never has either active to restore. A channel currently
            // off on the rail is cleared regardless of whether it has
            // snapshot data — an explicit off is a deliberate choice that
            // should never be silently overridden by old data from while
            // it was on.
            double snapshotRate = 0.0;
            for (int ch = 0; ch < 4; ++ch) {
                const bool channelOn = m_channel[ch].mode->checkedId() != ChannelOff;
                if (m_liveSnapshot.valid[ch] && channelOn) {
                    m_view->setSamples(ch, m_liveSnapshot.samples[ch],
                                       m_liveSnapshot.sampleRate, -1.0);
                    m_nav->setSamples(ch, m_liveSnapshot.samples[ch]);
                    snapshotRate = m_liveSnapshot.sampleRate;
                } else {
                    m_view->clearChannel(ch);
                }
                if (m_liveSnapshot.voltsPerDiv[ch] > 0.0)
                    m_view->setVoltsPerDiv(ch, m_liveSnapshot.voltsPerDiv[ch]);
                {
                    const double page = m_channel[ch].voltsPage->currentData().toDouble();
                    const double pct = page > 0.0
                        ? m_liveSnapshot.positionVolts[ch] / page * 100.0 : 0.0;
                    setChannelPositionPercent(ch, pct);
                }
            }
            // Matches refreshLiveView()'s own formula exactly — reads
            // m_view->secondsPerDiv() AFTER applyTimebase() just above has
            // already set it to this mode's own restored value, not
            // whatever the previous mode left it at (moving this whole
            // restore block to after applyTimebase() is specifically what
            // fixed that). Without a correct value here, the axis fell
            // back to the trigger-centred branch (always 0 for Live, which
            // has no real trigger), which is exactly what "the grid is
            // centred on zero until I drag" was describing: dragging works
            // because panRequested calls refreshLiveView(), which does set
            // this correctly.
            if (m_liveLoaded) {
                // A loaded file's snapshot is the whole saved buffer, not a
                // single ring window, so the ring-window origin formula
                // below does not apply to it; it keeps its own axis and pan.
                if (m_liveLoadedHasOrigin)
                    m_view->setAbsoluteTimeOrigin(m_liveLoadedOrigin);
                else
                    m_view->clearAbsoluteTimeOrigin();
                m_view->setViewCentreFraction(m_liveLoadedCentre);
            } else if (snapshotRate > 0.0) {
                const double perDiv = m_view->secondsPerDiv();
                const double wanted = perDiv * ScopeView::HorizontalDivs * snapshotRate;
                m_view->setAbsoluteTimeOrigin(
                    -(wanted + double(m_liveViewOffset)) / snapshotRate);
            }
            syncNavigator();
        } else {
            // Never run yet this session — an empty plot, not whatever
            // Capture or Record happened to leave on screen.
            for (int ch = 0; ch < 4; ++ch)
                m_view->clearChannel(ch);
            m_nav->clearAll();
        }
    }
    refreshCards();
    statusBar()->showMessage(streaming
        ? QStringLiteral("Stream: continuous acquisition, no trigger")
        : QStringLiteral("Capture: triggered snapshots"), 3000);

    // AcquisitionManager::stop() (just called above, inside stopAcquisition())
    // only stops our own polling timer — it tells the SDK nothing, so every
    // background thread the SDK spun up for block-mode capture is still
    // fully alive. Pressing Run immediately hands control to
    // StreamEngine::start(), whose SetOscCaptureMode() call tears down and
    // rebuilds the device object internally — while those leftover threads
    // may still be touching that same object. A brief, non-blocking window
    // (the Run button just stays disabled; nothing here blocks the UI) gives
    // the SDK's own threads a moment to settle before that specific,
    // apparently not fully thread-safe transition happens. This is a
    // mitigation for a suspected race inside the vendor's closed-source SDK,
    // not a confirmed fix — if it still happens, that's a genuinely useful
    // result: it would mean the failure isn't timing-sensitive after all.
    if (previousMode == ModeCapture && mode == ModeStream) {
        m_runCard->setEnabled(false);
        QTimer::singleShot(400, this, [this] {
            if (m_acquisitionMode == ModeStream)
                m_runCard->setEnabled(true);
        });
    }
    // Every channel's coupling and range, sent again for the mode now on
    // screen: block mode (Capture) and stream mode (Live/Record) keep their
    // own, a mode switch resets them, and a coupling switch resets the
    // range — applyChannel() sends coupling first, then the range.
    for (int ch = 0; ch < m_channelCount; ++ch)
        applyChannel(ch);
    updateZoomHistoryButtons();
    syncAnnotationContext();
}

void MainWindow::startAcquisition()
{
    VSX_TRACE("startAcquisition");
    leaveFileView();     // acquiring again: the user's own setup
    // Whatever suppressed samplesReady during a native waveform's own
    // restoration (see m_suppressSamplesReady's own comment) no longer
    // applies once a new acquisition is explicitly requested — this is a
    // well-defined moment to resume normal processing, unlike guessing how
    // long the prior channel reconfiguration's own hardware calls might
    // still take to settle.
    m_suppressSamplesReady = false;
    if (m_acquisitionMode == ModeStream)
        m_liveLoaded = false;

    if (m_acquisitionMode == ModeCapture) {
        m_acq->start();
        setRunning(true);
        return;
    }

    if (!m_acq->ready())
        return;

    m_stream->attach(m_acq->deviceId(), m_acq->channels());
    for (int ch = 0; ch < m_acq->channels(); ++ch)
        applyChannel(ch);

    unsigned short mask = 0;
    for (int ch = 0; ch < 4; ++ch) {
        if (m_channel[ch].mode->checkedId() != ChannelOff)
            mask = static_cast<unsigned short>(mask | (1u << ch));
    }
    if (mask == 0)
        mask = 0x0001;

    const int updates = m_updates->currentData().toInt();

    if (m_acquisitionMode == ModeRecord) {
        // Two bytes per sample is how the vendor counts a record, shared
        // across the channels being recorded.
        const int active = std::max(1, __builtin_popcount(unsigned(mask)));
        const quint64 bytes = quint64(m_recordSize->currentData().toInt()) * 1024ull * 1024ull;
        const quint64 perChannel = bytes / 2ull / quint64(active);

        // A fresh, uniquely-named subdirectory every time, never the base
        // record directory itself directly — this is what actually keeps
        // one recording from overwriting the last, on top of RecordWriter
        // now only ever writing a file for a channel that is on.
        const QString recDir = RecordWriter::newRecordingDirectory(recordDirectory());

        // The scope's driver (libvmmultmso) crashes recording two channels at
        // 62.5 MS/s: "pure virtual method called" in a streaming thread of its
        // own, nothing of ours on the stack (user's gdb backtrace). One
        // channel is fine. Two channels: the next rate down.
        constexpr unsigned int kTwoChannelRecordMax = 31250000;
        unsigned int recordRate = m_acq->sampleRate();
        if (active >= 2 && recordRate > kTwoChannelRecordMax) {
            recordRate = kTwoChannelRecordMax;
            statusBar()->showMessage(QStringLiteral(
                "Recording two channels at 31.25 MS/s: the scope's driver crashes at 62.5 MS/s "
                "with both channels on (one channel can use 62.5 MS/s)"), 10000);
        }

        m_recordViewStart = 0;
        m_recordViewLength = 0;
        m_recordRefresh.start();
        if (!m_stream->startRecord(recordRate, mask, updates,
                                   perChannel, recDir))
            return;
        if (m_recordPersistGroup && m_recordPersistGroup->checkedId() == 0)
            m_temporaryRecordings.append(recDir);
        // Same save-before-switching loadRecordTab() does for browsing —
        // a new recording starting is also a switch of what is currently
        // being viewed, away from whatever was on screen before.
        if (!m_currentRecordTab.isEmpty()) {
            RecordTabViewState &state = m_recordTabStates[m_currentRecordTab];
            state.viewStart = m_recordViewStart;
            state.viewLength = m_recordViewLength;
            for (int ch = 0; ch < 4; ++ch) {
                state.channels[ch].voltsPerDiv = m_view->voltsPerDiv(ch);
                state.channels[ch].positionVolts = m_view->positionVolts(ch);
                state.channels[ch].visible = m_view->channelVisible(ch);
            }
        }
        m_currentRecordTab = recDir;
        syncAnnotationContext();
        if (m_recordTabs) {
            // Disabled, not just refreshed: switching to a different past
            // recording while this one is actively being written would mean
            // pulling its files out from under the writer thread.
            m_recordTabs->setEnabled(false);
            refreshRecordTabs();
        }
    } else {
        const double history = m_history->currentData().toDouble();
        m_liveViewOffset = 0;
        m_liveOverviewRefresh.start();
        if (!m_stream->start(m_acq->sampleRate(), mask, updates, history))
            return;                   // the failed() signal explains why
        m_freezeButton->setEnabled(true);
    }

    m_streamTick->start();
    setRunning(true);
    m_streamStartClock.restart();
}

void MainWindow::stopAcquisition()
{
    VSX_TRACE("stopAcquisition");
    if (m_acquisitionMode != ModeCapture) {
        const bool wasRecording = m_stream->recording();
        m_streamTick->stop();
        m_stream->stop();
        m_freezeButton->setEnabled(false);
        m_freezeButton->setChecked(false);

        // A record stopped early is still a record: show what was captured.
        if (wasRecording && m_stream->recorded() > 0) {
            m_recordViewStart = 0;
            m_recordViewLength = m_stream->recorded();
            refreshRecordView();
        }
        if (wasRecording && m_recordTabs) {
            m_recordTabs->setEnabled(true);
            refreshRecordTabs();
        }
    } else {
        m_acq->stop();
    }
    setRunning(false);
    // A guided cranking test ends when its recording does — its 10 s timer
    // or the person pressing stop once the engine runs.
    if (m_guidedWaiting && m_guidedTest == GuidedCranking)
        QTimer::singleShot(0, this, [this] { finishCrankingTest(); });
    if (m_guidedWaiting && m_guidedTest == GuidedDrain)
        QTimer::singleShot(0, this, [this] { finishDrainTest(); });
}

void MainWindow::startDrainTest()
{
    const QString title = QStringLiteral("Parasitic drain test");
    // The button doubles as "Stop and analyse" while the test runs.
    if (m_guidedWaiting && m_guidedTest == GuidedDrain) {
        stopAcquisition();
        return;
    }
    if (!m_acq->ready()) {
        m_view->setTestPanel(title, { QStringLiteral("Connect the scope first") }, { true });
        return;
    }
    const int ch = m_drainChannel->currentData().toInt();
    if (ch >= m_channelCount) {
        m_view->setTestPanel(title, { QStringLiteral("This scope has no CH%1").arg(ch + 1) }, { true });
        return;
    }
    const Probe &clamp = m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                                 std::max(0, m_probes->count() - 1)));
    if (clamp.unit != QLatin1String("A")) {
        m_view->setTestPanel(title, { QStringLiteral(
            "Set CH%1's probe to your current clamp (a probe in amps)").arg(ch + 1) }, { true });
        return;
    }
    m_guidedWaiting = false;
    if (m_running)
        stopAcquisition();
    if (m_acquisitionMode != ModeRecord) {
        if (m_acqMode) {
            if (auto *button = m_acqMode->button(ModeRecord))
                button->setChecked(true);
        }
        setAcquisitionMode(ModeRecord);
    }
    // The clamp channel on, DC; every other channel off (a smaller recording).
    for (int other = 0; other < m_channelCount; ++other) {
        const int want = other == ch ? ChannelManual : ChannelOff;
        if ((m_channel[other].mode->checkedId() == ChannelOff) != (want == ChannelOff)) {
            if (auto *b = m_channel[other].mode->button(want))
                b->setChecked(true);
            emit m_channel[other].mode->idClicked(want);
        }
    }
    m_channel[ch].coupling->setCurrentIndex(std::max(0, m_channel[ch].coupling->findData(0)));

    // 10 kS/s catches brief wake-ups; 1 kS/s beyond 2 hours keeps it small.
    m_drainSeconds = m_drainDuration->currentData().toInt();
    const double wantRate = m_drainSeconds <= 7200 ? 10000.0 : 1000.0;
    {
        int best = -1;
        double bestDiff = 1e300;
        for (int i = 0; i < m_recordRate->count(); ++i) {
            const double d = std::fabs(std::log(m_recordRate->itemData(i).toDouble() / wantRate));
            if (d < bestDiff) { bestDiff = d; best = i; }
        }
        if (best >= 0)
            m_recordRate->setCurrentIndex(best);
    }
    // A record size that holds the whole test (two bytes per sample).
    const double rate = m_recordRate->currentData().toDouble();
    const double neededMb = rate * m_drainSeconds * 2.0 * 1.05 / (1024.0 * 1024.0);
    int size = m_recordSize->count() - 1;
    for (int i = 0; i < m_recordSize->count(); ++i) {
        if (m_recordSize->itemData(i).toDouble() >= neededMb) {
            size = i;
            break;
        }
    }
    m_recordSize->setCurrentIndex(size);

    m_guidedTest = GuidedDrain;
    m_guidedWaiting = true;
    m_drainStartMs = QDateTime::currentMSecsSinceEpoch();
    m_drainButton->setText(QStringLiteral("Stop and analyse"));
    startAcquisition();
    m_drainTimer->start(m_drainSeconds * 1000);
    m_drainTick->start();
    updateDrainProgress();
}

void MainWindow::updateDrainProgress()
{
    if (!m_guidedWaiting || m_guidedTest != GuidedDrain) {
        m_drainTick->stop();
        return;
    }
    const int ch = m_drainChannel->currentData().toInt();
    const Probe &clamp = m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                                 std::max(0, m_probes->count() - 1)));
    const double rate = m_stream->sampleRate();
    const quint64 total = m_stream->recorded();
    QStringList lines;
    QVector<bool> warn;
    const qint64 elapsed = (QDateTime::currentMSecsSinceEpoch() - m_drainStartMs) / 1000;
    const auto clock = [](qint64 s) {
        return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0'))
                                         .arg(s % 60, 2, 10, QLatin1Char('0'));
    };
    if (rate > 0.0 && total > 0) {
        const quint64 n = std::min<quint64>(total, quint64(rate));
        QVector<float> raw;
        const int got = m_stream->read(ch, total - n, int(n), &raw);
        if (got > 0) {
            double sum = 0.0;
            for (int i = 0; i < got; ++i)
                sum += double(raw[i]);
            const double amps = clamp.apply(sum / got);
            lines << QStringLiteral("Now %1 mA").arg(amps * 1000.0, 0, 'f', 0);
            warn << false;
        }
    }
    lines << QStringLiteral("%1 of %2").arg(clock(elapsed), clock(m_drainSeconds));
    warn << false;
    lines << QStringLiteral("Lock the car and leave it; Stop and analyse ends early");
    warn << false;
    m_view->setTestPanel(QStringLiteral("Parasitic drain test"), lines, warn);
}

void MainWindow::finishDrainTest()
{
    if (!m_guidedWaiting || m_guidedTest != GuidedDrain)
        return;
    m_guidedWaiting = false;
    m_drainTimer->stop();
    m_drainTick->stop();
    m_drainButton->setText(QStringLiteral("Start drain test"));

    const QString title = QStringLiteral("Parasitic drain test");
    const int ch = m_drainChannel->currentData().toInt();
    const Probe &clamp = m_probes->at(std::clamp(m_channel[ch].probe->currentIndex(), 0,
                                                 std::max(0, m_probes->count() - 1)));
    const double rate = m_stream->sampleRate();
    const quint64 total = m_stream->recorded();
    if (rate <= 0.0 || total < quint64(rate) * 60) {
        m_view->setTestPanel(title, { QStringLiteral("Too short to judge \u2014 run it for at least a few minutes") },
                             { true });
        return;
    }
    // Per second: the average, and the highest 1 ms average (brief wake-ups).
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const quint64 perSecond = quint64(rate);
    const quint64 perMs = std::max<quint64>(1, perSecond / 1000);
    std::vector<double> mean, peak;
    QVector<float> raw;
    for (quint64 start = 0; start + perSecond <= total; ) {
        const quint64 seconds = std::min<quint64>(60, (total - start) / perSecond);
        const int got = m_stream->read(ch, start, int(seconds * perSecond), &raw);
        if (got <= 0)
            break;
        for (quint64 s = 0; s + 1 <= quint64(got) / perSecond; ++s) {
            double sum = 0.0, hi = -1e300;
            for (quint64 m = 0; m + perMs <= perSecond; m += perMs) {
                double block = 0.0;
                for (quint64 k = 0; k < perMs; ++k)
                    block += double(raw[int(s * perSecond + m + k)]);
                sum += block;
                hi = std::max(hi, clamp.apply(block / double(perMs)));
            }
            mean.push_back(clamp.apply(sum / double(perSecond)));
            peak.push_back(hi);
        }
        start += quint64(got) / perSecond * perSecond;
        if (quint64(got) < seconds * perSecond)
            break;
    }
    QApplication::restoreOverrideCursor();

    const BatteryTools::Drain drain = BatteryTools::analyseDrain(mean, peak);
    const BatteryTools::BatteryVerdict verdict = BatteryTools::judgeDrain(
        drain, m_drainLimit->value() / 1000.0, m_drainCapacity->value());
    QStringList lines { QString::fromStdString(verdict.headline) };
    QVector<bool> warn { verdict.warn };
    for (const BatteryTools::VerdictLine &l : verdict.lines) {
        lines << QString::fromStdString(l.text);
        warn << l.warn;
    }
    m_view->setTestPanel(title, lines, warn);
}

namespace {
QString zeroKey(int path, int ch, int coupling, int halfMv)
{
    return QStringLiteral("%1/%2/%3/%4").arg(path).arg(ch).arg(coupling).arg(halfMv);
}
// The hardware half-ranges the app asks for: the volts-per-page list for x1
// and x10 probes (other probes use the nearest calibrated range).
QVector<int> calibrationRangesMv(int limitMv);

// The pages, the geometric midpoints between them and the limit, ascending:
// fine enough to land on every hardware range. The scope has 15, about 1.4x
// apart and never closer than 1.38x (Range report); the pages alone (2-2.5x
// apart) left 7 of them uncalibrated — exactly the ones an input offset
// selects. Requests landing on a range already measured are skipped.
QVector<int> calibrationGridMv(int limitMv)
{
    QVector<int> pages = calibrationRangesMv(limitMv);
    std::sort(pages.begin(), pages.end());
    QVector<int> grid;
    for (int i = 0; i < pages.size(); ++i) {
        grid << pages[i];
        if (i + 1 < pages.size())
            grid << int(std::lround(std::sqrt(double(pages[i]) * double(pages[i + 1]))));
    }
    grid << limitMv;
    std::sort(grid.begin(), grid.end());
    grid.erase(std::unique(grid.begin(), grid.end()), grid.end());
    return grid;
}

QVector<int> calibrationRangesMv(int limitMv)
{
    QVector<int> ranges;
    for (double v : kVoltsPerPage) {
        for (double probe : { 1.0, 10.0 }) {
            const int mv = int(std::lround(v * 1000.0 / probe));
            if (mv >= 1 && mv <= limitMv && !ranges.contains(mv))
                ranges << mv;
        }
    }
    std::sort(ranges.begin(), ranges.end());
    return ranges;
}
}

// The scope serves a request with the smallest of its ranges that covers it,
// so that's the calibrated range to use: the smallest stored one at or above
// the request (keys are the ranges really used, read back from the ADC).
// Above every stored range: the largest. (It used to take the nearest on a
// log scale, which could be a range below the one in use.)
double MainWindow::zeroOffsetFor(int path, int ch, int coupling, int halfMv) const
{
    const QString prefix = QStringLiteral("%1/%2/%3/").arg(path).arg(ch).arg(coupling);
    int above = -1, largest = -1;
    double aboveValue = 0.0, largestValue = 0.0;
    for (auto it = m_zeroTable.constBegin(); it != m_zeroTable.constEnd(); ++it) {
        if (!it.key().startsWith(prefix))
            continue;
        const int mv = it.key().mid(prefix.size()).toInt();
        if (mv >= halfMv && (above < 0 || mv < above)) {
            above = mv;
            aboveValue = it.value();
        }
        if (mv > largest) {
            largest = mv;
            largestValue = it.value();
        }
    }
    return above >= 0 ? aboveValue : largestValue;
}

void MainWindow::applyZeroOffsets(int ch)
{
    if (ch < 0 || ch >= 4 || !m_acq || !m_stream)
        return;
    const bool on = m_zeroEnable && m_zeroEnable->isChecked() && !m_calibrating;
    const int half = m_appliedHalfMv[size_t(ch)], coupling = m_appliedCoupling[size_t(ch)];
    m_acq->setZeroOffset(ch, on ? zeroOffsetFor(0, ch, coupling, half) : 0.0);
    m_stream->setZeroOffset(ch, on ? zeroOffsetFor(1, ch, coupling, half) : 0.0);
}

void MainWindow::refreshCalibrationStatus()
{
    if (!m_calStatus)
        return;
    const QString when = QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
                             .value(QStringLiteral("zero/date")).toString();
    m_calStatus->setText(m_zeroTable.isEmpty()
        ? QStringLiteral("Not calibrated yet.")
        : QStringLiteral("%1 points, last updated %2.").arg(m_zeroTable.size()).arg(when));
}

void MainWindow::startCalibration(bool quick)
{
    const QString title = QStringLiteral("Zero calibration");
    if (m_calibrating)
        return;
    if (!m_acq->ready()) {
        m_view->setTestPanel(title, { QStringLiteral("Connect the scope first") }, { true });
        return;
    }
    if (!VsxDialogs::question(this, title,
            QStringLiteral("Disconnect everything from the scope's inputs (probes off "
                           "both channels), then press Yes.\n\nThis measures what the "
                           "inputs read with nothing connected, so a probe still on a "
                           "circuit would be measured as an offset.")))
        return;

    stopAcquisition();
    m_calQuick = quick;
    m_calModeBefore = m_acquisitionMode;
    m_calResults.clear();
    m_calSteps.clear();
    m_calRejectCount.fill(0);
    m_calCoverage.clear();
    m_calCoveredSkips = 0;
    const int limit = std::max(1, m_acq->rangeMaxMv());
    const int channels = std::min(m_channelCount, 4);
    if (quick) {
        const int path = m_acquisitionMode == ModeCapture ? 0 : 1;
        for (int ch = 0; ch < channels; ++ch)
            m_calSteps.push_back({ path, ch, m_appliedCoupling[size_t(ch)],
                                   std::max(1, m_appliedHalfMv[size_t(ch)]) });
    } else {
        const QVector<int> ranges = calibrationGridMv(limit);   // ascending
        for (int path : { 0, 1 })
            for (int ch = 0; ch < channels; ++ch)
                for (int coupling : { 0, 1 })
                    for (int mv : ranges)
                        m_calSteps.push_back({ path, ch, coupling, mv });
    }
    // Every channel on for the measurement (restored afterwards), and no
    // correction while measuring.
    for (int ch = 0; ch < channels; ++ch) {
        m_calChannelModes[size_t(ch)] = m_channel[ch].mode->checkedId();
        if (m_calChannelModes[size_t(ch)] == ChannelOff) {
            if (auto *b = m_channel[ch].mode->button(ChannelManual))
                b->setChecked(true);
            emit m_channel[ch].mode->idClicked(ChannelManual);
        }
    }
    m_calibrating = true;
    for (int ch = 0; ch < 4; ++ch)
        applyZeroOffsets(ch);   // zero while measuring
    m_calPath = -1;
    m_calIndex = 0;
    m_quickZeroButton->setEnabled(false);
    m_fullCalButton->setEnabled(false);
    runCalibrationStep();
}

void MainWindow::runCalibrationStep()
{
    if (!m_calibrating)
        return;
    m_calTimeout->stop();
    // A request at or below the largest range already measured for its
    // path / channel / coupling lands on that same range: nothing new.
    while (!m_calQuick && m_calIndex < m_calSteps.size()) {
        const CalStep &s = m_calSteps[m_calIndex];
        const QString combo = QStringLiteral("%1/%2/%3").arg(s.path).arg(s.ch).arg(s.coupling);
        if (m_calCoverage.value(combo, 0) < s.halfMv)
            break;
        ++m_calIndex;
        ++m_calCoveredSkips;
    }
    if (m_calIndex >= m_calSteps.size()) {
        finishCalibration();
        return;
    }
    const CalStep step = m_calSteps[m_calIndex];
    m_view->setTestPanel(QStringLiteral("Zero calibration"),
        { QStringLiteral("Measuring %1 of %2 \u2014 keep the inputs disconnected")
              .arg(m_calIndex + 1).arg(m_calSteps.size()),
          QStringLiteral("%1, CH%2, %3, \u00b1%4")
              .arg(step.path == 0 ? QStringLiteral("Capture") : QStringLiteral("Live / Record"))
              .arg(step.ch + 1).arg(step.coupling ? QStringLiteral("AC") : QStringLiteral("DC"))
              .arg(formatVolts(step.halfMv / 1000.0)) },
        { false, false });

    // The data path: Capture for path 0, Live streaming for path 1.
    if (step.path != m_calPath) {
        stopAcquisition();
        const int mode = step.path == 0 ? ModeCapture : ModeStream;
        if (m_acquisitionMode != mode) {
            if (m_acqMode) {
                if (auto *b = m_acqMode->button(mode))
                    b->setChecked(true);
            }
            setAcquisitionMode(mode);
        }
        m_calPath = step.path;
    }
    // The step's range and coupling straight to the hardware (the channel
    // pages are left as they are).
    if (step.path == 0) {
        m_acq->setAcCoupling(step.ch, step.coupling == 1);
        m_acq->setRangeMv(step.ch, -step.halfMv, step.halfMv);
    } else {
        // Streaming first: starting it re-applies the channels' own ranges,
        // which must not overwrite the step's.
        if (!m_running)
            startAcquisition();
        m_stream->setChannelCoupling(step.ch, step.coupling == 1);
        m_stream->setChannelRange(step.ch, -step.halfMv, step.halfMv);
    }
    // Let the input settle (a coupling change charges the AC capacitor),
    // then measure.
    m_calTimer->disconnect();
    const bool slow = m_calIndex == 0 || step.coupling == 1;
    m_calTimer->setInterval(step.path == 0 ? (slow ? 400 : 150) : (slow ? 700 : 400));
    connect(m_calTimer, &QTimer::timeout, this, [this, step] {
        if (!m_calibrating)
            return;
        m_calTimeout->start();
        if (step.path == 0) {
            m_acq->single();
            QTimer::singleShot(80, this, [this] {
                if (m_calibrating)
                    m_acq->forceTrigger();   // nothing connected: never wait for an edge
            });
        } else {
            const int n = std::max(1000, int(m_stream->sampleRate() * 0.1));
            QVector<float> block;
            const int got = m_stream->newest(step.ch, n, &block);
            if (got > 0) {
                double sum = 0.0, lo = double(block[0]), hi = lo;
                for (int i = 0; i < got; ++i) {
                    const double v = double(block[i]);
                    sum += v;
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                calibrationMeasured(sum / got, hi - lo);
            }
            // else: the timeout skips the step
        }
    });
    m_calTimer->start();
}

void MainWindow::calibrationMeasured(double volts, double peakToPeak)
{
    if (!m_calibrating || m_calIndex >= m_calSteps.size())
        return;
    const CalStep step = m_calSteps[m_calIndex];
    // The hardware range the scope really used (its own conversion, read
    // back): the key the offset is filed under, and what's now covered.
    int rangeMv = step.halfMv;
    {
        const AdcWindow w = step.path == 0 ? m_acq->adcWindow(step.ch) : m_stream->adcWindow(step.ch);
        if (w.valid)
            rangeMv = std::max(step.halfMv, int(std::lround(std::max(std::fabs(w.minV), std::fabs(w.maxV)) * 1000.0)));
        const QString combo = QStringLiteral("%1/%2/%3").arg(step.path).arg(step.ch).arg(step.coupling);
        m_calCoverage[combo] = std::max(m_calCoverage.value(combo, 0), rangeMv);
    }
    if (std::fabs(volts) > plausibleOffsetV(rangeMv) || peakToPeak > plausibleSwingV(rangeMv)) {
        // A signal, not an offset: not stored (the first sighting per
        // channel is kept for the warning at the end).
        if (m_calRejectCount[size_t(step.ch)]++ == 0) {
            m_calRejectMean[size_t(step.ch)] = volts;
            m_calRejectPtp[size_t(step.ch)] = peakToPeak;
        }
    } else {
        m_calResults.insert(zeroKey(step.path, step.ch, step.coupling, rangeMv), volts);
    }
    ++m_calIndex;
    m_calTimeout->stop();
    QTimer::singleShot(0, this, &MainWindow::runCalibrationStep);
}

void MainWindow::finishCalibration()
{
    m_calTimer->stop();
    m_calTimeout->stop();
    stopAcquisition();
    m_calibrating = false;
    // Merge (a quick zero updates just what it measured) and keep.
    for (auto it = m_calResults.constBegin(); it != m_calResults.constEnd(); ++it)
        m_zeroTable.insert(it.key(), it.value());
    saveZeroTable();
    if (!m_calResults.isEmpty())
        QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
            .setValue(QStringLiteral("zero/date"),
                      QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));

    // Back as it was: channel modes, mode, settings (with the new offsets).
    const int channels = std::min(m_channelCount, 4);
    for (int ch = 0; ch < channels; ++ch) {
        if (m_calChannelModes[size_t(ch)] == ChannelOff) {
            if (auto *b = m_channel[ch].mode->button(ChannelOff))
                b->setChecked(true);
            emit m_channel[ch].mode->idClicked(ChannelOff);
        }
    }
    if (m_acquisitionMode != m_calModeBefore) {
        if (m_acqMode) {
            if (auto *b = m_acqMode->button(m_calModeBefore))
                b->setChecked(true);
        }
        setAcquisitionMode(m_calModeBefore);
    }
    for (int ch = 0; ch < 4; ++ch)
        applyChannel(ch);
    m_quickZeroButton->setEnabled(true);
    m_fullCalButton->setEnabled(true);
    refreshCalibrationStatus();

    // The progress panel goes once it's done (results panels stay until
    // cleared, which suits test results, not this); the summary goes to the
    // status bar and stays listed under Zero calibration in Settings.
    m_view->setTestPanel(QString(), {}, {});
    int rejected = 0;
    for (int n : m_calRejectCount)
        rejected += n;
    // No data: neither stored, rejected nor skipped as an already-measured range.
    const int skipped = m_calSteps.size() - m_calResults.size() - rejected - m_calCoveredSkips;
    const int path = m_acquisitionMode == ModeCapture ? 0 : 1;
    QStringList offsets;
    for (int ch = 0; ch < channels; ++ch) {
        const double off = zeroOffsetFor(path, ch, m_appliedCoupling[size_t(ch)], m_appliedHalfMv[size_t(ch)]);
        offsets << QStringLiteral("CH%1 %2 mV").arg(ch + 1).arg(off * 1000.0, 0, 'f', 2);
    }
    const QString summary = QStringLiteral("Zero calibration done: %1 of %2 measured%3. Removed offsets now: %4")
        .arg(m_calResults.size()).arg(m_calSteps.size())
        .arg(QString(skipped > 0 ? QStringLiteral(" (%1 skipped, no data)").arg(skipped) : QString())
             + (rejected > 0 ? QStringLiteral(" (%1 rejected: signal connected)").arg(rejected) : QString()))
        .arg(offsets.join(QStringLiteral(", ")));
    statusBar()->showMessage(summary, 8000);
    if (m_calStatus)
        m_calStatus->setText(m_calStatus->text() + QStringLiteral("\nRemoved at the current settings: ")
                             + offsets.join(QStringLiteral(", ")) + QLatin1Char('.'));
    if (rejected > 0) {
        QStringList who;
        for (int ch = 0; ch < 4; ++ch) {
            if (m_calRejectCount[size_t(ch)] > 0)
                who << QStringLiteral("CH%1: %2 V average, %3 V swing (%4 settings)")
                           .arg(ch + 1).arg(m_calRejectMean[size_t(ch)], 0, 'f', 3)
                           .arg(m_calRejectPtp[size_t(ch)], 0, 'f', 3).arg(m_calRejectCount[size_t(ch)]);
        }
        VsxDialogs::warning(this, QStringLiteral("Zero calibration"),
            QStringLiteral("A signal is connected, so those settings were not calibrated:\n\n%1\n\n"
                           "An open input reads within a few millivolts. Disconnect the probes, including "
                           "from the scope's calibration output, and run it again.")
                .arg(who.join(QLatin1Char('\n'))));
    }
}

// ---- ADC range measurement ------------------------------------------------

double MainWindow::filterLowParam(int ch) const
{
    if (ch < 0 || ch >= 4)
        return 0.0;
    const ChannelControls &c = m_channel[ch];
    if (c.filterType && c.filterHighResN && c.filterType->currentData().toInt() == ScopeView::FilterHighRes)
        return c.filterHighResN->currentData().toDouble();
    return c.filterLowHz ? c.filterLowHz->value() : 0.0;
}

QVector<double> MainWindow::averageFrame(int ch, const QVector<double> &samples, double fs, double *trigger)
{
    const int n = m_average ? m_average->currentData().toInt() : 1;
    if (n <= 1 || ch < 0 || ch >= 4 || samples.isEmpty())
        return samples;
    if (m_trigModeGroup->checkedId() == ModeNone) {
        // Free-running frames start anywhere: averaged, they'd blur away.
        if (!m_avgNoTriggerShown) {
            statusBar()->showMessage(QStringLiteral(
                "Averaging needs a trigger: free-running frames don't line up"), 6000);
            m_avgNoTriggerShown = true;
        }
        return samples;
    }
    FrameAverage &a = m_avg[size_t(ch)];
    if (a.generation != m_avgGeneration || a.acc.size() != samples.size() || a.rate != fs || a.count == 0) {
        a.acc = samples;
        a.count = 1;
        a.trigger = *trigger;
        a.rate = fs;
        a.generation = m_avgGeneration;
        return samples;
    }
    // Line the new frame up with the average by their trigger points.
    const long long shift = std::llround(*trigger - a.trigger);
    a.count = std::min(a.count + 1, n);
    const double weight = 1.0 / a.count;     // a plain mean up to N, then exponential
    const int size = samples.size();
    for (int j = 0; j < size; ++j) {
        const long long i = j + shift;
        if (i >= 0 && i < size)
            a.acc[j] += (samples[int(i)] - a.acc[j]) * weight;
    }
    *trigger = a.trigger;
    return a.acc;
}

QString MainWindow::adcWindowText(const AdcWindow &w) const
{
    if (!w.valid)
        return QStringLiteral("Shown once data arrives.");
    const double perCode = std::fabs(w.zoom);
    const double used = w.codesOnScreen();
    return QStringLiteral("%1-bit. Requested %2 to %3 V; the ADC covers %4 to %5 V at %6 per code. "
                          "The screen gets %7 of %8 codes (%9 %).")
        .arg(w.bits)
        .arg(w.requestedMinMv / 1000.0, 0, 'f', 3).arg(w.requestedMaxMv / 1000.0, 0, 'f', 3)
        .arg(w.minV, 0, 'f', 3).arg(w.maxV, 0, 'f', 3)
        .arg(perCode >= 1e-3 ? QStringLiteral("%1 mV").arg(perCode * 1e3, 0, 'g', 3)
                             : QStringLiteral("%1 \u00b5V").arg(perCode * 1e6, 0, 'g', 3))
        .arg(qlonglong(std::llround(used))).arg(w.codes())
        .arg(100.0 * used / w.codes(), 0, 'f', 0);
}

// Once a second, for the channel page on view: Capture from the last frame's
// conversion, Live / Record from the stream's.
void MainWindow::updateAdcInfo()
{
    if (!m_channelPages)
        return;
    // While streaming, a channel whose real window is far bigger than the
    // range sent has stopped following it (user: CH1 in Live stuck on
    // +-2.08 V for a +-50 mV page; cured by toggling the coupling). The
    // scope's ranges are at most ~2x apart (Range report), so more than 2.5x
    // means stuck. Reset once per stuck setting, so it can't loop.
    // Not just after a stream (re)start: the SDK is still settling then.
    const bool streamSettled = m_streamStartClock.isValid() && m_streamStartClock.elapsed() > 1500;
    if (m_acquisitionMode != ModeCapture && m_running && !m_liveLoaded && streamSettled) {
        for (int ch = 0; ch < std::min(m_channelCount, 4); ++ch) {
            if (m_channel[ch].mode->checkedId() == ChannelOff)
                continue;
            const AdcWindow sw = m_stream->adcWindow(ch);
            if (!sw.valid)
                continue;
            const double sentHalf = std::max(std::abs(sw.sentMinMv), std::abs(sw.sentMaxMv)) / 1000.0;
            const double actualHalf = std::max(std::fabs(sw.minV), std::fabs(sw.maxV));
            // Below the scope's smallest range (+-145 mV measured) a bigger
            // window is normal, not stuck: compare against at least 0.2 V.
            const double expectedHalf = std::max(sentHalf, 0.2);
            if (sentHalf <= 0.0 || actualHalf <= 2.5 * expectedHalf)
                continue;
            const QString key = QStringLiteral("%1/%2/%3").arg(sw.sentMinMv).arg(sw.sentMaxMv)
                                    .arg(m_appliedCoupling[size_t(ch)]);
            if (m_frontEndResetKey[size_t(ch)] == key)
                continue;                                   // already tried for this setting
            m_frontEndResetKey[size_t(ch)] = key;
            m_stream->resetChannelFrontEnd(ch);
            statusBar()->showMessage(QStringLiteral(
                "CH%1: the scope wasn't following its input range (it sat on \u00b1%2 V); "
                "its input was reset").arg(ch + 1).arg(actualHalf, 0, 'f', 2), 6000);
        }
    }
    const int ch = m_channelPages->currentIndex();
    if (ch < 0 || ch >= 4 || !m_channel[ch].adcInfo)
        return;
    AdcWindow w;
    if (m_acquisitionMode == ModeCapture)
        w = m_acq->adcWindow(ch);
    else if (m_running && !m_liveLoaded && streamSettled)
        w = m_stream->adcWindow(ch);
    QString text = adcWindowText(w);
    if (w.valid) {
        // Which coupling, and whether the scope refused the range: AC-coupled
        // CH1 in Live sat on +-2 V whatever the page (user report) while DC
        // CH2 followed it — these two say which of the possible causes it is.
        text.prepend(m_appliedCoupling[size_t(ch)] ? QStringLiteral("AC-coupled. ")
                                                    : QStringLiteral("DC-coupled. "));
        if (w.refused())
            text += QStringLiteral(" The scope refused that range, so %1 to %2 V was sent instead.")
                        .arg(w.sentMinMv / 1000.0, 0, 'f', 3).arg(w.sentMaxMv / 1000.0, 0, 'f', 3);
    }
    m_channel[ch].adcInfo->setText(text);
}

void MainWindow::startRangeReport()
{
    const QString title = QStringLiteral("Range report");
    if (m_rrIndex >= 0 || m_calibrating)
        return;
    if (!m_acq->ready()) {
        m_view->setTestPanel(title, { QStringLiteral("Connect the scope first") }, { true });
        return;
    }
    QVector<int> channels;
    for (int ch = 0; ch < std::min(m_channelCount, 4); ++ch) {
        if (m_channel[ch].mode->checkedId() != ChannelOff)
            channels << ch;
    }
    if (channels.isEmpty()) {
        m_view->setTestPanel(title, { QStringLiteral("Turn on at least one channel") }, { true });
        return;
    }
    stopAcquisition();
    m_rrModeBefore = m_acquisitionMode;
    if (m_acquisitionMode != ModeCapture) {
        if (m_acqMode) {
            if (auto *b = m_acqMode->button(ModeCapture))
                b->setChecked(true);
        }
        setAcquisitionMode(ModeCapture);
    }
    // Every page's window centred on 0 V, then shifted up by half and by a
    // whole page — whether the scope can really offset its window, and by
    // how much, is what this finds out.
    const int limit = std::max(1, m_acq->rangeMaxMv());
    m_rrSteps.clear();
    for (int ch : channels) {
        for (int half : calibrationRangesMv(limit)) {
            for (double shift : { 0.0, 0.5, 1.0 }) {
                const int centre = int(std::lround(shift * half));
                const int lo = std::clamp(centre - half, -limit, limit);
                const int hi = std::clamp(centre + half, -limit, limit);
                if (hi - lo < half)            // pushed against the limit: not a real test
                    continue;
                m_rrSteps.push_back({ ch, lo, hi });
            }
        }
    }
    m_rrRows.clear();
    m_rrRows << QStringLiteral("VimuScopeX range report \u2014 %1-bit ADC, Capture mode, %2 ch, limit \u00b1%3 mV")
                    .arg(m_acq->adcWindow(channels.first()).bits > 0 ? m_acq->adcWindow(channels.first()).bits : 12)
                    .arg(m_channelCount).arg(limit);
    m_rrRows << QStringLiteral("ch  requested (mV)         ADC window (mV)            per code     screen codes   SDK zoom / bias / resolution");
    m_rrIndex = 0;
    m_rrPhase = 0;
    m_rangeReportButton->setEnabled(false);
    m_rrTimer->start();
}

void MainWindow::rangeReportTick()
{
    if (m_rrIndex < 0)
        return;
    if (m_rrIndex >= m_rrSteps.size()) {
        finishRangeReport();
        return;
    }
    const RangeStep step = m_rrSteps[m_rrIndex];
    const auto next = [this] { ++m_rrIndex; m_rrPhase = 0; };
    switch (m_rrPhase) {
    case 0:   // apply the range, let it settle
        m_view->setTestPanel(QStringLiteral("Range report"),
            { QStringLiteral("Step %1 of %2 \u2014 CH%3, %4 to %5 mV")
                  .arg(m_rrIndex + 1).arg(m_rrSteps.size()).arg(step.ch + 1).arg(step.minMv).arg(step.maxMv) },
            { false });
        m_acq->setRangeMv(step.ch, step.minMv, step.maxMv);
        m_rrClock.restart();
        m_rrPhase = 1;
        break;
    case 1:   // one capture
        if (m_rrClock.elapsed() < 120)
            break;
        m_rrSerial = m_acq->frameSerial();
        m_rrForced = false;
        m_acq->single();
        m_rrClock.restart();
        m_rrPhase = 2;
        break;
    default:  // its frame, or give up
        if (!m_rrForced && m_rrClock.elapsed() > 80) {
            m_acq->forceTrigger();               // never wait for an edge
            m_rrForced = true;
        }
        if (m_acq->frameSerial() != m_rrSerial) {
            const AdcWindow w = m_acq->adcWindow(step.ch);
            if (w.valid) {
                const double perCode = std::fabs(w.zoom);
                m_rrRows << QStringLiteral("%1   %2 .. %3   %4 .. %5   %6   %7 / %8 (%9 %)   %10 / %11 / %12")
                                .arg(step.ch + 1)
                                .arg(step.minMv, 7).arg(step.maxMv, -7)
                                .arg(w.minV * 1e3, 9, 'f', 1).arg(w.maxV * 1e3, -9, 'f', 1)
                                .arg(perCode >= 1e-3 ? QStringLiteral("%1 mV").arg(perCode * 1e3, 6, 'g', 4)
                                                     : QStringLiteral("%1 uV").arg(perCode * 1e6, 6, 'g', 4))
                                .arg(qlonglong(std::llround(w.codesOnScreen())), 4).arg(w.codes())
                                .arg(100.0 * w.codesOnScreen() / w.codes(), 3, 'f', 0)
                                .arg(w.zoom, 0, 'g', 6).arg(w.bias, 0, 'g', 6).arg(w.sdkResolution, 0, 'g', 6);
            } else {
                m_rrRows << QStringLiteral("%1   %2 .. %3   (no conversion reported)").arg(step.ch + 1)
                                .arg(step.minMv).arg(step.maxMv);
            }
            next();
        } else if (m_rrClock.elapsed() > 1500) {
            m_rrRows << QStringLiteral("%1   %2 .. %3   (no frame)").arg(step.ch + 1).arg(step.minMv).arg(step.maxMv);
            next();
        }
        break;
    }
}

void MainWindow::finishRangeReport()
{
    m_rrTimer->stop();
    m_rrIndex = -1;
    m_rangeReportButton->setEnabled(true);
    // Back to the user's own ranges and mode.
    for (int ch = 0; ch < 4; ++ch)
        applyChannel(ch);
    if (m_rrModeBefore != m_acquisitionMode) {
        if (m_acqMode) {
            if (auto *b = m_acqMode->button(m_rrModeBefore))
                b->setChecked(true);
        }
        setAcquisitionMode(m_rrModeBefore);
    }
    m_view->setTestPanel(QString(), {}, {});
    const QString text = m_rrRows.join(QLatin1Char('\n'));
    QGuiApplication::clipboard()->setText(text);

    VsxDialog dialog(this, QStringLiteral("Range report (copied to the clipboard)"));
    QVBoxLayout *layout = dialog.contentLayout();
    auto *view = new QPlainTextEdit(text);
    view->setReadOnly(true);
    view->setLineWrapMode(QPlainTextEdit::NoWrap);
    view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    layout->addWidget(view);
    dialog.addButton(QStringLiteral("Close"), true, [&dialog] { dialog.reject(); });
    dialog.resize(1000, 560);
    dialog.exec();
}

void MainWindow::saveZeroTable() const
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    settings.remove(QStringLiteral("zero/table"));
    settings.beginGroup(QStringLiteral("zero/table"));
    for (auto it = m_zeroTable.constBegin(); it != m_zeroTable.constEnd(); ++it)
        settings.setValue(QString(it.key()).replace(QLatin1Char('/'), QLatin1Char('_')), it.value());
    settings.endGroup();
}

QString MainWindow::annotationContext() const
{
    switch (m_acquisitionMode) {
    case ModeCapture: return QStringLiteral("capture");
    case ModeStream:  return QStringLiteral("live");
    default:          return QStringLiteral("record:") + m_currentRecordTab;
    }
}

// The outgoing context's annotations kept, the incoming one's shown.
void MainWindow::syncAnnotationContext()
{
    if (!m_annotationsReady || !m_view)
        return;
    const QString next = annotationContext();
    if (next == m_annotationContext)
        return;
    m_annotationSets[m_annotationContext] = m_view->annotationsState();
    m_view->clearAnnotations();
    m_annotationContext = next;
    m_view->restoreAnnotations(m_annotationSets.value(next));
}

void MainWindow::refreshMathSources(bool apply)
{
    // Before a scope or file says otherwise: two, the MSO21AL's count.
    const int n = std::clamp(m_channelCount > 0 ? m_channelCount : 2, 1, 4);
    bool changed = false;
    for (QComboBox *box : { m_mathSourceA, m_mathSourceB }) {
        if (!box || box->count() == n)
            continue;
        const int keep = box->count() > 0 ? box->currentData().toInt() : (box == m_mathSourceB ? 1 : 0);
        box->blockSignals(true);
        box->clear();
        for (int ch = 0; ch < n; ++ch)
            box->addItem(QStringLiteral("CH%1").arg(ch + 1), ch);
        const int i = box->findData(keep);
        box->setCurrentIndex(i >= 0 ? i : (box == m_mathSourceB ? std::min(1, n - 1) : 0));
        box->blockSignals(false);
        changed = true;
    }
    if (changed && apply)
        applyMathChannel();
}

void MainWindow::applyMathChannel()
{
    const bool on = m_mathEnabled->isChecked();
    const auto op = static_cast<ScopeView::MathOp>(m_mathOp->currentData().toInt());
    const int sourceA = m_mathSourceA->currentData().toInt();
    const int sourceB = m_mathSourceB->currentData().toInt();
    // k / c before the operation, so k.A + c is computed with them at once.
    if (m_mathScaleK && m_mathScaleC) {
        m_view->setMathScaleParams(m_mathScaleK->value(), m_mathScaleC->value());
        m_nav->setMathScaleParams(m_mathScaleK->value(), m_mathScaleC->value());
    }
    m_view->setMathChannel(on, op, sourceA, sourceB);
    m_nav->setMathChannel(on, m_mathOp->currentData().toInt(), sourceA, sourceB);
    // Only what the operation uses.
    if (m_mathSourceBRow)
        m_mathSourceBRow->setVisible(MathOps::usesB(int(op)));
    if (m_mathScaleRow)
        m_mathScaleRow->setVisible(op == ScopeView::MathOp::Scale);

    const double voltsPage = m_mathVoltsPage->currentData().toDouble();
    const double mathVoltsPerDiv = voltsPage > 0.0 ? divsFromPage(voltsPage) : -1.0;
    m_view->setMathVoltsPerDiv(mathVoltsPerDiv);
    m_nav->setMathScale(mathVoltsPerDiv, m_view->mathPositionVolts());

    m_mathCard->setVisible(on);
    if (on) {
        // A compact card only ever shows its (centred) title — the note is
        // hidden in that mode — so the operation has to go into the title
        // itself to actually be visible, rather than a note nobody sees.
        // The shared label (mathops.h) — e.g. "CH1 x CH2", "d(CH1)/dt".
        m_mathCard->setTitle(m_view->mathLabel());
        m_mathCard->setAccent(m_view->mathColor());
        // Its volts per page on the right, the operation on the left.
        m_mathCard->setNote(voltsPage > 0.0 ? m_mathVoltsPage->currentText() : QStringLiteral("Auto"));
    }
    refreshRulersChannelList();
    refreshMeasureChannelSelector();
    refreshSpectrumChannelSelector();
    refreshXYChannelSelectors();
    refreshCards();
}

// Shows/hides each channel's own checkbox based on whether that channel (or
// Math) is actually on right now — same "don't offer what doesn't apply"
// rule as before, just per-channel instead of one shared selector, since any
// number of channels can have their cursors on at once.
void MainWindow::refreshRulersChannelList()
{
    for (int ch = 0; ch < 4; ++ch) {
        QCheckBox *box = m_rulersChannelChecks[ch];
        if (!box)
            continue;
        const bool available = m_channel[ch].mode->checkedId() != ChannelOff;
        box->setVisible(available);
        if (!available && m_view)
            m_view->setRulersChannelEnabled(ch, false);
        // ScopeView is the source of truth (it may have auto-enabled the
        // first visible channel when rulers were first turned on) — the
        // checkbox just mirrors it, both ways.
        box->blockSignals(true);
        box->setChecked(available && m_view && m_view->rulersChannelEnabled(ch));
        box->blockSignals(false);
    }
    if (m_rulersMathCheck) {
        const bool available = m_mathEnabled && m_mathEnabled->isChecked();
        m_rulersMathCheck->setVisible(available);
        if (!available && m_view)
            m_view->setRulersChannelEnabled(ScopeView::RulersMathChannel, false);
        m_rulersMathCheck->blockSignals(true);
        m_rulersMathCheck->setChecked(available && m_view
            && m_view->rulersChannelEnabled(ScopeView::RulersMathChannel));
        m_rulersMathCheck->blockSignals(false);
    }
}

// A palette change alone does not reliably reach a checkbox's own label text
// once an application-level stylesheet is active — Qt's styled paint path can
// ignore palette roles for widget types the stylesheet touches at all, even
// for properties it never explicitly sets (theme.h's QCheckBox rule only sets
// spacing, yet setPalette() here was still overridden). A widget's own
// setStyleSheet() always takes precedence over the app-level one regardless
// of selector specificity, so that is what actually reaches the text color.
void MainWindow::setRulerCheckboxColor(QCheckBox *box, const QColor &colour)
{
    if (!box)
        return;
    box->setStyleSheet(QStringLiteral("QCheckBox { color: %1; }").arg(colour.name()));
}

// Automotive: the engine tools (cycle overlay, compression test, crank and
// cam sensors), moved here from Rulers, which had become crowded.
QWidget *MainWindow::buildAutomotivePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 10, 0);
    layout->setSpacing(4);

    // --- Engine: 720-degree cycle, cylinder overlay, compression test ---
    layout->addWidget(groupHeading(QStringLiteral("Engine"), true));
    m_engineEnable = new QCheckBox(QStringLiteral("Engine cycle overlay"));
    m_engineEnable->setToolTip(QStringLiteral(
        "Shades one band per cylinder with its number and TDC, adds a crank-angle "
        "scale, and shows the cursors' angle in the ruler box."));
    layout->addWidget(m_engineEnable);

    // Each setting under its own title, like every other page.
    layout->addWidget(sectionLabel(QStringLiteral("Cylinders")));
    m_engineCylinders = new QSpinBox;
    m_engineCylinders->setRange(1, 12);
    m_engineCylinders->setValue(4);
    layout->addWidget(m_engineCylinders);
    layout->addWidget(sectionLabel(QStringLiteral("Firing order")));
    m_engineFiringOrder = new QLineEdit(QStringLiteral("1-3-4-2"));
    m_engineFiringOrder->setToolTip(QStringLiteral(
        "Every cylinder once, in firing order: 1-3-4-2, 1 5 3 6 2 4 ..."));
    layout->addWidget(m_engineFiringOrder);
    layout->addWidget(sectionLabel(QStringLiteral("Cycle")));
    m_engineStroke = new QComboBox;
    m_engineStroke->addItem(QStringLiteral("4-stroke"), 720.0);
    m_engineStroke->addItem(QStringLiteral("2-stroke"), 360.0);
    m_engineStroke->setToolTip(QStringLiteral("4-stroke: a 720\u00b0 cycle; 2-stroke: 360\u00b0."));
    layout->addWidget(m_engineStroke);
    layout->addWidget(sectionLabel(QStringLiteral("Overlay style")));
    m_engineStyle = new QComboBox;
    m_engineStyle->addItem(QStringLiteral("Timing chart"), 1);
    m_engineStyle->addItem(QStringLiteral("Cylinder bands"), 0);
    m_engineStyle->setToolTip(QStringLiteral(
        "Timing chart: one row per cylinder in firing order, showing intake, "
        "compression, power and exhaust at every crank angle. Bands: one shaded "
        "band per cylinder event."));
    layout->addWidget(m_engineStyle);

    layout->addWidget(sectionLabel(QStringLiteral("Cycle from")));
    m_engineSource = new QComboBox;
    m_engineSource->addItem(QStringLiteral("Markers (drag on the plot)"), 0);
    m_engineSource->addItem(QStringLiteral("Sync signal on CH2"), 1);
    m_engineSource->addItem(QStringLiteral("Crank + cam sensors"), 2);
    layout->addWidget(m_engineSource);
    m_engineResetMarkers = new QPushButton(QStringLiteral("Reset markers"));
    m_engineResetMarkers->setToolTip(QStringLiteral(
        "Put the 0\u00b0 marker on cylinder 1 (TDC, or its compression peak) and the "
        "end marker one engine cycle later."));
    layout->addWidget(m_engineResetMarkers);

    m_engineSyncBox = new QWidget;
    {
        auto *l = new QVBoxLayout(m_engineSyncBox);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        l->addWidget(sectionLabel(QStringLiteral("Sync threshold and edge")));
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        m_engineSyncThreshold = new QDoubleSpinBox;
        m_engineSyncThreshold->setRange(-50.0, 50.0);
        m_engineSyncThreshold->setDecimals(2);
        m_engineSyncThreshold->setValue(2.5);
        m_engineSyncThreshold->setSuffix(QStringLiteral(" V"));
        m_engineSyncEdge = new QComboBox;
        m_engineSyncEdge->addItem(QStringLiteral("Rising"), true);
        m_engineSyncEdge->addItem(QStringLiteral("Falling"), false);
        rowLayout->addWidget(m_engineSyncThreshold, 1);
        rowLayout->addWidget(m_engineSyncEdge);
        l->addWidget(row);
        l->addWidget(sectionLabel(QStringLiteral("Sync event at")));
        m_engineSyncOffset = new QDoubleSpinBox;
        m_engineSyncOffset->setRange(-720.0, 720.0);
        m_engineSyncOffset->setDecimals(1);
        m_engineSyncOffset->setSuffix(QStringLiteral("\u00b0"));
        m_engineSyncOffset->setToolTip(QStringLiteral(
            "Crank angle of the sync edge relative to cylinder 1 TDC (negative = before "
            "TDC). Ignition fires a few degrees before TDC; a cam sensor's edge is "
            "wherever the engine puts it."));
        l->addWidget(m_engineSyncOffset);
    }
    layout->addWidget(m_engineSyncBox);

    // Crank + cam: the crank wheel's missing-tooth gap fixes the angle once
    // per revolution; the cam picks which revolution starts the cycle.
    m_crankBox = new QWidget;
    {
        auto *l = new QVBoxLayout(m_crankBox);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        l->addWidget(sectionLabel(QStringLiteral("Crank and cam channels")));
        auto *row = new QWidget;
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        m_crankChannel = new QComboBox;
        m_crankChannel->addItem(QStringLiteral("Crank CH1"), 0);
        m_crankChannel->addItem(QStringLiteral("Crank CH2"), 1);
        m_camChannel = new QComboBox;
        m_camChannel->addItem(QStringLiteral("Cam CH2"), 1);
        m_camChannel->addItem(QStringLiteral("Cam CH1"), 0);
        m_camChannel->addItem(QStringLiteral("No cam"), -1);
        m_camChannel->setToolTip(QStringLiteral(
            "Without a cam signal the crank gives the angle but not which of the two "
            "revolutions a cycle starts on (cylinder 1 vs its partner). Use No cam "
            "with a compression test on the other channel to calibrate TDC."));
        rowLayout->addWidget(m_crankChannel, 1);
        rowLayout->addWidget(m_camChannel, 1);
        l->addWidget(row);

        l->addWidget(sectionLabel(QStringLiteral("Crank wheel")));
        m_crankPattern = new QComboBox;
        m_crankPattern->addItem(QStringLiteral("Detect automatically"), 0);
        static const std::pair<int, int> kPatterns[] = {
            { 60, 2 }, { 36, 1 }, { 36, 2 }, { 30, 2 }, { 24, 1 }, { 12, 1 } };
        for (const auto &pattern : kPatterns) {
            m_crankPattern->addItem(QStringLiteral("%1-%2").arg(pattern.first).arg(pattern.second),
                                    pattern.first * 100 + pattern.second);
        }
        l->addWidget(m_crankPattern);

        l->addWidget(sectionLabel(QStringLiteral("Cylinder 1 TDC after the gap")));
        m_crankTdc = new QDoubleSpinBox;
        m_crankTdc->setRange(0.0, 719.9);
        m_crankTdc->setDecimals(1);
        m_crankTdc->setSuffix(QStringLiteral("\u00b0"));
        m_crankTdc->setToolTip(QStringLiteral(
            "Crank degrees from the first tooth after the gap (on the cam's revolution) "
            "to cylinder 1 TDC — from the service manual, or Calibrate."));
        m_crankCalibrate = new QPushButton(QStringLiteral("Calibrate"));
        m_crankCalibrate->setToolTip(QStringLiteral(
            "From a compression test with the crank on the other channel: aligns TDC "
            "exactly to the compression strokes. All cylinders compress alike, so this "
            "fixes TDC to within one firing interval and keeps your choice of which "
            "stroke is cylinder 1."));
        l->addWidget(m_crankTdc);
        l->addWidget(m_crankCalibrate);

        l->addWidget(sectionLabel(QStringLiteral("Cam edge spec and tolerance")));
        auto *specRow = new QWidget;
        auto *specLayout = new QHBoxLayout(specRow);
        specLayout->setContentsMargins(0, 0, 0, 0);
        m_camSpec = new QDoubleSpinBox;
        m_camSpec->setRange(-1.0, 360.0);
        m_camSpec->setDecimals(1);
        m_camSpec->setSuffix(QStringLiteral("\u00b0"));
        m_camSpec->setSpecialValueText(QStringLiteral("No spec"));
        m_camSpec->setValue(-1.0);
        m_camSpec->setToolTip(QStringLiteral(
            "Expected cam edge position, crank degrees after the gap. One tooth on a "
            "60-2 wheel is 6\u00b0."));
        m_camTolerance = new QDoubleSpinBox;
        m_camTolerance->setRange(0.5, 30.0);
        m_camTolerance->setDecimals(1);
        m_camTolerance->setValue(3.0);
        m_camTolerance->setPrefix(QStringLiteral("\u00b1"));
        m_camTolerance->setSuffix(QStringLiteral("\u00b0"));
        specLayout->addWidget(m_camSpec, 1);
        specLayout->addWidget(m_camTolerance);
        l->addWidget(specRow);

        m_camRemember = new QPushButton(QStringLiteral("Remember as good"));
        m_camRemember->setToolTip(QStringLiteral(
            "Store the cam angle measured now as this engine's known-good value; later "
            "captures show how far they are from it."));
        m_camForget = new QPushButton(QStringLiteral("Forget"));
        l->addWidget(m_camRemember);
        l->addWidget(m_camForget);
    }
    layout->addWidget(m_crankBox);

    layout->addWidget(groupHeading(QStringLiteral("Compression test")));
    m_compressionEnable = new QCheckBox(QStringLiteral("Relative compression"));
    m_compressionEnable->setToolTip(QStringLiteral(
        "Crank the engine with fuel or ignition disabled. Each compression stroke "
        "shows as a current peak (or battery-voltage dip); each cylinder is shown "
        "as a percentage of the strongest, averaged over every complete cycle."));
    layout->addWidget(m_compressionEnable);
    m_compressionBox = new QWidget;
    {
        auto *l = new QVBoxLayout(m_compressionBox);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        // Each under its own title (side by side, two fields overflowed the panel).
        m_compressionChannel = new QComboBox;
        m_compressionChannel->addItem(QStringLiteral("CH1"), 0);
        m_compressionChannel->addItem(QStringLiteral("CH2"), 1);
        m_compressionSignal = new QComboBox;
        m_compressionSignal->addItem(QStringLiteral("Current clamp"), 0);
        m_compressionSignal->addItem(QStringLiteral("Battery voltage"), 1);
        l->addWidget(sectionLabel(QStringLiteral("Channel")));
        l->addWidget(m_compressionChannel);
        l->addWidget(sectionLabel(QStringLiteral("Signal")));
        l->addWidget(m_compressionSignal);
        l->addWidget(sectionLabel(QStringLiteral("Flag cylinders below the strongest by")));
        m_compressionFlag = new QDoubleSpinBox;
        m_compressionFlag->setRange(1.0, 50.0);
        m_compressionFlag->setDecimals(0);
        m_compressionFlag->setValue(15.0);
        m_compressionFlag->setSuffix(QStringLiteral(" %"));
        l->addWidget(m_compressionFlag);
    }
    layout->addWidget(m_compressionBox);

    connect(m_engineEnable, &QCheckBox::toggled, this, [this](bool) { engineSettingsChanged(); });
    connect(m_compressionEnable, &QCheckBox::toggled, this, [this](bool) { engineSettingsChanged(); });
    connect(m_engineCylinders, &QSpinBox::valueChanged, this, [this](int) { engineSettingsChanged(); });
    connect(m_engineFiringOrder, &QLineEdit::textChanged, this, [this](const QString &) { engineSettingsChanged(); });
    for (QComboBox *box : { m_crankChannel, m_camChannel, m_crankPattern })
        connect(box, &QComboBox::currentIndexChanged, this, [this](int) { engineSettingsChanged(); });
    for (QDoubleSpinBox *box : { m_crankTdc, m_camSpec, m_camTolerance })
        connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { engineSettingsChanged(); });
    connect(m_crankCalibrate, &QPushButton::clicked, this, [this] {
        if (m_lastTdcCalibration < 0.0) {
            statusBar()->showMessage(QStringLiteral(
                "Calibrate needs a compression test running, with the crank sensor on the "
                "other channel."), 5000);
            return;
        }
        // Calibration knows TDC to within one firing interval; of its
        // candidates, take the one nearest the current value, which keeps
        // the person's choice of which stroke is cylinder 1.
        const double interval = m_engineStroke->currentData().toDouble() / m_engineCylinders->value();
        const double current = m_crankTdc->value();
        double best = m_lastTdcCalibration;
        for (double v = m_lastTdcCalibration; v < 720.0; v += interval) {
            if (std::fabs(v - current) < std::fabs(best - current))
                best = v;
        }
        m_crankTdc->setValue(best);
        statusBar()->showMessage(QStringLiteral("Cylinder 1 TDC set to %1\u00b0 after the gap")
                                     .arg(best, 0, 'f', 1), 4000);
    });
    connect(m_camRemember, &QPushButton::clicked, this, [this] {
        if (m_lastCamAngle < 0.0) {
            statusBar()->showMessage(QStringLiteral("No cam angle measured yet."), 3000);
            return;
        }
        m_camKnownGood = m_lastCamAngle;
        QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
            .setValue(QStringLiteral("engine/camKnownGood"), m_camKnownGood);
        statusBar()->showMessage(QStringLiteral("Known-good cam angle: %1\u00b0")
                                     .arg(m_camKnownGood, 0, 'f', 1), 3000);
        engineSettingsChanged();
    });
    connect(m_camForget, &QPushButton::clicked, this, [this] {
        m_camKnownGood = -1.0;
        QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
            .remove(QStringLiteral("engine/camKnownGood"));
        engineSettingsChanged();
    });
    for (QComboBox *box : { m_engineStroke, m_engineStyle, m_engineSource, m_engineSyncEdge,
                            m_compressionChannel, m_compressionSignal })
        connect(box, &QComboBox::currentIndexChanged, this, [this](int) { engineSettingsChanged(); });
    for (QDoubleSpinBox *box : { m_engineSyncThreshold, m_engineSyncOffset, m_compressionFlag })
        connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { engineSettingsChanged(); });
    connect(m_engineResetMarkers, &QPushButton::clicked, this, [this] {
        if (m_view)
            m_view->resetEngineMarkers();
        engineSettingsChanged();
    });

    // Cheap when nothing changed (a key compare); recomputes when the data,
    // a setting or a marker position did — markers follow a drag within a
    // tenth of a second.
    m_engineTimer = new QTimer(this);
    m_engineTimer->setInterval(100);
    connect(m_engineTimer, &QTimer::timeout, this, &MainWindow::updateEngineAnalysis);
    m_engineTimer->start();
    engineSettingsChanged();

    // --- Battery and charging: guided tests ---
    layout->addWidget(groupHeading(QStringLiteral("Battery and charging")));
    layout->addWidget(sectionLabel(QStringLiteral("Battery system")));
    m_batterySystem = new QComboBox;
    m_batterySystem->addItem(QStringLiteral("12 V"), 12.0);
    m_batterySystem->addItem(QStringLiteral("24 V"), 24.0);
    layout->addWidget(m_batterySystem);
    // One button per row: side by side, the panel is too narrow for both labels.
    {
        m_crankTestButton = new QPushButton(QStringLiteral("Cranking test"));
        m_crankTestButton->setToolTip(QStringLiteral(
            "CH1 on the battery terminals. Click, then crank the engine within 10 s: "
            "records 10 s and reports resting voltage, the lowest voltage while "
            "cranking, cranking time and the charging voltage once it runs."));
        m_chargeTestButton = new QPushButton(QStringLiteral("Charging test"));
        m_chargeTestButton->setToolTip(QStringLiteral(
            "CH1 and CH2 both on battery + (CH2 is set to AC for the ripple), engine "
            "idling with headlights, blower and rear demister on: reports charging "
            "voltage, ripple, and whether the ripple is even (diodes)."));
        layout->addWidget(m_crankTestButton);
        layout->addWidget(m_chargeTestButton);
    }
    layout->addWidget(sectionLabel(QStringLiteral("Battery temperature")));
    m_batteryTemperature = new QDoubleSpinBox;
    m_batteryTemperature->setRange(-40.0, 60.0);
    m_batteryTemperature->setDecimals(0);
    m_batteryTemperature->setValue(20.0);
    m_batteryTemperature->setSuffix(QStringLiteral(" \u00b0C"));
    m_batteryTemperature->setToolTip(QStringLiteral(
        "A cold battery holds less voltage under load: the cranking limit is lowered "
        "for cold weather (9.6 V at 21 \u00b0C down to 8.5 V at \u221218 \u00b0C, per the "
        "BCI load-test table)."));
    layout->addWidget(m_batteryTemperature);
    m_batteryClamp = new QCheckBox(QStringLiteral("Current clamp on CH2"));
    m_batteryClamp->setToolTip(QStringLiteral(
        "Adds the battery's internal resistance and an estimate of its cold cranking "
        "amps. Clamp around a battery cable, set CH2's probe to the clamp (amps) and a "
        "range that covers about 1000 A; probe CH1 right on the battery posts."));
    layout->addWidget(m_batteryClamp);
    layout->addWidget(sectionLabel(QStringLiteral("Rated CCA (battery label)")));
    m_batteryRatedCca = new QDoubleSpinBox;
    m_batteryRatedCca->setRange(0.0, 3000.0);
    m_batteryRatedCca->setDecimals(0);
    m_batteryRatedCca->setSingleStep(10.0);
    m_batteryRatedCca->setSuffix(QStringLiteral(" A"));
    m_batteryRatedCca->setSpecialValueText(QStringLiteral("Not set"));
    layout->addWidget(m_batteryRatedCca);
    {
        // Title above the field (they wrapped to three lines beside it).
        const auto limitRow = [&](const QString &label, QDoubleSpinBox **box, const QString &suffix) {
            layout->addWidget(sectionLabel(label));
            *box = new QDoubleSpinBox;
            (*box)->setRange(0.0, 60.0);
            (*box)->setDecimals(2);
            (*box)->setSingleStep(0.1);
            (*box)->setSuffix(suffix);
            layout->addWidget(*box);
        };
        limitRow(QStringLiteral("Resting voltage, charged from"), &m_limitRestingGood, QStringLiteral(" V"));
        limitRow(QStringLiteral("Cranking voltage, at least (21 \u00b0C)"), &m_limitCrankMin, QStringLiteral(" V"));
        limitRow(QStringLiteral("Charging voltage, from"), &m_limitChargeLow, QStringLiteral(" V"));
        limitRow(QStringLiteral("Charging voltage, up to"), &m_limitChargeHigh, QStringLiteral(" V"));
        limitRow(QStringLiteral("Ripple, at most"), &m_limitRipple, QStringLiteral(" Vpp"));
    }
    m_clearTestButton = new QPushButton(QStringLiteral("Clear results"));
    layout->addWidget(m_clearTestButton);
    applyBatterySystemLimits();

    m_guidedStopTimer = new QTimer(this);
    m_guidedStopTimer->setSingleShot(true);
    connect(m_guidedStopTimer, &QTimer::timeout, this, [this] {
        if (m_guidedWaiting && m_guidedTest == GuidedCranking)
            stopAcquisition();   // finishes the test (see stopAcquisition())
    });
    connect(m_batterySystem, &QComboBox::currentIndexChanged, this,
            [this](int) { applyBatterySystemLimits(); });
    connect(m_crankTestButton, &QPushButton::clicked, this, &MainWindow::startCrankingTest);
    connect(m_chargeTestButton, &QPushButton::clicked, this, &MainWindow::startChargingTest);

    layout->addWidget(groupHeading(QStringLiteral("Parasitic drain")));
    {
        auto *about = new QLabel(QStringLiteral(
            "The current the car draws once locked and asleep. Clamp a low-current (mA) "
            "clamp around the battery negative cable and set it as that channel's probe. "
            "Lock the car, latch the doors closed, keep the laptop awake and plugged in."));
        about->setWordWrap(true);
        m_drainChannel = new QComboBox;
        for (int ch = 0; ch < 2; ++ch)
            m_drainChannel->addItem(QStringLiteral("CH%1").arg(ch + 1), ch);
        m_drainChannel->setCurrentIndex(1);
        m_drainDuration = new QComboBox;
        for (const auto &d : { std::pair<const char *, int>{ "15 min", 900 }, { "30 min", 1800 },
                               { "1 hour", 3600 }, { "2 hours", 7200 }, { "4 hours", 14400 },
                               { "8 hours", 28800 } })
            m_drainDuration->addItem(QString::fromLatin1(d.first), d.second);
        m_drainDuration->setCurrentIndex(2);
        m_drainDuration->setToolTip(QStringLiteral(
            "Most cars sleep within 20-40 minutes; some take over an hour. Longer tests "
            "also catch modules that wake the car now and then."));
        m_drainCapacity = new QDoubleSpinBox;
        m_drainCapacity->setRange(10.0, 300.0);
        m_drainCapacity->setDecimals(0);
        m_drainCapacity->setValue(60.0);
        m_drainCapacity->setSuffix(QStringLiteral(" Ah"));
        m_drainLimit = new QDoubleSpinBox;
        m_drainLimit->setRange(5.0, 500.0);
        m_drainLimit->setDecimals(0);
        m_drainLimit->setValue(50.0);
        m_drainLimit->setSuffix(QStringLiteral(" mA"));
        m_drainLimit->setToolTip(QStringLiteral(
            "Most modern cars settle at 20-50 mA; check the manufacturer's figure if you have it."));
        for (QWidget *w : std::initializer_list<QWidget *>{ m_drainChannel, m_drainDuration,
                                                            m_drainCapacity, m_drainLimit })
            w->setMinimumWidth(40);
        layout->addWidget(sectionLabel(QStringLiteral("Clamp on")));
        layout->addWidget(m_drainChannel);
        layout->addWidget(sectionLabel(QStringLiteral("Duration")));
        layout->addWidget(m_drainDuration);
        layout->addWidget(sectionLabel(QStringLiteral("Battery capacity")));
        layout->addWidget(m_drainCapacity);
        layout->addWidget(sectionLabel(QStringLiteral("Acceptable drain")));
        layout->addWidget(m_drainLimit);
        m_drainButton = new QPushButton(QStringLiteral("Start drain test"));
        // The how-to: the button's tooltip, not a paragraph on the page.
        m_drainButton->setToolTip(about->text());
        delete about;
        layout->addWidget(m_drainButton);
        m_drainTimer = new QTimer(this);
        m_drainTimer->setSingleShot(true);
        m_drainTick = new QTimer(this);
        m_drainTick->setInterval(1000);
        connect(m_drainButton, &QPushButton::clicked, this, &MainWindow::startDrainTest);
        connect(m_drainTimer, &QTimer::timeout, this, [this] {
            if (m_guidedWaiting && m_guidedTest == GuidedDrain)
                stopAcquisition();   // finishes the test (see stopAcquisition())
        });
        connect(m_drainTick, &QTimer::timeout, this, &MainWindow::updateDrainProgress);
    }
    // One group for the bus panels (a heading per bus only repeated its one setting).
    layout->addWidget(groupHeading(QStringLiteral("Bus health")));
    m_canHealthEnable = new QCheckBox(QStringLiteral("CAN"));
    m_canHealthEnable->setToolTip(QStringLiteral(
        "CAN-H / CAN-L levels checked against the CAN standard (probe either wire or "
        "both; each channel is recognised automatically), plus bus load, errors and a "
        "table of every ID seen, from a CAN decoder in Decoding. Works on whatever is "
        "on screen, Capture or Record."));
    layout->addWidget(m_canHealthEnable);
    connect(m_canHealthEnable, &QCheckBox::toggled, this, [this](bool) {
        ++m_canHealthRev;
        updateCanHealth();
    });
    connect(m_engineTimer, &QTimer::timeout, this, &MainWindow::updateCanHealth);

    m_serialHealthEnable = new QCheckBox(QStringLiteral("LIN / K-Line / SENT"));
    m_serialHealthEnable->setToolTip(QStringLiteral(
        "For each LIN or K-Line decoder in Decoding: the wire's recessive (battery) and dominant "
        "levels, the bus's real bit rate (LIN: measured from every sync byte), frames or messages "
        "with their errors (ID parity, checksums, stop bits, breaks; KWP2000 length and "
        "checksum), unanswered LIN headers, K-Line timing and a table of what was seen."));
    layout->addWidget(m_serialHealthEnable);
    connect(m_serialHealthEnable, &QCheckBox::toggled, this, [this](bool) {
        ++m_serialHealthRev;
        updateSerialHealth();
    });
    connect(m_engineTimer, &QTimer::timeout, this, &MainWindow::updateSerialHealth);

    m_diagSummaryEnable = new QCheckBox(QStringLiteral("OBD-II / UDS"));
    m_diagSummaryEnable->setToolTip(QStringLiteral(
        "For each OBD-II / UDS decoder in Decoding: the latest live values per ECU, the VIN, "
        "fault codes with their descriptions, negative responses and how fast each ECU answered."));
    layout->addWidget(m_diagSummaryEnable);
    connect(m_diagSummaryEnable, &QCheckBox::toggled, this, [this](bool) {
        ++m_diagRev;
        updateDiagSummary();
    });
    connect(m_engineTimer, &QTimer::timeout, this, &MainWindow::updateDiagSummary);

    connect(m_clearTestButton, &QPushButton::clicked, this, [this] {
        m_guidedWaiting = false;
        m_guidedTest = GuidedNone;
        if (m_view)
            m_view->setTestPanel(QString(), {}, {});
    });

    layout->addStretch();

    // Never wider than the panel. A row's label plus its box ("Resting,
    // charged from" + "12.40 V", two combos side by side) can want more
    // width than the panel has, and a page wider than its scroll area slides
    // under the scroll bar. Labels wrap instead, and boxes, buttons and combos
    // may narrow (clipping their text before the layout ever overflows).
    for (QLabel *label : page->findChildren<QLabel *>())
        label->setWordWrap(true);
    for (QWidget *w : page->findChildren<QWidget *>()) {
        if (qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QComboBox *>(w)
            || qobject_cast<QPushButton *>(w) || qobject_cast<QLineEdit *>(w))
            w->setMinimumWidth(40);
    }

    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("panelScroll"));
    page->setObjectName(QStringLiteral("panelScrollPage"));
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#panelScroll, QWidget#qt_scrollarea_viewport,"
        " QWidget#panelScrollPage { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    return scroll;
}

QWidget *MainWindow::buildRemotePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_remoteEnable = new QCheckBox(QStringLiteral("Share to a phone or tablet"));
    layout->addWidget(m_remoteEnable);

    m_remoteQr = new QLabel;
    m_remoteQr->setAlignment(Qt::AlignCenter);
    // Sized to the code itself (see refreshRemote()): a fixed minimum shorter
    // than the code squeezed it, letting the text below spill over it, and
    // left an empty block while sharing was off.
    m_remoteQr->setVisible(false);
    layout->addWidget(m_remoteQr, 0, Qt::AlignHCenter);
    m_remoteUrl = new QLabel;
    m_remoteUrl->setWordWrap(true);
    m_remoteUrl->setAlignment(Qt::AlignCenter);
    m_remoteUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_remoteUrl);
    m_remoteStatus = new QLabel;
    m_remoteStatus->setWordWrap(true);
    m_remoteStatus->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_remoteStatus);

    layout->addWidget(sectionLabel(QStringLiteral("Network address")));
    m_remoteAddress = new QComboBox;
    m_remoteAddress->setToolTip(QStringLiteral(
        "This computer's addresses. Pick the one on the same network as the phone "
        "(on a phone's hotspot it's usually 192.168.x.x or 172.20.x.x)."));
    layout->addWidget(m_remoteAddress);
    layout->addWidget(sectionLabel(QStringLiteral("Port")));
    m_remotePort = new QSpinBox;
    m_remotePort->setRange(1024, 65535);
    m_remotePort->setValue(8765);
    layout->addWidget(m_remotePort);
    layout->addWidget(sectionLabel(QStringLiteral("Updates per second")));
    m_remoteRate = new QComboBox;
    for (int fps : { 5, 10, 15, 20, 25 })
        m_remoteRate->addItem(QString::number(fps), fps);
    m_remoteRate->setCurrentIndex(2);
    layout->addWidget(m_remoteRate);
    m_remoteNewKey = new QPushButton(QStringLiteral("New key (disconnect phones)"));
    m_remoteNewKey->setToolTip(QStringLiteral(
        "The address carries a random key so others on the same network can't watch "
        "or control the scope. A new key disconnects every phone; scan again."));
    layout->addWidget(m_remoteNewKey);
    auto *note = new QLabel(QStringLiteral(
        "Scan the code with the phone's camera: it opens in the browser, nothing to "
        "install. Phone and computer on the same Wi-Fi, or the computer on the phone's "
        "hotspot. On Windows, allow VimuScope X through the firewall when asked."));
    // Not on the page any more (kept as the QR code's tooltip).
    m_remoteQr->setToolTip(note->text());
    delete note;
    layout->addStretch();
    m_remoteUrl->setVisible(false);

    layout->setContentsMargins(0, 0, 10, 0);
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("panelScroll"));
    page->setObjectName(QStringLiteral("panelScrollPage"));
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#panelScroll, QWidget#qt_scrollarea_viewport,"
        " QWidget#panelScrollPage { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);

    m_remote = new RemoteServer(this);
    {
        QFile html(QStringLiteral(":/remote/index.html"));
        if (html.open(QIODevice::ReadOnly))
            m_remote->setPage(html.readAll());
    }
    m_remoteTimer = new QTimer(this);
    connect(m_remoteTimer, &QTimer::timeout, this, &MainWindow::sendRemoteFrame);
    connect(m_remote, &RemoteServer::command, this, &MainWindow::handleRemoteCommand);
    connect(m_remote, &RemoteServer::clientsChanged, this, [this](int) { refreshRemote(); });
    connect(m_remoteEnable, &QCheckBox::toggled, this, [this](bool) { refreshRemote(); });
    connect(m_remotePort, &QSpinBox::valueChanged, this, [this](int) {
        if (m_remote->isRunning())
            m_remote->stop();   // restarted on the new port by refreshRemote()
        refreshRemote();
    });
    connect(m_remoteAddress, &QComboBox::currentIndexChanged, this, [this](int) { refreshRemote(); });
    connect(m_remoteRate, &QComboBox::currentIndexChanged, this, [this](int) { refreshRemote(); });
    connect(m_remoteNewKey, &QPushButton::clicked, this, [this] {
        m_remote->newKey();
        refreshRemote();
    });
    refreshRemote();
    return scroll;
}

void MainWindow::refreshRemote()
{
    if (!m_remote)
        return;
    const bool on = m_remoteEnable->isChecked();
    QString error;
    if (on && !m_remote->isRunning()
        && !m_remote->start(quint16(m_remotePort->value()), &error)) {
        m_remoteEnable->blockSignals(true);
        m_remoteEnable->setChecked(false);
        m_remoteEnable->blockSignals(false);
        m_remoteStatus->setText(QStringLiteral("Couldn't start: %1 (try another port)").arg(error));
        m_remoteStatus->setVisible(true);
        m_remoteQr->clear();
        m_remoteQr->setVisible(false);
        m_remoteUrl->clear();
        m_remoteUrl->setVisible(false);
        return;
    }
    if (!on && m_remote->isRunning())
        m_remote->stop();

    // This computer's IPv4 addresses, private networks (home Wi-Fi, phone
    // hotspots) first; the selection is kept across refreshes.
    const QString selected = m_remoteAddress->currentText();
    QStringList addresses, others;
    for (const QHostAddress &a : QNetworkInterface::allAddresses()) {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback())
            continue;
        const quint32 ip = a.toIPv4Address();
        const bool privateNet = (ip >> 24) == 10 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8;
        (privateNet ? addresses : others) << a.toString();
    }
    addresses << others;
    m_remoteAddress->blockSignals(true);
    m_remoteAddress->clear();
    m_remoteAddress->addItems(addresses);
    if (addresses.contains(selected))
        m_remoteAddress->setCurrentText(selected);
    m_remoteAddress->blockSignals(false);

    m_remoteTimer->setInterval(1000 / std::max(1, m_remoteRate->currentData().toInt()));
    if (!on) {
        m_remoteTimer->stop();
        m_remoteQr->clear();
        m_remoteQr->setVisible(false);
        m_remoteUrl->clear();
        m_remoteUrl->setVisible(false);
        m_remoteStatus->clear();                  // nothing to say while off
        m_remoteStatus->setVisible(false);
        return;
    }
    if (addresses.isEmpty()) {
        m_remoteQr->clear();
        m_remoteQr->setVisible(false);
        m_remoteUrl->clear();
        m_remoteUrl->setVisible(false);
        m_remoteStatus->setText(QStringLiteral("No network: connect to Wi-Fi or a phone's hotspot"));
        m_remoteStatus->setVisible(true);
        return;
    }
    m_remoteTimer->start();

    const QString url = QStringLiteral("http://%1:%2/?k=%3")
                            .arg(m_remoteAddress->currentText()).arg(m_remote->port()).arg(m_remote->key());
    m_remoteUrl->setText(url);
    // QR code: black on white with the standard 4-module quiet zone, scaled
    // to whole pixels so it stays sharp.
    const Qr::Code qr = Qr::Code::encode(url.toStdString());
    if (qr.size > 0) {
        const int quiet = 4;
        const int modules = qr.size + 2 * quiet;
        const int scale = std::max(2, 220 / modules);
        QImage image(modules * scale, modules * scale, QImage::Format_RGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        for (int y = 0; y < qr.size; ++y)
            for (int x = 0; x < qr.size; ++x)
                if (qr.at(x, y))
                    painter.fillRect((x + quiet) * scale, (y + quiet) * scale, scale, scale, Qt::black);
        painter.end();
        m_remoteQr->setPixmap(QPixmap::fromImage(image));
        m_remoteQr->setFixedSize(image.size());
        m_remoteQr->setVisible(true);
    }
    m_remoteUrl->setVisible(true);
    const int phones = m_remote->clientCount();
    m_remoteStatus->setVisible(true);
    m_remoteStatus->setText(phones == 0 ? QStringLiteral("Waiting for a phone\u2026")
                                        : QStringLiteral("%1 phone%2 connected").arg(phones)
                                              .arg(phones == 1 ? QString() : QStringLiteral("s")));
}

void MainWindow::sendRemoteFrame()
{
    if (!m_remote || !m_view || m_remote->clientCount() == 0)
        return;
    QJsonObject frame = m_view->remoteFrame(500);
    frame.insert(QStringLiteral("mode"), m_acquisitionMode == ModeCapture ? QStringLiteral("Capture")
                                         : m_acquisitionMode == ModeStream ? QStringLiteral("Live")
                                                                           : QStringLiteral("Record"));
    frame.insert(QStringLiteral("state"), m_running ? QStringLiteral("Running") : QStringLiteral("Stopped"));
    frame.insert(QStringLiteral("seq"), m_remoteSeq);
    // Every channel this scope has, on or off, for the phone's toggles.
    QJsonArray channels;
    for (int ch = 0; ch < m_channelCount; ++ch) {
        QJsonObject o;
        o.insert(QStringLiteral("n"), ch + 1);
        o.insert(QStringLiteral("on"), m_channel[ch].mode->checkedId() != ChannelOff);
        o.insert(QStringLiteral("color"), Theme::channelColor(ch).name());
        channels.append(o);
    }
    frame.insert(QStringLiteral("allChannels"), channels);
    m_remote->broadcast(QJsonDocument(frame).toJson(QJsonDocument::Compact));
}

// The phone's buttons do exactly what the matching desktop controls do: the
// same card signals a tap on the card emits.
void MainWindow::handleRemoteCommand(const QJsonObject &command)
{
    const QString cmd = command.value(QStringLiteral("cmd")).toString();
    const bool up = command.value(QStringLiteral("step")).toInt() > 0;
    if (cmd == QLatin1String("runstop")) {
        emit m_runCard->clicked();
    } else if (cmd == QLatin1String("single")) {
        if (m_singleButton && m_singleButton->isEnabled())
            m_singleButton->click();
    } else if (cmd == QLatin1String("timebase")) {
        if (up) emit m_scopeCard->stepUp(); else emit m_scopeCard->stepDown();
    } else if (cmd == QLatin1String("trigger")) {
        if (up) emit m_triggerCard->stepUp(); else emit m_triggerCard->stepDown();
    } else if (cmd == QLatin1String("position")) {
        // A trace dragged on the phone: the same signal a mouse drag on the
        // plot sends, in volts at the channel's current scale.
        const int ch = command.value(QStringLiteral("ch")).toInt();
        const double volts = command.value(QStringLiteral("volts")).toDouble();
        if (ch >= 0 && ch < m_channelCount && std::isfinite(volts) && volts != 0.0)
            emit m_view->offsetDragged(ch, volts);
        // Echoed in every frame, so the phone shows only the drag it has
        // sent but not yet seen applied — no snapping back mid-drag.
        m_remoteSeq = std::max(m_remoteSeq, command.value(QStringLiteral("seq")).toDouble());
        return;   // drags arrive often; the next regular frame shows them
    } else if (cmd == QLatin1String("pan")) {
        // A sideways drag on the phone: exactly a sideways drag on the plot
        // (each mode pans its own way: Capture, Live history, Record).
        const double fraction = command.value(QStringLiteral("fraction")).toDouble();
        if (std::isfinite(fraction) && fraction != 0.0)
            emit m_view->panRequested(std::clamp(fraction, -2.0, 2.0));
        return;
    } else if (cmd == QLatin1String("mathPosition")) {
        const double volts = command.value(QStringLiteral("volts")).toDouble();
        if (std::isfinite(volts) && volts != 0.0)
            emit m_view->mathOffsetDragged(volts);   // exactly a mouse drag on Math
        m_remoteSeq = std::max(m_remoteSeq, command.value(QStringLiteral("seq")).toDouble());
        return;
    } else if (cmd == QLatin1String("channel")) {
        // On/off, through the channel's own buttons and handler.
        const int ch = command.value(QStringLiteral("ch")).toInt();
        if (ch >= 0 && ch < m_channelCount) {
            const int next = m_channel[ch].mode->checkedId() == ChannelOff ? ChannelManual : ChannelOff;
            if (auto *b = m_channel[ch].mode->button(next))
                b->setChecked(true);
            emit m_channel[ch].mode->idClicked(next);
        }
    } else if (cmd == QLatin1String("scale")) {
        const int ch = command.value(QStringLiteral("ch")).toInt();
        if (ch >= 0 && ch < m_channelCount) {
            if (up) emit m_channelCard[ch]->stepUp(); else emit m_channelCard[ch]->stepDown();
        }
    }
    sendRemoteFrame();   // the phone sees the result straight away
}

QWidget *MainWindow::buildRulersPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_rulersTimeCheck = new QCheckBox(QStringLiteral("Time cursors"));
    m_rulersTimeCheck->setChecked(true);
    layout->addWidget(m_rulersTimeCheck);
    connect(m_rulersTimeCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_view->setRulersTimeEnabled(on);
        if (m_rulersTrackCheck)
            m_rulersTrackCheck->setEnabled(on);
    });

    m_rulersTrackCheck = new QCheckBox(QStringLiteral("Track the trace"));
    m_rulersTrackCheck->setToolTip(QStringLiteral(
        "Each channel's voltage rulers follow its trace at the two time rulers: drag a time "
        "ruler and read the value there, the difference and the slope between them."));
    layout->addWidget(m_rulersTrackCheck);
    connect(m_rulersTrackCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_view->setRulersTrack(on);
    });

    layout->addWidget(sectionLabel(QStringLiteral("Channel voltage cursors")));
    for (int ch = 0; ch < 4; ++ch) {
        auto *box = new QCheckBox(QStringLiteral("CH%1").arg(ch + 1));
        setRulerCheckboxColor(box, m_colour[ch]);
        layout->addWidget(box);
        m_rulersChannelChecks[ch] = box;
        connect(box, &QCheckBox::toggled, this, [this, ch](bool on) {
            m_view->setRulersChannelEnabled(ch, on);
        });
    }
    m_rulersMathCheck = new QCheckBox(QStringLiteral("Math"));
    // buildDetailPanel() (and so this function) runs before m_view is
    // constructed — m_view = new ScopeView happens later in buildUi(), even
    // though that line sits earlier in this file than buildRulersPage()'s own
    // code. Every other m_view reference here is safely inside a deferred
    // lambda; this one ran immediately and crashed on the null pointer.
    // Matches ScopeView's own default math colour, so this needs no correction
    // once m_view exists — synced for real in refreshRulersChannelList() below
    // regardless, the moment either is available.
    setRulerCheckboxColor(m_rulersMathCheck, m_view ? m_view->mathColor() : Theme::mathColor());
    layout->addWidget(m_rulersMathCheck);
    connect(m_rulersMathCheck, &QCheckBox::toggled, this, [this](bool on) {
        m_view->setRulersChannelEnabled(ScopeView::RulersMathChannel, on);
    });

    // A separate, self-contained cursor pair on SpectrumView itself — not
    // the same Rulers system as everything above, which lives on ScopeView
    // and measures time/volts. Frequency/level are different units on a
    // different widget; only the on/off toggle belongs on this shared page.
    m_rulersFftCheck = new QCheckBox(QStringLiteral("FFT"));
    m_rulersFftCheck->setVisible(false);   // Spectrum mode starts off; setInstrumentMode() reveals this
    layout->addWidget(m_rulersFftCheck);
    connect(m_rulersFftCheck, &QCheckBox::toggled, this, [this](bool on) {
        if (m_spectrum)
            m_spectrum->setRulersEnabled(on);
    });

    refreshRulersChannelList();

    layout->addStretch();

    // Scrolls like the channel and Decoding pages (transparent, bar at the
    // panel's edge) on short screens.
    layout->setContentsMargins(0, 0, 10, 0);
    auto *scroll = new QScrollArea;
    scroll->setObjectName(QStringLiteral("panelScroll"));
    page->setObjectName(QStringLiteral("panelScrollPage"));
    scroll->setWidget(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet(QStringLiteral(
        "QScrollArea#panelScroll, QWidget#qt_scrollarea_viewport,"
        " QWidget#panelScrollPage { background: transparent; }"));
    scroll->viewport()->setAutoFillBackground(false);
    page->setAutoFillBackground(false);
    return scroll;
}

QWidget *MainWindow::buildMeasurePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    // One exclusive selector, not a per-channel list like Rulers' own: eleven
    // checkboxes shown five times over (one set per channel, plus Math)
    // would be a lot of simultaneous UI for what is usually a "check one
    // channel's numbers" task. Each channel (and Math) still keeps its own
    // independent set underneath — this only picks which one is currently
    // visible below.
    layout->addWidget(sectionLabel(QStringLiteral("Source channel")));
    auto *channelRow = new QHBoxLayout;
    m_measureChannelGroup = new QButtonGroup(this);
    for (int ch = 0; ch < 4; ++ch) {
        auto *seg = segment(QStringLiteral("CH%1").arg(ch + 1));
        m_measureChannelGroup->addButton(seg, ch);
        m_measureChannelSegments[ch] = seg;
        channelRow->addWidget(seg);
    }
    m_measureMathSegment = segment(QStringLiteral("Math"));
    m_measureChannelGroup->addButton(m_measureMathSegment, ScopeView::RulersMathChannel);
    channelRow->addWidget(m_measureMathSegment);
    channelRow->addStretch();
    layout->addLayout(channelRow);
    connect(m_measureChannelGroup, &QButtonGroup::idClicked, this, [this](int ch) {
        m_measureChannel = ch;
        refreshMeasureChannelSelector();   // re-syncs the checkboxes below to ch
    });

    layout->addWidget(sectionLabel(QStringLiteral("Measurements")));
    static const char *const kLabels[ScopeView::MeasurementCount] = {
        "Vpp", "Vrms", "Mean", "Max", "Min", "Freq",
        "AC RMS", "Period", "Duty", "+Width", "-Width"
    };
    auto *grid = new QGridLayout;
    grid->setHorizontalSpacing(14);
    grid->setVerticalSpacing(4);
    for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
        auto *box = new QCheckBox(QString::fromLatin1(kLabels[i]));
        m_measureChecks[i] = box;
        grid->addWidget(box, i / 3, i % 3);
        connect(box, &QCheckBox::toggled, this, [this, i](bool on) {
            if (m_view)
                m_view->setChannelMeasurement(m_measureChannel, ScopeView::Measurement(i), on);
        });
    }
    layout->addLayout(grid);

    refreshMeasureChannelSelector();

    layout->addStretch();
    return page;
}

QWidget *MainWindow::buildSettingsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    layout->addWidget(sectionLabel(QStringLiteral("Appearance")));
    auto *themeRow = new QHBoxLayout;
    m_themeGroup = new QButtonGroup(this);
    auto *darkSeg = segment(QStringLiteral("Dark"));
    auto *lightSeg = segment(QStringLiteral("Light"));
    m_themeGroup->addButton(darkSeg, static_cast<int>(Theme::Mode::Dark));
    m_themeGroup->addButton(lightSeg, static_cast<int>(Theme::Mode::Light));
    (Theme::isDark() ? darkSeg : lightSeg)->setChecked(true);
    connect(m_themeGroup, &QButtonGroup::idClicked, this, [this](int id) {
        Theme::setMode(id == static_cast<int>(Theme::Mode::Light)
                           ? Theme::Mode::Light : Theme::Mode::Dark);
        applyTheme();
    });
    themeRow->addWidget(darkSeg);
    themeRow->addWidget(lightSeg);
    themeRow->addStretch();
    layout->addLayout(themeRow);

    layout->addWidget(sectionLabel(QStringLiteral("Sound and startup")));
    m_soundsEnable = new QCheckBox(QStringLiteral("Sound effects"));
    m_soundsEnable->setChecked(true);
    m_soundsEnable->setToolTip(QStringLiteral("A sound when the scope is plugged in or out."));
    layout->addWidget(m_soundsEnable);
    m_startupAnimation = new QCheckBox(QStringLiteral("Startup animation"));
    m_startupAnimation->setChecked(true);
    m_startupAnimation->setToolTip(QStringLiteral(
        "The animated logo at startup (a click or Esc skips it). Off: a still banner."));
    layout->addWidget(m_startupAnimation);
#ifdef VSX_HAVE_MULTIMEDIA
    // "[media]" lines on the terminal: whether each sound loaded (a sound
    // that fails to load, or no audio output, is otherwise silent).
    for (auto [sound, file] : { std::pair<QSoundEffect **, const char *>{ &m_plugInSound, "plugged_in.wav" },
                                { &m_plugOutSound, "plugged_out.wav" } }) {
        *sound = new QSoundEffect(this);
        QSoundEffect *effect = *sound;
        const QString name = QString::fromLatin1(file);
        connect(effect, &QSoundEffect::statusChanged, this, [effect, name] {
            if (effect->status() == QSoundEffect::Ready)
                VSX_MEDIA_LOG("%s loaded", qPrintable(name));
            else if (effect->status() == QSoundEffect::Error)
                VSX_MEDIA_LOG("%s failed to load", qPrintable(name));
        });
        effect->setSource(QUrl(QStringLiteral("qrc:/media/") + name));
    }
#else
    for (QCheckBox *box : { m_soundsEnable, m_startupAnimation }) {
        box->setEnabled(false);
        box->setToolTip(QStringLiteral("Needs Qt Multimedia (Arch: qt6-multimedia and "
                                       "qt6-multimedia-ffmpeg), then rebuild."));
    }
#endif

    layout->addWidget(sectionLabel(QStringLiteral("Zero calibration")));
    {
        auto *about = new QLabel(QStringLiteral(
            "Removes the small offset the inputs show with nothing connected, which "
            "drifts as the scope warms. Disconnect both probes first; most accurate "
            "after 15-20 minutes plugged in."));
        about->setWordWrap(true);
        layout->addWidget(about);
        m_zeroEnable = new QCheckBox(QStringLiteral("Apply zero calibration"));
        m_zeroEnable->setChecked(true);
        layout->addWidget(m_zeroEnable);
        m_quickZeroButton = new QPushButton(QStringLiteral("Quick zero (current settings)"));
        m_quickZeroButton->setToolTip(QStringLiteral(
            "About a second: both channels at their current range and coupling, in this mode."));
        layout->addWidget(m_quickZeroButton);
        m_fullCalButton = new QPushButton(QStringLiteral("Full calibration (all ranges)"));
        m_fullCalButton->setToolTip(QStringLiteral(
            "About a minute: every hardware range the scope has (found by reading back what it "
            "really uses), DC and AC, Capture and Live/Record."));
        layout->addWidget(m_fullCalButton);
        m_clearCalButton = new QPushButton(QStringLiteral("Clear calibration"));
        m_clearCalButton->setToolTip(QStringLiteral("Forget every stored offset (nothing is removed from the signal)."));
        layout->addWidget(m_clearCalButton);
        m_rangeReportButton = new QPushButton(QStringLiteral("Range report"));
        m_rangeReportButton->setToolTip(QStringLiteral(
            "Steps every volts/div page at several input offsets (Capture mode, about 30 s) and "
            "lists the window the ADC actually used for each, against the one requested. "
            "Your settings are restored afterwards."));
        layout->addWidget(m_rangeReportButton);
        m_rangeReportButton->setVisible(false);   // diagnostics, kept for development
        connect(m_rangeReportButton, &QPushButton::clicked, this, [this] { startRangeReport(); });
        connect(m_clearCalButton, &QPushButton::clicked, this, [this] {
            m_zeroTable.clear();
            saveZeroTable();
            for (int ch = 0; ch < 4; ++ch)
                applyZeroOffsets(ch);
            refreshCalibrationStatus();
            statusBar()->showMessage(QStringLiteral("Zero calibration cleared"), 5000);
        });
        m_calStatus = new QLabel;
        m_calStatus->setWordWrap(true);
        layout->addWidget(m_calStatus);
        m_calTimer = new QTimer(this);
        m_calTimer->setSingleShot(true);
        m_calTimeout = new QTimer(this);
        m_calTimeout->setSingleShot(true);
        m_calTimeout->setInterval(4000);
        connect(m_zeroEnable, &QCheckBox::toggled, this, [this](bool) {
            for (int ch = 0; ch < 4; ++ch)
                applyZeroOffsets(ch);
        });
        connect(m_quickZeroButton, &QPushButton::clicked, this, [this] { startCalibration(true); });
        connect(m_fullCalButton, &QPushButton::clicked, this, [this] { startCalibration(false); });
        connect(m_calTimeout, &QTimer::timeout, this, [this] {
            if (!m_calibrating)
                return;
            ++m_calIndex;          // no answer: skip this step
            runCalibrationStep();
        });
    }

    layout->addWidget(sectionLabel(QStringLiteral("Record directory")));
    m_recordDirLabel = new QLabel;
    m_recordDirLabel->setWordWrap(true);
    m_recordDirLabel->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    layout->addWidget(m_recordDirLabel);

    auto *dirRow = new QHBoxLayout;
    auto *browseButton = new QPushButton(QStringLiteral("Browse\u2026"));
    connect(browseButton, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Choose a record directory"), recordDirectory());
        if (chosen.isEmpty())
            return;   // cancelled
        QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
        settings.setValue(QStringLiteral("settings/recordDirectory"), chosen);
        refreshSettingsPage();
    });
    dirRow->addWidget(browseButton);

    auto *defaultButton = new QPushButton(QStringLiteral("Use default"));
    connect(defaultButton, &QPushButton::clicked, this, [this] {
        QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
        settings.remove(QStringLiteral("settings/recordDirectory"));
        refreshSettingsPage();
    });
    dirRow->addWidget(defaultButton);
    dirRow->addStretch();
    layout->addLayout(dirRow);

    refreshSettingsPage();

    layout->addStretch();
    return page;
}

// Re-reads whatever recordDirectory() currently resolves to — called once
// at construction and again after either Settings-page button changes it,
// rather than the label caching a path that could go stale the moment the
// user picks a new one.
void MainWindow::refreshSettingsPage()
{
    if (m_recordDirLabel)
        m_recordDirLabel->setText(recordDirectory());
}

// Called once at construction (after every widget below exists) and again
// whenever Settings' Dark/Light toggle changes Theme::currentMode() at
// runtime. Several kinds of widget need touching, for different reasons:
//   - Standard Qt controls (combo boxes, spin boxes, checkboxes, buttons,
//     line edits, scrollbars, tooltips, menus, group boxes) read the app
//     palette and the global style sheet, so re-applying both here reaches
//     every one of them automatically, wherever they are.
//   - A handful of widgets build their own style-sheet string instead of
//     relying on the global one (the record tab bar, the detail panel
//     frame, a couple of muted-text labels) — each needs that string
//     recomputed and re-applied explicitly, since nothing else would
//     prompt it.
//   - Every icon in this app is a baked QPixmap wrapped in a QIcon, coloured
//     once at the moment it was built — a style sheet re-evaluates itself
//     once re-applied, but nothing short of re-rendering a QIcon points it
//     at a new colour. m_themedRefreshers holds one callback per such
//     widget (collected as each was built, since most are anonymous locals
//     from a loop with no member pointer to reach later), each closing over
//     everything it needs to redo both its icon and its style sheet from
//     scratch — invoking all of them here is what makes a live toggle
//     actually repaint every toolbar and sidebar icon instead of only the
//     text beside it. A few icon-bearing widgets skip this list because
//     they already have their own member pointer for other reasons
//     (m_mathColour, and each channel's own colour/manageProbes buttons) —
//     refreshed directly below instead of through the list.
//   - The cards and the four plot views read Theme:: colours directly
//     inside their own paint/restyle code, so a card needs refreshTheme()
//     called explicitly and a plot view just needs a repaint.
// Not covered, on a live toggle specifically (still exactly right at the
// next launch, since all of this reads Theme:: fresh at construction time
// regardless): sectionLabel()'s ~20 scattered call sites, each an anonymous
// QLabel with no member pointer to re-style through, and the Record page's
// one-off info note for the same reason. Both need a restart to pick up a
// toggle made mid-session.
// logo-dark.png has light text for dark mode's own near-black bar;
// logo-light.png (dark text, from the design as supplied) is for light
// mode's near-white one — each is illegible against the opposite
// background, so this picks whichever actually matches right now.
void MainWindow::refreshLogo()
{
    if (!m_logoLabel)
        return;
    const QString path = Theme::isDark() ? QStringLiteral(":/images/logo-dark.png")
                                          : QStringLiteral(":/images/logo-light.png");
    const QPixmap logo(path);
    if (logo.isNull())
        return;
    // Scaled by height so it sits inside the bar; 20% up on the previous
    // 0.62 of the card height.
    m_logoLabel->setPixmap(logo.scaledToHeight(int(kCardHeight * 0.744),
                                               Qt::SmoothTransformation));
}

void MainWindow::applyTheme()
{
    VSX_TRACE("applyTheme");
    // Unchanged sheets / palettes are skipped: each set restyles everything
    // under it (the whole app for qApp), the dominant cost here.
    {
        const QPalette palette = Theme::palette();
        if (qApp->palette() != palette)
            qApp->setPalette(palette);
        const QString sheet = Theme::globalStyleSheet();
        if (qApp->styleSheet() != sheet)
            qApp->setStyleSheet(sheet);
    }
    const auto setSheet = [](QWidget *w, const QString &sheet) {
        if (w && w->styleSheet() != sheet)
            w->setStyleSheet(sheet);
    };
    refreshLogo();

    // Combo popups: Qt's default menu-style delegate ignores the style
    // sheet's item rules and paints hover in its own dark colour; a styled
    // delegate makes the accent hover/selection in globalStyleSheet() apply.
    // Done here (construction and every theme switch), after every combo in
    // the window exists.
    for (QComboBox *box : findChildren<QComboBox *>()) {
        if (!qobject_cast<QStyledItemDelegate *>(box->itemDelegate()))
            box->setItemDelegate(new QStyledItemDelegate(box));
        // Its list window: frameless and translucent, so the list's rounded
        // corners (dropdownListStyle()) show instead of a square window.
        if (QWidget *popup = box->view() ? box->view()->window() : nullptr;
            popup && popup != this && !popup->property("vsxRounded").toBool()) {
            popup->setWindowFlags(popup->windowFlags() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
            popup->setAttribute(Qt::WA_TranslucentBackground);
            popup->setProperty("vsxRounded", true);
        }
    }

    if (m_recordTabs) {
        setSheet(m_recordTabs, recordTabStyle());
        updateRecordTabCloseIcons();
    }
    if (m_panelHost) {
        setSheet(m_panelHost, panelHostStyle());
        setSheet(statusBar(), statusBarStyle());
        PanelControls::restyle(m_panelHost);
    }
    if (m_sampleInfo)
        setSheet(m_sampleInfo, QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    if (m_recordDirLabel)
        setSheet(m_recordDirLabel, QStringLiteral("color: %1;").arg(Theme::textMuted().name()));

    for (const auto &refresh : m_themedRefreshers)
        refresh();

    if (m_mathColour)
        m_mathColour->setIcon(uiIcon(this, QStringLiteral("eye"), QStyle::SP_DesktopIcon,
                                     Theme::text()));
    for (int ch = 0; ch < 4; ++ch) {
        if (m_channel[ch].colour)
            m_channel[ch].colour->setIcon(uiIcon(this, QStringLiteral("eye"),
                                                 QStyle::SP_DesktopIcon, Theme::text()));
        if (m_channel[ch].manageProbes)
            m_channel[ch].manageProbes->setIcon(uiIcon(this, QStringLiteral("wrench"),
                                                       QStyle::SP_FileDialogDetailedView,
                                                       Theme::text()));
    }

    // Every card, not a fixed list: the Math card was missing from one and
    // kept the previous theme until a hover made it restyle itself.
    for (CardButton *card : findChildren<CardButton *>())
        card->refreshTheme();
    for (AnnotationBox *box : findChildren<AnnotationBox *>())
        box->refreshTheme();
    // Section labels carry their colour in their own style sheet (see
    // sectionLabel()), so they kept the old theme until a restart.
    for (QLabel *label : findChildren<QLabel *>(QStringLiteral("sectionLabel"))) {
        setSheet(label, QStringLiteral("color: %1; margin-top: 6px;").arg(Theme::textMuted().name()));
    }
    for (QLabel *label : findChildren<QLabel *>(QStringLiteral("groupHeading")))
        setSheet(label, groupHeadingStyle(label->property("firstGroup").toBool()));
    for (int ch = 0; ch < 4; ++ch)
        if (m_channelCard[ch])
            m_channelCard[ch]->refreshTheme();

    if (m_view)
        m_view->update();
    if (m_nav)
        m_nav->update();
    if (m_spectrum)
        m_spectrum->update();
    if (m_xy)
        m_xy->update();
}

// currently being configured just went off, and re-syncs the six checkboxes
// to whichever channel that leaves selected.
void MainWindow::refreshMeasureChannelSelector()
{
    // Math's segment isn't in m_measureChannelSegments (RulersMathChannel is
    // out of that array's 0..3 range) — mirrors how Rulers keeps Math in a
    // separate checkbox pointer too, rather than stretching the array.
    auto segmentFor = [this](int ch) -> QPushButton * {
        if (ch == ScopeView::RulersMathChannel)
            return m_measureMathSegment;
        return (ch >= 0 && ch < 4) ? m_measureChannelSegments[ch] : nullptr;
    };

    // The scope's own channels and Math always shown (CH1 | CH2 | Math on a
    // 2-channel scope), greyed out while off; channels it doesn't have hidden.
    // (Enabled, not isVisible(): that's false whenever the page isn't shown.)
    const int channels = m_channelCount > 0 ? std::min(m_channelCount, 4) : 2;
    int firstAvailable = -1;
    for (int ch = 0; ch < 4; ++ch) {
        QPushButton *seg = m_measureChannelSegments[ch];
        if (!seg)
            continue;
        const bool available = m_channel[ch].mode->checkedId() != ChannelOff;
        seg->setVisible(ch < channels);
        seg->setEnabled(available && ch < channels);
        if (available && ch < channels && firstAvailable < 0)
            firstAvailable = ch;
    }
    if (m_measureMathSegment) {
        const bool available = m_mathEnabled && m_mathEnabled->isChecked();
        m_measureMathSegment->setVisible(true);
        m_measureMathSegment->setEnabled(available);
        if (available && firstAvailable < 0)
            firstAvailable = ScopeView::RulersMathChannel;
    }

    if (firstAvailable >= 0) {
        QPushButton *current = segmentFor(m_measureChannel);
        if (!current || !current->isEnabled() || current->isHidden())
            m_measureChannel = firstAvailable;
    }

    if (QPushButton *seg = segmentFor(m_measureChannel)) {
        seg->blockSignals(true);
        seg->setChecked(true);
        seg->blockSignals(false);
    }

    if (!m_view)
        return;   // buildMeasurePage() runs before m_view exists — see buildRulersPage()'s own note
    for (int i = 0; i < ScopeView::MeasurementCount; ++i) {
        QCheckBox *box = m_measureChecks[i];
        if (!box)
            continue;
        box->blockSignals(true);
        box->setChecked(m_view->channelMeasurementEnabled(m_measureChannel, ScopeView::Measurement(i)));
        box->blockSignals(false);
    }
}

// Shows/hides each source segment (real channels and Math) based on
// whether it is actually available right now — same rule
// refreshMeasureChannelSelector() and refreshRulersChannelList() already
// apply to their own lists — and unchecks any that just became
// unavailable. Unlike the old exclusive version, there is no "fall back to
// the first available" step: with up to two independent sources, whatever
// is left checked (zero, one, or two) is simply what stays selected.
void MainWindow::refreshSpectrumChannelSelector()
{
    if (!m_spectrum)
        return;   // buildInstrumentsPage() runs before m_spectrum exists

    auto setAvailable = [this](int id, bool available) {
        QAbstractButton *b = m_spectrumChannelGroup ? m_spectrumChannelGroup->button(id) : nullptr;
        if (!b)
            return;
        b->setVisible(available);
        if (!available && b->isChecked()) {
            b->blockSignals(true);
            b->setChecked(false);
            b->blockSignals(false);
        }
    };
    for (int ch = 0; ch < 4; ++ch)
        setAvailable(ch, m_channel[ch].mode->checkedId() != ChannelOff);
    setAvailable(ScopeView::RulersMathChannel, m_mathEnabled && m_mathEnabled->isChecked());

    updateSpectrumSources();
}

// The single place that turns "whichever segments are currently checked"
// into SpectrumView's two trace slots — colour included, so a source that
// changes its own colour elsewhere (the channel rail's colour picker,
// Math's own colour swatch) is picked up the next time this runs rather
// than needing its own separate wiring.
void MainWindow::updateSpectrumSources()
{
    if (!m_spectrum || !m_spectrumChannelGroup)
        return;

    // Checked ids, in a fixed order (real channels low-to-high, Math last)
    // so which slot a source lands in stays predictable as boxes are
    // (un)checked, rather than depending on click order.
    QVector<int> active;
    for (int candidate : { 0, 1, 2, 3, int(ScopeView::RulersMathChannel) }) {
        if (auto *b = m_spectrumChannelGroup->button(candidate); b && b->isChecked())
            active.append(candidate);
    }

    m_spectrumSources[0] = active.size() > 0 ? active[0] : -1;
    m_spectrumSources[1] = active.size() > 1 ? active[1] : -1;

    for (int slot = 0; slot < 2; ++slot) {
        const int src = m_spectrumSources[slot];
        m_spectrum->setTraceActive(slot, src >= 0);
        if (src < 0)
            continue;
        const QColor colour = (src == ScopeView::RulersMathChannel)
            ? (m_view ? m_view->mathColor() : Theme::mathColor())
            : m_colour[src];
        m_spectrum->setTraceColor(slot, colour);
    }
}

// Called from each of the three places MainWindow already has a channel's
// raw samples in hand (Capture's samplesReady handler, Live's
// refreshLiveView(), Record's refreshRecordView()) — sends them on to
// whichever spectrum slot(s) currently want this channel, and separately
// to any slot showing Math, since ch's update is also what just changed
// Math's own samples if ch is one of its two sources (recomputeMath() runs
// inside m_view->setSamples(), which every caller here has already called
// by this point).
void MainWindow::feedSpectrum(int ch, const QVector<double> &samples, double rate)
{
    if (m_currentInstrumentMode != InstrumentMode::Spectrum || !m_spectrum)
        return;
    for (int slot = 0; slot < 2; ++slot) {
        if (m_spectrumSources[slot] == ch) {
            m_spectrum->setTraceSamples(slot, samples, rate);
        } else if (m_spectrumSources[slot] == ScopeView::RulersMathChannel
                   && m_view->mathEnabled()) {
            m_spectrum->setTraceSamples(slot, m_view->mathSamples(), rate);
        }
    }
}

// Called from the same three places as feedSpectrum() above. Every
// channel's samples are cached unconditionally (cheap relative to a
// capture buffer itself, and means XY mode has a pairing to show
// immediately on activation rather than waiting for the next tick) —
// only actually forwarded to XYView while XY mode is active and this
// channel is one of its two axes.
void MainWindow::feedXY(int ch, const QVector<double> &samples)
{
    if (ch < 0 || ch >= 4)
        return;
    m_lastChannelSamples[ch] = samples;

    if (m_currentInstrumentMode != InstrumentMode::XY || !m_xy)
        return;
    if (ch != m_xyChannelX && ch != m_xyChannelY)
        return;
    const QVector<double> &xs = m_lastChannelSamples[m_xyChannelX];
    const QVector<double> &ys = m_lastChannelSamples[m_xyChannelY];
    if (xs.isEmpty() || ys.isEmpty())
        return;
    m_xy->setSamples(xs, ys);
}

// Shows/hides each axis's channel segments based on whether that channel
// is actually on right now (same rule refreshSpectrumChannelSelector() and
// refreshRulersChannelList() already apply to their own lists), falling
// back to the first channel still on if the one currently assigned to
// that axis just went off — same fallback Measure's own single-channel
// selector already uses, since X and Y are each an exclusive choice, not
// a multi-select like Spectrum's sources.
void MainWindow::refreshXYChannelSelectors()
{
    if (!m_xy)
        return;

    auto refreshAxis = [this](QButtonGroup *group, std::array<QPushButton *, 4> &segments,
                              int &selected) {
        if (!group)
            return;
        int firstAvailable = -1;
        for (int ch = 0; ch < 4; ++ch) {
            QPushButton *seg = segments[ch];
            if (!seg)
                continue;
            const bool available = m_channel[ch].mode->checkedId() != ChannelOff;
            seg->setVisible(available);
            if (available && firstAvailable < 0)
                firstAvailable = ch;
        }
        if (firstAvailable >= 0) {
            QPushButton *current = segments[selected];
            if (!current || !current->isVisible())
                selected = firstAvailable;
        }
        if (QPushButton *seg = segments[selected]) {
            seg->blockSignals(true);
            seg->setChecked(true);
            seg->blockSignals(false);
        }
    };
    refreshAxis(m_xyXGroup, m_xyXSegments, m_xyChannelX);
    refreshAxis(m_xyYGroup, m_xySegments, m_xyChannelY);

    updateXYColors();
    updateXYRanges();
}

// Y is treated as the "measured" axis by convention (X is usually the
// reference/independent one), so the trace takes Y's channel colour —
// re-read here rather than cached, so a colour changed elsewhere (the
// channel rail's own picker) is picked up the next time this runs instead
// of needing its own separate wiring, the same principle
// updateSpectrumSources() already follows for Spectrum's own traces.
void MainWindow::updateXYColors()
{
    if (m_xy)
        m_xy->setTraceColor(m_colour[m_xyChannelY]);
}

// Each axis's range is that axis's own assigned channel's own configured
// full-scale (its card's own "+-X V") — the same fixed-scale convention
// the main plot and Spectrum both already use, not a fit to whatever the
// data happens to peak at. Re-read here rather than cached, so a volts/div
// change made elsewhere (the channel card's own steppers, Auto range) is
// picked up the next time this runs — called from every place that could
// plausibly need it, via refreshXYChannelSelectors() below, the same
// principle updateXYColors() and updateSpectrumSources() already follow.
void MainWindow::updateXYRanges()
{
    if (m_xy && m_view)
        m_xy->setRanges(m_view->channelFullScaleVolts(m_xyChannelX),
                        m_view->channelFullScaleVolts(m_xyChannelY));
}

// The one place that switches Instruments' mode — shows/hides the second
// pane, its controls, and the FFT Rulers checkbox together, so none of
// them can end up out of step with each other (e.g. controls visible with
// no pane to show, after a mode switch that only remembered to toggle one
// of them). Spectrum and XY share the same physical pane
// (m_secondPaneStack); switching between them, or to/from Scope, is
// always a single call here.
void MainWindow::setInstrumentMode(InstrumentMode mode)
{
    if (m_currentInstrumentMode == mode)
        return;
    const InstrumentMode previous = m_currentInstrumentMode;
    m_currentInstrumentMode = mode;

    // Persistence draws on the main plot itself, so it needs no second pane.
    const bool showSecondPane = (mode == InstrumentMode::Spectrum
                                 || mode == InstrumentMode::XY);
    if (m_persistenceControls)
        m_persistenceControls->setVisible(mode == InstrumentMode::Persistence);
    if (m_view)
        m_view->setPersistenceEnabled(mode == InstrumentMode::Persistence);

    if (m_spectrumControls)
        m_spectrumControls->setVisible(mode == InstrumentMode::Spectrum);
    if (m_xyControls)
        m_xyControls->setVisible(mode == InstrumentMode::XY);

    if (m_rulersFftCheck) {
        m_rulersFftCheck->setVisible(mode == InstrumentMode::Spectrum);
        // Not just hidden: actually turned off too, same rule
        // refreshRulersChannelList() already applies to a per-channel
        // Ruler checkbox when that channel becomes unavailable — a
        // checkbox nobody can see should not keep something running
        // invisibly behind it. toggled(false) disables the overlay itself.
        if (mode != InstrumentMode::Spectrum && m_rulersFftCheck->isChecked())
            m_rulersFftCheck->setChecked(false);
    }

    // No point holding onto a trace nobody can see, in whichever of the
    // two this mode switch is leaving.
    if (previous == InstrumentMode::Spectrum && m_spectrum)
        m_spectrum->clearAll();
    else if (previous == InstrumentMode::XY && m_xy)
        m_xy->clear();

    if (m_secondPaneStack) {
        m_secondPaneStack->setVisible(showSecondPane);
        if (mode == InstrumentMode::Spectrum && m_spectrum) {
            m_secondPaneStack->setCurrentWidget(m_spectrum);
            refreshSpectrumChannelSelector();
        } else if (mode == InstrumentMode::XY && m_xy) {
            m_secondPaneStack->setCurrentWidget(m_xy);
            refreshXYChannelSelectors();
        }
    }

    // A prior drag could have collapsed this pane to nothing (which now
    // turns Instruments back to Scope mode entirely — see the splitter's
    // own splitterMoved handler), or this could be the very first
    // activation of either mode, where a hidden widget never gets a real
    // layout-computed size to begin with; either way "currently at
    // effectively zero height" is the one condition that matters,
    // regardless of why or which of Spectrum/XY is involved. 40% is a
    // reasonable, clearly visible starting point — applied only in that
    // case, so a larger split chosen deliberately earlier in the same
    // session is left exactly as the user left it.
    if (showSecondPane && m_plotSplitter) {
        const QList<int> sizes = m_plotSplitter->sizes();
        if (sizes.size() == 2) {
            const int total = sizes[0] + sizes[1];
            if (total > 0 && sizes[1] <= 1)
                m_plotSplitter->setSizes({int(total * 0.6), int(total * 0.4)});
        }
    }
}

void MainWindow::applyStreamSettings()
{
    if (m_acquisitionMode != ModeStream || !m_running)
        return;
    // Both history and update rate are arguments to StreamCapture, so they only
    // take effect on a fresh start.
    stopAcquisition();
    startAcquisition();
}

// Pulls exactly one screen's worth of the newest samples. The window is the
// whole pulled buffer, so the trace rolls: new samples enter from the right.
void MainWindow::pullStream()
{
    if (!m_stream->running())
        return;

    // A vanished device stops delivering blocks without saying so, so a long
    // silence while running means something is wrong.
    if (m_stream->stats().blocks > 0 && !m_acq->ready()) {
        stopAcquisition();
        statusBar()->showMessage(
            QStringLiteral("Device disconnected — acquisition stopped."), 6000);
        return;
    }

    if (m_acquisitionMode == ModeRecord) {
        updateStatusBar();

        if (m_stream->recordComplete()) {
            m_streamTick->stop();
            m_stream->stop();
            setRunning(false);
            m_recordViewStart = 0;
            m_recordViewLength = m_stream->recorded();
            refreshRecordView();
            // Same as stopping by hand (stopAcquisition()): the tab bar was
            // disabled for the recording and stayed greyed out when it ran
            // to its full size instead.
            if (m_recordTabs) {
                m_recordTabs->setEnabled(true);
                refreshRecordTabs();
            }
            statusBar()->showMessage(
                QStringLiteral("Record complete — arm Zoom and drag a box"), 6000);
            return;
        }

        // Show the record as it grows: the window is everything captured so
        // far, so the time axis extends with it and stops where you stop.
        if (m_recordRefresh.elapsed() > 400) {
            m_recordRefresh.restart();
            m_recordViewStart = 0;
            m_recordViewLength = m_stream->recorded();
            refreshRecordView();
        }
        return;
    }

    if (m_stream->frozen())
        return;

    // m_liveViewOffset is only ever nonzero while frozen (panning is gated
    // on Freeze — see the panRequested handler), so this always reads
    // exactly the newest window here, same as before this function existed.
    // Spaced to at least 3x what the last refresh cost (33 ms when cheap):
    // with very long windows, several channels and Math a refresh can still
    // be expensive, and back-to-back refreshes left the interface no time
    // for anything else. This keeps at least two thirds of it free.
    QElapsedTimer refreshClock;
    refreshClock.start();
    refreshLiveView();
    {
        const int spacing = std::clamp(int(3 * refreshClock.elapsed()), 33, 1000);
        if (m_streamTick->interval() != spacing)
            m_streamTick->setInterval(spacing);
    }
}

// Live's counterpart to refreshRecordView(): reads a window starting
// m_liveViewOffset samples before the newest sample instead of always the
// newest window, so a frozen display can be panned back through whatever
// the ring still holds. ingest() itself stops appending once frozen (see
// StreamEngine), so the ring is static while panned and an offset stays
// meaningful for as long as Freeze stays on — no drift to account for.
void MainWindow::refreshLiveView()
{
    VSX_TRACE("refreshLiveView");
    // A loaded .vsxwave is already fully in ScopeView and is windowed by it
    // directly (see m_liveLoaded); reading the ring here would replace it
    // with whatever the stream last held, or nothing.
    if (m_liveLoaded) {
        syncNavigator();
        refreshCards();
        return;
    }
    // ScopeView's own current secondsPerDiv, not the timebase dropdown's
    // nominal value — the two diverge the moment zoomToRect() (the drag-a-box
    // zoom) narrows the actual displayed window, since that only ever
    // touches ScopeView's own state and never updates the dropdown. Reading
    // the dropdown here after a zoom meant fetching a window sized for
    // whatever the dropdown still claimed rather than what was actually on
    // screen — see the panRequested handler below for what that mismatch
    // did to panning specifically.
    const double perDiv = m_view->secondsPerDiv();
    const double window = perDiv * ScopeView::HorizontalDivs;
    const double rate   = m_stream->sampleRate();
    const int wanted    = std::clamp(int(window * rate), 2, 4 * 1024 * 1024);
    {
        // The Live setup, once whenever it changes (for the timing log).
        int on = 0;
        for (int ch = 0; ch < m_stream->channels(); ++ch)
            on += m_channel[ch].mode->checkedId() != ChannelOff ? 1 : 0;
        const bool math = m_mathEnabled && m_mathEnabled->isChecked();
        static int lastWanted = -1, lastOn = -1;
        static double lastRate = -1.0;
        static bool lastMath = false;
        if (wanted != lastWanted || rate != lastRate || on != lastOn || math != lastMath) {
            lastWanted = wanted; lastRate = rate; lastOn = on; lastMath = math;
            if (PerfTrace::enabled())
                std::fprintf(stderr, "[live] window %d samples per channel (%.3g s at %.4g S/s), %d channel%s on, Math %s\n",
                         wanted, window, rate, on, on == 1 ? "" : "s", math ? "on" : "off");
            std::fflush(stderr);
        }
    }

    QVector<float> block;
    QVector<double> samples;
    bool any = false;
    quint64 available = 0;

    // Long windows: the plot gets min/max pairs instead of every sample.
    // Measured (50 s at 5 MS/s, 2 channels + Math): ~280 ms per refresh, every
    // step — filter, Math, conversion, readouts, drawing — working through
    // 4M samples per channel for a plot ~1500 px wide. Reduced to 8192 pairs
    // per channel (peaks and glitches kept) like Record's pages; measurements
    // stay exact on the full-rate data (refreshed at most 4x/s); filters run
    // before reducing. Not while decoding, the engine tools or CAN health
    // are on: those analyse individual samples.
    constexpr int kLiveFullResMax = 1 << 20;
    constexpr int kLivePairs = 8192;
    bool needsEverySample = (m_engineEnable && m_engineEnable->isChecked())
                            || (m_compressionEnable && m_compressionEnable->isChecked())
                            || (m_canHealthEnable && m_canHealthEnable->isChecked());
    for (const DecodeSlotUi &slot : m_decodeUi)
        needsEverySample = needsEverySample || (slot.enable && slot.enable->isChecked());
    // The whole window, uncapped (wanted is capped at 4M for the full-rate path).
    const qint64 windowSamples = std::max<qint64>(2, qint64(std::min(window * rate, 1e15)));
    const bool reduce = windowSamples > kLiveFullResMax && !needsEverySample;
    if (reduce != m_liveReduced) {
        m_liveReduced = reduce;
        m_view->setLowPassInView(!reduce);   // filtering happens before reducing
        m_view->setTraceGaps(reduce);        // whole-window frames: no data before the history
        m_view->setSamplesArePairs(reduce);
        if (!reduce) {
            m_view->clearMeasurementSources();
            m_view->clearMeasurementOverrides();
        }
        for (QElapsedTimer &clock : m_liveMeasureClock)
            clock.invalidate();
    }
    if (reduce) {
        // The worker reads, filters, reduces and measures; this thread only
        // shows the finished frame (applyLiveResult()). One at a time: while
        // one is being prepared, refreshes are skipped.
        if (m_liveJobInFlight) {
            m_liveJobPending = true;   // the newest request runs as soon as this frame lands
            return;
        }
        LiveJob job;
        job.wanted = windowSamples;
        job.offset = m_liveViewOffset;
        job.rate = rate;
        job.pairs = kLivePairs;
        if (!m_liveOverviewRefresh.isValid() || m_liveOverviewRefresh.elapsed() > 400) {
            job.overview = true;
            m_liveOverviewRefresh.restart();
        }
        for (int ch = 0; ch < m_stream->channels() && ch < 4; ++ch) {
            if (m_channel[ch].mode->checkedId() == ChannelOff)
                continue;
            const ChannelControls &cc = m_channel[ch];
            LiveJob::Channel jc;
            jc.ch = ch;
            jc.filter = cc.filterType ? cc.filterType->currentData().toInt() : 0;
            jc.lowHz = filterLowParam(ch);
            jc.highHz = cc.filterHighHz ? cc.filterHighHz->value() : 0.0;
            jc.gain = m_view->measureGain(ch);
            jc.bias = m_view->measureBias(ch);
            // While panning: no readouts (the full-rate measurement is most
            // of a frame's cost); they follow once the view settles.
            const bool panning = m_livePanClock.isValid() && m_livePanClock.elapsed() < 250;
            jc.measure = m_view->anyMeasurementActive(ch) && !panning;
            if (panning && m_view->anyMeasurementActive(ch) && !m_liveMeasureFollowUp) {
                m_liveMeasureFollowUp = true;
                QTimer::singleShot(300, this, [this] {
                    m_liveMeasureFollowUp = false;
                    if (m_acquisitionMode == ModeStream && m_liveReduced)
                        refreshLiveView();
                });
            }
            job.channels.push_back(jc);
        }
        if (job.channels.isEmpty())
            return;
        m_liveJobInFlight = true;
        LiveWorker *worker = m_liveWorker;
        QMetaObject::invokeMethod(worker, [this, worker, job] {
            LiveResult result = worker->process(job);
            QMetaObject::invokeMethod(this, [this, result] { applyLiveResult(result); },
                                      Qt::QueuedConnection);
        }, Qt::QueuedConnection);
        return;
    }

    for (int ch = 0; ch < m_stream->channels(); ++ch) {
        if (m_channel[ch].mode->checkedId() == ChannelOff)
            continue;
        available = std::max(available, m_stream->historyAvailable(ch));

        int got = 0;
        {
            VSX_TRACE("live.read");
            got = m_stream->rangeFromNewest(ch, m_liveViewOffset, wanted, &block);
        }
        if (got <= 0)
            continue;

        // ScopeView still speaks double. Converting here keeps the ring buffer
        // half the size; if this ever shows up in a profile, ScopeView should
        // take float instead.
        const double shownRate = rate;
        {
            {
                VSX_TRACE("live.convert");
                samples.resize(got);
                for (int i = 0; i < got; ++i)
                    samples[i] = double(block[i]);
            }
            {
                VSX_TRACE("live.setSamples");
                m_view->setSamples(ch, samples, rate, -1.0);
            }
            if (m_softwareAc[ch])
                trackSoftwareAc(ch, samples);
            {
                VSX_TRACE("live.feedSpectrumXY");
                feedSpectrum(ch, samples, rate);
                feedXY(ch, samples);
            }
        }
        m_liveSnapshot.samples[ch] = samples;
        m_liveSnapshot.valid[ch] = true;
        m_liveSnapshot.sampleRate = shownRate;
        m_liveSnapshot.hasData = true;
        any = true;
    }

    if (!any)
        return;

    // The overview strip needs the WHOLE available history, not the
    // windowed `samples` above — feeding it that was exactly what left the
    // overview showing the same thing as the main plot instead of an
    // actual overview. Decimating the full history on every call, which
    // runs every 33ms (m_streamTick), is unnecessary work an overview
    // strip does not need refreshed at, so this is throttled to roughly
    // Record's own 400ms cadence instead (see m_liveOverviewRefresh).
    if (!m_liveOverviewRefresh.isValid() || m_liveOverviewRefresh.elapsed() > 400) {
        VSX_TRACE("live.overview");
        m_liveOverviewRefresh.restart();
        // Walks the WHOLE history in bounded chunks rather than reading only
        // the newest N samples: the viewport box below is positioned against
        // the full history length, so an overview covering less than that
        // (high rates, long history) put the box in the wrong place. Exact
        // min/max per bucket, with memory bounded by the chunk size.
        constexpr int kOverviewBuckets = 1000;
        constexpr quint64 kChunk = 1024 * 1024;
        constexpr quint64 kMaxBlocks = 8192;
        QVector<float> chunk;
        QVector<float> lo(kOverviewBuckets), hi(kOverviewBuckets);
        QVector<char> seen(kOverviewBuckets);
        for (int ch = 0; ch < m_stream->channels(); ++ch) {
            if (m_channel[ch].mode->checkedId() == ChannelOff)
                continue;
            const quint64 chAvailable = m_stream->historyAvailable(ch);
            if (chAvailable == 0)
                continue;
            seen.fill(0);
            // Incremental: min/max per fixed-size block, reading only what
            // arrived since the last refresh. Walking the whole history
            // every 400 ms (the first version) grew with it — 140 ms
            // measured at 1 MS/s — and made Live sluggish.
            LiveOverviewCache &cache = m_liveOverview[size_t(ch)];
            const quint64 written = m_stream->written(ch);
            const quint64 capacity = std::max<quint64>(1, m_stream->historyCapacity(ch));
            if (written < cache.processed || capacity != cache.capacity) {   // restarted / resized
                cache = LiveOverviewCache();
                cache.capacity = capacity;
                cache.blockSize = std::max<quint64>(1, (capacity + kMaxBlocks - 1) / kMaxBlocks);
                cache.processed = written - std::min(written, chAvailable);
            }
            quint64 fresh = written - cache.processed;
            if (fresh > chAvailable) {   // more arrived than the history holds
                cache.blocks.clear();
                cache.partialCount = 0;
                fresh = chAvailable;
            }
            quint64 done = 0;   // new samples consumed, oldest first
            while (done < fresh) {
                const quint64 count = std::min(kChunk, fresh - done);
                const quint64 offset = fresh - done - count;   // of this chunk's newest sample
                const int got = m_stream->rangeFromNewest(ch, offset, int(count), &chunk);
                if (got <= 0)
                    break;
                for (int i = 0; i < got; ++i) {
                    const float v = chunk[i];
                    if (cache.partialCount == 0) {
                        cache.partialMin = cache.partialMax = v;
                    } else {
                        cache.partialMin = std::min(cache.partialMin, v);
                        cache.partialMax = std::max(cache.partialMax, v);
                    }
                    if (++cache.partialCount == cache.blockSize) {
                        cache.blocks.emplace_back(cache.partialMin, cache.partialMax);
                        cache.partialCount = 0;
                    }
                }
                done += quint64(got);
            }
            cache.processed = written;
            // Only the blocks still inside the history.
            const size_t keep = size_t(chAvailable / cache.blockSize + 1);
            while (cache.blocks.size() > keep)
                cache.blocks.pop_front();
            const size_t n = cache.blocks.size() + (cache.partialCount ? 1 : 0);
            for (size_t k = 0; k < n; ++k) {
                const bool whole = k < cache.blocks.size();
                const float bMin = whole ? cache.blocks[k].first : cache.partialMin;
                const float bMax = whole ? cache.blocks[k].second : cache.partialMax;
                const int b = int(quint64(k) * kOverviewBuckets / quint64(n));
                if (!seen[b]) {
                    lo[b] = bMin;
                    hi[b] = bMax;
                    seen[b] = 1;
                } else {
                    lo[b] = std::min(lo[b], bMin);
                    hi[b] = std::max(hi[b], bMax);
                }
            }

            QVector<double> overviewSamples;
            overviewSamples.reserve(kOverviewBuckets * 2);
            for (int b = 0; b < kOverviewBuckets; ++b) {
                if (!seen[b])
                    continue;
                overviewSamples.append(double(lo[b]));
                overviewSamples.append(double(hi[b]));
            }
            if (!overviewSamples.isEmpty())
                m_nav->setSamples(ch, overviewSamples);
        }
    }

    // Live has no trigger to centre the axis on the way a capture does, so
    // "now" (the newest sample) is used as the one meaningful reference
    // point instead, via setAbsoluteTimeOrigin() — Record's own mechanism
    // for a plot with nothing else to centre on. The window's left edge
    // sits `wanted` samples before the pan point, which is `wanted +
    // m_liveViewOffset` samples before "now", expressed as a negative time
    // since everything shown here already happened. This is set
    // unconditionally, every time, including at m_liveViewOffset == 0 —
    // conditionally falling back to the ordinary trigger-centred axis
    // there (as an earlier version of this fix did) put "now" at a
    // completely different reference point (screen centre, an arbitrary
    // position with no meaning for a trigger-less signal) than this
    // formula does (the right edge) the instant any pan at all began,
    // which is a full half-screen discontinuity right at the pan boundary
    // — exactly what "the grid isn't following" looks like in practice.
    finishLiveRefresh(wanted, rate, available);
}

// The end of a Live refresh, shared by the full-rate path and the worker's
// frames: the time axis ("now" at the right edge), the navigator viewport,
// the cards.
void MainWindow::finishLiveRefresh(qint64 wanted, double rate, quint64 available)
{
    m_view->setAbsoluteTimeOrigin(-(double(wanted) + double(m_liveViewOffset)) / rate);

    // Mirrors refreshRecordView()'s own direct navigator sync rather than the
    // generic syncNavigator(): the pan offset lives here, in MainWindow, not
    // in ScopeView's own follow/offset state (Live's pushed buffer is always
    // exactly one screen's worth, with no slack for ScopeView's own
    // windowing to pan within), so ScopeView has nothing meaningful to
    // report through viewportCentreFraction()/isFollowingTrigger() for it.
    if (available > 0) {
        const double span   = std::min<double>(wanted, available);
        const double centre = (double(available) - double(m_liveViewOffset) - span / 2.0)
                             / double(available);
        m_nav->setViewport(centre, span / available);
        m_nav->setTriggerFraction(-1.0);
        m_nav->setFollowingTrigger(m_liveViewOffset == 0);
    }

    refreshCards();
    updateStatusBar();
}

// A frame prepared by Live's worker: shown as is. Ignored if Live has moved on
// (another mode, a loaded file, or no longer a reduced window).
void MainWindow::applyLiveResult(const LiveResult &result)
{
    VSX_TRACE("live.applyFrame");
    m_liveJobInFlight = false;
    if (m_acquisitionMode != ModeStream || !m_liveReduced || m_liveLoaded) {
        m_liveJobPending = false;
        return;
    }
    if (m_liveJobPending) {
        // A newer position was asked for meanwhile: show this frame, then
        // fetch that one straight away (the last drag position always lands).
        m_liveJobPending = false;
        QTimer::singleShot(0, this, [this] { refreshLiveView(); });
    }
    for (const LiveResult::Channel &c : result.channels) {
        if (c.ch < 0 || c.ch >= m_channelCount || m_channel[c.ch].mode->checkedId() == ChannelOff)
            continue;
        m_view->setSamples(c.ch, c.pairs, c.shownRate, -1.0);
        if (c.measured)
            m_view->setMeasurementOverride(c.ch, c.measurements);
        if (m_softwareAc[c.ch])
            trackSoftwareAc(c.ch, c.pairs);
        feedSpectrum(c.ch, c.tail, result.rate);
        feedXY(c.ch, c.tail);
        if (!c.overview.isEmpty())
            m_nav->setSamples(c.ch, c.overview);
        m_liveSnapshot.samples[c.ch] = c.pairs;
        m_liveSnapshot.valid[c.ch] = true;
        m_liveSnapshot.sampleRate = c.shownRate;
        m_liveSnapshot.hasData = true;
    }
    finishLiveRefresh(result.wanted, result.rate, result.available);
}

// Rebuilds the tab list from whatever recordings actually exist on disk
// right now — never assumed to already match, since a recording could have
// been added, finished, or deleted (Temporary, on a previous exit) since the
// last time this ran. Deliberately does not itself load anything: it only
// selects a tab (blocked from emitting currentChanged while doing so), so a
// tab landing on the recording already actively being written is inert
// rather than an accidental reopen of it. Callers that want the selected
// recording actually loaded (entering Record mode with past recordings on
// hand) do that themselves afterward.
// Persists which recordings have been closed by hand — read back by
// refreshRecordTabs() so a closed tab stays closed across app restarts
// too, not just for the rest of this session, until something explicitly
// un-hides it again (Open, currently the only thing that does). Read and
// written directly against QSettings each time, the same "no separate
// cache to keep in sync" choice recordDirectory() already makes.
void MainWindow::setRecordTabHidden(const QString &directory, bool hidden)
{
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    QStringList list = settings.value(QStringLiteral("records/hiddenTabs")).toStringList();
    const bool alreadyHidden = list.contains(directory);
    if (hidden && !alreadyHidden) {
        list.append(directory);
        settings.setValue(QStringLiteral("records/hiddenTabs"), list);
    } else if (!hidden && alreadyHidden) {
        list.removeAll(directory);
        settings.setValue(QStringLiteral("records/hiddenTabs"), list);
        // Brought back: it goes on the right, not where it used to be.
        QStringList order = settings.value(QStringLiteral("records/tabOrder")).toStringList();
        order.removeAll(directory);
        settings.setValue(QStringLiteral("records/tabOrder"), order);
    }
}

// The one place that actually switches which recording is being viewed.
// Previously this logic was duplicated three times (entering Record mode
// with past recordings on hand, clicking a tab, Open) each doing its own
// slightly different version of "call openRecordForReading(), reset the
// view, refresh" — consolidating them here means there is exactly one
// place that can get the sequence wrong, and exactly one place that saves
// and restores each recording's own view state.
void MainWindow::loadRecordTab(const QString &directory)
{
    VSX_TRACE("loadRecordTab");
    if (directory.isEmpty() || directory == m_currentRecordTab)
        return;   // already showing this one

    // Save the outgoing recording's own view state before leaving it, so
    // switching back later restores exactly where it was left, not a
    // fresh default view every time.
    if (!m_currentRecordTab.isEmpty()) {
        RecordTabViewState &state = m_recordTabStates[m_currentRecordTab];
        state.viewStart = m_recordViewStart;
        state.viewLength = m_recordViewLength;
        for (int ch = 0; ch < 4; ++ch) {
            state.channels[ch].voltsPerDiv = m_view->voltsPerDiv(ch);
            state.channels[ch].positionVolts = m_view->positionVolts(ch);
            state.channels[ch].visible = m_view->channelVisible(ch);
        }
    }

    QString error;
    if (!m_stream->openRecordForReading(directory, &error)) {
        statusBar()->showMessage(error, 4000);
        return;
    }
    // Park the outgoing recording's zoom history and bring back this one's
    // (empty if it was never zoomed), so the arrows only ever step through
    // the recording on screen.
    if (!m_currentRecordTab.isEmpty())
        m_recordZoomHistory[m_currentRecordTab] = { m_zoomBack[ModeRecord], m_zoomForward[ModeRecord] };
    {
        const auto it = m_recordZoomHistory.constFind(directory);
        m_zoomBack[ModeRecord] = it != m_recordZoomHistory.constEnd() ? it->first : QVector<ZoomState>();
        m_zoomForward[ModeRecord] = it != m_recordZoomHistory.constEnd() ? it->second : QVector<ZoomState>();
    }
    m_currentRecordTab = directory;
    updateZoomHistoryButtons();
    syncAnnotationContext();

    // Browsing a recording with no scope connected: take the channel count
    // from the recording, and turn on every channel it holds. Its on/off
    // buttons otherwise keep their last saved state, and refreshRecordView()
    // skips any channel that is off — CH2 is often off, which hid half of a
    // two-channel recording with nothing to say it was there. With a scope
    // connected, the scope's count and the person's choices stand as usual.
    if (!m_acq->ready()) {
        const unsigned short mask = m_stream->recordChannelMask();
        int fileChannels = 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (mask & (1u << ch))
                fileChannels = ch + 1;
        }
        adoptChannelCountWithoutDevice(fileChannels);
        for (int ch = 0; ch < fileChannels; ++ch) {
            if (!(mask & (1u << ch)) || m_channel[ch].mode->checkedId() != ChannelOff)
                continue;
            if (auto *manual = m_channel[ch].mode->button(ChannelManual))
                manual->setChecked(true);
            // setChecked() alone skips the on/off handler (visibility,
            // overview, cards), which only listens to idClicked.
            emit m_channel[ch].mode->idClicked(ChannelManual);
        }
    }

    applyRecordTabViewState(directory);
}

// The half of loadRecordTab() that still needs to run even when the tab
// itself has not changed at all: this recording's own saved per-channel
// scale, position, and visibility (or fresh defaults, on a first-ever
// view), plus the last-viewed span, then a redraw. Split out because
// ScopeView's shared state can be overwritten by a trip through Capture
// or Live even when m_currentRecordTab never changed — entering Record
// mode after adjusting something in another mode needs this reapplied
// regardless, which is exactly why setAcquisitionMode() calls this
// directly for that case, bypassing loadRecordTab()'s own "already
// showing this one" guard (which exists for the ordinary in-mode tab
// click, where nothing else could have touched the display in between).
void MainWindow::applyRecordTabViewState(const QString &directory)
{
    const auto it = m_recordTabStates.constFind(directory);
    if (it != m_recordTabStates.constEnd()) {
        m_recordViewStart = it->viewStart;
        m_recordViewLength = it->viewLength;
        for (int ch = 0; ch < 4; ++ch) {
            if (it->channels[ch].voltsPerDiv > 0.0)
                m_view->setVoltsPerDiv(ch, it->channels[ch].voltsPerDiv);
            // Through setChannelPositionPercent(), not a direct
            // setPositionVolts() call — see the identical comment in
            // setAcquisitionMode()'s own Capture/Live restore for why:
            // this keeps the offsetPct spinbox in sync too, which is what
            // offsetDragged's handler actually bases the next drag on.
            const double page = m_channel[ch].voltsPage->currentData().toDouble();
            const double pct = page > 0.0
                ? it->channels[ch].positionVolts / page * 100.0 : 0.0;
            setChannelPositionPercent(ch, pct);
            m_view->setChannelVisible(ch, it->channels[ch].visible);
        }
    } else {
        m_recordViewStart = 0;
        m_recordViewLength = 0;
        for (int ch = 0; ch < 4; ++ch)
            m_view->setChannelVisible(ch, true);
    }

    refreshRecordView();
}

// The selected tab's background is the same bright accent green every other
// checked control in this app uses, and the close button's default light
// icon reads very poorly against it — the same reason the Instruments mode
// segments already swap their own icon to black when checked. Run whenever
// the tab list is rebuilt or the selection itself changes, since either can
// change which tab's button needs which colour.
void MainWindow::updateRecordTabCloseIcons()
{
    if (!m_recordTabs)
        return;
    const int current = m_recordTabs->currentIndex();
    for (int i = 0; i < m_recordTabs->count(); ++i) {
        auto *widget = m_recordTabs->tabButton(i, QTabBar::RightSide);
        // The close button now sits inside a small margin container (see
        // refreshRecordTabs()) rather than being the tabButton() widget
        // itself, so it has to be found as a child rather than cast
        // directly.
        auto *button = qobject_cast<QToolButton *>(widget);
        if (!button && widget)
            button = widget->findChild<QToolButton *>();
        if (!button)
            continue;
        const bool selected = (i == current);
        const QIcon icon = Icons::icon(QStringLiteral("xmark"),
                                       selected ? Theme::onAccent() : Theme::accentSoft(), 12);
        if (!icon.isNull())
            button->setIcon(icon);
    }
}

void MainWindow::refreshRecordTabs()
{
    VSX_TRACE("refreshRecordTabs");
    if (!m_recordTabs)
        return;

    QVector<RecordWriter::RecordInfo> recordings =
        RecordWriter::listRecordings(recordDirectory());
    for (const QString &opened : std::as_const(m_openedRecordings)) {
        bool listed = false;
        for (const RecordWriter::RecordInfo &info : recordings)
            listed = listed || QFileInfo(info.directory).absoluteFilePath() == opened;
        RecordWriter::RecordInfo info;
        if (!listed && RecordWriter::recordingInfo(opened, &info)) {
            info.directory = opened;
            recordings.append(info);
        }
    }

    // Tabs keep their order: ones already shown keep their place, anything
    // new — a fresh recording, or one brought back with Open — goes on the
    // right (it used to land at its date's position, often in the middle).
    {
        QSettings orderSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
        const QStringList order = orderSettings.value(QStringLiteral("records/tabOrder")).toStringList();
        std::stable_sort(recordings.begin(), recordings.end(),
                         [&](const RecordWriter::RecordInfo &a, const RecordWriter::RecordInfo &b) {
                             const int ia = int(order.indexOf(a.directory)), ib = int(order.indexOf(b.directory));
                             return (ia < 0 ? INT_MAX : ia) < (ib < 0 ? INT_MAX : ib);
                         });
    }
    // Read once, not per-recording inside the loop below — a closed tab
    // stays closed across restarts until Open explicitly brings it back.
    QSettings hiddenSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    const QStringList hiddenList =
        hiddenSettings.value(QStringLiteral("records/hiddenTabs")).toStringList();
    const QSet<QString> hidden(hiddenList.begin(), hiddenList.end());
    {
        QStringList shownOrder;
        for (const RecordWriter::RecordInfo &info : recordings) {
            if (!hidden.contains(info.directory))
                shownOrder << info.directory;
        }
        hiddenSettings.setValue(QStringLiteral("records/tabOrder"), shownOrder);
    }

    m_recordTabs->blockSignals(true);
    while (m_recordTabs->count() > 0)
        m_recordTabs->removeTab(0);

    int selectIndex = -1;
    for (const RecordWriter::RecordInfo &info : recordings) {
        if (hidden.contains(info.directory))
            continue;
        const int index = m_recordTabs->addTab(info.label);
        m_recordTabs->setTabData(index, info.directory);
        m_recordTabs->setTabToolTip(index, QFileInfo(info.directory).fileName());

        // A real Font Awesome glyph, the same one and colour the detail
        // panel's own close button already uses, rather than Qt's built-in
        // close-button rendering. Only ever removes the tab itself — the
        // recording's files are untouched, closing a tab is not deleting
        // its data. The directory is captured by value so this stays
        // correct even after later rebuilds shift every tab's index.
        auto *closeButton = new QToolButton(m_recordTabs);
        const QIcon closeIcon = Icons::icon(QStringLiteral("xmark"), Theme::accentSoft(), 12);
        if (closeIcon.isNull())
            closeButton->setText(QStringLiteral("\u00d7"));
        else
            closeButton->setIcon(closeIcon);
        closeButton->setCursor(Qt::PointingHandCursor);
        closeButton->setAutoRaise(true);
        closeButton->setFixedSize(18, 18);
        closeButton->setToolTip(QStringLiteral("Close this tab (keeps the recording on disk)"));
        const QString dirForClose = info.directory;
        connect(closeButton, &QToolButton::clicked, this, [this, dirForClose] {
            if (!m_recordTabs)
                return;
            for (int i = 0; i < m_recordTabs->count(); ++i) {
                if (m_recordTabs->tabData(i).toString() == dirForClose) {
                    m_recordTabs->removeTab(i);
                    break;
                }
            }
            setRecordTabHidden(dirForClose, true);
            // If it was the shown tab, the switch to its neighbour (via
            // currentChanged) has already parked its history here.
            if (m_openedRecordings.removeAll(QFileInfo(dirForClose).absoluteFilePath()) > 0) {
                QSettings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"))
                    .setValue(QStringLiteral("records/opened"), m_openedRecordings);
            }
            m_recordZoomHistory.remove(dirForClose);
            updateRecordTabCloseIcons();
        });
        // A small container with its own right margin, rather than the
        // button passed to setTabButton() directly: QTabBar positions that
        // widget flush against the tab's own edge regardless of the tab's
        // QSS padding (that padding governs the text label, not this one),
        // which is why increasing it alone never actually moved the
        // button — this gives direct control over the gap instead.
        auto *closeContainer = new QWidget(m_recordTabs);
        auto *closeLayout = new QHBoxLayout(closeContainer);
        closeLayout->setContentsMargins(0, 0, 4, 0);
        closeLayout->setSpacing(0);
        closeLayout->addWidget(closeButton);
        m_recordTabs->setTabButton(index, QTabBar::RightSide, closeContainer);

        // m_currentRecordTab, not "whichever recording was most recently
        // started" (the previous approach here, before that idea was
        // replaced entirely by a list of every Temporary recording made
        // this session — see m_temporaryRecordings): relying on "most
        // recently started" left the visibly-selected tab mismatched
        // against whatever was actually being displayed any time Open
        // loaded a recording
        // that was not made this session.
        if (info.directory == m_currentRecordTab)
            selectIndex = index;
    }
    // Falls back to the most recent recording (listRecordings() is
    // oldest-first) whenever nothing here matches whatever is currently
    // being viewed — including the ordinary case of entering Record mode
    // with nothing loaded yet at all.
    if (selectIndex < 0 && m_recordTabs->count() > 0)
        selectIndex = m_recordTabs->count() - 1;
    if (selectIndex >= 0)
        m_recordTabs->setCurrentIndex(selectIndex);
    m_recordTabs->blockSignals(false);
    updateRecordTabCloseIcons();
}

void MainWindow::refreshRecordView()
{
    VSX_TRACE("refreshRecordView");
    // No more than this many points reach the plot. A record can be hundreds of
    // millions of samples; the screen is a couple of thousand pixels.
    constexpr int kMaxPlotPoints = 4000;

    const quint64 total = m_stream->recorded();
    if (total == 0)
        return;
    if (m_recordViewLength == 0)
        m_recordViewLength = total;

    if (m_overviewCacheTab != m_currentRecordTab || m_overviewCacheTotal != total) {
        m_overviewCacheTab = m_currentRecordTab;
        m_overviewCacheTotal = total;
        m_overviewCacheValid.fill(false);
    }

    m_recordViewStart = std::min(m_recordViewStart, total - 1);
    const quint64 span = std::min<quint64>(m_recordViewLength,
                                           total - m_recordViewStart);
    if (span < 2)
        return;

    const double rate = m_stream->sampleRate();
    if (rate <= 0.0)
        return;

    const double duration = double(span) / rate;
    const int points = int(std::min<quint64>(span, kMaxPlotPoints));
    m_view->setSamplesArePairs(span > quint64(points));   // a decimated page: min / max pairs

    // The axis describes the slice on screen, so while recording it grows with
    // the record and stops wherever the record stops. The rate shown is always
    // the real acquisition rate — a synthetic "points over duration" figure
    // here previously made the label drift downward as the record grew, as if
    // the hardware were slowing down.
    m_view->setSecondsPerDiv(duration / ScopeView::HorizontalDivs);

    QVector<float> block;
    QVector<double> samples;
    bool any = false;

    // The RECORDING's own channels, not the live channel rail's current
    // on/off state — a recording made with CH1+CH2 must show CH1+CH2 when
    // browsed later, regardless of whatever the rail happens to be set to
    // at that moment (it could easily differ: another recording since,
    // or a channel toggled by hand). Reading the rail here instead was
    // the actual cause of a browsed recording appearing to show nothing at
    // all whenever its channels did not happen to match the rail's current
    // state — which a fresh app launch's default rail state, or simply a
    // later recording's own different mask, made the common case rather
    // than a rare one. The loop bound is RecordWriter::MaxChannels, not
    // m_stream->channels(): the latter is only ever set by attach(), which
    // only runs when a device is actually connected and ready — browsing a
    // recording from disk has nothing to do with whether hardware is
    // attached at all, and needs to work identically whether it is or not.
    // Low-pass in Record mode (the view doesn't filter here — see
    // ScopeView::setLowPassInView()): the raw samples behind the view are
    // read, filtered at the true record rate, and decimated into the page
    // like readDecimated() would. Bounded so a long view stays responsive;
    // beyond it the page is unfiltered and the status bar says so, while
    // measurements (always from raw, below) still see the filter.
    constexpr quint64 kFilterRecordMax = 4ull * 1024 * 1024;
    bool lowPassSkipped = false;
    const unsigned short recordMask = m_stream->recordChannelMask();
    for (int ch = 0; ch < RecordWriter::MaxChannels; ++ch) {
        // Shown only if BOTH the recording actually has this channel AND
        // it is not explicitly off on the rail right now — an explicit off
        // is a deliberate, current choice that should never be silently
        // overridden by a recording that happens to have this channel's
        // data, the same reasoning Capture's and Live's own snapshot
        // restores now apply too.
        if (!(recordMask & (1u << ch)) || m_channel[ch].mode->checkedId() == ChannelOff) {
            // Explicitly cleared, not just skipped — otherwise a channel
            // shown by whichever recording was browsed previously keeps
            // showing its old, now-unrelated trace as a stale overlay on
            // top of this one. The overview strip too: it kept the channel
            // another mode had shown (Capture's CH2 over a CH1-only
            // recording), since only recorded channels were ever set on it.
            m_view->clearChannel(ch);
            m_nav->setSamples(ch, {});
            continue;
        }
        const int got = m_stream->readDecimated(ch, m_recordViewStart, span,
                                                points, &block);
        if (got <= 0)
            continue;
        samples.resize(got);
        for (int i = 0; i < got; ++i)
            samples[i] = double(block[i]);

        // ScopeView's internal windowing (windowLength(), frequency, trigger
        // offset) assumes buffer index / sampleRate = seconds, so it needs the
        // rate scaled down by the ACTUAL decimation ratio applied — `got` may
        // be less than the requested `points` (e.g. a short final bucket, or
        // the exact-read fallback), so this must use `got`, not the nominal
        // target, or the axis would describe a span that does not match what
        // is actually in the buffer. This is bookkeeping for ScopeView's own
        // math, not a real change in acquisition rate — the status strip is
        // told the true rate separately below rather than shrinking as the
        // record grows.
        const int filterType = m_channel[ch].filterType ? m_channel[ch].filterType->currentData().toInt() : 0;
        const bool lowPass = filterType != ScopeView::FilterOff;   // any filter
        const double lowHz = filterLowParam(ch);
        const double highHz = m_channel[ch].filterHighHz ? m_channel[ch].filterHighHz->value() : 0.0;
        if (lowPass && span > kFilterRecordMax) {
            // Too wide to filter directly on every pan: drawn from the
            // recording's filtered overview instead (min/max per page
            // bucket), unless it's still being recorded.
            if (ensureFilteredOverview(ch, filterType, lowHz, highHz)) {
                const FilteredOverview &ov = m_filteredOverview[size_t(ch)];
                const qint64 first = qint64(m_recordViewStart / quint64(ov.factor));
                const qint64 last = std::min<qint64>(ov.points,
                    qint64((m_recordViewStart + span) / quint64(ov.factor)) + 1);
                const qint64 count = std::max<qint64>(1, last - first);
                const int buckets = std::max(1, got / 2);
                for (int b = 0; b < buckets; ++b) {
                    qint64 a = first + count * b / buckets;
                    qint64 e = first + count * (b + 1) / buckets;
                    a = std::clamp<qint64>(a, 0, ov.points - 1);
                    e = std::clamp<qint64>(std::max(e, a + 1), a + 1, ov.points);
                    if (filterType == ScopeView::FilterHighPass || filterType == ScopeView::FilterBandStop) {
                        // Exact high-pass / band-stop min/max per overview point.
                        float mn = ov.data[int(2 * a)], mx = ov.data[int(2 * a + 1)];
                        for (qint64 i = a; i < e; ++i) {
                            mn = std::min(mn, ov.data[int(2 * i)]);
                            mx = std::max(mx, ov.data[int(2 * i + 1)]);
                        }
                        samples[2 * b] = double(mn);
                        if (2 * b + 1 < got)
                            samples[2 * b + 1] = double(mx);
                        continue;
                    }
                    float mn = ov.data[int(a)], mx = mn;
                    for (qint64 i = a; i < e; ++i) {
                        mn = std::min(mn, ov.data[int(i)]);
                        mx = std::max(mx, ov.data[int(i)]);
                    }
                    samples[2 * b] = double(mn);
                    if (2 * b + 1 < got)
                        samples[2 * b + 1] = double(mx);
                }
                if (got % 2)
                    samples[got - 1] = samples[got - 2];
            } else {
                lowPassSkipped = true;
            }
        } else if (lowPass) {
            QVector<float> rawBlock;
            const int rawGot = m_stream->read(ch, m_recordViewStart, int(span), &rawBlock);
            if (rawGot > 0) {
                QVector<double> raw(rawGot);
                for (int i = 0; i < rawGot; ++i)
                    raw[i] = double(rawBlock[i]);
                raw = ScopeView::applyFilter(raw, rate, filterType, lowHz, highHz);
                if (rawGot == got) {
                    samples = raw;               // the page was full resolution anyway
                } else {
                    // Same shape as readDecimated(): a min/max pair per bucket,
                    // so the page keeps its point count and windowing rate.
                    const int buckets = std::max(1, got / 2);
                    for (int b = 0; b < buckets; ++b) {
                        const int a = int(qint64(b) * rawGot / buckets);
                        const int e = std::max(a + 1, int(qint64(b + 1) * rawGot / buckets));
                        const auto mm = std::minmax_element(raw.cbegin() + a,
                                                            raw.cbegin() + std::min(e, rawGot));
                        samples[2 * b] = *mm.first;
                        if (2 * b + 1 < got)
                            samples[2 * b + 1] = *mm.second;
                    }
                    if (got % 2)
                        samples[got - 1] = raw.last();
                }
            }
        }

        const double windowingRate = double(got) / duration;
        m_view->setSamples(ch, samples, windowingRate, -1.0);
        // Measurements from raw samples, not this decimated page — measuring
        // the page gave a "frequency" that was really how often its min/max
        // pairs alternate (hundreds of Hz for a 1 kHz wave, rising with
        // zoom). Up to 1M samples per channel from the middle of the view:
        // seconds of signal at typical record rates, and a few MB from disk.
        {
            constexpr quint64 kMeasureMax = 1024 * 1024;
            const quint64 count = std::min<quint64>(span, kMeasureMax);
            const quint64 first = m_recordViewStart + (span - count) / 2;
            QVector<float> raw;
            const int rawGot = m_stream->read(ch, first, int(count), &raw);
            QVector<double> rawSamples(std::max(rawGot, 0));
            for (int i = 0; i < rawSamples.size(); ++i)
                rawSamples[i] = double(raw[i]);
            if (lowPass)
                rawSamples = ScopeView::applyFilter(rawSamples, rate, filterType, lowHz, highHz);
            m_view->setMeasurementSource(ch, rawSamples, rate);
        }
        // The overview strip shows the WHOLE recording compressed, with
        // setViewport() below marking where the current page sits within
        // it — a separate read, spanning 0..total rather than the current
        // page's own start/span, since passing the same windowed `samples`
        // here would mean the overview only ever shows whatever slice
        // happens to be zoomed into rather than an actual overview of the
        // whole thing. That full-recording read is genuinely expensive for
        // a large record (RecordWriter::readDecimated() falls back to one
        // disk seek per bucket once the record no longer fits its
        // sequential fast path — hundreds of seeks for kOverviewPoints
        // buckets), so it only actually runs when m_overviewCacheValid[ch]
        // says this channel's cached copy is stale; every other call this
        // whole read is skipped outright, since the overview's own content
        // cannot have changed just from panning or zooming within the same,
        // already-written recording.
        if (!m_overviewCacheValid[ch]) {
            // About one bucket per pixel of the strip (500 left it visibly
            // coarse); cached, so only re-read when the recording grows.
            constexpr int kOverviewPoints = 3000;
            QVector<float> overviewBlock;
            const int overviewGot =
                m_stream->readDecimated(ch, 0, total, kOverviewPoints, &overviewBlock);
            if (overviewGot > 0) {
                QVector<double> overviewSamples(overviewGot);
                for (int i = 0; i < overviewGot; ++i)
                    overviewSamples[i] = double(overviewBlock[i]);
                m_overviewCacheSamples[ch] = overviewSamples;
                m_overviewCacheValid[ch] = true;
            }
        }
        // Every call, cache hit or not: m_nav is shared with Capture/Live,
        // so a hit here means the overview DATA has not changed, not that
        // m_nav's own widget still shows it — switching away to another
        // mode and back overwrites it with that mode's own data in between.
        if (m_overviewCacheValid[ch])
            m_nav->setSamples(ch, m_overviewCacheSamples[ch]);
        feedSpectrum(ch, samples, windowingRate);
        feedXY(ch, samples);
        any = true;
    }

    if (lowPassSkipped) {
        statusBar()->showMessage(QStringLiteral(
            "Low-pass filter: shown on the trace once the recording stops "
            "(measurements already use it)"), 3000);
    }
    if (!any)
        return;

    m_view->setDisplayRateOverride(rate);   // the true, constant acquisition rate
    // Absolute time, not trigger-centred: a record counts up from its own
    // start, so the axis should read 0 at the left edge of the very first
    // view and grow with the record, not sit centred on +/-half a window.
    m_view->setAbsoluteTimeOrigin(double(m_recordViewStart) / rate);
    m_nav->setViewport(double(m_recordViewStart + span / 2) / total,
                       double(span) / total);
    m_nav->setTriggerFraction(-1.0);
    m_nav->setFollowingTrigger(false);
    refreshCards();
    updateStatusBar();
    // Decoded blocks onto the new page at once; the new view decoded now.
    for (int slot = 0; slot < 2; ++slot)
        projectBusRow(slot);
    QTimer::singleShot(0, this, &MainWindow::onDecodeTick);
}

// A dragged box, in fractions of the plot. Horizontal narrows the time window;
// vertical magnifies every visible channel about the selection, each in its own
// scale, so a box means the same thing whatever the channels are set to.
// The selection band always spans the full plot height (see ScopeView), so
// this is a horizontal-only, time-axis zoom.
void MainWindow::zoomToRect(const QRectF &box)
{
    pushZoomHistory();

    if (m_acquisitionMode == ModeRecord && !m_currentRecordTab.isEmpty()
        && m_stream->recorded() > 0) {
        // The box is a fraction of the span currently on screen. Read both
        // ends of that span before changing either field, so a second zoom
        // never mixes an old start with a new length or vice versa.
        const quint64 total = m_stream->recorded();
        const quint64 shownStart = m_recordViewStart;
        const quint64 shownSpan  = m_recordViewLength > 0
            ? std::min(m_recordViewLength, total - shownStart) : total;

        const quint64 newStart = shownStart + quint64(box.left() * shownSpan);
        const quint64 newSpan  = std::max<quint64>(
            64, quint64(box.width() * shownSpan));

        m_recordViewStart  = std::min(newStart, total > 0 ? total - 1 : 0);
        m_recordViewLength = std::min(newSpan, total - m_recordViewStart);
        refreshRecordView();
        return;
    }

    if (m_acquisitionMode == ModeStream && !m_liveLoaded) {
        // Live's own buffer is refetched fresh from the ring on every
        // refresh, sized to exactly match the current zoom (see
        // refreshLiveView()) — so there is no slack within it for
        // ScopeView's own internal pan/zoom machinery (used below for
        // Capture, where the buffer is static once captured) to operate
        // in; the very next refresh fetches a differently-sized window,
        // and an offset computed against the OLD window no longer means
        // anything against the new one. Zooming for Live goes through
        // m_liveViewOffset instead (which samples-back-from-newest window
        // to fetch), with an immediate refetch — not deferred to the next
        // natural tick — so the overview strip and the plot itself both
        // reflect the new zoom right away.
        const double oldPerDiv = m_view->secondsPerDiv();
        const double oldWanted = oldPerDiv * ScopeView::HorizontalDivs * m_stream->sampleRate();

        // Where the box's own centre falls, in "samples back from newest"
        // terms: box.left() == 1 (right edge) is the newest sample in the
        // current window (m_liveViewOffset samples back); box.left() == 0
        // (left edge) is the oldest (m_liveViewOffset + oldWanted back).
        const double desiredCentreSamplesBack = double(m_liveViewOffset)
            + oldWanted * (1.0 - (box.left() + box.width() / 2.0));

        const double newPerDiv = oldPerDiv * std::max(0.001, box.width());
        m_view->setSecondsPerDiv(newPerDiv);
        m_liveTimebase = newPerDiv;
        const double newWanted =
            newPerDiv * ScopeView::HorizontalDivs * m_stream->sampleRate();

        quint64 available = 0;
        for (int ch = 0; ch < 4; ++ch) {
            if (m_channel[ch].mode->checkedId() != ChannelOff)
                available = std::max(available, m_stream->historyAvailable(ch));
        }
        const qint64 maxOffset = std::max<qint64>(0, qint64(available) - qint64(newWanted));
        const qint64 newOffset =
            qint64(std::lround(desiredCentreSamplesBack - newWanted / 2.0));
        m_liveViewOffset = quint64(std::clamp<qint64>(newOffset, 0, maxOffset));
        m_livePanClock.restart();

        refreshLiveView();
        refreshCards();
        return;
    }

    // Capture: the box is a fraction of the CURRENTLY DISPLAYED window, not
    // of the whole buffer — narrowing secondsPerDiv alone (the previous
    // behaviour) only changes how much is shown, still centred on wherever
    // the view already was, so a selection anywhere off-centre zoomed into
    // the wrong region. The new centre has to be computed in the
    // whole-buffer fraction space before the window narrows, then applied
    // with setViewCentreFraction() — a one-shot action, so the small
    // rounding that function does is not the concern it would be for a
    // continuous drag. This only works because Capture's own buffer is
    // static once captured, unlike Live's, which is refetched fresh from
    // the ring on every refresh — see the ModeStream branch just above for
    // why Live needs an entirely different mechanism.
    const double currentWidthFraction  = m_view->viewportWidthFraction();
    const double currentCentreFraction = m_view->viewportCentreFraction();
    const double desiredCentre = currentCentreFraction - currentWidthFraction / 2.0
                                + (box.left() + box.width() / 2.0) * currentWidthFraction;

    const double window = m_view->secondsPerDiv() * ScopeView::HorizontalDivs;
    const double narrower = window * std::max(0.001, box.width());
    const double newPerDiv = narrower / ScopeView::HorizontalDivs;
    m_view->setSecondsPerDiv(newPerDiv);
    // Also reached by Live while it shows a loaded file (m_liveLoaded), which
    // zooms its static buffer exactly like a capture but keeps its own
    // mode's timebase.
    if (m_acquisitionMode == ModeStream)
        m_liveTimebase = newPerDiv;
    else
        m_captureTimebase = newPerDiv;
    m_view->setViewCentreFraction(std::clamp(desiredCentre, 0.0, 1.0));
    syncNavigator();
    refreshCards();
}

void MainWindow::resetZoom()
{
    for (int ch = 0; ch < 4; ++ch)
        setChannelPositionPercent(ch, 0.0);

    if (m_acquisitionMode == ModeRecord && !m_currentRecordTab.isEmpty()
        && m_stream->recorded() > 0) {
        m_recordViewStart = 0;
        m_recordViewLength = m_stream->recorded();
        refreshRecordView();
    } else if (m_acquisitionMode == ModeStream && m_liveLoaded) {
        // A loaded Live file is a static buffer: back to the dropdown's
        // timebase and recentred, with no ring to re-read.
        applyTimebase();
        m_view->followTrigger();
        syncNavigator();
    } else if (m_acquisitionMode == ModeStream) {
        m_liveViewOffset = 0;
        // Freeze itself is left alone — it is an explicit, manual control,
        // not something a "reset the view" gesture should also toggle. While
        // frozen this just snaps back to the newest frozen slice; while
        // running there is no pan to undo, so it falls through to the same
        // timebase reapplication Capture gets below.
        if (m_stream->frozen())
            refreshLiveView();
        else
            applyTimebase();
    } else {
        applyTimebase();
    }
    // An explicit "start over," not one more step to fold into the zoom
    // trail — the whole point is that nothing before it matters anymore,
    // so a "back" arrow that led to some pre-reset zoom would be offering
    // exactly the view this gesture was just asked to discard. Only the
    // current mode's own history clears; the other two modes' zoom trails
    // have nothing to do with a reset happening in this one.
    m_zoomBack[m_acquisitionMode].clear();
    m_zoomForward[m_acquisitionMode].clear();
    updateZoomHistoryButtons();
    statusBar()->showMessage(QStringLiteral("Zoom reset"), 2000);
}

// Reads whatever the current mode's own view actually is right now, into a
// snapshot generic enough to sit in either stack regardless of which mode
// pushed it — see the ZoomState fields' own comments in mainwindow.h for
// which ones a given mode actually uses. Capture's own pan position comes
// from ScopeView directly (viewportCentreFraction()) rather than a member
// here, since that is the one already-correct source for it — the same
// one zoomToRect() itself reads from for the Capture branch above.
MainWindow::ZoomState MainWindow::captureZoomState() const
{
    ZoomState state;
    state.captureTimebase = m_captureTimebase;
    state.captureCentreFraction = m_view->viewportCentreFraction();
    state.liveTimebase = m_liveTimebase;
    state.liveViewOffset = m_liveViewOffset;
    state.liveCentreFraction = m_view->viewportCentreFraction();
    state.recordViewStart = m_recordViewStart;
    state.recordViewLength = m_recordViewLength;
    return state;
}

// The inverse of captureZoomState() — restores whichever fields the
// CURRENT mode actually uses. Deliberately does not switch modes itself:
// a state pushed in one mode is only ever popped from that same mode's
// own stack (see navigateZoomHistory()), so m_acquisitionMode is already
// correct by the time this runs.
// Back / Forward restore the zoom level only: the view keeps its current
// centre (it used to jump back to where it was when that zoom was set).
void MainWindow::applyZoomState(const ZoomState &state)
{
    switch (m_acquisitionMode) {
    case ModeCapture:
        m_captureTimebase = state.captureTimebase;
        m_view->setSecondsPerDiv(state.captureTimebase);   // keeps the view's centre
        refreshCards();
        break;
    case ModeStream:
        if (m_liveLoaded) {
            // A loaded Live file is a static buffer panned like a Capture.
            m_liveTimebase = state.liveTimebase;
            m_view->setSecondsPerDiv(state.liveTimebase);
            syncNavigator();
        } else {
            // The same centre time; following the newest data stays following.
            const double rate = double(m_stream->sampleRate());
            if (m_liveViewOffset > 0 && rate > 0.0) {
                const double oldWindow = m_view->secondsPerDiv() * ScopeView::HorizontalDivs * rate;
                const double newWindow = state.liveTimebase * ScopeView::HorizontalDivs * rate;
                quint64 available = 0;
                for (int ch = 0; ch < m_stream->channels(); ++ch)
                    available = std::max(available, m_stream->historyAvailable(ch));
                const double centre = double(m_liveViewOffset) + oldWindow / 2.0;
                const double maxOffset = std::max(0.0, double(available) - newWindow);
                m_liveViewOffset = quint64(std::llround(std::clamp(centre - newWindow / 2.0, 0.0, maxOffset)));
            }
            m_liveTimebase = state.liveTimebase;
            // The refresh takes its window from the view's own timebase.
            m_view->setSecondsPerDiv(state.liveTimebase);
            refreshLiveView();
        }
        refreshCards();
        break;
    case ModeRecord: {
        const quint64 total = m_stream->recorded();
        const quint64 length = state.recordViewLength > 0 ? std::min(state.recordViewLength, total) : total;
        const double centre = double(m_recordViewStart) + double(m_recordViewLength) / 2.0;
        const double maxStart = double(total > length ? total - length : 0);
        m_recordViewStart = quint64(std::llround(std::clamp(centre - double(length) / 2.0, 0.0, maxStart)));
        m_recordViewLength = length;
        refreshRecordView();
        break;
    }
    }
}

// Called at the start of zoomToRect(), before it changes anything — this
// is what a "back" press later returns to. A fresh zoom always branches
// off the current point rather than replaying whatever was previously
// undone past, so the forward stack — everything that was only reachable
// by going back from here — is no longer reachable from this new branch
// and is cleared, the same convention a web browser's own history uses.
void MainWindow::pushZoomHistory()
{
    m_zoomBack[m_acquisitionMode].append(captureZoomState());
    m_zoomForward[m_acquisitionMode].clear();
    updateZoomHistoryButtons();
}

// forward=false is "back", forward=true is "forward" — named after the
// direction relative to the stack being popped FROM, not a button label,
// since this is also what wires directly to ScopeView's own
// zoomBackRequested()/zoomForwardRequested() signals.
void MainWindow::navigateZoomHistory(bool forward)
{
    auto &from = forward ? m_zoomForward[m_acquisitionMode] : m_zoomBack[m_acquisitionMode];
    auto &to   = forward ? m_zoomBack[m_acquisitionMode] : m_zoomForward[m_acquisitionMode];
    if (from.isEmpty())
        return;
    to.append(captureZoomState());
    const ZoomState state = from.last();
    from.removeLast();
    applyZoomState(state);
    updateZoomHistoryButtons();
}

// Called after every change to either stack (pushZoomHistory(),
// navigateZoomHistory()) and after an acquisition mode switch, so the
// arrows always reflect whichever mode is current rather than the one
// that was active the last time either stack actually changed.
void MainWindow::updateZoomHistoryButtons()
{
    m_view->setZoomHistoryAvailable(!m_zoomBack[m_acquisitionMode].isEmpty(),
                                    !m_zoomForward[m_acquisitionMode].isEmpty());
}

void MainWindow::updateStatusBar()
{
    if (!m_statusLine)
        return;

    QString text;
    if (m_acquisitionMode == ModeRecord) {
        const quint64 done   = m_stream->recorded();
        const quint64 target = m_stream->recordTarget();
        const int active = std::max(1, m_stream->recordedChannels());
        const double doneMb   = m_stream->recordMegabytes();
        const double targetMb = double(target) * 2.0 * active / 1048576.0;
        const double seconds  = m_stream->sampleRate() > 0
            ? double(done) / m_stream->sampleRate() : 0.0;

        text = QStringLiteral("%1  |  %2 / %3 MB written  (%4 s)")
                   .arg(formatRate(m_stream->sampleRate()))
                   .arg(doneMb, 0, 'f', 1).arg(targetMb, 0, 'f', 0)
                   .arg(seconds, 0, 'f', 2);
        if (m_stream->recordOverflows() > 0) {
            text += QStringLiteral("  |  %1 blocks lost, disk too slow")
                        .arg(m_stream->recordOverflows());
        }
    } else if (m_acquisitionMode == ModeStream) {
        const StreamEngine::Stats st = m_stream->stats();
        text = formatRate(m_stream->sampleRate());   // (latency / drop counters were debugging)
        Q_UNUSED(st);
    } else {
        text = QStringLiteral("%1").arg(formatRate(m_acq->sampleRate()));
    }

    // Free space on the volume the records land on. The directory is created at
    // startup: QStorageInfo answers -1 for a path that does not exist, which is
    // where the "-0.0 GB free" came from.
    quint64 freeBytes = 0;
    RecordWriter::enoughSpace(recordDirectory(), 0, &freeBytes);
    text += QStringLiteral("  |  %1 GB free").arg(freeBytes / 1073741824.0, 0, 'f', 1);
    // The parts as rich text: muted, separated by small accent dots rather
    // than " | ", a disk-too-slow warning in the warning colour.
    QStringList parts;
    for (const QString &part : text.split(QStringLiteral("  |  "))) {
        const bool warning = part.contains(QStringLiteral("lost"));
        parts << (warning ? QStringLiteral("<span style='color:%1'>%2</span>")
                                .arg(QColor(0xff, 0x5a, 0x4f).name(), part.toHtmlEscaped())
                          : part.toHtmlEscaped());
    }
    m_statusLine->setText(parts.join(QStringLiteral("&nbsp;&nbsp;<span style='color:%1'>\u2022</span>&nbsp;&nbsp;")
                                         .arg(Theme::accentHex())));
}

void MainWindow::setRunning(bool running)
{
    m_running = running;
    m_waitingTrigger = false;
    m_lastFrameMs = 0;
    m_framesPerSec = 0.0;
    m_view->setRunning(running);
    refreshCards();
}

// ---------------------------------------------------------------------------
// Control application
// ---------------------------------------------------------------------------

// Picking the fastest rate that merely fills the screen is what broke
// triggering at fast timebases: at 20 us/div and 31 MS/s a 8 K capture spans
// 262 us, while a 1 kHz signal has an edge only every 500 us, so most captures
// contain no edge for the hardware to trigger on.
//
// So the capture is asked to span several screen widths, not one. Resolution on
// screen is traded for a capture long enough to contain edges — and a deeper
// capture buys back the resolution, which is what the depth control is for.
// Capture and Live only — see applyRecordRate() for Record, which picks its
// rate directly rather than deriving it from a screen window that does not
// exist until a recording has actually run.
// Live loads the shorter roll-mode list, Capture and Record share the full
// range — called before applyTimebase() so it always reads from whichever
// list is actually current for the mode being switched to.
void MainWindow::refreshTimebaseOptions()
{
    // Each mode's own remembered value, not whatever the combo currently
    // shows — see m_captureTimebase's own comment in mainwindow.h for why
    // "whatever it currently shows" was the actual bug: it let one mode's
    // value get silently substituted by a nearest-match into the other
    // mode's different range, then permanently adopted as if it had always
    // been that mode's own setting.
    const double target = (m_acquisitionMode == ModeStream) ? m_liveTimebase : m_captureTimebase;

    m_timebase->blockSignals(true);
    m_timebase->clear();
    if (m_acquisitionMode == ModeStream) {
        for (double t : kRollTimebases)
            m_timebase->addItem(formatSeconds(t), t);
    } else {
        for (double t : kTimebases)
            m_timebase->addItem(formatSeconds(t), t);
    }

    int index = m_timebase->findData(target);
    if (index < 0) {
        // No exact match — this mode has never actually run yet this
        // session, or its own remembered value somehow fell outside its
        // own range. Closest available value, so an edge case here still
        // lands somewhere reasonable rather than an arbitrary index.
        index = 0;
        double bestDiff = std::fabs(m_timebase->itemData(0).toDouble() - target);
        for (int i = 1; i < m_timebase->count(); ++i) {
            const double diff = std::fabs(m_timebase->itemData(i).toDouble() - target);
            if (diff < bestDiff) {
                bestDiff = diff;
                index = i;
            }
        }
    }
    m_timebase->setCurrentIndex(index);
    m_timebase->blockSignals(false);
}

void MainWindow::applyTimebase(double exactOverride)
{
    ++m_avgGeneration;   // a running Capture average starts over
    // (the Auto wait follows the Capture timebase: updateAutoTriggerWait())
    QTimer::singleShot(0, this, [this] { updateAutoTriggerWait(); });
    VSX_TRACE("applyTimebase");
    if (m_acquisitionMode == ModeRecord)
        return;

    const double perDiv = m_timebase->currentData().toDouble();
    // exactOverride is used when restoring a mode's own remembered
    // timebase on re-entry — which might be a continuous value a
    // drag-a-box zoom produced, not one of the dropdown's fixed options.
    // Storing perDiv (the dropdown's own, nearest-matched value) back into
    // the remembered member here would permanently replace that exact
    // value with a coarser, rounded one every single time the mode is
    // switched into — which is exactly what "the grid changed after
    // switching modes" was describing: a 6-item list like kRollTimebases
    // essentially never contains the precise value a continuous zoom
    // produced, so every mode round-trip silently snapped it to whichever
    // of those 6 happened to be nearest.
    if (exactOverride < 0.0) {
        if (m_acquisitionMode == ModeStream)
            m_liveTimebase = perDiv;
        else
            m_captureTimebase = perDiv;
    }
    const double actualPerDiv = exactOverride >= 0.0 ? exactOverride : perDiv;
    // History's auto-bump and Live's own rate logic below still read the
    // dropdown's own perDiv/window for their arithmetic — an approximation
    // there is harmless (both are sizing/safety margins, not the precise
    // value actually drawn), as long as the plot itself, set right here,
    // gets the exact one.
    const double window = perDiv * ScopeView::HorizontalDivs;
    m_view->setSecondsPerDiv(actualPerDiv);

    if (m_acquisitionMode == ModeStream) {
        const int historyBefore = m_history->currentIndex();
        // The ring buffer's own capacity (the "History" setting) is
        // completely independent of the timebase — bump it up if the
        // selected window would not fit, rather than silently showing less
        // than the requested time/div. This is exactly what made 1/2/5 s
        // per div all look identical: each asks for a 10, 20, or 50 second
        // window while History stayed at its 5 second default, so the ring
        // buffer never held more than 5 seconds regardless of what was
        // requested. Only ever bumped up, never shrunk automatically, so a
        // deliberately larger History for manual panning is left alone.
        if (m_history->currentData().toDouble() < window) {
            int sufficientIndex = m_history->count() - 1;   // largest available, if none reach window
            for (int i = 0; i < m_history->count(); ++i) {
                if (m_history->itemData(i).toDouble() >= window) {
                    sufficientIndex = i;
                    break;
                }
            }
            m_history->blockSignals(true);
            m_history->setCurrentIndex(sufficientIndex);
            m_history->blockSignals(false);
        }

        // Chosen directly from m_liveRate now, not derived from the
        // timebase — Live used to auto-pick whatever rate kept the screen
        // legible for the current window, which is also what silently made
        // it inherit a rate that tracked Capture's own timebase choices
        // rather than anything the person actually chose for Live itself.
        const unsigned int requestedRate = m_liveRate->currentData().toUInt();
        const QVector<unsigned int> streamRates = m_acq->ready()
            ? StreamEngine::supportedRates(m_acq->deviceId()) : QVector<unsigned int>();
        const unsigned int chosen = streamRates.isEmpty()
            ? requestedRate : nearestRate(streamRates, requestedRate);
        m_acq->setSampleRate(chosen);
        // A loaded Live buffer keeps the rate it was recorded at; the new
        // rate still goes to the hardware for the next Run.
        if (!m_liveLoaded)
            m_view->setSampleRate(chosen);
        syncNavigator();
        refreshCards();
        // The rate is an argument to StreamCapture, so a new rate needs a
        // restart — as does a longer history for the new window. Nothing
        // else does: the new window comes from the history already held.
        // (Every timebase step restarted the stream — ~0.3 s each — and
        // turning it quickly crashed the app: a storm of SDK stop / start
        // cycles.) A needed restart is collapsed into one, 0.3 s after the
        // changes stop.
        const bool restart = m_running
            && (m_history->currentIndex() != historyBefore || chosen != m_stream->sampleRate());
        if (restart)
            m_streamRestartTimer->start();
        else if (!m_liveLoaded)
            refreshLiveView();
        return;
    }

    const int captureKb = m_depth->currentData().toInt();
    const double capacity = double(captureKb) * 1024.0;

    const QVector<unsigned int> rates = m_acq->supportedRates();
    if (!rates.isEmpty()) {
        // Was 8.0 — sizing the capture to span 8 screens meant panning after
        // a single trigger had real room to scroll, but it also meant the
        // same depth had to stretch across 8x more real time, forcing the
        // rate 8x lower and the capture itself 8x slower to complete. The
        // vendor app sizes the capture to just the visible screen instead,
        // and measurably gets both a higher rate and far faster updates for
        // it (confirmed against its own numbers: ~512K samples at 1MS/s is
        // almost exactly one 50ms/div x 10-division screen, not several).
        // 1.02 rather than exactly 1.0 only to absorb floating-point
        // comparison noise at the boundary, not to reintroduce spare margin.
        constexpr double kScreensPerCapture = 1.02;
        constexpr double kMinPointsOnScreen = 600.0; // stays legible

        unsigned int chosen = 0;
        for (unsigned int r : rates) {
            const double spansEnough = double(r) * window * kScreensPerCapture <= capacity;
            const double legible     = double(r) * window >= kMinPointsOnScreen;
            if (spansEnough && legible)
                chosen = std::max(chosen, r);
        }
        // Nothing satisfies both: drop the legibility floor and just keep at
        // least the visible screen inside the capture, accepting a rate too
        // low to be very readable over one this insisting on 600 points.
        if (chosen == 0) {
            for (unsigned int r : rates) {
                if (double(r) * window <= capacity)
                    chosen = std::max(chosen, r);
            }
        }
        if (chosen == 0)
            chosen = rates.first();

        m_acq->setCaptureKb(captureKb);
        m_acq->setSampleRate(chosen);
        m_view->setSampleRate(m_acq->sampleRate());
    }
    syncNavigator();
    refreshCards();
}

// Record's own rate, chosen directly rather than derived. Mirrors the restart
// behaviour Live already has: the rate is baked into the StreamCapture() call,
// so changing it while a recording is actively running does nothing useful on
// its own — the running stream stays configured for the old rate regardless
// of what SetOscSampleRate is told afterward. Missing that restart the first
// time this control existed is the most likely reason it broke: the hardware
// would end up with a rate that disagreed with the stream already in flight.
void MainWindow::applyRecordRate()
{
    VSX_TRACE("applyRecordRate");
    const unsigned int requested = m_recordRate->currentData().toUInt();
    const QVector<unsigned int> rates = m_acq->ready()
        ? StreamEngine::supportedRates(m_acq->deviceId()) : QVector<unsigned int>();
    const unsigned int chosen = rates.isEmpty() ? requested : nearestRate(rates, requested);

    m_acq->setSampleRate(chosen);
    if (!m_currentRecordTab.isEmpty() && m_stream->recorded() > 0) {
        // A recording is on screen: ScopeView holds a decimated page of it
        // with a windowing rate refreshRecordView() computed, and the
        // overview box marks that page within the whole recording. Setting
        // the hardware rate on the view and syncNavigator() (which, in
        // Record, always reports the page as 100% of the data) undid both —
        // this runs on entering Record, right after the tab was loaded,
        // which is what stretched the box across the entire strip. The new
        // rate only matters for the next recording; redraw the current one.
        refreshRecordView();
    } else {
        m_view->setSampleRate(chosen);
        syncNavigator();
    }
    refreshCards();
    if (m_running) {
        stopAcquisition();
        startAcquisition();
    }
}

void MainWindow::applyChannel(int ch)
{
    ++m_avgGeneration;   // a running Capture average starts over
    VSX_TRACE("applyChannel");
    if (ch < 0 || ch >= 4)
        return;

    // applyChannel -> updateOffsetRanges -> applyChannel is
    // a legitimate path when a page change clamps the offset. Let it settle
    // once rather than recurse.
    if (m_inChannelApply)
        return;
    m_inChannelApply = true;

    const ChannelControls &c = m_channel[ch];

    // Established here unconditionally, not left to restoreSettings() (which
    // returns early on a fresh install, before any settings file exists at
    // all) or the mode combo's own change handler (which only runs once the
    // person actually touches it) — a channel that starts on by default
    // (CH1's initial Auto mode) otherwise never gets its first
    // setChannelVisible(true) at all: ScopeView's own Channel::visible
    // defaults to false, so the trace stayed invisible on the main plot
    // (though not the overview strip, which tracks this independently)
    // until something happened to call this.
    const bool on = c.mode->checkedId() != ChannelOff;
    m_view->setChannelVisible(ch, on);
    m_nav->setChannelVisible(ch, on);

    const double page        = c.voltsPage->currentData().toDouble();
    const double voltsPerDiv  = divsFromPage(page);
    const int probeIndex      = std::clamp(c.probe->currentIndex(), 0,
                                           std::max(0, m_probes->count() - 1));
    const Probe &probeSpec    = m_probes->at(probeIndex);
    const double probe        = probeSpec.gradient;
    const double displayScale = c.scale->currentData().toDouble();

    m_view->setVoltsPerDiv(ch, voltsPerDiv);
    m_view->setProbe(ch, probeSpec.gradient, probeSpec.offset, probeSpec.unit);
    m_view->setDisplayScale(ch, displayScale);
    m_nav->setVoltsPerDiv(ch, voltsPerDiv);
    m_nav->setProbe(ch, probeSpec.gradient, probeSpec.offset);
    m_nav->setDisplayScale(ch, displayScale);

    updateOffsetRanges(ch);
    // Coupling first: it decides where the hardware window goes. When AC is
    // selected but the hardware can't do it (or didn't take it), the DC is
    // removed in software instead — and the window has to sit on the DC
    // level, or a small range centred on 0 V shows a 12 V battery as nothing
    // but clipping. trackSoftwareAc() keeps it there frame to frame.
    const bool wantAc = c.coupling->currentData().toInt() == 1;
    // Both paths get it: block mode (Capture) and stream mode (Live/Record)
    // have separate SDK calls. The mode on screen decides which read-back
    // counts.
    const bool blockOk = m_acq->setAcCoupling(ch, wantAc);
    const bool streamOk = m_stream->setChannelCoupling(ch, wantAc);
    // Software AC only in Capture. In Live/Record it made things worse: it
    // centres a small window on the DC (12 V), which streaming can't do
    // finely — the range was widened to ~19.5 mV steps — and the stream
    // read-back it relied on is taken whenever a setting changes, mostly
    // while not streaming, where it means nothing. Streaming sends the
    // hardware command (re-sent at every start) and reports what it got.
    (void)streamOk;
    const bool softAc = wantAc && m_acquisitionMode == ModeCapture && !blockOk;
    if (softAc && !m_softwareAc[ch]) {
        m_softAcCentre[ch] = c.inputOffset->value();
        m_softAcUnreachable[ch] = false;
        m_softAcSeeking[ch] = true;   // find the DC level first
        statusBar()->showMessage(QStringLiteral(
            "CH%1: the scope didn't switch to AC coupling, so the DC is removed in "
            "software and the input window follows the DC level").arg(ch + 1), 6000);
    }
    m_softwareAc[ch] = softAc;
    m_view->setChannelRemoveDc(ch, softAc);

    const double offset      = c.offsetV->value();
    const double inputOffset = softAc ? m_softAcCentre[ch] : c.inputOffset->value();
    m_view->setOffset(ch, offset);
    m_nav->setOffset(ch, offset);
    // The plot's own centre: the DC is already taken off under software AC.
    m_view->setWindowCentre(ch, softAc ? 0.0 : inputOffset);
    m_nav->setWindowCentre(ch, softAc ? 0.0 : inputOffset);

    // Volts, sized to this channel's own page — see setChannelPositionPercent
    // for why: a divisions-based pan shrinks in volt-terms as the channel is
    // zoomed, which is what made dragging stop dead at the zoomed window edge.
    const double positionVolts = c.offsetPct->value() / 100.0 * page;
    m_view->setPositionVolts(ch, positionVolts);
    m_nav->setPositionVolts(ch, positionVolts);

    // The window requested is one page either side of the input offset.
    // MEASURED (Range report, 12-bit MSO21AL Plus, Capture): the scope does
    // NOT shift its window — it always digitises a symmetric +-H around 0 V,
    // H being the smallest of its 15 hardware ranges (+-145 mV .. +-29.9 V,
    // ~sqrt(2) steps) that covers both ends of the request. So an input
    // offset only makes it pick a larger range; resolution is set by the
    // largest absolute voltage on screen, not by the span shown.
    //
    // DC offset does NOT appear here: it is added on the display side and must
    // not change what the ADC digitises.
    const int limit = std::max(1, m_acq->rangeMaxMv());
    int centreMv = int(std::lround((inputOffset - probeSpec.offset) * 1000.0 / probe));
    int halfMv   = std::clamp(int(std::lround(page * 1000.0 / probe)), 1, limit);
    if (softAc && m_softAcSeeking[ch]) {
        centreMv = 0;        // the probe's whole range, for one frame
        halfMv = limit;
    }

    int minMv = std::clamp(centreMv - halfMv, -limit, limit);
    int maxMv = std::clamp(centreMv + halfMv, -limit, limit);
    if (maxMv <= minMv)
        maxMv = minMv + 1;
    // The range the scope really uses (see above): the larger end, not half
    // the span — with an input offset, half the span looked up the zero
    // calibration of a range several times smaller than the active one.
    m_appliedHalfMv[size_t(ch)] = std::max(std::abs(minMv), std::abs(maxMv));
    m_appliedCoupling[size_t(ch)] = wantAc ? 1 : 0;
    applyZeroOffsets(ch);

    // Both Live and Record acquire through StreamEngine, so both need the
    // STREAM range setter here, not the block/Capture one — calling
    // m_acq->setRangeMv() while in ModeRecord configures a session
    // (AcquisitionManager's Capture session) that Record never actually
    // reads from, silently leaving Record's real acquisition at whatever
    // range the hardware last happened to have. This is the same class of
    // bug HANDOFF.md already documents once before ("Record mode never
    // sets the stream input range"): narrowing this to `== ModeStream`
    // reintroduces exactly that gap for Record specifically.
    if (m_acquisitionMode != ModeCapture)
        m_stream->setChannelRange(ch, minMv, maxMv);   // the block setter is inert here
    else
        m_acq->setRangeMv(ch, minMv, maxMv);

    m_inChannelApply = false;
    refreshRulersChannelList();
    refreshMeasureChannelSelector();
    refreshSpectrumChannelSelector();
    refreshXYChannelSelectors();
    refreshCards();
}

// The threshold can only usefully sit inside what the source channel can see.
// Bounding the spin box stops it being set somewhere the signal never reaches,
// which in Normal mode looks like the application has frozen.
void MainWindow::updateTriggerLevelRange()
{
    const int source = m_trigSource->currentIndex() < 0
        ? 0 : m_trigSource->currentData().toInt();
    const int safe = std::clamp(source, 0, 3);

    const double page   = m_channel[safe].voltsPage->currentData().toDouble();
    const double centre = m_channel[safe].inputOffset->value();

    const double before = m_trigLevel->value();

    // The threshold lives inside the hardware window, which the input offset
    // may have moved well away from zero.
    m_trigLevel->blockSignals(true);
    m_trigLevel->setRange(centre - page, centre + page);
    m_trigLevel->setSingleStep(divsFromPage(page) / 5.0);
    m_trigLevel->setSuffix(QStringLiteral(" %1").arg(probeFor(safe).unit));
    m_trigLevel->blockSignals(false);

    // setRange() clamps the current value. With signals blocked that clamp is
    // silent, so the spin box would show one threshold while the hardware kept
    // the old one — which looks like the trigger simply not working.
    if (!qFuzzyCompare(before, m_trigLevel->value()))
        applyTrigger();
}

void MainWindow::setTriggerLevelToMidpoint()
{
    const int source = m_trigSource->currentIndex() < 0
        ? 0 : m_trigSource->currentData().toInt();

    double midpoint = 0.0;
    if (!m_view->channelMidpoint(std::clamp(source, 0, 3), &midpoint)) {
        statusBar()->showMessage(
            QStringLiteral("No capture on that channel yet — run first."), 4000);
        return;
    }
    m_trigLevel->setValue(midpoint);
}

// The DC offset spans twice the page either way and steps in round numbers. It
// is independent of the percentage offset: one adds voltage to the signal, the
// other slides the trace and its scale across the screen.
void MainWindow::updateOffsetRanges(int ch)
{
    ChannelControls &c = m_channel[ch];
    const double page = c.voltsPage->currentData().toDouble();
    const double step = niceStep(page / 25.0);

    const double before = c.offsetV->value();
    c.offsetV->blockSignals(true);
    c.offsetV->setRange(-2.0 * page, 2.0 * page);
    c.offsetV->setSingleStep(step);
    c.offsetV->setDecimals(step < 0.1 ? 3 : 2);
    c.offsetV->blockSignals(false);

    // The window has to stay inside what the hardware can reach, so the input
    // offset can only travel as far as the device range less one page. In probe
    // units that reach is scaled by the gradient and centred on the probe's own
    // offset.
    const Probe &probeSpec = probeFor(ch);
    const double gain = std::fabs(probeSpec.gradient);
    const double reach = std::max(0.0, m_acq->rangeMaxMv() / 1000.0 * gain - page);
    const double inputStep = niceStep(std::max(page / 5.0, reach / 50.0));

    const double beforeInput = c.inputOffset->value();
    c.inputOffset->blockSignals(true);
    c.inputOffset->setRange(probeSpec.offset - reach, probeSpec.offset + reach);
    c.inputOffset->setSingleStep(inputStep);
    c.inputOffset->setDecimals(inputStep < 0.1 ? 3 : 2);
    c.inputOffset->blockSignals(false);

    // setRange() clamps silently while signals are blocked.
    if (!qFuzzyCompare(before, c.offsetV->value())
        || !qFuzzyCompare(beforeInput, c.inputOffset->value()))
        applyChannel(ch);
}

// Percentage offset is a pure display shift: 100% is half the graticule, so
// +90% lifts the trace by 4.5 of the 10 divisions. No hardware call, which is
// what makes it safe to drive from a mouse drag.
// The pan amount is a fixed voltage — pct percent of this channel's own,
// UNZOOMED page — rather than a number of graticule divisions. Divisions
// shrink in volt-terms as the channel is magnified, which is what previously
// made dragging stop dead at the edge of whatever the current zoom happened
// to show. A volt-based pan keeps the same reach regardless of zoom.
void MainWindow::setChannelPositionPercent(int ch, double percent)
{
    if (ch < 0 || ch >= 4)
        return;
    const double clamped = std::clamp(percent, -900.0, 900.0);
    m_channelOffsetPctExact[ch] = clamped;

    QDoubleSpinBox *box = m_channel[ch].offsetPct;
    if (!qFuzzyCompare(box->value(), clamped)) {
        box->blockSignals(true);
        box->setValue(clamped);
        box->blockSignals(false);
    }

    const double page = m_channel[ch].voltsPage->currentData().toDouble();
    const double volts = clamped / 100.0 * page;
    m_view->setPositionVolts(ch, volts);
    m_nav->setPositionVolts(ch, volts);
    // Not refreshCards() here: this runs on every drag event while dragging
    // the voltage scale, and the existing 250 ms periodic tick already keeps
    // the card's summary text current — calling the full refresh on every
    // pixel of movement was adding up to visible lag for no benefit, since a
    // card label updating a quarter-second late is imperceptible.
}

void MainWindow::reloadProbeList(int ch)
{
    if (ch < 0 || ch >= 4 || !m_channel[ch].probe)
        return;
    QComboBox *box = m_channel[ch].probe;

    const QString previous = box->currentText();
    box->blockSignals(true);
    box->clear();
    for (const Probe &p : m_probes->probes())
        box->addItem(p.name.isEmpty() ? p.summary() : p.name);
    const int restored = box->findText(previous);
    box->setCurrentIndex(restored >= 0 ? restored : 0);
    box->blockSignals(false);
    applyProbeUnits(ch);
}

void MainWindow::applyProbeUnits(int ch)
{
    if (ch < 0 || ch >= 4)
        return;
    ChannelControls &c = m_channel[ch];
    const QString unit = probeFor(ch).unit.isEmpty() ? QStringLiteral("V") : probeFor(ch).unit;
    if (c.voltsPageTitle)
        c.voltsPageTitle->setText(unit == QLatin1String("V") ? QStringLiteral("Volts per page")
                                                             : QStringLiteral("%1 per page").arg(unit));
    if (c.voltsPage) {
        for (int i = 0; i < c.voltsPage->count(); ++i)
            c.voltsPage->setItemText(i, QStringLiteral("\u00b1%1")
                                            .arg(formatVolts(c.voltsPage->itemData(i).toDouble(), unit)));
    }
    for (QDoubleSpinBox *box : { c.inputOffset, c.offsetV }) {
        if (box)
            box->setSuffix(QStringLiteral(" %1").arg(unit));
    }
}

// The probe combo mirrors the library's order, so the current index maps
// straight through. It carries no user data — reading currentData() here is
// what made the trigger behave as if every probe were x1.
const Probe &MainWindow::probeFor(int ch) const
{
    const int index = (ch < 0 || ch >= 4) ? 0 : m_channel[ch].probe->currentIndex();
    return m_probes->at(std::clamp(index, 0, std::max(0, m_probes->count() - 1)));
}

void MainWindow::pickChannelColour(int ch)
{
    if (ch < 0 || ch >= 4)
        return;
    const QColor picked = VsxDialogs::pickColour(
        m_colour[ch], this, QStringLiteral("Colour for channel %1").arg(ch + 1));
    if (!picked.isValid())
        return;

    m_colour[ch] = picked;
    m_view->setChannelColor(ch, picked);
    m_nav->setChannelColor(ch, picked);
    m_channelCard[ch]->setAccent(picked);

    QPalette pal = m_channel[ch].colour->palette();
    pal.setColor(QPalette::ButtonText, picked);
    m_channel[ch].colour->setPalette(pal);

    if (m_rulersChannelChecks[ch])
        setRulerCheckboxColor(m_rulersChannelChecks[ch], picked);
    updateSpectrumSources();   // a Spectrum trace on this channel should pick this up immediately
    updateXYColors();          // same, for an XY trace using this channel as its Y axis
    refreshCards();
}

// Auto mode: pick the smallest page that still contains the signal, with a
// dead band so a trace sitting near a boundary does not oscillate between two
// ranges every frame.
void MainWindow::autoRange(int ch)
{
    if (ch < 0 || ch >= 4)
        return;

    double lo = 0.0, hi = 0.0;
    if (!m_view->channelSpan(ch, &lo, &hi))
        return;

    const double peak = std::max(std::fabs(lo), std::fabs(hi));
    const double page = m_channel[ch].voltsPage->currentData().toDouble();

    if (peak < page * 0.9 && peak > page * 0.3)
        return;                                  // comfortably framed already

    int wanted = kVoltsCount - 1;
    for (int i = 0; i < kVoltsCount; ++i) {
        if (kVoltsPerPage[i] >= peak * 1.3) {
            wanted = i;
            break;
        }
    }
    if (wanted == m_channel[ch].voltsPage->currentIndex())
        return;

    m_channel[ch].voltsPage->setCurrentIndex(wanted);   // triggers applyChannel
}

// The app's own Auto timeout from the chosen wait and the Capture timebase:
// 1 or 2 screen widths (at least 20 / 50 ms). Never longer than the scope's
// own (it simply ends the wait first). See AcquisitionManager::maybeForceAuto().
// Left / Right pan the recording by one division (a whole screen with
// Shift), like dragging the plot — dragging on a touchpad is awkward. An
// application-wide filter so it works wherever the focus is, except where
// the arrows already mean something: text, numbers, lists, dropdowns.
bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyPress && m_acquisitionMode == ModeRecord && isActiveWindow()
        && m_view && !m_currentRecordTab.isEmpty()) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const bool arrow = key->key() == Qt::Key_Left || key->key() == Qt::Key_Right;
        const bool plain = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier));
        if (arrow && plain) {
            QWidget *focus = QApplication::focusWidget();
            const bool arrowsTaken = focus
                && (qobject_cast<QLineEdit *>(focus) || qobject_cast<QAbstractSpinBox *>(focus)
                    || qobject_cast<QTextEdit *>(focus) || qobject_cast<QPlainTextEdit *>(focus)
                    || qobject_cast<QComboBox *>(focus) || qobject_cast<QAbstractItemView *>(focus)
                    || qobject_cast<QAbstractSlider *>(focus));
            if (!arrowsTaken) {
                const double step = (key->modifiers() & Qt::ShiftModifier) ? 1.0 : 0.1;
                // Positive = earlier, as a drag to the right.
                emit m_view->panRequested(key->key() == Qt::Key_Left ? step : -step);
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::updateAutoTriggerWait()
{
    if (!m_autoWait || !m_acq)
        return;
    const double screenMs = m_captureTimebase * ScopeView::HorizontalDivs * 1000.0;
    int ms = -1;
    switch (m_autoWait->currentData().toInt()) {
    case 0: ms = int(std::lround(std::max(20.0, screenMs))); break;
    case 1: ms = int(std::lround(std::max(50.0, 2.0 * screenMs))); break;
    default: break;
    }
    m_acq->setAutoTriggerWaitMs(ms < 0 ? -1 : std::min(ms, 5000));
}

void MainWindow::applyTrigger()
{
    ++m_avgGeneration;   // a running Capture average starts over
    const int mode = m_trigModeGroup->checkedId();
    const bool armed = mode != ModeNone;
    const bool falling = m_trigDirGroup->checkedId() == 1;

    const int source = m_trigSource->currentIndex() < 0
        ? 0 : m_trigSource->currentData().toInt();
    const int safeSource = std::clamp(source, 0, 3);
    // The threshold spin box is in probe units; the SDK wants millivolts at the
    // BNC. Undo the probe's scaling to get there: raw = (displayed - c) / m.
    const Probe &probeSpec = probeFor(safeSource);
    const double gradient = qFuzzyIsNull(probeSpec.gradient) ? 1.0 : probeSpec.gradient;

    const double levelVolts = m_trigLevel->value();
    const int levelMv =
        int(std::lround((levelVolts - probeSpec.offset) * 1000.0 / gradient));

    // Hysteresis scaled to the source channel's sensitivity, so a noisy signal
    // does not retrigger on every wobble. It is a delta, so only the gradient
    // applies, never the offset.
    const double voltsPerDiv = divsFromPage(
        m_channel[safeSource].voltsPage->currentData().toDouble());
    const int senseMv = std::max(2,
        int(std::lround(voltsPerDiv * 1000.0 * 0.1 / std::fabs(gradient))));

    m_waitingTrigger = false;
    m_acq->setTriggerStyle(armed ? (falling ? kStyleFalling : kStyleRising) : kStyleNone);
    m_acq->setRequireTrigger(mode == ModeNormal);
    m_acq->setTriggerSource(unsigned(safeSource));
    m_acq->setTriggerLevelMv(levelMv, senseMv);

    m_trigLevel->setEnabled(armed);
    m_trigSource->setEnabled(armed);
    m_levelToMid->setEnabled(armed);
    m_view->setTrigger(safeSource, levelVolts, falling ? 1 : 0,
                       armed && m_acquisitionMode == ModeCapture);

    m_acq->rearm();
    refreshCards();
}
