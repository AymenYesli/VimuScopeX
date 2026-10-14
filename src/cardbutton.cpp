#include "cardbutton.h"
#include "icons.h"
#include "fonts.h"

#include "theme.h"

#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QToolButton>

CardButton::CardButton(const QString &title, QWidget *parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(56);

    // The steppers run the full height of the card, flush to its edges, as on
    // PicoScope. Nothing sits between them and the border.
    auto *grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(0);
    grid->setVerticalSpacing(0);

    m_minus = new QToolButton;
    m_minus->setObjectName(QStringLiteral("cardMinus"));
    m_minus->setIconSize(QSize(12, 12));
    m_minus->setAutoRepeat(true);
    m_minus->setFixedWidth(23);
    m_minus->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_minus->setCursor(Qt::PointingHandCursor);

    m_plus = new QToolButton;
    m_plus->setObjectName(QStringLiteral("cardPlus"));
    m_plus->setIconSize(QSize(12, 12));
    m_plus->setAutoRepeat(true);
    m_plus->setFixedWidth(23);
    m_plus->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    m_plus->setCursor(Qt::PointingHandCursor);

    m_title = new QLabel(title);
    m_title->setContentsMargins(8, 4, 0, 0);
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    QFont titleFont = m_title->font();
    titleFont.setPointSizeF((titleFont.pointSizeF() - Fonts::scaled(1.5)) * 1.10);
    m_title->setFont(titleFont);

    // Same font, same margins and the same vertical alignment as the title, so
    // "Scope / 500 kS/s" and "CH1 / DC x1" read as one aligned line rather than
    // two labels that happen to share a row.
    m_note = new QLabel;
    m_note->setFont(titleFont);
    m_note->setContentsMargins(0, 4, 8, 0);
    m_note->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_detail = new QLabel;
    m_detail->setFont(titleFont);
    m_detail->setContentsMargins(0, 0, 8, 4);
    m_detail->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_value = new QLabel;
    m_value->setContentsMargins(8, 0, 8, 6);
    QFont valueFont = m_value->font();
    valueFont.setPointSizeF((valueFont.pointSizeF() - Fonts::scaled(0.5)) * 1.10);
    m_value->setFont(valueFont);

    m_icon = new QLabel;
    m_icon->setVisible(false);
    m_icon->setContentsMargins(8, 0, 0, 0);
    m_icon->setAlignment(Qt::AlignCenter);

    grid->addWidget(m_minus, 0, 0, 2, 1);
    grid->addWidget(m_icon,  0, 1, 2, 1);
    grid->addWidget(m_title, 0, 2);
    grid->addWidget(m_note,  0, 3);
    // The value spans both columns (the whole card width); the detail sits
    // over the right end of the same row (value left-aligned, detail right).
    // Confining the value to the left column clipped it ("±500 m") and, by
    // widening that column, squeezed the note above ("C x1 AUTO").
    grid->addWidget(m_value, 1, 2, 1, 2);
    grid->addWidget(m_detail, 1, 3);
    grid->addWidget(m_plus,  0, 4, 2, 1);
    grid->setColumnStretch(2, 1);

    connect(m_minus, &QToolButton::clicked, this, &CardButton::stepDown);
    connect(m_plus,  &QToolButton::clicked, this, &CardButton::stepUp);

    restyle();
}

void CardButton::setTitle(const QString &title)   { m_title->setText(title); }
void CardButton::setValue(const QString &value)   { m_value->setText(value); }
void CardButton::setNote(const QString &note)
{
    m_note->setText(note);
    if (m_compact)
        layoutCompact();
}
void CardButton::setDetail(const QString &detail) { m_detail->setText(detail); }

void CardButton::setIcon(const QIcon &icon)
{
    if (icon.isNull()) {
        m_icon->setVisible(false);
        return;
    }
    m_icon->setPixmap(icon.pixmap(18, 18));
    m_icon->setVisible(true);
}

void CardButton::setAccent(const QColor &color)
{
    m_accent = color;
    restyle();
}

// The step buttons' icons: drawn on the coloured bars in the bars' text
// colour; none on a card that has nothing to step. SVG icons (centred on the
// bar) where available — the text minus sat below the middle — else the old
// glyphs.
void CardButton::updateStepIcons()
{
    const bool svg = Icons::hasSvg(QStringLiteral("card-minus")) && Icons::hasSvg(QStringLiteral("card-plus"));
    // Shown on cards that step, and on the compact ones that show their signs
    // before they do anything (Logic, DIO: setStepperGlyphs(true)) — the
    // icons briefly cleared those.
    if (!m_steppers && !m_glyphsShown) {
        for (QToolButton *b : { m_minus, m_plus }) {
            b->setIcon(QIcon());
            b->setText(QString());
        }
        return;
    }
    if (svg) {
        m_minus->setText(QString());
        m_plus->setText(QString());
        m_minus->setIcon(Icons::icon(QStringLiteral("card-minus"), Theme::onAccentBar(), 12));
        m_plus->setIcon(Icons::icon(QStringLiteral("card-plus"), Theme::onAccentBar(), 12));
    } else {
        m_minus->setIcon(QIcon());
        m_plus->setIcon(QIcon());
        m_minus->setText(QStringLiteral("\u2212"));
        m_plus->setText(QStringLiteral("+"));
    }
}

