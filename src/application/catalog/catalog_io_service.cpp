#include "catalog_io_service.h"
#include "pet_detail_catalog.h"
#include "pet_skill_catalog.h"
#include "routine_overview_catalog.h"
#include "shop_exchange_catalog.h"
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
struct Request {
  quint64 id = 0;
  CatalogKind kind = CatalogKind::Shop;
  CatalogRequestMode mode = CatalogRequestMode::Reload;
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
                   int maximumNodes = 200000, qint64 maximumStringUnits = 4 * 1024 * 1024) {
  if (depth > 64 || ++*nodes > maximumNodes) return false;
  if (value.isString()) *stringUnits += value.toString().size();
  if (*stringUnits > maximumStringUnits) return false;
  if (value.isArray()) {
    const auto array = value.toArray();
    for (const auto& item : array)
      if (!boundedObject(item, nodes, stringUnits, depth + 1, maximumNodes, maximumStringUnits)) return false;
  } else if (value.isObject()) {
    const auto object = value.toObject();
    for (auto item = object.begin(); item != object.end(); ++item) {
      *stringUnits += item.key().size();
      if (!boundedObject(item.value(), nodes, stringUnits, depth + 1, maximumNodes, maximumStringUnits)) return false;
    }
  }
  return true;
}

QString officialShopUrl(const QString& version) {
  return QStringLiteral("https://aoqi.100bt.com/play/newactivityext/newact20260313/"
                        "storeexchangeframework/storeexchangeframework~%1.swf").arg(version);
}
QDate shopVersionDate(const QString& version) {
  static const QRegularExpression digits(QStringLiteral("^[0-9]{8,20}$"));
  if (!digits.match(version).hasMatch()) return {};
  return QDate::fromString(version.left(8), QStringLiteral("yyyyMMdd"));
}
QDate officialShopDate(const QJsonObject& root) {
  const auto source = root.value(QStringLiteral("source")).toObject();
  const QString version = source.value(QStringLiteral("shopVersion")).toString();
  const QDate date = shopVersionDate(version);
  const QString url = source.value(QStringLiteral("resources")).toObject()
      .value(QStringLiteral("shop")).toObject().value(QStringLiteral("url")).toString();
  if (date.isValid() && url == officialShopUrl(version)) return date;
  // Two precisely identified pre-versioned official catalogs: the old
  // description whitelist and the complete 2026-08-13 SEFConfig import.
  // Metadata/JSON whitespace are irrelevant; a user's changed catalog does
  // not match either hash and is not treated as an old official version.
  const QJsonObject content{{QStringLiteral("protocol"), root.value(QStringLiteral("protocol"))},
                            {QStringLiteral("shops"), root.value(QStringLiteral("shops"))}};
  const QByteArray digest = QCryptographicHash::hash(QJsonDocument(content).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex();
  if (digest == "fa48dbb5ef19ce72bbbc847ee9da53ace95b46d254144d905c1a629d1c517bb4" ||
      digest == "dba434623f89630cf77d34e1a6e34ff853cfcc624bc476b5e83e66041bea41d8")
    return QDate(2026, 8, 13);
  return {};
}
QDate bundledShopDate() {
  // Called on Storage I/O. Keep the baseline tied to the executable resource,
  // not to whichever local overlay happened to be loaded most recently.
  QFile file(QStringLiteral(":/kqpet/shop-exchange-data.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return {};
  const auto root = QJsonDocument::fromJson(file.readAll()).object();
  return officialShopDate(root);
}

QDate officialPetMetadataDate(const QJsonObject& root) {
  const auto source = root.value(QStringLiteral("source")).toObject();
  const QString version = source.value(QStringLiteral("petDictionaryVersion")).toString();
  const QDate date = shopVersionDate(version);
  const QString url = source.value(QStringLiteral("resources")).toObject()
      .value(QStringLiteral("petDictionaryUpdate")).toObject().value(QStringLiteral("url")).toString();
  if (date.isValid() && url == QStringLiteral("https://aoqi.100bt.com/play/pet/petdictionarydataupdate~%1.swf").arg(version))
    return date;
  // Exact pre-versioned 2026-08-13 pet table. Unrecognized user overlays stay
  // loadable; an old official full cache must not replace updated identities.
  const auto pets = root.value(QStringLiteral("pets")).toObject();
  if (!pets.isEmpty() && QCryptographicHash::hash(QJsonDocument(pets).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256).toHex() == "3612ecf10ac6f21876770ee3ae1fa370d122889b96c675328ee25c82f6326613")
    return QDate(2026, 8, 13);
  return {};
}

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
        std::shared_ptr<const PetSkillCatalogSnapshot> currentSkill, QObject* parent)
      : QObject(parent), state_(std::move(state)), request_(std::move(request)),
        options_(std::move(options)), dataRoot_(std::move(dataRoot)), protocol_(std::move(protocol)),
        embedded_(std::move(embedded)), currentDetail_(std::move(currentDetail)),
        embeddedSkill_(std::move(embeddedSkill)), currentSkill_(std::move(currentSkill)), timer_(this) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] { step(); });
    elapsed_.start();
    if (request_.mode == CatalogRequestMode::Reload) {
      files_.append(request_.kind == CatalogKind::PetDetail ? options_.petOverlayPath
          : QDir(dataRoot_).filePath(relativePath(request_.kind)));
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
    return request_.kind == CatalogKind::PetSkill ? 32 * 1024 * 1024 : options_.maximumSourceBytes;
  }
  void finish(StorageStatus status, const QString& error = {}, std::shared_ptr<Candidate> candidate = {}) {
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
    if (fileIndex_ == files_.size()) { prepare(); return false; }
    if (!file_) {
      const QString path = files_.at(fileIndex_);
      if (!safeInput(path)) { finish(StorageStatus::PathRejected, QStringLiteral("目录文件路径包含链接或越界形式")); return false; }
      if (!QFileInfo::exists(path)) { finish(StorageStatus::NotFound, QStringLiteral("尚无本地目录覆盖文件")); return false; }
      file_ = std::make_unique<QFile>(path);
      if (!file_->open(QIODevice::ReadOnly) || file_->size() > sourceLimit()) {
        finish(StorageStatus::ReadFailed, QStringLiteral("目录文件不可读或超过大小限制")); return false;
      }
      openedSize_ = file_->size();
      openedModified_ = QFileInfo(path).lastModified().toUTC();
      updatedAt_ = std::max(updatedAt_, openedModified_);
      bytes_.clear();
    }
    const QByteArray part = file_->read(std::min<qint64>(64 * 1024, sourceLimit() + 1 - bytes_.size()));
    bytes_.append(part);
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
    const int maximumNodes = request_.kind == CatalogKind::PetSkill ? 1000000 : 200000;
    const qint64 maximumStrings = request_.kind == CatalogKind::PetSkill ? 16 * 1024 * 1024 : 4 * 1024 * 1024;
    if (candidate->root.isEmpty() || !boundedObject(candidate->root, &nodes, &strings, 0, maximumNodes, maximumStrings)) {
      finish(StorageStatus::ReadFailed, error.isEmpty() ? QStringLiteral("目录结构超过节点、深度或字符串预算") : error); return;
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
};

CatalogIoService::CatalogIoService(StorageService* storage, CatalogIoOptions options, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()) {
  qRegisterMetaType<CatalogKind>();
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
quint64 CatalogIoService::requestReload(CatalogKind kind) { return request(kind, CatalogRequestMode::Reload); }
quint64 CatalogIoService::requestOfficialUpdate(CatalogKind kind) { return request(kind, CatalogRequestMode::OfficialUpdate); }
quint64 CatalogIoService::request(CatalogKind kind, CatalogRequestMode mode) {
  if (!impl_->storage || impl_->state->closing.load() ||
      (kind != CatalogKind::Shop && kind != CatalogKind::Routine && kind != CatalogKind::PetDetail &&
       kind != CatalogKind::PetSkill) ||
      (mode == CatalogRequestMode::OfficialUpdate &&
       (kind == CatalogKind::PetDetail || kind == CatalogKind::PetSkill))) return 0;
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
  impl_->submitted = impl_->storage->postAuxiliary(
      [state, active, options, dataRoot, protocol, embedded, currentDetail, embeddedSkill, currentSkill](QObject* ioRoot) {
    new IoJob(state, active, options, dataRoot, protocol, embedded, currentDetail,
              embeddedSkill, currentSkill, ioRoot);
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
  if (candidate->shop) { revision = candidate->shop->revision; ShopExchangeCatalog::instance().publish(candidate->shop); }
  else if (candidate->routine) { revision = candidate->routine->revision; RoutineOverviewCatalog::instance().publish(candidate->routine); }
  else if (candidate->detail) { revision = candidate->detail->revision; PetDetailCatalog::instance().publish(candidate->detail); }
  else if (candidate->skill) { revision = candidate->skill->revision; PetSkillCatalog::instance().publish(candidate->skill); }
  emit catalogUpdated(candidate->request.kind, revision);
}
void CatalogIoService::complete(StorageStatus status, const QString& error) {
  const auto completed = impl_->active;
  impl_->candidate.reset(); impl_->running = false; impl_->submitted = false; impl_->writeId = 0;
  if (!impl_->waiting.empty()) impl_->retry.start(0);
  const QPointer<CatalogIoService> guard(this);
  emit finished(completed.id, completed.kind, status, error);
  if (!guard) return;
  emit stateChanged();
}
