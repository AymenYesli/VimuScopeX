#include "panelcontrols.h"

#include "icons.h"
#include "theme.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QBoxLayout>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QSpinBox>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <algorithm>
#include <cmath>
#include <functional>

namespace PanelControls {
namespace {

// Every button in the panel one height (segments, Off | On, toggles, plain
// buttons — some were taller, with an icon or a style of their own), and the
// compact-card fields 20 % lower than they were (30 px), their text 10 %
// smaller.
constexpr int kButtonHeight = 26;
constexpr int kFieldHeight = 24;
// One width for every field on every page (65 % of the 230 px a page has),
// whatever the row or the texts in it.
constexpr int kFieldWidth = 150;
// Buttons and the titles over settings: 10 % smaller than the body text.
constexpr double kSmallerText = 0.9;

// The wrappers keep their height: on a page whose layout had room to spare,
// stretchable wrappers took different shares of it and the gaps between
// rows came out uneven (Math, Remote).
void fixHeight(QWidget *outer)
{
    outer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

// ---- layout plumbing ---------------------------------------------------------

QLayout *layoutHolding(QLayout *layout, QWidget *w)
{
    if (!layout)
        return nullptr;
    if (layout->indexOf(w) >= 0)
        return layout;
    for (int i = 0; i < layout->count(); ++i) {
        if (QLayout *child = layout->itemAt(i)->layout()) {
            if (QLayout *found = layoutHolding(child, w))
                return found;
        }
    }
    return nullptr;
}

QLayout *parentLayoutOf(QWidget *w)
{
    return w && w->parentWidget() ? layoutHolding(w->parentWidget()->layout(), w) : nullptr;
}

void repolish(QWidget *w)
{
    w->style()->unpolish(w);
    w->style()->polish(w);
    w->update();
}

// Keeps a stand-in in step with the control it replaced on screen: code
// elsewhere still shows, hides, enables and disables the original. Shown /
// hidden goes to the wrapper; enabled goes to the controls beside the
// original (bars, Off | On...), never to the wrapper holding it: a control
// inside a disabled parent gets no EnabledChange when it is re-enabled (it's
// still disabled through the parent), so the wrapper stayed greyed (a channel
// turned off and on kept its volts field greyed).
class Mirror : public QObject
{
public:
    Mirror(QWidget *source, QWidget *shown, QList<QWidget *> enabled = {},
           std::function<void()> extra = {})
        : QObject(source), m_target(shown), m_extra(std::move(extra))
    {
        for (QWidget *w : enabled)
            m_enabled << w;
        source->installEventFilter(this);
        sync(source);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::ShowToParent:
        case QEvent::HideToParent:
        case QEvent::EnabledChange:
            sync(static_cast<QWidget *>(watched));
            break;
        default:
            break;
        }
        return false;
    }

private:
    void sync(QWidget *source)
    {
        if (m_target)
            m_target->setVisible(!source->isHidden());
        const bool on = !source->testAttribute(Qt::WA_ForceDisabled);
        for (const QPointer<QWidget> &w : m_enabled) {
            if (w)
                w->setEnabled(on);
        }
        if (m_extra)
            m_extra();
    }

    QPointer<QWidget> m_target;
    QList<QPointer<QWidget>> m_enabled;
    std::function<void()> m_extra;
};

// ---- keeping stand-ins in step with their settings -----------------------------------
//
// The app often updates a setting with its signals blocked (37 places: a
// channel's measurement choices loaded into the check boxes, a coupling
// restored...), so a stand-in following toggled / currentIndexChanged alone
// went stale (Measure's buttons all showed off while the plot showed them).
// While the panel is shown, every stand-in is compared with its setting a few
// times a second and corrected — a few dozen comparisons.
std::vector<std::function<void()>> &syncs()
{
    static std::vector<std::function<void()>> list;
    return list;
}

void startSyncing(QWidget *root)
{
    auto *timer = new QTimer(root);
    timer->setInterval(120);
    QObject::connect(timer, &QTimer::timeout, root, [root] {
        if (!root->isVisible())
            return;
        for (const std::function<void()> &sync : syncs())
            sync();
    });
    timer->start();
}

// ---- segments ------------------------------------------------------------------------

// Left / middle / right among the buttons actually shown, redone whenever one
// is shown or hidden: groups hide their extra channels (CH3 / CH4 on a
// 2-channel scope), and by position the last shown one was styled as a middle
// segment, its right end square. A lone button gets no role (fully rounded).
class SegmentRoles : public QObject
{
public:
    SegmentRoles(const QList<QAbstractButton *> &ordered, QObject *owner) : QObject(owner)
    {
        for (QAbstractButton *b : ordered) {
            m_buttons << b;
            b->installEventFilter(this);
        }
        update();
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::ShowToParent || event->type() == QEvent::HideToParent)
            update();
        return false;
    }

private:
    void update()
    {
        QList<QAbstractButton *> shown;
        for (QAbstractButton *b : m_buttons) {
            if (b && !b->isHidden())
                shown << b;
        }
        for (QAbstractButton *b : m_buttons) {
            if (!b)
                continue;
            const int i = int(shown.indexOf(b));
            const QString role = i < 0 || shown.size() < 2 ? QString()
                               : i == 0 ? QStringLiteral("left")
                               : i == shown.size() - 1 ? QStringLiteral("right") : QStringLiteral("middle");
            if (b->property("segment").toString() != role) {
                b->setProperty("segment", role);
                repolish(b);
            }
        }
    }

