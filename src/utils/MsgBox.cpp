#include "MsgBox.h"

namespace MsgBox {

static QMessageBox::StandardButton show(QMessageBox::Icon icon, QWidget *parent,
    const QString &title, const QString &text, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    QMessageBox box(icon, title, text, buttons, parent);
    box.setDefaultButton(defaultButton);
#if QT_VERSION >= QT_VERSION_CHECK(6, 11, 0)
    // Not available before Qt 6.11.
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
#endif
    // exec() returns 0 when the dialog is dismissed via the window close
    // button; map that to NoButton so callers never see a stale value.
    return static_cast<QMessageBox::StandardButton>(box.exec());
}

QMessageBox::StandardButton warning(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    return show(QMessageBox::Warning, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton question(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    return show(QMessageBox::Question, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton information(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    return show(QMessageBox::Information, parent, title, text, buttons, defaultButton);
}

QMessageBox::StandardButton critical(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons,
    QMessageBox::StandardButton defaultButton)
{
    return show(QMessageBox::Critical, parent, title, text, buttons, defaultButton);
}

}
