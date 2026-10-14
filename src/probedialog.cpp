#include "probedialog.h"

#include "icons.h"
#include "theme.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QColor>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {

// The same preset units PicoScope offers, minus the ones that make no sense
// for a linear scaling of a voltage.
const char *const kPresetUnits[] = {
    "V", "A", "W", "VA", "VAr", "s", "Hz", "dB", "dBm", "dBu", "dBV",
    "\u00b0", "rads", "\u03a9", "baud", "%", "hPa", "bar", "psi", "lpm",
    "RPM", "\u00b0C", "\u00b0F", "Pa", "m/s\u00b2", "S", "B", "S/s"
};

QString formatScaled(double volts, double gradient, double offset, const QString &unit)
{
    const double v = volts * gradient + offset;
    const double a = std::fabs(v);
    if (a >= 1e6)  return QStringLiteral("%1 M%2").arg(v / 1e6, 0, 'g', 4).arg(unit);
    if (a >= 1e3)  return QStringLiteral("%1 k%2").arg(v / 1e3, 0, 'g', 4).arg(unit);
    if (a >= 1.0 || a == 0.0) return QStringLiteral("%1 %2").arg(v, 0, 'g', 4).arg(unit);
    if (a >= 1e-3) return QStringLiteral("%1 m%2").arg(v * 1e3, 0, 'g', 4).arg(unit);
    return QStringLiteral("%1 u%2").arg(v * 1e6, 0, 'g', 4).arg(unit);
}

const double kExampleInputs[] = { -10.0, -1.0, -0.1, 0.0, 0.1, 1.0, 10.0 };


// Checked state on a Fusion push button is nearly invisible against the
// palette, and the focus ring on the first button reads as a selection. Make
// the checked unit unmistakable and take the buttons out of the focus chain.
// A function, not a file-scope const: this dialog is built fresh every time
// it opens, so this only needs to read Theme:: at that moment — same
// reasoning as gridButtonStyle()/toolButtonQss() in mainwindow.cpp, minus
// needing any live-toggle re-apply, since there is no persistent instance to
// retroactively restyle.
QString presetStyle()
{
    return QStringLiteral(
        "QPushButton { background: %1; border: 1px solid %2;"
        "              border-radius: 5px; padding: 3px; color: %3; }"
        "QPushButton:hover { background: %4; }"
        "QPushButton:checked { background: %5; border: 1px solid %6;"
        "                      color: %7; font-weight: bold; }")
        // The dialogs' colours (the surface / tint colours read as dark blue):
        // resting on the sections' grey with an accent edge, the chosen unit
        // filled with the accent like a selected segment.
        .arg(Theme::windowBackground().name(), Theme::accentHex(), Theme::text().name(),
             (Theme::isDark() ? QColor(0x23, 0x23, 0x23) : QColor(0xdb, 0xe0, 0xe5)).name(),
             Theme::accentHex(), Theme::accentHex(), Theme::onAccent().name());
}

}   // namespace

// ---------------------------------------------------------------------------
// Wizard
// ---------------------------------------------------------------------------

ProbeWizard::ProbeWizard(QWidget *parent)
    : VsxDialog(parent, QStringLiteral("Add custom probe"))   // the app's own frame and title bar
{
    setMinimumWidth(420);
    QVBoxLayout *body = contentLayout();

    m_stepLabel = new QLabel;
    m_stepLabel->setAlignment(Qt::AlignCenter);
    QFont f = m_stepLabel->font();
    f.setBold(true);
    m_stepLabel->setFont(f);
    body->addWidget(m_stepLabel);

    m_pages = new QStackedWidget;
    m_pages->addWidget(buildDetailsPage());
    m_pages->addWidget(buildScalingPage());
    body->addWidget(m_pages, 1);

    auto *buttons = new QHBoxLayout;
    auto *cancel = new QPushButton(QStringLiteral("Cancel"));
    m_back = new QPushButton(QStringLiteral("Back"));
    m_next = new QPushButton(QStringLiteral("Next"));
    m_next->setProperty("primary", true);           // filled: the way forward
    buttons->addWidget(cancel);
    buttons->addStretch();
    buttons->addWidget(m_back);
    buttons->addWidget(m_next);
    body->addLayout(buttons);

    connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_back, &QPushButton::clicked, this, [this] { showPage(0); });
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (m_pages->currentIndex() == 0) {
            if (m_name->text().trimmed().isEmpty()) {
                VsxDialogs::warning(this, QStringLiteral("Probe name"),
                                     QStringLiteral("Give the probe a name first."));
                return;
            }
            showPage(1);
        } else {
            if (qFuzzyIsNull(m_gradient->value())) {
                VsxDialogs::warning(this, QStringLiteral("Scaling"),
                    QStringLiteral("A gradient of zero would flatten every signal."));
                return;
            }
            accept();
        }
    });

    syncPresetSelection();
    showPage(0);
}

