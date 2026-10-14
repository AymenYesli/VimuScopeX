#include "vsxdialog.h"

#include "icons.h"
#include "theme.h"

#include <QColorDialog>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <iterator>
#include <vector>

namespace {

QColor edgeColour() { return Theme::isDark() ? QColor(0x3a, 0x3a, 0x3a) : QColor(0xd0, 0xd0, 0xd0); }

// Buttons, fields and lists in the sliding panel's look.
QString dialogStyle()
{
    const bool dark = Theme::isDark();
    const QColor rest = Theme::windowBackground();
    const QColor hover = dark ? QColor(0x23, 0x23, 0x23) : QColor(0xdb, 0xe0, 0xe5);
    const QColor press = dark ? QColor(0x14, 0x14, 0x14) : QColor(0xd2, 0xd8, 0xde);
    const QColor disabled = dark ? QColor(0x30, 0x30, 0x30) : QColor(0xe2, 0xe2, 0xe2);
    return QStringLiteral(
        "QLabel { color: %1; background: transparent; }"
        "QPushButton { background: %2; color: %1; border: 1px solid %5; border-radius: 5px;"
        " padding: 0px 14px; min-height: 26px; max-height: 26px; }"
        "QPushButton:hover { background: %3; }"
        "QPushButton:pressed { background: %4; }"
        "QPushButton:checked, QPushButton[primary=\"true\"] { background: %5; color: %6; }"
        "QPushButton[primary=\"true\"]:hover { background: %7; }"
        "QPushButton:disabled { background: %8; color: %9; border-color: %10; }"
        "QLineEdit, QAbstractSpinBox, QComboBox { background: %2; color: %1; border: 1px solid transparent;"
        " border-radius: 5px; padding: 3px 8px; min-height: 20px; }"
        "QLineEdit:focus, QAbstractSpinBox:focus, QComboBox:focus { border-color: %5; }"
        "QListWidget, QTableWidget, QPlainTextEdit, QTextEdit { background: %2; color: %1; border: none;"
        " border-radius: 5px; padding: 4px; outline: none; }"
        "QListWidget::item { padding: 4px 8px; border-radius: 4px; }"
        "QListWidget::item:hover { background: %3; }"
        "QListWidget::item:selected { background: %5; color: %6; }"
        "QTableWidget { gridline-color: %10; }"
        "QHeaderView::section { background: %11; color: %12; border: none; padding: 4px; }")
        .arg(Theme::text().name(), rest.name(), hover.name(), press.name(), Theme::accentHex(),
             Theme::onAccent().name(), Theme::accent().lighter(110).name(), disabled.name(),
             Theme::textDisabled().name())
        .arg(edgeColour().name(), Theme::cardBackground().name(), Theme::textMuted().name());
}

} // namespace

VsxDialog::VsxDialog(QWidget *parent, const QString &title) : QDialog(parent)
{
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    if (!title.isEmpty())
        setWindowTitle(title);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(1, 1, 1, 1);           // inside the 1 px edge painted below
    root->setSpacing(0);

    // The title bar: accent, title centred (a space the close button's width
    // on the left keeps it centred), close at the right; drag it to move.
    auto *bar = new QFrame;
    bar->setObjectName(QStringLiteral("vsxDialogTitle"));
    bar->setFixedHeight(30);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(2, 2, 2, 2);
    barLayout->setSpacing(0);
    m_title = new QLabel(windowTitle());
    m_title->setAlignment(Qt::AlignCenter);
    QFont bold = m_title->font();
    bold.setBold(true);
    m_title->setFont(bold);
    auto *close = new QToolButton;
    close->setObjectName(QStringLiteral("vsxDialogClose"));
    close->setFixedSize(26, 26);
    close->setFocusPolicy(Qt::NoFocus);
    connect(close, &QToolButton::clicked, this, &QDialog::reject);
    barLayout->addSpacing(26);
    barLayout->addWidget(m_title, 1);
    barLayout->addWidget(close);
    root->addWidget(bar);
    m_titleBar = bar;
    bar->installEventFilter(this);

    auto *body = new QWidget;
    auto *bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(14, 12, 14, 14);
    bodyLayout->setSpacing(12);
    m_content = new QVBoxLayout;
    m_content->setContentsMargins(0, 0, 0, 0);
    m_content->setSpacing(8);
    bodyLayout->addLayout(m_content, 1);
    m_buttons = new QHBoxLayout;
    m_buttons->setSpacing(8);
    m_buttons->addStretch(1);
    bodyLayout->addLayout(m_buttons);
    root->addWidget(body, 1);

    restyle();
}

