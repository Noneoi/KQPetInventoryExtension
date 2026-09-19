#pragma once
#include "inventory_read_view.h"
#include "protocol/session_context.h"
#include <QHash>
#include <QSet>
#include <memory>
#include <mutex>

struct InventoryViewSnapshot {
  // Process-monotonic delivery number; the publisher increments across accounts.
  quint64 publication = 0;
  quint64 sessionEpoch = 0;
  quint64 inventoryRevision = 0;
  std::shared_ptr<const PetDetailCatalogSnapshot> metadata;
  QString account;
  QString cachePath;
  QString dataRoot;
  QDateTime updatedAt;
  bool authenticated = false;
  bool sourceVerified = false;
  SessionConnectionState sessionState = SessionConnectionState::Disconnected;
  bool membershipChanged = false;
  QList<QJsonObject> backpack;
  QList<QJsonObject> warehouse;
  QHash<qint64, QJsonObject> details;
  QHash<qint64, RawPetRecordHandle> rawDetails;
  QHash<qint64, PetRecordVersion> recordVersions;
  QHash<qint64, PetDerivedFactsHandle> facts;
  QHash<int, PreparedPetDetailHandle> preparedDetails;
  QHash<int, QString> detailErrors;
  QHash<qint64, QDateTime> detailSavedTimes;
  QSet<qint64> changedDetails;
};

// Own this projection on GUI. Its mailbox retains just the newest immutable
// value; no widgets/models are accessed by the publishing Core thread.
class InventoryProjection final : public InventoryReadView {
  Q_OBJECT
public:
  explicit InventoryProjection(QObject* parent = nullptr);
  InventoryProjection(const QString& dataRoot, QObject* parent);
  void publish(std::shared_ptr<const InventoryViewSnapshot> snapshot);
  std::shared_ptr<const InventoryViewSnapshot> snapshot() const { return snapshot_; }
  QList<QJsonObject> backpackPets() const override;
  QList<QJsonObject> warehousePets() const override;
  QJsonObject backpackPet(qint64 id) const override;
  QJsonObject warehousePet(qint64 id) const override;
  QJsonObject detailFor(qint64 id) const override;
  RawPetRecordHandle rawRecordHandle(qint64 id) const override;
  PetRecordVersion recordVersion(qint64 id) const override;
  PetDerivedFactsHandle derivedFactsFor(qint64 id) const override;
  void watchDetail(int consumer, qint64 id) override;
  PreparedPetDetailHandle preparedDetail(int consumer, qint64 id) const override;
  QString detailPreparationError(int consumer) const override;
  void requestDetailPage(int consumer, DetailSection section, int pageIndex) override;
  bool hasCachedDetail(qint64 id) const override;
  QDateTime detailSavedAt(qint64 id) const override;
  QString accountKey() const override;
  QString cachePath() const override;
  QString dataRoot() const override;
  QDateTime updatedAt() const override;
  bool isAuthenticated() const override;
  bool currentSourceVerified() const override {
    return snapshot_ && snapshot_->sourceVerified && snapshot_->sessionState == SessionConnectionState::Active;
  }
  std::shared_ptr<const PetDetailCatalogSnapshot> metadataSnapshot() const override {
    return snapshot_ ? snapshot_->metadata : nullptr;
  }

signals:
  void detailInterestsChanged(const QString& account, quint64 epoch, const QSet<qint64>& ids);
  void detailSelectionChanged(const QString& account, quint64 epoch, int consumer, qint64 id);
  void detailPageRequested(const QString& account, quint64 epoch, int consumer, DetailSection section, int pageIndex);
private:
  void drain();
  void notifyDetails();
  std::shared_ptr<const InventoryViewSnapshot> snapshot_;
  QHash<qint64, int> backpackRows_;
  QHash<qint64, int> warehouseRows_;
  QHash<int, qint64> detailInterests_;
  QSet<qint64> notifications_;
  bool notificationScheduled_ = false;
  std::mutex mailboxMutex_;
  std::shared_ptr<const InventoryViewSnapshot> pending_;
  QSet<qint64> pendingDetails_;
  bool pendingMembership_ = false;
  bool scheduled_ = false;
  quint64 highestPublication_ = 0;
};
