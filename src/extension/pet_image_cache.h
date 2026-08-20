#pragma once

#include <QIcon>
#include <QJsonObject>
#include <QObject>
#include <QPixmap>
#include <QSet>
#include <QStringList>

class QNetworkAccessManager;

class PetImageCache final : public QObject {
  Q_OBJECT

public:
  explicit PetImageCache(const QString& dataRoot, QObject* parent = nullptr);

  QIcon attributeIcon(const QString& sequence) const;
  QString cachedPetImage(const QString& visualKey) const;
  QString ensurePetImage(const QJsonObject& pet, const QStringList& candidateNames);

signals:
  void petImageReady(const QString& visualKey, const QString& localPath);

private:
  QString imageUrlFor(const QStringList& candidateNames) const;
  void ensureAttributeSprite();

  QString dataRoot_;
  QString attributeSpritePath_;
  QString petImageDirectory_;
  QJsonObject imageUrls_;
  mutable QPixmap attributeSprite_;
  QNetworkAccessManager* network_ = nullptr;
  QSet<QString> pendingVisualKeys_;
};
