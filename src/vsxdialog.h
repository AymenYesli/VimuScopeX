#pragma once
//
// The app's own dialogs, drawn like the rest of it (PicoScope-style) instead
// of the system's: no window frame, an accent title bar with the title
// centred and a close button (drag it to move the dialog), a rounded card
// body, buttons and fields in the panel's look. File open / save stay native
// on purpose (the desktop's places, recent files, search).
//
#include <QColor>
#include <QDialog>
#include <QPoint>
#include <QString>

#include <functional>

class QBoxLayout;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QVBoxLayout;

class VsxDialog : public QDialog
{
public:
    explicit VsxDialog(QWidget *parent = nullptr, const QString &title = QString());

    // Where the content goes (inside the card, with its margins).
    QVBoxLayout *contentLayout() const { return m_content; }

    // A button at the bottom right; primary ones are filled with the accent.
    QPushButton *addButton(const QString &text, bool primary, std::function<void()> onClick);

protected:
    void paintEvent(QPaintEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void restyle();

    QWidget     *m_titleBar = nullptr;
    QLabel      *m_title = nullptr;
    QVBoxLayout *m_content = nullptr;
    QHBoxLayout *m_buttons = nullptr;
    QPoint       m_dragOffset;
    bool         m_dragging = false;
};

namespace VsxDialogs {

// Yes / no; true for yes.
bool question(QWidget *parent, const QString &title, const QString &text,
              const QString &yes = QStringLiteral("Yes"), const QString &no = QStringLiteral("No"));

void warning(QWidget *parent, const QString &title, const QString &text);

// The app's colour picker (swatches + hex); invalid when cancelled.
QColor pickColour(const QColor &initial, QWidget *parent, const QString &title);

} // namespace VsxDialogs
