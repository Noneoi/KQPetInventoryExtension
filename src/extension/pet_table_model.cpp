#include "pet_table_model.h"

#include "pet_identity.h"
#include "pet_image_cache.h"
#include "pet_move_policy.h"
#include "inventory_read_view.h"
#include "pet_facts_ui.h"

#include <QBrush>
#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QThread>
#include <QSet>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointer>

#include <algorithm>
#include <climits>
#include <limits>

namespace {

QString battlePowerText(const PetBattlePowerState& state) {
  const bool localKnown = state.hasCurrent && state.currentLocallyCalculated;
  if (!localKnown && !state.hasExtreme) return QStringLiteral("—");
  const QString current = localKnown ? QString::number(state.current)
                                           : QStringLiteral("—");
  const QString extreme = state.hasExtreme ? QString::number(state.extreme)
                                           : QStringLiteral("—");
  QString text = QStringLiteral("%1 / %2").arg(current, extreme);
  if (localKnown && state.hasHighest && state.current >= state.highest)
    text += QStringLiteral("（至高）");
  return text;
}

QString positionText(const QJsonObject& pet, PetTableModel::Location location) {
  if (location == PetTableModel::Location::Warehouse) {
    return pet.value(QStringLiteral("_warehouseGroup")).toString() ==
                   QStringLiteral("elite")
               ? QStringLiteral("精英")
               : QStringLiteral("普通");
  }
  if (!pet.contains(QStringLiteral("_position"))) return QStringLiteral("待刷新");
  const int index = qMax(0, pet.value(QStringLiteral("_position")).toInt());
  return QStringLiteral("第%1页 第%2排")
      .arg(index / 12 + 1)
      .arg(index % 12 / 6 + 1);
}


bool changed(const QJsonObject& before, const QJsonObject& after, const QStringList& keys) {
  for (const QString& key : keys)
    if (before.value(key) != after.value(key)) return true;
  return false;
}

qint64 nonnegativeInteger(const QJsonValue& value, qint64 maximum = std::numeric_limits<qint64>::max()) {
  if (value.isDouble()) {
    const qint64 number = value.toInteger(-1);
    return number >= 0 && number <= maximum ? number : -1;
  }
  if (!value.isString() || value.toString().isEmpty()) return -1;
  qint64 number = 0;
  for (const QChar digit : value.toString()) {
    if (digit < QLatin1Char('0') || digit > QLatin1Char('9')) return -1;
    const int next = digit.unicode() - '0';
    if (number > (maximum - next) / 10) return -1;
    number = number * 10 + next;
  }
  return number;
}

qint64 checkedInstanceId(const QJsonObject& pet) {
  return qMax<qint64>(0, nonnegativeInteger(pet.value(QStringLiteral("id"))));
}

QString displayName(const QJsonObject& pet) {
  const QString name = pet.value(QStringLiteral("n")).toString();
  return name.isEmpty() ? QStringLiteral("未返回名称") : name;
}

QStringList categoryParts(const QString& text) {
  QStringList result;
  for (QString part : text.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
    part = part.trimmed();
    if (!part.isEmpty()) result.append(part);
  }
  if (result.isEmpty() && !text.trimmed().isEmpty()) result.append(text.trimmed());
  return result;
}

}  // namespace

struct PetTableModel::Preparation {
  enum Phase { Prepare, Membership, Caches, Retire } phase = Prepare;
  quint64 generation = 0;
  QList<QJsonObject> pets;
  QSet<qint64> ids;
  QList<std::shared_ptr<Row>> prepared;
  PetMetadataView metadata;
  int cursor = 0;
  bool force = false;
  Preparation(quint64 id, QList<QJsonObject> input, std::shared_ptr<const PetDetailCatalogSnapshot> snapshot,
              bool forceMetadata)
      : generation(id), pets(std::move(input)), metadata(std::move(snapshot)), force(forceMetadata) {}
};

PetTableModel::~PetTableModel() = default;