// The run/stop card keeps the side bars for symmetry with the other cards but
// has nothing to step, so the glyphs go and the bars stop taking clicks.
void CardButton::setSteppersInteractive(bool interactive)
{
    m_steppers = interactive;
    updateStepIcons();
    for (QToolButton *b : { m_minus, m_plus }) {
        b->setAttribute(Qt::WA_TransparentForMouseEvents, !interactive);
        b->setCursor(interactive ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}

// Half-height cards have no room for a second row, so the value label goes and
// the title centres in what is left.
void CardButton::setStepperGlyphs(bool shown)
{
    m_glyphsShown = shown;
    updateStepIcons();
}

// Compact: the title alone, centred across the card's full height (a
// two-row span: centred in the top row only, it looked top-heavy); with a note
// (Math: its volts per page), the title on the left and the note on the right.
void CardButton::layoutCompact()
{
    auto *grid = qobject_cast<QGridLayout *>(layout());
    const bool withNote = !m_note->text().isEmpty();
    m_note->setVisible(withNote);
    if (grid) {
        grid->removeWidget(m_title);
        grid->removeWidget(m_note);
        grid->addWidget(m_title, 0, 2, 2, withNote ? 1 : 2);
        if (withNote)
            grid->addWidget(m_note, 0, 3, 2, 1);
    }
    m_title->setContentsMargins(withNote ? 8 : 0, 0, 0, 0);
    m_title->setAlignment(withNote ? (Qt::AlignLeft | Qt::AlignVCenter) : Qt::AlignCenter);
    m_note->setContentsMargins(0, 0, 8, 0);
}

void CardButton::setCompact(bool compact)
{
    m_compact = compact;
    m_value->setVisible(!compact);
    m_note->setVisible(!compact);
    m_detail->setVisible(!compact);
    if (compact) {
        layoutCompact();
        return;
    }
    if (auto *grid = qobject_cast<QGridLayout *>(layout())) {
        grid->removeWidget(m_title);
        grid->removeWidget(m_note);
        grid->addWidget(m_title, 0, 2);
        grid->addWidget(m_note, 0, 3);
    }
    m_title->setContentsMargins(8, 4, 0, 0);
    m_title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_note->setContentsMargins(0, 4, 8, 0);
}

void CardButton::setSideColor(const QColor &color)
{
    if (m_sideColor == color)
        return;
    m_sideColor = color;
    restyle();
}

void CardButton::setSteppersVisible(bool visible)
{
    m_minus->setVisible(visible);
    m_plus->setVisible(visible);
}

void CardButton::setSelected(bool selected)
{
    if (m_selected == selected)
        return;
    m_selected = selected;
    restyle();
}

void CardButton::setMuted(bool muted)
{
    if (m_muted == muted)
        return;
    m_muted = muted;
    restyle();
}

void CardButton::mousePressEvent(QMouseEvent *event)
{
    // The steppers are children and handle their own clicks; anything else on
    // the card selects it.
    if (event->button() == Qt::LeftButton)
        emit clicked();
    QFrame::mousePressEvent(event);
}

void CardButton::enterEvent(QEnterEvent *event)
{
    m_hovered = true;
    restyle();
    QFrame::enterEvent(event);
}

void CardButton::leaveEvent(QEvent *event)
{
    m_hovered = false;
    restyle();
    QFrame::leaveEvent(event);
}

void CardButton::restyle()
{
    const QColor background = m_selected ? Theme::cardSelectedBackground()
                            : m_hovered  ? Theme::cardHoverBackground()
                                         : Theme::cardBackground();
    // No visible border unless selected. The 1 px stays (transparent, with
    // the background clipped inside it) so selecting a card doesn't shift it
    // — painting it in the card's colour left a rim round the side bars,
    // plain on the light theme.
    const QString border = m_selected ? Theme::accent().name() : QStringLiteral("transparent");

    // Solid accent bars down both edges with a dark glyph, sharing the card's
    // corner radius on their outer side so the card reads as one rounded block.
    const QColor side = m_sideColor.isValid() ? m_sideColor : Theme::accent();
    const QString hover = side.lighter(115).name();
    const QString press = side.darker(115).name();

    setStyleSheet(QStringLiteral(
        "CardButton { background: %1; border: 1px solid %2; border-radius: 8px;"
        "             background-clip: padding; }"
        "QToolButton { background: %3; border: none; color: %6;"
        "              font-size: 20px; font-weight: bold; }"
        "QToolButton:hover  { background: %4; }"
        "QToolButton:pressed{ background: %5; }"
        "QToolButton#cardMinus { border-top-left-radius: 7px;"
        "                        border-bottom-left-radius: 7px; }"
        "QToolButton#cardPlus  { border-top-right-radius: 7px;"
        "                        border-bottom-right-radius: 7px; }")
        .arg(background.name(), border, side.name(), hover, press,
             Theme::onAccentBar().name()));

    updateStepIcons();
    const QColor titleColour = m_muted ? Theme::textDisabled() : m_accent;
    const QColor valueColour = m_muted ? Theme::textDisabled() : Theme::text();

    m_title->setStyleSheet(QStringLiteral("color: %1;").arg(titleColour.name()));
    m_value->setStyleSheet(QStringLiteral("color: %1;").arg(valueColour.name()));
    m_note->setStyleSheet(QStringLiteral("color: %1;")
                              .arg(Theme::textMuted().name()));
    m_detail->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
}
