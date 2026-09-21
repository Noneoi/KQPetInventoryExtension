#pragma once

#include "contracts/refresh_timings.h"

#include <QDialog>
#include <QJsonObject>

class QSpinBox;
class QLineEdit;
class QLabel;
class QComboBox;
class QCheckBox;
class QPushButton;
class QProgressBar;
class QTextBrowser;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QDialogButtonBox;

class PetSettingsDialog final : public QDialog {
  Q_OBJECT

public:
  explicit PetSettingsDialog(const RefreshTimings& timings,
                             QWidget* parent = nullptr);
  RefreshTimings timings() const;
  void setCacheRoot(const QString& root);
  void applyCacheResult(const QString& action, const QJsonObject& result);
  void setDataUpdateStatus(const QString& message, bool busy);
  void setImageBatchProgress(int completed, int total, int failed);
  void finishImageBatch(bool cancelled, int failed);

signals:
  void cacheActionRequested(const QString& action, const QJsonObject& options);
  // Empty: update everything. Otherwise the selected public data parts.
  void dataUpdateRequested(const QStringList& components);
  void missingImagesRequested();
  void imageBatchPauseRequested(bool paused);
  void imageBatchCancelRequested();

private:
  void applyTimings(const RefreshTimings& timings);
  QWidget* createCachePage();
  QWidget* createDataPage();
  QWidget* createSoftwareUpdatePage();
  QWidget* createTimingPage();
  void checkSoftwareUpdate();
  void checkSoftwareUpdateFallback(const QString& reason);
  void installSoftwareUpdate();
  void finishSoftwareUpdate();
  void setSoftwareUpdateBusy(bool busy, const QString& status);
  void requestCacheAction(const QString& action, const QJsonObject& options = {});
  bool confirmRemoval(const QString& description);
  QString selectedAccount() const;

protected:
  void reject() override;

private:
  QSpinBox* listRequestGapMs_ = nullptr;
  QSpinBox* listTimeoutSeconds_ = nullptr;
  QSpinBox* detailRequestGapMs_ = nullptr;
  QSpinBox* detailBatchSize_ = nullptr;
  QSpinBox* detailBatchRestSeconds_ = nullptr;
  QSpinBox* detailTimeoutSeconds_ = nullptr;
  QSpinBox* detailMaxRetries_ = nullptr;
  QSpinBox* moveRequestTimeoutSeconds_ = nullptr;
  QString cacheRoot_;
  QLineEdit* cacheRootEdit_ = nullptr;
  QLabel* cacheSummary_ = nullptr;
  QLabel* cacheStatus_ = nullptr;
  QWidget* cacheActions_ = nullptr;
  QComboBox* account_ = nullptr;
  QLineEdit* instanceId_ = nullptr;
  QLineEdit* imageFile_ = nullptr;
  QCheckBox* restoreOverwrite_ = nullptr;
  QLabel* dataUpdateStatus_ = nullptr;
  QPushButton* dataUpdateButton_ = nullptr;
  QList<QPushButton*> partialUpdateButtons_;
  QPushButton* missingImagesButton_ = nullptr;
  QProgressBar* imageProgress_ = nullptr;
  QLabel* imageStatus_ = nullptr;
  QPushButton* imagePause_ = nullptr;
  QPushButton* imageCancel_ = nullptr;
  QLabel* softwareUpdateTitle_ = nullptr;
  QLabel* softwareUpdateStatus_ = nullptr;
  QTextBrowser* softwareUpdateNotes_ = nullptr;
  QPushButton* softwareUpdateCheck_ = nullptr;
  QPushButton* softwareUpdateInstall_ = nullptr;
  QPushButton* softwareUpdateReleasePage_ = nullptr;
  QProgressBar* softwareUpdateProgress_ = nullptr;
  QDialogButtonBox* dialogButtons_ = nullptr;
  QNetworkAccessManager* softwareUpdateNetwork_ = nullptr;
  QNetworkReply* softwareUpdateReply_ = nullptr;
  QProcess* softwareUpdateProcess_ = nullptr;
  QString softwareUpdateTag_;
  QString softwareUpdatePageUrl_;
  bool softwareUpdateAvailable_ = false;
  bool imageBatchRunning_ = false;
  bool dataUpdateBusy_ = false;
};