PetTableModel::PetTableModel(Location location, InventoryReadView* repository,
                             PetImageCache* imageCache, QObject* parent)
    : QAbstractTableModel(parent), location_(location), repository_(repository), imageCache_(imageCache) {
  metadataSnapshot_ = std::make_shared<PetDetailCatalogSnapshot>();
  if (repository_) {
    metadataSnapshot_ = repository_->metadataSnapshot();
    if (!metadataSnapshot_) metadataSnapshot_ = std::make_shared<PetDetailCatalogSnapshot>();
    connect(repository_,&InventoryReadView::detailChanged,this,&PetTableModel::refreshFacts);
    connect(repository_,&InventoryReadView::metadataChanged,this,[this](quint64) {
      if (const auto metadata = repository_->metadataSnapshot()) setMetadataSnapshot(metadata);
    });
    connect(repository_,&InventoryReadView::accountSessionChanged,this,[this](const QString&,quint64) {
      setPets({}); // Cancels pending generations before any old-account row can commit.
    });
  }
  metadataRevision_ = metadataSnapshot_->revision;
  if (imageCache_) connect(imageCache_, &PetImageCache::attributeIconsReady, this, [this] {
    attributeIcons_.clear();
    if (!rows_.isEmpty()) emit dataChanged(index(0, AttributesColumn), index(rows_.size() - 1, AttributesColumn), {Qt::DecorationRole});
  });
}

int PetTableModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : rows_.size();
}

int PetTableModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : location_ == Location::Backpack ? 9 : 8;
}

const PetRowCache* PetTableModel::cachedRow(int row) const {
  return row >= 0 && row < rows_.size() ? &rows_.at(row)->cache : nullptr;
}

QIcon PetTableModel::attributeIcon(const QString& key) const {
  if (!imageCache_) return {};
  if (const QIcon* cached = attributeIcons_.object(key)) return *cached;
  const QIcon icon = imageCache_->attributeIcon(key);
  attributeIcons_.insert(key, new QIcon(icon));
  ++stats_.attributeIconsBuilt;
  return icon;
}

QVariant PetTableModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || index.model() != this || index.column() < 0 ||
      index.column() >= columnCount()) return {};
  const PetRowCache* cache = cachedRow(index.row());
  if (!cache) return {};
  if (role == InstanceIdRole) return cache->instanceId;
  if (role == IdentityTextRole) return cache->identityText;
  if (role == PetObjectRole) return rows_.at(index.row())->pet;
  if (role == FilterCacheRevisionRole || role == SortCacheRevisionRole) return cache->revision;
  if (role == NameSearchIndexRole && index.column() <= OriginalNameColumn)
    return QVariant::fromValue(cache->displayedNameSearch[index.column()]);
  if (role == Qt::DisplayRole) return cache->display[index.column()];
  if (role == Qt::ToolTipRole && index.column() <= OriginalNameColumn) return cache->nameToolTip;
  if (role == Qt::ToolTipRole && index.column() == BattlePowerColumn) {
    return cache->powerToolTip;
  }
  if (role == Qt::ToolTipRole) return cache->display[index.column()];
  if (role == Qt::DecorationRole && index.column() == AttributesColumn) return attributeIcon(cache->attributeVisualKey);
  if (location_ == Location::Backpack && index.column() == DeployedColumn && cache->deployed) {
    if (role == Qt::FontRole) { QFont font; font.setBold(true); return font; }
    if (role == Qt::ForegroundRole) return QBrush(QColor(220, 38, 38));
  }
  return {};
}

QVariant PetTableModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
  static const QStringList headers{QStringLiteral("显示名称"), QStringLiteral("精灵原名"), QStringLiteral("属性"),
      QStringLiteral("职业"), QStringLiteral("时代"), QStringLiteral("等级"),
      QStringLiteral("战斗力 /\n极限战斗力")};
  if (section >= 0 && section < headers.size()) return headers[section];
  if (location_ == Location::Backpack && section == DeployedColumn) return QStringLiteral("是否\n出战");
  if (section == columnCount() - 1) return QStringLiteral("位置");
  return {};
}

