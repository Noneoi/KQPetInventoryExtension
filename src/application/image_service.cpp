#include "image_service.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <chrono>
#include <limits>
#include <utility>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace ImageServiceInternal {
struct Ledger {
  QMutex mutex;
  quint64 limit = 0;
  quint64 encodedLimit = 0;
  ImageMemoryUsage usage;
};
enum class ChargeKind { Normal, Encoded, Display };
struct Charge {
  std::shared_ptr<Ledger> ledger;
  quint64 bytes = 0;
  ChargeKind kind = ChargeKind::Normal;
  ~Charge() {
    if (!ledger || !bytes) return;
    QMutexLocker lock(&ledger->mutex);
    ledger->usage.chargedBytes -= bytes;
    if (kind == ChargeKind::Encoded) ledger->usage.encodedBytes -= bytes;
    if (kind == ChargeKind::Display) ledger->usage.displayBytes -= bytes;
  }
};
std::shared_ptr<void> reserve(const std::shared_ptr<Ledger>& ledger, quint64 bytes,
                              ChargeKind kind = ChargeKind::Normal) {
  // Allocate before changing counters: allocation failure cannot leak a charge.
  auto lease = std::make_shared<Charge>();
  QMutexLocker lock(&ledger->mutex);
  auto& use = ledger->usage;
  if (!bytes || bytes > ledger->limit - qMin(ledger->limit, use.chargedBytes) ||
      (kind == ChargeKind::Encoded &&
       bytes > ledger->encodedLimit - qMin(ledger->encodedLimit, use.encodedBytes))) return {};
  lease->ledger = ledger;
  lease->bytes = bytes;
  lease->kind = kind;
  use.chargedBytes += bytes;
  use.peakChargedBytes = qMax(use.peakChargedBytes, use.chargedBytes);
  if (kind == ChargeKind::Encoded) {
    use.encodedBytes += bytes;
    use.peakEncodedBytes = qMax(use.peakEncodedBytes, use.encodedBytes);
  }
  if (kind == ChargeKind::Display) {
    use.displayBytes += bytes;
    use.peakDisplayBytes = qMax(use.peakDisplayBytes, use.displayBytes);
  }
  return lease;
}
}  // namespace ImageServiceInternal

namespace {
using namespace ImageServiceInternal;
constexpr quint64 kReplyBufferBytes = 64 * 1024;
QSize outputSize(const ImageRequest& request) {
  const qreal ratio = std::isfinite(request.devicePixelRatio) ? qBound<qreal>(1, request.devicePixelRatio, 4) : 1;
  return {qBound(1, qRound(qBound(1, request.outputLogicalSize.width(), 1024) * ratio), 1024),
          qBound(1, qRound(qBound(1, request.outputLogicalSize.height(), 1024) * ratio), 1024)};
}
// Resource identity of a request. Render size and device pixel ratio are not
// part of it: the same identity is one source, read and decoded per size.
QString imageSourceIdentity(const ImageRequest& request) {
  return request.attributeId >= 0
      ? QStringLiteral("attribute:%1").arg(request.attributeId)
      : request.stargodId > 0 ? QStringLiteral("stargod:%1").arg(request.stargodId)
                              : request.visualKey;
}
QSize fittedSize(const QSize& input, const QSize& output) {
  const QSize scaled = input.scaled(output, Qt::KeepAspectRatio);
  return {qMax(1, scaled.width()), qMax(1, scaled.height())};
}
bool allowedUrl(const QString& value) {
  const QUrl url(value, QUrl::StrictMode);
  return url.isValid() && (url.scheme() == QStringLiteral("http") ||
      url.scheme() == QStringLiteral("https")) && !url.host().isEmpty() &&
      url.userInfo().isEmpty() && url.fragment().isEmpty();
}
bool safeVisualKey(const QString& key) {
  static const QRegularExpression expression(QStringLiteral("^[A-Za-z0-9_-]{1,96}$"));
  return expression.match(key).hasMatch();
}
// Signatures of the formats the preflight accepts. Used only to confirm that a
// stored file is still an image without decoding it again.
bool supportedImageHeader(const QByteArray& bytes) {
  if (bytes.size() < 8) return false;
  const auto starts = [&bytes](const char* signature, int length) {
    return bytes.size() >= length && bytes.startsWith(QByteArray(signature, length));
  };
  if (starts("\x89PNG\r\n\x1a\n", 8)) return true;
  if (bytes.startsWith("\xff\xd8\xff")) return true;
  if (starts("GIF87a", 6) || starts("GIF89a", 6)) return true;
  if (starts("BM", 2)) return true;
  return starts("RIFF", 4) && bytes.mid(8, 4) == QByteArray("WEBP", 4);
}
bool safeExistingChain(const QString& absolutePath) {  const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
  if (!QDir::isAbsolutePath(clean)) return false;
  QString current = clean;
  for (;;) {
#ifdef Q_OS_WIN
    const DWORD attrs = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(current).utf16()));
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    if (attrs == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    }
#else
    if (QFileInfo(current).isSymLink()) return false;
#endif
    const QString parent = QFileInfo(current).absolutePath();
    if (parent == current || current.isEmpty()) break;
    current = parent;
  }
  return true;
}
QString cachePath(const QString& root, const QString& key) {
  if (root.isEmpty() || !QDir::isAbsolutePath(root) || !safeVisualKey(key)) return {};
  const QString base = QDir::cleanPath(QDir(root).absolutePath());
  const QString relative = QStringLiteral("images/pets/%1.png").arg(ImageService::storageKey(key));
  const QString path = QDir(base).filePath(relative);
  if (!path.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive) ||
      !safeExistingChain(path)) return {};
  return path;
}
QPoint attributePosition(int id) {
  static const QHash<int, QPoint> positions = {
      {0,{225,90}}, {1,{225,90}}, {2,{45,45}}, {3,{225,180}}, {4,{90,135}},
      {5,{90,90}}, {6,{0,90}}, {7,{180,270}}, {8,{45,0}}, {9,{0,180}},
      {10,{0,45}}, {11,{45,135}}, {12,{0,135}}, {13,{180,90}}, {14,{315,45}},
      {15,{225,0}}, {16,{90,45}}, {17,{135,135}}, {18,{45,90}}, {19,{135,180}},
      {20,{135,315}}, {21,{45,225}}, {22,{270,225}}, {23,{225,225}},
      {24,{0,0}}, {25,{135,225}}, {26,{270,135}}, {27,{45,270}}, {28,{270,0}}};
  return positions.value(id, QPoint(225,90));
}
struct Job {
  ImageRequest request;
  QString key;
  QString url;
  QString sourceRevision;
  QString message;
  QStringList localCandidates;
  int localCandidate = 0;
  std::shared_ptr<void> encodedLease;
  std::shared_ptr<void> descriptorLease;
  std::shared_ptr<void> temporaryLease;
  std::shared_ptr<void> outputLease;
  QByteArray bytes; // Destroy storage before its leases.
  quint64 decodeCharge = 0;
  int stage = 0;
  bool cached = false;
  bool forceNetwork = false;
  bool migrate = false;
  bool networkAttempted = false;
  bool refreshChecked = false;
  bool attributeSheet = false;
  // Identity of this job and of the batch that owns its terminal accounting.
  quint64 jobId = 0;
  quint64 batchGeneration = 0;
  bool batch = false;
  // T9: bytes are acquired once per source identity. Every render size of the
  // same source waits on this shared task and then decodes its own size.
  QString sourceKey;
  std::shared_ptr<struct SourceTask> source;
  bool waitingOnSource = false;
  // Set while this job owns a persistence decision others wait for.
  QString persistenceKey;
  std::atomic_bool cancelled{false};
};
using JobPtr = std::shared_ptr<Job>;
using IoDone = std::function<void(JobPtr, bool)>;
using SourceDone = std::function<void(std::shared_ptr<struct SourceTask>, bool)>;

