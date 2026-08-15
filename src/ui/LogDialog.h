#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QTimer>

class LogDialog : public QWidget {
    Q_OBJECT
public:
    explicit LogDialog(QWidget *parent = nullptr);

    void setDetached(bool detached);
    bool isDetached() const { return m_detached; }

signals:
    // Emitted when the user clicks the detach button (toggles whether the
    // log window is glued to the right edge of the main window).
    void detachClicked();

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onClear();
    void onSave();
    void onRefresh();
    void updateLogColors();

private:
    QString buildHeader();
    static QString detectDistroString();

    QTextEdit *m_textEdit = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QToolButton *m_detachBtn = nullptr;
    QTimer *m_refreshTimer = nullptr;
    QString m_headerText;
    int m_lastLogCount = 0;
    bool m_detached = false;
};