void PetTableModel::derive(Row& row, const QJsonObject& source, bool force, QList<Change>* changes,
                           const PetMetadataView* metadata) {
  const QJsonObject pet = repository_ ? PetFactsUi::rowSummary(source) : source;
  const QJsonObject previous = row.pet;
  const PetMetadataView catalog(metadata ? metadata->snapshot() : metadataSnapshot_);
  const auto facts = repository_ ? PetFactsUi::current(repository_,checkedInstanceId(pet),catalog.snapshot()) : PetDerivedFactsHandle{};
  const auto previousFacts = row.factsIdentity.lock();
  const bool factsChanged = repository_ && (facts != previousFacts || row.cache.powerFromFacts != bool(facts));
  const bool sourceChanged = repository_ && row.cache.currentSourceVerified != repository_->currentSourceVerified();
  const bool metadataVersionChanged = row.cache.metadataRevision != catalog.snapshot()->revision ||
                                      row.cache.metadataDigest != catalog.snapshot()->contentDigest;
  if (!force && !metadataVersionChanged && !factsChanged && !sourceChanged && previous == pet) return;
  const auto oldDisplay = row.cache.display;
  const QString oldTip = row.cache.nameToolTip;
  const QString oldPowerTip = row.cache.powerToolTip;
  const QString oldIcon = row.cache.attributeVisualKey;
  const bool oldDeployed = row.cache.deployed;
  const auto oldSort = row.cache.sortKeys;
  static const QStringList metadataKeys{QStringLiteral("r"), QStringLiteral("ri"),
      QStringLiteral("n"), QStringLiteral("fr"), QStringLiteral("rt"),
      QStringLiteral("_metaRaceId"), QStringLiteral("_metaOriginalName"),
      QStringLiteral("_metaAttributes"), QStringLiteral("_metaJobs"), QStringLiteral("_metaEra")};
  static const QStringList powerKeys{QStringLiteral("zdl"), QStringLiteral("xzdl"),
      QStringLiteral("czdlv"), QStringLiteral("mzdlv"), QStringLiteral("sgs"),
      QStringLiteral("sgsp"), QStringLiteral("astrolabebr"), QStringLiteral("stargodSlotMaxLevel")};
  static const QStringList sortKeys{QStringLiteral("zdl"), QStringLiteral("xzdl"),
      QStringLiteral("gd"), QStringLiteral("r"), QStringLiteral("ri"), QStringLiteral("_position")};
  const bool metadataChanged = force || metadataVersionChanged || changed(previous, pet, metadataKeys);
  const bool namesChanged = metadataChanged ||
      previous.value(QStringLiteral("customName")) != pet.value(QStringLiteral("customName"));
  const bool powerChanged = metadataChanged || factsChanged || sourceChanged || changed(previous, pet, powerKeys);
  const bool keysChanged = force || factsChanged || metadataVersionChanged || changed(previous, pet, sortKeys);
  auto& cache = row.cache;
  cache.instanceId = checkedInstanceId(pet);
  cache.revision = ++nextRevision_;
  cache.metadataRevision = catalog.snapshot()->revision;
  cache.metadataDigest = catalog.snapshot()->contentDigest;
  cache.currentSourceVerified = repository_ && repository_->currentSourceVerified();
  ++stats_.rowRevisions;
  if (metadataChanged) {
    cache.display[AttributesColumn] = catalog.resolvedAttributes(pet);
    cache.display[JobsColumn] = catalog.resolvedJobs(pet);
    cache.display[EraColumn] = catalog.resolvedEra(pet);
    cache.attributes = categoryParts(cache.display[AttributesColumn]);
    cache.jobs = categoryParts(cache.display[JobsColumn]);
    cache.era = cache.display[EraColumn];
    cache.attributeVisualKey = catalog.metadataFor(pet).value(QStringLiteral("attributes"))
        .toString().split(QLatin1Char(','), Qt::SkipEmptyParts).value(0).trimmed();
    ++stats_.metadataCachesBuilt;
  }
  if (namesChanged) {
    const int raceId = petRaceId(pet);
    cache.display[DisplayNameColumn] = displayName(pet);
    const QString original = catalog.resolvedOriginalName(pet);
    cache.display[OriginalNameColumn] = original.isEmpty() ? QStringLiteral("待确认") : original;
    cache.nameToolTip = original.isEmpty() || original == cache.display[0] ? cache.display[0]
        : QStringLiteral("皮肤/当前名称：%1\n原名：%2").arg(cache.display[0], original);
    cache.identityText = QStringLiteral("%1 %2").arg(cache.instanceId).arg(raceId);
    cache.search = preparePetSearchIndex(
        {cache.display[0], original, catalog.petName(raceId), pet.value(QStringLiteral("customName")).toString()},
        {QString::number(cache.instanceId), QString::number(raceId)});
    cache.displayedNameSearch[0] = preparePetSearchText(cache.display[0]);
    cache.displayedNameSearch[1] = preparePetSearchText(cache.display[1]);
    ++stats_.searchIndexesBuilt;
  }
  if (powerChanged) {
    cache.display[BattlePowerColumn] = battlePowerText(facts ? facts->facts.battlePower : PetBattlePowerState{});
    cache.powerFromFacts = bool(facts);
    if (facts) {
      ++stats_.preparedFactsConsumed;
      const auto& power = facts->facts.battlePower;
      const auto number = [](bool known, int value) { return known ? QString::number(value) : QStringLiteral("—"); };
      cache.powerToolTip = QStringLiteral("表格：本地持有可达战斗力 / 官方极限战斗力\n"
          "本地持有可达值按实际槽位等级，计入本精灵已装备与背包中可合法搭配的星神。\n"
          "官方返回当前：%1\n官方极限：%2\n本地持有可达：%3\n至高战斗力：%4")
          .arg(number(power.hasServerCurrent, power.serverCurrent), number(power.hasExtreme, power.extreme),
               number(power.hasCurrent && power.currentLocallyCalculated, power.current), number(power.hasHighest, power.highest));
      if (power.completionKnown && power.isHighest) cache.powerToolTip += QStringLiteral("\n已具备至高培养条件");
      else if (power.highestGapKnown) cache.powerToolTip += QStringLiteral("\n距至高尚缺战斗力：%1").arg(power.highestGap);
      else cache.powerToolTip += QStringLiteral("\n至高差距尚不能完整计算");
      if (!power.completionKnown) cache.powerToolTip += QStringLiteral("\n培养条件尚未齐全，暂不能判定升无可升");
      if (!facts->facts.asset.observationVerified || !cache.currentSourceVerified)
        cache.powerToolTip += QStringLiteral("\n数据来自缓存或只读观察");
    } else {
      ++stats_.preparedFactsUnavailable;
      cache.powerToolTip = repository_ && repository_->recordVersion(cache.instanceId).complete
          ? QStringLiteral("当前详情版本的培养派生结果尚未就绪")
          : QStringLiteral("详情尚不完整，培养战力待确认");
    }
  }
  if (keysChanged) {
    const qint64 position = nonnegativeInteger(pet.value(QStringLiteral("_position")), INT_MAX);
    cache.sortKeys[0] = {position < 0 ? INT_MAX : position, true};
    const qint64 power = facts && facts->facts.battlePower.hasCurrent && facts->facts.battlePower.currentLocallyCalculated
        ? facts->facts.battlePower.current : -1;
    const qint64 extreme = facts && facts->facts.battlePower.hasExtreme ? facts->facts.battlePower.extreme : -1;
    cache.sortKeys[1] = {power, power >= 0};
    cache.sortKeys[2] = {extreme, extreme >= 0};
    cache.sortKeys[3] = {petRaceId(pet), petRaceId(pet) > 0};
    const QJsonValue obtained = pet.value(QStringLiteral("gd"));
    qint64 time = nonnegativeInteger(obtained);
    if (time <= 0 && obtained.isString()) {
      const QDateTime parsed = QDateTime::fromString(obtained.toString(), Qt::ISODate);
      time = parsed.isValid() ? parsed.toMSecsSinceEpoch() : -1;
    }
    cache.sortKeys[4] = {time, time > 0};
    ++stats_.sortKeysBuilt;
  }
  cache.display[LevelColumn] = pet.value(QStringLiteral("lv")).toVariant().toString();
  cache.display[DeployedColumn] = location_ == Location::Backpack ? PetMovePolicy::deploymentText(pet)
                                                    : positionText(pet, location_);
  cache.display[BackpackPositionColumn] = positionText(pet, location_);
  cache.deployed = location_ == Location::Backpack && cache.display[DeployedColumn] == QStringLiteral("是");
  row.pet = pet;
  row.factsIdentity = facts;
  if (!changes) return;
  int first = columnCount(), last = -1;
  for (int column = 0; column < columnCount(); ++column)
    if (oldDisplay[column] != cache.display[column]) { first = qMin(first, column); last = column; }
  if (last >= 0) changes->append({row.position, first, last, {Qt::DisplayRole}});
  if (oldTip != cache.nameToolTip) changes->append({row.position, DisplayNameColumn, OriginalNameColumn, {Qt::ToolTipRole}});
  if (oldPowerTip != cache.powerToolTip) changes->append({row.position, BattlePowerColumn, BattlePowerColumn, {Qt::ToolTipRole}});
  if (force || oldIcon != cache.attributeVisualKey)
    changes->append({row.position, AttributesColumn, AttributesColumn, {Qt::DecorationRole}});
  if (oldDeployed != cache.deployed)
    changes->append({row.position, DeployedColumn, DeployedColumn, {Qt::FontRole, Qt::ForegroundRole}});
  if (namesChanged || metadataChanged)
    changes->append({row.position, DisplayNameColumn, OriginalNameColumn, {FilterCacheRevisionRole, NameSearchIndexRole}});
  if (oldSort != cache.sortKeys) changes->append({row.position, 0, 0, {SortCacheRevisionRole}});
  changes->append({row.position, 0, columnCount() - 1, {PetObjectRole}});
}

