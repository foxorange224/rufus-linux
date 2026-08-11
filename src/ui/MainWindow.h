#pragma once

#include <QMainWindow>
#include <QComboBox>
#include <QPushButton>
#include <QLineEdit>
#include <QLabel>
#include <QCheckBox>
#include <QSpinBox>
#include <QSlider>
#include <QProgressBar>
#include <QTimer>
#include <QThread>
#include <QSettings>
#include <QActionGroup>
#include <QMenu>
#include <QToolBar>
#include <QToolButton>
#include <QMessageBox>
#include <QKeyEvent>
#include <QEvent>
#include <QElapsedTimer>
#include <QStatusBar>
#include <QHash>
#include <QVariant>
#include <QFutureWatcher>

#include "backend/DeviceManager.h"
#include "backend/PartitionManager.h"
#include "core/ImageHandler.h"
#include "worker/FormatWorker.h"

class LogDialog;
class QVBoxLayout;
class QHBoxLayout;
class QFrame;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

protected:
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void onStartStop();
    void onSelectImage();
    void onRefreshDevices();
    void onDeviceChanged(int index);
    void onImageChanged(const QString &path);
    void onFormatFinished(bool success, const QString &message);
    void onProgressChanged(int percent);
    void onDeviceProgress(qint64 current, qint64 total);
    void onLogMessage(const QString &message, int type);
    void onOpenLog();
    void onLogDetachClicked();
    void onLanguageChanged(QAction *action);
    void onAdvancedFormatToggled();
    void onAdvancedDriveToggled();
    void onAutoRefresh();
    void onSchemeChanged(int index);
    void onTargetSystemChanged(int index);
    void onFsChanged(int index);
    void onBootSelectionChanged(int index);
    void onBootloaderChanged();
    void onImageDetectFinished();
    void onHashFinished();

