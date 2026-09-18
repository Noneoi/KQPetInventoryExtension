#include "inventory_projection.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QThread>

InventoryProjection::InventoryProjection(QObject* parent) : InventoryProjection(QString{}, parent) {}
InventoryProjection::InventoryProjection(const QString& dataRoot, QObject* parent) : InventoryReadView(parent) {
  auto initial = std::make_shared<InventoryViewSnapshot>();
  initial->dataRoot = dataRoot;
  snapshot_ = std::move(initial);
}

void InventoryProjection::publish(std::shared_ptr<const InventoryViewSnapshot> next) {
  if (!next || !next->publication) return;
  std::lock_guard<std::mutex> lock(mailboxMutex_);
  if (next->publication <= highestPublication_) return;
  highestPublication_ = next->publication;
  if (pending_ && (pending_->account != next->account || pending_->sessionEpoch != next->sessionEpoch)) {
    pendingDetails_.clear();
    pendingMembership_ = true;
  }
  pending_ = std::move(next);
  pendingDetails_.unite(pending_->changedDetails);
  pendingMembership_ = pendingMembership_ || pending_->membershipChanged;
  if (scheduled_) return;
  scheduled_ = true;
  QMetaObject::invokeMethod(this, [this] { drain(); }, Qt::QueuedConnection);
}

void InventoryProjection::drain() {
  Q_ASSERT(thread() == QThread::currentThread());
  std::shared_ptr<const InventoryViewSnapshot> next;
  QSet<qint64> details;
  bool membership = false;
  {
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    next = std::move(pending_);
    details = std::move(pendingDetails_);
    membership = pendingMembership_;
    pendingMembership_ = false;
    scheduled_ = false;
  }
  if (!next) return;
  const bool sessionChanged = !snapshot_ || snapshot_->account != next->account ||
                              snapshot_->sessionEpoch != next->sessionEpoch;
  const bool metadataChanged = next->metadata && (!snapshot_ || snapshot_->metadata != next->metadata);
  const bool sourceChanged = !snapshot_ || snapshot_->sourceVerified != next->sourceVerified ||
      snapshot_->sessionState != next->sessionState || snapshot_->authenticated != next->authenticated;
  const auto previous = snapshot_; // Keep old payload leases until synchronous observers have updated.
  snapshot_ = std::move(next);
  if (sessionChanged) notifications_.clear();
  notifications_.unite(details);
  if (sessionChanged || membership) {
    backpackRows_.clear(); warehouseRows_.clear();
    for (qsizetype row = 0; row < snapshot_->backpack.size(); ++row)
      backpackRows_.insert(snapshot_->backpack.at(row).value(QStringLiteral("id")).toVariant().toLongLong(), int(row));
    for (qsizetype row = 0; row < snapshot_->warehouse.size(); ++row)
      warehouseRows_.insert(snapshot_->warehouse.at(row).value(QStringLiteral("id")).toVariant().toLongLong(), int(row));
  }
  QPointer<InventoryProjection> alive(this);
  if (sessionChanged) {
    detailInterests_.clear();
    emit detailInterestsChanged(snapshot_->account, snapshot_->sessionEpoch, {});
    if (!alive) return;
    emit accountSessionChanged(snapshot_->account, snapshot_->sessionEpoch);
    if (!alive) return;
  } else if (membership) {
    QSet<qint64> watched;
    bool removed = false;
    for (auto it = detailInterests_.begin(); it != detailInterests_.end();) {
      if (!backpackRows_.contains(it.value()) && !warehouseRows_.contains(it.value())) {
        it = detailInterests_.erase(it); removed = true;
      } else { watched.insert(it.value()); ++it; }
    }
    if (removed) emit detailInterestsChanged(snapshot_->account, snapshot_->sessionEpoch, watched);
    if (!alive) return;
  }
  if (metadataChanged) emit this->metadataChanged(snapshot_->metadata->revision);
  if (!alive) return;
  if (membership || sessionChanged || sourceChanged) emit dataChanged();
  if (!alive) return;
  if (!notificationScheduled_) notifyDetails();
}

void InventoryProjection::notifyDetails() {
  notificationScheduled_ = false;
  QElapsedTimer slice; slice.start();
  QPointer<InventoryProjection> alive(this);
  int units = 0;
  // Signals include their synchronous GUI work. Bound that work as well as the
  // mailbox itself; a burst must not emit thousands of row updates in one turn.
  while (!notifications_.isEmpty() && units++ < 32 && slice.nsecsElapsed() < 8000000) {
    const auto it = notifications_.begin(); const auto id = *it; notifications_.erase(it);
    if (!backpackRows_.contains(id) && !warehouseRows_.contains(id)) continue;
    emit detailChanged(id);
    if (!alive) return;
  }
  if (!notifications_.isEmpty() && !notificationScheduled_) {
    notificationScheduled_ = true;
    QMetaObject::invokeMethod(this, [this] { notifyDetails(); }, Qt::QueuedConnection);
  }
}