    QList<QPointer<QAbstractButton>> m_buttons;
};

// ---- compact-card fields: - [control] + -----------------------------------------

void setBarIcons(QToolButton *minus, QToolButton *plus)
{
    const QColor ink = Theme::onAccentBar();
    const QIcon m = Icons::icon(QStringLiteral("card-minus"), ink, 12);
    const QIcon p = Icons::icon(QStringLiteral("card-plus"), ink, 12);
    if (m.isNull() || p.isNull()) {
        minus->setText(QStringLiteral("\u2212"));
        plus->setText(QStringLiteral("+"));
    } else {
        minus->setIcon(m);
        plus->setIcon(p);
    }
}

void wrapStepper(QWidget *control, std::function<void()> down, std::function<void()> up, bool repeat)
{
    QLayout *holder = parentLayoutOf(control);
    if (!holder)
        return;
    auto *outer = new QWidget;
    auto *row = new QHBoxLayout(outer);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *field = new QFrame;
    field->setObjectName(QStringLiteral("stepField"));
    auto *inner = new QHBoxLayout(field);
    inner->setContentsMargins(0, 0, 0, 0);
    inner->setSpacing(0);
    auto *minus = new QToolButton;
    auto *plus = new QToolButton;
    minus->setObjectName(QStringLiteral("stepMinus"));
    plus->setObjectName(QStringLiteral("stepPlus"));
    for (QToolButton *b : { minus, plus }) {
        b->setFixedWidth(23);
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
        b->setFocusPolicy(Qt::NoFocus);
        b->setIconSize(QSize(12, 12));
        b->setAutoRepeat(repeat);
        b->setAutoRepeatDelay(400);
        b->setAutoRepeatInterval(90);
    }
    setBarIcons(minus, plus);
    field->setFixedHeight(kFieldHeight);
    delete holder->replaceWidget(control, outer);
    inner->addWidget(minus);
    inner->addWidget(control, 1);
    inner->addWidget(plus);
    field->setFixedWidth(kFieldWidth);
    if (auto *combo = qobject_cast<QComboBox *>(control)) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(4);    // its texts don't widen the field
    }
    row->addWidget(field);
    row->addStretch(1);
    fixHeight(outer);
    QObject::connect(minus, &QToolButton::clicked, control, [down] { down(); });
    QObject::connect(plus, &QToolButton::clicked, control, [up] { up(); });
    new Mirror(control, outer, { minus, plus });
}

// The next / previous selectable entry.
void stepCombo(QComboBox *combo, int direction)
{
    const auto *model = qobject_cast<const QStandardItemModel *>(combo->model());
    for (int i = combo->currentIndex() + direction; i >= 0 && i < combo->count(); i += direction) {
        const bool enabled = !model || !model->item(i) || (model->item(i)->flags() & Qt::ItemIsEnabled);
        if (enabled) {
            combo->setCurrentIndex(i);
            emit combo->activated(i);   // a user's pick, like choosing from the list
            return;
        }
    }
}