QWidget *ProbeWizard::buildDetailsPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(new QLabel(QStringLiteral("Probe name")));
    m_name = new QLineEdit;
    layout->addWidget(m_name);

    layout->addSpacing(6);
    layout->addWidget(new QLabel(QStringLiteral("Pre-set units")));

    auto *grid = new QGridLayout;
    grid->setSpacing(3);
    m_presets = new QButtonGroup(this);
    m_presets->setExclusive(true);

    const int columns = 6;
    int index = 0;
    for (const char *unit : kPresetUnits) {
        auto *button = new QPushButton(QString::fromUtf8(unit));
        button->setMaximumWidth(56);
        button->setCheckable(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setStyleSheet(presetStyle());
        m_presets->addButton(button, index);
        connect(button, &QPushButton::clicked, this, [this, unit] {
            m_unit->setText(QString::fromUtf8(unit));
            refreshExample();
        });
        grid->addWidget(button, index / columns, index % columns);
        ++index;
    }
    layout->addLayout(grid);

    layout->addSpacing(6);
    layout->addWidget(new QLabel(QStringLiteral("Custom unit")));
    m_unit = new QLineEdit(QStringLiteral("V"));
    connect(m_unit, &QLineEdit::textChanged, this, [this] {
        syncPresetSelection();
        refreshExample();
    });
    layout->addWidget(m_unit);

    layout->addStretch();
    return page;
}

QWidget *ProbeWizard::buildScalingPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);

    layout->addWidget(new QLabel(
        QStringLiteral("Linear scaling applied to the measured voltage (y = mx + c)")));

    auto *row = new QHBoxLayout;
    row->addWidget(new QLabel(QStringLiteral("y =")));
    m_gradient = new QDoubleSpinBox;
    m_gradient->setRange(-1e6, 1e6);
    m_gradient->setDecimals(4);
    m_gradient->setValue(1.0);
    row->addWidget(m_gradient);
    row->addWidget(new QLabel(QStringLiteral("x  +")));
    m_offset = new QDoubleSpinBox;
    m_offset->setRange(-1e6, 1e6);
    m_offset->setDecimals(4);
    row->addWidget(m_offset);
    layout->addLayout(row);

    auto *hint = new QLabel(QStringLiteral("Gradient (m)                Offset (c)"));
    hint->setStyleSheet(QStringLiteral("color: %1;").arg(Theme::textMuted().name()));
    layout->addWidget(hint);

    connect(m_gradient, &QDoubleSpinBox::valueChanged, this, [this] { refreshExample(); });
    connect(m_offset,   &QDoubleSpinBox::valueChanged, this, [this] { refreshExample(); });

    layout->addSpacing(6);
    layout->addWidget(new QLabel(QStringLiteral("Example")));

    m_example = new QTableWidget(int(sizeof(kExampleInputs) / sizeof(kExampleInputs[0])), 2);
    m_example->setHorizontalHeaderLabels({ QStringLiteral("Input (x)"),
                                           QStringLiteral("Scaled (y)") });
    m_example->verticalHeader()->setVisible(false);
    m_example->horizontalHeader()->setStretchLastSection(true);
    m_example->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_example->setSelectionMode(QAbstractItemView::NoSelection);
    layout->addWidget(m_example, 1);

    return page;
}

void ProbeWizard::showPage(int page)
{
    m_pages->setCurrentIndex(page);
    m_stepLabel->setText(page == 0 ? QStringLiteral("1.  Details")
                                   : QStringLiteral("2.  Scaling"));
    m_back->setEnabled(page > 0);
    m_next->setText(page == 0 ? QStringLiteral("Next") : QStringLiteral("Finish"));
    if (page == 1)
        refreshExample();
}

// Highlights whichever preset matches the unit currently in the field, so
// editing an existing probe shows its unit selected instead of defaulting to
// whatever was checked first.
void ProbeWizard::syncPresetSelection()
{
    if (!m_presets)
        return;
    const QString current = m_unit->text();

    m_presets->setExclusive(false);
    for (QAbstractButton *b : m_presets->buttons())
        b->setChecked(b->text() == current);
    m_presets->setExclusive(true);
}

