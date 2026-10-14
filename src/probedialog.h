#pragma once

#include "vsxdialog.h"
#include "probes.h"

#include <QDialog>

class QButtonGroup;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QStackedWidget;
class QTableWidget;

/*
 * Two-page wizard, following PicoScope's Add Custom Probe: details first
 * (name and unit), then the linear scaling with a worked example table.
 */
class ProbeWizard : public VsxDialog {
    Q_OBJECT

public:
    explicit ProbeWizard(QWidget *parent = nullptr);

    void setProbe(const Probe &probe);
    Probe probe() const;

private:
    QWidget *buildDetailsPage();
    QWidget *buildScalingPage();
    void showPage(int page);
    void refreshExample();
    void syncPresetSelection();

    QStackedWidget *m_pages     = nullptr;
    QLabel         *m_stepLabel = nullptr;
    QLineEdit      *m_name      = nullptr;
    QLineEdit      *m_unit      = nullptr;
    QButtonGroup   *m_presets   = nullptr;
    QDoubleSpinBox *m_gradient  = nullptr;
    QDoubleSpinBox *m_offset    = nullptr;
    QTableWidget   *m_example   = nullptr;
    QPushButton    *m_back      = nullptr;
    QPushButton    *m_next      = nullptr;
};

/*
 * The list of probes, with add / edit / delete. Built-ins cannot be changed.
 */
class ProbeManagerDialog : public VsxDialog {
    Q_OBJECT

public:
    ProbeManagerDialog(ProbeLibrary *library, QWidget *parent = nullptr);

private:
    void refresh();
    void addProbe();
    void editProbe();
    void deleteProbe();

    ProbeLibrary *m_library = nullptr;
    QListWidget  *m_list    = nullptr;
    QPushButton  *m_edit    = nullptr;
    QPushButton  *m_delete  = nullptr;
};
