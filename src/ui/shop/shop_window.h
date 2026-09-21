#pragma once

#include "domain/catalog_types.h"
#include "domain/compiled_shop_catalog.h"
#include "domain/pet_metadata_view.h"
#include "domain/shop_pet_eligibility.h"
#include "domain/shop_actionability.h"
#include "domain/activity_shop_observation.h"

#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QPointer>

class InventoryReadView;
class PetImageCache;
class PetImageBrowser;
class PetRawDataTree;
class QLabel;
class QLineEdit;
class QTimer;
class QPushButton;
class QComboBox;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;
class QTextBrowser;
class QGridLayout;
class QSplitter;

class ShopWindow final : public QDialog {
  Q_OBJECT

public:
  explicit ShopWindow(InventoryReadView* repository, QWidget* parent = nullptr,
                      PetImageCache* sharedImages = nullptr);
  void setPacket(const QJsonObject& packet, bool hasPacket);
  void setMaterialCounts(const QHash<QString, qint64>& counts, bool valid);
  void setReadOnlyObservations(const QJsonObject& packets);
  void setCatalogSnapshot(std::shared_ptr<const ShopCatalogSnapshot> catalog, QDate businessDate);
  void setQuotaValidity(quint64 revision, const QHash<QString, ShopCondition>& validity);
  void setStatus(const QString& status, const QString& details = {});
  void setRefreshRunning(bool running);
  void focusGood(const QString& stableKey);
  quint64 compiledEligibilityRules() const { return compiledEligibilityRules_; }
  bool petRowsPreparing() const { return petRowsPreparing_; }
  quint64 petIndexBuilds() const { return petIndexBuilds_; }
  quint64 petMembershipCacheHits() const { return petMembershipCacheHits_; }

public slots:
  void setWorkbenchMode(bool embedded, bool compact);
  void resetSessionContext();
  void setMoveRunning(bool running);
  void finishDetailRefresh(qint64 instanceId, bool succeeded,
                           const QString& reason);

signals:
  void refreshRequested();
  void catalogRefreshRequested();
  void openShopRequested(const QString& navigationLink);
  void detailRequested(qint64 instanceId);
  void moveToBackpackRequested(qint64 instanceId);

protected:
  bool event(QEvent* event) override;

private:
  struct GoodRow {
    ShopExchangeGood good;
    int tableRow = 0;
    QString stableKey;
  };
  struct CachedPetRow {
    QJsonObject brief;
    QString name;
    bool backpack = false;
    QStringList values;
    qint64 level = -1, power = -1, highest = -1;
  };
  struct CachedActivityGood {
    ActivityShopObservation observation;
    bool quotaVerified = false;
    QString costText;
    QList<ResourceRequirement> currencyRequirements;
  };

  void rebuild();
  void scheduleRebuild();
  void rebuildEligiblePetIndex();
  void rebuildActivityGoods(const QJsonObject& packet);
  int eligiblePetCount(const ShopExchangeGood& good) const;
  void updateCurrencySummary();
  void updateOpenShopButton();
  void ensureImageCache();
  void showGoodPets(const ShopExchangeGood& good, const QString& stableKey = {},
                    qint64 preserveInstanceId = 0, bool restoreScroll = false);
  void rebuildPetRows(qint64 preserveInstanceId = 0);
  QList<qint64> eligiblePets(const ShopExchangeGood& good);
  CachedPetRow& cachedPetRow(qint64 instanceId);
  void continuePetRows(quint64 generation);
  void cancelPetRows();
  void showPetDetail(qint64 instanceId, bool requestLatest);
  void updateCurrentDetail(qint64 instanceId);
  void updateCurrentImage(const QString& visualKey, const QString& localPath);
  void fillPetRow(int row, const QJsonObject& pet,
                  const ShopPetEligibility& eligibility);
  void updatePetTitle();
  void updateMoveButton();
  void moveCurrentToBackpack();
  QString costText(const ShopExchangeGood& good) const;
  // Need / own / short for one exchange of a good; colour is invalid when neutral.
  struct ResourceStatus { QString text; QString tip; QString color; };
  ResourceStatus resourceStatus(const ShopExchangeGood& good) const;
  // Hides goods that do not match the search box; optionally jumps to the first
  // shop tab with a match when the current one has none.
  void applyGoodSearch(bool selectMatchingTab);
  QString shopCurrencyText(const ShopExchangeShop& shop) const;
  const CompiledShopGood* compiledGood(const ShopExchangeGood& good) const;
  void changePetSort(int logicalColumn);
  void updateSelectedGoodEmphasis();
  void updateWorkbenchPanels();
  ShopPetEligibility eligibilityFor(qint64 id);

