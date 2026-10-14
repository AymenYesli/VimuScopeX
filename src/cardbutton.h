#pragma once

#include <QColor>
#include <QIcon>
#include <QFrame>
#include <QString>

#include "theme.h"

class QLabel;
class QToolButton;

/*
 * A card in the top bar or the left rail: a small title, a prominent value, an
 * optional corner note, and optional -/+ steppers down each side. Clicking the
 * body selects the card, which is what opens the detail panel.
 */
class CardButton : public QFrame {
    Q_OBJECT

public:
    explicit CardButton(const QString &title, QWidget *parent = nullptr);

    void setTitle(const QString &title);
    void setValue(const QString &value);
    void setNote(const QString &note);          // small text, top right
    void setDetail(const QString &detail);      // small text, bottom right
    void setIcon(const QIcon &icon);            // optional, left of the title
    void setAccent(const QColor &color);        // title colour and selected border
    void setSteppersVisible(bool visible);
    void setSteppersInteractive(bool interactive);
    void updateStepIcons();   // false: bars, no glyphs
    void setCompact(bool compact);                   // one row, for half cards
    // The - / + glyphs on a card whose steppers don't (yet) do anything:
    // shown, but the bars still take no clicks.
    void setStepperGlyphs(bool shown);
    void setSideColor(const QColor &color);          // overrides the accent
    void setSelected(bool selected);
    void setMuted(bool muted);                  // dimmed, for a channel that is off

    bool isSelected() const { return m_selected; }

    // Re-runs restyle() against Theme::currentMode()'s latest colours —
    // called by MainWindow::applyTheme() on every existing card when the
    // Dark/Light toggle changes at runtime, since nothing else would
    // otherwise prompt an already-built card to recompute its own
    // background/border/text colours.
    void refreshTheme() { restyle(); }

signals:
    void clicked();
    void stepUp();
    void stepDown();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    void restyle();

    QLabel      *m_title = nullptr;
    QLabel      *m_value = nullptr;
    QLabel      *m_note  = nullptr;
    QLabel      *m_detail = nullptr;
    QLabel      *m_icon  = nullptr;
    QToolButton *m_minus = nullptr;
    QToolButton *m_plus  = nullptr;

    QColor m_accent  = Theme::textMuted();
    bool   m_selected = false;
    bool   m_hovered  = false;
    bool   m_muted    = false;
    bool   m_steppers = true;
    bool   m_compact = false;
    void   layoutCompact();      // compact: title centred, or left with the note on the right
    bool   m_glyphsShown = false;   // signs shown even when not interactive (setStepperGlyphs())
    QColor m_sideColor;
};
