#include "catalog_io_service.h"
#include "catalog_legacy_compatibility.h"
#include "battle_replay_catalog_pack.h"
#include "battle_replay_catalog_validation.h"
#include "pet_detail_catalog.h"
#include "pet_skill_catalog.h"
#include "routine_overview_catalog.h"
#include "shop_exchange_catalog.h"
#include "public_dataset_store.h"
#include "protocol/protocol_transport.h"

#include <QDateTime>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <limits>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace CatalogIoInternal {
enum PublicOperation { Legacy = 0, ReloadDataset, ApplyDataset, RollbackDataset, LoadDatasetReplay };
struct Request {
  quint64 id = 0;
  CatalogKind kind = CatalogKind::Shop;
  CatalogRequestMode mode = CatalogRequestMode::Reload;
  int operation = Legacy;
  QString attempt;
  std::shared_ptr<std::atomic_bool> cancelled = std::make_shared<std::atomic_bool>(false);
};
struct Candidate {
  Request request;
  StorageStatus status = StorageStatus::ReadFailed;
  QString error;
  QJsonObject root;
  std::shared_ptr<const ShopCatalogSnapshot> shop;
  std::shared_ptr<const RoutineCatalogSnapshot> routine;
  std::shared_ptr<const PetDetailCatalogSnapshot> detail;
  std::shared_ptr<const PetSkillCatalogSnapshot> skill;
  std::shared_ptr<BattleReplayCatalogSnapshot> replay;
  PublicDatasetResult dataset;
  bool acknowledgement = false;
  quint64 scanned = 0;
  qint64 maximumSliceUs = 0;
};
struct State {
  QMutex mutex;
  CatalogIoService* receiver = nullptr;
  std::atomic_bool closing{false};
  void deliver(std::shared_ptr<Candidate> value) {
    QMutexLocker guard(&mutex);
    if (!receiver || closing.load()) return;
    CatalogIoService* target = receiver;
    QMetaObject::invokeMethod(target, [target, value = std::move(value)] { target->receive(value); }, Qt::QueuedConnection);
  }
};

namespace {
QString relativePath(CatalogKind kind) {
  if (kind == CatalogKind::BattleReplay) return QStringLiteral("catalog/battle-replay-catalog.json");
  if (kind == CatalogKind::Shop) return QStringLiteral("catalog/shop-exchange-data.json");
  if (kind == CatalogKind::Routine) return QStringLiteral("catalog/routine-overview.json");
  if (kind == CatalogKind::PetSkill) return QStringLiteral("catalog/pet-skill-data.json");
  return QStringLiteral("catalog/pet-detail-data.json");
}
bool reparse(const QString& path) {
#ifdef Q_OS_WIN
  const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return QFileInfo(path).isSymLink();
#endif
}
bool safeInput(const QString& path) {
  if (!QDir::isAbsolutePath(path) || path.size() > 32767 || path.contains(QChar::Null)) return false;
  QString current = QDir::cleanPath(path);
  for (;;) {
    if (reparse(current)) return false;
    const QString parent = QFileInfo(current).absolutePath();
    if (parent == current) return true;
    current = parent;
  }
}
bool boundedObject(const QJsonValue& value, int* nodes, qint64* stringUnits, int depth = 0,
                   int maximumNodes = 200000, qint64 maximumStringUnits = 4 * 1024 * 1024,
                   const std::atomic_bool* cancelled = nullptr) {
  if (depth > 64 || ++*nodes > maximumNodes) return false;
  if (cancelled && ((*nodes & 4095) == 0) && cancelled->load()) return false;
  if (value.isString()) *stringUnits += value.toString().size();
  if (*stringUnits > maximumStringUnits) return false;
  if (value.isArray()) {
    const auto array = value.toArray();
    for (const auto& item : array)
      if (!boundedObject(item, nodes, stringUnits, depth + 1, maximumNodes, maximumStringUnits, cancelled)) return false;
  } else if (value.isObject()) {
    const auto object = value.toObject();
    for (auto item = object.begin(); item != object.end(); ++item) {
      *stringUnits += item.key().size();
      if (!boundedObject(item.value(), nodes, stringUnits, depth + 1, maximumNodes, maximumStringUnits, cancelled)) return false;
    }
  }
  return true;
}

using namespace CatalogLegacyCompatibility;

// Created and destroyed only under the existing Storage I/O root. Directory
// traversal enumerates every entry, so a name filter cannot hide an unbounded
// traversal inside one hasNext(). Reads yield every 64 KiB.
class IoJob final : public QObject {
public:
  IoJob(std::shared_ptr<State> state, Request request, CatalogIoOptions options,
        QString dataRoot, QJsonObject protocol,
        std::shared_ptr<const PetDetailCatalogSnapshot> embedded,
        std::shared_ptr<const PetDetailCatalogSnapshot> currentDetail,
        std::shared_ptr<const PetSkillCatalogSnapshot> embeddedSkill,
        std::shared_ptr<const PetSkillCatalogSnapshot> currentSkill, bool replayFallbackAllowed, QObject* parent)
      : QObject(parent), state_(std::move(state)), request_(std::move(request)),
        options_(std::move(options)), dataRoot_(std::move(dataRoot)), protocol_(std::move(protocol)),
        embedded_(std::move(embedded)), currentDetail_(std::move(currentDetail)),
        embeddedSkill_(std::move(embeddedSkill)), currentSkill_(std::move(currentSkill)), timer_(this),
        replayFallbackAllowed_(replayFallbackAllowed) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { step(); });
    elapsed_.start();
    if (request_.mode == CatalogRequestMode::Reload) {
      QString path = request_.kind == CatalogKind::PetDetail ? options_.petOverlayPath
          : QDir(dataRoot_).filePath(relativePath(request_.kind));
      if (request_.kind == CatalogKind::BattleReplay) {
        const QString packed = QDir(dataRoot_).filePath(QStringLiteral("catalog/battle-replay-catalog.brpack"));
        const QFileInfo info(packed);
        // A broken link/reparse point is still a present but invalid preferred
        // package. Do not quietly fall back to a potentially stale JSON file.
        packedReplay_ = info.exists() || info.isSymLink() || reparse(packed);
        if (packedReplay_) path = packed;
      }
      files_.append(path);
      scanning_ = false;
    } else {
      directories_.append(options_.officialRoot);
      directoryBytes_ = options_.officialRoot.size() * 2 + 128;
    }
    timer_.start(0);
  }
