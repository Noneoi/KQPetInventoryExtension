#include "pet_image_cache.h"
#include "pet_identity.h"
#include "stargod_ring_object.h"

#include <QHideEvent>
#include <QContextMenuEvent>
#include <QIconEngine>
#include <QPainter>
#include <QMenu>
#include <QRegularExpression>
#include <QShowEvent>
#include <QTextDocument>
#include <QThread>
#include <QUrl>
#include <utility>
#include <cmath>

namespace {
class LeasedIconEngine final : public QIconEngine {
public:
  LeasedIconEngine(QPixmap pixmap, ImageHandle image, std::shared_ptr<void> lease)
      : lease_(std::move(lease)), image_(std::move(image)), pixmap_(std::move(pixmap)) {}
  QIconEngine* clone() const override { return new LeasedIconEngine(pixmap_, image_, lease_); }
  void paint(QPainter* painter, const QRect& rect, QIcon::Mode, QIcon::State) override {
    painter->drawPixmap(rect, pixmap_);
  }
  QPixmap pixmap(const QSize&, QIcon::Mode, QIcon::State) override { return pixmap_; }
private:
  std::shared_ptr<void> lease_;
  ImageHandle image_;
  QPixmap pixmap_;
};
}

PetImageBrowser::PetImageBrowser(QWidget* parent) : QTextBrowser(parent) {
  ring_ = new StargodRingObject([this](const QString& url) {
    const auto found = resources_.constFind(url);
    return found != resources_.cend() && found->displayed ? found->displayed->image : QImage{};
  }, this);
}
PetImageBrowser::~PetImageBrowser() { releaseImage(); }
void PetImageBrowser::setHtml(const QString& html) {
  releaseImage(); resources_.clear(); QTextBrowser::setHtml(html);
}
QVariant PetImageBrowser::loadResource(int, const QUrl&) { return {}; }
void PetImageBrowser::releaseImage() {
  for (auto it = resources_.begin(); it != resources_.end(); ++it) {
    document()->addResource(QTextDocument::ImageResource, QUrl(it.key()), QVariant{});
    it->displayed.reset();
    it->lease.reset();
  }
}
void PetImageBrowser::acquireImage() {
  for (auto it = resources_.begin(); it != resources_.end(); ++it) {
    if (it->displayed) continue;
    auto image = it->available.lock();
    auto lease = ImageService::retainDisplay(image);
    if (!lease) continue;
    it->displayed = std::move(image);
    it->lease = std::move(lease);
    document()->addResource(QTextDocument::ImageResource, QUrl(it.key()), it->displayed->image);
  }
  document()->markContentsDirty(0, document()->characterCount());
}
void PetImageBrowser::setImageHtml(const QString& html, const QString& resourceUrl, const ImageHandle& image) {
  setImagesHtml(html,resourceUrl.isEmpty() ? QHash<QString,ImageHandle>{} : QHash<QString,ImageHandle>{{resourceUrl,image}});
}
void PetImageBrowser::setImagesHtml(const QString& html, const QHash<QString,ImageHandle>& images) {
  releaseImage();
  resources_.clear();
  for (auto it = images.begin(); it != images.end(); ++it) {
    Resource resource; resource.available = it.value(); resources_.insert(it.key(),std::move(resource));
  }
  QTextBrowser::setHtml(html);
  StargodRingObject::install(document(), ring_);
  if (isVisible()) acquireImage();
}
void PetImageBrowser::updateImageResource(const QString& resourceUrl, const ImageHandle& image) {
  const auto found = resources_.find(resourceUrl);
  if (found == resources_.end() || !image || found->displayed == image) return;
  document()->addResource(QTextDocument::ImageResource,QUrl(resourceUrl),QVariant{});
  found->displayed.reset(); found->lease.reset(); found->available = image;
  if (isVisible()) acquireImage();
}
void PetImageBrowser::hideEvent(QHideEvent* event) { releaseImage(); QTextBrowser::hideEvent(event); }
void PetImageBrowser::showEvent(QShowEvent* event) { acquireImage(); QTextBrowser::showEvent(event); emit imageResourcesNeeded(); }
void PetImageBrowser::resizeEvent(QResizeEvent* event) {
  QTextBrowser::resizeEvent(event);
  document()->markContentsDirty(0, document()->characterCount());
}
void PetImageBrowser::contextMenuEvent(QContextMenuEvent* event) {
  auto* menu = createStandardContextMenu();
  menu->setAttribute(Qt::WA_DeleteOnClose);
  menu->addSeparator();
  QAction* retry = menu->addAction(QStringLiteral("重新加载精灵图片"));
  connect(retry, &QAction::triggered, this, &PetImageBrowser::imageRetryRequested);
  menu->popup(event->globalPos());
}

