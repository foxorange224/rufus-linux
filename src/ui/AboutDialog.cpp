#include "AboutDialog.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QApplication>
#include <QPixmap>

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("About Rufus"));

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(8);

    // Program icon (from the rufus-*.png set embedded in the resources)
    auto *iconLabel = new QLabel;
    QPixmap iconPix(QStringLiteral(":/icons/icons/rufus-256.png"));
    if (!iconPix.isNull())
        iconLabel->setPixmap(iconPix.scaled(72, 72, Qt::KeepAspectRatio,
            Qt::SmoothTransformation));
    iconLabel->setAlignment(Qt::AlignCenter);
    layout->addWidget(iconLabel);

    auto *title = new QLabel(QStringLiteral("<h2>Rufus</h2>"));
    title->setAlignment(Qt::AlignCenter);
    layout->addWidget(title);

    auto *version = new QLabel(tr("Version %1 (Linux Port)")
        .arg(QApplication::applicationVersion()));
    version->setAlignment(Qt::AlignCenter);
    layout->addWidget(version);

    layout->addSpacing(10);

    auto *desc = new QLabel(tr(
        "Create bootable USB drives from ISO/IMG files.\n\n"
        "Originally created by Pete Batard (Akeo)\n"
        "GNU GPL v3 License\n\n"
        "Linux port using Qt6 and C++"));
    desc->setWordWrap(true);
    desc->setMinimumWidth(380);
    desc->setAlignment(Qt::AlignCenter);
    layout->addWidget(desc);

    auto *linkPort = new QLabel(
        QStringLiteral("<p><a href=\"https://www.github.com/foxorange224/rufus-linux\">%1</a></p>")
        .arg(tr("Linux port on GitHub")));
    linkPort->setWordWrap(true);
    linkPort->setAlignment(Qt::AlignCenter);
    linkPort->setOpenExternalLinks(true);
    layout->addWidget(linkPort);

    auto *linkOriginal = new QLabel(
        QStringLiteral("<p><a href=\"https://github.com/pbatard/rufus\">%1</a></p>")
        .arg(tr("Original Rufus (Windows) on GitHub")));
    linkOriginal->setWordWrap(true);
    linkOriginal->setAlignment(Qt::AlignCenter);
    linkOriginal->setOpenExternalLinks(true);
    layout->addWidget(linkOriginal);

    layout->addStretch();

    auto *closeBtn = new QPushButton(tr("Close"));
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    layout->addWidget(closeBtn);

    // Size the dialog to its content (never clips the description or the
    // links, even with larger fonts or after a language change).
    layout->setSizeConstraint(QLayout::SetFixedSize);
}