void PetTableModel::notifyChanges(QList<Change> changes) {
  QPointer<PetTableModel> self(this);
  std::sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) {
    if (a.roles != b.roles) return a.roles < b.roles;
    if (a.first != b.first) return a.first < b.first;
    if (a.last != b.last) return a.last < b.last;
    return a.row < b.row;
  });
  for (qsizetype i = 0; i < changes.size();) {
    const Change& start = changes[i];
    int lastRow = start.row;
    qsizetype next = i + 1;
    while (next < changes.size() && changes[next].roles == start.roles &&
           changes[next].first == start.first && changes[next].last == start.last &&
           changes[next].row == lastRow + 1) lastRow = changes[next++].row;
    emit dataChanged(index(start.row, start.first), index(lastRow, start.last), start.roles);
    if (!self) return;
    i = next;
  }
}

void PetTableModel::reorder(const QList<std::shared_ptr<Row>>& ordered) {
  if (rows_ == ordered) return;
  emit layoutAboutToBeChanged();
  const QModelIndexList previous = persistentIndexList();
  QList<Row*> identities;
  for (const QModelIndex& value : previous) identities.append(rows_.at(value.row()).get());
  rows_ = ordered;
  for (int row = 0; row < rows_.size(); ++row) rows_[row]->position = row;
  ++stats_.positionPasses;
  QModelIndexList current;
  for (qsizetype i = 0; i < previous.size(); ++i)
    current.append(index(identities[i]->position, previous[i].column()));
  changePersistentIndexList(previous, current);
  emit layoutChanged();
}

