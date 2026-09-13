#include "image_service.h"
#include "pet_image_cache.h"
#include "pet_raw_data_tree.h"
#include "analysis_worker.h"
#include "storage_service.h"

#include <QApplication>
#include <QBuffer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <cstdio>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

namespace {
quint64 privateBytes() {
#ifdef Q_OS_WIN
  PROCESS_MEMORY_COUNTERS_EX counters{};
  counters.cb = sizeof(counters);
  if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
    return counters.PrivateUsage;
#endif
  return 0;
}
bool require(bool value, const char* message) {
  if (!value) std::fprintf(stderr, "FAIL: %s\n", message);
  return value;
}
bool until(const std::function<bool()>& predicate, int milliseconds = 5000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < milliseconds) QTest::qWait(5);
  return predicate();
}
QByteArray png(const QSize& size, const QColor& color = QColor(35, 120, 200)) {
  QImage image(size, QImage::Format_ARGB32);
  image.fill(color);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}
ImageRequest request(const QString& key, const QString& name) {
  ImageRequest value;
  value.visualKey = key;
  value.candidateNames = {name};
  return value;
}
}  // namespace

int main(int argc, char* argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  bool ok = true;
  QTemporaryDir directory;
  QTcpServer server;
  ok &= require(directory.isValid() && server.listen(QHostAddress::LocalHost), "isolated fixture failed");
  const QByteArray smallPng = png({128,64});
  const QByteArray large = png({5000,4000});
  const QByteArray full = png({3200,2400});
  QHash<QString,int> requests;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (QTcpSocket* socket = server.nextPendingConnection()) {
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
        if (socket->property("replied").toBool() || !socket->canReadLine()) return;
        const QByteArray line = socket->readLine();
        const QString path = QString::fromLatin1(line.split(' ').value(1));
        socket->setProperty("replied", true);
        ++requests[path];
        if (path == QStringLiteral("/slow")) return;
        if (path == QStringLiteral("/redirect")) {
          socket->write("HTTP/1.1 302 Found\r\nLocation: /must-not-follow\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
          socket->disconnectFromHost();
          return;
        }
        const QByteArray body = path == QStringLiteral("/large") ? large :
            path == QStringLiteral("/full") ? full :
            path == QStringLiteral("/bad") ? QByteArray("not an image") : smallPng;
        QTimer::singleShot(40, socket, [socket, body, path] {
          if (socket->state() != QAbstractSocket::ConnectedState) return;
          const qint64 length = path == QStringLiteral("/oversize") ? 16 * 1024 * 1024 + 1 : body.size();
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " +
                        QByteArray::number(length) + "\r\nConnection: close\r\n\r\n");
          if (path != QStringLiteral("/oversize")) socket->write(body);
          socket->disconnectFromHost();
        });
      });
    }
  });
  StorageService storage(directory.path());
  AnalysisWorker compute;
  std::atomic_bool wrongThread{false};
  const QThread* coreThread = QThread::currentThread();
  ImageExecutors executors{
      [&](auto job) { return storage.postAuxiliary([&, job = std::move(job)](QObject* root) {
        if (QThread::currentThread() == coreThread || !root || root->thread() != QThread::currentThread())
          wrongThread.store(true);
        job(root);
      }); },
      [&](auto job) { return compute.postPriority([&, job = std::move(job)] {
        if (QThread::currentThread() == coreThread) wrongThread.store(true);
        job();
      }); }};
  ImageServiceOptions options;
  options.dataRoot = directory.path();
  options.timeoutMilliseconds = 200;
  options.backoffMilliseconds = 30000;
  const QString base = QStringLiteral("http://127.0.0.1:%1").arg(server.serverPort());
  for (const auto& name : {"one", "two", "bad", "large", "oversize", "slow", "redirect", "full"})
    options.verifiedUrls.insert(QString::fromLatin1(name), base + QLatin1Char('/') + QString::fromLatin1(name));
  const quint64 privateBaseline = privateBytes();
  quint64 sampledPrivatePeak = privateBaseline;
  QTimer memorySampler;
  memorySampler.setInterval(5);
  QObject::connect(&memorySampler, &QTimer::timeout, &app, [&] { sampledPrivatePeak = qMax(sampledPrivatePeak, privateBytes()); });
  memorySampler.start();
  ImageService service(options, executors);
  QHash<QString, ImageResult> results;
  QObject::connect(&service, &ImageService::completed, &service, [&](const ImageResult& result) {
    results.insert(result.visualKey, result);
  });
  for (int duplicate = 0; duplicate < 20; ++duplicate) service.request(request(QStringLiteral("v-one"), QStringLiteral("one")));
  service.request(request(QStringLiteral("v-two"), QStringLiteral("two")));
  ok &= require(until([&] { return results.contains(QStringLiteral("v-one")) && results.contains(QStringLiteral("v-two")); }),
                "valid images did not complete");
  const auto first = results.value(QStringLiteral("v-one"));
  ok &= require(first.outcome == ImageOutcome::Ready && first.handle &&
      first.handle->image.size() == QSize(300,150) && requests.value(QStringLiteral("/one")) == 1,
      "decode sizing or shared request deduplication failed");
  ok &= require(service.memoryUsage().peakDownloads == 2 && !wrongThread.load(),
                "I/O or Compute ownership/download concurrency failed");
  ok &= require(until([&] { return QFile::exists(QDir(directory.path()).filePath(QStringLiteral("images/pets/v-one.png"))); }),
                "validated bytes did not persist through I/O");

  ImageServiceOptions nextResource = options;
  nextResource.resourceVersion = QStringLiteral("directory-2");
  ImageService versionedService(nextResource, executors);
  bool versionedReady = false;
  QObject::connect(&versionedService, &ImageService::completed, &versionedService,
                   [&](const ImageResult& result) { versionedReady = result.outcome == ImageOutcome::Ready; });
  versionedService.request(request(QStringLiteral("v-one"), QStringLiteral("one")));
  ok &= require(until([&] { return versionedReady; }) && requests.value(QStringLiteral("/one")) == 1,
                "a plugin/resource revision downloaded an already cached image again");
  versionedService.shutdown();

  // A stable resource ID survives a renamed pet, a new plugin revision and a
  // missing online index. Old directories remain available for manual cleanup.
  const QString legacyDirectory = QDir(directory.path()).filePath(QStringLiteral("images/pets/versions/old-resource"));
  QDir().mkpath(legacyDirectory);
  const QString legacyPath = QDir(legacyDirectory).filePath(QStringLiteral("123_456_abcdefabcdef.png"));
  QFile legacyFile(legacyPath);
  ok &= require(legacyFile.open(QIODevice::WriteOnly) && legacyFile.write(smallPng) == smallPng.size(), "legacy image setup failed");
  legacyFile.close();
  service.request(request(QStringLiteral("123_456_0123456789ab"), QStringLiteral("no-known-name")));
  ok &= require(until([&] { return results.contains(QStringLiteral("123_456_0123456789ab")); }) &&
      results.value(QStringLiteral("123_456_0123456789ab")).outcome == ImageOutcome::Ready,
      "offline renamed pet did not reuse an older same-resource image");
  ok &= require(until([&] { return QFile::exists(QDir(directory.path()).filePath(QStringLiteral("images/pets/123_456.png"))); }) &&
      QFile::exists(legacyPath), "legacy image was not migrated without deleting the original");
  ok &= require(ImageService::storageKey(QStringLiteral("123_457_0123456789ab")) != ImageService::storageKey(QStringLiteral("123_456_0123456789ab")),
      "different skins shared one disk path");

  const QString indexPath = QDir(directory.path()).filePath(QStringLiteral("catalog/pet-image-index.json"));
  QDir().mkpath(QFileInfo(indexPath).absolutePath());
  QFile index(indexPath);
  const QByteArray indexBytes = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("images"), QJsonObject{{QStringLiteral("999_0"), QJsonObject{{QStringLiteral("url"), base + QStringLiteral("/indexed")}}}}}}).toJson();
  service.request(request(QStringLiteral("999_0_0123456789ab"), QStringLiteral("unknown-before-update")));
  ok &= require(until([&] { return results.contains(QStringLiteral("999_0_0123456789ab")); }) &&
      results.value(QStringLiteral("999_0_0123456789ab")).outcome == ImageOutcome::Unavailable,
      "missing ID index was not reported");
  ok &= require(index.open(QIODevice::WriteOnly) && index.write(indexBytes) == indexBytes.size(), "ID index setup failed");
  index.close();
  QSignalSpy reloaded(&service, &ImageService::imageIndexReloaded);
  service.reloadImageIndex();
  ok &= require(until([&] { return reloaded.size() == 1; }) && requests.value(QStringLiteral("/indexed")) == 0,
      "local index reload itself fetched a pet image");
  results.remove(QStringLiteral("999_0_0123456789ab"));
  service.request(request(QStringLiteral("999_0_0123456789ab"), QStringLiteral("unknown-before-update")));
  ok &= require(until([&] { return results.contains(QStringLiteral("999_0_0123456789ab")); }) &&
      results.value(QStringLiteral("999_0_0123456789ab")).outcome == ImageOutcome::Ready && requests.value(QStringLiteral("/indexed")) == 1,
      "hot-reloaded resource ID index did not supply the missing image");
  ok &= require(until([&] { return QFile::exists(QDir(directory.path()).filePath(QStringLiteral("images/pets/999_0.json"))); }),
      "downloaded raster did not retain source metadata for future manual updates");
  const QByteArray changedIndex = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
      {QStringLiteral("images"), QJsonObject{{QStringLiteral("999_0"), QJsonObject{{QStringLiteral("url"), base + QStringLiteral("/indexed-new")},
          {QStringLiteral("revision"), QStringLiteral("new-revision")}}}}}}).toJson();
  ok &= require(index.open(QIODevice::WriteOnly | QIODevice::Truncate) && index.write(changedIndex) == changedIndex.size(), "changed image index setup failed");
  index.close();
  service.reloadImageIndex();
  ok &= require(until([&] { return reloaded.size() == 2; }), "changed image index did not reload");
  results.remove(QStringLiteral("999_0_0123456789ab"));
  auto changedRequest = request(QStringLiteral("999_0_0123456789ab"), QStringLiteral("unknown-before-update"));
  service.request(changedRequest);
  ok &= require(until([&] { return results.contains(changedRequest.visualKey); }) && requests.value(QStringLiteral("/indexed-new")) == 0,
      "ordinary viewing refreshed an existing image after the public index changed");
  changedRequest.refreshChangedSource = true;
  results.remove(changedRequest.visualKey); service.request(changedRequest);
  ok &= require(until([&] { return results.contains(changedRequest.visualKey); }) &&
      results.value(changedRequest.visualKey).outcome == ImageOutcome::Ready && requests.value(QStringLiteral("/indexed-new")) == 1,
      "explicit data update did not replace a changed image source");
  ok &= require(until([&] {
    QFile metadata(QDir(directory.path()).filePath(QStringLiteral("images/pets/999_0.json")));
    return metadata.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(metadata.readAll()).object().value(QStringLiteral("revision")).toString() == QStringLiteral("new-revision");
  }), "updated image source revision did not persist");
  results.remove(changedRequest.visualKey); service.request(changedRequest);
  ok &= require(until([&] { return results.contains(changedRequest.visualKey); }) && requests.value(QStringLiteral("/indexed-new")) == 1,
      "unchanged manual image update downloaded the same image again");

  QList<ImageRequest> batch;
  for (int i = 0; i < 70; ++i) {
    const QString name = QStringLiteral("batch-%1").arg(i);
    QFile file(QDir(directory.path()).filePath(QStringLiteral("images/pets/%1.png").arg(name)));
    ok &= require(file.open(QIODevice::WriteOnly) && file.write(smallPng) == smallPng.size(), "batch cached image setup failed");
    batch.append(request(name,QStringLiteral("no-online-entry")));
  }
  QSignalSpy batchDone(&service, &ImageService::batchFinished), batchProgress(&service, &ImageService::batchProgress);
  const auto beforeBatch = requests;
  service.requestBatch(batch); service.pauseBatch(true);
  QTest::qWait(25);
  ok &= require(batchDone.isEmpty() && batchProgress.size() == 1, "paused batch continued starting requests");
  service.pauseBatch(false);
  ok &= require(until([&] { return batchDone.size() == 1; }) && batchDone.last().at(1).toInt() == 0 &&
      batchProgress.last().at(0).toInt() == 70 && requests == beforeBatch,
      "batch larger than the descriptor queue failed or re-downloaded cached images");
  service.requestBatch({request(QStringLiteral("batch-cancel"),QStringLiteral("slow"))}); service.cancelBatch();
  QTest::qWait(25);
  ok &= require(batchDone.size() == 2 && batchDone.last().at(0).toBool() && requests == beforeBatch,
      "cancelled image batch continued network work");

  service.request(request(QStringLiteral("v-full"), QStringLiteral("full")));
  ok &= require(until([&] { return results.contains(QStringLiteral("v-full")); }) &&
      results.value(QStringLiteral("v-full")).outcome == ImageOutcome::Ready,
                "valid 7.68 megapixel input failed bounded decode");

  for (const auto& name : {"bad", "large", "oversize", "slow"})
    service.request(request(QStringLiteral("v-") + QString::fromLatin1(name), QString::fromLatin1(name)));
  ok &= require(until([&] { return results.contains(QStringLiteral("v-bad")) &&
      results.contains(QStringLiteral("v-large")) && results.contains(QStringLiteral("v-oversize")) &&
      results.contains(QStringLiteral("v-slow")); }), "failure/timeout results did not finish");
  for (const auto& name : {"bad", "large", "oversize", "slow"})
    ok &= require(results.value(QStringLiteral("v-") + QString::fromLatin1(name)).outcome != ImageOutcome::Ready,
                  "invalid/oversized/timeout image was published");
  service.request(request(QStringLiteral("v-redirect"), QStringLiteral("redirect")));
  ok &= require(until([&] { return results.contains(QStringLiteral("v-redirect")); }) &&
      results.value(QStringLiteral("v-redirect")).outcome != ImageOutcome::Ready &&
      requests.value(QStringLiteral("/must-not-follow")) == 0, "image download followed an unverified redirect");
  service.request(request(QStringLiteral("v-bad"), QStringLiteral("bad")));
  QTest::qWait(80);
  ok &= require(requests.value(QStringLiteral("/bad")) == 1, "failure backoff sent another request");
  // Failures are remembered on disk, so a restart does not issue another 404
  // or invalid download each time this pet is displayed.
  ImageService failedRestart(options, executors);
  ImageResult failedRestartResult;
  QObject::connect(&failedRestart, &ImageService::completed, &failedRestart, [&](const ImageResult& value) { failedRestartResult = value; });
  failedRestart.request(request(QStringLiteral("v-bad"), QStringLiteral("bad")));
  ok &= require(until([&] { return !failedRestartResult.key.isEmpty(); }) &&
      failedRestartResult.outcome != ImageOutcome::Ready && !failedRestartResult.message.isEmpty() && requests.value(QStringLiteral("/bad")) == 1,
      "restart retried a known failed download without a user retry");
  failedRestart.shutdown();
  auto retry = request(QStringLiteral("v-bad"), QStringLiteral("bad"));
  retry.retry = true;
  service.request(retry);
  ok &= require(until([&] { return requests.value(QStringLiteral("/bad")) == 2; }), "explicit retry did not bypass backoff");
  service.request(request(QStringLiteral("../escape"), QStringLiteral("one")));
  service.request(request(QStringLiteral("unknown-url"), QStringLiteral("not-in-directory")));
  ok &= require(until([&] { return results.contains(QStringLiteral("unknown-url")); }) &&
      results.value(QStringLiteral("../escape")).outcome == ImageOutcome::Rejected &&
      results.value(QStringLiteral("unknown-url")).outcome == ImageOutcome::Unavailable,
      "unsafe key or unverified directory entry accepted");

  // LRU eviction and public-result copies must not release retained pixels.
  ImageHandle retained = first.handle;
  auto display = ImageService::retainDisplay(retained);
  const quint64 displayBytes = service.memoryUsage().displayBytes;
  ok &= require(display && displayBytes >= retained->imageBytes, "GUI display lease was not charged");
  display.reset();
  ok &= require(service.memoryUsage().displayBytes == 0, "display charge was not released");
  {
    PetImageBrowser browser;
    browser.resize(320,240);
    browser.show();
    const QString url = QStringLiteral("kqimage://cache/test");
    browser.setImageHtml(QStringLiteral("<p>leased image</p><img src='%1' width='150'>").arg(url), url, retained);
    ok &= require(service.memoryUsage().displayBytes >= retained->imageBytes &&
        browser.document()->resource(QTextDocument::ImageResource, QUrl(url)).value<QImage>().size() == retained->image.size(),
        "HTML resource did not retain an explicit image lease");
    const QString secondUrl = QStringLiteral("kqstargod://icon/80");
    browser.setImagesHtml(QStringLiteral("<img src='%1'><img src='%2'>").arg(url,secondUrl),{{url,retained},{secondUrl,retained}});
    ok &= require(service.memoryUsage().displayBytes >= 2 * retained->imageBytes &&
        !browser.document()->resource(QTextDocument::ImageResource,QUrl(secondUrl)).value<QImage>().isNull(),
        "two document resources did not own independent display leases");
    browser.hide();
    ok &= require(service.memoryUsage().displayBytes == 0, "hidden page still retained its display lease");
    browser.show();
    ok &= require(service.memoryUsage().displayBytes > 0, "show did not reacquire a cached resource lease");
    browser.setHtml(QStringLiteral("<img src='file:///must-not-be-read.png'>"));
    ok &= require(service.memoryUsage().displayBytes == 0 &&
        !browser.document()->resource(QTextDocument::ImageResource, QUrl(QStringLiteral("file:///must-not-be-read.png"))).isValid(),
        "reset retained old image or implicit file resource loader survived");
  }
  {
    const auto beforeRequests = requests;
    PetImageCache proxy(directory.path());
    proxy.setService(&service);
    PetImageBrowser browser;
    browser.resize(320,240); browser.show();
    const QString firstStar = QStringLiteral("kqstargod://icon/1"), secondStar = QStringLiteral("kqstargod://icon/80");
    proxy.setDocumentImage(&browser,QStringLiteral("<p>selected detail stays put</p><img src='%1'><img src='%2'>").arg(firstStar,secondStar),{});
    auto cursor = browser.textCursor(); cursor.setPosition(3); browser.setTextCursor(cursor);
    const int revision = browser.document()->revision();
    ok &= require(until([&] {
      return !browser.document()->resource(QTextDocument::ImageResource,QUrl(firstStar)).value<QImage>().isNull() &&
          !browser.document()->resource(QTextDocument::ImageResource,QUrl(secondStar)).value<QImage>().isNull();
    }),"bundled official stargod sprites did not load into both document resources");
    ok &= require(requests == beforeRequests && browser.textCursor().position() == 3 && browser.document()->revision() == revision,
        "stargod resource completion performed network I/O or rebuilt the detail document");
    ok &= require(service.memoryUsage().displayBytes > 0,"stargod icons were displayed without pixel leases");
    browser.hide();
    ok &= require(service.memoryUsage().displayBytes == 0,"hidden stargod document retained display leases");
    browser.show();
    ok &= require(service.memoryUsage().displayBytes > 0,"visible stargod document failed to reacquire leases");
    ImageRequest missingStar; missingStar.visualKey = QStringLiteral("missing-star"); missingStar.stargodId = 99999;
    service.request(missingStar);
    ok &= require(until([&] { return results.contains(QStringLiteral("missing-star")); }) &&
        results.value(QStringLiteral("missing-star")).outcome != ImageOutcome::Ready && requests == beforeRequests,
        "missing embedded star escaped into network fallback");
  }
  {
    // Official updates supply ID-named PNGs. New IDs are not constrained by
    // the old built-in attribute sheet, and refreshing the local index must
    // replace both a displayed document image and a cached table icon.
    const auto beforeRequests = requests;
    const auto writeImage = [&](const QString& relative, const QByteArray& bytes) {
      const QString path = QDir(directory.path()).filePath(relative);
      QDir().mkpath(QFileInfo(path).absolutePath());
      QFile file(path);
      return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    ok &= require(writeImage(QStringLiteral("images/attributes/29.png"),smallPng) &&
        writeImage(QStringLiteral("images/stargods/93.png"),smallPng) &&
        writeImage(QStringLiteral("images/attributes/26.png"),QByteArray("corrupt")),
        "manual public-icon fixture could not be saved");
    PetImageCache proxy(directory.path()); proxy.setService(&service);
    PetImageBrowser browser; browser.resize(320,240); browser.show();
    const QString url = QStringLiteral("kqstargod://icon/93");
    proxy.setDocumentImage(&browser,QStringLiteral("<p>new official star</p><img src='%1'>").arg(url),{});
    ok &= require(until([&] { return !proxy.attributeIcon(QStringLiteral("29")).isNull() &&
        !browser.document()->resource(QTextDocument::ImageResource,QUrl(url)).value<QImage>().isNull(); }),
        "new official attribute or star did not read a local PNG");
    ImageRequest corrupted; corrupted.attributeId = 26; corrupted.visualKey = QStringLiteral("corrupt-local-attribute");
    service.request(corrupted);
    ok &= require(until([&] { return results.contains(corrupted.visualKey); }) &&
        results.value(corrupted.visualKey).outcome == ImageOutcome::Ready,
        "corrupted public attribute icon did not fall back to its bundled sprite");
    ImageRequest missingAttribute; missingAttribute.attributeId = 30; missingAttribute.visualKey = QStringLiteral("missing-new-attribute");
    service.request(missingAttribute);
    ok &= require(until([&] { return results.contains(missingAttribute.visualKey); }) &&
        results.value(missingAttribute.visualKey).outcome != ImageOutcome::Ready,
        "unknown new attribute silently inherited the normal attribute icon");
    const QColor updated(30,210,45);
    const QByteArray newPng = png({32,32},updated);
    ok &= require(writeImage(QStringLiteral("images/attributes/29.png"),newPng) &&
        writeImage(QStringLiteral("images/stargods/93.png"),newPng),"updated public-icon fixture could not be saved");
    const int revision = browser.document()->revision();
    service.reloadImageIndex();
    ok &= require(until([&] {
      const QImage displayed = browser.document()->resource(QTextDocument::ImageResource,QUrl(url)).value<QImage>();
      const QIcon attribute = proxy.attributeIcon(QStringLiteral("29"));
      return !displayed.isNull() && displayed.pixelColor(0,0) == updated && !attribute.isNull() &&
          attribute.pixmap(24,24).toImage().pixelColor(0,0) == updated;
    }),"manual icon update left a stale table icon or displayed star in GUI memory");
    ok &= require(requests == beforeRequests && browser.document()->revision() == revision,
        "public-icon reload performed network I/O or rebuilt the selected detail document");
  }
  const auto usage = service.memoryUsage();
  ok &= require(usage.peakChargedBytes <= options.totalBytes && usage.peakEncodedBytes <= options.encodedPoolBytes &&
      usage.peakDownloads <= 2 && usage.peakWaiting <= 64 && usage.decodeNanosecondsMaximum > 0,
      "image accounting or bounded scheduler limits failed");

  ImageServiceOptions limited = options;
  limited.totalBytes = 4 * 1024 * 1024;
  ImageService budgetService(limited, executors);
  ImageOutcome budgetOutcome = ImageOutcome::Ready;
  QObject::connect(&budgetService, &ImageService::completed, &budgetService,
                   [&](const ImageResult& result) { budgetOutcome = result.outcome; });
  budgetService.request(request(QStringLiteral("over-budget"), QStringLiteral("one")));
  ok &= require(until([&] { return budgetOutcome == ImageOutcome::BudgetExceeded; }), "budget pressure did not return a placeholder outcome");
  budgetService.shutdown();

  // Capacity that holds encoded bytes but cannot also admit the reply buffer
  // must terminate with a placeholder, rather than spinning a timer forever.
  limited.totalBytes = 16 * 1024 * 1024 + 64 * 1024;
  ImageService scratchBudgetService(limited, executors);
  ImageOutcome scratchOutcome = ImageOutcome::Ready;
  QObject::connect(&scratchBudgetService, &ImageService::completed, &scratchBudgetService,
                   [&](const ImageResult& result) { scratchOutcome = result.outcome; });
  scratchBudgetService.request(request(QStringLiteral("scratch-budget"), QStringLiteral("one")));
  ok &= require(until([&] { return scratchOutcome == ImageOutcome::BudgetExceeded; }),
                "encoded-only budget deadlocked instead of completing");
  scratchBudgetService.shutdown();

  ImageServiceOptions smallLru = options;
  smallLru.lruBytes = 181000; // One 300 x 150 output, never two.
  ImageService lruService(smallLru, executors);
  QHash<QString, ImageHandle> heldImages;
  QObject::connect(&lruService, &ImageService::completed, &lruService, [&](const ImageResult& result) {
    if (result.handle) heldImages.insert(result.visualKey, result.handle);
  });
  lruService.request(request(QStringLiteral("lru-a"), QStringLiteral("one")));
  ok &= require(until([&] { return heldImages.contains(QStringLiteral("lru-a")); }), "first LRU fixture did not complete");
  lruService.request(request(QStringLiteral("lru-b"), QStringLiteral("two")));
  ok &= require(until([&] { return heldImages.size() == 2; }), "second LRU fixture did not complete");
  const auto lruUsage = lruService.memoryUsage();
  const quint64 heldPixelBytes = heldImages.value(QStringLiteral("lru-a"))->imageBytes +
      heldImages.value(QStringLiteral("lru-b"))->imageBytes;
  ok &= require(lruUsage.lruBytes <= smallLru.lruBytes && lruUsage.chargedBytes >= heldPixelBytes,
                "LRU eviction uncharged pixels still retained by a published result");
  lruService.shutdown();
  ok &= require(lruService.memoryUsage().chargedBytes >= heldPixelBytes,
                "service shutdown uncharged GUI-retained pixels");
  heldImages.clear();
  ok &= require(until([&] { return lruService.memoryUsage().chargedBytes == 0; }),
                "image leases survived their final consumer and completed I/O");

  {
    PetRawDataTree raw;
    QJsonArray many;
    for (int index = 0; index < 2000; ++index)
      many.append(QJsonObject{{QStringLiteral("id"), qint64(9007199254740993LL + index)}});
    raw.setJson({{QStringLiteral("id"), qint64(9007199254740993LL)}, {QStringLiteral("sgsp"), many}});
    ok &= require(raw.topLevelItemCount() == 0, "hidden raw tab eagerly constructed JSON items");
    raw.show();
    QCoreApplication::processEvents();
    ok &= require(raw.topLevelItemCount() == 2 &&
        raw.topLevelItem(0)->text(1) == QStringLiteral("9007199254740993") &&
        raw.topLevelItem(1)->childCount() == 0, "raw root lost integer precision or eagerly expanded a child");
    raw.expandItem(raw.topLevelItem(1));
    ok &= require(raw.topLevelItem(1)->childCount() == 129, "raw expansion exceeded its bounded page");
    auto* continuation = raw.topLevelItem(1)->child(128);
    raw.expandItem(continuation);
    ok &= require(continuation->childCount() == 129 && continuation->child(0)->text(0) == QStringLiteral("[128]"),
                  "raw continuation did not preserve later fields");
    raw.hide();
    raw.setJson({{QStringLiteral("id"), 123}});
    ok &= require(raw.topLevelItemCount() == 0, "hidden updated raw tab rebuilt old personal details");
  }

  auto deletingService = std::make_unique<ImageService>(options, executors);
  QObject::connect(deletingService.get(), &ImageService::completed, &app,
                   [&](const ImageResult&) { deletingService.reset(); }, Qt::DirectConnection);
  deletingService->request(request(QStringLiteral("delete-service"), QStringLiteral("one")));
  ok &= require(until([&] { return !deletingService; }), "direct result deletion did not complete safely");
  ImageService closingService(options, executors);
  int closeCallbacks = 0;
  QObject::connect(&closingService, &ImageService::completed, &app, [&](const ImageResult&) {
    ++closeCallbacks;
    closingService.shutdown();
  }, Qt::DirectConnection);
  closingService.request(request(QStringLiteral("close-first"), QStringLiteral("one")));
  closingService.request(request(QStringLiteral("close-second"), QStringLiteral("two")));
  ok &= require(until([&] { return closeCallbacks > 0; }), "direct result shutdown did not complete safely");
  QTest::qWait(50);
  ok &= require(closeCallbacks == 1, "a queued image published after direct shutdown");

  // Hold Compute admission to exercise exactly the 64-descriptor waiting bound.
  ImageExecutors held = executors;
  held.compute = [](auto) { return false; };
  ImageService queueService(options, held);
  for (int i = 0; i < 100; ++i) queueService.request(request(QStringLiteral("queue-%1").arg(i), QStringLiteral("one")));
  ok &= require(queueService.memoryUsage().waiting == 64 && queueService.memoryUsage().peakWaiting == 64,
                "latest selected requests exceeded the descriptor bound");
  queueService.shutdown();
  service.request(request(QStringLiteral("cancel-slow"), QStringLiteral("slow")));
  QTest::qWait(20);
  service.shutdown();
  ok &= require(compute.shutdown() && storage.shutdown(), "existing executors did not drain safely after image cancellation");
  std::fprintf(stdout, "image peak=%llu encoded=%llu downloadPeak=%d queuePeak=%d decodeMaxNs=%llu\n",
      usage.peakChargedBytes, usage.peakEncodedBytes, usage.peakDownloads, usage.peakWaiting, usage.decodeNanosecondsMaximum);
  std::fprintf(stdout, "PrivateBytes baseline=%llu sampledPeak=%llu delta=%llu sampleIntervalMs=5 fixtureMaxDecodedPixels=7680000\n",
      privateBaseline, sampledPrivatePeak, sampledPrivatePeak - privateBaseline);
  if (!ok) return 1;
  std::fprintf(stdout, "PASS: shared images, trusted URLs, async IO/Compute, bounds, failures, backoff, display leases, shutdown\n");
  return 0;
}
