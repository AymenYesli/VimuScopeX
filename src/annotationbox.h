#pragma once

#include <QColor>
#include <QWidget>

class QPlainTextEdit;

/*
 * A floating note: just a bordered, padded, auto-growing plain-text box.
 * Nothing else lives inside it — no header, no trash button, no resize
 * grip. All of that chrome (trash icon, a move handle, resize handles) is
 * drawn and hit-tested by ScopeView itself, on the canvas, the same way
 * Rulers' own close/minimize buttons already are — painter-drawn, not real
 * child widgets. That is not a style choice: a real child widget positioned
 * outside its parent's own rectangle gets clipped to that rectangle by Qt
 * and simply never renders, which is exactly what made an earlier version
 * of this box's trash button invisible. Keeping this widget to just its
 * visible, fully-interior content sidesteps that entirely.
 *
 * Clicking anywhere in the box (it has a small focus proxy margin, but is
 * otherwise just the text edit) focuses the text edit for normal editing.
 * ScopeView finds out via selectionChanged(), which is how it knows when to
 * draw and enable the external chrome around this particular box.
 *
 * No scrollbars: the text edit never scrolls, and instead the box grows
 * taller on its own whenever the text needs more room than it has.
 */
class AnnotationBox : public QWidget {
    Q_OBJECT

public:
    explicit AnnotationBox(QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);

    void setAccentColor(const QColor &color);
    QColor accentColor() const { return m_accent; }

    // Re-applies the theme's colours after a light/dark switch. The text
    // edit holds its own palette (see the constructor), so it doesn't follow
    // the application palette by itself. The accent is left alone — it can
    // be chosen per annotation.
    void refreshTheme();

    // Focuses the text edit itself, not the frame around it, so a caller
    // (ScopeView, right after creating one) can drop the user straight into
    // typing without an extra click.
    void focusText();

    // ScopeView clamps any resize it applies (via the external resize
    // handles) to these, so a box can never be dragged down to nothing.
    static constexpr int kMinWidth  = 90;
    static constexpr int kMinHeight = 30;

signals:
    // Text, position, or size changed — the caller repaints whatever it
    // draws relative to this box (e.g. a connecting line to it).
    void changed();
    // The text edit gained or lost keyboard focus — the caller uses this to
    // decide whether this annotation is the one that should currently show
    // its external chrome (trash, move handle, resize handles, and the
    // default not-yet-aimed arrow).
    void selectionChanged(bool selected);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void layoutChildren();
    void growToFitText();

    QPlainTextEdit *m_text = nullptr;
    QColor          m_accent;
    bool            m_growing = false;   // re-entrancy guard: growToFitText() resizes itself

    static constexpr int kPadding = 4;
};
