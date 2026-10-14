#pragma once

#include "fonts.h"

#include <QApplication>
#include <QColor>
#include <QFont>      // QApplication::font() below; don't rely on includers having it
#include <QPalette>
#include <QSettings>
#include <QString>

/*
 * One place for every colour the app draws with, chrome and plot alike, so
 * a change here reaches the cards, the navigator viewport, the dialog
 * headers, the icons, and the oscilloscope plot itself together — and so
 * light/dark mode is a single switch rather than two parallel designs that
 * have to be kept in sync by hand.
 *
 * Every colour function below reads currentMode() fresh on each call rather
 * than caching a value: several call sites across the app hold onto colours
 * in file-scope `const` globals or struct default-member-initialisers
 * evaluated once, at static-init or construction time, and those are exactly
 * the call sites that need a live function call here, not a value that could
 * have been baked in before the persisted mode was even loaded from disk.
 */
namespace Theme {

enum class Mode { Dark, Light };

// Backed by QSettings ("ui/theme": "dark" | "light"), read once into a
// static on first use and cached from then on — setMode() below is the only
// way it changes after that, so every reader always agrees with the last
// call to it, in this run of the app, without re-reading QSettings itself
// as long as this stays the only writer.
inline Mode &modeStorage()
{
    static Mode mode = [] {
        QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
        const QString stored = settings.value(QStringLiteral("ui/theme"),
                                               QStringLiteral("dark")).toString();
        return stored == QStringLiteral("light") ? Mode::Light : Mode::Dark;
    }();
    return mode;
}

inline Mode currentMode() { return modeStorage(); }
inline bool isDark()      { return currentMode() == Mode::Dark; }

inline void setMode(Mode mode)
{
    modeStorage() = mode;
    QSettings settings(QStringLiteral("VimuScopeX"), QStringLiteral("VimuScopeX"));
    settings.setValue(QStringLiteral("ui/theme"),
                       mode == Mode::Light ? QStringLiteral("light") : QStringLiteral("dark"));
}

// Primary accent: selection, card steppers, viewport outline, headers.
// Slightly darker in light mode — the same bright green read a little too
// glaring against white, even though it is always paired with its own
// explicit text colour (onAccent(), below) wherever it is a background, so
// contrast on its own was never actually the concern here.
inline QColor accent()
{
    return isDark() ? QColor(0x73, 0xff, 0x00) : QColor(0x62, 0xd9, 0x00);
}
inline QString accentHex()  { return accent().name(); }
// Text and icons drawn ON an accent-filled bar (cards' step buttons, the
// panel's title bar): dark in dark mode, white in light mode.
inline QColor plotTimeText() { return isDark() ? QColor(0xff, 0xff, 0xff) : QColor(0x1b, 0x23, 0x2b); }   // time-axis labels
inline QColor onAccentBar() { return isDark() ? QColor(0x10, 0x18, 0x0a) : QColor(0xff, 0xff, 0xff); }

// Secondary accent: icon colour, and text sitting directly beside an icon —
// unlike accent() this is never paired with its own explicit background, so
// it has to carry its own contrast against whatever surface is behind it.
// Dark mode's original pale value reads fine on the app's near-black
// surfaces; that same paleness is nearly invisible against light mode's
// near-white ones, so this is the one accent that actually flips.
inline QColor accentSoft()
{
    // A little greener than before (cdf7ab / 3f7a1a): the icons and their
    // labels in the top bar and left rail, and the close icons.
    return isDark() ? QColor(0xbd, 0xf5, 0x9c) : QColor(0x35, 0x7f, 0x14);
}
inline QString accentSoftHex() { return accentSoft().name(); }

// Near-black text used ON TOP of the bright accent background (checked
// buttons, selected menu items, selected tab) — unchanged across modes for
// the same reason accent() itself is: it is that background's own paired
// text colour, not something read against the surrounding theme.
inline QColor onAccent() { return QColor(0x10, 0x18, 0x0a); }

// Text drawn on top of a flag filled with plotForegroundText() itself (the
// time cursor flags in ScopeView, and every cursor flag in SpectrumView) —
// needs its own colour rather than onAccent() because plotForegroundText()
// is light in dark mode but dark in light mode, so the same near-black
// text that reads fine against dark mode's light fill becomes unreadable
// against light mode's own dark fill. A voltage cursor flag in ScopeView
// is different — filled with that channel's own colour, a mid-tone in
// both modes — so those still use onAccent() and are unaffected here.
inline QColor onPlotForeground()
{
    return isDark() ? onAccent() : QColor(0xff, 0xff, 0xff);
}

// ---- Chrome: window, panels, cards, fields, borders, text -----------------

// The window's own background: the top bar, the left rail, the strip behind
// the recording tabs and the status bar. Slightly darker than the cards
// (cardBackground) and unselected tabs (surfaceBackground), so those stand
// out from it; was 1b1f25 / eef1f4.
inline QColor windowBackground()    { return isDark() ? QColor(0x19, 0x19, 0x19) : QColor(0xe5, 0xe9, 0xed); }
inline QColor panelBackground()     { return isDark() ? QColor(0x17, 0x1c, 0x22) : QColor(0xff, 0xff, 0xff); }
inline QColor panelBorder()         { return isDark() ? QColor(0x2c, 0x34, 0x3d) : QColor(0xd3, 0xda, 0xe1); }
// Cards, combo boxes, spin boxes, push buttons — the "raised surface" family.
inline QColor surfaceBackground()   { return isDark() ? QColor(0x20, 0x26, 0x2e) : QColor(0xff, 0xff, 0xff); }
inline QColor surfaceHoverBackground()  { return isDark() ? QColor(0x26, 0x2d, 0x36) : QColor(0xf3, 0xf5, 0xf7); }
inline QColor surfacePressedBackground(){ return isDark() ? QColor(0x17, 0x1b, 0x20) : QColor(0xe6, 0xea, 0xed); }
// QLineEdit and other visually "recessed" fields.
inline QColor recessedBackground()  { return isDark() ? QColor(0x14, 0x18, 0x1d) : QColor(0xe4, 0xe8, 0xec); }
// Dropdown popups, tooltips, menus.
inline QColor elevatedBackground()  { return isDark() ? QColor(0x1c, 0x21, 0x27) : QColor(0xff, 0xff, 0xff); }
// A subtle, accent-tinted background for a selected list-style row (the
// probe library's checked entry) — distinct from accent() itself, which is
// full-strength and reserved for buttons/tabs/menu selection.
inline QColor selectedTint()        { return isDark() ? QColor(0x2f, 0x4a, 0x17) : QColor(0xe3, 0xf5, 0xd1); }

inline QColor border()      { return isDark() ? QColor(0x33, 0x3c, 0x46) : QColor(0xc7, 0xcf, 0xd6); }
inline QColor borderSoft()  { return isDark() ? QColor(0x26, 0x2d, 0x35) : QColor(0xdd, 0xe3, 0xe9); }
inline QColor borderHover() { return isDark() ? QColor(0x4a, 0x55, 0x60) : QColor(0xa3, 0xae, 0xb8); }

inline QColor text()         { return isDark() ? QColor(0xdc, 0xe2, 0xe8) : QColor(0x1b, 0x23, 0x2b); }
inline QColor textMuted()    { return isDark() ? QColor(0x8a, 0x94, 0xa0) : QColor(0x5b, 0x65, 0x70); }
inline QColor textDisabled() { return isDark() ? QColor(0x6a, 0x72, 0x7c) : QColor(0xa2, 0xab, 0xb5); }

// Card backgrounds (CardButton's own three states) — a shade darker/lighter
// than the general surface family so cards read as their own thing next to
// combo boxes and buttons, matching this app's existing dark-mode design.
inline QColor cardBackground()         { return isDark() ? QColor(0x28, 0x28, 0x28) : QColor(0xff, 0xff, 0xff); }
inline QColor cardHoverBackground()    { return isDark() ? QColor(0x2e, 0x2e, 0x2e) : QColor(0xf3, 0xf5, 0xf7); }
inline QColor cardSelectedBackground() { return isDark() ? QColor(0x33, 0x33, 0x33) : QColor(0xe9, 0xef, 0xf5); }
inline QColor cardBorder()             { return isDark() ? QColor(0x2c, 0x34, 0x3d) : QColor(0xd3, 0xda, 0xe1); }

// The corner radius used everywhere a rounded rectangle appears — cards,
// the side panel, Record mode's tabs, and the plot views — so all of them
// read as the same design rather than a slightly different radius picked
// independently in each place.
inline int cornerRadius() { return 8; }

// ---- The oscilloscope plot itself (ScopeView, SpectrumView, XYView, and
// NavigatorView's overview strip all share this one set) --------------------

inline QColor plotBackground() { return isDark() ? QColor(0x0c, 0x0c, 0x0c) : QColor(0xfb, 0xfc, 0xfd); }
inline QColor plotGridMinor()  { return isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xc3, 0xca, 0xd1); }   // stronger: the dots were too faint
inline QColor plotGridMajor()  { return isDark() ? QColor(0x4b, 0x4b, 0x4b) : QColor(0xa9, 0xb3, 0xbd); }
inline QColor plotGridAxis()   { return isDark() ? QColor(0x62, 0x62, 0x62) : QColor(0x77, 0x82, 0x8d); }
// The trigger marker/level line — a warm amber with decent contrast against
// both a near-black and a near-white plot background, so unlike accentSoft()
// this one is left unchanged across modes rather than needing a light-mode
// retune.
inline QColor triggerColor()   { return QColor(0xd8, 0x92, 0x3a); }
// A semi-transparent patch drawn behind a measurement label so it stays
// readable over a busy trace — darkens in dark mode (to contrast with the
// pale channel-coloured text drawn on top of it there) and lightens in
// light mode (where that same text is now a dark shade instead).
inline QColor plotLabelBackdrop()
{
    return isDark() ? QColor(0, 0, 0, 110) : QColor(255, 255, 255, 180);
}
// The annotation ("sticky note") box drawn on top of the plot — a dark,
// faintly green-tinted card in dark mode; a pale, warm note-paper tint in
// light mode, the same "sticky note" association read the other way round.
inline QColor annotationBackground()
{
    return isDark() ? QColor(0x0d, 0x1f, 0x12, 235) : QColor(0xf3, 0xf6, 0xe4, 235);
}
// Axis tick labels and similar small, muted plot-area text — separate from
// the app-chrome textMuted() above purely so the plot's own palette stays
// self-contained here, even though the two happen to share a value.
inline QColor plotAxisText()   { return textMuted(); }
// Ruler labels and other plot-area text meant to read at full strength,
// same reasoning as plotAxisText() for why this is its own name.
inline QColor plotForegroundText() { return text(); }

