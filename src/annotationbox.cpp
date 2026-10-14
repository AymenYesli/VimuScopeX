#include "annotationbox.h"

#include <QApplication>

#include "theme.h"

#include <QEvent>
#include <QPainter>
#include <QPlainTextEdit>

#include <algorithm>

AnnotationBox::AnnotationBox(QWidget *parent)
    : QWidget(parent)
    , m_accent(Theme::accent())
{
    // Click-focus plus a focus proxy: a click anywhere on this widget's own
    // surface (the thin padding margin around the text edit — the text edit
    // itself, being a real child, takes clicks landing on it directly) ends
    // up focusing the text edit either way.
    setFocusPolicy(Qt::ClickFocus);

    m_text = new QPlainTextEdit(this);
    setFocusProxy(m_text);
    m_text->setFrameShape(QFrame::NoFrame);
    m_text->setPlaceholderText(QStringLiteral("Note..."));
    m_text->setCursor(Qt::IBeamCursor);
    // Otherwise QPlainTextEdit paints its own background from the app-wide
    // palette (QPalette::Base, a dark blue-grey — see main.cpp) via its
    // viewport, which covers nearly the entire box; this widget's own
    // paintEvent() fill would only ever show through the thin padding
    // margin around it. Opaque, not matching annotationBackground()'s alpha:
    // it is already drawn directly over that same fill, so there is nothing
    // underneath it that needs to show through.
    refreshTheme();
    // No scrolling: the box grows to fit instead (see growToFitText()).
    m_text->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    connect(m_text, &QPlainTextEdit::textChanged, this, [this] {
        growToFitText();
        emit changed();
    });
    // QPlainTextEdit has no focus-changed signal of its own, so this is
    // watched for directly — see eventFilter().
    m_text->installEventFilter(this);

    setAccentColor(m_accent);
    resize(150, 34);
    layoutChildren();
}

void AnnotationBox::refreshTheme()
{
    // A style sheet, not setPalette(): with the app-wide style sheet active,
    // Qt's style-sheet engine remembers a widget's custom palette and puts
    // it back whenever it re-polishes the widget — which a theme switch
    // triggers just after this runs — so a palette-based background stayed
    // on the previous theme. Rules in the widget's own style sheet are
    // re-applied on every polish instead. Opaque on purpose (see the
    // constructor): this is drawn over paintEvent()'s own fill.
    const QColor bg = Theme::annotationBackground();
    m_text->setStyleSheet(QStringLiteral(
        "QPlainTextEdit { background: rgb(%1, %2, %3); color: %4; border: none; }")
        .arg(bg.red()).arg(bg.green()).arg(bg.blue())
        .arg(Theme::text().name()));
    update();
}

QString AnnotationBox::text() const
{
    return m_text->toPlainText();
}

void AnnotationBox::setText(const QString &text)
{
    m_text->setPlainText(text);
    growToFitText();
}

void AnnotationBox::setAccentColor(const QColor &color)
{
    m_accent = color;
    update();
}

void AnnotationBox::focusText()
{
    m_text->setFocus();
}

void AnnotationBox::layoutChildren()
{
    const int textW = std::max(10, width()  - kPadding * 2);
    const int textH = std::max(10, height() - kPadding * 2);
    m_text->setGeometry(kPadding, kPadding, textW, textH);
}

void AnnotationBox::growToFitText()
{
    if (m_growing)
        return;

    // No scrollbars (see the constructor), so once the text needs more
    // vertical room than the edit currently has, the box grows to fit
    // rather than clipping or scrolling. Only ever grows, never shrinks —
    // shrinking back down is what ScopeView's resize handles are for.
    m_text->document()->setTextWidth(m_text->viewport()->width());
    const int neededTextHeight = int(m_text->document()->size().height()) + 2;
    const int neededBoxHeight  = neededTextHeight + kPadding * 2;
    if (neededBoxHeight > height()) {
        m_growing = true;
        resize(width(), neededBoxHeight);   // resizeEvent() re-lays-out and emits changed()
        m_growing = false;
    }
}

void AnnotationBox::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF full(0.5, 0.5, width() - 1.0, height() - 1.0);
    p.setBrush(Theme::annotationBackground());
    p.setPen(QPen(m_accent, 1.0));
    p.drawRoundedRect(full, 4, 4);
}

void AnnotationBox::resizeEvent(QResizeEvent *event)
{
    layoutChildren();
    growToFitText();   // narrowing the box can force more wrapped lines
    QWidget::resizeEvent(event);
    emit changed();
}

bool AnnotationBox::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_text) {
        if (event->type() == QEvent::FocusIn)
            emit selectionChanged(true);
        else if (event->type() == QEvent::FocusOut)
            emit selectionChanged(false);
    }
    return QWidget::eventFilter(watched, event);
}