// One in-flight byte acquisition per source identity (visual identity + resource
// version). Consumers join it instead of starting a second read/download/extract
// of the same source; each of them still decodes and scales independently.
struct SourceTask {
  QString key;
  ImageRequest request; // identity, candidate names and refresh flags
  QString url;
  QString sourceRevision;
  QString message;
  QStringList localCandidates;
  int localCandidate = 0;
  QSet<quint64> consumers;
  std::shared_ptr<void> encodedLease;
  std::shared_ptr<void> fetchLease;
  QByteArray bytes;
  bool cached = false;
  bool forceNetwork = false;
  bool migrate = false;
  bool networkAttempted = false;
  bool refreshChecked = false;
  bool attributeSheet = false;
  bool fetching = false;
  bool ready = false;
  // Cancelled only when the last consumer left: one cancelled batch entry must
  // never abort a download another consumer is still waiting for.
  std::atomic_bool cancelled{false};
};
using SourcePtr = std::shared_ptr<SourceTask>;

// Result of one disk-persistence attempt. Always produced for a request that
// asked the disk to hold the image, including every failure path.
struct ImagePersistenceReport {
  ImagePersistence outcome = ImagePersistence::Failed;
  QString message;
  bool metadataSaved = false;
};

// Exists exclusively on the existing I/O thread. Its parent owns shutdown.
class ImageIo final : public QObject {
public:
  explicit ImageIo(QObject* root, ImageServiceOptions options)
      : QObject(root), options_(std::move(options)), network_(this) {}
  ~ImageIo() override { cancel(); }
  void cancel() {
    closing_ = true;
    const auto replies = replies_;
    for (auto* reply : replies) reply->abort();
    const auto processes = processes_;
    for (auto* process : processes) process->kill();
  }
  QHash<QString, QString> readDirectory() {
    QHash<QString, QString> values = options_.verifiedUrls;
    if (values.isEmpty()) {
      QFile file(QStringLiteral(":/kqpet/pet-image-urls.json"));
      if (file.open(QIODevice::ReadOnly) && file.size() <= 16 * 1024 * 1024) {
        const auto object = QJsonDocument::fromJson(file.readAll()).object();
        for (auto entry = object.begin(); entry != object.end(); ++entry)
          if (entry.value().isString()) values.insert(entry.key(), entry.value().toString());
      }
    }
    for (auto it = values.begin(); it != values.end();)
      if (!allowedUrl(it.value())) it = values.erase(it); else ++it;
    const QString indexPath = QDir(options_.dataRoot).filePath(QStringLiteral("catalog/pet-image-index.json"));
    QFile index(indexPath);
    if (safeExistingChain(indexPath) && index.open(QIODevice::ReadOnly) && index.size() <= 16 * 1024 * 1024) {
      const auto root = QJsonDocument::fromJson(index.readAll()).object();
      if (root.value(QStringLiteral("schemaVersion")).toInt() == 1) {
        for (const auto& value : root.value(QStringLiteral("faceIdExceptions")).toArray()) {
          const qint64 id = value.toInteger();
          if (id >= 10000) values.insert(QStringLiteral("exception:%1").arg(id), QStringLiteral("1"));
        }
        for (const QString section : {QStringLiteral("images"), QStringLiteral("faces")}) {
          const auto images = root.value(section).toObject();
          for (auto entry = images.begin(); entry != images.end(); ++entry) {
            const auto item = entry->toObject();
            const QString url = entry->isString() ? entry->toString()
                : item.value(QStringLiteral("url")).toString(item.value(QStringLiteral("swfUrl")).toString());
            if (safeVisualKey(entry.key()) && allowedUrl(url))
            {
              const QString key = (section == QStringLiteral("images") ? QStringLiteral("id:") : QStringLiteral("face:")) + entry.key();
              values.insert(key, url);
              values.insert(QStringLiteral("revision:") + key, item.value(QStringLiteral("revision")).toString(item.value(QStringLiteral("version")).toString()));
            }
          }
        }
      }
    }
    return values;
  }
  QStringList localPaths(const QString& visualKey) const {
    QStringList paths;
    const QString stable = cachePath(options_.dataRoot, visualKey);
    if (stable.isEmpty()) return paths;
    paths.append(stable);
    const QString key = ImageService::storageKey(visualKey);
    const QDir base(QFileInfo(stable).absolutePath());
    const QString oldExact = base.filePath(visualKey + QStringLiteral(".png"));
    if (oldExact != stable && safeExistingChain(oldExact)) paths.append(oldExact);
    const auto appendMatches = [&](const QDir& directory) {
      const QStringList patterns = key == visualKey ? QStringList{key + QStringLiteral(".png")}
          : QStringList{key + QStringLiteral(".png"), key + QStringLiteral("_*.png")};
      const auto files = directory.entryInfoList(patterns, QDir::Files | QDir::NoSymLinks, QDir::Time);
      for (const auto& file : files)
        if (safeExistingChain(file.absoluteFilePath()) && !paths.contains(file.absoluteFilePath())) paths.append(file.absoluteFilePath());
    };
    appendMatches(base);
    const QDir versions(base.filePath(QStringLiteral("versions")));
    if (safeExistingChain(versions.absolutePath())) {
      const auto directories = versions.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Time);
      for (const auto& directory : directories)
        if (safeExistingChain(directory.absoluteFilePath())) appendMatches(QDir(directory.absoluteFilePath()));
    }
    return paths;
  }
  // force records a failure whose consumer side has already gone away: the
  // bytes were fetched and are unusable, so the on-disk backoff must still be
  // written even though the shared task was released before this I/O job ran.
  void rememberFailure(const SourcePtr& source, bool force = false) {
    if (closing_ || (!force && source->cancelled.load()) || !source->networkAttempted ||
        source->request.attributeId >= 0 || source->request.stargodId > 0) return;
    const QString path = cachePath(options_.dataRoot, source->request.visualKey) + QStringLiteral(".failure.json");
    if (!safeExistingChain(path) || !QDir().mkpath(QFileInfo(path).absolutePath())) return;
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("url"), source->url},
        {QStringLiteral("message"), source->message}, {QStringLiteral("attemptedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) file.commit();
  }
  void fetch(const SourcePtr& source, SourceDone done) {
    if (closing_ || source->cancelled.load()) { done(source, false); return; }
    const bool attribute = source->request.attributeId >= 0;
    const bool stargod = source->request.stargodId > 0;
    const QString path = attribute ? QStringLiteral(":/kqpet/attribute-icons.png")
                                   : stargod ? QStringLiteral(":/kqpet/stargod-icons/%1.png").arg(source->request.stargodId)
                                   : cachePath(options_.dataRoot, source->request.visualKey);
    if (source->request.refreshChangedSource && !source->refreshChecked && !attribute && !stargod) {
      source->refreshChecked = true;
      QFile metadata(QFileInfo(path).absolutePath() + QLatin1Char('/') + ImageService::storageKey(source->request.visualKey) + QStringLiteral(".json"));
      if (safeExistingChain(metadata.fileName()) && metadata.open(QIODevice::ReadOnly) && metadata.size() <= 65536) {
        const auto previous = QJsonDocument::fromJson(metadata.readAll()).object();
        const QString oldUrl = previous.value(QStringLiteral("url")).toString();
        const QString oldRevision = previous.value(QStringLiteral("revision")).toString();
        source->forceNetwork = !source->url.isEmpty() && !oldUrl.isEmpty() &&
            (oldUrl != source->url || (!source->sourceRevision.isEmpty() && oldRevision != source->sourceRevision));
      }
    }
    if (source->localCandidates.isEmpty() && !source->forceNetwork) {
      if (attribute || stargod) {
        const QString disk = QDir(options_.dataRoot).filePath(attribute
            ? QStringLiteral("images/attributes/%1.png").arg(source->request.attributeId)
            : QStringLiteral("images/stargods/%1.png").arg(source->request.stargodId));
        if (safeExistingChain(disk)) source->localCandidates.append(disk);
        // The bundled attribute sprite has a fixed, known set. An unknown ID
        // must not silently inherit the normal attribute's picture.
        if (stargod || source->request.attributeId <= 28) source->localCandidates.append(path);
      } else source->localCandidates = localPaths(source->request.visualKey);
    }
    while (!source->forceNetwork && source->localCandidate < source->localCandidates.size()) {
      const QString candidate = source->localCandidates.at(source->localCandidate++);
      source->attributeSheet = attribute && candidate.startsWith(QStringLiteral(":/"));
      QFile file(candidate);
      if (file.open(QIODevice::ReadOnly) && file.size() > 0 &&
          quint64(file.size()) <= options_.encodedImageBytes) {
        source->bytes = file.read(qint64(options_.encodedImageBytes) + 1);
        source->cached = true;
        source->migrate = candidate != path;
        if (!source->bytes.isEmpty() && quint64(source->bytes.size()) <= options_.encodedImageBytes) {
          done(source, true);
          return;
        }
        source->bytes.clear();
      }
    }
    if (attribute || stargod || !allowedUrl(source->url)) {
      source->message = QStringLiteral("本地没有图片；请点击“检查数据更新”补充图片索引"); done(source, false); return;
    }
    QFile previousFailure(path + QStringLiteral(".failure.json"));
    if (!source->request.retry && !source->request.refreshChangedSource && safeExistingChain(previousFailure.fileName()) && previousFailure.open(QIODevice::ReadOnly) && previousFailure.size() <= 8192 &&
        QJsonDocument::fromJson(previousFailure.readAll()).object().value(QStringLiteral("url")).toString() == source->url) {
      source->message = QStringLiteral("上次图片下载失败；右键选择“重新加载精灵图片”可重试"); done(source, false); return;
    }
    source->cached = false;
    source->networkAttempted = true;
    if (QUrl(source->url).path().endsWith(QStringLiteral(".swf"), Qt::CaseInsensitive)) {
      extract(source, std::move(done)); return;
    }
    QNetworkRequest request{QUrl(source->url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(options_.timeoutMilliseconds);
    request.setRawHeader("User-Agent", "KQPetInventory/2");
    auto* reply = network_.get(request);
    reply->setReadBufferSize(kReplyBufferBytes);
    replies_.insert(reply);
    // Reserve once, so QByteArray geometric growth cannot exceed its charge.
    source->bytes.clear();
    source->bytes.reserve(qsizetype(options_.encodedImageBytes));
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(options_.timeoutMilliseconds);
    const auto drain = [this, source, reply]() {
      if (source->cancelled.load() || closing_) { reply->abort(); return; }
      const qint64 available = reply->bytesAvailable();
      if (available > qint64(options_.encodedImageBytes) - source->bytes.size()) {
        reply->setProperty("kqOversize", true);
        reply->abort();
        return;
      }
      if (available > 0) source->bytes.append(reply->read(available));
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply]() {
      bool known = false;
      const qlonglong length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
      if (known && length > qint64(options_.encodedImageBytes)) {
        reply->setProperty("kqOversize", true); reply->abort();
      }
    });
    connect(reply, &QNetworkReply::finished, this, [this, source, reply, done = std::move(done), drain]() {
      drain();
      replies_.remove(reply);
      const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      const bool success = !closing_ && !source->cancelled.load() &&
          reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 &&
          !reply->property("kqOversize").toBool() && !source->bytes.isEmpty();
      if (!success) {
        source->message = status ? QStringLiteral("图片下载失败（HTTP %1）；可右键重试").arg(status)
                             : QStringLiteral("图片下载失败或超时；可右键重试");
        rememberFailure(source);
      }
      reply->deleteLater();
      done(source, success);
    });
  }
  void extract(const SourcePtr& source, SourceDone done) {
    const QString python = QDir(options_.dataRoot).filePath(QStringLiteral("data-tools/python/python.exe"));
    const QString script = QDir(options_.dataRoot).filePath(QStringLiteral("data-tools/scripts/public_data_updater.py"));
    if (!safeExistingChain(python) || !safeExistingChain(script) || !QFile::exists(python) || !QFile::exists(script)) {
      source->message = QStringLiteral("图片工具尚未准备；请先点击“检查数据更新”"); done(source, false); return;
    }
    auto* process = new QProcess(this);
#ifdef Q_OS_WIN
    process->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
    processes_.insert(process);
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PYTHONUTF8"), QStringLiteral("1"));
    environment.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    process->setProcessEnvironment(environment);
    auto* timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    timeout->start(180000);
    connect(process, &QProcess::readyReadStandardOutput, process, [process] { process->readAllStandardOutput(); });
    auto finished = std::make_shared<bool>(false);
    const auto complete = [this, process, source, done = std::move(done), finished](bool success) {
      if (std::exchange(*finished, true)) return;
      processes_.remove(process);
      process->deleteLater();
      if (success && !closing_ && !source->cancelled.load()) {
        const QString path = cachePath(options_.dataRoot, source->request.visualKey);
        QFile file(path);
        if (safeExistingChain(path) && file.open(QIODevice::ReadOnly) && file.size() > 0 && quint64(file.size()) <= options_.encodedImageBytes) {
          source->bytes = file.read(qint64(options_.encodedImageBytes) + 1);
          if (!source->bytes.isEmpty() && quint64(source->bytes.size()) <= options_.encodedImageBytes) {
            source->cached = true; source->forceNetwork = true; source->migrate = false; done(source, true); return;
          }
        }
      }
      source->message = QStringLiteral("官方图片下载或提取失败；可右键重试");
      rememberFailure(source);
      done(source, false);
    };
    connect(process, &QProcess::finished, process, [complete](int code, QProcess::ExitStatus status) {
      complete(code == 0 && status == QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, process, [complete](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) complete(false);
    });
    process->start(python, {script, QStringLiteral("--data-root"), options_.dataRoot,
        QStringLiteral("--extract-image"), QStringLiteral("--visual-key"), ImageService::storageKey(source->request.visualKey), QStringLiteral("--replace")});
  }
  // A bounded, cheap "the file on disk is still a usable image" check. It never
  // decodes the whole file: the RAM copy already proved decodability.
  bool validStoredImage(const QString& path) const {
    QFile file(path);
    return safeExistingChain(path) && file.open(QIODevice::ReadOnly) && file.size() > 0 &&
        quint64(file.size()) <= options_.encodedImageBytes && supportedImageHeader(file.read(16));
  }
  ImagePersistenceReport verify(const JobPtr& job) {
    if (closing_) return {ImagePersistence::Cancelled, QStringLiteral("图片服务正在关闭"), false};
    if (job->cancelled.load()) return {ImagePersistence::Cancelled, QStringLiteral("磁盘确认已取消"), false};
    const QString path = cachePath(options_.dataRoot, job->request.visualKey);
    if (path.isEmpty()) return {ImagePersistence::Failed, QStringLiteral("缓存路径无效或不可写"), false};
    if (validStoredImage(path)) return {ImagePersistence::AlreadyValid, {}, false};
    return {ImagePersistence::Failed,
        QStringLiteral("内存中已有图片，但磁盘缓存缺失或无效"), false};
  }
  // Display success never implies persistence success: every path below returns
  // an explicit outcome, including queue, path, lock, write and commit failures.
  ImagePersistenceReport save(const JobPtr& job) {
    if (closing_) return {ImagePersistence::Cancelled, QStringLiteral("图片服务正在关闭"), false};
    if (job->cancelled.load()) return {ImagePersistence::Cancelled, QStringLiteral("保存已取消"), false};
    if (job->cached && !job->migrate) return {ImagePersistence::AlreadyValid, {}, false};
    const QString path = cachePath(options_.dataRoot, job->request.visualKey);
    if (path.isEmpty()) return {ImagePersistence::Failed, QStringLiteral("缓存路径无效或不可写"), false};
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory) || !safeExistingChain(path))
      return {ImagePersistence::Failed, QStringLiteral("缓存目录不可写"), false};
    const QString lockPath = path + QStringLiteral(".lock");
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!safeExistingChain(lockPath) || !lock.tryLock(0) || !safeExistingChain(path))
      return {ImagePersistence::Failed, QStringLiteral("缓存文件被其他进程占用或路径无效"), false};
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly))
      return {ImagePersistence::Failed, QStringLiteral("无法打开缓存文件写入"), false};
    if (file.write(job->bytes) != job->bytes.size()) {
      file.cancelWriting();
      return {ImagePersistence::Failed, QStringLiteral("缓存写入不完整"), false};
    }
    // Cancellation before the commit point discards the temporary file. After a
    // successful commit the replaced file stays valid and is never written twice.
    if (job->cancelled.load() || closing_) {
      file.cancelWriting();
      return {ImagePersistence::Cancelled, QStringLiteral("保存期间已取消，未提交缓存文件"), false};
    }
    if (!safeExistingChain(path)) {
      file.cancelWriting();
      return {ImagePersistence::Failed, QStringLiteral("缓存路径检查失败"), false};
    }
    if (!file.commit()) return {ImagePersistence::Failed, QStringLiteral("缓存提交失败"), false};
    const bool wantsMetadata = !job->cached && !job->url.isEmpty();
    bool metadataSaved = false;
    if (wantsMetadata) {
      const QString metaPath = QDir(directory).filePath(ImageService::storageKey(job->request.visualKey) + QStringLiteral(".json"));
      QSaveFile metadata(metaPath);
      metadata.setDirectWriteFallback(false);
      const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("url"), job->url},
          {QStringLiteral("revision"), job->sourceRevision},
          {QStringLiteral("pngSha256"), QString::fromLatin1(QCryptographicHash::hash(job->bytes, QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact);
      if (safeExistingChain(metaPath) && metadata.open(QIODevice::WriteOnly) && metadata.write(body) == body.size())
        metadataSaved = metadata.commit();
    }
    if (metadataSaved || !wantsMetadata) {
      const QString failure = path + QStringLiteral(".failure.json");
      if (safeExistingChain(failure)) QFile::remove(failure);
      return {ImagePersistence::Saved, {}, metadataSaved};
    }
    // The raster is on disk but its source revision is not recorded, so this
    // file must not be presented as a confirmed new version.
    return {ImagePersistence::Failed,
        QStringLiteral("图片已写入但来源元数据未提交；下次检查会重新获取"), false};
  }