private:
  bool cancelled() const { return state_->closing.load() || request_.cancelled->load(); }
  qint64 sourceLimit() const {
    // The current complete replay catalog is 94 MB / 3.96M nodes / 58.66M
    // UTF-16 units. Its own budget must not widen unrelated catalog limits.
    if (request_.kind == CatalogKind::BattleReplay) return packedReplay_
        ? BattleReplayCatalogPack::MaximumPackBytes : BattleReplayCatalogPack::MaximumJsonBytes;
    return request_.kind == CatalogKind::PetSkill ? 32 * 1024 * 1024 : options_.maximumSourceBytes;
  }
  void finish(StorageStatus status, const QString& error = {}, std::shared_ptr<Candidate> candidate = {}) {
    if (request_.kind == CatalogKind::BattleReplay && replayFallbackAllowed_ && !embeddedReplay_ &&
        !cancelled() && (status == StorageStatus::NotFound || status == StorageStatus::ReadFailed ||
                         status == StorageStatus::PathRejected)) {
      embeddedReplay_ = true;
      if (status != StorageStatus::NotFound)
        replayWarning_ = QStringLiteral("工作台回放资料读取失败，已改用内置资料：%1").arg(error);
      file_.reset(); bytes_.clear(); contents_.clear(); fileIndex_ = 0;
      replayDecoder_.reset(); replayHash_.reset(); replayDigest_.clear();
      packedReplay_ = false; replayDecoded_ = false;
      files_ = {QStringLiteral(":/kqpet/battle-replay-catalog.json")};
      updatedAt_ = {}; openedModified_ = {}; openedSize_ = 0;
      timer_.start(0);
      return;
    }
    if (!candidate) candidate = std::make_shared<Candidate>();
    candidate->request = request_;
    candidate->status = status;
    candidate->error = error;
    candidate->scanned = scanned_;
    candidate->maximumSliceUs = std::max(maximumSliceUs_, slice_.isValid() ? slice_.nsecsElapsed() / 1000 : 0);
    state_->deliver(std::move(candidate));
    deleteLater();
  }
  void consider(const QFileInfo& info) {
    const QString name = info.fileName();
    if (request_.kind == CatalogKind::Shop) {
      if (name != QStringLiteral("SEFConfig.as") ||
          !info.filePath().contains(QStringLiteral("storeexchangeframework"), Qt::CaseInsensitive)) return;
    } else if (name != QStringLiteral("DiamondTaskConfig.as") && name != QStringLiteral("CommonHudConfig.as") &&
               name != QStringLiteral("RedPointConfig.as")) return;
    const QString path = info.filePath();
    static const QRegularExpression expression(QStringLiteral("~(\\d+)_decomp"));
    const auto match = expression.match(path);
    const quint64 version = match.hasMatch() ? match.captured(1).toULongLong() : 0;
    const auto previous = best_.value(name);
    if (previous.path.isEmpty() || version > previous.version ||
        (version == previous.version && (info.lastModified() > previous.modified ||
         (info.lastModified() == previous.modified && path > previous.path))))
      best_.insert(name, {path, version, info.lastModified().toUTC()});
  }
  bool scanStep() {
    if (elapsed_.elapsed() > options_.maximumScanMilliseconds) {
      finish(StorageStatus::ReadFailed, QStringLiteral("官方目录扫描超过时限，保留旧目录")); return false;
    }
    if (!iterator_) {
      if (directories_.isEmpty()) {
        const QStringList names = request_.kind == CatalogKind::Shop ? QStringList{QStringLiteral("SEFConfig.as")}
            : QStringList{QStringLiteral("DiamondTaskConfig.as"), QStringLiteral("CommonHudConfig.as"), QStringLiteral("RedPointConfig.as")};
        for (const QString& name : names) {
          if (best_.value(name).path.isEmpty()) { finish(StorageStatus::NotFound, QStringLiteral("官方目录缺少 %1").arg(name)); return false; }
          files_.append(best_.value(name).path);
        }
        scanning_ = false;
        return true;
      }
      const QString directory = directories_.takeLast();
      directoryBytes_ -= directory.size() * 2 + 128;
      if (!safeInput(directory) || !QFileInfo(directory).isDir()) {
        finish(StorageStatus::PathRejected, QStringLiteral("官方目录不存在或包含链接路径")); return false;
      }
      iterator_ = std::make_unique<QDirIterator>(directory, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    }
    if (!iterator_->hasNext()) { iterator_.reset(); return true; }
    iterator_->next();
    if (++scanned_ > static_cast<quint64>(options_.maximumScanEntries)) {
      finish(StorageStatus::ReadFailed, QStringLiteral("官方目录条目超过扫描上限，未发布不完整结果")); return false;
    }
    const QFileInfo info = iterator_->fileInfo();
    if (reparse(info.filePath())) return true;
    if (info.isDir()) {
      const qint64 directoryBytes = info.filePath().size() * 2 + 128;
      if (directories_.size() >= options_.maximumPendingDirectories || directoryBytes > 2 * 1024 * 1024 - directoryBytes_) {
        finish(StorageStatus::ReadFailed, QStringLiteral("官方目录待扫描目录数量或路径内存超过上限")); return false;
      }
      directories_.append(info.filePath());
      directoryBytes_ += directoryBytes;
    } else if (info.isFile()) consider(info);
    return true;
  }
  bool readStep() {
    if (fileIndex_ == files_.size()) {
      if (packedReplay_ && !replayDecoded_) {
        if (!replayDecoder_) {
          replayDecoder_ = std::make_unique<BattleReplayCatalogPack::Decoder>(contents_.takeFirst());
          return true;
        }
        const auto result = replayDecoder_->step();
        if (result == BattleReplayCatalogPack::StepResult::Failed) {
          finish(StorageStatus::ReadFailed, replayDecoder_->error()); return false;
        }
        if (result == BattleReplayCatalogPack::StepResult::More) return true;
        replayDigest_ = replayDecoder_->sha256();
        contents_.append(replayDecoder_->takeJson());
        replayDecoder_.reset();
        replayDecoded_ = true;
        return true;
      }
      prepare(); return false;
    }
    if (!file_) {
      const QString path = files_.at(fileIndex_);
      if (!(embeddedReplay_ && path == QStringLiteral(":/kqpet/battle-replay-catalog.json")) && !safeInput(path)) {
        finish(StorageStatus::PathRejected, QStringLiteral("目录文件路径包含链接或越界形式")); return false;
      }
      if (!QFileInfo::exists(path)) { finish(StorageStatus::NotFound, QStringLiteral("尚无本地目录覆盖文件")); return false; }
      file_ = std::make_unique<QFile>(path);
      if (!file_->open(QIODevice::ReadOnly) || file_->size() > sourceLimit()) {
        finish(StorageStatus::ReadFailed, QStringLiteral("目录文件不可读或超过大小限制")); return false;
      }
      openedSize_ = file_->size();
      openedModified_ = QFileInfo(path).lastModified().toUTC();
      updatedAt_ = std::max(updatedAt_, openedModified_);
      bytes_.clear();
      if (request_.kind == CatalogKind::BattleReplay && !packedReplay_)
        replayHash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    }
    const QByteArray part = file_->read(std::min<qint64>(64 * 1024, sourceLimit() + 1 - bytes_.size()));
    bytes_.append(part);
    if (replayHash_) replayHash_->addData(part);
    if (file_->error() != QFileDevice::NoError || bytes_.size() > sourceLimit() ||
        (part.isEmpty() && !file_->atEnd())) {
      finish(StorageStatus::ReadFailed, QStringLiteral("目录文件读取失败或读取期间增长超限")); return false;
    }
    if (file_->atEnd()) {
      const QFileInfo current(file_->fileName());
      if (file_->size() != openedSize_ || current.size() != openedSize_ ||
          current.lastModified().toUTC() != openedModified_) {
        finish(StorageStatus::ReadFailed, QStringLiteral("目录文件在读取期间发生变化，请重新更新")); return false;
      }
      contents_.append(std::move(bytes_)); file_.reset(); ++fileIndex_;
    }
    return true;
  }
  void prepare() {
    if (cancelled()) { finish(StorageStatus::Cancelled); return; }
    auto candidate = std::make_shared<Candidate>();
    QString error;
    if (request_.mode == CatalogRequestMode::Reload) {
      QJsonParseError parseError{};
      const auto document = QJsonDocument::fromJson(contents_.value(0), &parseError);
      if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        finish(StorageStatus::ReadFailed, QStringLiteral("目录 JSON 无效，保留旧目录")); return;
      }
      candidate->root = document.object();
    } else if (request_.kind == CatalogKind::Shop) {
      candidate->root = ShopExchangeCatalog::parseOfficialText(QString::fromUtf8(contents_.value(0)), protocol_, &error);
      static const QRegularExpression versionExpression(QStringLiteral("storeexchangeframework~([0-9]+)_decomp"));
      const auto match = versionExpression.match(files_.value(0));
      const QString version = match.captured(1);
      if (!candidate->root.isEmpty() && shopVersionDate(version).isValid()) {
        candidate->root.insert(QStringLiteral("source"), QJsonObject{
            {QStringLiteral("shopVersion"), version},
            {QStringLiteral("configSha256"), QString::fromLatin1(QCryptographicHash::hash(
                contents_.value(0), QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("resources"), QJsonObject{{QStringLiteral("shop"),
                QJsonObject{{QStringLiteral("url"), officialShopUrl(version)}}}}}});
      }
    } else candidate->root = RoutineOverviewCatalog::parseOfficialTexts(QString::fromUtf8(contents_.value(0)),
        QString::fromUtf8(contents_.value(1)), QString::fromUtf8(contents_.value(2)), &error);
    int nodes = 0; qint64 strings = 0;
    const bool replay = request_.kind == CatalogKind::BattleReplay;
    const int maximumNodes = replay ? 6000000 : request_.kind == CatalogKind::PetSkill ? 1000000 : 200000;
    const qint64 maximumStrings = replay ? 96 * 1024 * 1024 : request_.kind == CatalogKind::PetSkill ? 16 * 1024 * 1024 : 4 * 1024 * 1024;
    if (candidate->root.isEmpty() || !boundedObject(candidate->root, &nodes, &strings, 0, maximumNodes, maximumStrings,
                                                   request_.cancelled.get())) {
      if (cancelled()) { finish(StorageStatus::Cancelled); return; }
      finish(StorageStatus::ReadFailed, error.isEmpty() ? QStringLiteral("目录结构超过节点、深度或字符串预算") : error); return;
    }
    if (replay) {
      error = validateBattleReplayCatalog(candidate->root);
      if (!error.isEmpty()) { finish(StorageStatus::ReadFailed, error); return; }
      if (cancelled()) { finish(StorageStatus::Cancelled); return; }
      auto snapshot = std::make_shared<BattleReplayCatalogSnapshot>();
      snapshot->root = candidate->root;
      snapshot->sha256 = QString::fromLatin1((packedReplay_ ? replayDigest_ : replayHash_->result()).toHex());
      snapshot->sourcePath = files_.first();
      snapshot->sourceLabel = embeddedReplay_ ? QStringLiteral("程序内置回放资料")
          : packedReplay_ ? QStringLiteral("工作台共享回放资料（压缩包）") : QStringLiteral("工作台共享回放资料");
      snapshot->warning = replayWarning_;
      snapshot->embedded = embeddedReplay_;
      snapshot->loadedAt = QDateTime::currentDateTimeUtc();
      candidate->replay = std::move(snapshot);
      // Only the immutable parsed data crosses back to Core; release raw bytes
      // on I/O before publication. Full JSON parsing remains one bounded stage.
      contents_.clear();
      finish(StorageStatus::Loaded, {}, candidate);
      return;
    }
    if (request_.kind == CatalogKind::Shop) {
      const QDate incoming = officialShopDate(candidate->root);
      const QDate bundled = bundledShopDate();
      // Version suffixes are random, not a within-day ordering guarantee.
      // Only an explicitly older dated official release is superseded.
      if (incoming.isValid() && bundled.isValid() && incoming < bundled) {
        finish(StorageStatus::Superseded,
            QStringLiteral("本地商店目录版本 %1 早于程序内置版本 %2，继续使用新版；原文件已保留")
                .arg(incoming.toString(Qt::ISODate), bundled.toString(Qt::ISODate)));
        return;
      }
    }
    if (request_.kind == CatalogKind::PetDetail) {
      const QDate incoming = officialPetMetadataDate(candidate->root);
      const QDate current = std::max(officialPetMetadataDate(embedded_->root),
          officialPetMetadataDate(currentDetail_->root));
      if (incoming.isValid() && current.isValid() && incoming < current) {
        finish(StorageStatus::Superseded,
            QStringLiteral("本地精灵目录版本 %1 早于当前版本 %2，继续使用新版；原文件已保留")
                .arg(incoming.toString(Qt::ISODate), current.toString(Qt::ISODate)));
        return;
      }
    }
    QString label = request_.mode == CatalogRequestMode::Reload ? QStringLiteral("本地官方目录缓存") : QStringLiteral("官方解包动态配置");
    if (request_.kind == CatalogKind::Shop && request_.mode == CatalogRequestMode::Reload &&
        !officialShopDate(candidate->root).isValid()) label = QStringLiteral("本地目录（未提供可比较的官方版本）");
    if (request_.kind == CatalogKind::Shop) candidate->shop = ShopExchangeCatalog::prepare(candidate->root, label, updatedAt_, &error);
    else if (request_.kind == CatalogKind::Routine) candidate->routine = RoutineOverviewCatalog::prepare(candidate->root, label, updatedAt_, &error);
    else if (request_.kind == CatalogKind::PetSkill) candidate->skill = PetSkillCatalog::prepare(candidate->root, label, updatedAt_, &error);
    else candidate->detail = PetDetailCatalog::prepareOverlay(embedded_, candidate->root, label, updatedAt_, &error);
    if (!candidate->shop && !candidate->routine && !candidate->detail && !candidate->skill) {
      finish(StorageStatus::ReadFailed, error); return;
    }
    if (cancelled()) { finish(StorageStatus::Cancelled); return; }
    finish(StorageStatus::Loaded, {}, candidate);
  }
  void step() {
    if (cancelled()) { finish(StorageStatus::Cancelled); return; }
    slice_.start();
    try {
      for (int entries = 0; entries < 64 && slice_.elapsed() < 8; ++entries) {
        if (cancelled()) { finish(StorageStatus::Cancelled); return; }
        if (!(scanning_ ? scanStep() : readStep())) return;
      }
    } catch (...) { finish(StorageStatus::ReadFailed, QStringLiteral("目录解析失败，保留上一版本")); return; }
    maximumSliceUs_ = std::max(maximumSliceUs_, slice_.nsecsElapsed() / 1000);
    timer_.start(0);
  }
  struct Found { QString path; quint64 version = 0; QDateTime modified; };
  std::shared_ptr<State> state_;
  Request request_;
  CatalogIoOptions options_;
  QString dataRoot_;
  QJsonObject protocol_;
  std::shared_ptr<const PetDetailCatalogSnapshot> embedded_;
  std::shared_ptr<const PetDetailCatalogSnapshot> currentDetail_;
  std::shared_ptr<const PetSkillCatalogSnapshot> embeddedSkill_;
  std::shared_ptr<const PetSkillCatalogSnapshot> currentSkill_;
  QTimer timer_;
  QElapsedTimer elapsed_, slice_;
  bool scanning_ = true;
  QStringList directories_, files_;
  QHash<QString, Found> best_;
  std::unique_ptr<QDirIterator> iterator_;
  std::unique_ptr<QFile> file_;
  QList<QByteArray> contents_;
  QByteArray bytes_;
  int fileIndex_ = 0;
  quint64 scanned_ = 0;
  qint64 maximumSliceUs_ = 0;
  qint64 directoryBytes_ = 0;
  qint64 openedSize_ = 0;
  QDateTime updatedAt_, openedModified_;
  bool replayFallbackAllowed_ = false, embeddedReplay_ = false;
  bool packedReplay_ = false, replayDecoded_ = false;
  QString replayWarning_;
  QByteArray replayDigest_;
  std::unique_ptr<QCryptographicHash> replayHash_;
  std::unique_ptr<BattleReplayCatalogPack::Decoder> replayDecoder_;
};
} // namespace
} // namespace CatalogIoInternal