PetImageCache::PetImageCache(const QString&, QObject* parent) : QObject(parent) {}
void PetImageCache::setDevicePixelRatio(qreal ratio) {
  if (!std::isfinite(ratio)) return;
  ratio = qBound<qreal>(1, ratio, 4);
  if (qFuzzyCompare(devicePixelRatio_, ratio)) return;
  devicePixelRatio_ = ratio;
  emit attributeIconsReady();
}
void PetImageCache::setService(ImageService* service, const QString& resourceVersion) {
  Q_ASSERT(thread() == QThread::currentThread());
  if (service_ == service && version_ == resourceVersion) return;
  if (service_) disconnect(service_, nullptr, this, nullptr);
  service_ = service;
  version_ = resourceVersion;
  ++bindingGeneration_;
  images_.clear(); petKeys_.clear(); icons_.clear(); pending_.clear(); imageStatuses_.clear();
  if (!service) return;
  const quint64 generation = bindingGeneration_;
  connect(service, &ImageService::completed, this, [this, generation](const ImageResult& result) {
    if (bindingGeneration_ == generation) accept(result);
  }, Qt::QueuedConnection);
  connect(service, &ImageService::imageIndexReloaded, this, [this, generation] {
    if (bindingGeneration_ != generation) return;
    images_.clear(); icons_.clear(); imageStatuses_.clear(); pending_.clear();
    const auto browserList = browsers_.keys();
    for (auto* browser : browserList) refreshBrowserResources(browser);
    emit attributeIconsReady();
  }, Qt::QueuedConnection);
  const auto deferred = std::exchange(deferred_, {});
  for (const auto& request : deferred) enqueue(request);
  const auto browserList = browsers_.keys();
  for (auto* browser : browserList) refreshBrowserResources(browser);
  emit attributeIconsReady();
}
QString PetImageCache::resourceUrl(const QString& key) const {
  return QStringLiteral("kqimage://cache/%1").arg(QString::fromLatin1(QUrl::toPercentEncoding(key)));
}
void PetImageCache::enqueue(const ImageRequest& request) const {
  const QString key = ImageService::requestKey(request, version_);
  if (pending_.contains(key)) return;
  if (!service_) {
    if (deferred_.size() < 64) deferred_.insert(key, request);
    return;
  }
  if (pending_.size() >= 64) return;
  pending_.insert(key);
  const QPointer<ImageService> service = service_;
  QMetaObject::invokeMethod(service_, [service, request] { if (service) service->request(request); }, Qt::QueuedConnection);
}
QIcon PetImageCache::attributeIcon(const QString& sequence) const {
  bool valid = false;
  const int parsed = sequence.section(QLatin1Char(','), 0, 0).trimmed().toInt(&valid);
  const int id = valid && parsed >= 0 && parsed <= 100000 ? parsed : 0;
  ImageRequest request;
  request.attributeId = id;
  request.visualKey = QStringLiteral("attribute-%1").arg(id);
  request.outputLogicalSize = {24,24};
  request.devicePixelRatio = devicePixelRatio_;
  request.selected = false;
  const QString key = ImageService::requestKey(request, version_);
  if (auto icon = icons_.constFind(key); icon != icons_.cend()) return icon->icon;
  if (auto image = images_.value(key).lock()) {
    auto lease = ImageService::retainDisplay(image);
    if (lease) {
      IconEntry entry{QIcon(new LeasedIconEngine(QPixmap::fromImage(image->image), image, lease)), image, lease};
      if (icons_.size() >= 64) icons_.erase(icons_.begin());
      const QIcon icon = entry.icon;
      icons_.insert(key, std::move(entry));
      return icon;
    }
  }
  enqueue(request);
  return {};
}
QString PetImageCache::cachedPetImage(const QString& visualKey) const {
  const QString key = petKeys_.value(visualKey);
  return !key.isEmpty() && !images_.value(key).expired() ? resourceUrl(key) : QString();
}
QString PetImageCache::ensurePetImage(const QJsonObject& pet, const QStringList& candidateNames,
                                     qreal devicePixelRatio, bool retry) {
  if (petRaceId(pet) <= 0) return {};
  ImageRequest request;
  request.visualKey = petVisualKey(pet);
  request.candidateNames = candidateNames;
  request.outputLogicalSize = {300, 300};
  request.devicePixelRatio = devicePixelRatio;
  request.retry = retry;
  const QString key = ImageService::requestKey(request, version_);
  if (petRequests_.size() >= 128 && !petRequests_.contains(key)) petRequests_.erase(petRequests_.begin());
  petRequests_.insert(key, request);
  if (petKeys_.size() >= 128 && !petKeys_.contains(request.visualKey)) petKeys_.erase(petKeys_.begin());
  petKeys_.insert(request.visualKey, key);
  if (!images_.value(key).expired()) return resourceUrl(key);
  enqueue(request);
  return resourceUrl(key);
}
void PetImageCache::accept(const ImageResult& result) {
  pending_.remove(result.key);
  if (result.outcome != ImageOutcome::Ready || !result.handle) {
    const QString message = result.message.isEmpty() ? QStringLiteral("图片暂不可用；可右键重试") : result.message;
    if (imageStatuses_.size() >= 128) imageStatuses_.erase(imageStatuses_.begin());
    imageStatuses_.insert(result.key, message);
    for (auto it = browsers_.begin(); it != browsers_.end(); ++it)
      if (it->keys.values().contains(result.key)) it.key()->setToolTip(message);
    emit petImageStatusChanged(result.visualKey, message);
    return;
  }
  const bool wasUnavailable = imageStatuses_.remove(result.key) > 0;
  for (auto it = browsers_.begin(); it != browsers_.end(); ++it)
    if (it->keys.values().contains(result.key)) it.key()->setToolTip({});
  if (wasUnavailable) emit petImageStatusChanged(result.visualKey, {});
  if (images_.size() >= 128 && !images_.contains(result.key)) {
    for (auto it = images_.begin(); it != images_.end();)
      if (it->expired()) it = images_.erase(it); else ++it;
    if (images_.size() >= 128) images_.erase(images_.begin());
  }
  images_.insert(result.key, result.handle);
  if (result.key.startsWith(QStringLiteral("attribute:"))) emit attributeIconsReady();
  else if (result.key.startsWith(QStringLiteral("stargod:"))) {
    for (auto it = browsers_.begin(); it != browsers_.end(); ++it)
      for (auto resource = it->keys.begin(); resource != it->keys.end(); ++resource)
        if (resource.value() == result.key) it.key()->updateImageResource(resource.key(),result.handle);
    const auto browserList = browsers_.keys();
    for (auto* browser : browserList) refreshBrowserResources(browser);
  }
  else emit petImageReady(result.visualKey, resourceUrl(result.key));
}
void PetImageCache::refreshBrowserResources(PetImageBrowser* browser) {
  const auto found = browsers_.constFind(browser);
  if (found == browsers_.cend() || !browser->isVisible()) return;
  for (auto it = found->requests.begin(); it != found->requests.end(); ++it) {
    const QString key = ImageService::requestKey(it.value(),version_);
    if (const auto image = images_.value(key).lock()) browser->updateImageResource(it.key(),image);
    else enqueue(it.value());
  }
}
void PetImageCache::setDocumentImage(QTextBrowser* browser, const QString& html, const QString& url) {
  if (auto* imageBrowser = dynamic_cast<PetImageBrowser*>(browser)) {
    const QString prefix = QStringLiteral("kqimage://cache/");
    const QString key = url.startsWith(prefix) ? QUrl::fromPercentEncoding(url.mid(prefix.size()).toLatin1()) : QString();
    if (!browsers_.contains(imageBrowser)) {
      connect(imageBrowser,&QObject::destroyed,this,[this,imageBrowser] { browsers_.remove(imageBrowser); });
      connect(imageBrowser,&PetImageBrowser::imageResourcesNeeded,this,[this,imageBrowser] { refreshBrowserResources(imageBrowser); });
    }
    BrowserResources bindings;
    QHash<QString,ImageHandle> images;
    if (!url.isEmpty()) {
      images.insert(url,images_.value(key).lock()); bindings.keys.insert(url,key);
      if (petRequests_.contains(key)) {
        ImageRequest request = petRequests_.value(key); request.retry = false;
        bindings.requests.insert(url,request);
      }
      imageBrowser->setToolTip(imageStatuses_.value(key));
    }
    static const QRegularExpression icons(QStringLiteral("<img\\b[^>]*\\bsrc\\s*=\\s*['\"](kqstargod://icon/([1-9][0-9]{0,5}))['\"]"),QRegularExpression::CaseInsensitiveOption);
    auto match = icons.globalMatch(html);
    while (match.hasNext() && bindings.requests.size() < 96) {
      const auto icon = match.next();
      ImageRequest request;
      request.stargodId = icon.captured(2).toInt();
      if (request.stargodId > 100000) continue;
      request.visualKey = QStringLiteral("stargod-%1").arg(request.stargodId);
      request.outputLogicalSize = {48,48}; request.devicePixelRatio = devicePixelRatio_;
      const QString imageKey = ImageService::requestKey(request,version_);
      bindings.requests.insert(icon.captured(1),request);
      bindings.keys.insert(icon.captured(1),imageKey);
      images.insert(icon.captured(1),images_.value(imageKey).lock());
    }
    static const QRegularExpression rings(QStringLiteral("src=['\"](kqstargodring://ring/[A-Za-z0-9_-]+)['\"]"));
    auto ringMatch = rings.globalMatch(html);
    while (ringMatch.hasNext()) {
      for (const auto& cell : stargodRingSlots(QUrl(ringMatch.next().captured(1)))) {
        if (cell.empty || cell.imageId <= 0 || bindings.requests.size() >= 96) continue;
        ImageRequest request; request.stargodId = cell.imageId;
        request.visualKey = QStringLiteral("stargod-%1").arg(cell.imageId);
        request.outputLogicalSize = {96,96}; request.devicePixelRatio = devicePixelRatio_;
        const QString url = QStringLiteral("kqstargod://icon/%1").arg(cell.imageId);
        const QString imageKey = ImageService::requestKey(request,version_);
        bindings.requests.insert(url,request); bindings.keys.insert(url,imageKey);
        images.insert(url,images_.value(imageKey).lock());
      }
    }
    browsers_.insert(imageBrowser,std::move(bindings));
    imageBrowser->setImagesHtml(html,images);
    refreshBrowserResources(imageBrowser);
  } else if (browser) browser->setHtml(html);
}
void PetImageCache::updateDocumentImage(QTextBrowser* browser, const QString& url) {
  auto* target = dynamic_cast<PetImageBrowser*>(browser);
  const QString prefix = QStringLiteral("kqimage://cache/");
  if (!target || !url.startsWith(prefix)) return;
  const auto key = QUrl::fromPercentEncoding(url.mid(prefix.size()).toLatin1());
  target->updateImageResource(url, images_.value(key).lock());
}
