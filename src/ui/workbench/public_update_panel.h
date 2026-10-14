#pragma once

#include "contracts/public_update_state.h"
#include <QWidget>
#include <QPointer>
#include <QMap>

class QLabel;
class QPushButton;
class QProgressBar;
class QDialog;

// A presentation-only surface. Receipts and catalogs are read by the runtime.
class PublicUpdatePanel final : public QWidget {
  Q_OBJECT
public:
  explicit PublicUpdatePanel(QWidget* parent = nullptr);
  void setState(const PublicUpdateState& state);
  void setExternalBusy(bool dataBusy, bool imagesBusy);
  void setLegacyStatus(const QString& message, bool busy);

signals:
  void checkRequested(const QStringList& components);
  void retryRequested();
  void restoreRequested();
  void reloadRequested();
  void cancelRequested();
  void diagnosticsRequested();

private:
  void render();
  void showChanges();
  void showDetails();
  PublicUpdateState state_;
  bool hasState_ = false;
  bool externalDataBusy_ = false;
  bool imagesBusy_ = false;
  QString legacyStatus_;
  QLabel* title_ = nullptr;
  QLabel* summary_ = nullptr;
  QLabel* operationNotice_ = nullptr;
  QLabel* checked_ = nullptr;
  QLabel* applied_ = nullptr;
  QLabel* progressText_ = nullptr;
  QLabel* dependency_ = nullptr;
  QWidget* progressArea_ = nullptr;
  QWidget* advanced_ = nullptr;
  QPushButton* check_ = nullptr;
  QPushButton* retry_ = nullptr;
  QPushButton* cancel_ = nullptr;
  QPushButton* reload_ = nullptr;
  QPushButton* restore_ = nullptr;
  QPushButton* changes_ = nullptr;
  QPushButton* export_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QList<QLabel*> steps_;
  QMap<QString, QLabel*> components_;
  QList<QPushButton*> partial_;
  QPointer<QDialog> changesDialog_;
  QPointer<QDialog> detailsDialog_;
};
