#pragma once

#include "shop_exchange_catalog.h"

#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <functional>

class QTimer;
class PetRepository;

class ShopExchangeController final : public QObject {
  Q_OBJECT

public:
  using Sender = std::function<bool(const QString&, const QString&, const QString&)>;

  explicit ShopExchangeController(PetRepository* repository, QObject* parent = nullptr);

  void setSender(Sender sender);
  bool requestInfo();
  bool updateCatalog();
  bool isRunning() const { return running_; }
  QJsonObject packet() const { return packet_; }
  bool hasPacket() const { return hasPacket_; }
  QHash<QString, qint64> materialCounts() const { return materialCounts_; }
  bool hasMaterialCounts() const { return hasMaterialCounts_; }

public slots:
  void handlePacket(const QString& method, const QString& payload);

signals:
  void statusChanged(const QString& status);
  void runningChanged(bool running);
  void infoUpdated();
  void catalogUpdated();

private:
  void finish(bool ok, const QString& status);
  void changeSession(const QString& account, quint64 generation);
  void loadCache();
  void saveCache() const;
  QHash<QString, qint64> parseRequiredMaterialCounts(const QJsonObject& packet) const;

  PetRepository* repository_ = nullptr;
  Sender sender_;
  QTimer* timeout_ = nullptr;
  QJsonObject packet_;
  QHash<QString, qint64> materialCounts_;
  bool running_ = false;
  bool hasPacket_ = false;
  bool hasMaterialCounts_ = false;
  bool shopResponseReceived_ = false;
  bool materialResponseReceived_ = false;
  QString account_;
  QString requestAccount_;
  quint64 sessionGeneration_ = 0;
  quint64 requestSessionGeneration_ = 0;
};
