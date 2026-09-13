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
bool safeExistingChain(const QString& absolutePath) {
  const QString clean = QDir::cleanPath(QDir::fromNativeSeparators(absolutePath));
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
  std::atomic_bool cancelled{false};
};
using JobPtr = std::shared_ptr<Job>;
using IoDone = std::function<void(JobPtr, bool)>;

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
  void rememberFailure(const JobPtr& job) {
    if (closing_ || job->cancelled.load() || !job->networkAttempted || job->request.attributeId >= 0 || job->request.stargodId > 0) return;
    const QString path = cachePath(options_.dataRoot, job->request.visualKey) + QStringLiteral(".failure.json");
    if (!safeExistingChain(path) || !QDir().mkpath(QFileInfo(path).absolutePath())) return;
    const QByteArray data = QJsonDocument(QJsonObject{{QStringLiteral("url"), job->url},
        {QStringLiteral("message"), job->message}, {QStringLiteral("attemptedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}}).toJson(QJsonDocument::Compact);
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (file.open(QIODevice::WriteOnly) && file.write(data) == data.size()) file.commit();
  }
  void fetch(const JobPtr& job, IoDone done) {
    if (closing_ || job->cancelled.load()) { done(job, false); return; }
    const bool attribute = job->request.attributeId >= 0;
    const bool stargod = job->request.stargodId > 0;
    const QString path = attribute ? QStringLiteral(":/kqpet/attribute-icons.png")
                                   : stargod ? QStringLiteral(":/kqpet/stargod-icons/%1.png").arg(job->request.stargodId)
                                   : cachePath(options_.dataRoot, job->request.visualKey);
    if (job->request.refreshChangedSource && !job->refreshChecked && !attribute && !stargod) {
      job->refreshChecked = true;
      QFile metadata(QFileInfo(path).absolutePath() + QLatin1Char('/') + ImageService::storageKey(job->request.visualKey) + QStringLiteral(".json"));
      if (safeExistingChain(metadata.fileName()) && metadata.open(QIODevice::ReadOnly) && metadata.size() <= 65536) {
        const auto previous = QJsonDocument::fromJson(metadata.readAll()).object();
        const QString oldUrl = previous.value(QStringLiteral("url")).toString();
        const QString oldRevision = previous.value(QStringLiteral("revision")).toString();
        job->forceNetwork = !job->url.isEmpty() && !oldUrl.isEmpty() &&
            (oldUrl != job->url || (!job->sourceRevision.isEmpty() && oldRevision != job->sourceRevision));
      }
    }
    if (job->localCandidates.isEmpty() && !job->forceNetwork) {
      if (attribute || stargod) {
        const QString disk = QDir(options_.dataRoot).filePath(attribute
            ? QStringLiteral("images/attributes/%1.png").arg(job->request.attributeId)
            : QStringLiteral("images/stargods/%1.png").arg(job->request.stargodId));
        if (safeExistingChain(disk)) job->localCandidates.append(disk);
        // The bundled attribute sprite has a fixed, known set. An unknown ID
        // must not silently inherit the normal attribute's picture.
        if (stargod || job->request.attributeId <= 28) job->localCandidates.append(path);
      } else job->localCandidates = localPaths(job->request.visualKey);
    }
    while (!job->forceNetwork && job->localCandidate < job->localCandidates.size()) {
      const QString candidate = job->localCandidates.at(job->localCandidate++);
      job->attributeSheet = attribute && candidate.startsWith(QStringLiteral(":/"));
      QFile file(candidate);
      if (file.open(QIODevice::ReadOnly) && file.size() > 0 &&
          quint64(file.size()) <= options_.encodedImageBytes) {
        job->bytes = file.read(qint64(options_.encodedImageBytes) + 1);
        job->cached = true;
        job->migrate = candidate != path;
        if (!job->bytes.isEmpty() && quint64(job->bytes.size()) <= options_.encodedImageBytes) {
          done(job, true);
          return;
        }
        job->bytes.clear();
      }
    }
    if (attribute || stargod || !allowedUrl(job->url)) {
      job->message = QStringLiteral("本地没有图片；请点击“检查数据更新”补充图片索引"); done(job, false); return;
    }
    QFile previousFailure(path + QStringLiteral(".failure.json"));
    if (!job->request.retry && !job->request.refreshChangedSource && safeExistingChain(previousFailure.fileName()) && previousFailure.open(QIODevice::ReadOnly) && previousFailure.size() <= 8192 &&
        QJsonDocument::fromJson(previousFailure.readAll()).object().value(QStringLiteral("url")).toString() == job->url) {
      job->message = QStringLiteral("上次图片下载失败；右键选择“重新加载精灵图片”可重试"); done(job, false); return;
    }
    job->cached = false;
    job->networkAttempted = true;
    if (QUrl(job->url).path().endsWith(QStringLiteral(".swf"), Qt::CaseInsensitive)) {
      extract(job, std::move(done)); return;
    }
    QNetworkRequest request{QUrl(job->url)};
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(options_.timeoutMilliseconds);
    request.setRawHeader("User-Agent", "KQPetInventory/2");
    auto* reply = network_.get(request);
    reply->setReadBufferSize(kReplyBufferBytes);
    replies_.insert(reply);
    // Reserve once, so QByteArray geometric growth cannot exceed its charge.
    job->bytes.clear();
    job->bytes.reserve(qsizetype(options_.encodedImageBytes));
    auto* timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    timeout->start(options_.timeoutMilliseconds);
    const auto drain = [this, job, reply]() {
      if (job->cancelled.load() || closing_) { reply->abort(); return; }
      const qint64 available = reply->bytesAvailable();
      if (available > qint64(options_.encodedImageBytes) - job->bytes.size()) {
        reply->setProperty("kqOversize", true);
        reply->abort();
        return;
      }
      if (available > 0) job->bytes.append(reply->read(available));
    };
    connect(reply, &QNetworkReply::readyRead, this, drain);
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply]() {
      bool known = false;
      const qlonglong length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
      if (known && length > qint64(options_.encodedImageBytes)) {
        reply->setProperty("kqOversize", true); reply->abort();
      }
    });
    connect(reply, &QNetworkReply::finished, this, [this, job, reply, done = std::move(done), drain]() {
      drain();
      replies_.remove(reply);
      const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      const bool success = !closing_ && !job->cancelled.load() &&
          reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 &&
          !reply->property("kqOversize").toBool() && !job->bytes.isEmpty();
      if (!success) {
        job->message = status ? QStringLiteral("图片下载失败（HTTP %1）；可右键重试").arg(status)
                             : QStringLiteral("图片下载失败或超时；可右键重试");
        rememberFailure(job);
      }
      reply->deleteLater();
      done(job, success);
    });
  }
  void extract(const JobPtr& job, IoDone done) {
    const QString python = QDir(options_.dataRoot).filePath(QStringLiteral("data-tools/python/python.exe"));
    const QString script = QDir(options_.dataRoot).filePath(QStringLiteral("data-tools/scripts/public_data_updater.py"));
    if (!safeExistingChain(python) || !safeExistingChain(script) || !QFile::exists(python) || !QFile::exists(script)) {
      job->message = QStringLiteral("图片工具尚未准备；请先点击“检查数据更新”"); done(job, false); return;
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
    const auto complete = [this, process, job, done = std::move(done), finished](bool success) {
      if (std::exchange(*finished, true)) return;
      processes_.remove(process);
      process->deleteLater();
      if (success && !closing_ && !job->cancelled.load()) {
        const QString path = cachePath(options_.dataRoot, job->request.visualKey);
        QFile file(path);
        if (safeExistingChain(path) && file.open(QIODevice::ReadOnly) && file.size() > 0 && quint64(file.size()) <= options_.encodedImageBytes) {
          job->bytes = file.read(qint64(options_.encodedImageBytes) + 1);
          if (!job->bytes.isEmpty() && quint64(job->bytes.size()) <= options_.encodedImageBytes) {
            job->cached = true; job->forceNetwork = true; job->migrate = false; done(job, true); return;
          }
        }
      }
      job->message = QStringLiteral("官方图片下载或提取失败；可右键重试");
      rememberFailure(job);
      done(job, false);
    };
    connect(process, &QProcess::finished, process, [complete](int code, QProcess::ExitStatus status) {
      complete(code == 0 && status == QProcess::NormalExit);
    });
    connect(process, &QProcess::errorOccurred, process, [complete](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) complete(false);
    });
    process->start(python, {script, QStringLiteral("--data-root"), options_.dataRoot,
        QStringLiteral("--extract-image"), QStringLiteral("--visual-key"), ImageService::storageKey(job->request.visualKey), QStringLiteral("--replace")});
  }
  void save(const JobPtr& job) {
    if (closing_ || job->cancelled.load() || (job->cached && !job->migrate) || job->request.attributeId >= 0 || job->request.stargodId > 0) return;
    const QString path = cachePath(options_.dataRoot, job->request.visualKey);
    if (path.isEmpty()) return;
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory) || !safeExistingChain(path)) return;
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(0);
    if (!safeExistingChain(path + QStringLiteral(".lock")) || !lock.tryLock(0) || !safeExistingChain(path)) return;
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(job->bytes) != job->bytes.size() ||
        job->cancelled.load() || !safeExistingChain(path)) { file.cancelWriting(); return; }
    if (file.commit()) {
      if (!job->cached && !job->url.isEmpty()) {
        const QString metaPath = QDir(directory).filePath(ImageService::storageKey(job->request.visualKey) + QStringLiteral(".json"));
        QSaveFile metadata(metaPath);
        metadata.setDirectWriteFallback(false);
        const QByteArray body = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("url"), job->url},
            {QStringLiteral("revision"), job->sourceRevision},
            {QStringLiteral("pngSha256"), QString::fromLatin1(QCryptographicHash::hash(job->bytes, QCryptographicHash::Sha256).toHex())}}).toJson(QJsonDocument::Compact);
        if (safeExistingChain(metaPath) && metadata.open(QIODevice::WriteOnly) && metadata.write(body) == body.size()) metadata.commit();
      }
      const QString failure = path + QStringLiteral(".failure.json");
      if (safeExistingChain(failure)) QFile::remove(failure);
    }
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
  QList<JobPtr> encoded;
  QHash<QString, JobPtr> active;
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
      if (!batchRunning || !batchActive.remove(result.key)) return;
      ++batchCompleted;
      if (result.outcome != ImageOutcome::Ready) ++batchFailed;
      const QPointer<ImageService> owner(q);
      emit q->batchProgress(batchCompleted, batchTotal, batchFailed);
      if (owner && !closing) batchTimer.start(0);
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
  void report(const JobPtr& job, ImageOutcome outcome, ImageHandle handle = {}) {
    if (active.value(job->key) == job) active.remove(job->key);
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
  while (!waiting.isEmpty() && downloads < options.maximumDownloads) {
    JobPtr job = waiting.first();
    if (job->url.isEmpty() && job->request.attributeId < 0 && job->request.stargodId == 0) {
      const QString storage = ImageService::storageKey(job->request.visualKey);
      QString sourceKey = QStringLiteral("id:") + storage;
      job->url = urls.value(sourceKey);
      if (job->url.isEmpty()) {
        const QStringList ids = storage.split(QLatin1Char('_'));
        if (ids.size() == 2) {
          const QString face = ids.at(1) == QStringLiteral("0") ? ids.at(0) : ids.at(1);
          sourceKey = QStringLiteral("face:") + face;
          job->url = urls.value(sourceKey);
          if (job->url.isEmpty() && face.toLongLong() >= 10000 && !urls.contains(QStringLiteral("exception:") + face)) {
            sourceKey = QStringLiteral("face:%1").arg(face.toLongLong() % 10000);
            job->url = urls.value(sourceKey);
          }
        }
      }
      job->sourceRevision = urls.value(QStringLiteral("revision:") + sourceKey);
      for (const auto& name : job->request.candidateNames) {
        if (!job->url.isEmpty()) break;
        job->url = urls.value(name.trimmed());
      }
    }
    job->encodedLease = reserveEvicting(options.encodedImageBytes, ChargeKind::Encoded);
    if (!job->encodedLease) {
      if (downloads == 0 && !decoding && encoded.isEmpty()) {
        waiting.removeFirst(); report(job, ImageOutcome::BudgetExceeded); continue;
      }
      break;
    }
    job->temporaryLease = reserveEvicting(kReplyBufferBytes * 2, ChargeKind::Normal);
    if (!job->temporaryLease) {
      job->encodedLease.reset();
      if (downloads == 0 && !decoding && encoded.isEmpty()) {
        waiting.removeFirst(); report(job, ImageOutcome::BudgetExceeded); continue;
      }
      break;
    }
    const auto box = mailbox;
    const bool admitted = io([job, box](ImageIo& worker) {
      worker.fetch(job, [box](JobPtr result, bool success) {
        deliver(box, [result = std::move(result), success](Impl& self) {
          --self.downloads;
          result->temporaryLease.reset();
          if (success) self.encoded.append(result);
          else self.report(result, ImageOutcome::Unavailable);
          self.refreshStats();
          self.schedule();
        });
      });
    });
    if (!admitted) {
      job->encodedLease.reset(); job->temporaryLease.reset(); schedule(); break;
    }
    waiting.removeFirst();
    ++downloads;
    { QMutexLocker lock(&ledger->mutex); ++ledger->usage.downloadsStarted; }
  }
  refreshStats();
  // Held GUI leases may be released without posting a Core event. A bounded
  // retry only runs while a descriptor is waiting for that capacity.
  if (!waiting.isEmpty() || (decoding && (decoding->stage == 1 || decoding->stage == 3))) schedule();
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
      job->bytes = {}; job->encodedLease.reset();
      job->forceNetwork = !icon && job->localCandidate >= job->localCandidates.size();
      job->cached = false; job->stage = 0;
      if (waiting.size() >= options.maximumWaiting) report(waiting.takeLast(), ImageOutcome::Rejected);
      waiting.prepend(job);
    } else {
      job->message = QStringLiteral("图片文件无效或超过可显示大小；可右键重试");
      io([job](ImageIo& worker) { worker.rememberFailure(job); });
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
  if ((!job->cached || job->migrate) && job->request.attributeId < 0 && job->request.stargodId == 0)
    io([job](ImageIo& worker) { worker.save(job); });
  report(job, ImageOutcome::Ready, std::move(payload));
  refreshStats();
  schedule();
}