using namespace CatalogIoInternal;
struct CatalogIoService::Impl {
  QPointer<StorageService> storage;
  CatalogIoOptions options;
  std::shared_ptr<State> state = std::make_shared<State>();
  std::deque<Request> waiting;
  Request active;
  bool running = false, submitted = false;
  quint64 writeId = 0;
  StorageContext shared;
  std::shared_ptr<Candidate> candidate;
  QTimer retry;
  CatalogIoStats stats;
  quint64 selectionReadId = 0, selectionWriteId = 0;
  bool selectionLoaded = false;
  QSet<QString> selection, pendingSelection;
  QSet<QString> excludedAutomatic, pendingExcludedAutomatic;
  QString selectionMessage;
  BattleReplayCatalogHandle replay;
  PublicDatasetSnapshotHandle publicDataset;
  std::shared_ptr<Candidate> pendingAcknowledgement;
  bool sessionPublished = false;
  int recoveryRetries = 0;
};

CatalogIoService::CatalogIoService(StorageService* storage, CatalogIoOptions options, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  qRegisterMetaType<CatalogKind>();
  qRegisterMetaType<BattleReplayCatalogHandle>();
  qRegisterMetaType<PublicDatasetSnapshotHandle>();
  impl_->storage = storage;
  if (options.officialRoot.isEmpty()) options.officialRoot = qEnvironmentVariable("KQPET_OFFICIAL_UNPACK_ROOT", QStringLiteral("D:/奥奇工程/奥奇传说解包"));
  if (options.petOverlayPath.isEmpty()) options.petOverlayPath = qEnvironmentVariable("KQPET_CATALOG_PATH");
  if (options.petOverlayPath.isEmpty() && storage) options.petOverlayPath = QDir(storage->dataRoot()).filePath(relativePath(CatalogKind::PetDetail));
  options.maximumSourceBytes = std::clamp<qint64>(options.maximumSourceBytes, 1, 16 * 1024 * 1024);
  options.maximumScanEntries = std::clamp(options.maximumScanEntries, 1, 250000);
  options.maximumPendingDirectories = std::clamp(options.maximumPendingDirectories, 1, 4096);
  options.maximumScanMilliseconds = std::clamp(options.maximumScanMilliseconds, 1, 15000);
  impl_->options = std::move(options);
  impl_->state->receiver = this;
  impl_->retry.setSingleShot(true);
  connect(&impl_->retry, &QTimer::timeout, this, [this] { pump(); });
  if (storage) {
    impl_->shared = storage->createSharedContext();
    connect(storage, &StorageService::completed, this, [this](const StorageResult& result) {
      if (impl_->state->closing.load()) return;
      if (result.taskId == impl_->selectionReadId && impl_->selectionReadId) {
        impl_->selectionReadId = 0;
        QJsonParseError error{};
        const auto document = QJsonDocument::fromJson(result.content, &error);
        const auto object = document.object();
        const auto values = object.value(QStringLiteral("selected"));
        const auto excludedValues = object.value(QStringLiteral("excludedAutomatic"));
        const int schema = object.value(QStringLiteral("schema")).toInt();
        bool valid = result.status == StorageStatus::NotFound ||
            (result.status == StorageStatus::Loaded && error.error == QJsonParseError::NoError &&
             (schema == 1 || schema == 2) && values.isArray() && values.toArray().size() <= 20000 &&
             (schema == 1 || (excludedValues.isArray() && excludedValues.toArray().size() <= 20000)));
        QSet<QString> selected, excluded;
        const auto readKeys = [&valid](const QJsonArray& array, QSet<QString>& keys) {
          for (const auto& value : array) {
            if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 160) { valid = false; break; }
            keys.insert(value.toString());
          }
        };
        if (valid && result.status != StorageStatus::NotFound) {
          readKeys(values.toArray(), selected);
          if (schema == 2) readKeys(excludedValues.toArray(), excluded);
          if (selected.intersects(excluded)) valid = false;
        }
        impl_->selectionLoaded = valid;
        impl_->selectionMessage = valid ? QStringLiteral("商店选择已载入；默认项目也可以取消勾选。")
            : QStringLiteral("手动选择读取失败，原文件已保留；请重新读取后再修改。%1").arg(result.error);
        if (valid) { impl_->selection = selected; impl_->excludedAutomatic = excluded; publishShopSelection(); }
        emit shopSelectionChanged();
        return;
      }
      if (result.taskId == impl_->selectionWriteId && impl_->selectionWriteId) {
        impl_->selectionWriteId = 0;
        const bool saved = result.status == StorageStatus::Saved;
        impl_->selectionMessage = saved ? QStringLiteral("手动选择已保存，商店界面已更新。")
            : QStringLiteral("保存失败，仍使用上次的选择。%1").arg(result.error);
        if (saved) { impl_->selection = impl_->pendingSelection; impl_->excludedAutomatic = impl_->pendingExcludedAutomatic; publishShopSelection(); }
        impl_->pendingSelection.clear();
        impl_->pendingExcludedAutomatic.clear();
        emit shopSelectionChanged();
        return;
      }
      if (!impl_->writeId || result.taskId != impl_->writeId) return;
      impl_->writeId = 0;
      if (impl_->active.cancelled->load()) { complete(StorageStatus::Cancelled, QStringLiteral("目录请求已被新请求替代")); return; }
      const QPointer<CatalogIoService> guard(this);
      if (result.status == StorageStatus::Saved) publish(impl_->candidate);
      if (!guard) return;
      complete(result.status, result.error);
    });
  }
  // Resource-only controlled defaults are initialized on Core before any I/O
  // candidate is created, never lazily by a Compute job or a paint callback.
  ShopExchangeCatalog::instance(); RoutineOverviewCatalog::instance();
  PetDetailCatalog::instance(); PetSkillCatalog::instance();
}
CatalogIoService::~CatalogIoService() { close(); }
void CatalogIoService::close() {
  if (impl_->state->closing.exchange(true)) return;
  impl_->retry.stop();
  impl_->active.cancelled->store(true);
  for (auto& request : impl_->waiting) request.cancelled->store(true);
  impl_->waiting.clear();
  QMutexLocker guard(&impl_->state->mutex);
  impl_->state->receiver = nullptr;
}
quint64 CatalogIoService::requestReload(CatalogKind kind) {
  if (impl_->options.publicDatasetMode)
    return requestPublic(kind == CatalogKind::BattleReplay ? LoadDatasetReplay : ReloadDataset);
  return request(kind, CatalogRequestMode::Reload);
}
quint64 CatalogIoService::requestOfficialUpdate(CatalogKind kind) {
  // No legacy AS importer may overwrite the compatibility catalog after the
  // normal application has selected versioned public data.
  return impl_->options.publicDatasetMode ? 0 : request(kind, CatalogRequestMode::OfficialUpdate);
}
BattleReplayCatalogHandle CatalogIoService::battleReplaySnapshot() const { return impl_->replay; }
PublicDatasetSnapshotHandle CatalogIoService::publicDatasetSnapshot() const { return impl_->publicDataset; }
quint64 CatalogIoService::requestPublicReload(const QString& acknowledgementAttempt) {
  return requestPublic(ReloadDataset, acknowledgementAttempt);
}
quint64 CatalogIoService::requestPublicCandidate(const QString& attempt) { return requestPublic(ApplyDataset, attempt); }
quint64 CatalogIoService::requestPublicRollback() { return requestPublic(RollbackDataset); }
bool CatalogIoService::shopSelectionEditable() const {
  return impl_->selectionLoaded && !impl_->selectionReadId && !impl_->selectionWriteId && !impl_->state->closing.load();
}
QString CatalogIoService::shopSelectionMessage() const { return impl_->selectionMessage; }
void CatalogIoService::loadShopSelection() {
  if (!impl_->storage || impl_->state->closing.load() || impl_->selectionLoaded ||
      impl_->selectionReadId || impl_->selectionWriteId) return;
  const auto submitted = impl_->storage->submitRead({impl_->shared,
      QStringLiteral("catalog/shop-selection.json"), 0, 1024 * 1024});
  impl_->selectionReadId = submitted.accepted ? submitted.taskId : 0;
  impl_->selectionMessage = submitted.accepted ? QStringLiteral("正在读取手动选择…")
      : QStringLiteral("暂时无法读取手动选择，请稍后重试。%1").arg(submitted.error);
  emit shopSelectionChanged();
}
void CatalogIoService::saveShopSelection(const QStringList& selected, const QStringList& excludedAutomatic) {
  if (!shopSelectionEditable()) { emit shopSelectionChanged(); return; }
  const auto invalidKeys = [](const QStringList& keys) {
    return keys.size() > 20000 || std::any_of(keys.begin(), keys.end(), [](const QString& key) {
      return key.isEmpty() || key.size() > 160;
    });
  };
  const QSet<QString> selectedKeys(selected.begin(), selected.end());
  const QSet<QString> excludedKeys(excludedAutomatic.begin(), excludedAutomatic.end());
  if (invalidKeys(selected) || invalidKeys(excludedAutomatic) || selectedKeys.intersects(excludedKeys)) {
    impl_->selectionMessage = QStringLiteral("选择内容无效，保留上次设置。");
    emit shopSelectionChanged(); return;
  }
  impl_->pendingSelection = selectedKeys;
  impl_->pendingExcludedAutomatic = excludedKeys;
  QStringList canonical = impl_->pendingSelection.values();
  canonical.sort();
  QStringList canonicalExcluded = excludedKeys.values();
  canonicalExcluded.sort();
  const QJsonObject value{{QStringLiteral("schema"), 2},
      {QStringLiteral("selected"), QJsonArray::fromStringList(canonical)},
      {QStringLiteral("excludedAutomatic"), QJsonArray::fromStringList(canonicalExcluded)}};
  const auto submitted = impl_->storage->submitJsonWrite({impl_->shared,
      QStringLiteral("catalog/shop-selection.json"), nextTransportTaskId(), value, 1024 * 1024, true});
  impl_->selectionWriteId = submitted.accepted ? submitted.taskId : 0;
  impl_->selectionMessage = submitted.accepted ? QStringLiteral("正在保存手动选择…")
      : QStringLiteral("暂时无法保存，仍使用上次的选择。%1").arg(submitted.error);
  emit shopSelectionChanged();
}
void CatalogIoService::publishShopSelection() {
  auto& catalog = ShopExchangeCatalog::instance();
  const auto next = ShopExchangeCatalog::withManualSelection(catalog.snapshot(), impl_->selection, impl_->excludedAutomatic);
  catalog.publish(next);
  if (impl_->publicDataset) {
    auto dataset = std::make_shared<PublicDatasetSnapshot>(*impl_->publicDataset);
    dataset->shop = next;
    impl_->publicDataset = dataset;
    emit publicDatasetChanged(dataset);
  }
  emit catalogUpdated(CatalogKind::Shop, next->revision);
}
quint64 CatalogIoService::requestPublic(int operation, const QString& attempt) {
  if (!impl_->storage || impl_->state->closing.load() || operation < ReloadDataset || operation > LoadDatasetReplay)
    return 0;
  static const QRegularExpression identifier(QStringLiteral("^[a-f0-9]{32}$"));
  if ((operation == ApplyDataset || !attempt.isEmpty()) && !identifier.match(attempt).hasMatch()) return 0;
  if (impl_->waiting.size() >= 16) return 0;
  impl_->options.publicDatasetMode = true;
  Request next;
  next.id = nextTransportTaskId();
  next.operation = operation;
  next.attempt = attempt;
  next.kind = operation == LoadDatasetReplay ? CatalogKind::BattleReplay : CatalogKind::PublicDataset;
  // Publication/rollback operations are ordered. They are never cancelled by
  // a later reload after their disk commit may already have begun.
  impl_->waiting.push_back(next);
  if (!impl_->running) impl_->retry.start(0);
  emit stateChanged();
  return next.id;
}
quint64 CatalogIoService::request(CatalogKind kind, CatalogRequestMode mode) {
  if (!impl_->storage || impl_->state->closing.load() ||
      (kind != CatalogKind::Shop && kind != CatalogKind::Routine && kind != CatalogKind::PetDetail &&
       kind != CatalogKind::PetSkill && kind != CatalogKind::BattleReplay) ||
      (mode == CatalogRequestMode::OfficialUpdate &&
       (kind == CatalogKind::PetDetail || kind == CatalogKind::PetSkill || kind == CatalogKind::BattleReplay))) return 0;
  Request next; next.id = nextTransportTaskId(); next.kind = kind; next.mode = mode;
  QList<Request> superseded;
  for (auto it = impl_->waiting.begin(); it != impl_->waiting.end();) {
    if (it->kind == kind) { it->cancelled->store(true); superseded.append(*it); it = impl_->waiting.erase(it); }
    else ++it;
  }
  if (impl_->running && impl_->active.kind == kind) impl_->active.cancelled->store(true);
  impl_->waiting.push_back(next);
  if (!impl_->running) impl_->retry.start(0);
  const QPointer<CatalogIoService> guard(this);
  for (const auto& old : superseded) {
    emit finished(old.id, old.kind, StorageStatus::Cancelled, QStringLiteral("目录请求已被新请求替代"));
    if (!guard) return next.id;
  }
  emit stateChanged();
  return next.id;
}
CatalogIoStats CatalogIoService::stats() const {
  auto stats = impl_->stats;
  stats.pendingRequests = static_cast<int>(impl_->waiting.size()) + (impl_->running ? 1 : 0);
  stats.pendingWrites = impl_->candidate && impl_->active.mode == CatalogRequestMode::OfficialUpdate ? 1 : 0;
  return stats;
}
void CatalogIoService::pump() {
  if (!impl_->storage || impl_->state->closing.load()) return;
  if (!impl_->running) {
    if (impl_->waiting.empty()) return;
    impl_->active = impl_->waiting.front(); impl_->waiting.pop_front(); impl_->running = true;
  }
  if (impl_->active.operation != Legacy) {
    if (impl_->submitted) return;
    const auto state = impl_->state;
    const auto active = impl_->active;
    const auto root = impl_->storage->dataRoot();
    const auto snapshot = impl_->publicDataset;
    const auto acknowledgement = impl_->pendingAcknowledgement;
    impl_->submitted = impl_->storage->postAuxiliary([state, active, root, snapshot, acknowledgement](QObject*) {
      auto candidate = std::make_shared<Candidate>();
      candidate->request = active;
      PublicDatasetStore store(root, {active.cancelled.get(), {}});
      if (acknowledgement) {
        candidate->acknowledgement = true;
        const auto& prior = acknowledgement->dataset;
        const QString attempt = !active.attempt.isEmpty() ? active.attempt : prior.prepared ? prior.prepared->attempt
            : prior.receipt.value(QStringLiteral("attempt")).toString();
        candidate->dataset = store.markSessionApplied(attempt, prior.snapshot->revision);
      } else {
        auto result = active.operation == ApplyDataset ? store.prepareCandidate(active.attempt)
            : active.operation == RollbackDataset ? store.prepareRollback() : store.prepareCurrent(false);
        if (result.status == PublicDatasetStatus::NotFound &&
            (active.operation == ReloadDataset || active.operation == LoadDatasetReplay)) {
          if (snapshot) {
            result.status = PublicDatasetStatus::Invalid;
            result.error = QStringLiteral("公共资料当前入口丢失，继续保留当前会话的完整资料");
          } else result = store.prepareMigration();
        }
        if (result.ok() && result.prepared) result = store.commitPrepared(result.prepared);
        if (result.ok() && active.operation == LoadDatasetReplay) {
          const auto receipt = result.receipt;
          // Explicit rereads notice another process's complete publication.
          // Only reuse a loaded handle after checking the current revision.
          result = store.loadReplay(snapshot && result.snapshot && snapshot->revision == result.snapshot->revision
              ? snapshot : result.snapshot);
          if (result.receipt.isEmpty()) result.receipt = receipt;
        }
        candidate->dataset = std::move(result);
      }
      candidate->status = candidate->dataset.ok() ? StorageStatus::Loaded
          : candidate->dataset.status == PublicDatasetStatus::Cancelled ? StorageStatus::Cancelled : StorageStatus::ReadFailed;
      candidate->error = candidate->dataset.error;
      state->deliver(std::move(candidate));
    });
    if (!impl_->submitted) {
      if (impl_->storage->state().closing) complete(StorageStatus::Closing, QStringLiteral("存储正在退出"));
      else impl_->retry.start(10);
    }
    emit stateChanged();
    return;
  }
  if (impl_->candidate) {
    if (impl_->writeId) return;
    if (impl_->active.cancelled->load()) { complete(StorageStatus::Cancelled, {}); return; }
    const StoreJsonWrite write{impl_->shared, relativePath(impl_->active.kind), impl_->active.id,
        impl_->candidate->root, impl_->options.maximumSourceBytes, false};
    const auto admitted = impl_->storage->submitJsonWrite(write);
    if (!admitted.accepted && admitted.status == StorageStatus::QueueFull) { impl_->retry.start(10); return; }
    if (!admitted.accepted) { complete(admitted.status, admitted.error); return; }
    impl_->writeId = admitted.taskId;
    emit stateChanged();
    return;
  }
  if (impl_->submitted) return;
  const auto state = impl_->state; const auto active = impl_->active; const auto options = impl_->options;
  const auto dataRoot = impl_->storage->dataRoot();
  const auto protocol = ShopExchangeCatalog::instance().snapshot()->root.value(QStringLiteral("protocol")).toObject();
  const auto embedded = PetDetailCatalog::instance().embeddedSnapshot();
  const auto currentDetail = PetDetailCatalog::instance().snapshot();
  const auto embeddedSkill = PetSkillCatalog::instance().embeddedSnapshot();
  const auto currentSkill = PetSkillCatalog::instance().snapshot();
  const bool replayFallbackAllowed = !impl_->replay;
  impl_->submitted = impl_->storage->postAuxiliary(
      [state, active, options, dataRoot, protocol, embedded, currentDetail, embeddedSkill, currentSkill, replayFallbackAllowed](QObject* ioRoot) {
    new IoJob(state, active, options, dataRoot, protocol, embedded, currentDetail,
              embeddedSkill, currentSkill, replayFallbackAllowed, ioRoot);
  });
  if (!impl_->submitted) {
    if (impl_->storage->state().closing) complete(StorageStatus::Closing, QStringLiteral("存储正在退出"));
    else impl_->retry.start(10);
  }
  emit stateChanged();
}
void CatalogIoService::receive(std::shared_ptr<Candidate> candidate) {
  if (!impl_->running || candidate->request.id != impl_->active.id || impl_->state->closing.load()) return;
  impl_->stats.scannedEntries = candidate->scanned;
  impl_->stats.maximumSliceMicroseconds = std::max(impl_->stats.maximumSliceMicroseconds, candidate->maximumSliceUs);
  if (impl_->active.operation != Legacy) {
    impl_->submitted = false;
    const QPointer<CatalogIoService> guard(this);
    if (candidate->acknowledgement) {
      const bool recorded = candidate->dataset.ok();
      const auto receipt = recorded ? candidate->dataset.receipt : impl_->pendingAcknowledgement->dataset.receipt;
      const bool recoveryNeeded = !recorded && (candidate->dataset.status == PublicDatasetStatus::Conflict ||
                                               candidate->dataset.status == PublicDatasetStatus::Busy);
      if (recoveryNeeded) {
        const bool retry = impl_->recoveryRetries < 3;
        const QString message = retry ? QStringLiteral("资料已保存，正在重新载入当前完整版本")
            : QStringLiteral("资料已保存，当前会话尚未确认最新版本；请重新读取资料");
        if (retry) {
          const int delay = 200 * (1 << impl_->recoveryRetries++);
          // A lock retry still belongs to the same application receipt,
          // including a no-change restore whose pointer keeps an older attempt.
          // A conflict instead reloads whoever actually won publication.
          const auto& prior = impl_->pendingAcknowledgement->dataset;
          const QString acknowledgementAttempt = candidate->dataset.status != PublicDatasetStatus::Busy ? QString{}
              : !candidate->request.attempt.isEmpty() ? candidate->request.attempt
              : prior.prepared ? prior.prepared->attempt : prior.receipt.value(QStringLiteral("attempt")).toString();
          QTimer::singleShot(delay, this, [this, acknowledgementAttempt] { requestPublicReload(acknowledgementAttempt); });
        }
        emit publicDatasetFinished(candidate->request.id, false, message, receipt);
        if (guard) complete(StorageStatus::ReadFailed, message);
        return;
      }
      if (recorded) impl_->recoveryRetries = 0;
      const QString message = recorded ? QStringLiteral("共享资料已在当前会话应用")
          : QStringLiteral("共享资料已在当前会话应用，应用状态记录暂未保存：%1").arg(candidate->error);
      emit publicDatasetFinished(candidate->request.id, true, message, receipt);
      if (!guard) return;
      complete(StorageStatus::Loaded, message);
      return;
    }
    if (!candidate->dataset.ok() || !candidate->dataset.snapshot || !candidate->dataset.snapshot->detail ||
        !candidate->dataset.snapshot->skill || !candidate->dataset.snapshot->shop || !candidate->dataset.snapshot->routine) {
      const QString message = candidate->error.isEmpty() ? QStringLiteral("共享资料未应用，继续使用原有完整版本") : candidate->error;
      emit publicDatasetFinished(candidate->request.id, false, message, candidate->dataset.receipt);
      if (guard) complete(candidate->status, message);
      return;
    }
    publishPublicDataset(candidate->dataset.snapshot);
    if (!guard) return;
    impl_->sessionPublished = true;
    const QString attempt = !candidate->request.attempt.isEmpty() ? candidate->request.attempt : candidate->dataset.prepared ? candidate->dataset.prepared->attempt
        : candidate->dataset.receipt.value(QStringLiteral("attempt")).toString();
    if (!attempt.isEmpty()) {
      impl_->pendingAcknowledgement = candidate;
      pump();
    } else {
      emit publicDatasetFinished(candidate->request.id, true, QStringLiteral("共享资料已在当前会话应用"), candidate->dataset.receipt);
      if (guard) complete(StorageStatus::Loaded, {});
    }
    return;
  }
  if (impl_->active.cancelled->load()) { complete(StorageStatus::Cancelled, QStringLiteral("目录请求已被新请求替代")); return; }
  if (candidate->status != StorageStatus::Loaded) { complete(candidate->status, candidate->error); return; }
  if (impl_->active.mode == CatalogRequestMode::OfficialUpdate) { impl_->candidate = std::move(candidate); pump(); return; }
  const QPointer<CatalogIoService> guard(this);
  publish(candidate);
  if (!guard) return;
  complete(StorageStatus::Loaded, {});
}
void CatalogIoService::publish(const std::shared_ptr<Candidate>& candidate) {
  quint64 revision = 0;
  if (candidate->shop) {
    const auto selected = ShopExchangeCatalog::withManualSelection(candidate->shop, impl_->selection, impl_->excludedAutomatic);
    revision = selected->revision; ShopExchangeCatalog::instance().publish(selected);
  }
  else if (candidate->routine) { revision = candidate->routine->revision; RoutineOverviewCatalog::instance().publish(candidate->routine); }
  else if (candidate->detail) { revision = candidate->detail->revision; PetDetailCatalog::instance().publish(candidate->detail); }
  else if (candidate->skill) { revision = candidate->skill->revision; PetSkillCatalog::instance().publish(candidate->skill); }
  else if (candidate->replay) {
    if (impl_->replay && impl_->replay->sha256 == candidate->replay->sha256 &&
        impl_->replay->sourcePath == candidate->replay->sourcePath) return;
    candidate->replay->revision = impl_->replay ? impl_->replay->revision + 1 : 1;
    impl_->replay = candidate->replay;
    revision = impl_->replay->revision;
  }
  emit catalogUpdated(candidate->request.kind, revision);
}
void CatalogIoService::publishPublicDataset(PublicDatasetSnapshotHandle snapshot) {
  if (!snapshot || !snapshot->detail || !snapshot->skill || !snapshot->shop || !snapshot->routine) return;
  const auto before = impl_->publicDataset;
  auto next = std::make_shared<PublicDatasetSnapshot>(*snapshot);
  const auto changed = [&](const QString& component) {
    return !before || before->components.value(component).sha256 != next->components.value(component).sha256;
  };
  const bool detailChanged = changed(QStringLiteral("pets"));
  const bool skillChanged = changed(QStringLiteral("skills"));
  const bool shopChanged = changed(QStringLiteral("shop"));
  const bool routineChanged = changed(QStringLiteral("routines"));
  const bool replayChanged = changed(QStringLiteral("battleReplay"));
  if (!detailChanged) next->detail = before->detail;
  else {
    auto value = std::make_shared<PetDetailCatalogSnapshot>(*next->detail);
    value->revision = PetDetailCatalog::instance().snapshot()->revision + 1;
    next->detail = value;
  }
  if (!skillChanged) next->skill = before->skill;
  else {
    auto value = std::make_shared<PetSkillCatalogSnapshot>(*next->skill);
    value->revision = PetSkillCatalog::instance().snapshot()->revision + 1;
    next->skill = value;
  }
  if (!shopChanged) next->shop = before->shop;
  else {
    auto selected = ShopExchangeCatalog::withManualSelection(next->shop, impl_->selection, impl_->excludedAutomatic);
    auto value = std::make_shared<ShopCatalogSnapshot>(*selected);
    value->revision = ShopExchangeCatalog::instance().snapshot()->revision + 1;
    next->shop = value;
  }
  if (!routineChanged) next->routine = before->routine;
  else {
    auto value = std::make_shared<RoutineCatalogSnapshot>(*next->routine);
    value->revision = RoutineOverviewCatalog::instance().snapshot()->revision + 1;
    next->routine = value;
  }
  if (!replayChanged && before && before->replay) next->replay = before->replay;
  else if (next->replay) {
    auto value = std::make_shared<BattleReplayCatalogSnapshot>(*next->replay);
    value->revision = impl_->replay ? impl_->replay->revision + 1 : 1;
    next->replay = value;
  }
  // All compatibility accessors are changed within one Core callback before
  // observers run. Normal operations receive this single complete handle.
  PetDetailCatalog::instance().publish(next->detail);
  PetSkillCatalog::instance().publish(next->skill);
  ShopExchangeCatalog::instance().publish(next->shop);
  RoutineOverviewCatalog::instance().publish(next->routine);
  impl_->publicDataset = next;
  impl_->replay = next->replay;
  const QPointer<CatalogIoService> guard(this);
  emit publicDatasetChanged(next);
  if (!guard) return;
  const std::pair<CatalogKind, quint64> changes[] = {
    {CatalogKind::PetDetail, detailChanged ? next->detail->revision : 0},
    {CatalogKind::PetSkill, skillChanged ? next->skill->revision : 0},
    {CatalogKind::Shop, shopChanged ? next->shop->revision : 0},
    {CatalogKind::Routine, routineChanged ? next->routine->revision : 0}};
  for (const auto& item : changes) {
    if (item.second) emit catalogUpdated(item.first, item.second);
    if (!guard) return;
  }
  if (next->replay && (!before || before->replay != next->replay))
    emit catalogUpdated(CatalogKind::BattleReplay, next->replay->revision);
}
void CatalogIoService::complete(StorageStatus status, const QString& error) {
  const auto completed = impl_->active;
  impl_->candidate.reset(); impl_->running = false; impl_->submitted = false; impl_->writeId = 0;
  impl_->pendingAcknowledgement.reset(); impl_->sessionPublished = false;
  if (!impl_->waiting.empty()) impl_->retry.start(0);
  const QPointer<CatalogIoService> guard(this);
  emit finished(completed.id, completed.kind, status, error);
  if (!guard) return;
  emit stateChanged();
}