// A dropdown shown as joined segments (one per item: coupling DC | AC). The
// dropdown stays, at 0 x 0, as the setting; items it disables grey out.
void segmentCombo(QComboBox *combo)
{
    QLayout *holder = parentLayoutOf(combo);
    if (!holder || combo->count() < 2)
        return;
    auto *outer = new QWidget;
    auto *row = new QHBoxLayout(outer);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *group = new QButtonGroup(outer);
    group->setExclusive(true);
    for (int i = 0; i < combo->count(); ++i) {
        auto *b = new QPushButton(combo->itemText(i));
        b->setCheckable(true);
        b->setMinimumWidth(58);
        b->setFixedHeight(kButtonHeight);
        b->setToolTip(combo->toolTip());
        b->setProperty("segment", i == 0 ? QStringLiteral("left")
                                  : i == combo->count() - 1 ? QStringLiteral("right") : QStringLiteral("middle"));
        group->addButton(b, i);
        row->addWidget(b);
    }
    row->addStretch(1);
    fixHeight(outer);
    delete holder->replaceWidget(combo, outer);
    combo->setFixedSize(0, 0);
    combo->setFocusPolicy(Qt::NoFocus);
    row->addWidget(combo);
    const auto show = [combo, group] {
        const auto *model = qobject_cast<const QStandardItemModel *>(combo->model());
        const bool on = !combo->testAttribute(Qt::WA_ForceDisabled);
        for (QAbstractButton *b : group->buttons()) {
            const int i = group->id(b);
            b->setEnabled(on && (!model || !model->item(i) || (model->item(i)->flags() & Qt::ItemIsEnabled)));
            b->setChecked(i == combo->currentIndex());
        }
    };
    show();
    QObject::connect(combo, &QComboBox::currentIndexChanged, outer, show);
    QObject::connect(combo->model(), &QAbstractItemModel::dataChanged, outer, show);
    QObject::connect(group, &QButtonGroup::idClicked, combo, [combo](int i) { combo->setCurrentIndex(i); });
    new Mirror(combo, outer, {}, show);   // enabled per item and per combo, in show()
    syncs().push_back([combo = QPointer<QComboBox>(combo), group = QPointer<QButtonGroup>(group), show] {
        if (!combo || !group)
            return;
        QAbstractButton *current = group->button(combo->currentIndex());
        if (current && !current->isChecked())
            show();
    });
}

// Clicking the number opens the keypad (the line edit would only place a cursor).
class PadOpener : public QObject
{
public:
    PadOpener(QAbstractSpinBox *spin) : QObject(spin), m_spin(spin) {}
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::MouseButtonPress
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            if (m_spin && m_spin->isEnabled() && !m_spin->isReadOnly())
                openNumericPad(m_spin);
            return true;
        }
        return false;
    }

private:
    QPointer<QAbstractSpinBox> m_spin;
};

// ---- check boxes -------------------------------------------------------------------

// A check box in a grid as a toggle button: centred text, no indicator (a
// styled check box can't centre its text and drew its indicator as a dot).
// The box stays, at 0 x 0, as the setting.
void wrapToggle(QCheckBox *box)
{
    QLayout *holder = parentLayoutOf(box);
    if (!holder)
        return;
    auto *outer = new QWidget;
    auto *row = new QHBoxLayout(outer);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *button = new QPushButton(box->text());
    button->setCheckable(true);
    button->setChecked(box->isChecked());
    button->setToolTip(box->toolTip());
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    delete holder->replaceWidget(box, outer);
    row->addWidget(button, 1);
    box->setFixedSize(0, 0);
    box->setFocusPolicy(Qt::NoFocus);
    row->addWidget(box);
    fixHeight(outer);
    QObject::connect(box, &QCheckBox::toggled, button, [button](bool c) { button->setChecked(c); });
    QObject::connect(button, &QPushButton::clicked, box, [box](bool c) { box->setChecked(c); });
    new Mirror(box, outer, { button });
    syncs().push_back([box = QPointer<QCheckBox>(box), button = QPointer<QPushButton>(button)] {
        if (box && button && button->isChecked() != box->isChecked())
            button->setChecked(box->isChecked());
    });
}

void wrapOffOn(QCheckBox *box)
{
    QLayout *holder = parentLayoutOf(box);
    if (!holder)
        return;
    auto *outer = new QWidget;
    auto *column = new QVBoxLayout(outer);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);
    // The box's text is a setting title like the others (sectionLabel's look).
    auto *label = new QLabel(box->text());
    label->setObjectName(QStringLiteral("sectionLabel"));
    label->setStyleSheet(QStringLiteral("color: %1; margin-top: 6px;").arg(Theme::textMuted().name()));
    label->setWordWrap(true);
    label->setToolTip(box->toolTip());
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    auto *off = new QPushButton(QStringLiteral("Off"));
    auto *on = new QPushButton(QStringLiteral("On"));
    off->setProperty("segment", QStringLiteral("left"));
    on->setProperty("segment", QStringLiteral("right"));
    auto *group = new QButtonGroup(outer);
    for (QPushButton *b : { off, on }) {
        b->setCheckable(true);
        b->setMinimumWidth(58);
        b->setFixedHeight(kButtonHeight);
        b->setToolTip(box->toolTip());
        group->addButton(b);
    }
    group->setExclusive(true);
    delete holder->replaceWidget(box, outer);
    column->addWidget(label);
    row->addWidget(off);
    row->addWidget(on);
    row->addStretch(1);
    // The box stays as the setting (its signals drive everything): there,
    // but taking no space.
    box->setFixedSize(0, 0);
    box->setFocusPolicy(Qt::NoFocus);
    row->addWidget(box);
    column->addLayout(row);
    fixHeight(outer);
    const auto show = [on, off](bool checked) {
        on->setChecked(checked);
        off->setChecked(!checked);
    };
    show(box->isChecked());
    QObject::connect(box, &QCheckBox::toggled, on, show);
    QObject::connect(on, &QPushButton::clicked, box, [box] { box->setChecked(true); });
    QObject::connect(off, &QPushButton::clicked, box, [box] { box->setChecked(false); });
    new Mirror(box, outer, { on, off, label });
    syncs().push_back([box = QPointer<QCheckBox>(box), on = QPointer<QPushButton>(on),
                       off = QPointer<QPushButton>(off)] {
        if (box && on && off && on->isChecked() != box->isChecked()) {
            on->setChecked(box->isChecked());
            off->setChecked(!box->isChecked());
        }
    });
}

} // namespace