private:
  ImageServiceOptions options_;
  QNetworkAccessManager network_;
  QSet<QNetworkReply*> replies_;
  QSet<QProcess*> processes_;
  bool closing_ = false;
};
struct IoState { ImageIo* agent = nullptr; }; // Access only inside I/O jobs.
}  // namespace

struct ImageService::Impl {
  struct Mailbox { QMutex mutex; Impl* owner = nullptr; };
  struct Cached { ImageHandle image; quint64 access = 0; };
  ImageService* q;
  ImageServiceOptions options;
  ImageExecutors executors;
  std::shared_ptr<Ledger> ledger = std::make_shared<Ledger>();
  std::shared_ptr<Mailbox> mailbox = std::make_shared<Mailbox>();
  std::shared_ptr<IoState> ioState = std::make_shared<IoState>();
  std::shared_ptr<void> directoryLease;
  QHash<QString, QString> urls;
  QHash<QString, Cached> cache;
  QHash<QString, QString> failures;
  QList<JobPtr> waiting;
  QList<JobPtr> pendingSource;
  QList<JobPtr> encoded;
  QHash<QString, JobPtr> active;
  // One shared byte acquisition per source identity (T9).
  QHash<QString, SourcePtr> sources;
  // One disk write per source, then a cheap confirmation for every other
  // consumer: two render sizes must not race for the same cache file.
  struct PersistSlot { JobPtr owner; QList<JobPtr> waiting; };
  QHash<QString, PersistSlot> persistSlots;
  JobPtr decoding;
  QTimer pumpTimer;
  QTimer batchTimer;
  QList<ImageRequest> batchWaiting;
  QSet<QString> batchActive;
  int batchTotal = 0;
  int batchCompleted = 0;
  int batchFailed = 0;
  bool batchPaused = false;
  bool batchRunning = false;
  quint64 batchGeneration = 0;
  quint64 jobIdCounter = 0;
  quint64 clock = 0;
  quint64 cacheBytes = 0;
  bool initialized = false;
  bool initializing = false;
  bool reloading = false;
  bool closing = false;
  int downloads = 0;