// Per-channel default trace colours. Same four hues in both modes so a
// channel stays recognisable across a theme change, but retuned per mode:
// dark mode's pale yellow and mint green all but disappear against a light
// background, thin traces having far less contrast margin than text does.
inline QColor channelColor(int index)
{
    static const QColor kDark[4] = {
        QColor(0xff, 0xd5, 0x4f), QColor(0x4d, 0xb6, 0xff),
        QColor(0xff, 0x6e, 0x6e), QColor(0x6d, 0xe6, 0x9c),
    };
    static const QColor kLight[4] = {
        QColor(0xc9, 0x9a, 0x00), QColor(0x1f, 0x6f, 0xd6),
        QColor(0xd8, 0x38, 0x2f), QColor(0x27, 0x96, 0x53),
    };
    if (index < 0 || index >= 4)
        return isDark() ? kDark[0] : kLight[0];
    return isDark() ? kDark[index] : kLight[index];
}

// Math channel's own default trace colour — same reasoning as channelColor()
// above, retuned for the same reason.
inline QColor mathColor() { return isDark() ? QColor(0xd6, 0x7a, 0xff) : QColor(0x93, 0x31, 0xc9); }

// Reference waveforms' own default colours, one per slot — an amber and a
// teal, chosen to sit clearly apart from every channel default
// (yellow/blue/red/green), from mathColor()'s purple, and from each other,
// so a freshly-added reference is never accidentally the same colour as
// something already on screen. Retuned per mode for the same reason
// channelColor() is: a pale shade that reads fine in dark mode can all but
// disappear as a thin trace against a light background.
inline QColor referenceColor(int slot)
{
    if (slot == 0)
        return isDark() ? QColor(0xe0, 0xa0, 0x30) : QColor(0xb8, 0x7a, 0x00);
    return isDark() ? QColor(0x30, 0xc8, 0xc0) : QColor(0x0a, 0x8a, 0x82);
}