// ---- the keypad ----------------------------------------------------------------------

namespace {

struct Unit {
    QString label;
    double  mult;          // base units per one of this unit
};

double prefixValue(const QString &prefix, bool *ok)
{
    *ok = true;
    if (prefix.isEmpty()) return 1.0;
    if (prefix == QLatin1String("k")) return 1e3;
    if (prefix == QLatin1String("M")) return 1e6;
    if (prefix == QLatin1String("m")) return 1e-3;
    if (prefix == QStringLiteral("\u00b5") || prefix == QLatin1String("u")) return 1e-6;
    if (prefix == QLatin1String("n")) return 1e-9;
    *ok = false;
    return 1.0;
}

// The spin box's suffix ("mV", " ms", " %") to its base unit, how many base
// units one of its own is, and the unit buttons to offer.
void unitsFor(const QString &suffix, double *spinMult, QVector<Unit> *units)
{
    const QString s = suffix.trimmed();
    static const QStringList bases = { QStringLiteral("S/s"), QStringLiteral("Hz"), QStringLiteral("V"),
                                       QStringLiteral("A"), QStringLiteral("\u03a9"), QStringLiteral("W"),
                                       QStringLiteral("s") };
    *spinMult = 1.0;
    QString base;
    for (const QString &b : bases) {
        if (!s.endsWith(b))
            continue;
        bool ok = false;
        const double m = prefixValue(s.left(s.size() - b.size()), &ok);
        if (ok) {
            base = b;
            *spinMult = m;
            break;
        }
    }
    const QString mu = QStringLiteral("\u00b5");
    if (base == QLatin1String("V"))
        *units = { { QStringLiteral("kV"), 1e3 }, { QStringLiteral("V"), 1.0 }, { QStringLiteral("mV"), 1e-3 } };
    else if (base == QLatin1String("A"))
        *units = { { QStringLiteral("A"), 1.0 }, { QStringLiteral("mA"), 1e-3 }, { mu + QStringLiteral("A"), 1e-6 } };
    else if (base == QLatin1String("s"))
        *units = { { QStringLiteral("s"), 1.0 }, { QStringLiteral("ms"), 1e-3 }, { mu + QStringLiteral("s"), 1e-6 } };
    else if (base == QLatin1String("Hz"))
        *units = { { QStringLiteral("MHz"), 1e6 }, { QStringLiteral("kHz"), 1e3 }, { QStringLiteral("Hz"), 1.0 } };
    else if (base == QLatin1String("S/s"))
        *units = { { QStringLiteral("MS/s"), 1e6 }, { QStringLiteral("kS/s"), 1e3 }, { QStringLiteral("S/s"), 1.0 } };
    else if (!base.isEmpty())
        *units = { { QStringLiteral("k") + base, 1e3 }, { base, 1.0 }, { QStringLiteral("m") + base, 1e-3 } };
    else if (!s.isEmpty() && s.at(0).isLetter())
        // Any other unit (a custom probe's "bar", "psi"...): k / base / m too.
        *units = { { QStringLiteral("k") + s, 1e3 }, { s, 1.0 }, { QStringLiteral("m") + s, 1e-3 } };
    else
        *units = { { s.isEmpty() ? QString() : s, 1.0 } };   // %, degrees, none: the field's own
}

class NumericPad : public QWidget
{
public:
    explicit NumericPad(QAbstractSpinBox *spin);
    void place();

protected:
    void paintEvent(QPaintEvent *) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    double minimum() const;
    double maximum() const;
    double value() const;
    void   setValue(double spinUnits);
    QString suffix() const;
    void   type(const QString &key);
    void   backspace();
    void   commit();
    void   selectUnit(int index);
    void   refresh();