  Impl(ImageService* owner, ImageServiceOptions input, ImageExecutors workers)
      : q(owner), options(std::move(input)), executors(std::move(workers)), pumpTimer(owner), batchTimer(owner) {
    options.maximumDownloads = qBound(1, options.maximumDownloads, 2);
    options.maximumWaiting = qBound(1, options.maximumWaiting, 64);
    options.encodedImageBytes = qMin<quint64>(options.encodedImageBytes, 16ULL * 1024 * 1024);
    options.encodedPoolBytes = qMin<quint64>(options.encodedPoolBytes, 32ULL * 1024 * 1024);
    options.maximumInputPixels = qMin<quint64>(options.maximumInputPixels, 16000000);
    options.totalBytes = qMin<quint64>(options.totalBytes, 192ULL * 1024 * 1024);
    options.lruBytes = qMin<quint64>(options.lruBytes, 64ULL * 1024 * 1024);
    ledger->limit = options.totalBytes;
    ledger->encodedLimit = options.encodedPoolBytes;
    mailbox->owner = this;
    pumpTimer.setSingleShot(true);
    QObject::connect(&pumpTimer, &QTimer::timeout, q, [this] { pump(); });
    batchTimer.setSingleShot(true);
    QObject::connect(&batchTimer, &QTimer::timeout, q, [this] { pumpBatch(); });
    QObject::connect(q, &ImageService::completed, q, [this](const ImageResult& result) {
      if (!batchRunning || !batchActive.contains(result.key)) return;
      // A displayed image is not a persisted one. A batch entry stays pending
      // until its persistence receipt arrives; only a terminal display failure
      // decides it here.
      if (result.outcome == ImageOutcome::Ready) return;
      accountBatch(result.key, false);
    });
  }
  static void deliver(const std::shared_ptr<Mailbox>& box, std::function<void(Impl&)> action) {
    QMutexLocker lock(&box->mutex);
    if (!box->owner) return;
    QMetaObject::invokeMethod(box->owner->q, [box, action = std::move(action)] {
      Impl* owner = nullptr;
      { QMutexLocker inner(&box->mutex); owner = box->owner; }
      if (owner && !owner->closing) action(*owner);
    }, Qt::QueuedConnection);
  }
  bool io(std::function<void(ImageIo&)> action) {
    if (!executors.io || closing) return false;
    const auto state = ioState;
    const auto config = options;
    return executors.io([state, config, action = std::move(action)](QObject* root) {
      if (!root || root->thread() != QThread::currentThread()) return;
      if (!state->agent) {
        state->agent = new ImageIo(root, config);
        QObject::connect(state->agent, &QObject::destroyed, root, [state] { state->agent = nullptr; });
      }
      action(*state->agent);
    });
  }
  void start() {
    if (initialized || initializing || closing) return;
    initializing = true;
    quint64 initializationBytes = 64ULL * 1024 * 1024;
    if (!options.verifiedUrls.isEmpty()) {
      initializationBytes = 512;
      for (auto it = options.verifiedUrls.cbegin(); it != options.verifiedUrls.cend(); ++it)
        initializationBytes += quint64(it.key().capacity() + it.value().capacity()) * 2 + 128;
    }
    const auto initializationLease = reserve(ledger, initializationBytes);
    if (!initializationLease) { initializing = false; initialized = true; return; }
    const auto box = mailbox;
    if (!io([box, initializationLease](ImageIo& worker) {
      auto values = worker.readDirectory();
      quint64 bytes = 512;
      for (auto it = values.cbegin(); it != values.cend(); ++it)
        bytes += quint64(it.key().capacity() + it.value().capacity()) * 2 + 128;
      deliver(box, [values = std::move(values), bytes, initializationLease](Impl& self) {
        self.directoryLease = reserve(self.ledger, bytes);
        if (self.directoryLease) self.urls = values;
        self.initialized = true; self.initializing = false;
        if (self.reloading) {
          self.reloading = false;
          const QPointer<ImageService> owner(self.q);
          QMetaObject::invokeMethod(self.q, [owner] { if (owner && !owner->impl_->closing) emit owner->imageIndexReloaded(); }, Qt::QueuedConnection);
        }
        self.pump();
      });
    })) {
      initializing = false;
      schedule();
    }
  }
  void refreshStats() {
    QMutexLocker lock(&ledger->mutex);
    auto& use = ledger->usage;
    use.lruBytes = cacheBytes;
    use.waiting = waiting.size();
    use.peakWaiting = qMax(use.peakWaiting, use.waiting);
    use.downloads = downloads;
    use.peakDownloads = qMax(use.peakDownloads, downloads);
    use.decoding = bool(decoding);
  }
  // Exactly one terminal accounting per batch entry: the first receipt or
  // display failure removes the key, later ones are ignored.
  void accountBatch(const QString& key, bool success) {
    if (!batchRunning || !batchActive.remove(key)) return;
    ++batchCompleted;
    if (!success) ++batchFailed;
    const QPointer<ImageService> owner(q);
    emit q->batchProgress(batchCompleted, batchTotal, batchFailed);
    if (owner && !closing) batchTimer.start(0);
  }
  void finishPersistence(const JobPtr& job, const ImagePersistenceReport& report) {
    if (closing) return;
    const bool success = report.outcome == ImagePersistence::Saved ||
                         report.outcome == ImagePersistence::AlreadyValid;
    {
      QMutexLocker lock(&ledger->mutex);
      if (success) ++ledger->usage.persistedImages; else ++ledger->usage.persistenceFailures;
    }
    // A receipt from an older batch generation must not decide a new batch even
    // if the same key reappears in it.
    const bool counts = job->batch && job->batchGeneration == batchGeneration &&
        batchActive.contains(job->key);
    accountBatch(job->key, success);
    const ImagePersistenceResult result{job->key, job->request.visualKey, report.outcome,
        report.message, report.metadataSaved, counts};
    const QPointer<ImageService> owner(q);
    emit q->persistenceCompleted(result);
    if (owner && !closing) schedule();
    // Consumers that share this source waited for the single writer's decision
    // instead of racing it for the same cache file.
    if (!job->persistenceKey.isEmpty()) {
      const auto slot = persistSlots.find(job->persistenceKey);
      if (slot != persistSlots.end() && slot->owner == job) {
        const QList<JobPtr> waiting = slot->waiting;
        persistSlots.erase(slot);
        for (const auto& next : waiting) {
          if (closing) return;
          if (next->cancelled.load()) continue;
          verifyPersistedAfterSave(next);
        }
      }
    }
  }
  // The bytes already exist in RAM, so a shared consumer only confirms that the
  // single writer actually left a valid file behind.
  void verifyPersistedAfterSave(const JobPtr& job) {
    const auto box = mailbox;
    const bool admitted = io([job, box](ImageIo& worker) {
      const auto report = worker.verify(job);
      deliver(box, [job, report](Impl& self) { self.finishPersistence(job, report); });
    });
    if (!admitted)
      finishPersistence(job, {ImagePersistence::Failed, QStringLiteral("I/O 队列拒绝，未能确认磁盘缓存"), false});
  }
  void requestPersistence(const JobPtr& job) {
    job->persistenceKey = job->source ? job->sourceKey : job->key;
    auto& slot = persistSlots[job->persistenceKey];
    if (!slot.owner) {
      slot.owner = job;
      startPersistence(job);
      return;
    }
    if (slot.owner != job) slot.waiting.append(job);
  }
  void attachReadySource(const JobPtr& job, const SourcePtr& source) {
    job->source = source;
    job->bytes = source->bytes; // shared, read-only copy-on-write buffer
    job->cached = source->cached;
    job->migrate = source->migrate;
    job->forceNetwork = source->forceNetwork;
    job->attributeSheet = source->attributeSheet;
    job->networkAttempted = source->networkAttempted;
    job->refreshChecked = source->refreshChecked;
    job->url = source->url;
    job->sourceRevision = source->sourceRevision;
    job->localCandidates = source->localCandidates;
    job->localCandidate = source->localCandidate;
    // The charge is shared: it is released with the last consumer, not with the
    // task that happened to start the read.
    job->encodedLease = source->encodedLease;
    job->waitingOnSource = false;
    job->stage = 0;
    encoded.append(job);
  }
  void finishSourceFetch(const SourcePtr& source, bool success) {
    if (downloads > 0) --downloads;
    source->fetching = false;
    source->fetchLease.reset();
    const bool current = sources.value(source->key) == source;
    QList<JobPtr> released;
    for (int i = 0; i < pendingSource.size();) {
      const JobPtr job = pendingSource.at(i);
      if (job->source != source) { ++i; continue; }
      pendingSource.removeAt(i);
      job->waitingOnSource = false;
      released.append(job);
    }
    if (success && current && !source->cancelled.load()) {
      source->ready = true;
      for (const auto& job : std::as_const(released)) attachReadySource(job, source);
      // A consumer that left while the bytes were arriving leaves nothing to
      // reuse: drop the task instead of serving a later request from it.
      if (source->consumers.isEmpty()) {
        source->bytes.clear();
        source->encodedLease.reset();
        sources.remove(source->key);
      }
    } else {
      if (current) sources.remove(source->key);
      for (const auto& job : std::as_const(released)) {
        job->message = source->message;
        report(job, ImageOutcome::Unavailable);
      }
    }
    refreshStats();
    schedule();
  }
  // A consumer that reaches any terminal state is removed from its source. The
  // shared read/download/extract is cancelled only when the last one is gone.
  void releaseSource(const JobPtr& job) {
    if (job->waitingOnSource) {
      pendingSource.removeAll(job);
      job->waitingOnSource = false;
    }
    const auto source = job->source;
    job->source.reset();
    if (!source) return;
    source->consumers.remove(job->jobId);
    if (!source->consumers.isEmpty()) return;
    source->cancelled.store(true);
    source->bytes.clear();
    source->encodedLease.reset();
    if (!source->fetching && sources.value(source->key) == source) sources.remove(source->key);
  }
  void startPersistence(const JobPtr& job) {
    const auto box = mailbox;
    const bool admitted = io([job, box](ImageIo& worker) {
      const auto report = worker.save(job);
      deliver(box, [job, report](Impl& self) { self.finishPersistence(job, report); });
    });
    // A rejected or unavailable I/O queue is a visible failure, never a silent
    // "displayed but nothing was written".
    if (!admitted)
      finishPersistence(job, {ImagePersistence::Failed, QStringLiteral("I/O 队列拒绝，未能写入磁盘缓存"), false});
  }
  // A RAM hit says nothing about the disk, so a batch entry asks I/O to confirm
  // the stored file before it may count as persisted.
  void verifyPersisted(const QString& key, const ImageRequest& request) {
    auto job = std::make_shared<Job>();
    job->key = key;
    job->request = request;
    job->batch = true;
    job->batchGeneration = batchGeneration;
    job->jobId = ++jobIdCounter;
    const auto box = mailbox;
    const bool admitted = io([job, box](ImageIo& worker) {
      const auto report = worker.verify(job);
      deliver(box, [job, report](Impl& self) { self.finishPersistence(job, report); });
    });
    if (!admitted)
      finishPersistence(job, {ImagePersistence::Failed, QStringLiteral("I/O 队列拒绝，未能确认磁盘缓存"), false});
  }
  void report(const JobPtr& job, ImageOutcome outcome, ImageHandle handle = {}) {    if (active.value(job->key) == job) active.remove(job->key);
    releaseSource(job);
    if (job->cancelled.load()) return;
    if (outcome != ImageOutcome::Ready && !job->request.refreshChangedSource) {
      if (failures.size() >= 128) failures.erase(failures.begin());
      failures.insert(job->key, job->message);
    }
    else failures.remove(job->key);
    const ImageResult result{job->key, job->request.visualKey, outcome, std::move(handle), job->message};
    const QPointer<ImageService> owner(q);
    // Publication is a separate Core event. A direct completed slot may delete
    // or close the service; no scheduler frame remains on that call stack.
    QMetaObject::invokeMethod(q, [owner, result] {
      if (owner && !owner->impl_->closing) emit owner->completed(result);
    }, Qt::QueuedConnection);
  }
  bool evictOne() {
    if (cache.isEmpty()) return false;
    auto oldest = cache.begin();
    for (auto it = cache.begin(); it != cache.end(); ++it)
      if (it->access < oldest->access) oldest = it;
    cacheBytes -= oldest->image->imageBytes;
    cache.erase(oldest);
    refreshStats();
    return true;
  }
  std::shared_ptr<void> reserveEvicting(quint64 bytes, ChargeKind kind) {
    auto lease = reserve(ledger, bytes, kind);
    while (!lease && evictOne()) lease = reserve(ledger, bytes, kind);
    return lease;
  }
  void pump();
  void pumpBatch() {
    if (closing || !batchRunning || batchPaused) return;
    if (batchWaiting.isEmpty() && batchActive.isEmpty()) {
      batchRunning = false; emit q->batchFinished(false, batchFailed); return;
    }
    const QPointer<ImageService> owner(q);
    while (!batchWaiting.isEmpty() && batchActive.size() < 8) {
      const auto request = batchWaiting.takeFirst();
      batchActive.insert(ImageService::requestKey(request, options.resourceVersion));
      q->request(request);
      if (!owner || closing || !batchRunning) return;
    }
  }
  void schedule() { if (!closing && !pumpTimer.isActive()) pumpTimer.start(8); }
  void preflight(const JobPtr& job);
  void decode(const JobPtr& job);
  void completeDecode(const JobPtr& job, QImage image, qint64 elapsed);
};

