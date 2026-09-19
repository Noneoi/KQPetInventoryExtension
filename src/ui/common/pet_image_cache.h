#pragma once

#include "application/images/image_service.h"

#include <QHash>
#include <QIcon>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTextBrowser>
class StargodRingObject;

// Documents never follow file/network URLs. Visible resources own explicit leases.
class PetImageBrowser final : public QTextBrowser {
  Q_OBJECT
public:
  explicit PetImageBrowser(QWidget* parent = nullptr);
  ~PetImageBrowser() override;
  void setHtml(const QString& html);
  void setImageHtml(const QString& html, const QString& resourceUrl, const ImageHandle& image);
  void setImagesHtml(const QString& html, const QHash<QString,ImageHandle>& images);
  void updateImageResource(const QString& resourceUrl, const ImageHandle& image);
signals:
  void imageRetryRequested();
  void imageResourcesNeeded();
protected:
  QVariant loadResource(int type, const QUrl& name) override;
  void hideEvent(QHideEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
private:
  void releaseImage();
  void acquireImage();
  struct Resource {
    std::weak_ptr<const ImagePayload> available;
    ImageHandle displayed;
    std::shared_ptr<void> lease;
  };
  QHash<QString,Resource> resources_;
  StargodRingObject* ring_ = nullptr;
};

// Shared GUI proxy: no disk/network/decode work. Unconnected previews show placeholders.
class PetImageCache final : public QObject {
  Q_OBJECT
public:
  explicit PetImageCache(const QString& dataRoot, QObject* parent = nullptr);
  void setService(ImageService* service, const QString& resourceVersion = QStringLiteral("1"));
  void setDevicePixelRatio(qreal ratio);
  QIcon attributeIcon(const QString& sequence) const;
  QString cachedPetImage(const QString& visualKey) const;
  QString ensurePetImage(const QJsonObject& pet, const QStringList& candidateNames,
                         qreal devicePixelRatio = 1, bool retry = false);
  void setDocumentImage(QTextBrowser* browser, const QString& html, const QString& resourceUrl);
  void updateDocumentImage(QTextBrowser* browser, const QString& resourceUrl);

signals:
  void petImageReady(const QString& visualKey, const QString& resourceUrl);
  void petImageStatusChanged(const QString& visualKey, const QString& message);
  void attributeIconsReady();

private:
  struct IconEntry { QIcon icon; ImageHandle image; std::shared_ptr<void> lease; };
  void enqueue(const ImageRequest& request) const;
  void accept(const ImageResult& result);
  void refreshBrowserResources(PetImageBrowser* browser);
  QString resourceUrl(const QString& key) const;
  QPointer<ImageService> service_;
  QString version_ = QStringLiteral("1");
  mutable QHash<QString, std::weak_ptr<const ImagePayload>> images_;
  mutable QHash<QString, QString> petKeys_;
  QHash<QString, ImageRequest> petRequests_;
  QHash<QString, QString> imageStatuses_;
  mutable QHash<QString, IconEntry> icons_;
  mutable QSet<QString> pending_;
  mutable QHash<QString, ImageRequest> deferred_;
  struct BrowserResources {
    QHash<QString,QString> keys;
    QHash<QString,ImageRequest> requests;
  };
  QHash<PetImageBrowser*,BrowserResources> browsers_;
  quint64 bindingGeneration_ = 0;
  qreal devicePixelRatio_ = 1;
};