    QPointer<QAbstractSpinBox> m_spin;
    QLabel  *m_display = nullptr;
    QSlider *m_slider = nullptr;
    QString  m_entry;
    bool     m_negative = false;
    double   m_spinMult = 1.0;
    QVector<Unit> m_units;
    int      m_unit = 0;                 // the unit the entry is in (its button highlighted)
    QList<QToolButton *> m_unitButtons;
    bool     m_arrowDown = true;
    int      m_arrowX = 0;
    static constexpr int kArrow = 9;
};

double NumericPad::minimum() const
{
    if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin.data())) return d->minimum();
    if (auto *i = qobject_cast<QSpinBox *>(m_spin.data())) return i->minimum();
    return 0.0;
}
double NumericPad::maximum() const
{
    if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin.data())) return d->maximum();
    if (auto *i = qobject_cast<QSpinBox *>(m_spin.data())) return i->maximum();
    return 0.0;
}
double NumericPad::value() const
{
    if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin.data())) return d->value();
    if (auto *i = qobject_cast<QSpinBox *>(m_spin.data())) return i->value();
    return 0.0;
}
void NumericPad::setValue(double v)
{
    v = std::clamp(v, minimum(), maximum());
    if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin.data())) d->setValue(v);
    else if (auto *i = qobject_cast<QSpinBox *>(m_spin.data())) i->setValue(int(std::lround(v)));
}
QString NumericPad::suffix() const
{
    if (auto *d = qobject_cast<QDoubleSpinBox *>(m_spin.data())) return d->suffix();
    if (auto *i = qobject_cast<QSpinBox *>(m_spin.data())) return i->suffix();
    return QString();
}

NumericPad::NumericPad(QAbstractSpinBox *spin) : QWidget(nullptr), m_spin(spin)
{
    setWindowFlags(Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_DeleteOnClose);
    QVector<Unit> units;
    unitsFor(suffix(), &m_spinMult, &units);

    const QColor ink = Theme::text();
    QFont big = font();
    big.setPointSizeF(big.pointSizeF() * 1.45);
    const QString keyStyle = QStringLiteral(
        "QToolButton { color: %1; background: transparent; border: none; border-radius: 6px; }"
        "QToolButton:hover { background: %2; }"
        "QToolButton:pressed { background: %3; }"
        "QToolButton[current=\"true\"] { background: %4; color: %5; }"
        "QToolButton[primary=\"true\"] { background: %4; color: %5; font-weight: bold; }")
        .arg(ink.name(), Theme::cardHoverBackground().name(), Theme::cardSelectedBackground().name(),
             Theme::accentHex(), Theme::onAccent().name());
    const auto key = [&](const QString &text, std::function<void()> action) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setFont(big);
        b->setMinimumSize(52, 44);
        b->setFocusPolicy(Qt::NoFocus);
        b->setStyleSheet(keyStyle);
        connect(b, &QToolButton::clicked, this, action);
        return b;
    };

    auto *main = new QHBoxLayout(this);
    main->setSpacing(10);
    auto *left = new QVBoxLayout;
    left->setSpacing(8);
    // The entry, and backspace.
    auto *top = new QHBoxLayout;
    m_display = new QLabel;
    m_display->setFont(big);
    m_display->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_display->setMinimumWidth(190);
    // (its full style is set by refresh())
    top->addWidget(m_display, 1);
    top->addWidget(key(QStringLiteral("\u232b"), [this] { backspace(); }));
    left->addLayout(top);
    // Digits, sign and point; the units on the right confirm, like Enter.
    auto *grid = new QGridLayout;
    grid->setSpacing(4);
    const char *digits[4][3] = { { "7", "8", "9" }, { "4", "5", "6" }, { "1", "2", "3" }, { "\u00b1", "0", "." } };
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 3; ++c) {
            const QString k = QString::fromUtf8(digits[r][c]);
            grid->addWidget(key(k, [this, k] { type(k); }), r, c);
        }
    }
    // Unit buttons pick the entry's unit (they used to confirm and close, so
    // with nothing typed the pad just vanished); the check key or Enter
    // confirms. The field's own unit starts selected.
    m_units = units;
    m_unit = 0;
    for (int u = 0; u < units.size(); ++u) {
        if (std::fabs(units[u].mult - m_spinMult) < 1e-12 * std::max(1.0, units[u].mult))
            m_unit = u;
    }
    for (int u = 0; u < units.size() && u < 3; ++u) {
        if (units[u].label.isEmpty())
            continue;
        QToolButton *b = key(units[u].label, [this, u] { selectUnit(u); });
        b->setMinimumWidth(64);
        grid->addWidget(b, u, 3);
        m_unitButtons << b;
    }
    QToolButton *confirm = key(QStringLiteral("\u2713"), [this] { commit(); });
    confirm->setProperty("primary", true);
    repolish(confirm);
    confirm->setMinimumWidth(64);
    grid->addWidget(confirm, 3, 3);
    selectUnit(m_unit);
    left->addLayout(grid);
    main->addLayout(left);
    // The slider over the whole range, moving the value live.
    auto *right = new QVBoxLayout;
    right->setSpacing(4);
    auto *maxLabel = new QLabel(QString::number(maximum(), 'g', 6) + suffix());
    auto *minLabel = new QLabel(QString::number(minimum(), 'g', 6) + suffix());
    for (QLabel *l : { maxLabel, minLabel }) {
        l->setAlignment(Qt::AlignCenter);
        l->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    }
    m_slider = new QSlider(Qt::Vertical);
    m_slider->setRange(0, 1000);
    m_slider->setFocusPolicy(Qt::NoFocus);
    // A slim rounded track in the sections' grey, filled with the accent from
    // the minimum up to the value, and a round accent handle ringed in the
    // card colour (brighter while hovered / dragged).
    m_slider->setStyleSheet(QStringLiteral(
        "QSlider::groove:vertical { background: %1; width: 6px; border-radius: 3px; }"
        "QSlider::sub-page:vertical { background: %1; border-radius: 3px; }"
        "QSlider::add-page:vertical { background: %2; border-radius: 3px; }"
        "QSlider::handle:vertical { background: %2; border: 3px solid %3; width: 12px; height: 12px;"
        " margin: 0px -6px; border-radius: 9px; }"
        "QSlider::handle:vertical:hover, QSlider::handle:vertical:pressed { background: %4; }")
        .arg(Theme::windowBackground().name(), Theme::accentHex(), Theme::cardBackground().name(),
             Theme::accent().lighter(115).name()));
    m_slider->setMinimumWidth(24);
    const double span = maximum() - minimum();
    m_slider->setValue(span > 0 ? int(std::lround((value() - minimum()) / span * 1000.0)) : 0);
    connect(m_slider, &QSlider::sliderMoved, this, [this](int s) {
        setValue(minimum() + (maximum() - minimum()) * s / 1000.0);
        m_entry.clear();
        m_negative = false;
        refresh();
    });
    right->addWidget(maxLabel);
    right->addWidget(m_slider, 1, Qt::AlignHCenter);
    right->addWidget(minLabel);
    main->addLayout(right);
    refresh();
}