void ProbeWizard::refreshExample()
{
    if (!m_example)
        return;

    const QString unit = m_unit->text().isEmpty() ? QStringLiteral("V") : m_unit->text();
    int row = 0;
    for (double input : kExampleInputs) {
        m_example->setItem(row, 0, new QTableWidgetItem(
            formatScaled(input, 1.0, 0.0, QStringLiteral("V"))));
        m_example->setItem(row, 1, new QTableWidgetItem(
            formatScaled(input, m_gradient->value(), m_offset->value(), unit)));
        ++row;
    }
}

void ProbeWizard::setProbe(const Probe &probe)
{
    m_name->setText(probe.name);
    m_unit->setText(probe.unit);
    m_gradient->setValue(probe.gradient);
    m_offset->setValue(probe.offset);
    setWindowTitle(QStringLiteral("Edit probe"));
    syncPresetSelection();
    refreshExample();
}

Probe ProbeWizard::probe() const
{
    Probe p;
    p.name     = m_name->text().trimmed();
    p.unit     = m_unit->text().isEmpty() ? QStringLiteral("V") : m_unit->text();
    p.gradient = m_gradient->value();
    p.offset   = m_offset->value();
    p.builtin  = false;
    return p;
}

// ---------------------------------------------------------------------------
// Manager
// ---------------------------------------------------------------------------

ProbeManagerDialog::ProbeManagerDialog(ProbeLibrary *library, QWidget *parent)
    : VsxDialog(parent, QStringLiteral("Manage probes")), m_library(library)
{
    setMinimumSize(360, 320);
    QVBoxLayout *body = contentLayout();

    m_list = new QListWidget;
    body->addWidget(m_list, 1);

    auto *row = new QHBoxLayout;
    auto *add = new QPushButton(QStringLiteral("Add"));
    m_edit    = new QPushButton(QStringLiteral("Edit"));
    m_delete  = new QPushButton(QStringLiteral("Delete"));

    const QColor iconColour = Theme::text();
    add->setIcon(Icons::icon(QStringLiteral("plus"), iconColour, 16));
    m_edit->setIcon(Icons::icon(QStringLiteral("pen"), iconColour, 16));
    m_delete->setIcon(Icons::icon(QStringLiteral("trash"), iconColour, 16));
    row->addWidget(add);
    row->addWidget(m_edit);
    row->addWidget(m_delete);
    row->addStretch();
    body->addLayout(row);

    addButton(QStringLiteral("Close"), true, [this] { accept(); });

    connect(add,      &QPushButton::clicked, this, &ProbeManagerDialog::addProbe);
    connect(m_edit,   &QPushButton::clicked, this, &ProbeManagerDialog::editProbe);
    connect(m_delete, &QPushButton::clicked, this, &ProbeManagerDialog::deleteProbe);
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        const bool editable = row >= 0 && row < m_library->count()
                              && !m_library->at(row).builtin;
        m_edit->setEnabled(editable);
        m_delete->setEnabled(editable);
    });

    refresh();
}

void ProbeManagerDialog::refresh()
{
    const int row = m_list->currentRow();
    m_list->clear();
    for (const Probe &p : m_library->probes()) {
        auto *item = new QListWidgetItem(
            QStringLiteral("%1    %2").arg(p.name, p.summary()));
        if (p.builtin)
            item->setForeground(Theme::textMuted());
        m_list->addItem(item);
    }
    m_list->setCurrentRow(std::min(row, m_list->count() - 1));
    m_edit->setEnabled(false);
    m_delete->setEnabled(false);
}

void ProbeManagerDialog::addProbe()
{
    ProbeWizard wizard(this);
    if (wizard.exec() != QDialog::Accepted)
        return;

    Probe p = wizard.probe();
    if (m_library->indexOfName(p.name) >= 0) {
        VsxDialogs::warning(this, QStringLiteral("Probe name"),
                             QStringLiteral("A probe called \"%1\" already exists.")
                                 .arg(p.name));
        return;
    }
    m_library->add(p);
    refresh();
}

void ProbeManagerDialog::editProbe()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_library->count() || m_library->at(row).builtin)
        return;

    ProbeWizard wizard(this);
    wizard.setProbe(m_library->at(row));
    if (wizard.exec() != QDialog::Accepted)
        return;

    m_library->update(row, wizard.probe());
    refresh();
}

void ProbeManagerDialog::deleteProbe()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_library->count() || m_library->at(row).builtin)
        return;

    const QString name = m_library->at(row).name;
    if (!VsxDialogs::question(this, QStringLiteral("Delete probe"),
            QStringLiteral("Delete \"%1\"?").arg(name)))
        return;

    m_library->remove(row);
    refresh();
}