void PetTableModel::setPets(const QList<QJsonObject>& pets) { applyPets(pets); }

bool PetTableModel::applyPets(const QList<QJsonObject>& pets) {
  Q_ASSERT(QThread::currentThread() == thread());
  cancelPreparation();
  if (pets.size() > INT_MAX) { ++stats_.invalidSnapshots; return false; }
  QSet<qint64> incoming;
  for (const QJsonObject& pet : pets) {
    const qint64 id = checkedInstanceId(pet);
    if (id <= 0 || incoming.contains(id)) { ++stats_.invalidSnapshots; return false; }
    incoming.insert(id);
  }
  QList<std::shared_ptr<Row>> retained, removed, added;
  for (const auto& row : rows_) (incoming.contains(row->cache.instanceId) ? retained : removed).append(row);
  // Pack disjoint removals once, then remove one contiguous tail. Stable row
  // handles keep the ID hash valid without rebuilding it for every mutation.
  if (!removed.isEmpty()) {
    auto grouped = retained;
    grouped.append(removed);
    reorder(grouped);
    const int first = retained.size();
    beginRemoveRows({}, first, rows_.size() - 1);
    for (const auto& row : removed) rowsById_.remove(row->cache.instanceId);
    rows_.remove(first, removed.size());
    endRemoveRows();
  }
  for (const QJsonObject& pet : pets) {
    const qint64 id = checkedInstanceId(pet);
    if (rowsById_.contains(id)) continue;
    auto row = std::make_shared<Row>();
    row->position = rows_.size() + added.size();
    derive(*row, pet, true, nullptr);
    added.append(row);
  }
  if (!added.isEmpty()) {
    beginInsertRows({}, rows_.size(), rows_.size() + added.size() - 1);
    for (const auto& row : added) rowsById_.insert(row->cache.instanceId, row.get());
    rows_.append(added);
    endInsertRows();
  }
  QList<std::shared_ptr<Row>> ordered;
  ordered.reserve(pets.size());
  for (const QJsonObject& pet : pets) ordered.append(rows_.at(rowsById_.value(checkedInstanceId(pet))->position));
  reorder(ordered);
  QList<Change> changes;
  for (int row = 0; row < pets.size(); ++row) derive(*rows_[row], pets[row], false, &changes);
  notifyChanges(changes);
  appliedMetadata_ = metadataSnapshot_;
  return true;
}