// Applied once, app-wide, in main.cpp, and again by MainWindow::applyTheme()
// whenever the mode toggles at runtime — see that function for everything
// else a live toggle also has to redo beyond just this palette and the
// style sheet below (baked icon pixmaps, cached style-sheet strings, and
// every custom-painted view's own repaint).
inline QPalette palette()
{
    QPalette p;
    p.setColor(QPalette::Window,          windowBackground());
    p.setColor(QPalette::WindowText,      text());
    p.setColor(QPalette::Base,            recessedBackground());
    p.setColor(QPalette::AlternateBase,   surfaceBackground());
    p.setColor(QPalette::Text,            text());
    p.setColor(QPalette::Button,          surfaceBackground());
    p.setColor(QPalette::ButtonText,      text());
    p.setColor(QPalette::Highlight,       accent());
    p.setColor(QPalette::HighlightedText, onAccent());
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textDisabled());
    p.setColor(QPalette::Disabled, QPalette::WindowText, textDisabled());
    return p;
}

// A QPalette only recolours what a style already draws — it can't change the
// bevels, gradients and arrow glyphs Fusion renders for standard controls,
// which is why combo boxes, checkboxes, spin buttons and plain push buttons
// still looked like generic Fusion chrome even with the palette applied.
// This replaces that chrome outright for the specific widget types that
// still needed it.
//
// Deliberately does NOT touch QWidget/QMainWindow/QDialog backgrounds — the
// palette already gets those right, and a blanket rule risks side effects in
// places this pass has no reason to touch. It also can't affect CardButton
// (pure paintEvent, no styled background) or the toolbar/rail QToolButtons
// (each carries its own setStyleSheet(), which Qt always prioritises over an
// inherited app-level one, regardless of selector specificity).
// Dropdown lists, everywhere: a rounded card-coloured list with a neutral
// edge and rounded highlights (the popup window is made frameless and
// translucent per combo in MainWindow::applyTheme(), so the corners show).
inline QString dropdownListStyle()
{
    const QColor list = isDark() ? QColor(0x28, 0x28, 0x28) : QColor(0xff, 0xff, 0xff);
    const QColor edge = isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0);
    return QStringLiteral(
        "QComboBoxPrivateContainer { background: transparent; border: none; }"
        "QComboBox QAbstractItemView { background: %1; border: 1px solid %2; border-radius: 6px;"
        " padding: 4px; outline: none; }"
        "QComboBox QAbstractItemView::item { padding: 4px 8px; border-radius: 4px; }"
        "QComboBox QAbstractItemView::item:hover,"
        " QComboBox QAbstractItemView::item:selected { background: %3; color: %4; }")
        .arg(list.name(), edge.name(), accent().name(), QColor(0x10, 0x18, 0x0a).name());
}

