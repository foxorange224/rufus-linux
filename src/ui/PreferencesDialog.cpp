#include "PreferencesDialog.h"
#include "utils/Settings.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QStyleFactory>
#include <QVBoxLayout>

PreferencesDialog::PreferencesDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Preferences"));
    setModal(true);

    auto *layout = new QVBoxLayout(this);

    auto *group = new QGroupBox(tr("Preferences"), this);
    auto *groupLayout = new QVBoxLayout(group);

    groupLayout->addWidget(new QLabel(tr("Interface theme:"), group));

    m_styleCombo = new QComboBox(group);
    // Fusion is the default: it is predictable and avoids the platform
    // theme quirks, so it is listed first.
    m_styleCombo->addItem(QStringLiteral("Fusion"), QStringLiteral("fusion"));
    const QStringList styles = QStyleFactory::keys();
    for (const QString &name : styles) {
        if (name.compare(QStringLiteral("Fusion"), Qt::CaseInsensitive) == 0)
            continue; // already added first
        // Qt style names come back case-insensitively unique (e.g.
        // "Windows"); normalize to lowercase for the stored preference.
        m_styleCombo->addItem(name, name.toLower());
    }
    const QString saved = Settings::instance().style();
    int idx = m_styleCombo->findData(saved);
    m_styleCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    groupLayout->addWidget(m_styleCombo);

    layout->addWidget(group);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply
                                         | QDialogButtonBox::Cancel, this);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
            this, &PreferencesDialog::apply);
    connect(buttons->button(QDialogButtonBox::Cancel), &QPushButton::clicked,
            this, &QDialog::reject);
    layout->addWidget(buttons);

    layout->setSizeConstraint(QLayout::SetFixedSize);
}

void PreferencesDialog::apply() {
    const QString styleName = m_styleCombo->currentData().toString();
    QStyle *style = QStyleFactory::create(styleName);
    if (style)
        QApplication::setStyle(style);
    Settings::instance().setStyle(styleName);
    Settings::instance().sync();
}