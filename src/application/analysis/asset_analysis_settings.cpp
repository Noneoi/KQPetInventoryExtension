#include "asset_analysis_settings.h"

#include "application/pet/pet_repository.h"
#include "protocol/packet_contract.h"
#include "storage/storage_service.h"

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

AssetAnalysisSettings::AssetAnalysisSettings(PetRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository), storage_(repository ? repository->storageService() : nullptr),
      retryTimer_(this) {
  retryTimer_.setSingleShot(true);
  connect(&retryTimer_, &QTimer::timeout, this, &AssetAnalysisSettings::beginLoad);
  if (!repository_ || !storage_) return;
  connect(storage_, &StorageService::completed, this, &AssetAnalysisSettings::receive);
  connect(repository_, &PetRepository::accountSessionChanged, this, &AssetAnalysisSettings::switchAccount);
  requestLoad(repository_->accountKey());
}

AssetAnalysisSettings::~AssetAnalysisSettings() {
  retryTimer_.stop();
  if (storage_) storage_->cancelReads(readContext_);
}

bool AssetAnalysisSettings::known(const QString& account) const {
  return account == account_ && known_;
}

bool AssetAnalysisSettings::pending(const QString& account) const {
  return account == account_ && (loading_ || saving_ || loadWanted_);
}

bool AssetAnalysisSettings::loadAutoSnapshot(const QString& account) const {
  return known(account) && !pending(account) && enabled_;
}

int AssetAnalysisSettings::pendingTaskCount() const {
  return static_cast<int>(tasks_.size()) + (loadWanted_ ? 1 : 0);
}

void AssetAnalysisSettings::publish(const QString& error) {
  emit stateChanged(account_, epoch_, known_, loadAutoSnapshot(account_), pending(account_), error);
}

void AssetAnalysisSettings::switchAccount(const QString& account, quint64 epoch) {
  Q_UNUSED(epoch);
  requestLoad(account);
}

void AssetAnalysisSettings::requestLoad(const QString& account) {
  if (!repository_ || !storage_ || account != repository_->accountKey() || !repository_->storageContext()) return;
  if (account == account_ && epoch_ == repository_->sessionGeneration() && (loading_ || saving_)) return;
  if (readContext_) storage_->cancelReads(readContext_);
  account_ = account;
  epoch_ = repository_->sessionGeneration();
  ++generation_;
  ++memoryRevision_;
  loadRevision_ = memoryRevision_;
  known_ = false;
  enabled_ = false;
  loading_ = true;
  saving_ = false;
  loadWanted_ = true;
  readContext_ = storage_->createAccountContext(account,
      QFileInfo(repository_->storageContext()->directory()).fileName());
  publish();
  retryTimer_.start(0);
}

void AssetAnalysisSettings::beginLoad() {
  if (!loadWanted_ || !storage_ || !repository_ || account_ != repository_->accountKey() ||
      epoch_ != repository_->sessionGeneration()) return;
  const StorageSubmission admission = storage_->submitRead(
      {readContext_, QStringLiteral("asset-analysis.json"), loadRevision_, 64 * 1024, true});
  if (!admission.accepted) {
    if (admission.status == StorageStatus::QueueFull) { retryTimer_.start(5); return; }
    loadWanted_ = false;
    loading_ = false;
    known_ = false;
    publish(admission.error);
    return;
  }
  tasks_.insert(admission.taskId, {account_, epoch_, generation_, loadRevision_, false, false});
  loadWanted_ = false;
}

StorageSubmission AssetAnalysisSettings::saveAutoSnapshot(const QString& account, bool enabled) {
  StorageSubmission rejected;
  if (!repository_ || !storage_ || account.isEmpty() || account != repository_->accountKey() ||
      !repository_->storageContext()) {
    rejected.error = QStringLiteral("设置账号与当前账号不一致");
    return rejected;
  }
  if (account != account_ || epoch_ != repository_->sessionGeneration()) requestLoad(account);
  ++memoryRevision_;
  enabled_ = enabled;
  known_ = false;
  saving_ = false;
  const QJsonObject patch{{QStringLiteral("schema"), 1}, {QStringLiteral("account"), account},
                          {QStringLiteral("autoSnapshot"), enabled}};
  StoreJsonWrite write{repository_->storageContext(), QStringLiteral("asset-analysis.json"),
                       ++nextWriteRevision_, patch, 64 * 1024, true};
  write.mergeJsonObject = true;
  const StorageSubmission admission = storage_->submitJsonWrite(write);
  if (admission.accepted) {
    saving_ = true;
    tasks_.insert(admission.taskId, {account_, epoch_, generation_, memoryRevision_, true, enabled});
  }
  publish(admission.accepted ? QString{} : admission.error);
  return admission;
}

void AssetAnalysisSettings::receive(const StorageResult& result) {
  const auto found = tasks_.find(result.taskId);
  if (found == tasks_.end()) return;
  const Pending task = found.value();
  tasks_.erase(found);
  const bool sameSession = repository_ && task.account == account_ && task.epoch == epoch_ &&
      task.generation == generation_ && account_ == repository_->accountKey() && epoch_ == repository_->sessionGeneration();
  if (task.write) {
    if (sameSession && task.memoryRevision == memoryRevision_) {
      saving_ = false;
      known_ = result.status == StorageStatus::Saved;
      if (known_) enabled_ = task.enabled;
      publish(result.error);
    }
    emit writeFinished(result.taskId, task.account, task.epoch, task.enabled, result.status, result.error);
    return;
  }
  if (!sameSession) return;
  loading_ = false;
  if (task.memoryRevision != memoryRevision_) { publish(); return; }
  if (result.status == StorageStatus::NotFound) {
    known_ = true;
    enabled_ = false;
    publish();
    return;
  }
  const QJsonObject object = result.status == StorageStatus::Loaded
      ? QJsonDocument::fromJson(result.content).object() : QJsonObject{};
  qint64 schema = 0;
  known_ = result.status == StorageStatus::Loaded &&
      PacketContracts::checkedInteger(object.value(QStringLiteral("schema")), &schema) && schema == 1 &&
      object.value(QStringLiteral("account")).toString() == account_ &&
      object.value(QStringLiteral("autoSnapshot")).isBool();
  enabled_ = known_ && object.value(QStringLiteral("autoSnapshot")).toBool();
  publish(known_ ? QString{} : QStringLiteral("自动快照设置读取失败或格式无效，暂不自动记录"));
}
