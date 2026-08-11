#pragma once

#include <QMessageBox>

namespace MsgBox {

// Wrappers around the QMessageBox static functions that force a non-native
// dialog (Qt6's native/portal dialog crashes when dismissed via the window
// close button on some platforms).
QMessageBox::StandardButton warning(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton question(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons = QMessageBox::Yes | QMessageBox::No,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton information(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);
QMessageBox::StandardButton critical(QWidget *parent, const QString &title,
    const QString &text, QMessageBox::StandardButtons buttons = QMessageBox::Ok,
    QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

}
