#pragma once

#include <QHash>
#include <QImage>
#include <QList>
#include <QObject>
#include <QSize>
#include <QStringList>
#include <functional>
#include <memory>
#include <limits>
#include "contracts/public_dataset.h"

// Only the two small index views are retained by a document. They share the
// bundle's immutable JSON storage without pinning an expanded replay catalog.
struct ImageCatalogSnapshot {
  QString imageIndexObject, iconIndexObject;
  QJsonObject imageIndex, iconIndex;
};
using ImageCatalogHandle = std::shared_ptr<const ImageCatalogSnapshot>;

struct ImageExecutors {
  // Nonblocking admission. Jobs run on the existing I/O and Compute threads.
  // ioRoot owns network children and outlives accepted asynchronous downloads.
  std::function<bool(std::function<void(QObject* ioRoot)>)> io;
  std::function<bool(std::function<void()>)> compute;
};

struct ImageServiceOptions {
  QString dataRoot;
  QString resourceVersion = QStringLiteral("1");
  // Empty means the embedded directory is read once on I/O. This injection is
  // for an already verified resource directory or isolated test fixture.
  QHash<QString, QString> verifiedUrls;
  quint64 totalBytes = 192ULL * 1024 * 1024;
  quint64 lruBytes = 64ULL * 1024 * 1024;
  quint64 encodedPoolBytes = 32ULL * 1024 * 1024;
  quint64 encodedImageBytes = 16ULL * 1024 * 1024;
  quint64 maximumInputPixels = 16000000;
  int maximumWaiting = 64;
  int maximumDownloads = 2;
  int timeoutMilliseconds = 10000;
};

struct ImageRequest {
  QString visualKey;
  QStringList candidateNames;
  // Names are legacy caller metadata only; they never select a download.
  QString sourceIdentity;
  QString contentSha256;
  ImageCatalogHandle catalog;
  // A request sent before the asynchronous directory handshake keeps its
  // original delivery key when the completion supplies the exact index lease.
  bool catalogKeyed = true;
  // A GUI document pins the directory generation it first requested. The
  // default permits current-directory batch/standalone requests.
  quint64 sourceGeneration = std::numeric_limits<quint64>::max();
  QSize outputLogicalSize{300, 300};
  qreal devicePixelRatio = 1;
  bool selected = true;
  bool retry = false;
  // Only an explicit public-data update may replace an existing valid image.
  bool refreshChangedSource = false;
  // Numeric IDs select manually updated disk icons, then bundled fallbacks.
  // An icon request never downloads public data or image resource packages.
  int attributeId = -1;
  // Callers cannot supply a file or network URL.
  int stargodId = 0;
};

namespace ImageServiceInternal { struct Ledger; }
struct ImagePayload {
  QString key;
  QString visualKey;
  std::shared_ptr<void> memoryRetention;
  std::shared_ptr<ImageServiceInternal::Ledger> ledger;
  QImage image; // Destroy pixels before the accounting lease.
  quint64 imageBytes = 0;
  QString sourceIdentity;
  QString contentSha256;
  ImageCatalogHandle catalog;
};
using ImageHandle = std::shared_ptr<const ImagePayload>;

enum class ImageOutcome { Ready, Unavailable, Rejected, BudgetExceeded, Closed };
struct ImageResult {
  QString key;
  QString visualKey;
  ImageOutcome outcome = ImageOutcome::Unavailable;
  ImageHandle handle;
  QString message;
  QString sourceIdentity;
  QString contentSha256;
  ImageCatalogHandle catalog;
};

// Displaying an image and keeping it on disk are separate results. A decoded
// image is displayable before, and even when, it can never be written to disk,
// so a preview never waits for the file system and a batch entry never counts
// a visible image as a persisted one.
enum class ImagePersistence { Saved, AlreadyValid, Failed, Cancelled };
struct ImagePersistenceResult {
  QString key;
  QString visualKey;
  ImagePersistence outcome = ImagePersistence::Failed;
  QString message;         // explicit reason for Failed/Cancelled
  bool metadataSaved = false; // source metadata committed together with the image
  bool batchCounted = false;  // this receipt decided a current-batch entry
};

struct ImageMemoryUsage {
  quint64 chargedBytes = 0;
  quint64 peakChargedBytes = 0;
  quint64 encodedBytes = 0;
  quint64 peakEncodedBytes = 0;
  quint64 lruBytes = 0;
  quint64 displayBytes = 0;
  quint64 peakDisplayBytes = 0;
  quint64 downloadsStarted = 0;
  quint64 decodedImages = 0;
  quint64 decodeNanosecondsMaximum = 0;
  quint64 persistedImages = 0;
  quint64 persistenceFailures = 0;
  int downloads = 0;
  int peakDownloads = 0;
  int waiting = 0;
  int peakWaiting = 0;
  bool decoding = false;
};

// Core-owned scheduler. No thread is created here. All disk/network work uses
// io, all image readers/scaling use compute, and only immutable QImage values
// cross to GUI. Public instance methods must run on the owning Core thread.
class ImageService final : public QObject {
  Q_OBJECT
public:
  explicit ImageService(ImageServiceOptions options, ImageExecutors executors,
                        QObject* parent = nullptr);
  ~ImageService() override;
  void request(const ImageRequest& request);
  // Reloads only the local catalog file. Call after an explicit data update.
  void reloadImageIndex();
  void applyPublicDataset(PublicDatasetSnapshotHandle dataset);
  void publishImageGeneration();
  void requestBatch(const QList<ImageRequest>& requests);
  void pauseBatch(bool paused);
  void cancelBatch();
  void shutdown();
  ImageMemoryUsage memoryUsage() const;
  static QString requestKey(const ImageRequest& request, const QString& resourceVersion);
  static QString storageKey(const QString& visualKey);
  // Every GUI representation acquires its own lease; cache eviction cannot
  // uncharge a still-visible pixmap/document. Safe on any thread.
  static std::shared_ptr<void> retainDisplay(const ImageHandle& handle);

signals:
  void completed(const ImageResult& result);
  // Emitted once per request that asked the disk to hold this image (a new
  // download, a legacy-path migration, or a batch entry that had to confirm the
  // file is still there). Never emitted for a plain preview.
  void persistenceCompleted(const ImagePersistenceResult& result);
  void imageIndexReloaded();
  void imageGenerationAvailable(quint64 generation);
  void imageCatalogAvailable(const ImageCatalogHandle& catalog);
  void batchProgress(int completed, int total, int failed);
  void batchFinished(bool cancelled, int failed);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

Q_DECLARE_METATYPE(ImageRequest)
Q_DECLARE_METATYPE(ImageResult)
Q_DECLARE_METATYPE(ImageHandle)
Q_DECLARE_METATYPE(ImagePersistenceResult)
Q_DECLARE_METATYPE(ImageCatalogHandle)