QPushButton *VsxDialog::addButton(const QString &text, bool primary, std::function<void()> onClick)
{
    auto *button = new QPushButton(text);
    button->setProperty("primary", primary);
    button->setMinimumWidth(84);
    button->setAutoDefault(false);
    button->setDefault(primary);
    connect(button, &QPushButton::clicked, this, [onClick] {
        if (onClick)
            onClick();
    });
    m_buttons->addWidget(button);
    return button;
}

void VsxDialog::restyle()
{
    const QColor ink = Theme::onAccentBar();
    m_titleBar->setStyleSheet(QStringLiteral(
        "QFrame#vsxDialogTitle { background: %1; border-top-left-radius: 7px; border-top-right-radius: 7px; }"
        "QLabel { color: %2; background: transparent; }"
        "QToolButton { background: transparent; border: none; border-radius: 4px; }"
        "QToolButton:hover { background: rgba(0, 0, 0, 40); }")
        .arg(Theme::accentHex(), ink.name()));
    if (auto *close = m_titleBar->findChild<QToolButton *>(QStringLiteral("vsxDialogClose"))) {
        const QIcon icon = Icons::icon(QStringLiteral("xmark"), ink, 16);
        if (icon.isNull())
            close->setText(QStringLiteral("\u00d7"));
        else
            close->setIcon(icon);
    }
    setStyleSheet(dialogStyle());
}

// The card: rounded, the cards' colour, a neutral edge (no window frame).
void VsxDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
    p.setPen(QPen(edgeColour(), 1.0));
    p.setBrush(Theme::cardBackground());
    p.drawPath(path);
}

void VsxDialog::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowTitleChange && m_title)
        m_title->setText(windowTitle());           // setWindowTitle() keeps working
    QDialog::changeEvent(event);
}

bool VsxDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleBar) {
        switch (event->type()) {
        case QEvent::MouseButtonPress: {
            auto *e = static_cast<QMouseEvent *>(event);
            if (e->button() == Qt::LeftButton) {
                m_dragging = true;
                m_dragOffset = e->globalPosition().toPoint() - frameGeometry().topLeft();
                return true;
            }
            break;
        }
        case QEvent::MouseMove:
            if (m_dragging) {
                move(static_cast<QMouseEvent *>(event)->globalPosition().toPoint() - m_dragOffset);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            m_dragging = false;
            break;
        default:
            break;
        }
    }
    return QDialog::eventFilter(watched, event);
}