private:
    void setupUi();
    void setupConnections();
    void populateDevices();
    void setControlsEnabled(bool enabled);
    void setFormatNotApplicable(bool notApplicable);
    void updateContextualStates();
    void updateIdleProgressBarText();
    void updateRecommendedSettings(const QString &imagePath);
    void updateAdvancedFromImage(const ImageInfo &info);
    void loadSettings();
    void saveSettings();
    void retranslateUi();
    void setBootloaderFromImage(const ImageInfo &info);
    void setPartitionSchemeFromImage(const ImageInfo &info);
    void rebuildBootSelectionCombo();
    bool runBootChecks();
    void populateFsCombo(BootType bootType);
    void populateClusterSizes(FileSystem fs);
    void populateSelectMenu();
    void updateDeviceCountStatus();
    void updateElapsedLabel();
    void updateFixedSize();
    void updateLogPanelSize();
    void updatePartitionSchemeForBootType(BootType bt);
    void updateTargetSystemForScheme();
    void applyImageInfo(const QString &path);
    void updateBootloaderItemState();
    bool isMsdosBootloader() const {
        return m_bootloaderCombo &&
               m_bootloaderCombo->currentData().toString() == QStringLiteral("msdos");
    }

    static QString formatSize(qint64 bytes);

    // Drive Properties
    QComboBox *m_deviceCombo = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QComboBox *m_bootSelectionCombo = nullptr;
    QPushButton *m_saveBtn = nullptr;
    QPushButton *m_hashBtn = nullptr;
    QToolButton *m_selectBtn = nullptr;
    QMenu *m_selectMenu = nullptr;

    // Image option + persistence row
    QLabel *m_imageOptionLabel = nullptr;
    QComboBox *m_imageOptionCombo = nullptr;
    QSlider *m_persistenceSlider = nullptr;
    QSpinBox *m_persistenceSizeSpin = nullptr;
    QComboBox *m_persistenceUnitsCombo = nullptr;

    // Partition scheme + target system
    QComboBox *m_schemeCombo = nullptr;
    QComboBox *m_targetSystemCombo = nullptr;
    QLabel *m_csmHelpLabel = nullptr;

    // Drive advanced section
    QPushButton *m_advancedDriveToggle = nullptr;
    QCheckBox *m_listUsbHddCheck = nullptr;
    QCheckBox *m_oldBiosFixCheck = nullptr;
    QCheckBox *m_uefiMediaCheck = nullptr;

    // Format Options
    QComboBox *m_fsCombo = nullptr;
    QComboBox *m_clusterSizeCombo = nullptr;
    QLabel *m_clusterSizeLabel = nullptr;
    QLineEdit *m_labelEdit = nullptr;
    QLabel *m_partitionSchemeLabel = nullptr;
    QLabel *m_fileSystemLabel = nullptr;


    QLabel *m_volumeLabelLabel = nullptr;
    QLabel *m_bootSelectionLabel = nullptr;
    QLabel *m_bootloaderLabel = nullptr;

    // Format advanced section
    QPushButton *m_advancedFormatToggle = nullptr;
    QCheckBox *m_quickFormatCheck = nullptr;
    QCheckBox *m_extendedLabelCheck = nullptr;
    QCheckBox *m_badBlocksCheck = nullptr;
    QComboBox *m_nbPassesCombo = nullptr;
    QCheckBox *m_persistentCheck = nullptr;
    QCheckBox *m_verifyWriteCheck = nullptr;
    QCheckBox *m_espCheck = nullptr;
    QCheckBox *m_uefiNtfsCheck = nullptr;
    QComboBox *m_bootloaderCombo = nullptr;

    // Section headers
    QLabel *m_driveHeader = nullptr;
    QLabel *m_formatHeader = nullptr;
    QLabel *m_statusHeader = nullptr;

    // Progress
    QProgressBar *m_progressBar = nullptr;

    // Status bar: device count (left) + elapsed time (right)
    QLabel *m_elapsedLabel = nullptr;
    QTimer *m_elapsedTick = nullptr;
    QElapsedTimer m_elapsedTimer;

    // Bottom buttons
    QPushButton *m_startBtn = nullptr;
    QPushButton *m_closeBtn = nullptr;

    // Icon-only toolbar (Language, About, Log)
    QToolButton *m_langBtn = nullptr;
    QToolButton *m_aboutBtn = nullptr;
    QToolButton *m_logBtn = nullptr;

    // Language
    QActionGroup *m_langGroup = nullptr;
    QMenu *m_langMenu = nullptr;

    // Worker
    QThread *m_workerThread = nullptr;
    FormatWorker *m_worker = nullptr;

    // Log panel (embedded on the right side, detachable)
    LogDialog *m_logDialog = nullptr;
    bool m_logAttached = true;
    QVBoxLayout *m_mainLayout = nullptr;
    QHBoxLayout *m_mainHBox = nullptr;
    QWidget *m_formWidget = nullptr;
    QFrame *m_logSeparator = nullptr;

    // State
    QTimer *m_autoRefreshTimer = nullptr;
    bool m_isRunning = false;
    bool m_pendingClose = false;
    bool m_isHashing = false;
    DeviceInfo m_lastDevice;

    // "Not applicable" state for images that cannot use the format options
    // (raw disk copies written in DD mode, or ZIP/TAR archives).
    bool m_formatNotApplicable = false;
    QVariant m_savedSchemeData;
    QVariant m_savedTargetData;
    QVariant m_savedFsData;
    QVariant m_savedClusterData;
    int m_savedSchemeIndex = -1;
    int m_savedTargetIndex = -1;
    int m_savedFsIndex = -1;
    int m_savedClusterIndex = -1;
    QString m_savedLabelText;

    // Last image info
    ImageInfo m_lastImageInfo;

    // Async image detection + hash computation
    QFutureWatcher<ImageInfo> *m_imageWatcher = nullptr;
    QFutureWatcher<QString> *m_hashWatcher = nullptr;
    QString m_pendingImagePath;   // image currently being detected
    QHash<QString, ImageInfo> m_imageInfoCache;   // key: path|size|mtime

    // Recently used images (SELECT dropdown menu)
    QStringList m_recentImages;
};