void ImageService::Impl::pump() {
  if (closing) return;
  if (!initialized) { start(); return; }
  if (!decoding && !encoded.isEmpty()) {
    int selected = 0;
    for (int i = 0; i < encoded.size(); ++i)
      if (encoded[i]->request.selected) { selected = i; break; }
    decoding = encoded.takeAt(selected);
    decoding->stage = 1;
  }
  if (decoding && decoding->stage == 1) preflight(decoding);
  else if (decoding && decoding->stage == 3) decode(decoding);
  // T9: a waiting render size first looks for the shared byte acquisition of its
  // source identity. Bytes that are already held are reused without another
  // read, download or extract; a fetch in flight gains one more consumer.
  for (int i = 0; i < waiting.size();) {
    const JobPtr job = waiting.at(i);
    const auto source = sources.value(job->sourceKey);
    if (!source) { ++i; continue; }
    waiting.removeAt(i);
    job->source = source;
    if (source->ready) { attachReadySource(job, source); continue; }
    job->waitingOnSource = true;
    source->consumers.insert(job->jobId);
    pendingSource.append(job);
  }
  while (!waiting.isEmpty() && downloads < options.maximumDownloads) {
    JobPtr job = waiting.first();
    // A task started earlier in this same pass (or by a previous pass) already
    // owns this source: join it instead of reading or downloading it again.
    if (const auto existing = sources.value(job->sourceKey)) {
      waiting.removeFirst();
      job->source = existing;
      if (existing->ready) { attachReadySource(job, existing); continue; }
      job->waitingOnSource = true;
      existing->consumers.insert(job->jobId);
      pendingSource.append(job);
      continue;
    }
    auto source = std::make_shared<SourceTask>();
    source->key = job->sourceKey;
    source->request = job->request;
    // A re-acquisition continues the local candidate search the previous attempt
    // stopped at. A first request starts from an empty acquisition state.
    source->url = job->url;
    source->sourceRevision = job->sourceRevision;
    source->localCandidates = job->localCandidates;
    source->localCandidate = job->localCandidate;
    source->forceNetwork = job->forceNetwork;
    source->networkAttempted = job->networkAttempted;
    source->refreshChecked = job->refreshChecked;
    if (source->url.isEmpty() && source->request.attributeId < 0 && source->request.stargodId == 0) {
      const QString storage = ImageService::storageKey(source->request.visualKey);
      QString found = QStringLiteral("id:") + storage;
      source->url = urls.value(found);
      if (source->url.isEmpty()) {
        const QStringList ids = storage.split(QLatin1Char('_'));
        if (ids.size() == 2) {
          const QString face = ids.at(1) == QStringLiteral("0") ? ids.at(0) : ids.at(1);
          found = QStringLiteral("face:") + face;
          source->url = urls.value(found);
          if (source->url.isEmpty() && face.toLongLong() >= 10000 && !urls.contains(QStringLiteral("exception:") + face)) {
            found = QStringLiteral("face:%1").arg(face.toLongLong() % 10000);
            source->url = urls.value(found);
          }
        }
      }
      source->sourceRevision = urls.value(QStringLiteral("revision:") + found);
      for (const auto& name : source->request.candidateNames) {
        if (!source->url.isEmpty()) break;
        source->url = urls.value(name.trimmed());
      }
    }
    source->encodedLease = reserveEvicting(options.encodedImageBytes, ChargeKind::Encoded);
    if (!source->encodedLease) {
      if (downloads == 0 && !decoding && encoded.isEmpty()) {
        waiting.removeFirst(); report(job, ImageOutcome::BudgetExceeded); continue;
      }
      break;
    }
    source->fetchLease = reserveEvicting(kReplyBufferBytes * 2, ChargeKind::Normal);
    if (!source->fetchLease) {
      source->encodedLease.reset();
      if (downloads == 0 && !decoding && encoded.isEmpty()) {
        waiting.removeFirst(); report(job, ImageOutcome::BudgetExceeded); continue;
      }
      break;
    }
    const auto box = mailbox;
    const bool admitted = io([source, box](ImageIo& worker) {
      worker.fetch(source, [box](SourcePtr result, bool success) {
        deliver(box, [result = std::move(result), success](Impl& self) {
          self.finishSourceFetch(result, success);
        });
      });
    });
    if (!admitted) {
      source->encodedLease.reset(); source->fetchLease.reset(); schedule(); break;
    }
    sources.insert(source->key, source);
    source->fetching = true;
    waiting.removeFirst();
    job->source = source;
    job->waitingOnSource = true;
    source->consumers.insert(job->jobId);
    pendingSource.append(job);
    ++downloads;
    { QMutexLocker lock(&ledger->mutex); ++ledger->usage.downloadsStarted; }
  }
  refreshStats();
  // Held GUI leases may be released without posting a Core event. A bounded
  // retry only runs while a descriptor is waiting for that capacity.
  if (!waiting.isEmpty() || !pendingSource.isEmpty() ||
      (decoding && (decoding->stage == 1 || decoding->stage == 3))) schedule();
}