ImageService::ImageService(ImageServiceOptions options, ImageExecutors executors, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this, std::move(options), std::move(executors))) {
  qRegisterMetaType<ImageResult>();
  qRegisterMetaType<ImageRequest>();
  qRegisterMetaType<ImageHandle>();
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
  const QString identity = request.attributeId >= 0
      ? QStringLiteral("attribute:%1").arg(request.attributeId)
      : request.stargodId > 0 ? QStringLiteral("stargod:%1").arg(request.stargodId) : request.visualKey;
  return QStringLiteral("%1|%2|%3x%4|%5").arg(identity, resourceVersion)
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
  QSet<QString> seen;
  for (auto request : requests) {
    request.selected = false;
    // Batch results are used to populate the disk, not held for a GUI image.
    request.outputLogicalSize = {1, 1}; request.devicePixelRatio = 1;
    const QString key = requestKey(request, impl_->options.resourceVersion);
    if (seen.contains(key)) continue;
    seen.insert(key); ++impl_->batchTotal;
    bool valid = safeVisualKey(request.visualKey) && request.candidateNames.size() <= 8;
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
  for (const auto& key : std::as_const(impl_->batchActive)) {
    const auto job = impl_->active.take(key);
    if (!job) continue;
    job->cancelled.store(true);
    impl_->waiting.removeAll(job); impl_->encoded.removeAll(job);
    if (impl_->decoding == job) impl_->decoding.reset();
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
  impl_->batchWaiting.clear(); impl_->batchActive.clear();
  for (const auto& job : std::as_const(impl_->active)) job->cancelled.store(true);
  impl_->waiting.clear();
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