void NumericPad::type(const QString &k)
{
    if (k == QStringLiteral("\u00b1")) {
        m_negative = !m_negative;
    } else if (k == QLatin1String(".")) {
        if (!m_entry.contains(QLatin1Char('.')))
            m_entry += m_entry.isEmpty() ? QStringLiteral("0.") : QStringLiteral(".");
    } else if (m_entry.size() < 12) {
        m_entry += k;
    }
    refresh();
}

void NumericPad::backspace()
{
    if (!m_entry.isEmpty())
        m_entry.chop(1);
    else
        m_negative = false;
    refresh();
}

void NumericPad::selectUnit(int index)
{
    if (index < 0 || index >= m_units.size())
        return;
    m_unit = index;
    for (QToolButton *b : m_unitButtons) {
        const bool current = b->text() == m_units[index].label;
        if (b->property("current").toBool() != current) {
            b->setProperty("current", current);
            repolish(b);
        }
    }
    refresh();
}

// The entry, in the selected unit, into the field (in its own unit).
void NumericPad::commit()
{
    if (!m_entry.isEmpty() && m_unit >= 0 && m_unit < m_units.size()) {
        bool ok = false;
        const double typed = m_entry.toDouble(&ok);
        if (ok)
            setValue((m_negative ? -typed : typed) * m_units[m_unit].mult / m_spinMult);
    }
    close();
}

// The typed entry, or the current value (dimmed) until something is typed.
void NumericPad::refresh()
{
    const bool typed = !m_entry.isEmpty() || m_negative;
    const Unit unit = m_unit >= 0 && m_unit < m_units.size() ? m_units[m_unit] : Unit { QString(), 1.0 };
    const QString unitText = unit.label.isEmpty() ? QString() : QStringLiteral(" ") + unit.label;
    if (!typed)
        m_display->setText(QString::number(value() * m_spinMult / unit.mult, 'g', 6) + unitText);
    else
        m_display->setText((m_negative ? QStringLiteral("\u2212") : QString())
                           + (m_entry.isEmpty() ? QStringLiteral("0") : m_entry) + unitText);
    m_display->setStyleSheet(QStringLiteral(
        "QLabel { border: 1px solid %1; border-radius: 6px; padding: 4px 10px; color: %2; }")
        .arg(Theme::accentHex(), (typed ? Theme::text() : Theme::textMuted()).name()));
}

void NumericPad::keyPressEvent(QKeyEvent *event)
{
    const QString t = event->text();
    if (event->key() == Qt::Key_Escape) {
        close();
    } else if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        commit();                                   // in the selected unit
    } else if (event->key() == Qt::Key_Backspace) {
        backspace();
    } else if (t.size() == 1 && t[0].isDigit()) {
        type(t);
    } else if (t == QLatin1String(".") || t == QLatin1String(",")) {
        type(QStringLiteral("."));
    } else if (t == QLatin1String("-")) {
        type(QStringLiteral("\u00b1"));
    } else {
        QWidget::keyPressEvent(event);
    }
}