void ImageService::Impl::preflight(const JobPtr& job) {
  if (!executors.compute) {
    decoding.reset(); report(job, ImageOutcome::Rejected); schedule(); return;
  }
  const auto box = mailbox;
  const quint64 maximumPixels = options.maximumInputPixels;
  const QSize output = outputSize(job->request);
  const bool admitted = executors.compute([box, job, maximumPixels, output] {
    quint64 charge = 0;
    if (!job->cancelled.load()) {
      QBuffer buffer(&job->bytes);
      if (buffer.open(QIODevice::ReadOnly)) {
        QImageReader reader(&buffer);
        const QSize size = reader.size();
        const QByteArray format = reader.format().toLower();
        const QSet<QByteArray> formats{"png", "jpg", "jpeg", "webp", "bmp", "gif"};
        const quint64 pixels = size.width() > 0 && size.height() > 0
            ? quint64(size.width()) * quint64(size.height()) : 0;
        if (pixels && pixels <= maximumPixels && formats.contains(format))
          // Covers 64-bit source pixels, format conversion, output and decoder
          // scratch. This is a conservative charge, not process Private Bytes.
          charge = pixels * 12 + quint64(output.width()) * output.height() * 8 + 256 * 1024;
      }
    }
    deliver(box, [job, charge](Impl& self) {
      if (self.decoding != job) return;
      if (!charge) { self.completeDecode(job, {}, 0); return; }
      job->decodeCharge = charge;
      job->stage = 3;
      self.schedule();
    });
  });
  if (admitted) job->stage = 2; else schedule();
}