QList<QJsonObject> InventoryProjection::backpackPets() const { return snapshot_ ? snapshot_->backpack : QList<QJsonObject>{}; }
QList<QJsonObject> InventoryProjection::warehousePets() const { return snapshot_ ? snapshot_->warehouse : QList<QJsonObject>{}; }
QJsonObject InventoryProjection::backpackPet(qint64 id) const {
  const auto row = backpackRows_.constFind(id);
  return snapshot_ && row != backpackRows_.cend() ? snapshot_->backpack.value(row.value()) : QJsonObject{};
}
QJsonObject InventoryProjection::warehousePet(qint64 id) const {
  const auto row = warehouseRows_.constFind(id);
  return snapshot_ && row != warehouseRows_.cend() ? snapshot_->warehouse.value(row.value()) : QJsonObject{};
}
QJsonObject InventoryProjection::detailFor(qint64 id) const {
  const auto raw = rawRecordHandle(id);
  QJsonObject result = raw ? raw->object() : snapshot_ ? snapshot_->details.value(id) : QJsonObject{};
  const QJsonObject brief = backpackRows_.contains(id) ? backpackPet(id) : warehousePet(id);
  for (auto it = brief.constBegin(); it != brief.constEnd(); ++it) result.insert(it.key(), it.value());
  return result;
}
QDateTime InventoryProjection::detailSavedAt(qint64 id) const { return snapshot_ ? snapshot_->detailSavedTimes.value(id) : QDateTime{}; }
bool InventoryProjection::hasCachedDetail(qint64 id) const {
  return snapshot_ && (snapshot_->recordVersions.value(id).complete || snapshot_->details.contains(id));
}
RawPetRecordHandle InventoryProjection::rawRecordHandle(qint64 id) const {
  const auto result = snapshot_ ? snapshot_->rawDetails.value(id) : RawPetRecordHandle{};
  if (!result || result->key.account != snapshot_->account || result->key.epoch != snapshot_->sessionEpoch ||
      result->key.instanceId != id || !(result->key == snapshot_->recordVersions.value(id).key)) return {};
  return result;
}
PetRecordVersion InventoryProjection::recordVersion(qint64 id) const {
  const auto result = snapshot_ ? snapshot_->recordVersions.value(id) : PetRecordVersion{};
  return snapshot_ && result.key.account == snapshot_->account && result.key.epoch == snapshot_->sessionEpoch &&
      result.key.instanceId == id ? result : PetRecordVersion{};
}
PetDerivedFactsHandle InventoryProjection::derivedFactsFor(qint64 id) const {
  const auto result = snapshot_ ? snapshot_->facts.value(id) : PetDerivedFactsHandle{};
  if (!result || result->key.record.account != snapshot_->account || result->key.record.epoch != snapshot_->sessionEpoch ||
      !(result->key.record == snapshot_->recordVersions.value(id).key) || !snapshot_->metadata ||
      result->key.metadataRevision != snapshot_->metadata->revision || result->key.metadataDigest != snapshot_->metadata->contentDigest ||
      result->key.analysisVersion != AssetAnalysisVersion::kCurrentAnalysis) return {};
  return result;
}
void InventoryProjection::watchDetail(int consumer, qint64 id) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (consumer < 0 || consumer >= kDetailConsumerCount) return;
  if (id > 0 && !backpackRows_.contains(id) && !warehouseRows_.contains(id)) id = 0;
  if (detailInterests_.value(consumer) == id) return;
  if (id > 0) detailInterests_.insert(consumer, id); else detailInterests_.remove(consumer);
  QSet<qint64> ids;
  for (auto selected : detailInterests_) ids.insert(selected);
  QPointer<InventoryProjection> alive(this);
  emit detailInterestsChanged(accountKey(), snapshot_ ? snapshot_->sessionEpoch : 0, ids);
  if (alive) emit detailSelectionChanged(accountKey(), snapshot_ ? snapshot_->sessionEpoch : 0, consumer, id);
}
PreparedPetDetailHandle InventoryProjection::preparedDetail(int consumer, qint64 id) const {
  const auto value = snapshot_ ? snapshot_->preparedDetails.value(consumer) : PreparedPetDetailHandle{};
  if (!value || value->identity.instanceId != id || !(value->version.facts.record == recordVersion(id).key) ||
      !snapshot_->metadata || value->version.facts.metadataRevision != snapshot_->metadata->revision ||
      value->version.facts.metadataDigest != snapshot_->metadata->contentDigest) return {};
  return value;
}
QString InventoryProjection::detailPreparationError(int consumer) const {
  return snapshot_ ? snapshot_->detailErrors.value(consumer) : QString{};
}
void InventoryProjection::requestDetailPage(int consumer, DetailSection section, int pageIndex) {
  if (!snapshot_ || consumer < 0 || consumer >= kDetailConsumerCount || pageIndex < 0 || !detailInterests_.contains(consumer)) return;
  emit detailPageRequested(snapshot_->account, snapshot_->sessionEpoch, consumer, section, pageIndex);
}
QString InventoryProjection::accountKey() const { return snapshot_ ? snapshot_->account : QString{}; }
QString InventoryProjection::cachePath() const { return snapshot_ ? snapshot_->cachePath : QString{}; }
QString InventoryProjection::dataRoot() const { return snapshot_ ? snapshot_->dataRoot : QString{}; }
QDateTime InventoryProjection::updatedAt() const { return snapshot_ ? snapshot_->updatedAt : QDateTime{}; }
bool InventoryProjection::isAuthenticated() const { return snapshot_ && snapshot_->authenticated; }
