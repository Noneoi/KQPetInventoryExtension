#pragma once

#include "shop_exchange_catalog.h"
#include "shop_pet_eligibility.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>

class PetRepository;
class PetImageCache;
class QLabel;
class QPushButton;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QTextBrowser;

class ShopWindow final : public QDialog {
  Q_OBJECT

public:
  explicit ShopWindow(PetRepository* repository, QWidget* parent = nullptr);
  void setPacket(const QJsonObject& packet, bool hasPacket);
  void setMaterialCounts(const QHash<QString, qint64>& counts, bool valid);
  void setStatus(const QString& status);
  void setRefreshRunning(bool running);

public slots:
  void setMoveRunning(bool running);
  void finishDetailRefresh(qint64 instanceId, bool succeeded,
                           const QString& reason);
  void requestReplacement(qint64 incomingInstanceId,
                          const QList<qint64>& eligibleBackpackIds);

signals:
  void refreshRequested();
  void catalogRefreshRequested();
  void detailRequested(qint64 instanceId);
  void moveToBackpackRequested(qint64 instanceId);
  void moveReplacementChosen(qint64 outgoingInstanceId);
  void moveCancelRequested();

private:
  struct GoodRow {
    ShopExchangeGood good;
    int tableRow = 0;
  };

  void rebuild();
  void scheduleRebuild();
  void rebuildEligiblePetIndex();
  int eligiblePetCount(const ShopExchangeGood& good) const;
  void updateCurrencySummary();
  void ensureImageCache();
  void showGoodPets(const ShopExchangeGood& good);
  void rebuildPetRows(qint64 preserveInstanceId = 0);
  QList<QJsonObject> eligiblePets(const ShopExchangeGood& good) const;
  void showPetDetail(qint64 instanceId, bool requestLatest);
  void updateCurrentDetail(qint64 instanceId);
  void updateCurrentImage(const QString& visualKey, const QString& localPath);
  void fillPetRow(int row, const QJsonObject& pet,
                  const ShopPetEligibility& eligibility);
  void updatePetTitle();
  void updateMoveButton();
  void moveCurrentToBackpack();
  QString costText(const ShopExchangeGood& good) const;
  QString shopCurrencyText(const ShopExchangeShop& shop) const;
  void changePetSort(int logicalColumn);
  void updateSelectedGoodEmphasis();

  PetRepository* repository_ = nullptr;
  PetImageCache* imageCache_ = nullptr;
  QPushButton* refresh_ = nullptr;
  QPushButton* refreshCatalog_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* currencySummary_ = nullptr;
  QTabWidget* shopTabs_ = nullptr;
  QTableWidget* petTable_ = nullptr;
  QLabel* petTitle_ = nullptr;
  QTextBrowser* petDetail_ = nullptr;
  QPlainTextEdit* rawDetail_ = nullptr;
  QPushButton* moveToBackpack_ = nullptr;
  QJsonObject packet_;
  QHash<QString, qint64> materialCounts_;
  bool hasPacket_ = false;
  bool hasMaterialCounts_ = false;
  QList<QList<GoodRow>> shopRows_;
  ShopExchangeGood currentGood_;
  QHash<qint64, ShopPetEligibility> currentEligibility_;
  qint64 currentInstanceId_ = 0;
  qint64 pendingDetailId_ = 0;
  QString currentVisualKey_;
  bool moveRunning_ = false;
  int petSortColumn_ = -1;
  bool petSortAscending_ = true;
  bool rebuildScheduled_ = false;
  QHash<int, QSet<qint64>> eligiblePetIdsByRace_;
};