void ImageService::Impl::decode(const JobPtr& job) {
  const QSize output = outputSize(job->request);
  const quint64 outputCharge = quint64(output.width()) * output.height() * 4 + 512;
  if (!job->temporaryLease) job->temporaryLease = reserveEvicting(job->decodeCharge, ChargeKind::Normal);
  if (job->temporaryLease && !job->outputLease)
    job->outputLease = reserveEvicting(outputCharge, ChargeKind::Normal);
  if (!job->temporaryLease || !job->outputLease) {
    job->temporaryLease.reset(); job->outputLease.reset();
    decoding.reset(); report(job, ImageOutcome::BudgetExceeded); schedule(); return;
  }
  const auto box = mailbox;
  const quint64 maximumPixels = options.maximumInputPixels;
  const bool admitted = executors.compute([box, job, output, maximumPixels] {
    QElapsedTimer elapsed;
    elapsed.start();
    QImage result;
    if (!job->cancelled.load()) {
      QBuffer buffer(&job->bytes);
      if (buffer.open(QIODevice::ReadOnly)) {
        QImageReader reader(&buffer);
        if (!job->attributeSheet) reader.setScaledSize(fittedSize(reader.size(), output));
        QImage decoded = reader.read();
        if (!decoded.isNull() && quint64(decoded.width()) * decoded.height() <= maximumPixels) {
          if (job->attributeSheet) {
            const QRect rectangle(attributePosition(job->request.attributeId), QSize(40, 40));
            if (decoded.rect().contains(rectangle)) decoded = decoded.copy(rectangle); else decoded = {};
          }
          if (!decoded.isNull()) {
            decoded = decoded.scaled(fittedSize(decoded.size(), output), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            result = decoded.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            result.setDevicePixelRatio(qBound<qreal>(1, job->request.devicePixelRatio, 4));
          }
        }
      }
    }
    if (job->cancelled.load()) result = {};
    const qint64 nanos = elapsed.nsecsElapsed();
    deliver(box, [job, result = std::move(result), nanos](Impl& self) mutable {
      self.completeDecode(job, std::move(result), nanos);
    });
  });
  if (admitted) job->stage = 4; else schedule();
}

void ImageService::Impl::completeDecode(const JobPtr& job, QImage image, qint64 elapsed) {
  if (decoding != job) return;
  decoding.reset();
  job->temporaryLease.reset();
  {
    QMutexLocker lock(&ledger->mutex);
    ledger->usage.decodeNanosecondsMaximum = qMax(ledger->usage.decodeNanosecondsMaximum, quint64(qMax<qint64>(0, elapsed)));
    if (!image.isNull()) ++ledger->usage.decodedImages;
  }
  if (image.isNull()) {
    job->outputLease.reset();
    const bool icon = job->request.attributeId >= 0 || job->request.stargodId > 0;
    if (job->cached && !job->forceNetwork && (!icon || job->localCandidate < job->localCandidates.size())) {
      // The bytes of this source are unusable, so every render size that shares
      // them must acquire them again, not just the one that decoded first. The
      // candidate search continues from where this attempt stopped.
      if (const auto source = job->source) {
        job->localCandidates = source->localCandidates;
        job->localCandidate = source->localCandidate;
        job->forceNetwork = !icon && source->localCandidate >= source->localCandidates.size();
        job->networkAttempted = source->networkAttempted;
        job->refreshChecked = source->refreshChecked;
        job->url = source->url;
        job->sourceRevision = source->sourceRevision;
        job->cached = false;
        source->ready = false;
        source->bytes.clear();
        source->encodedLease.reset();
        if (sources.value(source->key) == source) sources.remove(source->key);
        for (int i = encoded.size() - 1; i >= 0; --i) {
          if (encoded.at(i)->source != source) continue;
          const JobPtr sibling = encoded.takeAt(i);
          if (sibling == job) continue;
          sibling->localCandidates = job->localCandidates;
          sibling->localCandidate = job->localCandidate;
          sibling->forceNetwork = job->forceNetwork;
          sibling->networkAttempted = job->networkAttempted;
          sibling->refreshChecked = job->refreshChecked;
          sibling->url = job->url;
          sibling->sourceRevision = job->sourceRevision;
          sibling->cached = false;
          sibling->bytes = {};
          sibling->encodedLease.reset();
          sibling->stage = 0;
          sibling->waitingOnSource = true;
          if (waiting.size() >= options.maximumWaiting) report(waiting.takeLast(), ImageOutcome::Rejected);
          waiting.prepend(sibling);
        }
      } else {
        job->forceNetwork = !icon && job->localCandidate >= job->localCandidates.size();
        job->cached = false;
      }
      job->bytes = {}; job->encodedLease.reset();
      job->stage = 0;
      if (waiting.size() >= options.maximumWaiting) report(waiting.takeLast(), ImageOutcome::Rejected);
      job->waitingOnSource = true;
      waiting.prepend(job);
    } else {
      job->message = QStringLiteral("图片文件无效或超过可显示大小；可右键重试");
      if (const auto source = job->source) {
        source->message = job->message;
        io([source](ImageIo& worker) { worker.rememberFailure(source, true); });
      }
      report(job, ImageOutcome::Rejected);
    }
    schedule();
    return;
  }
  auto payload = std::make_shared<ImagePayload>();
  payload->key = job->key;
  payload->visualKey = job->request.visualKey;
  payload->image = std::move(image);
  payload->imageBytes = quint64(payload->image.sizeInBytes()) + 512;
  payload->memoryRetention = std::move(job->outputLease);
  payload->ledger = ledger;
  while (cacheBytes + payload->imageBytes > options.lruBytes && evictOne()) {}
  if (payload->imageBytes <= options.lruBytes) {
    cache.insert(job->key, {payload, ++clock});
    cacheBytes += payload->imageBytes;
  }
  // Save validated encoded bytes on I/O. Its capture keeps the encoded lease
  // alive through QSaveFile, including a GUI publication that overlaps it.
  // A batch entry always needs a persistence decision: either it writes the
  // file or it confirms the existing one. A plain preview is published now and
  // only asks the disk when the bytes were not already there.
  const bool icon = job->request.attributeId >= 0 || job->request.stargodId > 0;
  const bool batchEntry = job->batch && job->batchGeneration == batchGeneration &&
      batchActive.contains(job->key);
  if (!icon && ((!job->cached || job->migrate) || batchEntry)) requestPersistence(job);
  report(job, ImageOutcome::Ready, std::move(payload));
  refreshStats();
  schedule();
}

ImageService::ImageService(ImageServiceOptions options, ImageExecutors executors, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(options), std::move(executors))) {
  qRegisterMetaType<ImageResult>();
  qRegisterMetaType<ImageRequest>();
  qRegisterMetaType<ImageHandle>();
  qRegisterMetaType<ImagePersistenceResult>();
  impl_->start();
}
ImageService::~ImageService() {
  Q_ASSERT(thread() == QThread::currentThread());
  shutdown();
  QMutexLocker lock(&impl_->mailbox->mutex);
  impl_->mailbox->owner = nullptr;
}
QString ImageService::requestKey(const ImageRequest& request, const QString& resourceVersion) {
  const QSize size = outputSize(request);
  return QStringLiteral("%1|%2|%3x%4|%5").arg(imageSourceIdentity(request), resourceVersion)
      .arg(size.width()).arg(size.height()).arg(qBound<qreal>(1, request.devicePixelRatio, 4), 0, 'f', 3);
}
QString ImageService::storageKey(const QString& visualKey) {
  static const QRegularExpression numeric(QStringLiteral("^([1-9][0-9]*)_([0-9]+)(?:_[0-9a-fA-F]{12})?$"));
  const auto match = numeric.match(visualKey);
  return match.hasMatch() ? match.captured(1) + QLatin1Char('_') + match.captured(2) : visualKey;
}
void ImageService::reloadImageIndex() {
  Q_ASSERT(thread() == QThread::currentThread());
  if (impl_->closing) return;
  const QPointer<ImageService> owner(this);
  cancelBatch();
  if (!owner) return;
  impl_->failures.clear();
  for (const auto& job : std::as_const(impl_->active)) job->cancelled.store(true);
  impl_->waiting.clear(); impl_->encoded.clear(); impl_->decoding.reset(); impl_->active.clear();
  impl_->pendingSource.clear();
  // A new directory generation must not be served by a task that started before
  // the update, and its late completion must not publish anything.
  for (const auto& source : std::as_const(impl_->sources)) source->cancelled.store(true);
  impl_->sources.clear();
  impl_->persistSlots.clear();
  // The updater may have atomically replaced cached images. Discard only the
  // service's RAM references; visible documents keep their existing leases.
  impl_->cache.clear(); impl_->cacheBytes = 0;
  impl_->initialized = false; impl_->reloading = true;
  impl_->refreshStats();
  impl_->start();
}
void ImageService::requestBatch(const QList<ImageRequest>& requests) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (impl_->closing) return;
  const QPointer<ImageService> lifetime(this);
  cancelBatch();
  if (!lifetime) return;
  impl_->batchTotal = 0; impl_->batchCompleted = 0; impl_->batchFailed = 0;
  impl_->batchPaused = false; impl_->batchRunning = true;
  ++impl_->batchGeneration;
  QSet<QString> seen;
  for (auto request : requests) {
    request.selected = false;
    // Batch results populate the disk instead of being held for a GUI image.
    request.outputLogicalSize = {1, 1}; request.devicePixelRatio = 1;
    const QString key = requestKey(request, impl_->options.resourceVersion);
    if (seen.contains(key)) continue;
    seen.insert(key); ++impl_->batchTotal;
    // A bundled icon has no account cache entry, so a batch would have nothing
    // to persist for it. Reject it here instead of waiting for a receipt that
    // can never exist.
    bool valid = safeVisualKey(request.visualKey) && request.candidateNames.size() <= 8 &&
        request.attributeId < 0 && request.stargodId <= 0;
    for (const auto& name : request.candidateNames) valid &= name.size() <= 512;
    if (!valid) { ++impl_->batchCompleted; ++impl_->batchFailed; continue; }
    impl_->batchWaiting.append(request);
  }
  const QPointer<ImageService> owner(this);
  emit batchProgress(impl_->batchCompleted, impl_->batchTotal, impl_->batchFailed);
  if (owner && !impl_->closing) impl_->batchTimer.start(0);
}
void ImageService::pauseBatch(bool paused) {
  Q_ASSERT(thread() == QThread::currentThread());
  impl_->batchPaused = paused;
  if (!paused && impl_->batchRunning) impl_->batchTimer.start(0);
}
void ImageService::cancelBatch() {
  Q_ASSERT(thread() == QThread::currentThread());
  if (!impl_->batchRunning) return;
  impl_->batchRunning = false; impl_->batchTimer.stop();
  // Later persistence receipts must not be accounted against a future batch.
  ++impl_->batchGeneration;
  for (const auto& key : std::as_const(impl_->batchActive)) {
    const auto job = impl_->active.take(key);
    if (!job) continue;
    job->cancelled.store(true);
    impl_->waiting.removeAll(job); impl_->encoded.removeAll(job);
    if (impl_->decoding == job) impl_->decoding.reset();
    // A batch entry leaving the shared task does not abort a read or download
    // that a preview is still waiting for; the last consumer does.
    impl_->releaseSource(job);
  }
  impl_->batchWaiting.clear(); impl_->batchActive.clear();
  impl_->refreshStats(); impl_->schedule();
  emit batchFinished(true, impl_->batchFailed);
}
void ImageService::request(const ImageRequest& request) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (impl_->closing) { emit completed({{}, request.visualKey, ImageOutcome::Closed, {}}); return; }
  if ((!safeVisualKey(request.visualKey) && request.attributeId < 0 && request.stargodId == 0) || request.attributeId > 100000 ||
      request.stargodId < 0 || request.stargodId > 100000 || (request.attributeId >= 0 && request.stargodId > 0) ||
      request.candidateNames.size() > 8 || request.outputLogicalSize.width() <= 0 ||
      request.outputLogicalSize.height() <= 0 || !std::isfinite(request.devicePixelRatio)) {
    emit completed({{}, request.visualKey, ImageOutcome::Rejected, {}}); return;
  }
  for (const auto& name : request.candidateNames) if (name.size() > 512) {
    emit completed({{}, request.visualKey, ImageOutcome::Rejected, {}}); return;
  }
  const QString key = requestKey(request, impl_->options.resourceVersion);
  if (auto cached = impl_->cache.find(key); cached != impl_->cache.end()) {
    if (!request.refreshChangedSource) {
      cached->access = ++impl_->clock;
      emit completed({key, request.visualKey, ImageOutcome::Ready, cached->image});
      const QPointer<ImageService> owner(this);
      // The RAM copy proves neither file existence nor content. A batch entry
      // must let I/O confirm the stored file before it can count as persisted.
      if (owner && !impl_->closing && impl_->batchRunning && impl_->batchActive.contains(key))
        impl_->verifyPersisted(key, request);
      return;
    }
    impl_->cacheBytes -= cached->image->imageBytes;
    impl_->cache.erase(cached);
  }
  if (auto active = impl_->active.find(key); active != impl_->active.end()) {
    if (request.selected) {
      // Mutate request priority only before it crosses to a worker.
      for (int i = 0; i < impl_->waiting.size(); ++i) if (impl_->waiting[i] == active.value()) {
        active.value()->request.selected = true;
        impl_->waiting.move(i, 0); break;
      }
    }
    return;
  }
  if (!request.retry && !request.refreshChangedSource && impl_->failures.contains(key)) {
    emit completed({key, request.visualKey, ImageOutcome::Unavailable, {}, impl_->failures.value(key)}); return;
  }
  if (impl_->waiting.size() >= impl_->options.maximumWaiting) {
    if (!request.selected) { emit completed({key, request.visualKey, ImageOutcome::Rejected, {}}); return; }
    const auto displaced = impl_->waiting.takeLast();
    impl_->report(displaced, ImageOutcome::Rejected);
  }
  auto job = std::make_shared<Job>();
  quint64 descriptorBytes = sizeof(Job) + 512 + quint64(key.capacity() + request.visualKey.capacity()) * 2;
  for (const auto& name : request.candidateNames) descriptorBytes += quint64(name.capacity()) * 2 + 64;
  job->descriptorLease = impl_->reserveEvicting(descriptorBytes, ChargeKind::Normal);
  if (!job->descriptorLease) {
    emit completed({key, request.visualKey, ImageOutcome::BudgetExceeded, {}});
    return;
  }
  job->request = request;
  job->key = key;
  job->sourceKey = imageSourceIdentity(request) + QLatin1Char('|') + impl_->options.resourceVersion;
  // An explicit "this source changed" refresh re-reads instead of reusing bytes
  // that were fetched before the update.
  if (request.refreshChangedSource) job->sourceKey += QStringLiteral("|refresh");
  job->jobId = ++impl_->jobIdCounter;
  job->batch = impl_->batchRunning && impl_->batchActive.contains(key);
  job->batchGeneration = impl_->batchGeneration;
  impl_->active.insert(key, job);
  if (request.selected) impl_->waiting.prepend(job); else impl_->waiting.append(job);
  impl_->refreshStats();
  impl_->schedule();
}
void ImageService::shutdown() {
  Q_ASSERT(thread() == QThread::currentThread());
  if (impl_->closing) return;
  impl_->closing = true;
  impl_->pumpTimer.stop();
  impl_->batchTimer.stop(); impl_->batchRunning = false;
  ++impl_->batchGeneration;
  impl_->batchWaiting.clear(); impl_->batchActive.clear();
  for (const auto& job : std::as_const(impl_->active)) job->cancelled.store(true);
  for (const auto& source : std::as_const(impl_->sources)) {
    source->cancelled.store(true);
    source->bytes.clear();
  }
  impl_->sources.clear();
  impl_->persistSlots.clear();
  impl_->waiting.clear();
  impl_->pendingSource.clear();
  impl_->encoded.clear();
  impl_->decoding.reset();
  impl_->active.clear();
  impl_->cache.clear();
  impl_->cacheBytes = 0;
  impl_->urls.clear();
  impl_->directoryLease.reset();
  const auto state = impl_->ioState;
  if (impl_->executors.io) impl_->executors.io([state](QObject*) {
    if (state->agent) { state->agent->cancel(); state->agent->deleteLater(); }
  });
  impl_->refreshStats();
}
ImageMemoryUsage ImageService::memoryUsage() const {
  Q_ASSERT(thread() == QThread::currentThread());
  QMutexLocker lock(&impl_->ledger->mutex);
  return impl_->ledger->usage;
}
std::shared_ptr<void> ImageService::retainDisplay(const ImageHandle& handle) {
  if (!handle || !handle->ledger || handle->image.isNull()) return {};
  return reserve(handle->ledger, handle->imageBytes, ChargeKind::Display);
}