// Above the field (arrow down) if it fits, else below; centred on it, kept on screen.
void NumericPad::place()
{
    if (!m_spin)
        return;
    QWidget *anchor = m_spin->parentWidget() && m_spin->parentWidget()->objectName() == QLatin1String("stepField")
                          ? m_spin->parentWidget() : m_spin.data();
    const QRect a(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    const QRect screen = anchor->screen() ? anchor->screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
    layout()->setContentsMargins(12, 12, 12, 12 + kArrow);
    adjustSize();
    const QSize s = size();
    m_arrowDown = a.top() - s.height() >= screen.top();
    layout()->setContentsMargins(12, 12 + (m_arrowDown ? 0 : kArrow), 12, 12 + (m_arrowDown ? kArrow : 0));
    const int x = std::clamp(a.center().x() - s.width() / 2, screen.left(), screen.right() - s.width());
    const int y = m_arrowDown ? a.top() - s.height() : a.bottom() + 1;
    m_arrowX = std::clamp(a.center().x() - x, 20, s.width() - 20);
    move(x, y);
}

void NumericPad::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF body = QRectF(rect()).adjusted(0.5, m_arrowDown ? 0.5 : kArrow + 0.5, -0.5,
                                                m_arrowDown ? -kArrow - 0.5 : -0.5);
    QPainterPath path;
    path.addRoundedRect(body, 8, 8);
    QPainterPath arrow;
    if (m_arrowDown) {
        arrow.moveTo(m_arrowX - kArrow, body.bottom() - 1);
        arrow.lineTo(m_arrowX, body.bottom() + kArrow);
        arrow.lineTo(m_arrowX + kArrow, body.bottom() - 1);
    } else {
        arrow.moveTo(m_arrowX - kArrow, body.top() + 1);
        arrow.lineTo(m_arrowX, body.top() - kArrow);
        arrow.lineTo(m_arrowX + kArrow, body.top() + 1);
    }
    arrow.closeSubpath();
    path = path.united(arrow);
    p.setPen(QPen(Theme::accent(), 1.0));          // the accent edge, like the panel's controls
    p.setBrush(Theme::cardBackground());
    p.drawPath(path);
}

} // namespace

void openNumericPad(QAbstractSpinBox *spin)
{
    if (!spin)
        return;
    auto *pad = new NumericPad(spin);
    pad->place();
    pad->show();
    pad->setFocus();
}

// ---- the pass ------------------------------------------------------------------------