  InventoryReadView* repository_ = nullptr;
  QPointer<PetImageCache> imageCache_;
  QGridLayout* toolbarLayout_ = nullptr;
  QSplitter* contentSplitter_ = nullptr;
  QSplitter* listingSplitter_ = nullptr;
  QTabWidget* detailTabs_ = nullptr;
  QPushButton* detailToggle_ = nullptr;
  bool workbenchEmbedded_ = false;
  bool workbenchCompact_ = false;
  bool compactDetails_ = false;
  QList<int> workbenchWideSizes_;
  QPushButton* refresh_ = nullptr;
  QPushButton* refreshCatalog_ = nullptr;
  QPushButton* openShop_ = nullptr;
  QComboBox* sourceFilter_ = nullptr;
  QLineEdit* goodSearch_ = nullptr;
  QTimer* goodSearchDebounce_ = nullptr;
  QLabel* status_ = nullptr;
  QLabel* currencySummary_ = nullptr;
  QTabWidget* shopTabs_ = nullptr;
  QTableWidget* petTable_ = nullptr;
  QLabel* petTitle_ = nullptr;
  PetImageBrowser* petDetail_ = nullptr;
  PetRawDataTree* rawDetail_ = nullptr;
  QPushButton* moveToBackpack_ = nullptr;
  QJsonObject packet_;
  QJsonObject readOnlyPackets_;
  QJsonObject readOnlyShopPacket_;
  QHash<QString, qint64> readOnlyMaterialCounts_;
  QSet<QString> readOnlyMaterialTypes_;
  QHash<QString, ShopCondition> quotaValidity_;
  quint64 quotaRevision_ = 0;
  bool quotaInitialized_ = false;
  QHash<QString, qint64> materialCounts_;
  bool hasPacket_ = false;
  bool hasMaterialCounts_ = false;
  std::shared_ptr<const ShopCatalogSnapshot> catalogSnapshot_;
  QDate catalogDate_;
  QList<ShopExchangeShop> visibleShops_;
  CompiledShopCatalog compiledCatalog_;
  QHash<QString, qsizetype> compiledGoodsByKey_;
  QHash<QString,CachedActivityGood> activityGoods_;
  PetMetadataView materialMetadata_{nullptr};
  QList<QList<GoodRow>> shopRows_;
  QHash<QString,QPair<int,int>> goodLocations_;
  QString currentGoodKey_, highlightedGoodKey_;
  ShopExchangeGood currentGood_;
  CompiledShopPetRule currentRule_;
  QString currentRuleExpression_;
  bool currentRuleKnown_ = false;
  quint64 compiledEligibilityRules_ = 0;
  QHash<qint64, ShopPetEligibility> currentEligibility_;
  QHash<QString,CompiledShopPetRule> eligibilityRules_;
  qint64 currentInstanceId_ = 0;
  qint64 pendingDetailId_ = 0;
  QString currentVisualKey_;
  bool moveRunning_ = false;
  int petSortColumn_ = -1;
  bool petSortAscending_ = true;
  bool rebuildScheduled_ = false;
  QHash<int, QSet<qint64>> eligiblePetIdsByRace_;
  QHash<qint64,CachedPetRow> petRowCache_;
  QHash<qint64,int> defaultPetOrder_;
  QHash<QString,QList<qint64>> eligibleOrderByGood_;
  QHash<qint64,int> visiblePetRows_;
  bool petIndexDirty_ = true, petRowsPreparing_ = false, synchronousPetRows_ = false;
  bool restoreScrollAfterRows_ = false;
  QPair<int,int> petScrollAfterRows_, detailScrollAfterRows_;
  QList<qint64> pendingPetIds_;
  int pendingPetRow_ = 0;
  qint64 preservePetId_ = 0;
  quint64 petRowsGeneration_ = 0, petIndexBuilds_ = 0, petMembershipCacheHits_ = 0;
  QString pendingFocusGoodKey_;
};