bool PetTableModel::applyChanges(const QList<QJsonObject>& upserts, const QList<qint64>& removedIds) {
  Q_ASSERT(QThread::currentThread() == thread());
  QSet<qint64> seen;
  bool structural = !removedIds.isEmpty();
  for (const QJsonObject& pet : upserts) {
    const qint64 id = checkedInstanceId(pet);
    if (id <= 0 || seen.contains(id)) { ++stats_.invalidSnapshots; return false; }
    seen.insert(id);
    structural |= !rowsById_.contains(id);
  }
  if (!structural) {
    QList<Change> changes;
    for (const QJsonObject& pet : upserts) {
      const qint64 id = checkedInstanceId(pet);
      if (preparationRunning_) pendingOverrides_.insert(id, repository_ ? PetFactsUi::rowSummary(pet) : pet);
      derive(*rowsById_.value(id), pet, false, &changes);
    }
    notifyChanges(changes);
    return true;
  }
  QSet<qint64> removed(removedIds.begin(), removedIds.end());
  QList<QJsonObject> next;
  QHash<qint64, qsizetype> positions;
  for (const auto& row : rows_) {
    if (removed.contains(row->cache.instanceId)) continue;
    positions.insert(row->cache.instanceId, next.size());
    next.append(row->pet);
  }
  for (const QJsonObject& pet : upserts) {
    const qint64 id = checkedInstanceId(pet);
    const auto existing = positions.constFind(id);
    if (existing == positions.cend()) { positions.insert(id, next.size()); next.append(pet); }
    else next[*existing] = pet;
  }
  return applyPets(next);
}

bool PetTableModel::updatePet(const QJsonObject& pet) {
  Q_ASSERT(QThread::currentThread() == thread());
  const qint64 id = checkedInstanceId(pet);
  if (id > 0 && preparationRunning_) pendingOverrides_.insert(id, repository_ ? PetFactsUi::rowSummary(pet) : pet);
  Row* row = rowsById_.value(id);
  if (!row) return false;
  QList<Change> changes;
  derive(*row, pet, false, &changes);
  notifyChanges(changes);
  return true;
}

QJsonObject PetTableModel::petAt(int row) const {
  return row >= 0 && row < rows_.size() ? rows_.at(row)->pet : QJsonObject{};
}
qint64 PetTableModel::instanceIdAt(int row) const {
  const auto* cache = cachedRow(row);
  return cache ? cache->instanceId : 0;
}
int PetTableModel::rowForInstanceId(qint64 id) const {
  ++stats_.idLookups;
  const Row* row = rowsById_.value(id);
  return row ? row->position : -1;
}
PetTableModel::Location PetTableModel::location() const { return location_; }
PetModelCacheStats PetTableModel::cacheStats() const {
  auto result = stats_;
  result.attributeIconEntries = attributeIcons_.totalCost();
  return result;
}
void PetTableModel::setMetadataRevision(quint64 revision) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (revision == metadataRevision_) return;
  if (repository_) {
    const auto metadata = repository_->metadataSnapshot();
    if (metadata && metadata->revision == revision) setMetadataSnapshot(metadata);
    return;
  }
  auto metadata = std::make_shared<PetDetailCatalogSnapshot>(*metadataSnapshot_);
  metadata->revision = revision;
  setMetadataSnapshot(std::move(metadata));
}