namespace VsxDialogs {

namespace {
QLabel *messageLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setMinimumWidth(340);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
} // namespace

bool question(QWidget *parent, const QString &title, const QString &text, const QString &yes, const QString &no)
{
    VsxDialog dialog(parent, title);
    dialog.contentLayout()->addWidget(messageLabel(text));
    dialog.addButton(no, false, [&dialog] { dialog.reject(); });
    dialog.addButton(yes, true, [&dialog] { dialog.accept(); });
    return dialog.exec() == QDialog::Accepted;
}

void warning(QWidget *parent, const QString &title, const QString &text)
{
    VsxDialog dialog(parent, title);
    dialog.contentLayout()->addWidget(messageLabel(text));
    dialog.addButton(QStringLiteral("OK"), true, [&dialog] { dialog.accept(); });
    dialog.exec();
}

// Swatches that read well on the plot, plus a hex field for anything else.
QColor pickColour(const QColor &initial, QWidget *parent, const QString &title)
{
    static const char *const kPalette[] = {
        "#ffd400", "#ffb300", "#ff8a00", "#ff5a36", "#ff3b3b", "#ff2d7a", "#e040fb", "#b44cff",
        "#7c4dff", "#536dfe", "#2f80ff", "#00a3ff", "#00c8ff", "#00e5d4", "#00e676", "#73ff00",
        "#c6ff00", "#eeff41", "#ffab40", "#ffd180", "#ff9e80", "#f48fb1", "#ce93d8", "#b388ff",
        "#80d8ff", "#a7ffeb", "#b9f6ca", "#ffffff", "#d0d0d0", "#a0a0a0", "#707070", "#8d6e63",
    };
    VsxDialog dialog(parent, title);
    QColor chosen = initial.isValid() ? initial : QColor(kPalette[0]);

    auto *grid = new QGridLayout;
    grid->setSpacing(6);
    std::vector<QToolButton *> swatches;
    auto *preview = new QFrame;
    preview->setFixedSize(40, 26);
    auto *hex = new QLineEdit;
    hex->setMaxLength(7);
    hex->setFixedWidth(110);

    std::function<void(const QColor &)> choose = [&](const QColor &c) {
        chosen = c;
        preview->setStyleSheet(QStringLiteral("QFrame { background: %1; border: 1px solid %2; border-radius: 5px; }")
                                   .arg(c.name(), edgeColour().name()));
        if (hex->text().compare(c.name(), Qt::CaseInsensitive) != 0)
            hex->setText(c.name());
        for (QToolButton *s : swatches)
            s->setChecked(s->property("colour").toString().compare(c.name(), Qt::CaseInsensitive) == 0);
    };

    const int columns = 8;
    for (int i = 0; i < int(std::size(kPalette)); ++i) {
        const QColor c(kPalette[i]);
        auto *s = new QToolButton;
        s->setCheckable(true);
        s->setFixedSize(28, 28);
        s->setFocusPolicy(Qt::NoFocus);
        s->setProperty("colour", c.name());
        s->setToolTip(c.name());
        s->setStyleSheet(QStringLiteral(
            "QToolButton { background: %1; border: 2px solid transparent; border-radius: 6px; }"
            "QToolButton:hover { border-color: %2; }"
            "QToolButton:checked { border-color: %3; }")
            .arg(c.name(), Theme::textMuted().name(), Theme::text().name()));
        QObject::connect(s, &QToolButton::clicked, &dialog, [&choose, c] { choose(c); });
        grid->addWidget(s, i / columns, i % columns);
        swatches.push_back(s);
    }
    dialog.contentLayout()->addLayout(grid);

    auto *row = new QHBoxLayout;
    row->setSpacing(8);
    row->addWidget(preview);
    row->addWidget(new QLabel(QStringLiteral("Hex")));
    row->addWidget(hex);
    row->addStretch(1);
    // Anything the swatches don't have, picked visually: the system's picker
    // (a wheel / field), starting from the current choice.
    auto *custom = new QPushButton(QStringLiteral("Custom colour\u2026"));
    custom->setAutoDefault(false);
    QObject::connect(custom, &QPushButton::clicked, &dialog, [&] {
        const QColor c = QColorDialog::getColor(chosen, &dialog, title);
        if (c.isValid())
            choose(c);
    });
    row->addWidget(custom);
    dialog.contentLayout()->addLayout(row);
    QObject::connect(hex, &QLineEdit::textEdited, &dialog, [&choose](const QString &text) {
        const QColor c(text.startsWith(QLatin1Char('#')) ? text : QLatin1Char('#') + text);
        if (c.isValid() && (text.size() == 7 || (text.size() == 6 && !text.startsWith(QLatin1Char('#')))))
            choose(c);
    });
    choose(chosen);

    dialog.addButton(QStringLiteral("Cancel"), false, [&dialog] { dialog.reject(); });
    dialog.addButton(QStringLiteral("OK"), true, [&dialog] { dialog.accept(); });
    return dialog.exec() == QDialog::Accepted ? chosen : QColor();
}

} // namespace VsxDialogs
