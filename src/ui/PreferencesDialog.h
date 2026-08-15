#ifndef PREFERENCESDIALOG_H
#define PREFERENCESDIALOG_H

#include <QDialog>

class QComboBox;

// Preferences window: Qt style (theme) selection with Apply/Cancel.
class PreferencesDialog : public QDialog {
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget *parent = nullptr);

private slots:
    void apply();

private:
    QComboBox *m_styleCombo = nullptr;
};

#endif // PREFERENCESDIALOG_H