void PetTableModel::refreshFacts(qint64 id) {
  if (!repository_ || id <= 0) return;
  QJsonObject summary = location_ == Location::Backpack ? repository_->backpackPet(id) : repository_->warehousePet(id);
  if (summary.isEmpty()) {
    if (const Row* row = rowsById_.value(id)) summary = row->pet;
  }
  if (!summary.isEmpty()) updatePet(summary);
}

bool PetTableModel::preparationRunning() const { return preparationRunning_; }

void PetTableModel::schedulePreparation() {
  if (preparationScheduled_) return;
  preparationScheduled_ = true;
  QTimer::singleShot(0, this, [this] { preparationScheduled_ = false; prepareSlice(); });
}

void PetTableModel::cancelPreparation() {
  ++requestedGeneration_;
  preparation_.reset();
  requestedPets_.clear(); pendingOverrides_.clear();
  if (preparationRunning_) { preparationRunning_ = false; emit preparationStateChanged(false); }
}

void PetTableModel::setPetsAsync(const QList<QJsonObject>& pets) {
  Q_ASSERT(QThread::currentThread() == thread());
  requestedPets_ = pets; pendingOverrides_.clear(); ++requestedGeneration_;
  if (!metadataSnapshot_) metadataSnapshot_ = std::make_shared<PetDetailCatalogSnapshot>();
  if (!preparationRunning_) { preparationRunning_ = true; emit preparationStateChanged(true); }
  schedulePreparation();
}

void PetTableModel::setMetadataSnapshot(std::shared_ptr<const PetDetailCatalogSnapshot> metadata) {
  Q_ASSERT(QThread::currentThread() == thread());
  if (!metadata || metadata == metadataSnapshot_ || (metadataSnapshot_ && metadata->revision < metadataSnapshot_->revision)) return;
  metadataSnapshot_ = std::move(metadata); metadataRevision_ = metadataSnapshot_->revision;
  attributeIcons_.clear();
  if (!preparationRunning_) {
    requestedPets_.clear(); requestedPets_.reserve(rows_.size());
    for (const auto& row : rows_) requestedPets_.append(row->pet);
  }
  ++requestedGeneration_;
  if (!preparationRunning_) { preparationRunning_ = true; emit preparationStateChanged(true); }
  schedulePreparation();
}