inline QString globalStyleSheet()
{
    // Once ANY style-sheet rule matches a widget, Qt's style-sheet engine
    // takes over full responsibility for that widget's font resolution —
    // not just whichever properties a rule actually sets — and falls back
    // to Qt's own default font rather than continuing to inherit
    // QApplication::font(), unless a rule says otherwise. Every widget type
    // this style sheet touches (QComboBox, QPushButton, QLineEdit, ...) is
    // exactly such a widget, so without this "*" rule every one of them
    // would silently stop using the app's own font despite main.cpp having
    // set it — this was the actual cause of "the font isn't applied
    // everywhere". Skipped entirely if font registration failed
    // (Fonts::family() empty), rather than emit a rule naming a family
    // that does not exist.
    QString fontRule;
    const QString family = Fonts::family();
    if (!family.isEmpty())
        fontRule = QStringLiteral("* { font-family: \"%1\"; }\n").arg(family);

    // QComboBox's own displayed/selected value reads noticeably smaller
    // than the rest of the app despite inheriting the same base font as
    // everything else — %15/%16 make the closed box itself 10% larger
    // while pinning the dropdown popup's own list items back to the
    // ordinary base size, so only the always-visible selected value
    // actually changes.
    const double baseSize = QApplication::font().pointSizeF();

    return fontRule + QStringLiteral(R"(
QComboBox {
    background: %2;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 4px 8px;
    min-height: 20px;
    font-size: %15pt;
}
QComboBox:hover           { border-color: %4; }
QComboBox:focus           { border-color: %1; }
QComboBox:disabled        { color: %5; background: %6; border-color: %7; }
QComboBox::drop-down      { border: none; width: 22px; }
QComboBox::down-arrow     { image: url(:/icons/arrow-down.png); width: 10px; height: 10px; }
QComboBox QAbstractItemView {
    background: %8;
    border: 1px solid %3;
    outline: none;
    font-size: %16pt;
    padding: 2px;
    selection-background-color: %1;
    selection-color: %9;
}
/* Only honoured with a QStyledItemDelegate on the combo — MainWindow's
   applyTheme() gives every combo one; Qt's default menu-style delegate
   ignores these and paints its own darker hover. */
QComboBox QAbstractItemView::item { padding: 3px 6px; }
QComboBox QAbstractItemView::item:hover,
QComboBox QAbstractItemView::item:selected { background: %1; color: %9; }

QSpinBox, QDoubleSpinBox {
    background: %2;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 3px 6px;
}
QSpinBox:focus, QDoubleSpinBox:focus { border-color: %1; }
QSpinBox:disabled, QDoubleSpinBox:disabled {
    color: %5; background: %6; border-color: %7;
}
QSpinBox::up-button, QDoubleSpinBox::up-button,
QSpinBox::down-button, QDoubleSpinBox::down-button {
    background: transparent;
    border: none;
    width: 16px;
}
QSpinBox::up-arrow, QDoubleSpinBox::up-arrow     { image: url(:/icons/arrow-up.png); width: 10px; height: 10px; }
QSpinBox::down-arrow, QDoubleSpinBox::down-arrow { image: url(:/icons/arrow-down.png); width: 10px; height: 10px; }
QCheckBox { spacing: 8px; }
QCheckBox::indicator {
    width: 15px; height: 15px;
    border: 1px solid %4;
    border-radius: 3px;
    background: %2;
}
QCheckBox::indicator:hover    { border-color: %1; }
QCheckBox::indicator:checked  { background: %1; border-color: %1; }
QCheckBox::indicator:disabled { background: %6; border-color: %7; }

QPushButton {
    background: %2;
    border: 1px solid %3;
    border-radius: 5px;
    padding: 5px 12px;
}
QPushButton:hover    { background: %10; border-color: %4; }
QPushButton:pressed  { background: %11; }
QPushButton:disabled { background: %6; color: %5; border-color: %7; }
QPushButton:checked  { background: %1; border-color: %1; color: %9; }
QPushButton:checked:hover { background: %1; }

QLineEdit {
    background: %12;
    border: 1px solid %3;
    border-radius: 4px;
    padding: 3px 6px;
    selection-background-color: %1;
    selection-color: %9;
}
QLineEdit:focus    { border-color: %1; }
QLineEdit:disabled { color: %5; background: %6; border-color: %7; }

QScrollBar:vertical {
    background: transparent;
    width: 10px;
    margin: 0;
}
QScrollBar::handle:vertical       { background: %3; border-radius: 4px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: %4; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical   { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical   { background: transparent; }
QScrollBar:horizontal {
    background: transparent;
    height: 10px;
}
QScrollBar::handle:horizontal       { background: %3; border-radius: 4px; min-width: 24px; }
QScrollBar::handle:horizontal:hover { background: %4; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }

QToolTip {
    background: %8;
    color: %13;
    border: 1px solid %1;
    border-radius: 4px;
    padding: 4px 8px;
}

QStatusBar          { background: %12; border-top: 1px solid %7; }
QStatusBar::item     { border: none; }

QMenu {
    background: %8;
    border: 1px solid %3;
    padding: 4px;
}
QMenu::item          { padding: 5px 20px; border-radius: 3px; }
QMenu::item:selected { background: %1; color: %9; }

QGroupBox {
    border: 1px solid %7;
    border-radius: 5px;
    margin-top: 10px;
    padding-top: 6px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    left: 8px;
    padding: 0 4px;
    color: %14;
}
)").arg(accentHex(), surfaceBackground().name(), border().name(), borderHover().name(),
        textDisabled().name(), elevatedBackground().name(), borderSoft().name(),
        elevatedBackground().name())
   .arg(onAccent().name(), surfaceHoverBackground().name(), surfacePressedBackground().name(),
        recessedBackground().name(), text().name(), textMuted().name())
   .arg(QString::number(baseSize * 1.10, 'f', 1), QString::number(baseSize, 'f', 1))
   + dropdownListStyle();
}

}   // namespace Theme