void adopt(QWidget *root, QObject *groupOwner)
{
    if (!root)
        return;
    // One text size for every widget on the pages, 90 % of the body text, set
    // by style sheet. Fonts set in code don't hold here: the global sheet's
    // "* { font-family }" makes Qt rebuild each widget's font from the
    // application's, so a smaller panel font was dropped (number fields
    // stayed at 100 %) while combos got their size from a sheet (110 %
    // globally, 90 % from the earlier per-combo sheet) — "x1" vs "0.00 V".
    // A sheet on the pages beats both; bold stays bold (only the size is set).
    root->setStyleSheet(root->styleSheet()
                        + QStringLiteral(" * { font-size: %1pt; }")
                              .arg(QApplication::font().pointSizeF() * kSmallerText, 0, 'f', 2));
    // Exclusive button groups: a row becomes joined segments, a grid flat buttons.
    if (groupOwner) {
        for (QButtonGroup *group : groupOwner->findChildren<QButtonGroup *>()) {
            const QList<QAbstractButton *> buttons = group->buttons();
            if (buttons.size() < 2 || !group->exclusive())
                continue;
            bool mine = true;
            for (QAbstractButton *b : buttons)
                mine = mine && qobject_cast<QPushButton *>(b) && root->isAncestorOf(b);
            if (!mine)
                continue;
            QLayout *holder = parentLayoutOf(buttons.first());
            bool together = holder != nullptr;
            for (QAbstractButton *b : buttons)
                together = together && holder->indexOf(b) >= 0;
            if (!together)
                continue;
            if (auto *box = qobject_cast<QBoxLayout *>(holder);
                box && (box->direction() == QBoxLayout::LeftToRight || box->direction() == QBoxLayout::RightToLeft)) {
                QList<QAbstractButton *> ordered = buttons;
                std::sort(ordered.begin(), ordered.end(), [holder](QAbstractButton *a, QAbstractButton *b) {
                    return holder->indexOf(a) < holder->indexOf(b);
                });
                new SegmentRoles(ordered, group);   // end caps among the shown ones
                box->setSpacing(0);
                // The whole width (Acquisition was held compact by its row's
                // stretch; Keep already spanned it): no stretch, buttons expand.
                for (int i = box->count() - 1; i >= 0; --i) {
                    if (box->itemAt(i)->spacerItem())
                        delete box->takeAt(i);
                }
                for (QAbstractButton *b : ordered)
                    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            } else if (qobject_cast<QGridLayout *>(holder)) {
                for (QAbstractButton *b : buttons) {
                    b->setProperty("flatChoice", true);
                    repolish(b);
                }
            }
        }
    }
    // Check boxes: in a grid, toggle buttons; on their own, Off | On. All
    // classified before any is converted: converting as it went emptied the
    // grid of check boxes, so its last one (-Width) found no siblings left
    // and became an Off | On pair.
    QList<QCheckBox *> toggles, switches;
    QList<QGridLayout *> toggleGrids;
    for (QCheckBox *box : root->findChildren<QCheckBox *>()) {
        if (box->text().isEmpty() || box->isTristate())
            continue;
        QLayout *holder = parentLayoutOf(box);
        if (!holder)
            continue;
        int siblings = 0;
        auto *grid = qobject_cast<QGridLayout *>(holder);
        if (grid) {
            for (int i = 0; i < holder->count(); ++i)
                siblings += qobject_cast<QCheckBox *>(holder->itemAt(i)->widget()) ? 1 : 0;
        }
        if (siblings >= 2) {
            toggles << box;
            if (!toggleGrids.contains(grid))
                toggleGrids << grid;
        } else {
            switches << box;
        }
    }
    for (QCheckBox *box : toggles)
        wrapToggle(box);
    for (QCheckBox *box : switches)
        wrapOffOn(box);
    for (QGridLayout *grid : toggleGrids) {          // equal columns: the buttons line up
        for (int c = 0; c < grid->columnCount(); ++c)
            grid->setColumnStretch(c, 1);
    }
    // Dropdowns marked asSegments (coupling): joined segments. The rest, and
    // number fields: compact-card fields.
    for (QComboBox *combo : root->findChildren<QComboBox *>()) {
        if (combo->property("asSegments").toBool())
            segmentCombo(combo);
        else
            wrapStepper(combo, [combo] { stepCombo(combo, -1); }, [combo] { stepCombo(combo, +1); }, false);
    }
    for (QAbstractSpinBox *spin : root->findChildren<QAbstractSpinBox *>()) {
        if (!qobject_cast<QSpinBox *>(spin) && !qobject_cast<QDoubleSpinBox *>(spin))
            continue;
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFrame(false);
        spin->setAlignment(Qt::AlignCenter);
        wrapStepper(spin, [spin] { spin->stepBy(-1); }, [spin] { spin->stepBy(1); }, true);
        auto *opener = new PadOpener(spin);
        spin->installEventFilter(opener);
        if (QLineEdit *edit = spin->findChild<QLineEdit *>())
            edit->installEventFilter(opener);
    }
    for (QPushButton *b : root->findChildren<QPushButton *>()) {
        b->setFixedHeight(kButtonHeight);
        b->setIconSize(QSize(14, 14));
    }
    // Text boxes (not the ones inside number fields): field-sized.
    for (QLineEdit *edit : root->findChildren<QLineEdit *>()) {
        if (qobject_cast<QAbstractSpinBox *>(edit->parentWidget()) || qobject_cast<QComboBox *>(edit->parentWidget()))
            continue;
        edit->setFixedSize(kFieldWidth, kFieldHeight);
    }
    startSyncing(root);
    // Spare room at the bottom of each page, not between its rows.
    QList<QWidget *> pages;
    if (auto *stack = qobject_cast<QStackedWidget *>(root)) {
        for (int i = 0; i < stack->count(); ++i)
            pages << stack->widget(i);
    }
    for (QScrollArea *area : root->findChildren<QScrollArea *>()) {
        if (area->widget())
            pages << area->widget();
    }
    for (QWidget *page : pages) {
        auto *column = qobject_cast<QBoxLayout *>(page->layout());
        if (!column || (column->direction() != QBoxLayout::TopToBottom))
            continue;
        const int last = column->count() - 1;
        if (last < 0 || !column->itemAt(last)->spacerItem())
            column->addStretch(1);
    }
}

void restyle(QWidget *root)
{
    // (heights are fixed in adopt(); nothing here depends on the theme but the icons)
    if (!root)
        return;
    for (QFrame *field : root->findChildren<QFrame *>(QStringLiteral("stepField"))) {
        auto *minus = field->findChild<QToolButton *>(QStringLiteral("stepMinus"), Qt::FindDirectChildrenOnly);
        auto *plus = field->findChild<QToolButton *>(QStringLiteral("stepPlus"), Qt::FindDirectChildrenOnly);
        if (minus && plus)
            setBarIcons(minus, plus);
    }
}

} // namespace PanelControls