void PetTableModel::prepareSlice() {
  if (!preparationRunning_) return;
  QElapsedTimer timer; timer.start();
  ++stats_.preparationBatches;
  const auto measured = [this, &timer] {
    stats_.maximumBatchNanoseconds = qMax(stats_.maximumBatchNanoseconds, timer.nsecsElapsed());
  };
  // Disposal also yields: replacing a half-built 10k snapshot must not destroy
  // all temporary row caches in the admission callback.
  if (preparation_ && preparation_->generation != requestedGeneration_) preparation_->phase = Preparation::Retire;
  if (preparation_ && preparation_->phase == Preparation::Retire) {
    for (int count = 0; count < 128 && timer.elapsed() < 8; ++count) {
      if (!preparation_->prepared.isEmpty()) preparation_->prepared.removeLast();
      if (!preparation_->pets.isEmpty()) preparation_->pets.removeLast();
      if (preparation_->prepared.isEmpty() && preparation_->pets.isEmpty()) break;
    }
    if (preparation_->prepared.isEmpty() && preparation_->pets.isEmpty()) { preparation_.reset(); ++stats_.supersededPreparations; }
    measured(); schedulePreparation(); return;
  }
  if (!preparation_) preparation_ = std::make_unique<Preparation>(requestedGeneration_, requestedPets_, metadataSnapshot_,
                                                                 appliedMetadata_ != metadataSnapshot_);
  auto& job = *preparation_;
  if (job.phase == Preparation::Prepare) {
    for (int count = 0; job.cursor < job.pets.size() && count < 128 && timer.elapsed() < 8; ++count, ++job.cursor) {
      const QJsonObject& input = job.pets.at(job.cursor);
      const qint64 id = checkedInstanceId(input);
      if (id <= 0 || job.ids.contains(id)) {
        ++stats_.invalidSnapshots;
        const auto generation = job.generation;
        preparation_.reset(); requestedPets_.clear(); pendingOverrides_.clear(); preparationRunning_ = false;
        QPointer<PetTableModel> alive(this);
        emit preparationStateChanged(false); if (!alive) return;
        emit preparationFinished(generation, false); if (!alive) return;
        measured(); return;
      }
      job.ids.insert(id);
      auto row = std::make_shared<Row>();
      if (const Row* existing = rowsById_.value(id)) *row = *existing;
      const QJsonObject pet = pendingOverrides_.value(id, input);
      QElapsedTimer atomic; atomic.start();
      derive(*row, pet, job.force || row->cache.instanceId == 0, nullptr, &job.metadata);
      stats_.maximumAtomicNanoseconds = qMax(stats_.maximumAtomicNanoseconds, atomic.nsecsElapsed());
      job.prepared.append(std::move(row)); ++stats_.preparationRows;
    }
    if (job.cursor == job.pets.size()) { job.phase = Preparation::Membership; job.cursor = 0; }
    measured(); schedulePreparation(); return;
  }
  if (job.phase == Preparation::Membership) {
    // Only pointer/index operations remain here, not metadata/power parsing.
    // Their synchronous Qt notification cost is included in the measured slice.
    QList<std::shared_ptr<Row>> retained, removed, added;
    for (const auto& row : rows_) (job.ids.contains(row->cache.instanceId) ? retained : removed).append(row);
    if (!removed.isEmpty()) {
      auto grouped = retained; grouped.append(removed); reorder(grouped);
      beginRemoveRows({}, retained.size(), rows_.size() - 1);
      for (const auto& row : removed) rowsById_.remove(row->cache.instanceId);
      rows_.remove(retained.size(), removed.size()); endRemoveRows();
    }
    for (const auto& row : job.prepared) if (!rowsById_.contains(row->cache.instanceId)) added.append(row);
    if (!added.isEmpty()) {
      beginInsertRows({}, rows_.size(), rows_.size() + added.size() - 1);
      for (const auto& row : added) { row->position = rows_.size(); rowsById_.insert(row->cache.instanceId, row.get()); rows_.append(row); }
      endInsertRows();
    }
    QList<std::shared_ptr<Row>> ordered; ordered.reserve(job.prepared.size());
    for (const auto& row : job.prepared) ordered.append(rows_.at(rowsById_.value(row->cache.instanceId)->position));
    reorder(ordered); job.phase = Preparation::Caches;
    measured(); schedulePreparation(); return;
  }
  QList<Change> changes;
  for (int count = 0; job.cursor < job.prepared.size() && count < 128 && timer.elapsed() < 8; ++count, ++job.cursor) {
    auto& prepared = *job.prepared[job.cursor];
    Row& row = *rowsById_.value(prepared.cache.instanceId);
    const auto override = pendingOverrides_.constFind(prepared.cache.instanceId);
    // Facts may change without any summary JSON changing. Revalidate the
    // frozen metadata/current record at commit, even with no raw override.
    derive(prepared, override != pendingOverrides_.cend() ? override.value() : prepared.pet,
           false, &row == &prepared ? &changes : nullptr, &job.metadata);
    if (&row == &prepared || (row.pet == prepared.pet && row.cache.revision == prepared.cache.revision)) continue;
    row.pet = prepared.pet; row.cache = prepared.cache; row.factsIdentity = prepared.factsIdentity;
    changes.append({row.position, 0, columnCount() - 1,
        {Qt::DisplayRole, Qt::ToolTipRole, Qt::DecorationRole, Qt::FontRole, Qt::ForegroundRole,
         PetObjectRole, FilterCacheRevisionRole, SortCacheRevisionRole, NameSearchIndexRole}});
  }
  const quint64 committingGeneration = job.generation;
  QPointer<PetTableModel> self(this);
  notifyChanges(std::move(changes));
  if (!self) return;
  if (!preparation_ || preparation_->generation != committingGeneration || requestedGeneration_ != committingGeneration) {
    measured(); schedulePreparation(); return;
  }
  if (job.cursor == job.prepared.size()) {
    const auto generation = job.generation;
    appliedMetadata_ = job.metadata.snapshot(); ++stats_.completedPreparations;
    preparation_.reset(); requestedPets_.clear(); pendingOverrides_.clear(); preparationRunning_ = false;
    QPointer<PetTableModel> alive(this);
    emit preparationStateChanged(false); if (!alive) return;
    emit preparationFinished(generation, true); if (!alive) return;
    measured(); return;
  }
  measured(); schedulePreparation();
}
